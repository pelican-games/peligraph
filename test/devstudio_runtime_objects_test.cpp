// PM-3c: the Outliner's rows for the objects the running game spawned, and
// the Inspector's read-only view of one. The model on its own first; then the
// real window over the real RPC transport: a child process
// (process_fixture_child rpc-hold) takes every request the window sends and
// writes it back on stderr, and this test answers in the engine's place
// through the transport's own signals, so the viewport's routing (the
// get_status reads it hands on, the refusals it passes whole) is the
// production one. The fake engine keeps a play state (edit, playing, paused)
// and lists its spawned objects the way RO-1 does: none in the edit state.
//
// What is judged is what a person sees and what the engine is sent.

// Everything the two headers below include, first, so that only their own
// classes are opened up (as in the Outliner edit test).
#include "layoutpreset.hpp"
#include "toollayoutpreset.hpp"
#include "../src/devstudio/model/gizmomodel.hpp"
#include "../src/devstudio/model/inspectormodel.hpp"
#include "../src/devstudio/model/project.hpp"
#include "../src/devstudio/model/runtimeobjects.hpp"
#include "../src/devstudio/model/selection.hpp"
#include "../src/devstudio/viewport/engineattach.hpp"
#include "../src/devstudio/viewport/enginefailuremodel.hpp"
#include "../src/devstudio/viewport/enginelogbuffer.hpp"
#include "../src/devstudio/viewport/engineprocess.hpp"
#include "../src/devstudio/viewport/nativewindowhost.hpp"
#include "../src/devstudio/viewport/viewportgeometry.hpp"
#include "inspectorwidget.hpp"
#include "frameplanwidget.hpp"
#include "graphcanvaswidget.hpp"
#include "../src/devstudio/guitrace.hpp"
#include "outlinertree.hpp"
#include "runtimeoutliner.hpp"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QGroupBox>
#include <QGraphicsSceneMouseEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPalette>
#include <QPoint>
#include <QPushButton>
#include <QScrollArea>
#include <QStatusBar>
#include <QTest>
#include <QTimer>
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
#include <map>
#include <optional>
#include <string>
#include <utility>
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
    static char application_name[] = "pelican_runtime_objects_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

Json status(const char *state, std::uint64_t game_frame = 10) {
    return {{"frame", 900},
            {"scene", "main"},
            {"play",
             {{"state", state},
              {"game_frame", game_frame},
              {"game_time", 0.5},
              {"pending_steps", 0}}}};
}

Json shot(std::uint32_t index = 7, double x = 1.5) {
    return {{"name", "Shot"},
            {"entity", {{"index", index}, {"generation", 1}}},
            {"spawned_by", {{"prefab", "pm3c_shot"}}},
            {"transform",
             {{"local_trs",
               {{"pos", {x, 2.0, -0.5}},
                {"rotation", {0.0, 0.0, 0.0, 1.0}},
                {"scale", {1.0, 1.0, 1.0}}}},
              {"world_trs",
               {{"pos", {x, 2.0, -0.5}},
                {"rotation", {0.0, 0.0, 0.0, 1.0}},
                {"scale", {1.0, 1.0, 1.0}}}}}}};
}

// The one request the model has queued; it must be get_runtime_objects {}.
std::uint64_t oneRead(RuntimeObjectsModel &model) {
    const auto requests = model.takeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests.front().method == "get_runtime_objects");
    CHECK(requests.front().params == Json::object());
    return requests.front().request_id;
}

// The engine as far as the Outliner, the Inspector and the play line can
// tell: one scene (Camera, Cube), a play state, and the objects it spawned.
class FakeEngine {
  public:
    explicit FakeEngine(EmbeddedViewport &viewport) : viewport_{viewport} {
        QObject::connect(&viewport, &EmbeddedViewport::engineStandardErrorReceived,
                         [this](const QString &text) { receive(text); });
    }

    std::string state = "playing";
    std::uint64_t game_frame = 100;
    // What a running game has spawned; get_runtime_objects lists none in edit.
    Json spawned = Json::array({shot()});
    // An engine from before RO-1: get_runtime_objects is an unknown method.
    bool knows_runtime_objects = true;
    int runtime_reads = 0;
    int frame_plan_reads = 0;
    std::uint64_t frame_plan_generation = 21;
    // Every operation an `edit` carried.
    std::vector<Json> operations;

    int graph_node_count = 4;

    Json graphRead() const {
        Json nodes = Json::array({
            {{"id", "first"}, {"op", "render.fullscreen"}, {"parameters", Json::object()},
             {"outputs", Json::array({"color"})}},
            {{"id", "second"}, {"op", "render.fullscreen"}, {"parameters", Json::object()},
             {"outputs", Json::array({"color"})}},
            {{"id", "value_a"}, {"op", "graph.value"}, {"parameters", Json::object()},
             {"outputs", Json::array({"value"})}},
            {{"id", "value_b"}, {"op", "graph.value"}, {"parameters", Json::object()},
             {"outputs", Json::array({"value"})}}});
        for (int index = 4; index < graph_node_count; ++index)
            nodes.push_back({{"id", "node_" + std::to_string(index)},
                             {"op", "render.fullscreen"}, {"parameters", Json::object()},
                             {"outputs", Json::array({"color"})}});
        return {{"source_reference", "fp1"}, {"source_digest", {{"hex", "fp1"}}},
                {"instances", Json::array()}, {"definitions", Json::object()},
                {"nodes", std::move(nodes)}};
    }

