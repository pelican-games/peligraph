// OL-1: the Outliner's Duplicate, Delete and drag on the real window, over
// the real RPC transport. A child process (process_fixture_child rpc-hold)
// takes every request the window sends and writes it back on stderr; this
// test answers in the engine's place, through the viewport's own reply
// signals, keeping a small scene of named objects that duplicate, destroy
// and reparent change the way the engine does.
//
// What is judged is what a person sees and what the engine is sent: which row
// is selected, which rows are open, whether a question is asked, and the one
// operation each gesture puts in an `edit`.

// Everything the two headers below include, first, so that only their own
// classes are opened up. MainWindow and EmbeddedViewport have no test seam;
// as in the Inspector widget test, this translation unit reaches their data
// members (never their private functions) to run the window over the
// viewport's real transport without starting an engine.
#include "layoutpreset.hpp"
#include "toollayoutpreset.hpp"
#include "../src/devstudio/model/gizmomodel.hpp"
#include "../src/devstudio/model/inspectormodel.hpp"
#include "../src/devstudio/model/project.hpp"
#include "../src/devstudio/model/selection.hpp"
#include "../src/devstudio/viewport/engineattach.hpp"
#include "../src/devstudio/viewport/enginefailuremodel.hpp"
#include "../src/devstudio/viewport/enginelogbuffer.hpp"
#include "../src/devstudio/viewport/engineprocess.hpp"
#include "../src/devstudio/viewport/nativewindowhost.hpp"
#include "../src/devstudio/viewport/viewportgeometry.hpp"
#include "outlinertree.hpp"
#include "graphcanvaswidget.hpp"
#include "inspectorwidget.hpp"
#include <QDockWidget>

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCursor>
#include <QElapsedTimer>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QHash>
#include <QJsonValue>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPoint>
#include <QPointer>
#include <QSignalSpy>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
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
    static char application_name[] = "pelican_outliner_edit_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

// The engine, as far as the Outliner and the Inspector can tell: one scene of
// named objects, edits that commit on the first poll, and every commit moving
// the scene revision. Anything else asked of it is refused.
class FakeEngine {
  public:
    struct Object {
        std::uint64_t id = 0;
        std::string name;
        std::string parent;
    };

    explicit FakeEngine(EmbeddedViewport &viewport) : viewport_{viewport} {
        QObject::connect(&viewport, &EmbeddedViewport::engineStandardErrorReceived,
                         [this](const QString &text) { receive(text); });
    }

    std::vector<Object> objects;
    std::uint64_t revision = 1;
    std::uint64_t next_id = 100;
    // Every operation an `edit` carried, in order.
    std::vector<Json> operations;
    // UX-13c: false is an engine from before scene_outline, which refuses it
    // as an unknown method (-32601).
    bool knows_scene_outline = true;
    // HUB-1a: whether the graph document still holds blur. A delete and its
    // Undo move it; no outside reload does.
    bool graph_node_exists = true;
    // HUB-1a: a definition body the Graph can descend into, for the scope
    // change that must not move the shared selection either.
    bool graph_body_scope = false;
    // Every request the window sent, in order, and the size in bytes of each
    // scene_outline / scene_tree answer.
    std::vector<std::pair<std::string, Json>> asked;
    std::vector<std::pair<std::string, std::size_t>> list_replies;

    void pump() {
        QApplication::processEvents();
        while (!pending_.empty()) {
            const Json request = std::move(pending_.front());
            pending_.pop_front();
            const auto id = request.at("id").get<qint64>();
            const auto method = request.at("method").get<std::string>();
            const Json params = request.value("params", Json::object());
            asked.emplace_back(method, params);
            if (method == "scene_outline" && !knows_scene_outline) {
                // As the viewport reports a JSON-RPC error: the code, then
                // the one-line failure.
                emit viewport_.engineRpcRefused(id, -32601, QStringLiteral("Method not found"),
                                                QJsonValue{});
                emit viewport_.inspectorRpcFailed(id, QStringLiteral("RPC -32601: Method not found"));
                QApplication::processEvents();
                continue;
            }
            std::optional<Json> result = answer(method, params);
            if (result) {
                const std::string reply = result->dump();
                if (method == "scene_outline" || method == "scene_tree")
                    list_replies.emplace_back(method, reply.size());
                emit viewport_.inspectorRpcSucceeded(id, QByteArray::fromStdString(reply));
            } else {
                emit viewport_.inspectorRpcFailed(id, QStringLiteral("not in this fake engine"));
            }
            QApplication::processEvents();
        }
    }

    // UX-13c: what scene_tree would answer now, in bytes.
    std::size_t sceneTreeBytes() const { return sceneTree().dump().size(); }

    std::vector<std::string> names() const {
        std::vector<std::string> result;
        for (const auto &object : objects) result.push_back(object.name);
        return result;
    }

  private:
    EmbeddedViewport &viewport_;
    QString buffer_;
    std::deque<Json> pending_;
    std::map<std::string, Json> edits_;
    int next_ticket_ = 1;
    // HUB-1a: the graph document's own history, so a delete and its Undo are
    // real edits through the Graph model rather than a reload from outside.
    std::map<std::string, Json> graph_edits_;
    std::string graph_digest_ = "hub1a";
    int graph_generation_ = 0;
    int next_graph_ticket_ = 1;

    void applyGraphOps(const Json &ops) {
        for (const auto &op : ops) {
            if (!op.is_object()) continue;
            const std::string kind = op.value("op", std::string{});
            std::string id;
            if (op.contains("node") && op.at("node").is_string())
                id = op.at("node").get<std::string>();
            else if (op.contains("node") && op.at("node").is_object())
                id = op.at("node").value("id", std::string{});
            if (id != "blur") continue;
            if (kind == "remove_node") graph_node_exists = false;
            else if (kind == "add_node" || kind == "insert_node") graph_node_exists = true;
        }
        graph_digest_ = "hub1a-" + std::to_string(++graph_generation_);
    }

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

    std::size_t indexOf(std::uint64_t id) const {
        for (std::size_t index = 0; index < objects.size(); ++index)
            if (objects[index].id == id) return index;
        FAIL("the fake engine has no object " << id);
        return 0;
    }

