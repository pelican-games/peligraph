// RT-D: the real MainWindow injection and Graph capability wrappers, over
// the viewport's RPC transport. History's first poll is held while the
// controller reads its commit, submits Verify, and History observes that
// second commit first. Undo is judged by the actual restore's bytes and CAS.
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
#include "graphcanvaswidget.hpp"
#include "renderconfigdocument.hpp"
#include <QCheckBox>
#include <QGraphicsItem>
#include <QJsonDocument>
#include <map>

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
#include <QTabWidget>
#include <QTest>
#include <QTreeWidgetItemIterator>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>

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
const std::string Root = "project://passes/main.json";

QApplication &application() {
    static int argc = 1;
    static char name[] = "pelican_tab_render_history_test";
    static char *argv[] = {name, nullptr};
    static QApplication app{argc, argv};
    return app;
}

struct Sandbox {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pelican-tab-render-history-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    Sandbox() { std::filesystem::create_directories(root); }
    ~Sandbox() { std::error_code error; std::filesystem::remove_all(root, error); }
};

class FakeEngine {
  public:
    EmbeddedViewport &viewport;
    RenderEditHistory &history;
    Json document{{"schema", "pelican.graph"}, {"version", 1},
                  {"nodes", Json::array({{{"id", "a"}, {"op", "value.constant"},
                                         {"parameters", {{"value", 1}}}, {"outputs", {"value"}}},
                                        {{"id", "b"}, {"op", "value.constant"},
                                         {"parameters", {{"value", 2}}}, {"outputs", {"value"}}}})},
                  {"definitions", Json::object()}, {"outputs", Json::object()}};
    std::vector<std::pair<std::string, Json>> requests;
    std::vector<Json> restores;
    std::vector<qint64> held;
    std::map<std::string, Json> commits;
    bool hold_fold = true;
    bool fold_seen_by_controller = false;
    std::string group;

    FakeEngine(EmbeddedViewport &v, RenderEditHistory &h) : viewport{v}, history{h} {
        QObject::connect(&viewport, &EmbeddedViewport::engineStandardErrorReceived, &viewport,
                         [this](const QString &text) { receive(text); });
    }
    std::string digest() const { return Pelican::renderConfigSourceDigest(document.dump()); }
    std::size_t count(const std::string &method) const {
        return std::ranges::count(requests, method, &decltype(requests)::value_type::first);
    }
    Json snapshot() const {
        return {{"documents", Json::array({{{"reference", Root}, {"kind", "root"},
                                            {"digest", digest()}, {"content", document.dump()}}})}};
    }
    Json commit() const {
        return {{"status", "committed"}, {"committed", true},
                {"source_digest", {{"hex", digest()}}}, {"documents", {{Root, digest()}}}};
    }
    void reply(qint64 id, const Json &result) {
        emit viewport.inspectorRpcSucceeded(id, QByteArray::fromStdString(result.dump()));
    }
    void releaseFold() {
        REQUIRE(held.size() == 1);
        hold_fold = false;
        const auto id = held.front();
        held.clear();
        REQUIRE(history.waitingFor(id));
        reply(id, commits.at("fold_nodes"));
    }
    void pump() {
        QApplication::processEvents();
        while (!pending_.empty()) {
            auto request = std::move(pending_.front());
            pending_.pop_front();
            const auto id = request.at("id").get<qint64>();
            const auto method = request.at("method").get<std::string>();
            const auto params = request.value("params", Json::object());
            requests.emplace_back(method, params);
            if (method == "get_edit_result" && params.at("ticket") == "fold_nodes" &&
                hold_fold && history.waitingFor(id)) {
                held.push_back(id);
                continue;
            }
            if (const auto result = answer(method, params)) reply(id, *result);
            else emit viewport.inspectorRpcFailed(id, QStringLiteral("not in this fake engine"));
            QApplication::processEvents();
        }
    }