    Json framePlan() const {
        Json nodes = Json::array();
        Json resources = Json::array();
        for (const auto &graph_node : graphRead().at("nodes")) {
            const std::string name = graph_node.at("id").get<std::string>();
            if (graph_node.at("op") != "render.fullscreen") continue;
            const int order = static_cast<int>(nodes.size());
            nodes.push_back({{"name", name}, {"kind", "render"}, {"order", order},
                             {"level", order}, {"source", "project"},
                             {"origin", {{"node", name}}},
                             {"reads", Json::array()}, {"writes", Json::array({name})},
                             {"reads_history", Json::array()}, {"declaration_index", order},
                             {"shader_resolution", {{"state", "not_applicable"}}}});
            resources.push_back({{"name", name}, {"kind", "render_target"}, {"format", "RGBA8"},
                                 {"writers", Json::array({name})}, {"readers", Json::array()},
                                 {"from_nodes", Json::array({name})}});
        }
        return {{"schema", "pelican.frame_plan"}, {"version", 1},
                {"profile", "runtime"}, {"graph", "fp1_graph"},
                {"graph_variant", {{"name", "flat"}}},
                {"runtime_generation", frame_plan_generation},
                {"nodes", std::move(nodes)}, {"resources", std::move(resources)},
                {"barriers", Json::array()}, {"loops", Json::array()}};
    }

    void pump() {
        QApplication::processEvents();
        while (!pending_.empty()) {
            const Json request = std::move(pending_.front());
            pending_.pop_front();
            answer(request);
            QApplication::processEvents();
        }
        if (state == "playing") ++game_frame;
    }

  private:
    EmbeddedViewport &viewport_;
    QString buffer_;
    std::deque<Json> pending_;

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

    void result(qint64 id, const Json &value) {
        emit viewport_.process_.rpcResultReceived(id, QByteArray::fromStdString(value.dump()));
    }

    void error(qint64 id, int code, const std::string &message) {
        emit viewport_.process_.rpcErrorReceived(id, code, QString::fromStdString(message),
                                                 QJsonValue{QJsonObject{}});
    }

    Json sceneTree() const {
        return {{"scene_revision", 1},
                {"scene_id", "main"},
                {"objects",
                 Json::array({{{"authoring_object_id", 1},
                               {"declaration_index", 0},
                               {"name", "Camera"},
                               {"components", Json::array()}},
                              {{"authoring_object_id", 2},
                               {"declaration_index", 1},
                               {"name", "Cube"},
                               {"components", Json::array()}}})}};
    }

    void answer(const Json &request) {
        const auto id = request.at("id").get<qint64>();
        const auto method = request.at("method").get<std::string>();
        const Json params = request.value("params", Json::object());
        if (method == "get_graph_instances") {
            result(id, graphRead());
            return;
        }
        if (method == "get_frame_plan") {
            ++frame_plan_reads;
            result(id, framePlan());
            return;
        }
        if (method == "get_graph_catalog" || method == "get_live_inputs") {
            result(id, method == "get_graph_catalog"
                           ? Json{{"schema", "pelican.graph_catalog"}, {"version", 1},
                                  {"modules", Json::array()}}
                           : Json{{"inputs", Json::array()}});
            return;
        }
        if (method == "get_status") {
            Json reading = status(state.c_str(), game_frame);
            result(id, reading);
            return;
        }
        if (method == "get_runtime_objects") {
            ++runtime_reads;
            if (!knows_runtime_objects) {
                error(id, -32601, "Method not found");
                return;
            }
            result(id, {{"objects", state == "edit" ? Json::array() : spawned}});
            return;
        }
        if (method == "set_play_state") {
            const auto previous = state;
            state = params.at("state").get<std::string>();
            result(id, {{"state", state}, {"previous", previous}});
            return;
        }
        if (method == "open_editor_session") {
            result(id, {{"actor_id", 9}, {"display_name", "Pelican Studio Inspector"}});
            return;
        }
        if (method == "scene_tree") {
            result(id, sceneTree());
            return;
        }
        if (method == "scene_outline") {
            // UX-13c: the same two objects, as scene_outline lists them.
            result(id, {{"scene_id", "main"},
                        {"scene_revision", 1},
                        {"not_modified", false},
                        {"objects",
                         Json::array({{{"authoring_object_id", 1},
                                       {"declaration_index", 0},
                                       {"name", "Camera"},
                                       {"parent_authoring_object_id", nullptr}},
                                      {{"authoring_object_id", 2},
                                       {"declaration_index", 1},
                                       {"name", "Cube"},
                                       {"parent_authoring_object_id", nullptr}}})}});
            return;
        }
        if (method == "get_scene_revision") {
            result(id, {{"scene_revision", 1}, {"preview_epoch", 0}});
            return;
        }
        if (method == "edit") {
            for (const auto &operation : params.at("operations")) operations.push_back(operation);
            result(id, {{"status", "accepted"}, {"ticket", "edit:1"}});
            return;
        }
        error(id, -32601, "not in this fake engine");
    }
};