    bool under(std::size_t index, const std::string &ancestor) const {
        for (std::string parent = objects[index].parent; !parent.empty();) {
            if (parent == ancestor) return true;
            std::string next;
            for (const auto &object : objects)
                if (object.name == parent) next = object.parent;
            parent = next;
        }
        return false;
    }

    // The subtree of `root`, as declaration indices in order.
    std::vector<std::size_t> subtree(std::size_t root) const {
        std::vector<std::size_t> result{root};
        for (std::size_t index = 0; index < objects.size(); ++index)
            if (index != root && under(index, objects[root].name)) result.push_back(index);
        return result;
    }

    Json apply(const Json &operation) {
        operations.push_back(operation);
        Json created = Json::array();
        const auto op = operation.at("op").get<std::string>();
        const auto id = operation.at("object_id").get<std::uint64_t>();
        if (op == "duplicate") {
            const auto source = subtree(indexOf(id));
            std::vector<Object> copies;
            std::map<std::string, std::string> renamed;
            for (const auto index : source) {
                Object copy = objects[index];
                copy.id = next_id++;
                copy.name = objects[index].name + "_copy1";
                renamed[objects[index].name] = copy.name;
                copies.push_back(copy);
            }
            for (auto &copy : copies)
                if (renamed.contains(copy.parent)) copy.parent = renamed.at(copy.parent);
            const std::size_t at = source.back() + 1;
            objects.insert(objects.begin() + static_cast<std::ptrdiff_t>(at), copies.begin(),
                           copies.end());
            for (std::size_t offset = 0; offset < copies.size(); ++offset)
                created.push_back({{"object_id", copies[offset].id},
                                   {"scene_id", "main"},
                                   {"declaration_index", at + offset},
                                   {"name", copies[offset].name},
                                   {"source_object_id", objects[source[offset]].id}});
        } else if (op == "destroy") {
            auto doomed = subtree(indexOf(id));
            std::sort(doomed.rbegin(), doomed.rend());
            for (const auto index : doomed)
                objects.erase(objects.begin() + static_cast<std::ptrdiff_t>(index));
        } else if (op == "reparent") {
            const auto &parent = operation.at("new_parent_id");
            objects[indexOf(id)].parent =
                parent.is_null() ? std::string{} : objects[indexOf(parent.get<std::uint64_t>())].name;
        }
        return created;
    }

    Json sceneTree() const {
        Json listed = Json::array();
        for (std::size_t index = 0; index < objects.size(); ++index) {
            Json object{{"authoring_object_id", objects[index].id},
                        {"declaration_index", index},
                        {"name", objects[index].name},
                        {"components", Json::array()}};
            if (!objects[index].parent.empty()) object["parent"] = objects[index].parent;
            listed.push_back(std::move(object));
        }
        return {{"scene_revision", revision}, {"scene_id", "main"}, {"objects", listed}};
    }

    // UX-13c: the engine's scene_outline -- parents by authoring id, and
    // not_modified, without the list, for the revision the caller holds.
    Json sceneOutline(const Json &params) const {
        if (params.contains("if_scene_revision") && params.at("if_scene_revision") == revision)
            return {{"scene_id", "main"}, {"scene_revision", revision}, {"not_modified", true}};
        std::map<std::string, std::uint64_t> ids;
        for (const auto &object : objects) ids[object.name] = object.id;
        Json listed = Json::array();
        for (std::size_t index = 0; index < objects.size(); ++index) {
            Json object{{"authoring_object_id", objects[index].id},
                        {"declaration_index", index},
                        {"name", objects[index].name},
                        {"parent_authoring_object_id", nullptr}};
            if (!objects[index].parent.empty())
                object["parent_authoring_object_id"] = ids.at(objects[index].parent);
            listed.push_back(std::move(object));
        }
        return {{"scene_id", "main"},
                {"scene_revision", revision},
                {"not_modified", false},
                {"objects", listed}};
    }

    std::optional<Json> answer(const std::string &method, const Json &params) {
        if (method == "open_editor_session")
            return Json{{"actor_id", 9}, {"display_name", "Pelican Studio Inspector"}};
        if (method == "scene_tree") return sceneTree();
        if (method == "scene_outline") return sceneOutline(params);
        if (method == "get_scene_revision")
            return Json{{"scene_revision", revision}, {"preview_epoch", 0}};
        if (method == "get_document_journal")
            return Json{{"dirty", false}, {"changes", Json::array()}};
        if (method == "list_assets") return Json{{"assets", Json::array()}};
        // HUB-1a: the Graph tab's smallest readable document. The window's
        // Graph page only asks the model for the node a press named, so the
        // rest of the engine's graph surface is irrelevant here.
        if (method == "get_graph_instances") {
            auto nodes = Json::array();
            if (graph_node_exists) {
                nodes.push_back({{"id", "blur"},
                                 {"op", "value.add"},
                                 {"parameters", Json::object()}});
            }
            // HUB-1a: a second node the page must not follow. It is what the
            // Graph tab has selected while the hub still names blur.
            nodes.push_back({{"id", "sharpen"},
                             {"op", "value.multiply"},
                             {"parameters", Json::object()}});
            Json definitions = Json::object();
            if (graph_body_scope) {
                definitions["gain_curve"] = {
                    {"nodes", Json::array({{{"id", "gain"},
                                            {"op", "value.add"},
                                            {"parameters", Json::object()}}})},
                    {"outputs", Json::object()}};
            }
            return Json{{"source_digest", {{"hex", graph_digest_}}},
                        {"instances", Json::array()},
                        {"nodes", std::move(nodes)},
                        {"definitions", std::move(definitions)},
                        {"layout", Json::object()}};
        }
        if (method == "get_graph_catalog")
            return Json{{"schema", "pelican.graph_catalog"}, {"version", 1},
                        {"modules", Json::array()}};
        if (method == "get_graph_templates")
            return Json{{"templates", Json::array()}};
        if (method == "get_components") {
            const auto id = params.at("authoring_object_id").get<std::uint64_t>();
            const Json transform{{"pos", Json::array({0.0, 0.0, 0.0})},
                                 {"rotation", Json::array({0.0, 0.0, 0.0, 1.0})},
                                 {"scale", Json::array({1.0, 1.0, 1.0})}};
            return Json{
                {"scene_revision", revision},
                {"authoring_object_id", id},
                {"declaration_index", indexOf(id)},
                {"name", objects[indexOf(id)].name},
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
                                 {"fields", Json::array({{{"name", "pos"}, {"type", "vec3"}},
                                                         {{"name", "rotation"}, {"type", "quat"}},
                                                         {{"name", "scale"}, {"type", "vec3"}}})}}}}})}};
        }
        if (method == "edit") {
            const std::string ticket = "edit:" + std::to_string(next_ticket_++);
            edits_[ticket] = params.at("operations");
            return Json{{"status", "accepted"}, {"ticket", ticket}};
        }
        if (method == "edit_graph_document") {
            const std::string ticket = "graph:" + std::to_string(next_graph_ticket_++);
            graph_edits_[ticket] = params.at("ops");
            return Json{{"status", "accepted"}, {"ticket", ticket}};
        }
        if (method == "get_edit_result") {
            const std::string ticket = params.at("ticket").get<std::string>();
            if (const auto graph = graph_edits_.find(ticket); graph != graph_edits_.end()) {
                applyGraphOps(graph->second);
                return Json{{"status", "committed"},
                            {"committed_revision", ++revision},
                            {"source_digest", {{"hex", graph_digest_}}}};
            }
            Json created = Json::array();
            for (const auto &operation : edits_.at(ticket))
                for (auto &entry : apply(operation)) created.push_back(std::move(entry));
            return Json{{"status", "committed"},
                        {"committed_revision", ++revision},
                        {"created_objects", created}};
        }
        return std::nullopt;
    }
};

