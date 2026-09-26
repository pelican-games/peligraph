// RU-1: which Undo Ctrl+Z is, by where the keyboard is. The real window over
// the real RPC transport: a child process (process_fixture_child rpc-hold)
// takes every request and writes it back on stderr, and this test answers in
// the engine's place through the viewport's own reply signals.
//
// Judged: with the focus in the Frame Plan (or the Pass Editor) Ctrl+Z is the
// render edit history's Undo and sends its restore; in the Graph tab the key
// goes to the canvas (the graph's own history); in the Inspector it is the
// scene's Undo; in a text field it is the field's own.

// Everything the two headers below include, first, so that only their own
// classes are opened up (as in the Outliner edit test): the window runs over
// the viewport's real transport without starting an engine.
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
    static int argument_count = 1;
    static char application_name[] = "pelican_render_undo_keys_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

const std::string Preset = "project://passes/local-pipeline.json";
const std::string PresetBefore(64, '1');
const std::string PresetAfter(64, '2');

// The engine, as far as these keys can tell: one scene with one object for
// the Inspector, one preset for the render history. Edits commit on the first
// poll. Anything else asked of it is refused.
class FakeEngine {
  public:
    explicit FakeEngine(EmbeddedViewport &viewport) : viewport_{viewport} {
        QObject::connect(&viewport, &EmbeddedViewport::engineStandardErrorReceived,
                         [this](const QString &text) { receive(text); });
    }

    // Every request, by method, in order.
    std::vector<std::pair<std::string, Json>> requests;
    std::uint64_t revision = 1;

    std::size_t count(const std::string &method) const {
        std::size_t found = 0;
        for (const auto &[name, params] : requests) found += name == method;
        return found;
    }

    void pump() {
        QApplication::processEvents();
        while (!pending_.empty()) {
            const Json request = std::move(pending_.front());
            pending_.pop_front();
            const auto id = request.at("id").get<qint64>();
            const auto method = request.at("method").get<std::string>();
            const Json params = request.value("params", Json::object());
            requests.emplace_back(method, params);
            std::optional<Json> result = answer(method, params);
            if (result)
                emit viewport_.inspectorRpcSucceeded(id, QByteArray::fromStdString(result->dump()));
            else
                emit viewport_.inspectorRpcFailed(id, QStringLiteral("not in this fake engine"));
            QApplication::processEvents();
        }
    }

  private:
    EmbeddedViewport &viewport_;
    QString buffer_;
    std::deque<Json> pending_;
    int next_ticket_ = 1;
    std::string preset_digest_ = PresetBefore;

    void receive(const QString &text) {
        buffer_ += text;
        for (qsizetype end = buffer_.indexOf(QLatin1Char('\n')); end >= 0;
             end = buffer_.indexOf(QLatin1Char('\n'))) {
            const QString line = buffer_.left(end).trimmed();
            buffer_.remove(0, end + 1);
            const QString prefix = QStringLiteral("rpc-hold-request:");
            if (line.startsWith(prefix))
                pending_.push_back(Json::parse(line.mid(prefix.size()).toStdString()));
        }
    }

    std::optional<Json> answer(const std::string &method, const Json &params) {
        if (method == "open_editor_session")
            return Json{{"actor_id", 9}, {"display_name", "Pelican Studio Inspector"}};
        if (method == "scene_tree")
            return Json{{"scene_revision", revision},
                        {"scene_id", "main"},
                        {"objects", Json::array({{{"authoring_object_id", 2},
                                                  {"declaration_index", 0},
                                                  {"name", "Cube"},
                                                  {"components", Json::array()}}})}};
        if (method == "get_scene_revision")
            return Json{{"scene_revision", revision}, {"preview_epoch", 0}};
        if (method == "list_assets") return Json{{"assets", Json::array()}};
        if (method == "get_components") {
            const Json transform{{"pos", Json::array({0.0, 0.0, 0.0})},
                                 {"rotation", Json::array({0.0, 0.0, 0.0, 1.0})},
                                 {"scale", Json::array({1.0, 1.0, 1.0})}};
            return Json{{"scene_revision", revision},
                        {"authoring_object_id", 2},
                        {"declaration_index", 0},
                        {"name", "Cube"},
                        {"components",
                         Json::array({{{"name", "transform"},
                                       {"component_index", 0},
                                       {"authored_json", transform},
                                       {"runtime_json", transform},
                                       {"editable", true},
                                       {"pending", false},
                                       {"codec", {{"state", "registered"}}},
                                       {"schema",
                                        {{"state", "available"},
                                         {"fields",
                                          Json::array({{{"name", "pos"}, {"type", "vec3"}},
                                                       {{"name", "rotation"}, {"type", "quat"}},
                                                       {{"name", "scale"}, {"type", "vec3"}}})}}}}})}};
        }
        // The scene's Undo / Redo.
        if (method == "undo" || method == "redo")
            return Json{{"status", "accepted"}, {"ticket", "scene:" + std::to_string(next_ticket_++)}};
        // The render history's reads and restores, and the edit it follows.
        if (method == "get_render_documents")
            return Json{{"documents",
                         Json::array({{{"reference", Preset},
                                       {"kind", "preset"},
                                       {"digest", preset_digest_},
                                       {"content", "preset " + preset_digest_.substr(0, 1)}}})}};
        if (method == "edit_preset_pass" || method == "restore_render_documents")
            return Json{{"status", "accepted"}, {"ticket", method + ":" + std::to_string(next_ticket_++)}};
        if (method == "get_edit_result") {
            const auto ticket = params.at("ticket").get<std::string>();
            if (ticket.starts_with("scene:"))
                return Json{{"status", "committed"}, {"committed_revision", ++revision}};
            preset_digest_ = ticket.starts_with("edit_preset_pass") ? PresetAfter : PresetBefore;
            return Json{{"status", "committed"},
                        {"committed", true},
                        {"source_digest", {{"hex", std::string(64, 'a')}}},
                        {"documents", {{Preset, preset_digest_}}}};
        }
        return std::nullopt;
    }
};