struct ProjectSandbox {
    std::filesystem::path root;

    ProjectSandbox() {
        const auto suffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        root = std::filesystem::temp_directory_path() / ("pelican_runtime_objects_" + suffix);
        std::filesystem::create_directories(root / "scenes");
        std::filesystem::create_directories(root / "config");
        write("project.json", R"json({
  "schema":"pelican.project","version":1,"name":"pm3c",
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

const char *const RuntimeRow = "Shot (runtime)";

struct Rig {
    QApplication &app = application();
    ProjectSandbox sandbox;
    MainWindow window{QString::fromStdWString((sandbox.root / "config").wstring())};
    EmbeddedViewport &viewport = *window.viewport_;
    FakeEngine engine{viewport};
    OutlinerTree &outliner = *window.outliner_;
    InspectorWidget &inspector = *window.inspector_;

    explicit Rig(std::string state = "playing", bool knows_runtime_objects = true,
                 int graph_node_count = 4) {
        engine.state = std::move(state);
        engine.knows_runtime_objects = knows_runtime_objects;
        engine.graph_node_count = graph_node_count;
        QString error;
        REQUIRE(viewport.process_.start(
            {QString::fromUtf8(PELICAN_PROCESS_FIXTURE_PATH), {QStringLiteral("rpc-hold")}, {}},
            &error));
        REQUIRE(viewport.process_.waitForStarted(5000));
        // What opening a project does, without starting an engine for it.
        window.project_model_ = ProjectOutlinerModel::open(sandbox.root);
        window.selection_model_.bindProject(&*window.project_model_);
        window.resize(1200, 800);
        window.show();
        window.activateWindow();
        REQUIRE(QTest::qWaitForWindowActive(&window));
        viewport.rpc_ready_ = true;
        emit viewport.engineRpcBecameAvailable();
        REQUIRE(pumpUntil([&] { return row("Cube") != nullptr; }));
    }

    void pumpFor(int milliseconds) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < milliseconds) {
            engine.pump();
            QTest::qWait(5);
        }
    }

    bool pumpUntil(const std::function<bool()> &done, int milliseconds = 5000) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < milliseconds) {
            engine.pump();
            if (done()) return true;
            QTest::qWait(5);
        }
        return done();
    }

    QTreeWidgetItem *row(const QString &text) const {
        for (QTreeWidgetItemIterator item{&outliner}; *item != nullptr; ++item)
            if ((*item)->text(0) == text) return *item;
        return nullptr;
    }
    QTreeWidgetItem *runtimeRow() const { return row(QString::fromUtf8(RuntimeRow)); }

    // Studio reads the play state, as the play controls do after a change.
    void readStatus() {
        QString error;
        REQUIRE(viewport.requestRpc(QStringLiteral("get_status"), QJsonObject{}, &error) != 0);
    }

    void click(QTreeWidgetItem *item) {
        REQUIRE(item != nullptr);
        outliner.scrollToItem(item);
        QTest::mouseClick(outliner.viewport(), Qt::LeftButton, {},
                          outliner.visualItemRect(item).center());
        REQUIRE(outliner.currentItem() == item);
    }

    void mouse(QEvent::Type type, const QPoint &position, Qt::MouseButtons held) {
        QMouseEvent event(type, QPointF{position}, outliner.viewport()->mapToGlobal(QPointF{position}),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held,
                          Qt::NoModifier, QPointingDevice::primaryPointingDevice());
        QApplication::sendEvent(outliner.viewport(), &event);
    }

    // The tree's own drag: a press, moves with the button held, a release.
    void drag(QTreeWidgetItem *from, QTreeWidgetItem *onto) {
        const QPoint start = outliner.visualItemRect(from).center();
        const QPoint end = outliner.visualItemRect(onto).center();
        mouse(QEvent::MouseButtonPress, start, Qt::LeftButton);
        for (int step = 1; step <= 8; ++step)
            mouse(QEvent::MouseMove, start + (end - start) * step / 8, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, end, Qt::NoButton);
    }

    template <class T> T *named(const char *name) const {
        return window.findChild<T *>(QString::fromLatin1(name));
    }
    QString selectionText() const { return named<QLabel>("pelican.inspectorSelection")->text(); }
    bool runtimeViewShown() const { return !named<QGroupBox>("pelican.inspectorRuntime")->isHidden(); }
};

} // namespace