struct ProjectSandbox {
    std::filesystem::path root;

    ProjectSandbox() {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        root = std::filesystem::temp_directory_path() / ("pelican_outliner_edit_" + suffix);
        std::filesystem::create_directories(root / "scenes");
        std::filesystem::create_directories(root / "config");
        write("project.json", R"json({
  "schema":"pelican.project","version":1,"name":"ol1",
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

struct Rig {
    QApplication &app = application();
    ProjectSandbox sandbox;
    MainWindow window{QString::fromStdWString((sandbox.root / "config").wstring())};
    EmbeddedViewport &viewport = *window.viewport_;
    FakeEngine engine{viewport};
    OutlinerTree &outliner = *window.outliner_;
    // UX-13c: from the engine becoming available to the Outliner showing the
    // last object.
    qint64 opened_ms = 0;

    // Camera, Cube, and Floor with Leg under it.
    explicit Rig(std::vector<FakeEngine::Object> objects = {{1, "Camera", ""},
                                                            {2, "Cube", ""},
                                                            {3, "Floor", ""},
                                                            {4, "Leg", "Floor"}},
                 bool knows_scene_outline = true) {
        engine.objects = std::move(objects);
        engine.knows_scene_outline = knows_scene_outline;
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
        QElapsedTimer clock;
        clock.start();
        emit viewport.engineRpcBecameAvailable();
        const QString last = QString::fromStdString(engine.objects.back().name);
        REQUIRE(pumpUntil([&] { return row(last) != nullptr; }));
        opened_ms = clock.elapsed();
    }

    // How many requests of `method` were sent, with exactly `params` when
    // given.
    std::size_t asked(const std::string &method, const std::optional<Json> &params = {}) const {
        return static_cast<std::size_t>(std::count_if(
            engine.asked.begin(), engine.asked.end(), [&](const auto &request) {
                return request.first == method && (!params || request.second == *params);
            }));
    }

    void pumpFor(int milliseconds) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < milliseconds) {
            engine.pump();
            QTest::qWait(5);
        }
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

    QTreeWidgetItem *row(const QString &text) {
        for (QTreeWidgetItemIterator item{&outliner}; *item != nullptr; ++item)
            if ((*item)->text(0) == text) return *item;
        return nullptr;
    }

    std::uint64_t selectedId() const {
        const auto &selected = window.selection_model_.selected();
        return selected ? selected->authoring_object_id : 0;
    }

    // A person's click on a row: the tree selects it, and the window hears.
    void select(const QString &text) {
        QTreeWidgetItem *item = row(text);
        REQUIRE(item != nullptr);
        outliner.scrollToItem(item);
        QTest::mouseClick(outliner.viewport(), Qt::LeftButton, {},
                          outliner.visualItemRect(item).center());
        REQUIRE(outliner.currentItem() == item);
        REQUIRE(pumpUntil([&] {
            return window.selection_model_.selected() &&
                   window.project_model_->findObject(*window.selection_model_.selected())
                           ->display_name == text.toStdString();
        }));
        // Let the Inspector read the object, so the session is idle again.
        pumpFor(150);
    }

    // HUB-1a: the engine's graph document is read by the window's own model;
    // this names a node exactly as a Graph press would, then waits for the
    // coalesced Inspector refresh.
    void loadGraphDocument() {
        window.graph_canvas_->model().refresh();
        pumpFor(100);
    }

    GraphNodeRef graphNode(const std::string &id = "blur") const {
        return GraphNodeRef{.scope = "", .id = id};
    }

    void showGraphNode(const std::string &id = "blur") {
        loadGraphDocument();
        const GraphNodeRef ref = graphNode(id);
        REQUIRE(window.graph_canvas_->nodeView(ref).found);
        window.selection_hub_.pickedGraphNode(ref);
        REQUIRE(pumpUntil([&] { return window.inspector_->showsGraphNode(); }));
    }

    // The tree's own drag, as the automation channel sends it: a press, moves
    // with the button held, a release.
    void mouse(QEvent::Type type, const QPoint &position, Qt::MouseButtons held,
               Qt::KeyboardModifiers modifiers) {
        QMouseEvent event(type, QPointF{position},
                          outliner.viewport()->mapToGlobal(QPointF{position}),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held,
                          modifiers, QPointingDevice::primaryPointingDevice());
        QApplication::sendEvent(outliner.viewport(), &event);
    }

    void pressAndMove(const QString &from, const QPoint &to,
                      Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTreeWidgetItem *item = row(from);
        REQUIRE(item != nullptr);
        const QPoint start = outliner.visualItemRect(item).center();
        mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, modifiers);
        for (int step = 1; step <= 8; ++step)
            mouse(QEvent::MouseMove, start + (to - start) * step / 8, Qt::LeftButton, modifiers);
    }

    void release(const QPoint &at, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        mouse(QEvent::MouseButtonRelease, at, Qt::NoButton, modifiers);
    }

    void drag(const QString &from, const QPoint &to,
              Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        pressAndMove(from, to, modifiers);
        release(to, modifiers);
    }

    QPoint middleOf(const QString &text) { return outliner.visualItemRect(row(text)).center(); }
    // The line between this row and the one above it.
    QPoint topEdgeOf(const QString &text) {
        const QRect rect = outliner.visualItemRect(row(text));
        return {rect.center().x(), rect.top() + 1};
    }

    QMessageBox *deleteQuestion() const {
        return window.findChild<QMessageBox *>(QStringLiteral("pelican.outliner.deleteConfirm"));
    }

    QString notice() const {
        return window.findChild<QLabel *>(QStringLiteral("pelican.outlinerNotice"))->toolTip();
    }
};

} // namespace