  private:
    QString buffer_;
    std::deque<Json> pending_;
    unsigned restore_ticket_ = 0;
    void receive(const QString &text) {
        buffer_ += text;
        for (auto end = buffer_.indexOf(QLatin1Char('\n')); end >= 0;
             end = buffer_.indexOf(QLatin1Char('\n'))) {
            const auto line = buffer_.left(end).trimmed();
            buffer_.remove(0, end + 1);
            const QString prefix = QStringLiteral("rpc-hold-request:");
            if (line.startsWith(prefix))
                pending_.push_back(Json::parse(line.mid(prefix.size()).toStdString()));
        }
    }
    std::optional<Json> answer(const std::string &method, const Json &params) {
        if (method == "get_render_documents") return snapshot();
        if (method == "get_graph_instances") {
            auto context = document;
            context["root_schema"] = "pelican.graph";
            context["source_reference"] = Root;
            context["source_digest"] = {{"hex", digest()}};
            context["instances"] = Json::array();
            return context;
        }
        if (method == "get_fold_groups") {
            auto groups = Json::array();
            if (document.contains("fold_groups"))
                for (const auto &[name, value] : document.at("fold_groups").items())
                    groups.push_back({{"group", name}, {"nodes", value.at("nodes")},
                                      {"kind", "decisions"}, {"digest", digest()},
                                      {"state", value.at("folded").contains("verified")
                                                    ? "verified" : "unverified"}});
            return Json{{"source_digest", {{"hex", digest()}}}, {"fold_groups", groups}};
        }
        if (method == "fold_nodes" || method == "verify_fold") {
            REQUIRE(params.at("base_source_digest") == digest());
            group = params.at("group").get<std::string>();
            if (method == "fold_nodes")
                document["fold_groups"][group] = {{"nodes", params.at("nodes")},
                                                   {"folded", params.at("folded")}};
            else
                document["fold_groups"][group]["folded"]["verified"] = {{"digest", digest()}};
            commits[method] = commit();
            return Json{{"status", "accepted"}, {"ticket", method}};
        }
        if (method == "restore_render_documents") {
            const auto &documents = params.at("documents");
            REQUIRE(documents.size() == 1);
            CHECK(documents[0].at("reference") == Root);
            REQUIRE(documents[0].at("base_digest") == digest());
            restores.push_back(documents);
            document = Json::parse(documents[0].at("content").get<std::string>());
            const auto ticket = "restore:" + std::to_string(++restore_ticket_);
            commits[ticket] = commit();
            return Json{{"status", "accepted"}, {"ticket", ticket}};
        }
        if (method == "get_edit_result") {
            const auto ticket = params.at("ticket").get<std::string>();
            if (ticket == "fold_nodes") fold_seen_by_controller = true;
            if (const auto found = commits.find(ticket); found != commits.end()) return found->second;
        }
        return std::nullopt;
    }
};

struct Rig {
    QApplication &app = application();
    Sandbox sandbox;
    MainWindow window{QString::fromStdWString(sandbox.root.wstring())};
    EmbeddedViewport &viewport = *window.viewport_;
    FakeEngine engine{viewport, *window.render_history_};
    Rig() {
        QString error;
        REQUIRE(viewport.process_.start(
            {QString::fromUtf8(PELICAN_PROCESS_FIXTURE_PATH), {QStringLiteral("rpc-hold")}, {}}, &error));
        REQUIRE(viewport.process_.waitForStarted(5000));
        window.resize(1400, 900);
        window.show();
        REQUIRE(window.showRenderToolsTab(QStringLiteral("graph")));
        viewport.rpc_ready_ = true;
        emit viewport.engineRpcBecameAvailable();
        named<QCheckBox>("pelican.graphCanvasShow.value").setChecked(true);
        REQUIRE(pumpUntil([&] { return window.graph_canvas_->model().contextReceived(); }));
    }
    ~Rig() {
        // rpc-hold exits on EOF; do not terminate or kill a process.
        viewport.process_.closeInput();
        viewport.process_.waitForFinished(5000);
    }
    bool pumpUntil(const std::function<bool()> &done, int milliseconds = 5000) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            engine.pump();
            if (done()) return true;
            QTest::qWait(5);
        }
        return done();
    }
    template<typename T> T &named(const char *name) {
        auto *found = window.findChild<T *>(QString::fromLatin1(name));
        REQUIRE(found != nullptr);
        return *found;
    }
    void undoTo(const Json &expected, std::size_t restores) {
        named<QAction>("pelican.graphCanvas.undo").trigger();
        REQUIRE(pumpUntil([&] { return engine.restores.size() == restores && !window.render_history_->busy(); }));
        CHECK(engine.restores.back()[0].at("content") == expected.dump());
        CHECK(engine.document == expected);
    }
};
} // namespace