TEST_CASE("PM-3c get_runtime_objects is read as RO-1 writes it", "[devstudio][play][pm3c]") {
    const auto objects = parseRuntimeObjects(Json{
        {"objects",
         Json::array({shot(),
                      // A spawn with no name or entity cannot be a row.
                      {{"name", "Nameless"}},
                      {{"name", "spawn:2.0"}, {"entity", {{"index", 9}, {"generation", 3}}}}})}});
    REQUIRE(objects);
    REQUIRE(objects->size() == 2);
    const RuntimeObject &first = objects->front();
    CHECK(first.name == "Shot");
    CHECK(first.entity == RuntimeObjectEntity{.index = 7, .generation = 1});
    CHECK(first.prefab == "pm3c_shot");
    REQUIRE(first.local);
    CHECK(first.local->pos == std::array<double, 3>{1.5, 2.0, -0.5});
    CHECK(first.local->rotation == std::array<double, 4>{0.0, 0.0, 0.0, 1.0});
    REQUIRE(first.world);
    CHECK(first.world->pos == std::array<double, 3>{1.5, 2.0, -0.5});
    // No spawned_by, no transform: an unnamed spawn's own form, kept.
    CHECK(objects->back().name == "spawn:2.0");
    CHECK(objects->back().prefab.empty());
    CHECK_FALSE(objects->back().local);

    CHECK_FALSE(parseRuntimeObjects(Json::array()));
    CHECK_FALSE(parseRuntimeObjects(Json{{"objects", 3}}));
}

TEST_CASE("PM-3c the list is read when the play state moves and while it plays, never in edit",
          "[devstudio][play][pm3c][negative-contrast]") {
    RuntimeObjectsModel model;
    // The readiness probe's reading comes before the engine is called ready.
    CHECK(model.observeStatus(status("playing")));
    CHECK(model.takeRequests().empty());
    model.connect();
    auto read = oneRead(model);
    CHECK(model.owns(read));
    // One at a time: the clock does not stack a second read on it.
    CHECK_FALSE(model.tick());
    model.receiveResult(read, Json{{"objects", Json::array({shot()})}});
    REQUIRE(model.objects().size() == 1);
    CHECK(model.objects().front().name == "Shot");
    CHECK(model.sceneId() == "main");

    // Playing: the clock reads; another panel's get_status does not.
    CHECK(model.tickWanted());
    CHECK(model.observeStatus(status("playing", 11)));
    CHECK(model.takeRequests().empty());
    CHECK(model.tick());
    model.receiveResult(oneRead(model), Json{{"objects", Json::array({shot(7, 3.0)})}});
    REQUIRE(model.objects().front().local);
    CHECK((*model.objects().front().local->pos)[0] == 3.0);

    // Paused: read once on the change, again only when a step moved the game.
    CHECK(model.observeStatus(status("paused", 40)));
    CHECK_FALSE(model.tickWanted());
    model.receiveResult(oneRead(model), Json{{"objects", Json::array({shot()})}});
    CHECK(model.observeStatus(status("paused", 40)));
    CHECK(model.takeRequests().empty());
    CHECK(model.observeStatus(status("paused", 41)));
    read = oneRead(model);

    // Stop. The edit state lists nothing: the rows go at once, the answer
    // still on its way is about the game that stopped, and nothing is read.
    CHECK(model.observeStatus(status("edit", 0)));
    CHECK(model.objects().empty());
    CHECK_FALSE(model.tickWanted());
    CHECK_FALSE(model.tick());
    model.receiveResult(read, Json{{"objects", Json::array({shot()})}});
    CHECK(model.objects().empty());
    CHECK(model.observeStatus(status("edit", 0)));
    CHECK(model.takeRequests().empty());

    // The contrast: Play again, and it is read.
    CHECK(model.observeStatus(status("playing", 1)));
    model.receiveResult(oneRead(model), Json{{"objects", Json::array({shot(8)})}});
    CHECK(model.objects().size() == 1);

    // An engine that starts in edit is not asked at all.
    RuntimeObjectsModel edit_start;
    CHECK(edit_start.observeStatus(status("edit", 0)));
    edit_start.connect();
    CHECK(edit_start.takeRequests().empty());
    CHECK_FALSE(edit_start.tickWanted());
}

TEST_CASE("PM-3c an engine without get_runtime_objects is asked once",
          "[devstudio][play][pm3c]") {
    RuntimeObjectsModel model;
    model.observeStatus(status("playing"));
    model.connect();
    model.receiveFailure(oneRead(model), true);
    CHECK_FALSE(model.supported());
    CHECK_FALSE(model.tickWanted());
    CHECK_FALSE(model.tick());
    CHECK(model.observeStatus(status("paused", 3)));
    CHECK(model.takeRequests().empty());

    // A failure of another kind leaves it asking; the next engine is new.
    model.disconnect();
    CHECK(model.supported());
    model.observeStatus(status("playing"));
    model.connect();
    model.receiveFailure(oneRead(model), false);
    CHECK(model.supported());
    CHECK(model.tick());
}