TEST_CASE("OL-1 Duplicate sends one duplicate, selects the copy, and keeps open rows open",
          "[devstudio][outliner][ol1][duplicate][negative-contrast]") {
    Rig rig;
    rig.row("Floor")->setExpanded(true);
    rig.select("Cube");
    REQUIRE(rig.selectedId() == 2);

    // Ctrl+D, with the tree holding the keyboard.
    rig.outliner.setFocus();
    QTest::keyClick(&rig.outliner, Qt::Key_D, Qt::ControlModifier);
    REQUIRE(rig.pumpUntil([&] { return rig.selectedId() == 100; }));
    REQUIRE(rig.engine.operations.size() == 1);
    CHECK(rig.engine.operations.front() == Json{{"op", "duplicate"}, {"object_id", 2}});

    // The copy, not the original, and not whatever stands at an index: the
    // copy is declared right after Cube, so Floor and Leg moved down one.
    CHECK(rig.engine.names() ==
          std::vector<std::string>{"Camera", "Cube", "Cube_copy1", "Floor", "Leg"});
    REQUIRE(rig.outliner.currentItem() != nullptr);
    CHECK(rig.outliner.currentItem()->text(0) == QStringLiteral("Cube_copy1"));
    CHECK(rig.window.selection_model_.selected()->declaration_index == 2);
    // Floor was open at index 2 and is open at index 3.
    CHECK(rig.row("Floor")->isExpanded());

    // Selecting Floor, then duplicating Cube's copy: Floor's copy index moves
    // again, and the selection follows the new copy by its id.
    rig.select("Floor");
    rig.outliner.duplicateAction()->trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.selectedId() == 101 || rig.selectedId() == 102; }));
    CHECK(rig.outliner.currentItem()->text(0) == QStringLiteral("Floor_copy1"));
    // A duplicate copies the subtree; the root copy is the one selected.
    CHECK(rig.engine.names() == std::vector<std::string>{"Camera", "Cube", "Cube_copy1", "Floor",
                                                         "Leg", "Floor_copy1", "Leg_copy1"});
    CHECK(rig.selectedId() == 101);
}

TEST_CASE("OL-1 Delete asks once when children go with it, and never otherwise",
          "[devstudio][outliner][ol1][delete]") {
    Rig rig;

    // Floor takes Leg with it: one question, and nothing sent until it is
    // answered.
    rig.select("Floor");
    rig.outliner.setFocus();
    QTest::keyClick(&rig.outliner, Qt::Key_Delete);
    QPointer<QMessageBox> question = rig.deleteQuestion();
    REQUIRE(question);
    CHECK(question->isVisible());
    CHECK(question->text().contains(QStringLiteral("Floor")));
    CHECK(question->text().contains(QStringLiteral("1 object under it")));
    rig.pumpFor(100);
    CHECK(rig.engine.operations.empty());
    question->findChild<QPushButton *>(QStringLiteral("pelican.outliner.deleteConfirm.cancel"))
        ->click();
    rig.pumpFor(100);
    CHECK(rig.engine.operations.empty());
    CHECK(rig.engine.names().size() == 4);

    // Cube has nothing under it: no question, one destroy, and the sibling
    // declared before it is selected.
    rig.select("Cube");
    rig.outliner.deleteAction()->trigger();
    const bool asked = rig.deleteQuestion() != nullptr && rig.deleteQuestion()->isVisible();
    CHECK_FALSE(asked);
    REQUIRE(rig.pumpUntil([&] { return rig.row("Cube") == nullptr; }));
    REQUIRE(rig.engine.operations.size() == 1);
    CHECK(rig.engine.operations.front() == Json{{"op", "destroy"}, {"object_id", 2}});
    REQUIRE(rig.pumpUntil([&] { return rig.selectedId() == 1; }));
    CHECK(rig.outliner.currentItem()->text(0) == QStringLiteral("Camera"));

    // Answered Delete, Floor and Leg go in one destroy of Floor.
    rig.select("Floor");
    rig.outliner.deleteAction()->trigger();
    question = rig.deleteQuestion();
    REQUIRE(question);
    question->findChild<QPushButton *>(QStringLiteral("pelican.outliner.deleteConfirm.delete"))
        ->click();
    REQUIRE(rig.pumpUntil([&] { return rig.row("Leg") == nullptr; }));
    REQUIRE(rig.engine.operations.size() == 2);
    CHECK(rig.engine.operations.back() == Json{{"op", "destroy"}, {"object_id", 3}});
    CHECK(rig.engine.names() == std::vector<std::string>{"Camera"});
}

TEST_CASE("OL-1 a row dragged onto a row goes under it, between rows to the root, "
          "and with Alt keeps its own transform",
          "[devstudio][outliner][ol1][drag]") {
    Rig rig;

    // Onto Floor's row: under Floor, kept where it is on screen.
    rig.drag("Cube", rig.middleOf("Floor"));
    REQUIRE(rig.pumpUntil([&] { return rig.engine.operations.size() == 1; }));
    CHECK(rig.engine.operations.back() ==
          Json{{"op", "reparent"}, {"object_id", 2}, {"new_parent_id", 3}, {"preserve", "world"}});
    REQUIRE(rig.pumpUntil([&] { return rig.row("Cube") && rig.row("Cube")->parent() == rig.row("Floor"); }));
    // The object dragged stays the one selected, under its new parent.
    CHECK(rig.selectedId() == 2);

    // On the line between two rows: to the root.
    rig.drag("Cube", rig.topEdgeOf("Camera"));
    REQUIRE(rig.pumpUntil([&] { return rig.engine.operations.size() == 2; }));
    CHECK(rig.engine.operations.back() ==
          Json{{"op", "reparent"}, {"object_id", 2}, {"new_parent_id", nullptr},
               {"preserve", "world"}});
    REQUIRE(rig.pumpUntil([&] { return rig.row("Cube") && rig.row("Cube")->parent() != rig.row("Floor"); }));

    // Alt held: under Camera, keeping its own transform.
    rig.drag("Cube", rig.middleOf("Camera"), Qt::AltModifier);
    REQUIRE(rig.pumpUntil([&] { return rig.engine.operations.size() == 3; }));
    CHECK(rig.engine.operations.back() ==
          Json{{"op", "reparent"}, {"object_id", 2}, {"new_parent_id", 1}, {"preserve", "local"}});
}