struct ProjectSandbox {
    std::filesystem::path root;

    ProjectSandbox() {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        root = std::filesystem::temp_directory_path() / ("pelican_render_undo_keys_" + suffix);
        std::filesystem::create_directories(root / "scenes");
        std::filesystem::create_directories(root / "config");
        write("project.json", R"json({
  "schema":"pelican.project","version":1,"name":"ru1",
  "basic_config":{"scene_data_json":"scenes/main.scene.json"}
})json");
        write("scenes/main.scene.json", R"json({
  "schema":"pelican.scene","version":2,"scenes":{"main":{"objects":[]}}
})json");
    }

    ~ProjectSandbox() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    void write(const char *relative, const char *text) {
        std::ofstream stream(root / relative, std::ios::binary);
        stream << text;
    }
};

// Counts the Ctrl+Z key presses that reach a widget as keys (not as a
// shortcut somebody else took).
struct KeySpy : QObject {
    int undo_keys = 0;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent *>(event)->matches(QKeySequence::Undo))
            ++undo_keys;
        return false;
    }
};

struct Rig {
    QApplication &app = application();
    ProjectSandbox sandbox;
    MainWindow window{QString::fromStdWString((sandbox.root / "config").wstring())};
    EmbeddedViewport &viewport = *window.viewport_;
    FakeEngine engine{viewport};
    int render_undos = 0;
    int render_redos = 0;

    Rig() {
        QString error;
        REQUIRE(viewport.process_.start(
            {QString::fromUtf8(PELICAN_PROCESS_FIXTURE_PATH), {QStringLiteral("rpc-hold")}, {}},
            &error));
        REQUIRE(viewport.process_.waitForStarted(5000));
        window.project_model_ = ProjectOutlinerModel::open(sandbox.root);
        window.selection_model_.bindProject(&*window.project_model_);
        window.resize(1400, 900);
        window.show();
        window.activateWindow();
        REQUIRE(QTest::qWaitForWindowActive(&window));
        viewport.rpc_ready_ = true;
        emit viewport.engineRpcBecameAvailable();
        QObject::connect(window.render_undo_action_, &QAction::triggered,
                         [this] { ++render_undos; });
        QObject::connect(window.render_redo_action_, &QAction::triggered,
                         [this] { ++render_redos; });
        REQUIRE(pumpUntil([&] { return row("Cube") != nullptr; }));
    }

    bool pumpUntil(const std::function<bool()> &done, int milliseconds = 8000) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < milliseconds) {
            engine.pump();
            if (done()) return true;
            QTest::qWait(5);
        }
        return done();
    }

    void pumpFor(int milliseconds) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < milliseconds) {
            engine.pump();
            QTest::qWait(5);
        }
    }

    QTreeWidgetItem *row(const QString &text) {
        for (QTreeWidgetItemIterator item{window.outliner_}; *item != nullptr; ++item)
            if ((*item)->text(0) == text) return *item;
        return nullptr;
    }

    // A panel's render edit, as a panel sends it: through the viewport.
    void renderEdit() {
        const auto before = window.render_history_->undoStack().size();
        QString error;
        REQUIRE(viewport.requestRpc(QStringLiteral("edit_preset_pass"),
                                    QJsonObject{{QStringLiteral("graph"), QStringLiteral("main_render")},
                                                {QStringLiteral("pass_name"), QStringLiteral("present")}},
                                    &error) != 0);
        REQUIRE(pumpUntil([&] { return window.render_history_->undoStack().size() == before + 1; }));
        pumpFor(50);
    }

    template <typename Widget> Widget &named(const char *name) {
        auto *found = window.findChild<Widget *>(QString::fromLatin1(name));
        REQUIRE(found != nullptr);
        return *found;
    }

    // Ctrl+`key` with the keyboard in `widget`.
    void press(QWidget &widget, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::ControlModifier) {
        INFO("focus into " << widget.objectName().toStdString() << " (visible "
                           << widget.isVisible() << ", enabled " << widget.isEnabled() << ")");
        widget.activateWindow();
        widget.setFocus(Qt::OtherFocusReason);
        REQUIRE(pumpUntil([&] { return QApplication::focusWidget() == &widget; }, 2000));
        QTest::keyClick(&widget, key, modifiers);
        pumpFor(100);
    }
};

} // namespace

