// RT-C: real MainWindow RPC observer and History notifications, using the
// rpc-hold child transport from the Undo keys test. No engine or GPU.

#include "layoutpreset.hpp"
#include "toollayoutpreset.hpp"
#include "../src/devstudio/model/gizmomodel.hpp"
#include "../src/devstudio/model/inspectormodel.hpp"
#include "../src/devstudio/model/project.hpp"
#include "../src/devstudio/model/renderedithistory.hpp"
#include "../src/devstudio/model/selection.hpp"
#include "../src/devstudio/viewport/engineattach.hpp"
#include "../src/devstudio/viewport/enginefailuremodel.hpp"
#include "../src/devstudio/viewport/enginelogbuffer.hpp"
#include "../src/devstudio/viewport/engineprocess.hpp"
#include "../src/devstudio/viewport/nativewindowhost.hpp"
#include "../src/devstudio/viewport/viewportgeometry.hpp"
#include "outlinertree.hpp"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QGraphicsView>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QStatusBar>
#include <QTabWidget>
#include <QTest>
#include <QTreeWidgetItemIterator>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#define private public
#include "../src/devstudio/viewport/embeddedviewport.hpp"
#include "mainwindow.hpp"
#undef private

#ifndef PELICAN_PROCESS_FIXTURE_PATH
#error "PELICAN_PROCESS_FIXTURE_PATH must name the process fixture child"
#endif


namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

QApplication &application() {
    static int count = 1;
    static char name[] = "pelican_render_post_commit_test";
    static char *arguments[] = {name};
    static QApplication app{count, arguments};
    return app;
}

const std::string Preset = "project://passes/local-pipeline.json";
const std::string Before(64, '1');
const std::string After(64, '2');
const QString ErrorLine = QStringLiteral(
    "Saved. Applying it in the running engine failed after the commit: history reset failed");

Json committed(const std::string &ticket, const std::string &digest, bool error) {
    Json answer{{"ticket", ticket}, {"status", "committed"}, {"committed", true},
                {"published_generation", 7u},
                {"source_digest", {{"algorithm", "sha256"}, {"hex", std::string(64, 'a')}}},
                {"documents", {{Preset, digest}}}};
    if (error) answer["post_commit_error"] = "history reset failed";
    return answer;
}

struct Sandbox {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pelican_render_post_commit_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    Sandbox() { std::filesystem::create_directories(root); }
    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

struct Rig {
    QApplication &app = application();
    Sandbox sandbox;
    MainWindow window{QString::fromStdWString(sandbox.root.wstring())};
    EmbeddedViewport &viewport = *window.viewport_;
    QString buffer;
    std::deque<Json> pending;
    std::string digest = Before;
    std::string restore_content;
    int next_ticket = 1;
    bool with_error = false;
    Json last_commit;

    Rig() {
        QObject::connect(&viewport, &EmbeddedViewport::engineStandardErrorReceived, &window,
                         [this](const QString &text) {
            buffer += text;
            for (auto end = buffer.indexOf(QLatin1Char('\n')); end >= 0;
                 end = buffer.indexOf(QLatin1Char('\n'))) {
                const auto line = buffer.left(end).trimmed();
                buffer.remove(0, end + 1);
                const QString prefix = QStringLiteral("rpc-hold-request:");
                if (line.startsWith(prefix))
                    pending.push_back(Json::parse(line.mid(prefix.size()).toStdString()));
            }
        });
        QString error;
        REQUIRE(viewport.process_.start(
            {QString::fromUtf8(PELICAN_PROCESS_FIXTURE_PATH), {QStringLiteral("rpc-hold")}, {}}, &error));
        REQUIRE(viewport.process_.waitForStarted(5000));
        viewport.rpc_ready_ = true;
    }

    void reply(qint64 id, const Json &answer) {
        emit viewport.inspectorRpcSucceeded(id, QByteArray::fromStdString(answer.dump()));
    }

