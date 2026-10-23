#include "renderdocumentwatcher.hpp"
#include "renderdocumentreload.hpp"
#include "renderingsettingswidget.hpp"
#include "raylayoutwidget.hpp"
#include "embeddedviewport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

void application() {
    static int argc = 1;
    static char name[] = "pelican_render_document_watcher_test";
    static char *argv[] = {name};
    static QApplication instance{argc, argv};
}

std::optional<std::string> digest(const std::filesystem::path &path) {
    QFile file{QString::fromStdWString(path.wstring())};
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
        .toHex().toStdString();
}

void write(const QString &path, const QByteArray &bytes) {
    QFile file{path};
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    REQUIRE(file.write(bytes) == bytes.size());
}

void replace(const QString &path, const QByteArray &bytes) {
    QSaveFile file{path};
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes) == bytes.size());
    REQUIRE(file.commit());
}

struct Rig {
    QTemporaryDir directory;
    std::vector<std::pair<int, std::function<void()>>> scheduled;
    std::vector<std::string> requests;
    std::int64_t next_id = 1;
    RenderDocumentReload reload{RenderDocumentReload::Transport{
        .request = [this](const std::string &method, const Json &, std::string *) {
            requests.push_back(method);
            return next_id++;
        },
        .schedule = [this](int milliseconds, std::function<void()> task) {
            scheduled.emplace_back(milliseconds, std::move(task));
        },
        .file_digest = digest,
    }};
    // The same model and watcher wiring the window uses. No FramePlanWidget.
    RenderDocumentWatcher watcher{reload};

    QString path(const char *relative) const {
        return directory.filePath(QString::fromLatin1(relative));
    }

    Rig() {
        REQUIRE(directory.isValid());
        REQUIRE(QDir{directory.path()}.mkpath(QStringLiteral("fragments")));
        write(path("root.json"), "{}");
        write(path("fragments/pass.json"), "{}");
        write(path("initial.bin"), "initial");
        reload.watchChanged = [this] { watcher.setFiles(reload.watchedFiles()); };
        reload.open(std::filesystem::path{directory.path().toStdWString()});
        reload.engineReady();
        REQUIRE(requests == std::vector<std::string>{"get_render_documents"});
        REQUIRE(reload.receiveResult(next_id - 1,
            Json{{"documents", Json::array({
                {{"reference", "root.json"}, {"kind", "root"},
                 {"digest", *digest(std::filesystem::path{path("root.json").toStdWString()})},
                 {"content", "{}"}},
                {{"reference", "project://fragments/pass.json"}, {"kind", "feature"},
                 {"digest", *digest(std::filesystem::path{path("fragments/pass.json").toStdWString()})},
                 {"content", "{}"}},
            })}}));
        reload.setInitialAssets({"initial.bin"});
        requests.clear();
        QTest::qWait(100);
        scheduled.clear();
    }

    void notification() {
        QElapsedTimer elapsed;
        elapsed.start();
        while (scheduled.empty() && elapsed.elapsed() < 3000) QTest::qWait(20);
        REQUIRE_FALSE(scheduled.empty());
        // Deliver all signals from this save before testing the next save.
        QTest::qWait(100);
        for (const auto &[milliseconds, task] : scheduled) {
            (void)task;
            CHECK(milliseconds == RenderDocumentReload::ReloadDebounceMs);
        }
    }

    void flush() {
        auto tasks = std::move(scheduled);
        scheduled.clear();
        for (auto &[milliseconds, task] : tasks) {
            (void)milliseconds;
            task();
        }
    }
};

} // namespace

TEST_CASE("RT-B the window's watcher delivers real file changes without a Frame Plan",
          "[devstudio][render-document-watcher]") {
    application();
    Rig rig;

    SECTION("the root, fragments and initial assets reach fileChanged") {
        const auto file = GENERATE("root.json", "fragments/pass.json", "initial.bin");
        write(rig.path(file), "changed externally");
        rig.notification();
        rig.flush();
        REQUIRE(rig.requests == std::vector<std::string>{"reload_render_documents"});
    }

    SECTION("a write matching a commit is ignored by the digest policy") {
        write(rig.path("root.json"), "Studio's save");
        const auto saved = *digest(std::filesystem::path{rig.path("root.json").toStdWString()});
        rig.reload.observeReply(Json{{"status", "committed"},
                                     {"documents", {{"root.json", saved}}}});
        rig.notification();
        rig.flush();
        CHECK(rig.requests.empty());
    }

    SECTION("replacement saves restore the watch for the next save") {
        replace(rig.path("root.json"), "replacement one");
        rig.notification();
        rig.scheduled.clear();
        replace(rig.path("root.json"), "replacement two");
        rig.notification();
        rig.scheduled.clear();
        write(rig.path("root.json"), "in-place after replacements");
        rig.notification();
    }

    SECTION("the parent watch recovers a removed and recreated fragment") {
        REQUIRE(QFile::remove(rig.path("fragments/pass.json")));
        rig.notification();
        rig.scheduled.clear();
        write(rig.path("fragments/pass.json"), "recreated");
        rig.notification();
        rig.scheduled.clear();
        write(rig.path("fragments/pass.json"), "saved again");
        rig.notification();
    }

    SECTION("a new watch set retires both old files and their directories") {
        REQUIRE(QDir{rig.directory.path()}.mkpath(QStringLiteral("other")));
        write(rig.path("other/root.json"), "{}");
        rig.watcher.setFiles({std::filesystem::path{rig.path("other/root.json").toStdWString()}});
        QTest::qWait(100);
        rig.scheduled.clear();
        write(rig.path("root.json"), "no longer watched");
        write(rig.path("fragments/new.json"), "old directory");
        QTest::qWait(400);
        CHECK(rig.scheduled.empty());
        write(rig.path("other/root.json"), "new watch");
        rig.notification();
    }
}

TEST_CASE("RT-B Rendering settings and Ray layout help name their own manual sections",
          "[devstudio][panel-help]") {
    application();
    EmbeddedViewport viewport;
    RenderingSettingsWidget settings{&viewport};
    RayLayoutWidget ray{&viewport};
    const auto *settings_help = settings.findChild<QWidget *>(
        QStringLiteral("pelican.renderingSettings.help"));
    const auto *ray_help = ray.findChild<QWidget *>(QStringLiteral("pelican.rayLayout.help"));
    REQUIRE(settings_help != nullptr);
    REQUIRE(ray_help != nullptr);
    CHECK(settings_help->property("pelicanManualAnchor").toString() ==
          QStringLiteral("15_studio.md#1511-rendering-settings"));
    CHECK(ray_help->property("pelicanManualAnchor").toString() ==
          QStringLiteral("15_studio.md#1512-ray-layout"));
}

} // namespace PelicanStudio