TEST_CASE("PM-3c a selected spawned object is let go when it leaves the list",
          "[devstudio][play][pm3c]") {
    RuntimeObjectsModel model;
    model.observeStatus(status("playing"));
    model.connect();
    model.receiveResult(oneRead(model), Json{{"objects", Json::array({shot()})}});
    CHECK_FALSE(model.select({.index = 3, .generation = 1}));
    REQUIRE(model.select({.index = 7, .generation = 1}));
    REQUIRE(model.selectedObject() != nullptr);
    CHECK(model.selectedObject()->name == "Shot");
    CHECK(model.tick());
    model.receiveResult(oneRead(model), Json{{"objects", Json::array()}});
    CHECK_FALSE(model.selected());
    CHECK(model.selectedObject() == nullptr);
}

TEST_CASE("PM-3c a spawned object is a greyed runtime row after the scene's own, read-only in "
          "the Inspector, and no Outliner edit reaches it",
          "[devstudio][play][pm3c][negative-contrast]") {
    Rig rig;
    REQUIRE(rig.pumpUntil([&] { return rig.runtimeRow() != nullptr; }));
    QTreeWidgetItem *runtime = rig.runtimeRow();
    QTreeWidgetItem *cube = rig.row(QStringLiteral("Cube"));
    REQUIRE(cube != nullptr);

    // Root level of the running scene, after its own objects.
    REQUIRE(runtime->parent() != nullptr);
    CHECK(runtime->parent() == cube->parent());
    CHECK(runtime->parent()->text(0) == QStringLiteral("main"));
    CHECK(runtime->parent()->indexOfChild(runtime) == runtime->parent()->childCount() - 1);
    CHECK(runtime->parent()->indexOfChild(cube) < runtime->parent()->indexOfChild(runtime));
    // Greyed, with the prefab it came from and why it cannot be edited.
    CHECK(runtime->foreground(0).style() != Qt::NoBrush);
    CHECK(runtime->foreground(0).color() ==
          rig.outliner.palette().color(QPalette::Disabled, QPalette::Text));
    CHECK(cube->foreground(0).style() == Qt::NoBrush);
    CHECK(runtime->toolTip(0).contains(QStringLiteral("pm3c_shot")));
    CHECK(runtime->toolTip(0).contains(RuntimeOutliner::editRefusal()));
    // Not an object the scene declares: no key for any edit to name.
    CHECK_FALSE(outlinerObjectKey(runtime));
    REQUIRE(outlinerRuntimeEntity(runtime));
    CHECK(*outlinerRuntimeEntity(runtime) == RuntimeObjectEntity{.index = 7, .generation = 1});

    // Selected: the Inspector shows it, read-only.
    rig.click(cube);
    REQUIRE(rig.pumpUntil([&] { return rig.window.selection_model_.selected().has_value(); }));
    CHECK(rig.window.outliner_->duplicateAction()->isEnabled());
    CHECK_FALSE(rig.runtimeViewShown());
    rig.click(runtime);
    REQUIRE(rig.pumpUntil([&] { return rig.runtimeViewShown(); }));
    CHECK_FALSE(rig.window.selection_model_.selected());
    CHECK(rig.selectionText().startsWith(QStringLiteral("Shot (runtime)\nspawned from pm3c_shot")));
    CHECK(rig.named<QLabel>("pelican.inspectorRuntime.position")->text() ==
          QStringLiteral("1.5, 2, -0.5"));
    CHECK(rig.named<QLabel>("pelican.inspectorRuntime.rotation")->text() ==
          QStringLiteral("0, 0, 0, 1"));
    CHECK(rig.named<QLabel>("pelican.inspectorRuntime.worldPosition")->text() ==
          QStringLiteral("1.5, 2, -0.5"));
    CHECK(rig.named<QScrollArea>("pelican.inspectorProperties")->isHidden());
    CHECK(rig.named<QPushButton>("pelican.inspectorAddComponent")->isHidden());
    CHECK(rig.named<QPushButton>("pelican.inspectorRayGeometry")->isHidden());
    CHECK(rig.inspector.showsRuntimeObject());

    // A new reading moves what the Inspector shows, and keeps the row.
    rig.engine.spawned = Json::array({shot(7, 4.0)});
    REQUIRE(rig.pumpUntil(
        [&] {
            return rig.named<QLabel>("pelican.inspectorRuntime.position")->text() ==
                   QStringLiteral("4, 2, -0.5");
        },
        3000));
    CHECK(rig.outliner.currentItem() == rig.runtimeRow());
    runtime = rig.runtimeRow();

    // Duplicate and Delete are off, and say why.
    QAction &duplicate = *rig.outliner.duplicateAction();
    QAction &remove = *rig.outliner.deleteAction();
    CHECK_FALSE(duplicate.isEnabled());
    CHECK_FALSE(remove.isEnabled());
    CHECK(duplicate.toolTip() == RuntimeOutliner::editRefusal());
    CHECK(remove.toolTip() == RuntimeOutliner::editRefusal());
    rig.outliner.setFocus();
    QTest::keyClick(&rig.outliner, Qt::Key_Delete);
    QTest::keyClick(&rig.outliner, Qt::Key_D, Qt::ControlModifier);
    rig.pumpFor(100);
    CHECK(rig.engine.operations.empty());
    CHECK(rig.window.findChild<QMessageBox *>(QStringLiteral("pelican.outliner.deleteConfirm")) ==
          nullptr);

    // Nothing goes under it, and it goes nowhere.
    CHECK(rig.outliner.dropTargetAt(cube, rig.outliner.visualItemRect(runtime).center()).place ==
          OutlinerTree::DropPlace::Refused);
    rig.drag(cube, runtime);
    rig.pumpFor(100);
    CHECK(rig.engine.operations.empty());
    CHECK(rig.named<QLabel>("pelican.outlinerNotice")->text() == RuntimeOutliner::editRefusal());
    rig.drag(rig.runtimeRow(), rig.row(QStringLiteral("Cube")));
    rig.pumpFor(100);
    CHECK(rig.engine.operations.empty());

    // A declared object again: its tooltips and its fields come back.
    rig.click(rig.row(QStringLiteral("Cube")));
    REQUIRE(rig.pumpUntil([&] { return !rig.runtimeViewShown(); }));
    CHECK_FALSE(rig.named<QScrollArea>("pelican.inspectorProperties")->isHidden());
    CHECK_FALSE(rig.named<QPushButton>("pelican.inspectorAddComponent")->isHidden());
    CHECK(duplicate.toolTip() != RuntimeOutliner::editRefusal());
    CHECK_FALSE(rig.window.runtime_outliner_->model().selected());
}