TEST_CASE("OL-1 a row cannot be dropped under its own child",
          "[devstudio][outliner][ol1][drag][negative-contrast]") {
    Rig rig;
    rig.row("Floor")->setExpanded(true);
    rig.pumpFor(50);

    // Over Leg, which is under Floor: the drag says no before it is let go.
    rig.pressAndMove("Floor", rig.middleOf("Leg"));
    CHECK(rig.outliner.dragging());
    CHECK(rig.outliner.hoveredDropTarget().place == OutlinerTree::DropPlace::Refused);
    CHECK(rig.outliner.viewport()->cursor().shape() == Qt::ForbiddenCursor);
    rig.release(rig.middleOf("Leg"));
    rig.pumpFor(200);
    CHECK(rig.engine.operations.empty());
    CHECK(rig.notice() == QStringLiteral("Floor cannot go under Leg: Leg is inside Floor."));
    CHECK(rig.outliner.viewport()->cursor().shape() != Qt::ForbiddenCursor);

    // Over a row that can take it, the same drag is an ordinary one.
    rig.pressAndMove("Floor", rig.middleOf("Cube"));
    CHECK(rig.outliner.hoveredDropTarget().place == OutlinerTree::DropPlace::Onto);
    rig.release(rig.middleOf("Cube"));
    REQUIRE(rig.pumpUntil([&] { return rig.engine.operations.size() == 1; }));
    CHECK(rig.engine.operations.back().at("new_parent_id") == 2);
}

TEST_CASE("UX-13c the Outliner polls scene_outline with the revision it holds",
          "[devstudio][outliner][ux13c][negative-contrast]") {
    // A thousand objects: a root every tenth, the nine after it its children.
    std::vector<FakeEngine::Object> objects;
    for (std::uint64_t index = 0; index < 1000; ++index)
        objects.push_back({100 + index, "Object" + std::to_string(index),
                           index % 10 == 0 ? std::string{}
                                           : "Object" + std::to_string(index - index % 10)});
    Rig rig{objects};
    // The first read names no scene: Studio does not know it yet.
    CHECK(rig.asked("scene_outline", Json::object()) == 1);
    CHECK(rig.asked("scene_tree") == 0);
    QTreeWidgetItem *shown = rig.row("Object999");
    REQUIRE(shown != nullptr);
    CHECK(shown->parent() == rig.row("Object990"));

    // While nothing changes, every poll names the scene and the revision
    // held, and its not_modified answer rebuilds no row.
    const Json held{{"scene_id", "main"}, {"if_scene_revision", 1}};
    REQUIRE(rig.pumpUntil([&] { return rig.asked("scene_outline", held) >= 2; }, 4000));
    CHECK(rig.row("Object999") == shown);
    CHECK(rig.asked("scene_tree") == 0);

    // A change elsewhere moves the revision: the next poll is answered with
    // the whole list, and the Outliner shows it.
    rig.engine.objects[1].name = "Renamed";
    ++rig.engine.revision;
    REQUIRE(rig.pumpUntil([&] { return rig.row("Renamed") != nullptr; }, 4000));
    const Json moved{{"scene_id", "main"}, {"if_scene_revision", 2}};
    REQUIRE(rig.pumpUntil([&] { return rig.asked("scene_outline", moved) >= 1; }, 4000));

    // Decision 6: the numbers for the report, not a pass condition.
    REQUIRE(rig.engine.list_replies.size() >= 3);
    const auto full = rig.engine.list_replies.front().second;
    const auto unchanged = rig.engine.list_replies[1].second;
    std::cout << "UX-13c Outliner, 1000 objects: shown in " << rig.opened_ms
              << " ms; scene_outline " << full << " bytes, not_modified poll " << unchanged
              << " bytes, scene_tree (this fake, components empty) "
              << rig.engine.sceneTreeBytes() << " bytes\n";
    CHECK(unchanged < 100);
}

TEST_CASE("UX-13c an engine without scene_outline is read with scene_tree, asked once",
          "[devstudio][outliner][inspector][ux13c]") {
    Rig rig{{{1, "Camera", ""}, {2, "Cube", ""}, {3, "Floor", ""}, {4, "Leg", "Floor"}}, false};
    // The Outliner asked once, was refused, and read the tree with its named
    // parents.
    CHECK(rig.row("Leg")->parent() == rig.row("Floor"));
    // The Inspector, asked about Cube, does the same and reads its components.
    rig.select("Cube");
    REQUIRE(rig.pumpUntil([&] {
        return rig.asked("get_components", Json{{"scene_id", "main"}, {"authoring_object_id", 2}}) >= 1;
    }));
    rig.pumpFor(1200);
    CHECK(rig.asked("scene_tree") >= 3);
    // Each asked once and remembered; nothing more went out as scene_outline.
    CHECK(rig.asked("scene_outline") == 2);
    CHECK(rig.asked("scene_outline", Json::object()) == 1);
    CHECK(rig.asked("scene_outline", Json{{"scene_id", "main"}}) == 1);
}