    void pump() {
        QApplication::processEvents();
        while (!pending.empty()) {
            const auto request = std::move(pending.front());
            pending.pop_front();
            const auto id = request.at("id").get<qint64>();
            const auto method = request.at("method").get<std::string>();
            const auto params = request.value("params", Json::object());
            if (method == "get_render_documents") {
                reply(id, Json{{"documents", Json::array({{{"reference", Preset}, {"kind", "preset"},
                        {"digest", digest}, {"content", "preset " + digest.substr(0, 1)}}})}});
            } else if (method == "edit_preset_pass" || method == "restore_render_documents") {
                if (method == "restore_render_documents")
                    restore_content = params.at("documents").at(0).at("content").get<std::string>();
                reply(id, Json{{"status", "accepted"}, {"ticket", method + ":" + std::to_string(next_ticket++)}});
            } else if (method == "get_edit_result") {
                const auto ticket = params.at("ticket").get<std::string>();
                digest = ticket.starts_with("edit_preset_pass") || restore_content == "preset 2" ? After : Before;
                last_commit = committed(ticket, digest, with_error);
                reply(id, last_commit);
            } else {
                emit viewport.inspectorRpcFailed(id, QStringLiteral("not in this fake engine"));
            }
        }
    }

    bool until(const std::function<bool()> &done) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 8000) {
            pump();
            if (done()) return true;
            QTest::qWait(5);
        }
        return done();
    }

    void edit() {
        const auto size = window.render_history_->undoStack().size();
        QString error;
        REQUIRE(viewport.requestRpc(QStringLiteral("edit_preset_pass"),
            QJsonObject{{QStringLiteral("graph"), QStringLiteral("main_render")},
                        {QStringLiteral("pass_name"), QStringLiteral("present")}}, &error) != 0);
        REQUIRE(until([&] { return window.render_history_->undoStack().size() == size + 1; }));
    }
};

TEST_CASE("RT-C MainWindow keeps post-commit errors after History and forgets them on disconnect",
          "[devstudio][rendereditcompletion][rtc][negative-contrast]") {
    int error_notices = 0;
    Rig rig;
    auto &history = *rig.window.render_history_;
    auto &status = *rig.window.statusBar();

    // Missing field: a real committed response, with history still recorded.
    rig.edit();
    REQUIRE(history.undoLabel() == "Edit pass present");
    CHECK_FALSE(status.currentMessage().contains(QStringLiteral("failed after the commit")));
    CHECK(history.redoStack().empty());

    // The next edit has an error. Observe the actual status, not the helper.
    rig.with_error = true;
    rig.edit();
    CHECK(status.currentMessage() == ErrorLine);
    // A second reader polling the same commit must not repeat its notice.
    QObject::connect(&status, &QStatusBar::messageChanged, &rig.window,
                     [&](const QString &line) { if (line == ErrorLine) ++error_notices; });
    status.showMessage(QStringLiteral("another notice"));
    rig.reply(90001, rig.last_commit);
    CHECK(status.currentMessage() == QStringLiteral("another notice"));
    CHECK(error_notices == 0);

    // Undo and Redo really finish through MainWindow's History handler.
    REQUIRE(history.undo());
    REQUIRE(rig.until([&] { return !history.busy() && history.redoStack().size() == 1; }));
    CHECK(history.line() == "Undid \"Edit pass present\".");
    CHECK(history.undoStack().size() == 1);
    CHECK(status.currentMessage() == ErrorLine);
    REQUIRE(history.redo());
    REQUIRE(rig.until([&] { return !history.busy() && history.redoStack().empty(); }));
    CHECK(history.line() == "Redid \"Edit pass present\".");
    CHECK(history.undoStack().size() == 2);
    CHECK(status.currentMessage() == ErrorLine);

    // Two distinct preset commits in different engine sessions can have an
    // identical ticket, generation and unchanged root digest. Only the
    // preset document (and thus its document digest) changes.
    const auto first = committed("render-config-edit-1", Before, true);
    const auto second = committed("render-config-edit-1", After, true);
    REQUIRE(first.at("ticket") == second.at("ticket"));
    REQUIRE(first.at("published_generation") == second.at("published_generation"));
    REQUIRE(first.at("source_digest") == second.at("source_digest"));
    REQUIRE(first.at("documents") != second.at("documents"));
    status.clearMessage();
    rig.reply(90002, first);
    CHECK(status.currentMessage() == ErrorLine);
    const auto notices_before_disconnect = error_notices;
    emit rig.viewport.engineRpcBecameUnavailable(QStringLiteral("engine restarted"));
    status.clearMessage();
    rig.reply(90003, second);
    CHECK(status.currentMessage() == ErrorLine);
    CHECK(error_notices == notices_before_disconnect + 1);
    status.showMessage(QStringLiteral("after restart"));
    rig.reply(90004, second);
    CHECK(status.currentMessage() == QStringLiteral("after restart"));
    CHECK(error_notices == notices_before_disconnect + 1);
    // The persisted history survives the cut of the RPC session.
    CHECK(history.undoStack().size() == 2);
}

} // namespace
} // namespace PelicanStudio