TEST_CASE("RT-D MainWindow Graph fold and verify Undo restore documents after reversed notifications",
          "[devstudio][tab-render-history][rtd][negative-contrast]") {
    const auto connected = GENERATE(true, false);
    INFO((connected ? "production injection and capability wrappers"
                   : "negative control disconnects shared history but keeps enabled actions"));
    Rig rig;
    if (!connected) rig.window.graph_canvas_->setRenderHistory(nullptr);
    const auto original = rig.engine.document;
    auto &scene = rig.window.graph_canvas_->scene();
    scene.clearSelection();
    unsigned picked = 0;
    for (auto *item : scene.items())
        if (item->data(NodeGraphItemKindRole).toString() == QLatin1String{GraphCanvasNodeItem}) {
            const auto name = item->data(NodeGraphNameRole).toString();
            if (name == QStringLiteral("a") || name == QStringLiteral("b")) {
                item->setSelected(true);
                ++picked;
            }
        }
    REQUIRE(picked == 2);
    rig.named<QAction>("pelican.graphCanvas.foldIntoGroup").trigger();
    REQUIRE(rig.pumpUntil([&] {
        const auto *verify = rig.window.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasFold.verify"));
        return rig.engine.fold_seen_by_controller && !rig.engine.held.empty() && verify && verify->isEnabled() &&
               rig.window.graph_canvas_->model().digest() == rig.engine.digest();
    }));
    const auto folded = rig.engine.document;
    REQUIRE(folded != original);
    REQUIRE(rig.window.render_history_->undoStack().empty()); // R1 is still pending in History.
    rig.named<QPushButton>("pelican.graphCanvasFold.verify").click();
    REQUIRE(rig.pumpUntil([&] { return rig.window.render_history_->undoStack().size() == 1 &&
        rig.window.graph_canvas_->model().digest() == rig.engine.digest(); }));
    REQUIRE(rig.engine.count("fold_nodes") == 1);
    REQUIRE(rig.engine.count("verify_fold") == 1);
    REQUIRE(rig.engine.document != folded);
    const auto second = rig.window.render_history_->undoStack().back().serial;
    rig.engine.releaseFold();
    REQUIRE(rig.pumpUntil([&] { return rig.window.render_history_->undoStack().size() == 2; }));
    REQUIRE(rig.window.render_history_->undoStack().back().serial == second);
    REQUIRE(rig.window.render_undo_action_->isEnabled());
    if (!connected) {
        CHECK_FALSE(scene.canUndoAny());
        scene.undoEdit();
        // Also exercise the QAction; enabled shared actions cannot replace injection.
        rig.named<QAction>("pelican.graphCanvas.undo").trigger();
        rig.pumpUntil([] { return false; }, 100);
        CHECK(rig.engine.restores.empty());
        CHECK(rig.engine.count("restore_render_documents") == 0);
        return;
    }
    REQUIRE(scene.canUndoAny());
    rig.undoTo(folded, 1);   // Verify's mark is removed; the fold remains.
    rig.undoTo(original, 2); // Fold itself is removed.
    CHECK(rig.window.render_history_->undoStack().empty());
    REQUIRE(rig.window.render_history_->redoStack().size() == 2);
    CHECK(rig.engine.count("edit_graph_document") == 0);
}
} // namespace PelicanStudio