TEST_CASE("HUB-1a a real Outliner rebuild leaves the Graph front standing",
          "[devstudio][outliner][hub1a]") {
    Rig rig;
    rig.select("Cube");
    rig.showGraphNode();
    REQUIRE(rig.window.selection_hub_.front() == SelectionFront::graph_node);
    REQUIRE(rig.window.inspector_->showsGraphNode());

    // Trigger the window's own populateOutliner() through its timer, not a
    // test double: the rebuild replaces every row and restores the current
    // one, and none of that is a user pick. Data members are the only thing
    // this test reaches into; the refresh itself stays a signal away.
    QTreeWidgetItem *before = rig.row("Cube");
    REQUIRE(before != nullptr);
    // The revision moves because a new object is declared: a poll that only
    // moved the number would leave the same outline, and the window would
    // rightly keep the rows it has.
    rig.engine.objects.push_back({200, "Added", ""});
    ++rig.engine.revision;
    rig.window.outliner_refresh_timer_->setInterval(0);
    rig.window.outliner_refresh_timer_->start();
    REQUIRE(rig.pumpUntil([&] { return rig.row("Added") != nullptr; }));
    CHECK(rig.row("Cube") != before);
    rig.window.outliner_refresh_timer_->setInterval(250);
    rig.pumpFor(100);
    CHECK(rig.window.selection_hub_.front() == SelectionFront::graph_node);
    CHECK(rig.window.inspector_->showsGraphNode());
    CHECK(rig.window.selection_hub_.intent() >= 1);
}

TEST_CASE("HUB-1a a real mouse press on the current row is a scene entrance",
          "[devstudio][outliner][hub1a]") {
    Rig rig;
    rig.select("Cube");
    rig.showGraphNode();
    QTreeWidgetItem *cube = rig.row("Cube");
    REQUIRE(cube != nullptr);
    REQUIRE(rig.outliner.currentItem() == cube);

    // No currentItemChanged for a row that is already current: itemPressed is
    // what makes this press count.
    QTest::mouseClick(rig.outliner.viewport(), Qt::LeftButton, {},
                      rig.outliner.visualItemRect(cube).center());
    REQUIRE(rig.pumpUntil([&] {
        return rig.window.selection_hub_.front() == SelectionFront::scene;
    }));
    CHECK_FALSE(rig.window.inspector_->showsGraphNode());
}

TEST_CASE("HUB-1a a keyboard move in the Outliner is a scene entrance",
          "[devstudio][outliner][hub1a]") {
    Rig rig;
    rig.select("Cube");
    rig.showGraphNode();
    rig.outliner.setFocus();
    QTest::keyClick(&rig.outliner, Qt::Key_Down);
    REQUIRE(rig.pumpUntil([&] {
        return rig.window.selection_hub_.front() == SelectionFront::scene;
    }));
    CHECK_FALSE(rig.window.inspector_->showsGraphNode());
}

TEST_CASE("HUB-1a Back to the scene is the Inspector's own entrance",
          "[devstudio][outliner][inspector][hub1a]") {
    Rig rig;
    rig.select("Cube");
    rig.showGraphNode();
    auto *back = rig.window.inspector_->findChild<QPushButton *>(
        QStringLiteral("pelican.inspectorGraph.backToScene"));
    REQUIRE(back != nullptr);
    CHECK(back->text() == QStringLiteral("Back to Cube"));
    back->click();
    REQUIRE(rig.pumpUntil([&] {
        return rig.window.selection_hub_.front() == SelectionFront::scene;
    }));
    CHECK_FALSE(rig.window.inspector_->showsGraphNode());
}

TEST_CASE("HUB-1a a node that goes away keeps the Graph page until it returns",
          "[devstudio][outliner][graph][hub1a]") {
    Rig rig;
    rig.select("Cube");
    rig.showGraphNode();
    const auto missing = [&]() -> QLabel * {
        return rig.window.inspector_->findChild<QLabel *>(
            QStringLiteral("pelican.inspectorGraph.noLongerInDocument"));
    };
    CHECK(missing() == nullptr);

    // The Graph's own delete, through the same model call its Delete action
    // makes. The front is not the Graph's to move: the page stays and says
    // the node it follows is gone.
    auto &model = rig.window.graph_canvas_->model();
    REQUIRE(rig.pumpUntil([&] { return model.canEdit(); }, 1000));
    REQUIRE(model.removeNode("blur"));
    REQUIRE(rig.pumpUntil([&] { return missing() != nullptr && missing()->isVisible(); }));
    CHECK(rig.window.selection_hub_.front() == SelectionFront::graph_node);
    CHECK(rig.window.inspector_->showsGraphNode());
    CHECK_FALSE(rig.window.graph_canvas_->nodeView(rig.graphNode()).found);

    // And its Undo restores the node under the same graph page.
    REQUIRE(rig.pumpUntil([&] { return model.canUndo(); }));
    REQUIRE(model.undo());
    REQUIRE(rig.pumpUntil([&] { return missing() == nullptr; }));
    CHECK(rig.window.selection_hub_.front() == SelectionFront::graph_node);
    CHECK(rig.window.inspector_->showsGraphNode());
    CHECK(rig.window.graph_canvas_->nodeView(rig.graphNode()).found);
}

TEST_CASE("HUB-1a an Inspector tab that is behind builds nothing until it is shown",
          "[devstudio][inspector][hub1a]") {
    Rig rig;
    rig.select("Cube");
    rig.loadGraphDocument();
    QDockWidget *inspector_dock = rig.window.docks_[MainWindow::InspectorDock];
    REQUIRE(inspector_dock != nullptr);
    inspector_dock->hide();
    QApplication::processEvents();

    rig.window.selection_hub_.pickedGraphNode(rig.graphNode());
    rig.pumpFor(100);
    CHECK_FALSE(rig.window.inspector_->showsGraphNode());

    inspector_dock->show();
    inspector_dock->raise();
    REQUIRE(rig.pumpUntil([&] {
        return rig.window.inspector_->showsGraphNode();
    }));
    CHECK(rig.window.inspector_->findChild<QLabel *>(
              QStringLiteral("pelican.inspectorGraph.op"))
              ->text()
              .contains(QStringLiteral("value.add")));
}