TEST_CASE("PM-3c Stop empties the runtime rows, and the edit state reads nothing until Play",
          "[devstudio][play][pm3c][negative-contrast]") {
    Rig rig;
    REQUIRE(rig.pumpUntil([&] { return rig.runtimeRow() != nullptr; }));
    rig.click(rig.runtimeRow());
    REQUIRE(rig.pumpUntil([&] { return rig.runtimeViewShown(); }));

    // Stopped where Studio has not read it yet: the next reading is empty.
    rig.engine.state = "edit";
    REQUIRE(rig.pumpUntil([&] { return rig.runtimeRow() == nullptr; }, 3000));
    CHECK(rig.pumpUntil([&] { return !rig.runtimeViewShown(); }, 1000));
    CHECK(rig.selectionText() == QStringLiteral("None"));
    CHECK(rig.row(QStringLiteral("Cube")) != nullptr);

    // Once Studio reads the edit state it asks nothing more.
    rig.readStatus();
    REQUIRE(rig.pumpUntil([&] {
        return rig.window.runtime_outliner_->model().state() == PlayState::edit;
    }));
    const int reads = rig.engine.runtime_reads;
    CHECK_FALSE(rig.window.runtime_outliner_->readTimer()->isActive());
    rig.pumpFor(2500);
    CHECK(rig.engine.runtime_reads == reads);

    // Play: it is read again at once, and its rows come back.
    rig.engine.state = "playing";
    rig.readStatus();
    REQUIRE(rig.pumpUntil([&] { return rig.runtimeRow() != nullptr; }));
    CHECK(rig.engine.runtime_reads > reads);
}

TEST_CASE("PM-3c a Studio attached to an engine in the edit state asks for no runtime objects",
          "[devstudio][play][pm3c][negative-contrast]") {
    Rig rig{"edit"};
    rig.pumpFor(2500);
    CHECK(rig.engine.runtime_reads == 0);
    CHECK(rig.runtimeRow() == nullptr);
    CHECK_FALSE(rig.window.runtime_outliner_->readTimer()->isActive());

    // The contrast: the same Studio, once the game plays.
    rig.engine.state = "playing";
    rig.readStatus();
    REQUIRE(rig.pumpUntil([&] { return rig.runtimeRow() != nullptr; }));
    CHECK(rig.window.runtime_outliner_->readTimer()->isActive());
}