TEST_CASE("RU-1 Ctrl+Z is the render history's in the Frame Plan and the Pass Editor, the "
          "graph's in the Graph tab and the scene's in the Inspector",
          "[devstudio][render-history][ru1][keys][negative-contrast]") {
    Rig rig;
    rig.renderEdit();
    REQUIRE(rig.window.render_history_->undoLabel() == "Edit pass present");
    CHECK(rig.window.render_undo_action_->isEnabled());
    CHECK(rig.window.render_undo_action_->toolTip() == QStringLiteral("Undo Edit pass present"));
    // The Frame Plan's Undo button follows the action.
    auto &undo_button = rig.named<QPushButton>("pelican.framePlan.undoButton");
    CHECK(undo_button.isEnabled());
    CHECK(undo_button.toolTip() == QStringLiteral("Undo Edit pass present"));

    // The Frame Plan: the render history, all the way to its restore.
    REQUIRE(rig.window.showRenderToolsTab(QStringLiteral("frame_plan")));
    // The canvas takes no focus without a plan (this engine has none); the
    // panel's Refresh button is as much inside the Frame Plan.
    auto &frame_plan_refresh = rig.named<QPushButton>("pelican.framePlanRefresh");
    const auto restores = rig.engine.count("restore_render_documents");
    rig.press(frame_plan_refresh, Qt::Key_Z);
    CHECK(rig.render_undos == 1);
    REQUIRE(rig.pumpUntil([&] { return rig.window.render_history_->redoStack().size() == 1; }));
    CHECK(rig.engine.count("restore_render_documents") == restores + 1);
    CHECK(rig.window.render_history_->line() == "Undid \"Edit pass present\".");

    // Ctrl+Y there is its Redo, and Ctrl+Shift+Z too.
    rig.press(frame_plan_refresh, Qt::Key_Y);
    CHECK(rig.render_redos == 1);
    REQUIRE(rig.pumpUntil([&] { return rig.window.render_history_->undoStack().size() == 1; }));
    rig.press(frame_plan_refresh, Qt::Key_Z);
    REQUIRE(rig.pumpUntil([&] { return rig.window.render_history_->redoStack().size() == 1; }));
    rig.press(frame_plan_refresh, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(rig.render_redos == 2);
    REQUIRE(rig.pumpUntil([&] { return rig.window.render_history_->undoStack().size() == 1; }));

    // A text field in the Frame Plan keeps Ctrl+Z for its text.
    const int undos = rig.render_undos;
    auto &filter = rig.named<QLineEdit>("pelican.framePlanFilter");
    rig.press(filter, Qt::Key_Z);
    CHECK(rig.render_undos == undos);

    // The Graph tab: the key goes to its canvas, whose own history takes it.
    REQUIRE(rig.window.showRenderToolsTab(QStringLiteral("graph")));
    auto &graph_view = rig.named<QGraphicsView>("pelican.graphCanvasView");
    KeySpy graph_keys;
    graph_view.installEventFilter(&graph_keys);
    rig.press(graph_view, Qt::Key_Z);
    CHECK(graph_keys.undo_keys == 1);
    CHECK(rig.render_undos == undos);

    // The Inspector: the scene's Undo, once an object is selected.
    auto &outliner = *rig.window.outliner_;
    QTreeWidgetItem *cube = rig.row(QStringLiteral("Cube"));
    REQUIRE(cube != nullptr);
    QTest::mouseClick(outliner.viewport(), Qt::LeftButton, {}, outliner.visualItemRect(cube).center());
    auto &scene_undo = rig.named<QPushButton>("pelican.inspectorUndo");
    REQUIRE(rig.pumpUntil([&] { return scene_undo.isEnabled(); }));
    auto &refresh = rig.named<QPushButton>("pelican.inspectorRefresh");
    const auto scene_undos = rig.engine.count("undo");
    rig.press(refresh, Qt::Key_Z);
    REQUIRE(rig.pumpUntil([&] { return rig.engine.count("undo") == scene_undos + 1; }));
    CHECK(rig.render_undos == undos);

    // The Pass Editor: the render history again.
    rig.window.docks_[MainWindow::FullscreenPassDock]->show();
    rig.window.docks_[MainWindow::FullscreenPassDock]->raise();
    auto &pass_editor = *rig.window.docks_[MainWindow::FullscreenPassDock]->widget();
    if (pass_editor.focusPolicy() == Qt::NoFocus) pass_editor.setFocusPolicy(Qt::StrongFocus);
    rig.press(pass_editor, Qt::Key_Z);
    CHECK(rig.render_undos == undos + 1);
    REQUIRE(rig.pumpUntil([&] { return rig.window.render_history_->redoStack().size() == 1; }));
}

} // namespace PelicanStudio