TEST_CASE("HUB-1a a runtime view does not survive a Graph round trip",
          "[devstudio][inspector][hub1a]") {
    Rig rig;
    RuntimeObject runtime;
    runtime.name = "Spawned";
    rig.window.inspector_->showRuntimeObject(runtime);
    CHECK(rig.window.inspector_->showsRuntimeObject());

    rig.showGraphNode();
    CHECK(rig.window.selection_hub_.front() == SelectionFront::graph_node);
    CHECK(rig.window.inspector_->showsGraphNode());
    CHECK(rig.window.inspector_->showsRuntimeObject());

    auto *back = rig.window.inspector_->findChild<QPushButton *>(
        QStringLiteral("pelican.inspectorGraph.backToScene"));
    REQUIRE(back != nullptr);
    back->click();
    REQUIRE(rig.pumpUntil([&] {
        return rig.window.selection_hub_.front() == SelectionFront::scene;
    }));
    CHECK_FALSE(rig.window.inspector_->showsGraphNode());
    CHECK_FALSE(rig.window.inspector_->showsRuntimeObject());
}


TEST_CASE("HUB-1a a reveal from the Graph is not a user pick",
          "[devstudio][outliner][graph][hub1a]") {
    Rig rig;
    rig.select("Cube");
    rig.loadGraphDocument();
    REQUIRE(rig.window.graph_canvas_->nodeView(rig.graphNode()).found);

    // This is the Graph's own reveal entry point, the same one another panel's
    // Open in Graph calls. It changes the Graph's selection and builds its
    // editors, but nobody picked a node there.
    REQUIRE(rig.window.graph_canvas_->revealGraphNode(
        QStringLiteral(""), QStringLiteral("blur"), QStringLiteral("")));
    rig.pumpFor(150);
    CHECK(rig.window.selection_hub_.front() == SelectionFront::scene);
    CHECK_FALSE(rig.window.inspector_->showsGraphNode());
}

TEST_CASE("HUB-1a rapid Graph picks build the page once in an event-loop turn",
          "[devstudio][outliner][inspector][hub1a]") {
    Rig rig;
    rig.select("Cube");
    rig.loadGraphDocument();
    QSignalSpy fired(rig.window.inspector_refresh_timer_, &QTimer::timeout);
    CHECK_FALSE(rig.window.inspector_->showsGraphNode());

    for (int index = 0; index < 5; ++index)
        rig.window.selection_hub_.pickedGraphNode(rig.graphNode());
    CHECK(fired.count() == 0);
    // Being picked five times builds nothing by itself: the page waits for
    // the event loop, so a press never pays for it.
    CHECK_FALSE(rig.window.inspector_->showsGraphNode());

    REQUIRE(rig.pumpUntil([&] { return rig.window.inspector_->showsGraphNode(); }));
    CHECK(fired.count() == 1);
    CHECK(rig.window.inspector_->findChild<QLabel *>(
              QStringLiteral("pelican.inspectorGraph.id"))
              ->text() == QStringLiteral("blur"));
}

TEST_CASE("HUB-1a a real Graph press moves the front but keeps the scene selection",
          "[devstudio][outliner][graph][hub1a]") {
    Rig rig;
    rig.select("Cube");
    REQUIRE(rig.window.selection_model_.selected());
    const OutlinerObjectKey cube = *rig.window.selection_model_.selected();
    rig.loadGraphDocument();

    auto *canvas = rig.window.graph_canvas_;
    // The tab follows the document by default through its Render domain;
    // this fake's two nodes are value nodes. Showing Value is a user's own
    // filter, not a selection.
    CHECK(canvas->model().setDomainShown("value", true));
    canvas->refresh();
    QApplication::processEvents();

    QGraphicsItem *blur = nullptr;
    for (QGraphicsItem *item : canvas->scene().items()) {
        if (item->data(NodeGraphItemKindRole).toString() ==
                QLatin1String{GraphCanvasNodeItem} &&
            item->data(NodeGraphNameRole).toString() == QStringLiteral("blur")) {
            blur = item;
            break;
        }
    }
    REQUIRE(blur != nullptr);

    // The scene's own press path: this is what the canvas dispatches when a
    // hand lands on a node, and where the owner's observer is called.
    const auto press = [&](const QPointF &position) {
        QGraphicsSceneMouseEvent event{QEvent::GraphicsSceneMousePress};
        event.setScenePos(position);
        event.setButton(Qt::LeftButton);
        event.setButtons(Qt::LeftButton);
        QApplication::sendEvent(&canvas->scene(), &event);
        QGraphicsSceneMouseEvent release{QEvent::GraphicsSceneMouseRelease};
        release.setScenePos(position);
        release.setButton(Qt::LeftButton);
        QApplication::sendEvent(&canvas->scene(), &release);
        QApplication::processEvents();
    };
    press(blur->sceneBoundingRect().center());
    REQUIRE(rig.pumpUntil([&] {
        return rig.window.selection_hub_.front() == SelectionFront::graph_node;
    }));

    // The Graph press is the Inspector's business, not the scene's: the
    // Outliner row and the scene selection stay on Cube.
    CHECK(rig.window.selection_model_.selected() == cube);
    CHECK(rig.outliner.currentItem() == rig.row("Cube"));

    // An empty part of the canvas is the scene entrance, and still not a
    // reason to change the scene selection.
    press(canvas->scene().sceneRect().topLeft() + QPointF(4.0, 4.0));
    REQUIRE(rig.pumpUntil([&] {
        return rig.window.selection_hub_.front() == SelectionFront::scene;
    }));
    CHECK(rig.window.selection_model_.selected() == cube);
    CHECK(rig.outliner.currentItem() == rig.row("Cube"));
}

TEST_CASE("HUB-1a a viewport pick after a later Graph intent keeps the Graph page",
          "[devstudio][outliner][viewport][hub1a]") {
    Rig rig;
    rig.select("Cube");
    REQUIRE(rig.window.selection_hub_.front() == SelectionFront::scene);

    // The pick the transport would be waiting on. The window records the
    // intent it began under; here that is the scene front it starts from.
    const auto token = rig.window.selection_model_.beginViewportPick();
    const std::uint64_t pick_intent = rig.window.selection_hub_.intent();
    constexpr qint64 request_id = 4242;
    rig.window.pending_pick_tokens_.insert(request_id, token);
    rig.window.pending_pick_intents_.insert(request_id, pick_intent);

    // Before it lands, the user presses a Graph node. That is the later
    // intent, and the page follows it.
    rig.loadGraphDocument();
    rig.window.selection_hub_.pickedGraphNode(rig.graphNode());
    REQUIRE(rig.pumpUntil([&] { return rig.window.inspector_->showsGraphNode(); }));

    // The scene result still lands -- on Camera -- but it does not move the
    // Inspector's front back to the scene.
    const QByteArray result = QByteArrayLiteral(
        R"json({"contract":1,"hit":{"scene_id":"main","declaration_index":0}})json");
    emit rig.viewport.pickObjectSucceeded(request_id, result);
    REQUIRE(rig.pumpUntil([&] {
        return rig.window.selection_model_.selected() &&
               rig.window.selection_model_.selected()->declaration_index == 0;
    }));
    CHECK(rig.window.selection_hub_.front() == SelectionFront::graph_node);
    REQUIRE(rig.pumpUntil([&] {
        auto *id = rig.window.inspector_->findChild<QLabel *>(
            QStringLiteral("pelican.inspectorGraph.id"));
        return id != nullptr && id->isVisible();
    }));
    CHECK(rig.window.inspector_->findChild<QLabel *>(
              QStringLiteral("pelican.inspectorGraph.id"))
              ->text() == QStringLiteral("blur"));
}