TEST_CASE("FP-1 MainWindow follows the hub only while its panel is visible", "[fp1]") {
    Rig rig;
    auto *graph = static_cast<GraphCanvasWidget *>(
        rig.named<QWidget>("pelican.graphCanvas"));
    auto *plan = static_cast<FramePlanWidget *>(
        rig.named<QWidget>("pelican.framePlan"));
    REQUIRE(graph != nullptr);
    REQUIRE(plan != nullptr);
    auto *graph_dock = rig.named<QDockWidget>("pelican.graphDock");
    auto *plan_dock = rig.named<QDockWidget>("pelican.framePlanDock");
    REQUIRE(graph_dock != nullptr);
    REQUIRE(plan_dock != nullptr);
    graph_dock->show();
    graph_dock->raise();
    plan_dock->hide();
    REQUIRE(rig.pumpUntil([&] {
        return graph->scene().property("pelicanGraphNodeCount").toInt() >= 2;
    }));
    auto node = [&](const QString &name) -> QGraphicsItem * {
        for (auto *item : graph->scene().items())
            if (item->data(NodeGraphNameRole).toString() == name &&
                item->data(NodeGraphItemKindRole).toString() == QLatin1String{GraphCanvasNodeItem})
                return item;
        return nullptr;
    };
    auto *graph_view = graph->findChild<QGraphicsView *>(
        QStringLiteral("pelican.graphCanvasView"));
    REQUIRE(graph_view != nullptr);
    auto click_node = [&](const QString &name) {
        CAPTURE(name);
        graph_dock->show();
        graph_dock->raise();
        QApplication::processEvents();
        INFO("graph pick: " << name.toStdString());
        REQUIRE(rig.pumpUntil([&] { return node(name) != nullptr; }));
        auto *item = node(name);
        REQUIRE(item != nullptr);
        const QPointF point = item->sceneBoundingRect().center();
        CAPTURE(name, point);
        QGraphicsSceneMouseEvent press{QEvent::GraphicsSceneMousePress};
        press.setScenePos(point);
        press.setButton(Qt::LeftButton);
        press.setButtons(Qt::LeftButton);
        QApplication::sendEvent(&graph->scene(), &press);
        QGraphicsSceneMouseEvent release{QEvent::GraphicsSceneMouseRelease};
        release.setScenePos(point);
        release.setButton(Qt::LeftButton);
        QApplication::sendEvent(&graph->scene(), &release);
        if (graph->scene().property("pelicanGraphSelectedNode").toString() != name)
            rig.pumpUntil([&] {
                return graph->scene().property("pelicanGraphSelectedNode").toString() == name;
            });
        REQUIRE(graph->scene().property("pelicanGraphSelectedNode").toString() == name);
        rig.pumpFor(30);
        CAPTURE(static_cast<int>(rig.window.selection_hub_.front()),
                rig.window.selection_hub_.intent(), rig.window.selection_hub_.revision());
        REQUIRE(rig.window.selection_hub_.front() == SelectionFront::graph_node);
        REQUIRE(rig.window.selection_hub_.graphNode().has_value());
        REQUIRE(rig.window.selection_hub_.graphNode()->id == name.toStdString());
    };
    auto *check = rig.named<QCheckBox>("pelican.framePlanFollowGraph");
    auto *status = rig.named<QLabel>("pelican.framePlanFollowStatus");
    auto *target = rig.named<QComboBox>("pelican.framePlanTarget");
    REQUIRE(check != nullptr);
    REQUIRE(status != nullptr);
    REQUIRE(target != nullptr);
    plan_dock->show();
    plan_dock->raise();
    check->setChecked(true);
    // It's Studio's setting: the engine's set_layout refuses anything in
    // frame_plan but a placement per graph, so the project's layout stays clean.
    {
        const auto placements = graph->scene().layoutMember("frame_plan");
        CHECK((!placements.is_object() || !placements.contains("follow_graph")));
    }
    CHECK(rig.window.user_settings_->value(QStringLiteral("frame_plan/follow_graph")).toBool());
    REQUIRE(rig.pumpUntil([&] { return plan->followGraph() && !target->isEnabled(); }));
    INFO("initial Frame Plan reads=" << rig.engine.frame_plan_reads
         << ", status=" << status->toolTip().toStdString()
         << ", plan status=" << rig.named<QLabel>("pelican.framePlanStatus")->toolTip().toStdString());
    REQUIRE(rig.pumpUntil([&] {
        return plan->property("pelicanFramePlanPopulateCount").toInt() > 0;
    }));
    const int plan_reads = rig.engine.frame_plan_reads;
    const int populated = plan->property("pelicanFramePlanPopulateCount").toInt();

    plan_dock->hide();
    click_node(QStringLiteral("first"));
    CHECK(status->toolTip().contains(QStringLiteral("No Graph node picked")));
    // The engine's read count also holds the Graph's own follow-up reads
    // (UX-12c), so the populate count is what says the hidden panel kept still.
    CHECK(plan->property("pelicanFramePlanPopulateCount").toInt() == populated);
    CHECK(plan->property("pelicanFramePlanFollowDirty").toBool());
    const int reads_before_new_plan = rig.engine.frame_plan_reads;
    rig.engine.frame_plan_generation = 22;
    graph->poll();
    REQUIRE(rig.pumpUntil([&] { return rig.engine.frame_plan_reads > reads_before_new_plan; }));
    plan->planChangedByCommit(22, 1);
    click_node(QStringLiteral("second"));
    CHECK(plan->property("pelicanFramePlanPopulateCount").toInt() == populated);
    plan_dock->show();
    plan_dock->raise();
    REQUIRE(rig.pumpUntil([&] {
        return status->toolTip().contains(QStringLiteral("second")) &&
               status->toolTip().contains(QStringLiteral("passes"));
    }));
    CHECK(plan->followGraph());
    CHECK(rig.engine.frame_plan_reads > plan_reads + 1);
    CHECK(plan->property("pelicanFramePlanPopulateCount").toInt() > populated);
    CHECK_FALSE(plan->property("pelicanFramePlanFollowDirty").toBool());
    QApplication::processEvents();
    auto *inspector_graph = rig.named<QWidget>("pelican.inspectorGraphPage");
    auto *inspector_dock = rig.named<QDockWidget>("pelican.inspectorDock");
    REQUIRE(inspector_graph != nullptr);
    REQUIRE(inspector_dock != nullptr);
    inspector_dock->show();
    inspector_dock->raise();
    REQUIRE(rig.pumpUntil([&] { return !inspector_graph->isHidden(); }));

    int before_pick = plan->property("pelicanFramePlanPopulateCount").toInt();
    click_node(QStringLiteral("first"));
    REQUIRE(rig.pumpUntil([&] { return status->toolTip().contains(QStringLiteral("first")); }));
    CHECK(plan->property("pelicanFramePlanPopulateCount").toInt() == before_pick + 1);
    before_pick = plan->property("pelicanFramePlanPopulateCount").toInt();
    click_node(QStringLiteral("first"));
    CHECK(plan->property("pelicanFramePlanPopulateCount").toInt() == before_pick);

    click_node(QStringLiteral("value_a"));
    REQUIRE(rig.pumpUntil([&] {
        return status->toolTip().contains(QStringLiteral("value_a")) &&
               status->toolTip().contains(QStringLiteral("0 passes"));
    }));
    before_pick = plan->property("pelicanFramePlanPopulateCount").toInt();
    click_node(QStringLiteral("value_b"));
    REQUIRE(rig.pumpUntil([&] { return status->toolTip().contains(QStringLiteral("value_b")); }));
    CHECK(status->toolTip().contains(QStringLiteral("0 passes")));
    CHECK(plan->property("pelicanFramePlanPopulateCount").toInt() == before_pick);
    CHECK_FALSE(inspector_graph->isHidden());

    rig.click(rig.row(QStringLiteral("Cube")));
    rig.pumpFor(30);
    CHECK(status->toolTip().contains(QStringLiteral("No Graph node picked")));
    CHECK(inspector_graph->isHidden());

    click_node(QStringLiteral("first"));
    REQUIRE(rig.pumpUntil([&] { return status->toolTip().contains(QStringLiteral("first")); }));
    plan->showPlanNode(QStringLiteral("second"));
    CHECK_FALSE(plan->followGraph());
    CHECK(rig.window.statusBar()->currentMessage().contains(QStringLiteral("Follow Graph turned off")));
}

