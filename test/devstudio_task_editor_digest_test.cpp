// Q-15: the Task Editor (Render features' Compute and ray tracing tasks) on
// real Qt widgets, over the real RPC transport. A child process
// (process_fixture_child rpc-hold) takes every request the editor sends and
// writes it back on stderr; this test answers in the engine's place, through
// the viewport's own reply signal.
//
// What is judged: the digest the editor reads is told to the other panels,
// and another panel's commit (Q-13's shared digest) makes the editor read the
// document again at once, keeping the draft, so its next edit goes out
// against the revision the engine holds.

#include "renderconfigdigest.hpp"
#include "renderfeatureswidget.hpp"

// Everything embeddedviewport.hpp includes, first, so that only its own class
// is opened up. EmbeddedViewport has no test seam; as in the Inspector widget
// test, this translation unit reaches its process and readiness members so
// the editor runs over the viewport's real transport.
#include "engineattach.hpp"
#include "enginefailuremodel.hpp"
#include "engineprocess.hpp"
#include "nativewindowhost.hpp"
#include "viewportgeometry.hpp"

#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QTest>
#include <QWidget>

#define private public
#include "embeddedviewport.hpp"
#undef private

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <deque>
#include <memory>
#include <optional>
#include <string>

#ifndef PELICAN_PROCESS_FIXTURE_PATH
#error "PELICAN_PROCESS_FIXTURE_PATH must name the process fixture child"
#endif

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QApplication &application() {
    static int argument_count = 1;
    static char application_name[] = "pelican_task_editor_digest_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

const std::string RootA(64, 'a');
const std::string RootB(64, 'b');
const std::string RootC(64, 'c');
const std::string TaskDigest(64, 'd');

Json blurDeclaration(const std::string &shader) {
    return {{"name", "blur"},
            {"shader", shader},
            {"dispatch", {{"groups", Json::array({1, 1, 1})}}}};
}

// get_compute_task_authoring at `root`: one task a feature file declares.
Json taskContext(const std::string &root) {
    const auto declaration = blurDeclaration("project://shaders/blur");
    return {{"source_digest", {{"hex", root}}},
            {"tasks",
             Json::array({{{"name", "blur"},
                           {"kind", "compute"},
                           {"origin", "feature"},
                           {"editable", true},
                           {"source_reference", "project://features/blur.json"},
                           {"provider_feature", "blur"},
                           {"declaration", declaration},
                           {"declaration_json", declaration.dump(2)},
                           {"source_digest", {{"hex", TaskDigest}}}}})},
            {"resources", Json::array()}};
}

struct Rig {
    // First: every widget below needs the application.
    QApplication &app = application();
    EmbeddedViewport viewport;
    std::shared_ptr<RenderConfigDigest> digest =
        std::make_shared<RenderConfigDigest>();
    std::unique_ptr<RenderFeaturesWidget> features;
    std::deque<Json> pending;
    QString buffer;

    Rig() {
        QObject::connect(&viewport, &EmbeddedViewport::engineStandardErrorReceived,
                         [this](const QString &text) { receive(text); });
        QString error;
        REQUIRE(viewport.process_.start(
            {QString::fromUtf8(PELICAN_PROCESS_FIXTURE_PATH), {QStringLiteral("rpc-hold")}, {}},
            &error));
        REQUIRE(viewport.process_.waitForStarted(5000));
        viewport.rpc_ready_ = true;
        features = std::make_unique<RenderFeaturesWidget>(&viewport);
        features->setRenderConfigDigest(digest);
    }

    ~Rig() { features.reset(); }

    void receive(const QString &text) {
        buffer += text;
        for (qsizetype end = buffer.indexOf(QLatin1Char('\n')); end >= 0;
             end = buffer.indexOf(QLatin1Char('\n'))) {
            const QString line = buffer.left(end).trimmed();
            buffer.remove(0, end + 1);
            const QString prefix = QStringLiteral("rpc-hold-request:");
            if (line.startsWith(prefix))
                pending.push_back(Json::parse(line.mid(prefix.size()).toStdString()));
        }
    }

    // The next request of `method` the panels send within `milliseconds`;
    // requests of other methods (Render features' own reads) stay unanswered.
    std::optional<Json> next(const std::string &method, int milliseconds = 5000) {
        QElapsedTimer clock;
        clock.start();
        while (true) {
            QApplication::processEvents();
            while (!pending.empty()) {
                Json request = std::move(pending.front());
                pending.pop_front();
                if (request.value("method", std::string{}) == method) return request;
            }
            if (clock.elapsed() >= milliseconds) return std::nullopt;
            QTest::qWait(5);
        }
    }

    void answer(const Json &request, const Json &result) {
        emit viewport.inspectorRpcSucceeded(request.at("id").get<qint64>(),
                                            QByteArray::fromStdString(result.dump()));
        QApplication::processEvents();
    }

    QDialog *editor() const {
        return features->findChild<QDialog *>(QStringLiteral("pelican.computeTasks"));
    }
};

template <typename Widget> Widget &required(QWidget &parent, const char *name) {
    auto *found = parent.findChild<Widget *>(QString::fromLatin1(name));
    REQUIRE(found != nullptr);
    return *found;
}

} // namespace

TEST_CASE("Q-15 the Task Editor reads the document again when another panel "
          "commits, keeping its draft",
          "[devstudio][task-editor][q15][q13][widget][negative-contrast]") {
    Rig rig;
    rig.features->editComputeTask(QStringLiteral("blur"));
    const auto first = rig.next("get_compute_task_authoring");
    REQUIRE(first);
    rig.answer(*first, taskContext(RootA));
    QDialog *dialog = rig.editor();
    REQUIRE(dialog != nullptr);
    // What the editor read, the other panels are told.
    CHECK(rig.digest->current() == RootA);

    auto &editor = required<QPlainTextEdit>(*dialog, "pelican.computeTasks.declaration");
    auto &apply = required<QPushButton>(*dialog, "pelican.computeTasks.apply");
    REQUIRE(editor.toPlainText().contains(QStringLiteral("project://shaders/blur")));
    const auto draft =
        QString::fromStdString(blurDeclaration("project://shaders/blur_wide").dump(2));
    editor.setPlainText(draft);

    // Control: nothing moved, nothing is read.
    CHECK_FALSE(rig.next("get_compute_task_authoring", 200));

    // Another panel's commit: read again at once, the draft kept.
    rig.digest->publish(RootB);
    const auto again = rig.next("get_compute_task_authoring");
    REQUIRE(again);
    rig.answer(*again, taskContext(RootB));
    CHECK(editor.toPlainText() == draft);

    // The edit goes out against the revision read, the task still guarded by
    // its own digest.
    REQUIRE(apply.isEnabled());
    apply.click();
    const auto edit = rig.next("edit_compute_task");
    REQUIRE(edit);
    const auto &params = edit->at("params");
    CHECK(params.at("base_source_digest") == RootB);
    CHECK(params.at("base_task_source_digest") == TaskDigest);
    CHECK(params.at("task").at("shader") == "project://shaders/blur_wide");

    // A move told while the edit is out is followed once it comes back.
    rig.digest->publish(RootC);
    CHECK_FALSE(rig.next("get_compute_task_authoring", 200));
    rig.answer(*edit, {{"status", "rejected"},
                       {"error",
                        {{"code", "gate_closed"},
                         {"message", "The editor gate is closed"}}}});
    const auto followed = rig.next("get_compute_task_authoring");
    REQUIRE(followed);
    rig.answer(*followed, taskContext(RootC));
    CHECK(editor.toPlainText() == draft);
}

} // namespace PelicanStudio