TEST_CASE("HUB-1a a viewport pick with no later intent lands on the scene even unchanged",
          "[devstudio][outliner][viewport][hub1a]") {
    Rig rig;
    rig.select("Cube");
    rig.showGraphNode();
    REQUIRE(rig.window.selection_hub_.front() == SelectionFront::graph_node);

    // Cube is already selected: the scene answer changes nothing, but it is
    // still an answer to the user's viewport click, with no later intent.
    const auto token = rig.window.selection_model_.beginViewportPick();
    const std::uint64_t pick_intent = rig.window.selection_hub_.intent();
    constexpr qint64 request_id = 4243;
    rig.window.pending_pick_tokens_.insert(request_id, token);
    rig.window.pending_pick_intents_.insert(request_id, pick_intent);

    const QByteArray result = QByteArrayLiteral(
        R"json({"contract":1,"hit":{"scene_id":"main","declaration_index":1}})json");
    emit rig.viewport.pickObjectSucceeded(request_id, result);
    REQUIRE(rig.pumpUntil([&] {
        return rig.window.selection_hub_.front() == SelectionFront::scene;
    }));
    CHECK_FALSE(rig.window.inspector_->showsGraphNode());
    REQUIRE(rig.window.selection_model_.selected());
    CHECK(rig.window.selection_model_.selected()->declaration_index == 1);
    CHECK(rig.window.pending_pick_intents_.isEmpty());
}

TEST_CASE("HUB-1a the page follows the hub's node, not the Graph's current selection",
          "[devstudio][graph][inspector][hub1a]") {
    Rig rig;
    rig.select("Cube");
    rig.loadGraphDocument();
    const GraphNodeRef blur = rig.graphNode("blur");
    REQUIRE(rig.window.graph_canvas_->nodeView(blur).found);

    // The hub names blur; the Graph tab is looking at sharpen. No press is
    // involved in either, so the page must be built from the hub's ref.
    rig.window.selection_hub_.pickedGraphNode(blur);
    rig.window.graph_canvas_->scene().selectNode("", "sharpen");
    REQUIRE(rig.pumpUntil([&] { return rig.window.inspector_->showsGraphNode(); }));
    CHECK(rig.window.inspector_->findChild<QLabel *>(
              QStringLiteral("pelican.inspectorGraph.id"))
              ->text() == QStringLiteral("blur"));
    // The rest of the page is the hub node's too: blur adds, sharpen
    // multiplies. A page built from the tab's selection would say multiply.
    CHECK(rig.window.inspector_->findChild<QLabel *>(
              QStringLiteral("pelican.inspectorGraph.op"))
              ->text().contains(QStringLiteral("value.add")));
}

TEST_CASE("HUB-1a a Graph scope change leaves the hub's page standing",
          "[devstudio][graph][inspector][hub1a]") {
    Rig rig;
    rig.select("Cube");
    // The document has a body the canvas can enter; entering it is a change
    // of the Graph's own view, not a pick.
    rig.engine.graph_body_scope = true;
    rig.showGraphNode();
    REQUIRE(rig.window.selection_hub_.front() == SelectionFront::graph_node);

    auto &model = rig.window.graph_canvas_->model();
    // The first graph read raced this setup at window construction. Read the
    // document again now that the body exists, and wait for the canvas to
    // hold it: entering it is a change of the Graph's own view only.
    rig.window.graph_canvas_->refresh();
    REQUIRE(rig.pumpUntil([&] {
        const auto scopes = model.scopes();
        return std::ranges::any_of(scopes, [](const auto &scope) {
            return scope.id == "gain_curve";
        });
    }));
    REQUIRE(model.selectScope("gain_curve"));
    rig.pumpFor(100);
    CHECK(model.currentScope() == "gain_curve");
    CHECK(rig.window.selection_hub_.front() == SelectionFront::graph_node);
    CHECK(rig.window.inspector_->showsGraphNode());
    CHECK(rig.window.inspector_->findChild<QLabel *>(
              QStringLiteral("pelican.inspectorGraph.id"))
              ->text() == QStringLiteral("blur"));
}

TEST_CASE("HUB-1a a project switch drops the Graph page and an in-flight pick",
          "[devstudio][outliner][graph][hub1a]") {
    Rig rig;
    rig.select("Cube");
    rig.showGraphNode();
    REQUIRE(rig.window.selection_hub_.front() == SelectionFront::graph_node);
    // A viewport pick from the old project is in the air.
    rig.window.pending_pick_intents_.insert(73, rig.window.selection_hub_.intent());

    ProjectSandbox other;
    other.write("project.json", R"json({
  "schema":"pelican.project","version":1,"name":"other",
  "basic_config":{"scene_data_json":"scenes/main.scene.json"}
})json");
    rig.window.openProject(QString::fromStdWString(other.root.wstring()));
    const auto on_other_project = [&] {
        std::error_code error;
        return rig.window.project_model_ &&
               std::filesystem::equivalent(rig.window.project_model_->projectRoot(),
                                           other.root, error) &&
               !error;
    };
    REQUIRE(rig.pumpUntil(on_other_project, 10000));
    REQUIRE(rig.pumpUntil([&] {
        return rig.window.selection_hub_.front() == SelectionFront::scene;
    }));
    CHECK_FALSE(rig.window.inspector_->showsGraphNode());
    CHECK(rig.window.pending_pick_intents_.isEmpty());
}

} // namespace PelicanStudio