TEST_CASE("FP-1 Follow Graph full refresh timing at 163 Graph nodes", "[fp1]") {
    Rig rig{"playing", true, 163};
    auto *graph = rig.window.graph_canvas_;
    auto *plan = rig.window.frame_plan_;
    auto *graph_dock = rig.window.docks_[MainWindow::GraphDock];
    auto *plan_dock = rig.window.docks_[MainWindow::FramePlanDock];
    graph_dock->hide();
    plan_dock->show();
    plan_dock->raise();
    rig.named<QCheckBox>("pelican.framePlanFollowGraph")->setChecked(true);
    REQUIRE(rig.pumpUntil([&] {
        return graph->model().nodes().size() == 163 &&
               plan->property("pelicanFramePlanPopulateCount").toInt() > 0;
    }));
    std::vector<double> milliseconds;
    GuiTrace::owner = &milliseconds;
    GuiTrace::sink = [](void *owner, const char *stage, std::int64_t begin,
                        std::int64_t end, double, double) {
        if (QLatin1String{stage} == QLatin1String{"frame_plan.follow_graph"})
            static_cast<std::vector<double> *>(owner)->push_back((end - begin) / 1.0e6);
    };
    rig.window.selection_hub_.pickedGraphNode(GraphNodeRef{.id = "node_100"});
    rig.pumpUntil([&] { return !milliseconds.empty(); });
    GuiTrace::sink = nullptr;
    GuiTrace::owner = nullptr;
    REQUIRE(milliseconds.size() == 1);
    WARN("FP-1 frame_plan.follow_graph at 163 Graph nodes: " << milliseconds.front() << " ms");
    CHECK(rig.named<QLabel>("pelican.framePlanFollowStatus")->toolTip().contains(
        QStringLiteral("node_100")));
}

TEST_CASE("PM-3c an engine from before RO-1 is asked once and shows no runtime rows",
          "[devstudio][play][pm3c]") {
    Rig rig{"playing", false};
    REQUIRE(rig.pumpUntil([&] { return rig.engine.runtime_reads == 1; }));
    rig.pumpFor(2500);
    CHECK(rig.engine.runtime_reads == 1);
    CHECK(rig.runtimeRow() == nullptr);
    CHECK_FALSE(rig.window.runtime_outliner_->model().supported());
}

} // namespace PelicanStudio
