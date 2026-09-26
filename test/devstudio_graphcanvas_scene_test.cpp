#include "graphcanvaswidget.hpp"
#include "graphmodule.hpp"
#include "graphpartition.hpp"
#include "studiotheme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QFocusEvent>
#include <QFont>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QImage>
#include <QPainter>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QKeyEvent>
#include <QRectF>
#include <QScrollBar>
#include <QTabWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QWidget>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <functional>
#include <vector>

namespace {
using Json = nlohmann::json;
using PelicanStudio::GraphCanvasModel;
using PelicanStudio::GraphCanvasScene;

QApplication &application() {
    static int argc = 1;
    static char name[] = "devstudio_graphcanvas_scene_test";
    static char *argv[] = {name, nullptr};
    static QApplication app{argc, argv};
    return app;
}

Json readFixture(const char *name) {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" /
                      "fixtures" / name / "passes" / "main.json";
    std::ifstream stream{path};
    REQUIRE(stream.good());
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return Json::parse(buffer.str());
}

Json fixtureDocument() { return readFixture("graph_instance_inputs"); }

// The shapes the engine really publishes for a contract.
Json declaredInput(const std::string &port,
                   const std::string &type = "pelican.value.scalar@1") {
    return {{"port", port}, {"required", true},
            {"accepted_type", {{"semantic", type},
                               {"required_traits", Json::array()},
                               {"predicates", Json::array()}}}};
}

Json declaredOutput(const std::string &port,
                    const std::string &type = "pelican.value.scalar@1") {
    return {{"port", port},
            {"type", {{"schema", "pelican.logical_type"}, {"version", 1},
                      {"constructor", "value"}, {"semantic", type},
                      {"arguments", Json::array()}}}};
}

Json declaredOperation(const std::string &op, const std::string &domain,
                       const Json &inputs, const Json &outputs,
                       bool sink = false) {
    return {{"op", op}, {"domain", domain}, {"evaluation", "direct"},
            {"sink", sink}, {"ports", "declared"},
            {"inputs", inputs}, {"outputs", outputs}};
}

Json typeSchema(const std::string &id, const Json &traits,
                const Json &display = Json{}) {
    Json schema{{"id", id}, {"constructor", "value"}, {"traits", traits},
                {"parameters", Json::array()}};
    if (!display.is_null()) schema["display"] = display;
    return schema;
}

// The catalog for the operations the visual fixtures use, plus the type
// schemas the sockets are resolved from. `pelican.render.surface@1` carries a
// display declaration; nothing else does.
Json visualCatalog() {
    const auto scalar_in = [](const char *port) { return declaredInput(port); };
    return {{"schema", "pelican.graph_catalog"}, {"version", 1},
        {"modules", Json::array({
            {{"namespace", "pelican.value"}, {"version", 1}, {"description", ""},
             {"sinks", Json::array()},
             {"types", Json::array({
                 typeSchema("pelican.value.scalar@1", Json::array({"numeric"}))})},
             {"operations", Json::array({
                 declaredOperation("value.input", "values", Json::array(),
                                   Json::array({declaredOutput("value")})),
                 declaredOperation("value.constant", "values", Json::array(),
                                   Json::array({declaredOutput("value")})),
                 declaredOperation("value.curve", "values",
                                   Json::array({scalar_in("time")}),
                                   Json::array({declaredOutput("value")})),
                 declaredOperation("value.add", "values",
                                   Json::array({scalar_in("a"), scalar_in("b")}),
                                   Json::array({declaredOutput("value")})),
                 declaredOperation("value.multiply", "values",
                                   Json::array({scalar_in("a"), scalar_in("b")}),
                                   Json::array({declaredOutput("value")}))})}},
            {{"namespace", "pelican.render"}, {"version", 1}, {"description", ""},
             {"sinks", Json::array()},
             {"types", Json::array({
                 typeSchema("pelican.render.storage_buffer@1",
                            Json::array({"shader_storage"})),
                 // A module that declares its own look for one of its types.
                 typeSchema("pelican.render.surface@1", Json::array({"opaque"}),
                            Json{{"shape", "hexagon"}, {"color", "#123456"}})})},
             {"operations", Json::array({
                 declaredOperation(
                     "render.buffer", "rendering",
                     Json::array({declaredInput("value")}),
                     Json::array({declaredOutput(
                         "resource", "pelican.render.storage_buffer@1")}),
                     true)})}},
            {{"namespace", "pelican.anim"}, {"version", 1}, {"description", ""},
             {"sinks", Json::array()},
             {"types", Json::array({
                 typeSchema("pelican.pose@1", Json::array({"opaque", "pose"})),
                 // Named nowhere in the shipped table and carrying only the
                 // opaque trait: the traits rule is the only thing left.
                 typeSchema("pelican.anim.handle@1", Json::array({"opaque"}))})},
             {"operations", Json::array()}}})}};
}

Json context(const Json &document) {
    auto nodes = document.at("nodes");
    auto instances = Json::array();
    for (const auto &node : nodes)
        if (node.at("op") == "graph.instance") instances.push_back(node);
    return {{"source_digest", {{"hex", "source-a"}}},
            {"instances", std::move(instances)},
            {"nodes", std::move(nodes)},
            {"definitions", document.at("definitions")},
            // N-6: the placement travels on the read that was already there.
            {"layout", document.value("layout", Json::object())}};
}

// The fixture with one authored edge, so the root scope is a graph rather than
// two loose nodes.
Json connectedDocument() {
    auto document = fixtureDocument();
    for (auto &node : document.at("nodes"))
        if (node.at("id") == "left")
            node["inputs"] = {{"gain", {{"node", "clock"}, {"port", "value"}}}};
    return document;
}

void answer(GraphCanvasModel &model, const Json &document,
            const Json &catalog = Json{{"schema", "pelican.graph_catalog"},
                                       {"version", 1},
                                       {"modules", Json::array()}}) {
    for (const auto &request : model.takeRpcRequests()) {
        if (request.method == "get_graph_instances")
            model.receiveRpcResult(request.request_id, context(document).dump());
        else if (request.method == "get_graph_catalog")
            model.receiveRpcResult(request.request_id, catalog.dump());
    }
}

QGraphicsItem *nodeItem(QGraphicsScene &scene, const QString &name) {
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasNodeItem} &&
            item->data(PelicanStudio::NodeGraphNameRole).toString() == name)
            return item;
    return nullptr;
}

// What the model says about one input port, so the scene assertions can name
// the state the drawing is supposed to answer to.
PelicanStudio::GraphCanvasPort port(const GraphCanvasModel &model,
                                    const std::string &node,
                                    const std::string &name) {
    const auto nodes = model.nodes();
    const auto found = std::ranges::find(nodes, node,
                                         &PelicanStudio::GraphCanvasNode::id);
    REQUIRE(found != nodes.end());
    const auto entry = std::ranges::find(found->inputs, name,
                                         &PelicanStudio::GraphCanvasPort::port);
    REQUIRE(entry != found->inputs.end());
    return *entry;
}

QGraphicsItem *socketItem(QGraphicsScene &scene, const QString &node,
                          const QString &port, const QString &direction) {
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasSocketItem} &&
            item->data(PelicanStudio::NodeGraphNameRole).toString() == node &&
            item->data(PelicanStudio::GraphCanvasPortRole).toString() == port &&
            item->data(PelicanStudio::GraphCanvasSocketDirectionRole)
                    .toString() == direction)
            return item;
    return nullptr;
}

QGraphicsItem *pillItem(QGraphicsScene &scene, const QString &node,
                        const QString &port) {
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasValuePillItem} &&
            item->data(PelicanStudio::NodeGraphNameRole).toString() == node &&
            item->data(PelicanStudio::GraphCanvasPortRole).toString() == port &&
            !item->data(PelicanStudio::GraphCanvasValueTextRole)
                 .toString()
                 .isEmpty())
            return item;
    return nullptr;
}

QGraphicsPathItem *edgeItem(QGraphicsScene &scene, const QString &from,
                            const QString &from_port, const QString &to,
                            const QString &to_port) {
    for (QGraphicsItem *item : scene.items()) {
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() !=
                QLatin1String{PelicanStudio::GraphCanvasEdgeItem} ||
            item->data(PelicanStudio::NodeGraphFromNameRole).toString() != from ||
            item->data(PelicanStudio::GraphCanvasFromPortRole).toString() !=
                from_port ||
            item->data(PelicanStudio::NodeGraphToNameRole).toString() != to ||
            item->data(PelicanStudio::GraphCanvasToPortRole).toString() != to_port)
            continue;
        return dynamic_cast<QGraphicsPathItem *>(item);
    }
    return nullptr;
}

// The heading band holds the two texts that name a node: the label in bold and
// the operation type beside it.
bool headingHas(QGraphicsScene &scene, const QString &node, const QString &text,
                bool bold) {
    for (QGraphicsItem *item : scene.items()) {
        auto *drawn = dynamic_cast<QGraphicsSimpleTextItem *>(item);
        if (drawn == nullptr ||
            drawn->data(PelicanStudio::NodeGraphNameRole).toString() != node ||
            drawn->text() != text)
            continue;
        if (drawn->font().bold() == bold) return true;
    }
    return false;
}

std::vector<QGraphicsItem *> itemsOfKind(QGraphicsScene &scene, const char *kind) {
    std::vector<QGraphicsItem *> found;
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
            QLatin1String{kind})
            found.push_back(item);
    return found;
}

// The scene changes scope from the event loop, never from inside the press it
// is dispatching -- rebuilding there would delete the item Qt is still using.
void click(QGraphicsScene &scene, const QPointF &position,
           QEvent::Type type = QEvent::GraphicsSceneMousePress) {
    QGraphicsSceneMouseEvent event{type};
    event.setScenePos(position);
    event.setButton(Qt::LeftButton);
    event.setButtons(Qt::LeftButton);
    QApplication::sendEvent(&scene, &event);
    QCoreApplication::processEvents();
}

// Press on one point, move, release on another: the whole of a socket drag,
// which is the only gesture on this canvas that spans three events.
void drag(QGraphicsScene &scene, const QPointF &from, const QPointF &to) {
    QGraphicsSceneMouseEvent press{QEvent::GraphicsSceneMousePress};
    press.setScenePos(from);
    press.setButton(Qt::LeftButton);
    press.setButtons(Qt::LeftButton);
    QApplication::sendEvent(&scene, &press);
    QGraphicsSceneMouseEvent move{QEvent::GraphicsSceneMouseMove};
    move.setScenePos(to);
    move.setButtons(Qt::LeftButton);
    QApplication::sendEvent(&scene, &move);
    QGraphicsSceneMouseEvent release{QEvent::GraphicsSceneMouseRelease};
    release.setScenePos(to);
    release.setButton(Qt::LeftButton);
    QApplication::sendEvent(&scene, &release);
    QCoreApplication::processEvents();
}

void pressKey(QGraphicsScene &scene, int key,
              Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event{QEvent::KeyPress, key, modifiers};
    QApplication::sendEvent(&scene, &event);
    QCoreApplication::processEvents();
}

// The one edit_graph_document a gesture produced, accepted so the model moves
// on to polling for its ticket. Reads may be travelling beside it -- AG-3b's
// observation pair goes out whenever the scope or the watched instance moves
// -- and the question here is how many *edits* a gesture is, which is one.
Json takeEdit(GraphCanvasModel &model) {
    auto requests = model.takeRpcRequests();
    std::vector<PelicanStudio::InspectorRpcRequest> edits;
    for (const auto &request : requests)
        if (request.method == "edit_graph_document") edits.push_back(request);
    REQUIRE(edits.size() == 1);
    model.receiveRpcResult(
        edits.front().request_id,
        Json{{"status", "accepted"}, {"ticket", "graph-edit-1"}}.dump());
    return edits.front().params;
}

// No gesture reached the document. Reads are not edits: a scope change asks
// the engine what the new scope's values are, and that is not a change.
void requireNoEdit(GraphCanvasModel &model) {
    for (const auto &request : model.takeRpcRequests()) {
        INFO("request " << request.method);
        REQUIRE(request.method != "edit_graph_document");
    }
}

void finishEdit(GraphCanvasModel &model, const Json &document,
                const Json &catalog) {
    for (const auto &request : model.takeRpcRequests()) {
        REQUIRE(request.method == "get_edit_result");
        model.receiveRpcResult(
            request.request_id,
            Json{{"status", "committed"}, {"committed", true}}.dump());
    }
    answer(model, document, catalog);
}

QGraphicsItem *kindItem(QGraphicsScene &scene, const char *kind,
                        const QString &node) {
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{kind} &&
            item->data(PelicanStudio::NodeGraphNameRole).toString() == node)
            return item;
    return nullptr;
}

// The in-place editor as a widget, so the keyboard path -- Enter confirms,
// Escape abandons -- can be driven the way a user drives it.
QWidget *editorWidget(QGraphicsScene &scene) {
    for (QGraphicsItem *item : scene.items()) {
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() !=
            QLatin1String{PelicanStudio::GraphCanvasEditorItem})
            continue;
        if (auto *proxy = dynamic_cast<QGraphicsProxyWidget *>(item))
            return proxy->widget();
    }
    return nullptr;
}

void typeKey(QWidget *widget, int key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    REQUIRE(widget != nullptr);
    QKeyEvent event{QEvent::KeyPress, key, modifiers};
    QApplication::sendEvent(widget, &event);
    // The editor reports its decision from the event loop: it cannot delete
    // itself inside the event it is still dispatching.
    QCoreApplication::processEvents();
}

// Both instance inputs fed from the one producer, so the root scope holds two
// edges -- the smallest graph a bundle can be made out of.
Json twoEdgeDocument() {
    auto document = fixtureDocument();
    for (auto &node : document.at("nodes"))
        if (node.at("id") == "left")
            node["inputs"] = {{"gain", {{"node", "clock"}, {"port", "value"}}},
                              {"bias", {{"node", "clock"}, {"port", "value"}}}};
    return document;
}

// Two producers each feeding a node two columns away, with a node in between
// that both of those edges have to fly over. Nothing smaller makes the
// automatic placement keep a lane at all, and a lane is what "Auto layout"
// turns into a control point.
Json flyoverDocument() {
    const auto input = [](const char *id, const char *name) {
        return Json{{"id", id}, {"op", "value.input"},
                    {"parameters", {{"name", name}}},
                    {"outputs", Json::array({"value"})}};
    };
    const auto pair = [](const char *id, const char *op, const char *a,
                         const char *b) {
        return Json{{"id", id}, {"op", op},
                    {"inputs", {{"a", {{"node", a}, {"port", "value"}}},
                                {"b", {{"node", b}, {"port", "value"}}}}},
                    {"outputs", Json::array({"value"})}};
    };
    return Json{{"schema", "pelican.graph"}, {"version", 1},
                {"definitions", Json::object()}, {"outputs", Json::object()},
                {"nodes", Json::array({input("src_a", "scene_time"),
                                       input("src_b", "delta_time"),
                                       pair("mid", "value.multiply", "src_a", "src_b"),
                                       pair("gate_a", "value.add", "src_a", "mid"),
                                       pair("gate_b", "value.add", "src_b", "mid")})}};
}

// Two dependencies between the same pair of nodes. The placement counts that
// pair once, so both edges fly over the middle column in one lane -- and a
// shared lane is exactly what a bundle is. This is the smallest graph "Auto
// layout" makes one on.
Json bundleDocument() {
    return Json{{"schema", "pelican.graph"}, {"version", 1},
                {"definitions", Json::object()}, {"outputs", Json::object()},
                {"nodes", Json::array({
        Json{{"id", "src"}, {"op", "value.input"},
             {"parameters", {{"name", "scene_time"}}},
             {"outputs", Json::array({"value"})}},
        Json{{"id", "step"}, {"op", "value.curve"},
             {"inputs", {{"time", {{"node", "src"}, {"port", "value"}}}}},
             {"outputs", Json::array({"value"})}},
        Json{{"id", "gate"}, {"op", "value.mix"},
             {"inputs", {{"a", {{"node", "src"}, {"port", "value"}}},
                         {"b", {{"node", "src"}, {"port", "value"}}},
                         {"c", {{"node", "step"}, {"port", "value"}}}}},
             {"outputs", Json::array({"value"})}}})}};
}

QGraphicsItem *controlItem(QGraphicsScene &scene, const char *kind,
                           const QString &identity) {
    for (QGraphicsItem *item : scene.items()) {
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() !=
            QLatin1String{kind})
            continue;
        QString name = item->data(PelicanStudio::NodeGraphNameRole).toString();
        const QString port =
            item->data(PelicanStudio::GraphCanvasPortRole).toString();
        if (!port.isEmpty()) name += QStringLiteral("/") + port;
        if (name == identity) return item;
    }
    return nullptr;
}

QVector<QPointF> edgeBends(QGraphicsScene &scene, const QString &from,
                           const QString &from_port, const QString &to,
                           const QString &to_port) {
    QGraphicsPathItem *line = edgeItem(scene, from, from_port, to, to_port);
    REQUIRE(line != nullptr);
    return PelicanStudio::nodeGraphBends(*line);
}

// The one `set_layout` a gesture produced, in the shape the document holds.
Json takeLayoutEdit(GraphCanvasModel &model) {
    const auto params = takeEdit(model);
    const auto &ops = params.at("ops");
    REQUIRE(ops.size() == 1);
    REQUIRE(ops.at(0).at("op") == "set_layout");
    return ops.at(0).at("layout");
}

qreal nodeHeight(QGraphicsScene &scene, const QString &name) {
    auto *item = dynamic_cast<QGraphicsPathItem *>(nodeItem(scene, name));
    REQUIRE(item != nullptr);
    return item->path().boundingRect().height();
}
} // namespace

TEST_CASE("The graph canvas lays every edge out left to right",
          "[devstudio][graph-canvas][scene]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, connectedDocument());
    scene.setModel(&model);

    REQUIRE(scene.property("pelicanGraphScope").toString() ==
            QStringLiteral("root"));
    REQUIRE(scene.property("pelicanGraphNodeCount").toULongLong() == 2);
    REQUIRE(scene.property("pelicanGraphEdgeCount").toULongLong() == 1);

    const auto check_columns = [&] {
        std::map<QString, int> columns;
        for (QGraphicsItem *item : itemsOfKind(scene, PelicanStudio::GraphCanvasNodeItem))
            columns[item->data(PelicanStudio::NodeGraphNameRole).toString()] =
                item->data(PelicanStudio::GraphCanvasColumnRole).toInt();
        const auto edges = itemsOfKind(scene, PelicanStudio::GraphCanvasEdgeItem);
        REQUIRE_FALSE(edges.empty());
        for (QGraphicsItem *edge : edges) {
            const QString from = edge->data(PelicanStudio::NodeGraphFromNameRole).toString();
            const QString to = edge->data(PelicanStudio::NodeGraphToNameRole).toString();
            REQUIRE(columns.contains(from));
            REQUIRE(columns.contains(to));
            REQUIRE(columns.at(from) < columns.at(to));
        }
        return columns;
    };
    const auto root_columns = check_columns();
    REQUIRE(root_columns.at(QStringLiteral("clock")) == 0);
    REQUIRE(root_columns.at(QStringLiteral("left")) == 1);

    // Every edge of the four-node definition body points forward too.
    REQUIRE(model.enterInstance("left"));
    scene.repopulate();
    REQUIRE(scene.property("pelicanGraphNodeCount").toULongLong() == 4);
    REQUIRE(scene.property("pelicanGraphEdgeCount").toULongLong() == 3);
    const auto definition_columns = check_columns();
    REQUIRE(definition_columns.at(QStringLiteral("gain")) == 0);
    REQUIRE(definition_columns.at(QStringLiteral("bias")) == 0);
    REQUIRE(definition_columns.at(QStringLiteral("sum")) == 1);
    REQUIRE(definition_columns.at(QStringLiteral("buffer")) == 2);
}

TEST_CASE("A moved node keeps its place across a refresh and loses it when it goes",
          "[devstudio][graph-canvas][scene]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, connectedDocument());
    scene.setModel(&model);

    QGraphicsItem *left = nodeItem(scene, QStringLiteral("left"));
    REQUIRE(left != nullptr);
    const QPointF laid_out = left->pos();
    const QPointF moved = laid_out + QPointF{180.0, 260.0};
    left->setPos(moved);

    // The drag reroutes the edge into it without a rebuild.
    const auto edges = itemsOfKind(scene, PelicanStudio::GraphCanvasEdgeItem);
    REQUIRE(edges.size() == 1);
    REQUIRE(edges.front()->sceneBoundingRect().bottom() >
            laid_out.y() + PelicanStudio::NodeGraphNodeHeight);

    // A refresh that lands on the same document must not shove it back.
    model.refresh();
    answer(model, connectedDocument());
    scene.repopulate();
    REQUIRE(nodeItem(scene, QStringLiteral("left"))->pos() == moved);

    // A revision that no longer holds the node throws its position away, so
    // the same id coming back later starts from the layout, not the drag.
    auto without = connectedDocument();
    auto nodes = Json::array();
    for (const auto &node : without.at("nodes"))
        if (node.at("id") != "left") nodes.push_back(node);
    without["nodes"] = nodes;
    model.refresh();
    answer(model, without);
    scene.repopulate();
    REQUIRE(nodeItem(scene, QStringLiteral("left")) == nullptr);

    model.refresh();
    answer(model, connectedDocument());
    scene.repopulate();
    REQUIRE(nodeItem(scene, QStringLiteral("left"))->pos() == laid_out);
}

TEST_CASE("Double-clicking an instance descends and the breadcrumb comes back out",
          "[devstudio][graph-canvas][scene]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, connectedDocument());
    int scope_changes = 0;
    scene.scopeChanged = [&] { ++scope_changes; };
    scene.setModel(&model);

    REQUIRE(scene.property("pelicanGraphBreadcrumb").toString() ==
            QStringLiteral("root"));
    REQUIRE(itemsOfKind(scene, PelicanStudio::GraphCanvasBreadcrumbItem).empty());

    // The clock is not an instance: double-clicking it changes nothing.
    QGraphicsItem *clock = nodeItem(scene, QStringLiteral("clock"));
    REQUIRE(clock != nullptr);
    click(scene, clock->sceneBoundingRect().center(),
          QEvent::GraphicsSceneMouseDoubleClick);
    REQUIRE(model.currentScope().empty());
    REQUIRE(scope_changes == 0);

    QGraphicsItem *left = nodeItem(scene, QStringLiteral("left"));
    REQUIRE(left != nullptr);
    click(scene, left->sceneBoundingRect().center(),
          QEvent::GraphicsSceneMouseDoubleClick);
    REQUIRE(scope_changes == 1);
    REQUIRE(model.currentScope() == "gain_curve");
    REQUIRE(scene.property("pelicanGraphScope").toString() ==
            QStringLiteral("gain_curve"));
    REQUIRE(scene.property("pelicanGraphBreadcrumb").toString() ==
            QStringLiteral("root/gain_curve"));
    REQUIRE(nodeItem(scene, QStringLiteral("sum")) != nullptr);

    const auto crumbs = itemsOfKind(scene, PelicanStudio::GraphCanvasBreadcrumbItem);
    REQUIRE_FALSE(crumbs.empty());
    click(scene, crumbs.front()->sceneBoundingRect().center());
    REQUIRE(scope_changes == 2);
    REQUIRE(model.currentScope().empty());
    REQUIRE(scene.property("pelicanGraphScope").toString() ==
            QStringLiteral("root"));
    REQUIRE(nodeItem(scene, QStringLiteral("sum")) == nullptr);
    REQUIRE(nodeItem(scene, QStringLiteral("left")) != nullptr);
}

TEST_CASE("Two scopes keep their own moved positions",
          "[devstudio][graph-canvas][scene]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, connectedDocument());
    scene.setModel(&model);

    QGraphicsItem *left = nodeItem(scene, QStringLiteral("left"));
    const QPointF root_move = left->pos() + QPointF{40.0, 90.0};
    left->setPos(root_move);

    REQUIRE(model.enterInstance("left"));
    scene.repopulate();
    QGraphicsItem *sum = nodeItem(scene, QStringLiteral("sum"));
    REQUIRE(sum != nullptr);
    const QPointF definition_move = sum->pos() + QPointF{70.0, 30.0};
    sum->setPos(definition_move);

    REQUIRE(model.leaveScope());
    scene.repopulate();
    REQUIRE(nodeItem(scene, QStringLiteral("left"))->pos() == root_move);
    REQUIRE(model.enterInstance("left"));
    scene.repopulate();
    REQUIRE(nodeItem(scene, QStringLiteral("sum"))->pos() == definition_move);
}

TEST_CASE("One resolver answers a socket: the module, then the table, then the traits",
          "[devstudio][graph-canvas][scene][socket-style]") {
    (void)application();
    PelicanStudio::resetSocketStyleDefaults();
    GraphCanvasModel model;
    model.setConnected(true);
    answer(model, connectedDocument(), visualCatalog());
    REQUIRE(model.catalogReceived());

    const auto style = [&](const char *type) {
        return PelicanStudio::socketStyleFor(
            PelicanStudio::graphCanvasSocketFacts(model, type));
    };

    SECTION("the shipped table is what a named type draws as") {
        const auto scalar = style("pelican.value.scalar@1");
        REQUIRE(scalar.shape == PelicanStudio::SocketShape::circle);
        REQUIRE(scalar.fill == QColor{QStringLiteral("#a9b4bf")});
        // The table's namespace rule covers every render resource at once, so
        // a new one needs no entry of its own.
        const auto buffer = style("pelican.render.storage_buffer@1");
        REQUIRE(buffer.shape == PelicanStudio::SocketShape::circle);
        REQUIRE(buffer.fill == QColor{QStringLiteral("#4a86c8")});
        const auto pose = style("pelican.pose@1");
        REQUIRE(pose.shape == PelicanStudio::SocketShape::diamond);
        REQUIRE(pose.fill == QColor{QStringLiteral("#e08b3c")});
    }

    SECTION("a module's own declaration outranks the table") {
        // pelican.render.surface@1 falls under the table's namespace rule and
        // would be a blue circle; its own schema says otherwise.
        const auto declared = style("pelican.render.surface@1");
        REQUIRE(declared.shape == PelicanStudio::SocketShape::hexagon);
        REQUIRE(declared.fill == QColor{QStringLiteral("#123456")});
    }

    SECTION("a type nobody declared is the unknown socket") {
        const auto unknown = style("acme.mystery@1");
        REQUIRE(unknown.shape == PelicanStudio::SocketShape::circle);
        REQUIRE(unknown.fill == QColor{QStringLiteral("#f4f7fa")});
        // And so is a port with no type at all.
        REQUIRE(style("").fill == QColor{QStringLiteral("#f4f7fa")});
    }

    SECTION("the control: an empty table sends everything to the traits rule") {
        PelicanStudio::setSocketStyleDefaults("{}");
        // The table was the only thing that made a scalar grey.
        const auto scalar = style("pelican.value.scalar@1");
        REQUIRE(scalar.fill == QColor{QStringLiteral("#f4f7fa")});
        REQUIRE(scalar.shape == PelicanStudio::SocketShape::circle);
        REQUIRE(style("pelican.render.storage_buffer@1").fill ==
                QColor{QStringLiteral("#f4f7fa")});
        // What survives an empty table is the built-in traits rule and the
        // module's own declaration -- nothing else.
        REQUIRE(style("pelican.anim.handle@1").shape ==
                PelicanStudio::SocketShape::diamond);
        REQUIRE(style("pelican.render.surface@1").shape ==
                PelicanStudio::SocketShape::hexagon);
        REQUIRE(style("pelican.render.surface@1").fill ==
                QColor{QStringLiteral("#123456")});
        PelicanStudio::resetSocketStyleDefaults();
    }
    PelicanStudio::resetSocketStyleDefaults();
}

TEST_CASE("A node is a heading over socket rows and the edges land on the sockets",
          "[devstudio][graph-canvas][scene][socket-style]") {
    (void)application();
    PelicanStudio::resetSocketStyleDefaults();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, connectedDocument(), visualCatalog());
    scene.setModel(&model);

    REQUIRE(model.enterInstance("left"));
    scene.repopulate();

    // The heading names the node and, beside it, what the node is.
    REQUIRE(headingHas(scene, QStringLiteral("sum"), QStringLiteral("sum"), true));
    REQUIRE(headingHas(scene, QStringLiteral("sum"),
                       QStringLiteral("value.add"), false));
    // A node carrying a `label` is headed by it; its type stays visible.
    REQUIRE(headingHas(scene, QStringLiteral("buffer"),
                       QStringLiteral("pulse buffer"), true));
    REQUIRE(headingHas(scene, QStringLiteral("buffer"),
                       QStringLiteral("render.buffer"), false));

    // One socket per declared port, on the side that port belongs to.
    QGraphicsItem *a = socketItem(scene, QStringLiteral("sum"),
                                  QStringLiteral("a"), QStringLiteral("input"));
    QGraphicsItem *b = socketItem(scene, QStringLiteral("sum"),
                                  QStringLiteral("b"), QStringLiteral("input"));
    QGraphicsItem *out = socketItem(scene, QStringLiteral("sum"),
                                    QStringLiteral("value"),
                                    QStringLiteral("output"));
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(out != nullptr);
    REQUIRE(socketItem(scene, QStringLiteral("sum"), QStringLiteral("a"),
                       QStringLiteral("output")) == nullptr);
    // The type reached the socket, so its shape is the type's, not a default.
    REQUIRE(a->data(PelicanStudio::GraphCanvasPortTypeRole).toString() ==
            QStringLiteral("pelican.value.scalar@1"));
    REQUIRE(socketItem(scene, QStringLiteral("buffer"),
                       QStringLiteral("resource"), QStringLiteral("output"))
                ->data(PelicanStudio::GraphCanvasPortTypeRole)
                .toString() == QStringLiteral("pelican.render.storage_buffer@1"));

    QGraphicsItem *node = nodeItem(scene, QStringLiteral("sum"));
    REQUIRE(node != nullptr);
    // Half outside the node, the way a node editor draws a socket: the centre
    // sits on the edge, so the dot straddles it.
    const QRectF body = node->sceneBoundingRect();
    REQUIRE(std::abs(a->sceneBoundingRect().center().x() - body.left()) < 1.5);
    REQUIRE(std::abs(out->sceneBoundingRect().center().x() - body.right()) < 1.5);
    REQUIRE(a->sceneBoundingRect().left() < body.left());
    REQUIRE(out->sceneBoundingRect().right() > body.right());
    // The output row sits above the input rows, as the reference node does.
    REQUIRE(out->sceneBoundingRect().center().y() <
            a->sceneBoundingRect().center().y());
    REQUIRE(a->sceneBoundingRect().center().y() <
            b->sceneBoundingRect().center().y());

    // The edge runs socket centre to socket centre, not node edge to node edge.
    QGraphicsPathItem *edge =
        edgeItem(scene, QStringLiteral("gain"), QStringLiteral("value"),
                 QStringLiteral("sum"), QStringLiteral("a"));
    REQUIRE(edge != nullptr);
    QGraphicsItem *gain_out =
        socketItem(scene, QStringLiteral("gain"), QStringLiteral("value"),
                   QStringLiteral("output"));
    REQUIRE(gain_out != nullptr);
    const QPainterPath path = edge->path();
    REQUIRE(path.elementCount() >= 2);
    const QPointF start{path.elementAt(0).x, path.elementAt(0).y};
    const QPointF tip{path.elementAt(path.elementCount() - 1).x,
                      path.elementAt(path.elementCount() - 1).y};
    REQUIRE(start == gain_out->sceneBoundingRect().center());
    REQUIRE(tip == a->sceneBoundingRect().center());
    // And it still does after a drag, which is the only thing that moves a
    // socket without rebuilding the scene.
    node->setPos(node->pos() + QPointF{60.0, 130.0});
    const QPainterPath moved = edge->path();
    const QPointF moved_tip{moved.elementAt(moved.elementCount() - 1).x,
                            moved.elementAt(moved.elementCount() - 1).y};
    REQUIRE(moved_tip == a->sceneBoundingRect().center());

    // The edge labels are gone: the rows say what the ends are.
    REQUIRE(itemsOfKind(scene, "graph_edge_label").empty());
}

TEST_CASE("Unconnected input rows carry the value, connected ones carry none",
          "[devstudio][graph-canvas][scene][socket-style]") {
    (void)application();
    PelicanStudio::resetSocketStyleDefaults();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    const auto document = readFixture("graph_node_visuals");
    answer(model, document, visualCatalog());
    scene.setModel(&model);

    REQUIRE(model.currentScope().empty());
    // The heading is the authored label; the id is not what the band shows.
    REQUIRE(headingHas(scene, QStringLiteral("helmet_pulse"),
                       QStringLiteral("Helmet pulse"), true));
    REQUIRE(headingHas(scene, QStringLiteral("helmet_pulse"),
                       QStringLiteral("graph.instance"), false));
    REQUIRE_FALSE(headingHas(scene, QStringLiteral("helmet_pulse"),
                             QStringLiteral("helmet_pulse"), true));
    // An instance without a label is still headed by its id.
    REQUIRE(headingHas(scene, QStringLiteral("bloom_trim"),
                       QStringLiteral("bloom_trim"), true));

    // The definition's boundary ports are the instance's rows.
    for (const auto *port : {"time", "gain", "bias"})
        REQUIRE(socketItem(scene, QStringLiteral("helmet_pulse"),
                           QString::fromLatin1(port),
                           QStringLiteral("input")) != nullptr);

    // Unconnected and standing on a stored parameter: the row shows it.
    QGraphicsItem *gain =
        pillItem(scene, QStringLiteral("helmet_pulse"), QStringLiteral("gain"));
    QGraphicsItem *bias =
        pillItem(scene, QStringLiteral("helmet_pulse"), QStringLiteral("bias"));
    REQUIRE(gain != nullptr);
    REQUIRE(bias != nullptr);
    REQUIRE(gain->data(PelicanStudio::GraphCanvasValueTextRole).toString() ==
            QStringLiteral("2.5"));
    REQUIRE(bias->data(PelicanStudio::GraphCanvasValueTextRole).toString() ==
            QStringLiteral("0.5"));
    REQUIRE(gain->data(PelicanStudio::GraphCanvasValueSourceRole).toString() ==
            QStringLiteral("stored"));
    // `time` stores a value too -- the number the port returns to when the
    // edge is removed -- but it is connected, so the edge is the answer and
    // the row carries no pill.
    REQUIRE(port(model, "helmet_pulse", "time").has_parameter);
    REQUIRE(port(model, "helmet_pulse", "time").connected);
    REQUIRE(pillItem(scene, QStringLiteral("helmet_pulse"),
                     QStringLiteral("time")) == nullptr);
    // The pill sits on the right of its own row, beside that port's socket.
    QGraphicsItem *gain_socket =
        socketItem(scene, QStringLiteral("helmet_pulse"),
                   QStringLiteral("gain"), QStringLiteral("input"));
    REQUIRE(std::abs(gain->sceneBoundingRect().center().y() -
                     gain_socket->sceneBoundingRect().center().y()) < 2.0);
    REQUIRE(gain->sceneBoundingRect().left() >
            gain_socket->sceneBoundingRect().right());

    // What a published generation really fed the port replaces the stored
    // number, and says where it came from.
    model.watchInstance("helmet_pulse");
    for (const auto &request : model.takeRpcRequests()) {
        // Naming an instance asks three questions now (AG-3b): its inputs, its
        // node values and the status the Timeline's axis is. This case is
        // about the inputs.
        if (request.method != "get_graph_values") continue;
        model.receiveRpcResult(
            request.request_id,
            Json{{"instance", "helmet_pulse"}, {"published", true},
                 {"generation", 4}, {"frame", 12},
                 {"inputs", Json::array({
                     Json{{"port", "time"}, {"value", 1.5},
                          {"source", "connected"},
                          {"connected", {{"node", "clock"}, {"port", "value"}}}},
                     Json{{"port", "gain"}, {"value", 9.0},
                          {"source", "override"}},
                     Json{{"port", "bias"}, {"value", 0.5},
                          {"source", "parameter"}}})}}
                .dump());
    }
    scene.repopulate();
    QGraphicsItem *published =
        pillItem(scene, QStringLiteral("helmet_pulse"), QStringLiteral("gain"));
    REQUIRE(published != nullptr);
    REQUIRE(published->data(PelicanStudio::GraphCanvasValueTextRole).toString() ==
            QStringLiteral("9"));
    REQUIRE(published->data(PelicanStudio::GraphCanvasValueSourceRole)
                .toString() == QStringLiteral("override"));
    // A published number for a connected port is still not a pill: the edge
    // into it is what the row shows.
    REQUIRE(port(model, "helmet_pulse", "time").has_value);
    REQUIRE(pillItem(scene, QStringLiteral("helmet_pulse"),
                     QStringLiteral("time")) == nullptr);
}

TEST_CASE("The heading triangle folds the socket rows and the edges follow",
          "[devstudio][graph-canvas][scene][graph-edit]") {
    (void)application();
    PelicanStudio::resetSocketStyleDefaults();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, connectedDocument(), visualCatalog());
    scene.setModel(&model);
    REQUIRE(model.enterInstance("left"));
    scene.repopulate();

    const qreal open_height = nodeHeight(scene, QStringLiteral("sum"));
    REQUIRE(open_height > PelicanStudio::GraphCanvasHeaderHeight + 1.0);
    REQUIRE_FALSE(scene.collapsed("sum"));
    REQUIRE(socketItem(scene, QStringLiteral("sum"), QStringLiteral("a"),
                       QStringLiteral("input")) != nullptr);

    QGraphicsItem *arrow = kindItem(
        scene, PelicanStudio::GraphCanvasCollapseItem, QStringLiteral("sum"));
    REQUIRE(arrow != nullptr);
    click(scene, arrow->sceneBoundingRect().center());

    REQUIRE(scene.collapsed("sum"));
    // The node is its heading band and nothing else.
    REQUIRE(std::abs(nodeHeight(scene, QStringLiteral("sum")) -
                     PelicanStudio::GraphCanvasHeaderHeight) < 0.01);
    // The per-port sockets are gone; one anchor stands for all of them.
    REQUIRE(socketItem(scene, QStringLiteral("sum"), QStringLiteral("a"),
                       QStringLiteral("input")) == nullptr);
    QGraphicsItem *anchor = socketItem(scene, QStringLiteral("sum"), QString{},
                                       QStringLiteral("input"));
    REQUIRE(anchor != nullptr);
    QGraphicsItem *folded = nodeItem(scene, QStringLiteral("sum"));
    REQUIRE(std::abs(anchor->sceneBoundingRect().center().y() -
                     (folded->pos().y() +
                      PelicanStudio::GraphCanvasHeaderHeight / 2.0)) < 0.01);

    // Both edges into the folded node now end on that one point.
    for (const auto &wire : {std::pair{"gain", "a"}, std::pair{"bias", "b"}}) {
        QGraphicsPathItem *edge = edgeItem(
            scene, QString::fromLatin1(wire.first), QStringLiteral("value"),
            QStringLiteral("sum"), QString::fromLatin1(wire.second));
        REQUIRE(edge != nullptr);
        const QPainterPath path = edge->path();
        const QPointF tip{path.elementAt(path.elementCount() - 1).x,
                          path.elementAt(path.elementCount() - 1).y};
        REQUIRE(tip == anchor->sceneBoundingRect().center());
    }

    // A refresh that lands on the same document keeps the fold, exactly as it
    // keeps a moved position.
    model.refresh();
    answer(model, connectedDocument(), visualCatalog());
    scene.repopulate();
    REQUIRE(scene.collapsed("sum"));
    REQUIRE(std::abs(nodeHeight(scene, QStringLiteral("sum")) -
                     PelicanStudio::GraphCanvasHeaderHeight) < 0.01);

    // And unfolding puts the rows back.
    click(scene, kindItem(scene, PelicanStudio::GraphCanvasCollapseItem,
                          QStringLiteral("sum"))
                     ->sceneBoundingRect()
                     .center());
    REQUIRE_FALSE(scene.collapsed("sum"));
    REQUIRE(nodeHeight(scene, QStringLiteral("sum")) == open_height);
    REQUIRE(socketItem(scene, QStringLiteral("sum"), QStringLiteral("a"),
                       QStringLiteral("input")) != nullptr);
    // Nothing about folding is an edit: the document was never asked to change.
    requireNoEdit(model);
}

TEST_CASE("A socket drag sends one set_input, and a mismatched type sends none",
          "[devstudio][graph-canvas][scene][graph-edit]") {
    (void)application();
    PelicanStudio::resetSocketStyleDefaults();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, readFixture("graph_node_visuals"), visualCatalog());
    scene.setModel(&model);
    REQUIRE(model.enterInstance("helmet_pulse"));
    scene.repopulate();

    SECTION("output to input") {
        QGraphicsItem *from =
            socketItem(scene, QStringLiteral("shape"), QStringLiteral("value"),
                       QStringLiteral("output"));
        QGraphicsItem *to = socketItem(scene, QStringLiteral("sum"),
                                       QStringLiteral("b"),
                                       QStringLiteral("input"));
        REQUIRE(from != nullptr);
        REQUIRE(to != nullptr);
        drag(scene, from->sceneBoundingRect().center(),
             to->sceneBoundingRect().center());

        const auto edit = takeEdit(model);
        REQUIRE(edit.at("base_source_digest") == "source-a");
        REQUIRE(edit.at("ops").size() == 1);
        REQUIRE(edit.at("ops").at(0) ==
                Json{{"op", "set_input"},
                     {"scope", "gain_curve"},
                     {"node", "sum"},
                     {"port", "b"},
                     {"source", {{"node", "shape"}, {"port", "value"}}}});
    }

    SECTION("a drag onto a port of another type does not land") {
        // The definition body types everything as a scalar, so the mismatch
        // has to come from the fixture that has a buffer in it.
        GraphCanvasModel typed;
        GraphCanvasScene typed_scene;
        typed.setConnected(true);
        answer(typed, connectedDocument(), visualCatalog());
        typed_scene.setModel(&typed);
        REQUIRE(typed.enterInstance("left"));
        typed_scene.repopulate();

        QGraphicsItem *buffer =
            socketItem(typed_scene, QStringLiteral("buffer"),
                       QStringLiteral("resource"), QStringLiteral("output"));
        QGraphicsItem *scalar =
            socketItem(typed_scene, QStringLiteral("sum"), QStringLiteral("a"),
                       QStringLiteral("input"));
        REQUIRE(buffer != nullptr);
        REQUIRE(scalar != nullptr);
        REQUIRE(buffer->data(PelicanStudio::GraphCanvasPortTypeRole).toString() ==
                QStringLiteral("pelican.render.storage_buffer@1"));
        REQUIRE_FALSE(typed.canConnect("buffer", "resource", "sum", "a"));
        drag(typed_scene, buffer->sceneBoundingRect().center(),
             scalar->sceneBoundingRect().center());
        requireNoEdit(typed);
        // The same drag onto a port that does match is still allowed, so it is
        // the type and not the drag that was refused.
        REQUIRE(typed.canConnect("gain", "value", "sum", "b"));
    }

    SECTION("dragging a connected input onto nothing cuts the edge") {
        QGraphicsItem *to = socketItem(scene, QStringLiteral("sum"),
                                       QStringLiteral("b"),
                                       QStringLiteral("input"));
        REQUIRE(to != nullptr);
        drag(scene, to->sceneBoundingRect().center(),
             to->sceneBoundingRect().center() + QPointF{0.0, -600.0});
        const auto edit = takeEdit(model);
        REQUIRE(edit.at("ops").size() == 1);
        REQUIRE(edit.at("ops").at(0) == Json{{"op", "set_input"},
                                             {"scope", "gain_curve"},
                                             {"node", "sum"},
                                             {"port", "b"},
                                             {"source", nullptr}});
    }
}

TEST_CASE("Selecting an edge and pressing Delete clears the input it lands on",
          "[devstudio][graph-canvas][scene][graph-edit]") {
    (void)application();
    PelicanStudio::resetSocketStyleDefaults();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, readFixture("graph_node_visuals"), visualCatalog());
    scene.setModel(&model);
    REQUIRE(model.enterInstance("helmet_pulse"));
    scene.repopulate();

    QGraphicsPathItem *edge =
        edgeItem(scene, QStringLiteral("bias"), QStringLiteral("value"),
                 QStringLiteral("sum"), QStringLiteral("b"));
    REQUIRE(edge != nullptr);
    click(scene, edge->path().pointAtPercent(0.5));
    REQUIRE(scene.selectedEdge().has_value());
    REQUIRE(scene.selectedEdge()->to_node == "sum");
    REQUIRE(scene.selectedEdge()->to_port == "b");
    REQUIRE_FALSE(scene.selectedNode().has_value());

    pressKey(scene, Qt::Key_Delete);
    const auto edit = takeEdit(model);
    REQUIRE(edit.at("ops").size() == 1);
    REQUIRE(edit.at("ops").at(0) == Json{{"op", "set_input"},
                                         {"scope", "gain_curve"},
                                         {"node", "sum"},
                                         {"port", "b"},
                                         {"source", nullptr}});
}

TEST_CASE("Graph keyboard Undo and Redo use committed history while text keeps its own Undo",
          "[devstudio][graph-canvas][scene][graph-edit][graph-history]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    const auto original = readFixture("graph_node_visuals");
    answer(model, original, visualCatalog());
    scene.setModel(&model);

    auto renamed = original;
    for (auto &node : renamed.at("nodes"))
        if (node.at("id") == "helmet_pulse") node["label"] = "Visor pulse";

    // Match the engine's commit and refresh digest: a submission alone must
    // never be enough to create an Undo entry.
    const auto commit = [&](const Json &document, const char *digest) {
        const auto polls = model.takeRpcRequests();
        REQUIRE(polls.size() == 1);
        REQUIRE(polls.front().method == "get_edit_result");
        model.receiveRpcResult(polls.front().request_id,
            Json{{"status", "committed"},
                 {"source_digest", {{"hex", digest}}}}.dump());
        const auto reads = model.takeRpcRequests();
        REQUIRE(reads.size() == 1);
        REQUIRE(reads.front().method == "get_graph_instances");
        auto snapshot = context(document);
        snapshot["source_digest"]["hex"] = digest;
        model.receiveRpcResult(reads.front().request_id, snapshot.dump());
        scene.repopulate();
    };

    REQUIRE(model.setNodeLabel("helmet_pulse", "Visor pulse"));
    const auto forward = takeEdit(model);
    REQUIRE_FALSE(model.canUndo());
    commit(renamed, "source-b");
    REQUIRE(model.canUndo());
    REQUIRE_FALSE(model.canRedo());

    SECTION("canvas shortcuts submit the inverse and then the original edit") {
        pressKey(scene, Qt::Key_Z, Qt::ControlModifier);
        const auto undo = takeEdit(model);
        REQUIRE(undo.at("base_source_digest") == "source-b");
        REQUIRE(undo.at("ops").size() == 1);
        REQUIRE(undo.at("ops").at(0).at("op") == "replace_node");
        REQUIRE(undo.at("ops").at(0).at("node").at("label") == "Helmet pulse");
        commit(original, "source-c");
        REQUIRE_FALSE(model.canUndo());
        REQUIRE(model.canRedo());

        pressKey(scene, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        const auto redo = takeEdit(model);
        REQUIRE(redo.at("base_source_digest") == "source-c");
        REQUIRE(redo.at("ops") == forward.at("ops"));
        commit(renamed, "source-d");
        REQUIRE(model.canUndo());
        REQUIRE_FALSE(model.canRedo());
    }

    SECTION("an inline label uses native text Undo without submitting graph history") {
        REQUIRE(scene.beginLabelEdit("helmet_pulse"));
        auto *line = qobject_cast<QLineEdit *>(editorWidget(scene));
        REQUIRE(line != nullptr);
        line->insert(QStringLiteral("Local draft"));
        REQUIRE(scene.inlineEditText() == QStringLiteral("Local draft"));
        REQUIRE(line->isUndoAvailable());

        typeKey(line, Qt::Key_Z, Qt::ControlModifier);
        REQUIRE(scene.inlineEditText() == QStringLiteral("Visor pulse"));
        REQUIRE(scene.editingInline());
        REQUIRE(model.canUndo());
        requireNoEdit(model);

        // Even if the key reaches the scene while the editor is open, graph
        // history must not consume it or submit a document mutation.
        pressKey(scene, Qt::Key_Z, Qt::ControlModifier);
        REQUIRE(scene.editingInline());
        REQUIRE(model.canUndo());
        requireNoEdit(model);
        typeKey(line, Qt::Key_Escape);
        REQUIRE_FALSE(scene.editingInline());
        requireNoEdit(model);
    }
}

TEST_CASE("A value pill opens in place and sends only the key that changed",
          "[devstudio][graph-canvas][scene][graph-edit]") {
    (void)application();
    PelicanStudio::resetSocketStyleDefaults();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    const auto document = readFixture("graph_node_visuals");
    answer(model, document, visualCatalog());
    scene.setModel(&model);

    QGraphicsItem *pill =
        pillItem(scene, QStringLiteral("helmet_pulse"), QStringLiteral("gain"));
    REQUIRE(pill != nullptr);
    click(scene, pill->sceneBoundingRect().center());
    REQUIRE(scene.editingInline());
    // The editor starts on the stored value, spelled the way the pill does.
    REQUIRE(scene.inlineEditText().toDouble() == 2.5);
    scene.setInlineEditText(QStringLiteral("4.5"));
    // Enter is what confirms it -- not a button, and not an Apply somewhere.
    typeKey(editorWidget(scene), Qt::Key_Return);
    REQUIRE_FALSE(scene.editingInline());

    const auto edit = takeEdit(model);
    REQUIRE(edit.at("ops").size() == 1);
    // set_node_parameters replaces the object, so the whole of it goes out --
    // with exactly one key different from what the document holds.
    REQUIRE(edit.at("ops").at(0) ==
            Json{{"op", "set_node_parameters"},
                 {"scope", "root"},
                 {"node", "helmet_pulse"},
                 {"parameters", {{"time", 0}, {"gain", 4.5}, {"bias", 0.5}}}});
    finishEdit(model, document, visualCatalog());

    // Escape is the control: the same editor, abandoned, sends nothing.
    QGraphicsItem *again =
        pillItem(scene, QStringLiteral("helmet_pulse"), QStringLiteral("gain"));
    REQUIRE(again != nullptr);
    click(scene, again->sceneBoundingRect().center());
    REQUIRE(scene.editingInline());
    scene.setInlineEditText(QStringLiteral("9.25"));
    typeKey(editorWidget(scene), Qt::Key_Escape);
    REQUIRE_FALSE(scene.editingInline());
    requireNoEdit(model);

    // A connected port has no pill, so there is nothing there to open.
    REQUIRE(pillItem(scene, QStringLiteral("helmet_pulse"),
                     QStringLiteral("time")) == nullptr);
    REQUIRE_FALSE(scene.beginValueEdit("helmet_pulse", "time"));
}

// Q-3. The in-place editor used to end on any FocusOut at all, and on
// Windows the Studio blinks inactive for one trip through the event loop when
// the keyboard moves between the embedded engine viewport's native window and
// the main window -- which is what opening this very editor does. Qt reports
// that blink as FocusOut(ActiveWindowFocusReason), so the editor committed and
// deleted itself a few hundred milliseconds after it opened, before anything
// could be typed into it. The same blink is what a person causes by Alt-Tabbing
// away mid-edit.
TEST_CASE("An in-place editor survives the window going inactive, and still "
          "ends when the focus really leaves it",
          "[devstudio][graph-canvas][scene][graph-edit]") {
    (void)application();
    PelicanStudio::resetSocketStyleDefaults();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    const auto document = readFixture("graph_node_visuals");
    answer(model, document, visualCatalog());
    scene.setModel(&model);

    QGraphicsItem *pill =
        pillItem(scene, QStringLiteral("helmet_pulse"), QStringLiteral("gain"));
    REQUIRE(pill != nullptr);
    click(scene, pill->sceneBoundingRect().center());
    REQUIRE(scene.editingInline());
    scene.setInlineEditText(QStringLiteral("3.75"));

    // The application went to the back and came forward again. Nothing about
    // the edit changed: the editor is still there, still holding what was
    // typed, and still the thing the keyboard reaches.
    QWidget *editing = editorWidget(scene);
    REQUIRE(editing != nullptr);
    QFocusEvent away{QEvent::FocusOut, Qt::ActiveWindowFocusReason};
    QApplication::sendEvent(editing, &away);
    QCoreApplication::processEvents();
    REQUIRE(scene.editingInline());
    REQUIRE(scene.inlineEditText().toDouble() == 3.75);
    REQUIRE(editorWidget(scene) == editing);
    requireNoEdit(model);
    QFocusEvent back{QEvent::FocusIn, Qt::ActiveWindowFocusReason};
    QApplication::sendEvent(editing, &back);
    QCoreApplication::processEvents();
    REQUIRE(scene.editingInline());

    // A popup taking the keyboard is the same kind of loan, and is refused the
    // same way.
    QFocusEvent popup{QEvent::FocusOut, Qt::PopupFocusReason};
    QApplication::sendEvent(editing, &popup);
    QCoreApplication::processEvents();
    REQUIRE(scene.editingInline());
    requireNoEdit(model);

    // The control: the focus really leaving for another widget is a person
    // saying "done here", and that still commits -- the spreadsheet rule the
    // editor was built on.
    QFocusEvent elsewhere{QEvent::FocusOut, Qt::MouseFocusReason};
    QApplication::sendEvent(editing, &elsewhere);
    QCoreApplication::processEvents();
    REQUIRE_FALSE(scene.editingInline());
    const auto edit = takeEdit(model);
    REQUIRE(edit.at("ops").at(0).at("parameters").at("gain") == 3.75);
}

// The other half of the same rule: the canvas is redrawn four times a second
// while Live is on (AG-3b), and a redraw deletes every item. The editor is
// carried over the rebuild rather than deleted with them.
TEST_CASE("A redraw under an open editor re-opens it with what was typed",
          "[devstudio][graph-canvas][scene][graph-edit]") {
    (void)application();
    PelicanStudio::resetSocketStyleDefaults();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    const auto document = readFixture("graph_node_visuals");
    answer(model, document, visualCatalog());
    scene.setModel(&model);

    QGraphicsItem *pill =
        pillItem(scene, QStringLiteral("helmet_pulse"), QStringLiteral("gain"));
    REQUIRE(pill != nullptr);
    click(scene, pill->sceneBoundingRect().center());
    REQUIRE(scene.editingInline());
    scene.setInlineEditText(QStringLiteral("6.25"));

    scene.repopulate();
    REQUIRE(scene.editingInline());
    REQUIRE(scene.inlineEditText().toDouble() == 6.25);
    // The editor is over the pill the new draw made, not a dangling proxy from
    // the old one, and Enter still reaches it.
    QWidget *reopened = editorWidget(scene);
    REQUIRE(reopened != nullptr);
    requireNoEdit(model);
    typeKey(reopened, Qt::Key_Return);
    REQUIRE_FALSE(scene.editingInline());
    const auto edit = takeEdit(model);
    REQUIRE(edit.at("ops").at(0).at("parameters").at("gain") == 6.25);

    // The control: a redraw that no longer holds the port has nothing to open
    // over, and the edit ends there rather than hanging on a name that is gone.
    finishEdit(model, document, visualCatalog());
    QGraphicsItem *again =
        pillItem(scene, QStringLiteral("helmet_pulse"), QStringLiteral("gain"));
    REQUIRE(again != nullptr);
    click(scene, again->sceneBoundingRect().center());
    REQUIRE(scene.editingInline());
    auto without = document;
    for (auto &node : without.at("nodes"))
        if (node.at("id") == "helmet_pulse") node.at("id") = "helmet_gone";
    model.refresh();
    answer(model, without, visualCatalog());
    scene.repopulate();
    REQUIRE_FALSE(scene.editingInline());
}

TEST_CASE("The palette adds a node and Delete takes one away",
          "[devstudio][graph-canvas][scene][graph-edit]") {
    (void)application();
    PelicanStudio::resetSocketStyleDefaults();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    const auto document = readFixture("graph_node_visuals");
    answer(model, document, visualCatalog());
    scene.setModel(&model);

    SECTION("what the palette offers depends on which side of the boundary it is on") {
        const auto root = model.paletteOperations();
        REQUIRE(std::ranges::any_of(root, [](const auto &entry) {
            return entry.op == "graph.instance" && entry.structural;
        }));
        REQUIRE(std::ranges::none_of(
            root, [](const auto &entry) { return entry.op == "graph.input"; }));
        REQUIRE(model.enterInstance("helmet_pulse"));
        const auto body = model.paletteOperations();
        REQUIRE(std::ranges::any_of(body, [](const auto &entry) {
            return entry.op == "graph.input" && entry.structural;
        }));
        REQUIRE(std::ranges::none_of(body, [](const auto &entry) {
            return entry.op == "graph.instance";
        }));
        // The text filter is a substring of the operation name.
        const auto filtered = model.paletteOperations("multi");
        REQUIRE(filtered.size() == 1);
        REQUIRE(filtered.front().op == "value.multiply");
        REQUIRE(model.paletteOperations({}, "rendering").size() == 1);
    }

    SECTION("an operation with no required input arrives on its own") {
        REQUIRE(model.enterInstance("helmet_pulse"));
        scene.repopulate();
        REQUIRE(scene.addFromPalette("value.constant"));
        const auto edit = takeEdit(model);
        REQUIRE(edit.at("ops").size() == 1);
        REQUIRE(edit.at("ops").at(0) ==
                Json{{"op", "add_node"},
                     {"scope", "gain_curve"},
                     {"node", {{"id", "value_constant"},
                               {"op", "value.constant"},
                               {"outputs", Json::array({"value"})}}}});
    }

    SECTION("an operation with required inputs arrives standing on its own values") {
        REQUIRE(model.enterInstance("helmet_pulse"));
        scene.repopulate();
        REQUIRE(scene.addFromPalette("value.multiply"));
        const auto edit = takeEdit(model);
        // One gesture is still one call: the seeds and the node travel
        // together, because a node whose required input is unconnected is not
        // a document the engine would load.
        REQUIRE(edit.at("ops").size() == 3);
        REQUIRE(edit.at("ops").at(0).at("node").at("id") == "value_multiply__a");
        REQUIRE(edit.at("ops").at(0).at("node").at("op") == "value.constant");
        REQUIRE(edit.at("ops").at(2).at("node").at("id") == "value_multiply");
        REQUIRE(edit.at("ops").at(2).at("node").at("inputs") ==
                Json{{"a", {{"node", "value_multiply__a"}, {"port", "value"}}},
                     {"b", {{"node", "value_multiply__b"}, {"port", "value"}}}});
        for (const auto &op : edit.at("ops")) REQUIRE(op.at("scope") == "gain_curve");
    }

    SECTION("a selected node is removed by name") {
        REQUIRE(model.enterInstance("helmet_pulse"));
        scene.repopulate();
        QGraphicsItem *node = nodeItem(scene, QStringLiteral("shape"));
        REQUIRE(node != nullptr);
        click(scene, node->sceneBoundingRect().center());
        REQUIRE(scene.selectedNode() == std::optional<std::string>{"shape"});
        pressKey(scene, Qt::Key_Delete);
        const auto edit = takeEdit(model);
        REQUIRE(edit.at("ops").size() == 1);
        REQUIRE(edit.at("ops").at(0) == Json{{"op", "remove_node"},
                                             {"scope", "gain_curve"},
                                             {"node", "shape"}});
    }
}

TEST_CASE("Double-clicking a heading renames, and below it still descends",
          "[devstudio][graph-canvas][scene][graph-edit]") {
    (void)application();
    PelicanStudio::resetSocketStyleDefaults();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    const auto document = readFixture("graph_node_visuals");
    answer(model, document, visualCatalog());
    scene.setModel(&model);

    QGraphicsItem *node = nodeItem(scene, QStringLiteral("helmet_pulse"));
    REQUIRE(node != nullptr);
    click(scene,
          QPointF{node->sceneBoundingRect().center().x(),
                  node->pos().y() +
                      PelicanStudio::GraphCanvasHeaderHeight / 2.0},
          QEvent::GraphicsSceneMouseDoubleClick);
    REQUIRE(scene.editingInline());
    // The field starts on the authored label, not on the heading fallback.
    REQUIRE(scene.inlineEditText() == QStringLiteral("Helmet pulse"));
    // The scope did not change: a rename is not a descent.
    REQUIRE(model.currentScope().empty());
    scene.setInlineEditText(QStringLiteral("Visor pulse"));
    REQUIRE(scene.commitInlineEdit());
    const auto edit = takeEdit(model);
    REQUIRE(edit.at("ops").size() == 1);
    REQUIRE(edit.at("ops").at(0) == Json{{"op", "set_node_label"},
                                         {"scope", "root"},
                                         {"node", "helmet_pulse"},
                                         {"label", "Visor pulse"}});
    finishEdit(model, document, visualCatalog());

    // The empty string is how a label is taken off again.
    REQUIRE(scene.beginLabelEdit("helmet_pulse"));
    scene.setInlineEditText(QString{});
    REQUIRE(scene.commitInlineEdit());
    REQUIRE(takeEdit(model).at("ops").at(0).at("label") == "");
    finishEdit(model, document, visualCatalog());

    // Below the heading, a double-click still means show me what is in it.
    scene.repopulate();
    QGraphicsItem *again = nodeItem(scene, QStringLiteral("helmet_pulse"));
    click(scene,
          QPointF{again->sceneBoundingRect().center().x(),
                  again->pos().y() +
                      PelicanStudio::GraphCanvasHeaderHeight + 12.0},
          QEvent::GraphicsSceneMouseDoubleClick);
    REQUIRE_FALSE(scene.editingInline());
    REQUIRE(model.currentScope() == "gain_curve");
}

TEST_CASE("A CAS mismatch keeps the notice, drops the gesture and reads again",
          "[devstudio][graph-canvas][scene][graph-edit]") {
    (void)application();
    PelicanStudio::resetSocketStyleDefaults();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    const auto document = readFixture("graph_node_visuals");
    answer(model, document, visualCatalog());
    scene.setModel(&model);

    QGraphicsItem *pill =
        pillItem(scene, QStringLiteral("helmet_pulse"), QStringLiteral("gain"));
    REQUIRE(pill != nullptr);
    click(scene, pill->sceneBoundingRect().center());
    scene.setInlineEditText(QStringLiteral("4.5"));
    REQUIRE(scene.commitInlineEdit());

    auto requests = model.takeRpcRequests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().method == "edit_graph_document");
    // The engine read a different document than the canvas did.
    model.receiveRpcResult(
        requests.front().request_id,
        Json{{"status", "rejected"},
             {"error",
              {{"code", "external_modification"},
               {"message", "render config base digest no longer matches"}}}}
            .dump());

    REQUIRE(model.notice().kind == PelicanStudio::InspectorNoticeKind::Error);
    REQUIRE(model.notice().message.find("external_modification") !=
            std::string::npos);
    // The gesture is gone and the canvas asks for the document again rather
    // than standing on the one it had.
    auto after = model.takeRpcRequests();
    REQUIRE(after.size() == 1);
    REQUIRE(after.front().method == "get_graph_instances");
    model.receiveRpcResult(after.front().request_id, context(document).dump());
    scene.repopulate();
    REQUIRE(pillItem(scene, QStringLiteral("helmet_pulse"),
                     QStringLiteral("gain"))
                ->data(PelicanStudio::GraphCanvasValueTextRole)
                .toString() == QStringLiteral("2.5"));
    // And the canvas can be edited again: the failure did not wedge it.
    REQUIRE(model.canEdit());
}

TEST_CASE("A refresh keeps the node the user was working on selected",
          "[devstudio][graph-canvas][scene][graph-edit]") {
    (void)application();
    PelicanStudio::resetSocketStyleDefaults();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, connectedDocument(), visualCatalog());
    scene.setModel(&model);

    QGraphicsItem *left = nodeItem(scene, QStringLiteral("left"));
    REQUIRE(left != nullptr);
    click(scene, left->sceneBoundingRect().center());
    REQUIRE(scene.selectedNode() == std::optional<std::string>{"left"});

    model.refresh();
    answer(model, connectedDocument(), visualCatalog());
    scene.repopulate();
    REQUIRE(scene.selectedNode() == std::optional<std::string>{"left"});
    REQUIRE(scene.property("pelicanGraphSelectedNode").toString() ==
            QStringLiteral("left"));

    // A revision that no longer holds it drops the selection with it.
    auto without = connectedDocument();
    auto nodes = Json::array();
    for (const auto &node : without.at("nodes"))
        if (node.at("id") != "left") nodes.push_back(node);
    without["nodes"] = nodes;
    model.refresh();
    answer(model, without, visualCatalog());
    scene.repopulate();
    REQUIRE_FALSE(scene.selectedNode().has_value());
}

// N-5. The theme table is the one place the canvas takes colour from, and a
// scene follows a switch by drawing itself again: every item holds the brush
// it was built with, so nothing short of a repopulate can change it. The
// control is the repopulate: take the listener out of the scene's constructor
// and these colours stay at the theme the scene was built in.
TEST_CASE("The canvas draws in the theme's colours and follows a switch",
          "[devstudio][graph-canvas][scene][theme]") {
    (void)application();
    PelicanStudio::resetStudioThemeTable();
    PelicanStudio::resetSocketStyleDefaults();
    REQUIRE(PelicanStudio::setCurrentStudioTheme(QStringLiteral("dark")));

    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, connectedDocument(), visualCatalog());
    scene.setModel(&model);
    REQUIRE(model.enterInstance("left"));
    scene.repopulate();

    const auto body = [&](const QString &name) {
        auto *item = dynamic_cast<QGraphicsPathItem *>(nodeItem(scene, name));
        REQUIRE(item != nullptr);
        return item;
    };
    const auto anyEdge = [&] {
        const auto edges =
            itemsOfKind(scene, PelicanStudio::GraphCanvasEdgeItem);
        REQUIRE_FALSE(edges.empty());
        auto *item = dynamic_cast<QGraphicsPathItem *>(edges.front());
        REQUIRE(item != nullptr);
        return item;
    };

    const PelicanStudio::StudioTheme &dark = PelicanStudio::currentStudioTheme();
    REQUIRE(dark.name == QStringLiteral("dark"));
    REQUIRE(body(QStringLiteral("sum"))->brush().color() == dark.canvas.node_body);
    REQUIRE(body(QStringLiteral("sum"))->pen().color() == dark.canvas.node_border);
    REQUIRE(anyEdge()->pen().color() == dark.canvas.edge);
    // The socket's rim and the no-claim fill are the theme's; the type fills
    // in the table are identity and must not move with it.
    QGraphicsItem *socket = socketItem(scene, QStringLiteral("sum"),
                                       QStringLiteral("a"),
                                       QStringLiteral("input"));
    REQUIRE(socket != nullptr);
    auto *socket_path = dynamic_cast<QGraphicsPathItem *>(socket);
    REQUIRE(socket_path != nullptr);
    REQUIRE(socket_path->pen().color() == dark.canvas.node_border);

    const QColor dark_body = dark.canvas.node_body;
    const QColor dark_edge = dark.canvas.edge;

    // The switch alone -- nobody calls repopulate() here.
    REQUIRE(PelicanStudio::setCurrentStudioTheme(QStringLiteral("light")));
    const PelicanStudio::StudioTheme &light =
        PelicanStudio::currentStudioTheme();
    REQUIRE(light.name == QStringLiteral("light"));
    REQUIRE(light.canvas.node_body != dark_body);
    REQUIRE(light.canvas.edge != dark_edge);

    REQUIRE(body(QStringLiteral("sum"))->brush().color() == light.canvas.node_body);
    REQUIRE(body(QStringLiteral("sum"))->pen().color() == light.canvas.node_border);
    REQUIRE(anyEdge()->pen().color() == light.canvas.edge);
    socket_path = dynamic_cast<QGraphicsPathItem *>(
        socketItem(scene, QStringLiteral("sum"), QStringLiteral("a"),
                   QStringLiteral("input")));
    REQUIRE(socket_path != nullptr);
    REQUIRE(socket_path->pen().color() == light.canvas.node_border);
    REQUIRE(socket_path->brush().color() ==
            PelicanStudio::socketStyleFor(
                PelicanStudio::SocketTypeFacts{"pelican.value.scalar@1"}).fill);

    // And back, so the switch is not a one-way door.
    REQUIRE(PelicanStudio::setCurrentStudioTheme(QStringLiteral("dark")));
    REQUIRE(body(QStringLiteral("sum"))->brush().color() == dark_body);

    // A name the table does not carry changes nothing.
    REQUIRE_FALSE(PelicanStudio::setCurrentStudioTheme(QStringLiteral("solar")));
    REQUIRE(PelicanStudio::currentStudioThemeName() == QStringLiteral("dark"));
    REQUIRE(body(QStringLiteral("sum"))->brush().color() == dark_body);
}

// The table is data: a Studio whose resource will not parse still draws, in
// the built-in dark values, and still answers every drawing site.
TEST_CASE("A theme table that says nothing leaves the built-in dark values",
          "[devstudio][graph-canvas][scene][theme]") {
    (void)application();
    PelicanStudio::setStudioThemeTable("{ this is not json");
    REQUIRE(PelicanStudio::studioThemeNames() ==
            QStringList{QStringLiteral("dark")});
    const PelicanStudio::StudioTheme &theme =
        PelicanStudio::currentStudioTheme();
    REQUIRE(theme.canvas.node_body == QColor{QStringLiteral("#272e36")});
    REQUIRE(theme.domainTint(QStringLiteral("rendering")) ==
            QColor{QStringLiteral("#3f5d86")});
    // A domain and a source the table never named still answer.
    REQUIRE(theme.domainTint(QStringLiteral("nobody")) ==
            theme.domainTint(QStringLiteral("structural")));
    REQUIRE(theme.sourceTint(QStringLiteral("nobody")) ==
            theme.sourceTint(QStringLiteral("unknown")));
    PelicanStudio::resetStudioThemeTable();
    REQUIRE(PelicanStudio::setCurrentStudioTheme(
        PelicanStudio::defaultStudioThemeName()));
}

// N-4: "Auto layout" throws away the positions the user moved in this scope
// and places the scope again. It is session state, so it reaches neither the
// document nor the other scopes.
TEST_CASE("Auto layout drops the moved positions of this scope only",
          "[devstudio][graph-canvas][scene][layout]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, connectedDocument());
    scene.setModel(&model);

    QGraphicsItem *left = nodeItem(scene, QStringLiteral("left"));
    REQUIRE(left != nullptr);
    const QPointF laid_out = left->pos();
    left->setPos(laid_out + QPointF{240.0, 310.0});
    REQUIRE(nodeItem(scene, QStringLiteral("left"))->pos() != laid_out);

    // A refresh keeps it (N-1's promise, unchanged).
    model.refresh();
    answer(model, connectedDocument());
    scene.repopulate();
    REQUIRE(nodeItem(scene, QStringLiteral("left"))->pos() != laid_out);

    // Move a node inside a definition too, then come back out: auto layout in
    // the root must not touch what was moved in another scope.
    REQUIRE(model.enterInstance("left"));
    scene.repopulate();
    QGraphicsItem *sum = nodeItem(scene, QStringLiteral("sum"));
    REQUIRE(sum != nullptr);
    const QPointF definition_laid_out = sum->pos();
    const QPointF definition_moved = definition_laid_out + QPointF{70.0, 30.0};
    sum->setPos(definition_moved);
    REQUIRE(model.leaveScope());
    scene.repopulate();
    // N-6 changed what a drag costs: a place is kept now, so leaving the scope
    // it happened in put one `set_layout` on the wire, naming that scope and
    // nothing else. It is still not a change to the graph.
    {
        const auto written = takeEdit(model);
        REQUIRE(written.at("ops").size() == 1);
        CHECK(written.at("ops").at(0).at("op") == "set_layout");
        CHECK(written.at("ops").at(0).at("scope") == "root");
    }

    scene.autoLayout();
    REQUIRE(nodeItem(scene, QStringLiteral("left"))->pos() == laid_out);

    REQUIRE(model.enterInstance("left"));
    scene.repopulate();
    REQUIRE(nodeItem(scene, QStringLiteral("sum"))->pos() == definition_moved);
    scene.autoLayout();
    REQUIRE(nodeItem(scene, QStringLiteral("sum"))->pos() ==
            definition_laid_out);
}

// N-6 decision 1. The automatic placement used to be thrown away the moment a
// person touched either end of an edge -- the hole N-4 recorded. A control
// point is an absolute place the person put, so a node moving is exactly the
// case it exists for.
TEST_CASE("A control point is made by double-clicking an edge and survives a node drag",
          "[devstudio][graph-canvas][scene][reroute]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, connectedDocument());
    scene.setModel(&model);
    REQUIRE(model.canEdit());
    REQUIRE(scene.layout().reroutes.empty());

    QGraphicsPathItem *line =
        edgeItem(scene, QStringLiteral("clock"), QStringLiteral("value"),
                 QStringLiteral("left"), QStringLiteral("gain"));
    REQUIRE(line != nullptr);
    const QPointF middle = line->path().pointAtPercent(0.5);
    click(scene, middle, QEvent::GraphicsSceneMouseDoubleClick);

    REQUIRE(scene.layout().reroutes.size() == 1);
    REQUIRE(scene.layout().reroutes.front().points.size() == 1);
    CHECK(scene.layout().reroutes.front().edge.node == "left");
    CHECK(scene.layout().reroutes.front().edge.port == "gain");
    const QPointF control = scene.layout().reroutes.front().points.front();
    // The canvas names it the way the automation surface does.
    QGraphicsItem *dot = controlItem(scene, PelicanStudio::GraphCanvasRerouteItem,
                                     QStringLiteral("left/gain/0"));
    REQUIRE(dot != nullptr);
    // And it is over the nodes, because a handle under one cannot be reached.
    CHECK(dot->zValue() > nodeItem(scene, QStringLiteral("left"))->zValue());
    // And the line is drawn through it.
    REQUIRE(edgeBends(scene, QStringLiteral("clock"), QStringLiteral("value"),
                      QStringLiteral("left"), QStringLiteral("gain"))
                .contains(control));

    // The drag. Before N-6 this threw the bend away; it is the mutation
    // control for the whole decision.
    QGraphicsItem *left = nodeItem(scene, QStringLiteral("left"));
    REQUIRE(left != nullptr);
    left->setPos(left->pos() + QPointF{40.0, 180.0});
    CHECK(edgeBends(scene, QStringLiteral("clock"), QStringLiteral("value"),
                    QStringLiteral("left"), QStringLiteral("gain"))
              .contains(control));

    // The canvas is ahead of the document between a gesture and its reply, and
    // it stays ahead: a rebuild in that window must not put the older placement
    // back over what was done since. (The scenario found this: a control point
    // added right after a placement had been saved vanished on the next draw.)
    scene.flushLayout();
    auto saved = connectedDocument();
    saved["layout"] = takeLayoutEdit(model);
    finishEdit(model, saved,
               Json{{"schema", "pelican.graph_catalog"},
                    {"version", 1},
                    {"modules", Json::array()}});
    scene.repopulate();
    REQUIRE(scene.layout().reroutes.size() == 1);
    REQUIRE(scene.insertReroute(
        PelicanStudio::GraphCanvasEdge{"clock", "value", "left", "gain"},
        control + QPointF{30.0, 10.0}));
    scene.repopulate();
    CHECK(scene.layout().reroutes.size() == 1);
    CHECK(scene.layout().reroutes.front().points.size() == 2);
    // And back to one, so what follows is about the one point it started with.
    scene.selectReroute(PelicanStudio::GraphCanvasEdgeKey{"left", "gain"}, 1, false);
    pressKey(scene, Qt::Key_Delete);
    REQUIRE(scene.layout().reroutes.size() == 1);
    REQUIRE(scene.layout().reroutes.front().points.size() == 1);

    // Picked and deleted, the control point goes and the line straightens.
    scene.selectReroute(PelicanStudio::GraphCanvasEdgeKey{"left", "gain"}, 0, false);
    REQUIRE(scene.selectedReroutes().size() == 1);
    pressKey(scene, Qt::Key_Delete);
    CHECK(scene.layout().reroutes.empty());
    CHECK(edgeBends(scene, QStringLiteral("clock"), QStringLiteral("value"),
                    QStringLiteral("left"), QStringLiteral("gain"))
              .isEmpty());
    // None of it was a change to the graph: a placement is not a document edit.
    for (const auto &request : model.takeRpcRequests()) {
        INFO("request " << request.method);
        if (request.method != "edit_graph_document") continue;
        REQUIRE(request.params.at("ops").at(0).at("op") == "set_layout");
    }
}

// N-6 decision 2. Two lines carried side by side through one place.
TEST_CASE("Two edges tie into one bundle, move together, and untie again",
          "[devstudio][graph-canvas][scene][tie]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, twoEdgeDocument());
    scene.setModel(&model);
    REQUIRE(scene.property("pelicanGraphEdgeCount").toULongLong() == 2);

    const PelicanStudio::GraphCanvasEdge gain{"clock", "value", "left", "gain"};
    const PelicanStudio::GraphCanvasEdge bias{"clock", "value", "left", "bias"};
    scene.selectEdge(gain, false);
    scene.selectEdge(bias, true);
    REQUIRE(scene.selectedEdges().size() == 2);
    REQUIRE(scene.tieSelection());
    REQUIRE(scene.layout().ties.size() == 1);
    REQUIRE(scene.layout().ties.front().edges.size() == 2);

    const auto bundled = [&] {
        return std::pair{
            edgeBends(scene, QStringLiteral("clock"), QStringLiteral("value"),
                      QStringLiteral("left"), QStringLiteral("gain")),
            edgeBends(scene, QStringLiteral("clock"), QStringLiteral("value"),
                      QStringLiteral("left"), QStringLiteral("bias"))};
    };
    auto [first, second] = bundled();
    // Two points each, so each line runs level across the band -- which is what
    // "parallel" means here -- and the two are one slot apart.
    REQUIRE(first.size() == 2);
    REQUIRE(second.size() == 2);
    CHECK(first[0].y() == first[1].y());
    CHECK(second[0].y() == second[1].y());
    CHECK(std::abs(second[0].y() - first[0].y()) ==
          PelicanStudio::GraphCanvasTieSpacing);
    CHECK(first[0].x() == second[0].x());

    // One drag moves the whole bundle.
    QGraphicsItem *bar =
        controlItem(scene, PelicanStudio::GraphCanvasTieItem, QStringLiteral("0"));
    REQUIRE(bar != nullptr);
    // A bundle's place is the middle of the lines it carries, and the middle
    // of a line is often over a node. A handle drawn under a node cannot be
    // taken hold of: the press reaches for the bundle and drags the node. So
    // the handles are over everything they may come to lie on. (The scenario
    // found this on the real window: the first drag of the bundle moved the
    // node that was covering it, and the bundle did not move at all.)
    CHECK(bar->zValue() > nodeItem(scene, QStringLiteral("left"))->zValue());
    // While a hand is on it the canvas says so, and the owner waits: a redraw
    // arriving mid-gesture would delete the item being held, and the drag would
    // then move nothing. (The scenario found this on the real window.)
    CHECK_FALSE(scene.dragging());
    {
        QGraphicsSceneMouseEvent press{QEvent::GraphicsSceneMousePress};
        press.setScenePos(bar->sceneBoundingRect().center());
        press.setButton(Qt::LeftButton);
        press.setButtons(Qt::LeftButton);
        QApplication::sendEvent(&scene, &press);
        CHECK(scene.dragging());
        QGraphicsSceneMouseEvent release{QEvent::GraphicsSceneMouseRelease};
        release.setScenePos(bar->sceneBoundingRect().center());
        release.setButton(Qt::LeftButton);
        QApplication::sendEvent(&scene, &release);
    }
    const QPointF shift{25.0, -35.0};
    bar->setPos(bar->pos() + shift);
    const auto [moved_first, moved_second] = bundled();
    CHECK(moved_first[0] == first[0] + shift);
    CHECK(moved_second[0] == second[0] + shift);

    // Double-clicking the band turns the order of the slots round.
    const auto order = scene.layout().ties.front().edges;
    click(scene, bar->sceneBoundingRect().center(),
          QEvent::GraphicsSceneMouseDoubleClick);
    REQUIRE(scene.layout().ties.size() == 1);
    CHECK(scene.layout().ties.front().edges.front() == order.back());

    // Untying leaves one control point per line, where its slot was.
    scene.selectTie(0, false);
    REQUIRE(scene.untieSelection());
    CHECK(scene.layout().ties.empty());
    REQUIRE(scene.layout().reroutes.size() == 2);
    for (const auto &reroute : scene.layout().reroutes)
        CHECK(reroute.points.size() == 1);
}

// N-6 decision 3. The placement is the document's, and the session table is
// only what answers for a node the document has never placed.
TEST_CASE("A drag is one placement edit, and the document is where it lives",
          "[devstudio][graph-canvas][scene][layout-persistence]") {
    (void)application();
    REQUIRE(GraphCanvasScene::GraphCanvasLayoutDebounceMs == 300);
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, connectedDocument());
    scene.setModel(&model);

    QGraphicsItem *left = nodeItem(scene, QStringLiteral("left"));
    REQUIRE(left != nullptr);
    const QPointF placed = left->pos() + QPointF{120.0, 90.0};
    // Three moves, which is what one drag is made of.
    left->setPos(left->pos() + QPointF{40.0, 30.0});
    left->setPos(left->pos() + QPointF{40.0, 30.0});
    left->setPos(placed);
    REQUIRE(scene.layoutPending());
    // Nothing has gone out yet: the wait is what makes a drag one edit.
    requireNoEdit(model);

    scene.flushLayout();
    const auto written = takeLayoutEdit(model);
    // Still marked: the canvas lets go of it when the document comes back
    // carrying the same placement, not when the request leaves.
    CHECK(scene.layoutPending());
    REQUIRE(written.at("nodes").contains("left"));
    CHECK(written.at("nodes").at("left").at("x") == placed.x());
    CHECK(written.at("nodes").at("left").at("y") == placed.y());

    // What the engine holds afterwards. A canvas that has never seen this
    // session -- so its session table is empty -- draws the node there.
    auto document = connectedDocument();
    document["layout"] = written;
    GraphCanvasModel reopened;
    GraphCanvasScene fresh;
    reopened.setConnected(true);
    answer(reopened, document);
    fresh.setModel(&reopened);
    REQUIRE(nodeItem(fresh, QStringLiteral("left")) != nullptr);
    CHECK(nodeItem(fresh, QStringLiteral("left"))->pos() == placed);
    CHECK(fresh.layout().nodes.contains("left"));
}

// N-6 decision 1 and 2, the automatic half: the lanes the placement kept come
// back as control points, and the ones running side by side as one bundle.
TEST_CASE("Auto layout keeps the lanes it chose, as control points and bundles",
          "[devstudio][graph-canvas][scene][layout][reroute]") {
    (void)application();

    SECTION("lines that share a lane come back as one bundle") {
        GraphCanvasModel model;
        GraphCanvasScene scene;
        model.setConnected(true);
        answer(model, bundleDocument());
        scene.setModel(&model);
        REQUIRE(scene.property("pelicanGraphNodeCount").toULongLong() == 3);
        REQUIRE(scene.property("pelicanGraphEdgeCount").toULongLong() == 4);
        // Nothing is kept until it is asked for.
        REQUIRE(scene.layout().reroutes.empty());
        REQUIRE(scene.layout().ties.empty());

        scene.autoLayout();
        const auto written = takeLayoutEdit(model);
        // Every node it placed is in the placement it wrote.
        CHECK(written.at("nodes").size() == 3);
        CHECK(written.at("ties").size() == 1);
        // The two dependencies that fly over the middle column share the lane
        // the placement kept, so they come back as one bundle of two; the two
        // edges that step a single column get nothing.
        CHECK(scene.layout().reroutes.empty());
        REQUIRE(scene.layout().ties.size() == 1);
        REQUIRE(scene.layout().ties.front().edges.size() == 2);
        CHECK(scene.layout().ties.front().edges.at(0) ==
              PelicanStudio::GraphCanvasEdgeKey{"gate", "a"});
        CHECK(scene.layout().ties.front().edges.at(1) ==
              PelicanStudio::GraphCanvasEdgeKey{"gate", "b"});

        // And they are drawn as two level lines, one slot apart.
        const auto first = edgeBends(scene, QStringLiteral("src"),
                                     QStringLiteral("value"),
                                     QStringLiteral("gate"), QStringLiteral("a"));
        const auto second = edgeBends(scene, QStringLiteral("src"),
                                      QStringLiteral("value"),
                                      QStringLiteral("gate"), QStringLiteral("b"));
        REQUIRE(first.size() == 2);
        REQUIRE(second.size() == 2);
        CHECK(first[0].y() == first[1].y());
        CHECK(second[0].y() - first[0].y() ==
              PelicanStudio::GraphCanvasTieSpacing);

        // Once it is kept, a node drag no longer straightens the lines.
        QGraphicsItem *step = nodeItem(scene, QStringLiteral("step"));
        REQUIRE(step != nullptr);
        step->setPos(step->pos() + QPointF{0.0, 260.0});
        CHECK(edgeBends(scene, QStringLiteral("src"), QStringLiteral("value"),
                        QStringLiteral("gate"), QStringLiteral("a")) == first);
    }

    SECTION("a lane with nobody beside it stays a control point of its own") {
        GraphCanvasModel model;
        GraphCanvasScene scene;
        model.setConnected(true);
        answer(model, flyoverDocument());
        scene.setModel(&model);
        REQUIRE(scene.property("pelicanGraphNodeCount").toULongLong() == 5);
        REQUIRE(scene.property("pelicanGraphEdgeCount").toULongLong() == 6);

        scene.autoLayout();
        const auto written = takeLayoutEdit(model);
        CHECK(written.at("nodes").size() == 5);
        // Here the two lanes run either side of the node they fly over, which
        // is further apart than a node's own pitch -- so they are two lines
        // going two ways, and the rule leaves them as two control points.
        INFO("reroutes " << scene.layout().reroutes.size() << " ties "
                         << scene.layout().ties.size());
        CHECK(scene.layout().ties.empty());
        REQUIRE(scene.layout().reroutes.size() == 2);
        for (const auto &reroute : scene.layout().reroutes) {
            // A lane is two points: the line runs level across the column it
            // flies over, and only the gaps between columns slope.
            CHECK(reroute.points.size() == 2);
            CHECK(reroute.points.at(0).y() == reroute.points.at(1).y());
        }
        for (const auto &edge : {std::pair{"gate_a", "a"}, std::pair{"gate_b", "a"}}) {
            const PelicanStudio::GraphCanvasEdgeKey key{edge.first, edge.second};
            INFO("edge " << key.node << "." << key.port);
            CHECK(std::ranges::any_of(
                scene.layout().reroutes,
                [&](const auto &reroute) { return reroute.edge == key; }));
        }
    }
}

// N-3 carried this over: the middle-button pan used to move the scroll bars,
// and a drawing that already fits the view has nothing to scroll -- so the
// canvas decided for the user where they were allowed to look. The pan moves
// the view itself now, and `center` (what `view_state` publishes) is the
// reading that answers for it either way.
TEST_CASE("A pan moves the view even when the whole drawing fits",
          "[devstudio][graph-canvas][scene][pan]") {
    (void)application();
    QGraphicsScene scene;
    // Far smaller than the viewport below: there is nothing to scroll.
    scene.addRect(QRectF{0.0, 0.0, 40.0, 30.0});
    scene.setSceneRect(QRectF{0.0, 0.0, 40.0, 30.0});
    PelicanStudio::NodeCanvasView view{&scene};
    view.resize(600, 400);
    view.show();
    QCoreApplication::processEvents();

    REQUIRE(std::abs(view.transform().m11() - 1.0) < 0.000001);
    REQUIRE(view.horizontalScrollBar()->minimum() ==
            view.horizontalScrollBar()->maximum());

    const QPointF before = view.mapToScene(view.viewport()->rect().center());
    view.panBy(QPointF{-120.0, -60.0});
    QCoreApplication::processEvents();
    const QPointF after = view.mapToScene(view.viewport()->rect().center());
    // Dragging left looks further right: the centre of the viewport lands on a
    // bigger scene x than it did.
    REQUIRE(after.x() > before.x() + 60.0);
    REQUIRE(after.y() > before.y() + 20.0);
    // And the zoom is untouched: a pan is not a zoom.
    REQUIRE(std::abs(view.transform().m11() - 1.0) < 0.000001);

    // A fit puts the drawing back and takes the room the pan opened away.
    view.fitView();
    QCoreApplication::processEvents();
    const QRectF visible = view.mapToScene(view.viewport()->rect()).boundingRect();
    REQUIRE(visible.contains(QRectF{0.0, 0.0, 40.0, 30.0}));
}

// Q-2. The canvas ground is a flat fill and a grid at a fixed scene pitch: the
// same picture every paint until the view moves. It used to be rebuilt -- up
// to ~180 freshly allocated QLineF -- on every exposed repaint, and a pan is
// made of repaints. The cached background and the reused line vector are what
// stopped that; antialiasing and the theme's colours are not what changed.
TEST_CASE("The canvas caches its ground without giving up antialiasing",
          "[devstudio][graph-canvas][scene][paint][q2]") {
    (void)application();
    QGraphicsScene scene;
    scene.addRect(QRectF{0.0, 0.0, 400.0, 300.0});
    PelicanStudio::NodeCanvasView view{&scene};
    view.resize(600, 400);
    view.show();
    QCoreApplication::processEvents();

    REQUIRE(view.cacheMode().testFlag(QGraphicsView::CacheBackground));
    REQUIRE(view.optimizationFlags().testFlag(
        QGraphicsView::DontSavePainterState));
    // Deliberately NOT set: the items here are stock Qt shapes whose
    // boundingRect is padded by the pen width alone, and an antialiased edge
    // reaches past that. Dropping Qt's margin would leave trails on the
    // partial repaints a pan is made of.
    REQUIRE_FALSE(view.optimizationFlags().testFlag(
        QGraphicsView::DontAdjustForAntialiasing));

    // The same ground, painted twice into two images, is the same ground: the
    // cache must not change what is drawn.
    const auto grab = [&] {
        QImage image{view.viewport()->size(),
                     QImage::Format_ARGB32_Premultiplied};
        image.fill(Qt::transparent);
        QPainter painter{&image};
        painter.setRenderHint(QPainter::Antialiasing, true);
        view.render(&painter);
        painter.end();
        return image;
    };
    const QImage first = grab();
    const QImage second = grab();
    REQUIRE(first == second);
    // Antialiasing is the panels' to ask for -- both do -- and nothing the
    // cache does takes it away again.
    view.setRenderHint(QPainter::Antialiasing, true);
    REQUIRE(view.renderHints().testFlag(QPainter::Antialiasing));
    REQUIRE(view.cacheMode().testFlag(QGraphicsView::CacheBackground));
}

// --------------------------------------------------------------------------
// AG-3b: the value band on an output row, the Live switch, the Timeline strip
// and the scrub -- driven the way the window drives them.

namespace {
Json ag3bNodeValues(const std::string &instance, const Json &nodes,
                    bool published = true) {
    return {{"instance", instance}, {"generation", 4}, {"frame", 12},
            {"published", published}, {"source", "rendering"},
            {"nodes", nodes}};
}

Json ag3bReportedNode(const std::string &node, const std::string &expanded,
                      const Json &outputs) {
    return {{"node", node}, {"expanded", expanded}, {"outputs", outputs}};
}

QGraphicsItem *bandItem(QGraphicsScene &scene, const QString &node,
                        const QString &port) {
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasValueBandItem} &&
            item->data(PelicanStudio::NodeGraphNameRole).toString() == node &&
            item->data(PelicanStudio::GraphCanvasPortRole).toString() == port &&
            !item->data(PelicanStudio::GraphCanvasValueTextRole)
                 .toString()
                 .isEmpty())
            return item;
    return nullptr;
}

// The transport a GraphCanvasWidget is given in this test: every seam of the
// capability records what it was asked for and answers with an id, so what the
// widget is allowed to send is exactly what the test can see.
struct RecordedCapability {
    struct Call {
        QString method;
        QString instance;
        double time = 0;
        // Q-5: what an edit carried, so a test can answer with the document
        // that edit produced.
        QByteArray body;
    };
    std::vector<Call> calls;
    qint64 next = 1;
    std::function<void(qint64, QByteArray)> deliver;

    PelicanStudio::GraphCanvasReadCapability capability(bool node_values = true) {
        PelicanStudio::GraphCanvasReadCapability seam;
        const auto note = [this](QString method, QString instance = {},
                                 double time = 0) {
            calls.push_back(Call{std::move(method), std::move(instance), time});
            return next++;
        };
        seam.ready = [](QObject *, auto handler) { handler(true, {}); };
        seam.requestGraphInstances = [note](QString *) {
            return note(QStringLiteral("get_graph_instances"));
        };
        seam.requestGraphCatalog = [note](QString *) {
            return note(QStringLiteral("get_graph_catalog"));
        };
        seam.requestGraphValues = [note](const QString &instance, QString *) {
            return note(QStringLiteral("get_graph_values"), instance);
        };
        if (node_values)
            seam.requestGraphNodeValues = [note](const QString &instance,
                                                 QString *) {
                return note(QStringLiteral("get_graph_node_values"), instance);
            };
        seam.requestStatus = [note](QString *) {
            return note(QStringLiteral("get_status"));
        };
        seam.requestSetTime = [note](double time, QString *) {
            return note(QStringLiteral("set_time"), {}, time);
        };
        seam.requestRenderFrame = [note](QString *) {
            return note(QStringLiteral("render_frame"));
        };
        seam.requestEditGraphDocument = [this](const QString &,
                                               const QByteArray &ops, QString *) {
            calls.push_back(Call{QStringLiteral("edit_graph_document"), {}, 0, ops});
            return next++;
        };
        seam.requestEditResult = [note](const QString &, QString *) {
            return note(QStringLiteral("get_edit_result"));
        };
        seam.result = [this](QObject *, auto handler) { deliver = handler; };
        seam.failure = [](QObject *, auto) {};
        return seam;
    }

    // The transport id the nth call of this method was given.
    [[nodiscard]] qint64 idOf(const QString &method, int occurrence = -1) const {
        qint64 found = 0;
        int seen = 0;
        for (std::size_t index = 0; index < calls.size(); ++index) {
            if (calls[index].method != method) continue;
            ++seen;
            if (occurrence < 0 || seen == occurrence + 1)
                found = static_cast<qint64>(index) + 1;
            if (occurrence >= 0 && seen == occurrence + 1) break;
        }
        return found;
    }
    [[nodiscard]] int countOf(const QString &method) const {
        return static_cast<int>(std::ranges::count(calls, method, &Call::method));
    }
};
} // namespace

TEST_CASE("An output row carries the published value in a definition scope",
          "[devstudio][graph-canvas][scene][ag3b]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    scene.setModel(&model);
    model.setConnected(true);
    answer(model, fixtureDocument(), visualCatalog());
    model.watchInstance("left");
    (void)model.takeRpcRequests();
    REQUIRE(model.selectScope("gain_curve"));
    for (const auto &request : model.takeRpcRequests())
        if (request.method == "get_graph_node_values")
            model.receiveRpcResult(
                request.request_id,
                ag3bNodeValues("left",
                               Json::array({
                                   ag3bReportedNode("sum", "left/sum",
                                                    {{"value", 7.5}}),
                                   // A rendering node: listed, with nothing on
                                   // its ports.
                                   ag3bReportedNode("buffer", "left/buffer",
                                                    Json::object())}))
                    .dump());
    scene.repopulate();

    QGraphicsItem *band = bandItem(scene, QStringLiteral("sum"),
                                   QStringLiteral("value"));
    REQUIRE(band != nullptr);
    // The band files the number itself, which is what `text` answers with.
    REQUIRE(band->data(PelicanStudio::GraphCanvasValueTextRole).toString() ==
            QStringLiteral("7.5"));
    // It sits on the left of the row, opposite the port's own name.
    REQUIRE(band->pos().x() < PelicanStudio::GraphCanvasNodeMinimumWidth / 2.0);
    // A node the report listed with no outputs has no band at all.
    REQUIRE(bandItem(scene, QStringLiteral("buffer"),
                     QStringLiteral("resource")) == nullptr);
}

TEST_CASE("The Graph tab's Live switch, Timeline and scrub go out through the capability",
          "[devstudio][graph-canvas][scene][ag3b]") {
    (void)application();
    RecordedCapability transport;
    PelicanStudio::GraphCanvasWidget widget{transport.capability()};
    widget.resize(900, 600);
    widget.show();
    QCoreApplication::processEvents();

    const auto answerRpc = [&](const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        REQUIRE(transport.deliver);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
    };
    answerRpc(QStringLiteral("get_graph_instances"), context(fixtureDocument()));
    answerRpc(QStringLiteral("get_graph_catalog"), visualCatalog());

    auto *live = widget.findChild<QCheckBox *>(
        QStringLiteral("pelican.graphCanvasLive"));
    auto *toggle = widget.findChild<QPushButton *>(
        QStringLiteral("pelican.graphTimelineToggle"));
    auto *summary = widget.findChild<QLabel *>(
        QStringLiteral("pelican.graphTimelineSummary"));
    auto *strip = widget.findChild<QWidget *>(
        QStringLiteral("pelican.graphTimeline"));
    REQUIRE(live != nullptr);
    REQUIRE(toggle != nullptr);
    REQUIRE(summary != nullptr);
    REQUIRE(strip != nullptr);

    // Naming the instance in "Values from" is what starts the observation, in
    // the root scope and -- through the same control -- inside a definition
    // body. The combo is driven, not the model: this is the user's path.
    auto *instances = widget.findChild<QComboBox *>(
        QStringLiteral("pelican.graphCanvasValues"));
    REQUIRE(instances != nullptr);
    REQUIRE(instances->findData(QStringLiteral("left")) > 0);
    instances->setCurrentIndex(instances->findData(QStringLiteral("left")));
    QCoreApplication::processEvents();
    REQUIRE(widget.model().watchedInstance() == "left");
    REQUIRE(transport.countOf(QStringLiteral("get_graph_node_values")) > 0);
    // Answer the pair that selection asked for. A question already in the air
    // is not asked again, so every section below starts with free slots.
    transport.deliver(transport.idOf(QStringLiteral("get_status")),
                      QByteArray::fromStdString(
                          Json{{"time", 0.25}, {"frame", 1},
                               {"animation", {{"available", false}}}}
                              .dump()));
    transport.deliver(
        transport.idOf(QStringLiteral("get_graph_node_values")),
        QByteArray::fromStdString(
            ag3bNodeValues("", Json::array({ag3bReportedNode(
                                   "clock", "clock", {{"value", 1.0}})}))
                .dump()));

    // The strip follows the selection, and a strip that has watched nothing
    // says so rather than drawing an empty box.
    widget.scene().selectNode("left");
    QCoreApplication::processEvents();
    REQUIRE(summary->text().contains(QStringLiteral("left")));

    SECTION("Live is what turns the periodic read on") {
        REQUIRE_FALSE(widget.model().live());
        const int before = transport.countOf(QStringLiteral("get_status"));
        live->click();
        QCoreApplication::processEvents();
        REQUIRE(widget.model().live());
        REQUIRE(transport.countOf(QStringLiteral("get_status")) > before);
        live->click();
        QCoreApplication::processEvents();
        REQUIRE_FALSE(widget.model().live());
    }

    SECTION("the Timeline is folded away until it is asked for") {
        auto *panel = strip->parentWidget();
        REQUIRE(panel != nullptr);
        REQUIRE_FALSE(panel->isVisible());
        toggle->click();
        QCoreApplication::processEvents();
        REQUIRE(panel->isVisible());
    }

    SECTION("a drag on the Timeline is set_time then one drawn frame") {
        // Two readings, so the strip has a span to map an x onto.
        widget.scene().selectNode("clock");
        QCoreApplication::processEvents();
        live->click();
        QCoreApplication::processEvents();
        for (int tick = 0; tick < 2; ++tick) {
            widget.poll();
            const auto status = transport.idOf(QStringLiteral("get_status"));
            const auto values =
                transport.idOf(QStringLiteral("get_graph_node_values"));
            transport.deliver(
                status,
                QByteArray::fromStdString(
                    Json{{"time", 0.5 * (tick + 1)}, {"frame", tick},
                         {"animation", {{"available", false}}}}
                        .dump()));
            transport.deliver(
                values,
                QByteArray::fromStdString(
                    ag3bNodeValues("", Json::array({ag3bReportedNode(
                                           "clock", "clock",
                                           {{"value", 1.0 + tick}})}))
                        .dump()));
        }
        REQUIRE(widget.model().timelineSamples().size() == 2);
        toggle->click();
        QCoreApplication::processEvents();

        const int before_time = transport.countOf(QStringLiteral("set_time"));
        const int before_frame = transport.countOf(QStringLiteral("render_frame"));
        QMouseEvent press{QEvent::MouseButtonPress,
                          QPointF{strip->width() / 2.0, 20.0},
                          strip->mapToGlobal(QPointF{strip->width() / 2.0, 20.0}),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier};
        QApplication::sendEvent(strip, &press);
        QCoreApplication::processEvents();

        REQUIRE(transport.countOf(QStringLiteral("set_time")) == before_time + 1);
        REQUIRE(transport.countOf(QStringLiteral("render_frame")) ==
                before_frame + 1);
        // In that order, and nothing between them.
        const auto &calls = transport.calls;
        std::size_t index = calls.size();
        while (index > 0 && calls[index - 1].method != QLatin1String("set_time"))
            --index;
        REQUIRE(index > 0);
        REQUIRE(calls[index].method == QLatin1String("render_frame"));
        // The time it asked for is inside the window the strip is showing.
        REQUIRE(calls[index - 1].time >= 0.5);
        REQUIRE(calls[index - 1].time <= 1.0);
    }
}

TEST_CASE("Without the node values seam the canvas says which method it cannot issue",
          "[devstudio][graph-canvas][scene][ag3b]") {
    (void)application();
    RecordedCapability transport;
    // The control for the observation: the same widget, with that one seam of
    // the capability left unfilled.
    PelicanStudio::GraphCanvasWidget widget{transport.capability(false)};
    widget.resize(900, 600);
    widget.show();
    QCoreApplication::processEvents();

    const auto answerRpc = [&](const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
    };
    answerRpc(QStringLiteral("get_graph_instances"), context(fixtureDocument()));
    answerRpc(QStringLiteral("get_graph_catalog"), visualCatalog());
    auto *instances = widget.findChild<QComboBox *>(
        QStringLiteral("pelican.graphCanvasValues"));
    REQUIRE(instances != nullptr);
    instances->setCurrentIndex(instances->findData(QStringLiteral("left")));
    QCoreApplication::processEvents();

    REQUIRE(transport.countOf(QStringLiteral("get_graph_node_values")) == 0);
    auto *notice = widget.findChild<QLabel *>(
        QStringLiteral("pelican.graphCanvasNotice"));
    REQUIRE(notice != nullptr);
    // By name: a seam that is not there is reported as the method that could
    // not leave, not as a blank overlay.
    REQUIRE(notice->text().contains(QStringLiteral("get_graph_node_values")));
    REQUIRE_FALSE(widget.model().nodeValuesReceived());
}

// N-8. `set_layout` replaces the whole `layout` object, so the Frame Plan
// canvas's own placement has to travel with this canvas's. It hands its member
// over; one debounce, one write, both halves -- and a member this canvas has
// never heard of survives a drag rather than being deleted by it.
TEST_CASE("One set_layout carries both this canvas's placement and the Frame Plan's",
          "[devstudio][graph-canvas][scene][layout-persistence][n8]") {
    (void)application();
    // A document that already carries the other panel's member.
    auto document = connectedDocument();
    const Json frame_plan{
        {"main_render",
         {{"nodes", {{"ssao_pass", {{"x", 12.0}, {"y", 34.0}}}}}}}};
    document["layout"] = Json{{"nodes", Json::object()},
                              {"reroutes", Json::array()},
                              {"ties", Json::array()},
                              {"frame_plan", frame_plan}};

    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, document);
    scene.setModel(&model);
    // It was read back, untouched, without this canvas understanding it.
    CHECK(scene.layoutMember("frame_plan") == frame_plan);

    QGraphicsItem *left = nodeItem(scene, QStringLiteral("left"));
    REQUIRE(left != nullptr);
    const QPointF placed = left->pos() + QPointF{80.0, 60.0};
    left->setPos(placed);
    REQUIRE(scene.layoutPending());
    scene.flushLayout();
    {
        const auto written = takeLayoutEdit(model);
        REQUIRE(written.at("nodes").contains("left"));
        CHECK(written.at("nodes").at("left").at("x") == placed.x());
        // The control for the whole arrangement: a drag on THIS canvas did not
        // delete the other panel's member, because the one write carries both.
        CHECK(written.at("frame_plan") == frame_plan);
    }

    // The other direction, on a canvas of its own because one model carries one
    // edit at a time: the Frame Plan moves something, and the one write that
    // goes out still carries this canvas's placement with it.
    const Json moved{
        {"main_render",
         {{"nodes", {{"ssao_pass", {{"x", 90.0}, {"y", -5.0}}}}}}}};
    GraphCanvasModel second;
    GraphCanvasScene fresh;
    second.setConnected(true);
    answer(second, document);
    fresh.setModel(&second);
    QGraphicsItem *other = nodeItem(fresh, QStringLiteral("left"));
    REQUIRE(other != nullptr);
    const QPointF elsewhere = other->pos() + QPointF{25.0, 15.0};
    other->setPos(elsewhere);
    fresh.setLayoutMember("frame_plan", moved);
    REQUIRE(fresh.layoutPending());
    fresh.flushLayout();
    {
        const auto written = takeLayoutEdit(second);
        CHECK(written.at("frame_plan") == moved);
        REQUIRE(written.at("nodes").contains("left"));
        CHECK(written.at("nodes").at("left").at("x") == elsewhere.x());
    }
    CHECK(fresh.layoutMember("frame_plan") == moved);
}

// Q-5. A drag's placement goes out after the debounce and comes back as a new
// read of the document. That read used to rebuild the whole scene a moment
// after the person had gone on to pick something. Driven through the widget,
// because whether to keep the scene or rebuild it is the tab's decision.
TEST_CASE("Q-5 a placement save coming back keeps the scene and what is picked on it",
          "[devstudio][graph-canvas][scene][layout-persistence][q5]") {
    (void)application();
    RecordedCapability transport;
    PelicanStudio::GraphCanvasWidget widget{transport.capability()};
    widget.resize(900, 600);
    widget.show();
    QCoreApplication::processEvents();

    const auto deliver = [&](const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        REQUIRE(transport.deliver);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    };
    auto document = twoEdgeDocument();
    deliver(QStringLiteral("get_graph_instances"), context(document));
    deliver(QStringLiteral("get_graph_catalog"), visualCatalog());

    auto &scene = widget.scene();
    auto *notice =
        widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasNotice"));
    auto *tie = widget.findChild<QAction *>(
        QStringLiteral("pelican.graphCanvas.tieTogether"));
    REQUIRE(notice != nullptr);
    REQUIRE(tie != nullptr);

    // One save the way the engine answers it: accepted, then committed. The
    // read of the document that follows is left to the caller, so what the
    // canvas does in between can be looked at.
    const auto save = [&] {
        scene.flushLayout();
        QCoreApplication::processEvents();
        const qint64 edit = transport.idOf(QStringLiteral("edit_graph_document"));
        REQUIRE(edit != 0);
        const auto ops = Json::parse(
            transport.calls[static_cast<std::size_t>(edit - 1)].body.toStdString());
        REQUIRE(ops.size() == 1);
        REQUIRE(ops.at(0).at("op") == "set_layout");
        document["layout"] = ops.at(0).at("layout");
        deliver(QStringLiteral("edit_graph_document"),
                Json{{"status", "accepted"}, {"ticket", "q5"}});
        deliver(QStringLiteral("get_edit_result"),
                Json{{"status", "committed"}, {"committed", true}});
    };

    // A control point on one line, saved, so every kind of pick is there.
    const PelicanStudio::GraphCanvasEdge gain{"clock", "value", "left", "gain"};
    const PelicanStudio::GraphCanvasEdge bias{"clock", "value", "left", "bias"};
    QGraphicsPathItem *bias_line =
        edgeItem(scene, QStringLiteral("clock"), QStringLiteral("value"),
                 QStringLiteral("left"), QStringLiteral("bias"));
    REQUIRE(bias_line != nullptr);
    REQUIRE(scene.insertReroute(bias, bias_line->path().pointAtPercent(0.5)));
    save();
    deliver(QStringLiteral("get_graph_instances"), context(document));
    REQUIRE_FALSE(scene.layoutPending());
    REQUIRE(notice->text() == QStringLiteral("Graph change saved."));

    // Decision 2. The drag: the notice no longer says the last save was the
    // end of it, so a wait for "saved" waits for this change, not the last.
    QGraphicsItem *left = nodeItem(scene, QStringLiteral("left"));
    REQUIRE(left != nullptr);
    left->setPos(left->pos() + QPointF{60.0, 140.0});
    CHECK(scene.placementUnsent());
    CHECK(notice->text() == QStringLiteral("Saving the graph change..."));

    // What the person does next: the control point, and a second line.
    scene.selectReroute(PelicanStudio::GraphCanvasEdgeKey{"left", "bias"}, 0, false);
    scene.selectEdge(gain, true);
    REQUIRE(tie->isEnabled());
    // Items made by this draw carry a mark; a rebuild makes new ones without it.
    constexpr int Marked = Qt::UserRole + 905;
    for (QGraphicsItem *item : scene.items()) item->setData(Marked, true);

    save();
    CHECK_FALSE(scene.placementUnsent());
    CHECK(notice->text() == QStringLiteral("Graph change saved."));
    const auto picked = [&] {
        CHECK(scene.selectedReroutes().size() == 1);
        CHECK(scene.selectedEdges().size() == 1);
        CHECK(tie->isEnabled());
    };

    SECTION("the document places what was sent: the scene is kept") {
        deliver(QStringLiteral("get_graph_instances"), context(document));
        CHECK_FALSE(scene.layoutPending());
        CHECK(nodeItem(scene, QStringLiteral("left"))->data(Marked).toBool());
        picked();
    }

    SECTION("something else changed with it: redrawn, and the pick carried across") {
        auto changed = document;
        for (auto &node : changed.at("nodes"))
            if (node.at("id") == "clock") node["label"] = "Clock";
        deliver(QStringLiteral("get_graph_instances"), context(changed));
        CHECK_FALSE(scene.layoutPending());
        CHECK_FALSE(nodeItem(scene, QStringLiteral("left"))->data(Marked).toBool());
        picked();
    }
}

// Q-8. The ui.* sinks GP-3 added to the catalog: the palette offers them,
// places one the engine would load, and the node's `node` field lists the
// paths of the project's UI document instead of asking for one to be typed.
namespace {
struct UiProject {
    std::filesystem::path root;

    UiProject() {
        root = std::filesystem::temp_directory_path() /
               ("pelican_q8_ui_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        std::filesystem::create_directories(root / "ui");
        std::ofstream{root / "project.json"} << R"json({
 "schema":"pelican.project","version":1,"name":"q8_ui",
 "basic_config":{"ui_config_json":"ui/hud.json"}})json";
        // Document order is list order: a panel, a label and two gauges.
        std::ofstream{root / "ui" / "hud.json"} << R"json({
 "schema":"pelican.ui","version":1,"key":"q8_hud","revision":"q8",
 "root":{"id":"root","type":"panel","children":[
  {"id":"hud","type":"panel","children":[
   {"id":"label","type":"label","text":"HI"},
   {"id":"meter","type":"gauge"},
   {"id":"fuel","type":"gauge"}]}]}})json";
    }
    ~UiProject() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

// The catalog the engine answers get_graph_catalog with, not a hand-built one:
// whether the palette offers the ui.* sinks is a question about that reply.
Json engineCatalog() {
    return Json::parse(Pelican::makeBuiltinGraphModuleRegistry().catalog().dump());
}

Json uiSinkDocument() {
    return {{"nodes", Json::array({
                {{"id", "clock"}, {"op", "value.input"},
                 {"parameters", {{"name", "time"}}}, {"inputs", Json::object()},
                 {"outputs", Json::array({"value"})}},
                {{"id", "meter_sink"}, {"op", "ui.value"},
                 {"parameters", {{"node", "root/hud/meter"}}},
                 {"inputs", {{"value", {{"node", "clock"}, {"port", "value"}}}}},
                 {"outputs", Json::array()}},
                {{"id", "label_sink"}, {"op", "ui.visible"},
                 {"parameters", {{"node", "root/hud/label"}}},
                 {"inputs", {{"value", {{"node", "clock"}, {"port", "value"}}}}},
                 {"outputs", Json::array()}},
            })},
            {"definitions", Json::object()}};
}

QStringList comboItems(const QComboBox &combo) {
    QStringList items;
    for (int index = 0; index < combo.count(); ++index)
        items.push_back(combo.itemText(index));
    return items;
}
} // namespace

TEST_CASE("Q-8 a ui sink's node field lists the UI document, and the palette "
          "places one standing on it",
          "[devstudio][graph-canvas][scene][graph-edit][q8]") {
    (void)application();
    const UiProject project;
    RecordedCapability transport;
    PelicanStudio::GraphCanvasWidget widget{transport.capability()};
    widget.setProjectRoot(project.root);
    widget.resize(900, 600);
    widget.show();
    QCoreApplication::processEvents();

    const auto deliver = [&](const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        REQUIRE(transport.deliver);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    };
    deliver(QStringLiteral("get_graph_instances"), context(uiSinkDocument()));
    deliver(QStringLiteral("get_graph_catalog"), engineCatalog());
    auto &model = widget.model();
    REQUIRE(model.canEdit());

    auto *row = widget.findChild<QWidget *>(
        QStringLiteral("pelican.graphCanvasUiNodeRow"));
    auto *field =
        widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasUiNode"));
    REQUIRE(row != nullptr);
    REQUIRE(field != nullptr);
    REQUIRE(field->isEditable());

    const auto lastOps = [&] {
        const qint64 edit = transport.idOf(QStringLiteral("edit_graph_document"));
        REQUIRE(edit != 0);
        return Json::parse(
            transport.calls[static_cast<std::size_t>(edit - 1)].body.toStdString());
    };

    SECTION("the palette offers both sinks from the engine's own catalog") {
        const auto offered = model.paletteOperations("ui.");
        const auto has = [&](const char *op) {
            return std::ranges::any_of(offered, [&](const auto &entry) {
                return entry.op == op && entry.sink && !entry.structural;
            });
        };
        REQUIRE(has("ui.value"));
        REQUIRE(has("ui.visible"));
    }

    SECTION("a placed ui.value writes the first gauge and stands on its own value") {
        REQUIRE(widget.scene().addFromPalette("ui.value"));
        QCoreApplication::processEvents();
        const auto ops = lastOps();
        REQUIRE(ops.size() == 2);
        REQUIRE(ops.at(0).at("node").at("op") == "value.constant");
        REQUIRE(ops.at(1) ==
                Json{{"op", "add_node"},
                     {"scope", "root"},
                     {"node", {{"id", "ui_value"},
                               {"op", "ui.value"},
                               {"outputs", Json::array()},
                               {"inputs", {{"value", {{"node", "ui_value__value"},
                                                      {"port", "value"}}}}},
                               {"parameters", {{"node", "root/hud/meter"}}}}}});
    }

    SECTION("a placed ui.visible writes the first node of the document") {
        REQUIRE(widget.scene().addFromPalette("ui.visible"));
        QCoreApplication::processEvents();
        const auto ops = lastOps();
        REQUIRE(ops.back().at("node").at("parameters") == Json{{"node", "root"}});
    }

    SECTION("ui.value lists the gauges, ui.visible every node, and a pick is one edit") {
        widget.scene().selectNode("meter_sink");
        QCoreApplication::processEvents();
        REQUIRE(row->isVisible());
        // Only a gauge takes a value; anything else would be refused by name.
        REQUIRE(comboItems(*field) ==
                QStringList{QStringLiteral("root/hud/meter"),
                            QStringLiteral("root/hud/fuel")});
        REQUIRE(field->currentText() == QStringLiteral("root/hud/meter"));

        widget.scene().selectNode("label_sink");
        QCoreApplication::processEvents();
        REQUIRE(comboItems(*field) ==
                QStringList{QStringLiteral("root"), QStringLiteral("root/hud"),
                            QStringLiteral("root/hud/label"),
                            QStringLiteral("root/hud/meter"),
                            QStringLiteral("root/hud/fuel")});
        REQUIRE(field->currentText() == QStringLiteral("root/hud/label"));

        const int before = transport.countOf(QStringLiteral("edit_graph_document"));
        const int fuel = field->findText(QStringLiteral("root/hud/fuel"));
        REQUIRE(fuel >= 0);
        field->setCurrentIndex(fuel);
        emit field->activated(fuel);
        QCoreApplication::processEvents();
        REQUIRE(transport.countOf(QStringLiteral("edit_graph_document")) == before + 1);
        REQUIRE(lastOps() == Json::array({Json{{"op", "set_node_parameters"},
                                               {"scope", "root"},
                                               {"node", "label_sink"},
                                               {"parameters",
                                                {{"node", "root/hud/fuel"}}}}}));
    }

    SECTION("a typed path goes out as typed; the engine is the one that refuses it") {
        widget.scene().selectNode("label_sink");
        QCoreApplication::processEvents();
        field->lineEdit()->setText(QStringLiteral("root/hud/nowhere"));
        emit field->lineEdit()->returnPressed();
        QCoreApplication::processEvents();
        REQUIRE(lastOps().at(0).at("parameters") ==
                Json{{"node", "root/hud/nowhere"}});
        // It is not added to the list the document gave.
        REQUIRE(field->findText(QStringLiteral("root/hud/nowhere")) < 0);
    }

    SECTION("the field is there only for a ui sink") {
        widget.scene().selectNode("clock");
        QCoreApplication::processEvents();
        REQUIRE_FALSE(row->isVisible());
    }
}

TEST_CASE("Q-8 without a UI document the field says why and the palette "
          "leaves the path out",
          "[devstudio][graph-canvas][scene][graph-edit][q8]") {
    (void)application();
    RecordedCapability transport;
    PelicanStudio::GraphCanvasWidget widget{transport.capability()};
    widget.resize(900, 600);
    widget.show();
    QCoreApplication::processEvents();
    transport.deliver(transport.idOf(QStringLiteral("get_graph_instances")),
                      QByteArray::fromStdString(context(uiSinkDocument()).dump()));
    transport.deliver(transport.idOf(QStringLiteral("get_graph_catalog")),
                      QByteArray::fromStdString(engineCatalog().dump()));
    QCoreApplication::processEvents();

    widget.scene().selectNode("meter_sink");
    QCoreApplication::processEvents();
    auto *field =
        widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasUiNode"));
    REQUIRE(field != nullptr);
    REQUIRE(field->count() == 0);
    REQUIRE(field->currentText() == QStringLiteral("root/hud/meter"));
    REQUIRE(field->lineEdit()->placeholderText().contains(
        QStringLiteral("no project is open")));

    REQUIRE(widget.scene().addFromPalette("ui.value"));
    QCoreApplication::processEvents();
    const qint64 edit = transport.idOf(QStringLiteral("edit_graph_document"));
    REQUIRE(edit != 0);
    const auto ops = Json::parse(
        transport.calls[static_cast<std::size_t>(edit - 1)].body.toStdString());
    REQUIRE_FALSE(ops.back().at("node").contains("parameters"));
}

TEST_CASE("Q-8 the UI document's paths are the engine's: ids joined from the root",
          "[devstudio][graph-canvas][q8]") {
    const UiProject project;
    const auto read = PelicanStudio::readProjectUiNodes(project.root);
    REQUIRE(read.error.empty());
    std::vector<std::string> paths;
    for (const auto &node : read.nodes) paths.push_back(node.path);
    REQUIRE(paths == std::vector<std::string>{"root", "root/hud", "root/hud/label",
                                              "root/hud/meter", "root/hud/fuel"});
    REQUIRE(read.nodes.at(3).type == "gauge");

    // A project that names no UI document says so rather than listing nothing.
    std::ofstream{project.root / "project.json"}
        << R"json({"schema":"pelican.project","version":1,"name":"q8_none","basic_config":{}})json";
    const auto none = PelicanStudio::readProjectUiNodes(project.root);
    REQUIRE(none.nodes.empty());
    REQUIRE(none.error.find("ui_config_json") != std::string::npos);
}

// Q-10. AU-1 gave audio.playback a position input, the integrated time map
// (source_begin / source_end / speed / delay as ports) and three parameters
// that are no port at all: the sound, the bus and the wrap. The ports come
// from the catalog like any other; the three parameters are fields.
namespace {
struct AudioProject {
    std::filesystem::path root;

    AudioProject() {
        root = std::filesystem::temp_directory_path() /
               ("pelican_q10_audio_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        std::filesystem::create_directories(root / "sounds");
        std::filesystem::create_directories(root / "music");
        std::filesystem::create_directories(root / ".pelican");
        std::ofstream{root / "project.json"}
            << R"json({"schema":"pelican.project","version":1,"name":"q10_audio","basic_config":{}})json";
        // Only the name decides what is listed; the engine checks the bytes.
        std::ofstream{root / "sounds" / "hit.wav"} << "RIFF";
        std::ofstream{root / "sounds" / "LOUD.WAV"} << "RIFF";
        std::ofstream{root / "music" / "theme.ogg"} << "OggS";
        std::ofstream{root / "music" / "notes.txt"} << "not a sound";
        std::ofstream{root / "music" / "take.mp3"} << "ID3";
        // Studio's own folder is not the project's content.
        std::ofstream{root / ".pelican" / "cache.wav"} << "RIFF";
    }
    ~AudioProject() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

bool catalogHas(const Json &catalog, std::string_view op) {
    for (const auto &module : catalog.at("modules"))
        for (const auto &operation : module.at("operations"))
            if (operation.value("op", std::string{}) == op) return true;
    return false;
}

Json audioDocument() {
    const auto edge = [](const char *node, const char *port = "value") {
        return Json{{"node", node}, {"port", port}};
    };
    return {{"nodes", Json::array({
                {{"id", "now"}, {"op", "value.input"},
                 {"parameters", {{"name", "scene_time"}}}, {"inputs", Json::object()},
                 {"outputs", Json::array({"value"})}},
                {{"id", "cue"}, {"op", "event.playback"}, {"inputs", Json::object()},
                 {"outputs", Json::array({"events"})}},
                // The integrated form: started by its events, no wrap stored.
                {{"id", "voice"}, {"op", "audio.playback"},
                 {"parameters", {{"asset", "project://sounds/hit.wav"}}},
                 {"inputs", {{"time", edge("now")}, {"events", edge("cue", "events")}}},
                 {"outputs", Json::array({"voice"})}},
                // The decomposed form: follows its position, no events at all.
                {{"id", "follower"}, {"op", "audio.playback"},
                 {"parameters", {{"asset", "project://music/theme.ogg"}, {"bus", "bgm"}}},
                 {"inputs", {{"time", edge("now")}, {"position", edge("now")}}},
                 {"outputs", Json::array({"voice"})}},
            })},
            {"definitions", Json::object()}};
}
} // namespace

TEST_CASE("Q-10 audio.playback: its new ports come from the catalog, and the sound, "
          "bus and wrap are picked from lists",
          "[devstudio][graph-canvas][scene][graph-edit][q10]") {
    const auto catalog = engineCatalog();
    if (!catalogHas(catalog, "audio.playback"))
        SKIP("this build registers no pelican.audio module");
    (void)application();
    const AudioProject project;
    RecordedCapability transport;
    PelicanStudio::GraphCanvasWidget widget{transport.capability()};
    widget.setProjectRoot(project.root);
    widget.resize(900, 600);
    widget.show();
    QCoreApplication::processEvents();
    const auto deliver = [&](const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    };
    deliver(QStringLiteral("get_graph_instances"), context(audioDocument()));
    deliver(QStringLiteral("get_graph_catalog"), catalog);
    auto &model = widget.model();
    REQUIRE(model.canEdit());
    const auto lastOps = [&] {
        const qint64 edit = transport.idOf(QStringLiteral("edit_graph_document"));
        REQUIRE(edit != 0);
        return Json::parse(
            transport.calls[static_cast<std::size_t>(edit - 1)].body.toStdString());
    };
    auto *row = widget.findChild<QWidget *>(QStringLiteral("pelican.graphCanvasAudioRow"));
    auto *asset = widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasAudioAsset"));
    auto *bus = widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasAudioBus"));
    auto *wrap = widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasAudioWrap"));
    REQUIRE(row != nullptr);
    REQUIRE(asset != nullptr);
    REQUIRE(bus != nullptr);
    REQUIRE(wrap != nullptr);
    const QStringList sounds{QStringLiteral("project://music/theme.ogg"),
                             QStringLiteral("project://sounds/LOUD.WAV"),
                             QStringLiteral("project://sounds/hit.wav")};

    SECTION("every port AU-1 added is drawn, and events is not required") {
        const auto nodes = model.nodes();
        const auto voice = std::ranges::find(nodes, std::string{"voice"},
                                         &PelicanStudio::GraphCanvasNode::id);
        REQUIRE(voice != nodes.end());
        std::map<std::string, bool> required;
        for (const auto &port : voice->inputs) {
            CHECK(port.declared);
            required[port.port] = port.required;
        }
        CHECK(required == std::map<std::string, bool>{
                              {"delay", false}, {"events", false}, {"position", false},
                              {"source_begin", false}, {"source_end", false},
                              {"speed", false}, {"time", true}});
        // The node that follows its position leaves events unconnected, and
        // nothing on the canvas calls that a problem.
        CHECK(model.notice().kind != PelicanStudio::InspectorNoticeKind::Error);
    }

    SECTION("the palette offers it, and places it on the project's first sound") {
        const auto offered = model.paletteOperations("audio.");
        CHECK(std::ranges::any_of(offered, [](const auto &entry) {
            return entry.op == "audio.playback" && !entry.structural;
        }));
        REQUIRE(widget.scene().addFromPalette("audio.playback"));
        QCoreApplication::processEvents();
        const auto ops = lastOps();
        // Q-11: the scope already reads scene_time ("now"), and the new voice
        // reads it too -- no second clock, no constant. Only the required
        // `time` is wired: an optional events port is left open rather than
        // wired to something the user did not ask for.
        REQUIRE(ops.size() == 1);
        const auto &node = ops.at(0).at("node");
        CHECK(node.at("op") == "audio.playback");
        CHECK(node.at("parameters") == Json{{"asset", "project://music/theme.ogg"}});
        CHECK(node.at("inputs") == Json{{"time", {{"node", "now"}, {"port", "value"}}}});
    }

    SECTION("the fields show what is stored, and one pick is one edit") {
        widget.scene().selectNode("voice");
        QCoreApplication::processEvents();
        REQUIRE(row->isVisible());
        REQUIRE(asset->isEditable());
        CHECK(comboItems(*asset) == sounds);
        CHECK(asset->currentText() == QStringLiteral("project://sounds/hit.wav"));
        CHECK_FALSE(bus->isEditable());
        CHECK(comboItems(*bus) == QStringList{QStringLiteral("se"), QStringLiteral("bgm")});
        CHECK(bus->currentText() == QStringLiteral("se"));
        CHECK_FALSE(wrap->isEditable());
        CHECK(comboItems(*wrap) == QStringList{QStringLiteral("once"), QStringLiteral("repeat"),
                                               QStringLiteral("ping_pong")});
        CHECK(wrap->currentText() == QStringLiteral("once"));
        CHECK(wrap->isEnabled());

        // Picking the default already in effect sends nothing.
        emit wrap->activated(0);
        QCoreApplication::processEvents();
        CHECK(transport.countOf(QStringLiteral("edit_graph_document")) == 0);

        const int bgm = bus->findText(QStringLiteral("bgm"));
        bus->setCurrentIndex(bgm);
        emit bus->activated(bgm);
        QCoreApplication::processEvents();
        REQUIRE(transport.countOf(QStringLiteral("edit_graph_document")) == 1);
        CHECK(lastOps() == Json::array({Json{
                               {"op", "set_node_parameters"}, {"scope", "root"},
                               {"node", "voice"},
                               {"parameters", {{"asset", "project://sounds/hit.wav"},
                                               {"bus", "bgm"}}}}}));
    }

    SECTION("a sound can still be typed; it goes out as typed") {
        widget.scene().selectNode("voice");
        QCoreApplication::processEvents();
        asset->lineEdit()->setText(QStringLiteral("project://elsewhere/cue.ogg"));
        emit asset->lineEdit()->returnPressed();
        QCoreApplication::processEvents();
        CHECK(lastOps().at(0).at("parameters") ==
              Json{{"asset", "project://elsewhere/cue.ogg"}});
        CHECK(asset->findText(QStringLiteral("project://elsewhere/cue.ogg")) < 0);
    }

    SECTION("a voice that follows its position offers no wrap") {
        widget.scene().selectNode("follower");
        QCoreApplication::processEvents();
        REQUIRE(row->isVisible());
        CHECK(bus->currentText() == QStringLiteral("bgm"));
        CHECK(asset->currentText() == QStringLiteral("project://music/theme.ogg"));
        CHECK_FALSE(wrap->isEnabled());
    }

    SECTION("the fields are there only for audio.playback") {
        widget.scene().selectNode("now");
        QCoreApplication::processEvents();
        CHECK_FALSE(row->isVisible());
    }
}

// Q-11. A voice placed where no node reads scene_time brings its own, wired
// to `time` in the same edit: the engine loads it as placed, it starts on its
// owner's first frame (AU-3), and one Undo takes back the clock with it.
TEST_CASE("Q-11 a placed audio.playback reads a new scene clock, and one Undo "
          "takes both back",
          "[devstudio][graph-canvas][scene][graph-edit][graph-history][q11]") {
    const auto catalog = engineCatalog();
    if (!catalogHas(catalog, "audio.playback"))
        SKIP("this build registers no pelican.audio module");
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    // An app-time input is a clock, but not the one a voice reads its events
    // on; it is not taken for scene_time.
    const Json document{
        {"nodes", Json::array({{{"id", "clock"}, {"op", "value.input"},
                                {"parameters", {{"name", "time"}}},
                                {"inputs", Json::object()},
                                {"outputs", Json::array({"value"})}}})},
        {"definitions", Json::object()}};
    answer(model, document, catalog);
    scene.setModel(&model);
    model.setAudioAssets({"project://sounds/hit.wav"});
    REQUIRE(model.canEdit());

    REQUIRE(scene.addFromPalette("audio.playback"));
    const auto forward = takeEdit(model);
    const auto &ops = forward.at("ops");
    REQUIRE(ops.size() == 2);
    CHECK(ops.at(0) == Json{{"op", "add_node"}, {"scope", "root"},
                            {"node", {{"id", "scene_time"}, {"op", "value.input"},
                                      {"parameters", {{"name", "scene_time"}}},
                                      {"outputs", Json::array({"value"})}}}});
    const auto &voice = ops.at(1).at("node");
    CHECK(voice.at("id") == "audio_playback");
    CHECK(voice.at("inputs") ==
          Json{{"time", {{"node", "scene_time"}, {"port", "value"}}}});

    // The document the edit leaves is one the engine's graph validation takes:
    // every required input is connected, and typed as it expects.
    auto placed = document;
    placed["schema"] = "pelican.graph";
    placed["version"] = 1;
    placed["outputs"] = Json::object();
    for (const auto &op : ops) placed["nodes"].push_back(op.at("node"));
    const auto registry = Pelican::makeBuiltinGraphModuleRegistry();
    CHECK_NOTHROW(Pelican::partitionGraphDocument(placed, registry));

    // Commit it the way the engine does, then Undo once.
    const auto polls = model.takeRpcRequests();
    REQUIRE(polls.size() == 1);
    REQUIRE(polls.front().method == "get_edit_result");
    model.receiveRpcResult(polls.front().request_id,
        Json{{"status", "committed"}, {"source_digest", {{"hex", "source-b"}}}}.dump());
    const auto reads = model.takeRpcRequests();
    REQUIRE(reads.size() == 1);
    REQUIRE(reads.front().method == "get_graph_instances");
    auto snapshot = context(placed);
    snapshot["source_digest"]["hex"] = "source-b";
    model.receiveRpcResult(reads.front().request_id, snapshot.dump());
    scene.repopulate();
    REQUIRE(model.canUndo());
    REQUIRE(model.undo());
    const auto undo = takeEdit(model);
    REQUIRE(undo.at("ops").size() == 2);
    CHECK(undo.at("ops").at(0) ==
          Json{{"op", "remove_node"}, {"scope", "root"}, {"node", "audio_playback"}});
    CHECK(undo.at("ops").at(1) ==
          Json{{"op", "remove_node"}, {"scope", "root"}, {"node", "scene_time"}});
}

TEST_CASE("Q-10 the project's sounds: .wav and .ogg in any case, outside dot folders",
          "[devstudio][graph-canvas][q10]") {
    const AudioProject project;
    const auto read = PelicanStudio::readProjectAudioAssets(project.root);
    CHECK(read.error.empty());
    CHECK(read.assets == std::vector<std::string>{"project://music/theme.ogg",
                                                  "project://sounds/LOUD.WAV",
                                                  "project://sounds/hit.wav"});
    // No project, or a project without a sound: the field says why.
    CHECK(PelicanStudio::readProjectAudioAssets({}).error == "no project is open");
    const auto empty = std::filesystem::temp_directory_path() /
                       ("pelican_q10_silent_" +
                        std::to_string(std::chrono::steady_clock::now()
                                           .time_since_epoch()
                                           .count()));
    std::filesystem::create_directories(empty);
    const auto silent = PelicanStudio::readProjectAudioAssets(empty);
    std::error_code ignored;
    std::filesystem::remove_all(empty, ignored);
    CHECK(silent.assets.empty());
    CHECK(silent.error == "no .wav or .ogg file in the project");
}

// Q-12. "Open in Graph" on a task a render.compute node made: the Graph tab
// becomes the current one, the scope MG-3b's graph_node names is open --
// reached through the instance that placed the node, so the trail and the
// watched instance are that instance's -- and the node is picked and on
// screen. A node the document does not hold leaves everything where it was.
TEST_CASE("Q-12 Open in Graph switches to the Graph tab and picks the node in its scope",
          "[devstudio][graph-canvas][scene][q12]") {
    (void)application();
    RecordedCapability transport;
    QTabWidget tabs;
    auto *frame_plan = new QWidget;
    tabs.addTab(frame_plan, QStringLiteral("Frame Plan"));
    auto *graph = new PelicanStudio::GraphCanvasWidget{transport.capability()};
    tabs.addTab(graph, QStringLiteral("Graph"));
    tabs.resize(900, 600);
    tabs.show();
    QCoreApplication::processEvents();

    const auto deliver = [&](const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        REQUIRE(transport.deliver);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    };
    // The fixture, and one level deeper: `wrap` places `outer`, whose body
    // places gain_curve again as `inner`.
    auto document = fixtureDocument();
    document["definitions"]["outer"] = {
        {"nodes", Json::array({{{"id", "inner"}, {"op", "graph.instance"},
                                {"definition", "gain_curve"},
                                {"parameters", {{"gain", 1}, {"bias", 0}}}}})},
        {"outputs", Json::object()}};
    document["nodes"].push_back({{"id", "wrap"}, {"op", "graph.instance"},
                                 {"definition", "outer"},
                                 {"owner", {{"scene", "main"}, {"object", 42}}}});
    deliver(QStringLiteral("get_graph_instances"), context(document));
    deliver(QStringLiteral("get_graph_catalog"), visualCatalog());

    auto &model = graph->model();
    auto &scene = graph->scene();
    QGraphicsView *view = nullptr;
    for (auto *candidate : graph->findChildren<QGraphicsView *>())
        if (dynamic_cast<PelicanStudio::NodeCanvasView *>(candidate) != nullptr)
            view = candidate;
    REQUIRE(view != nullptr);
    REQUIRE(tabs.currentWidget() == frame_plan);
    REQUIRE(model.currentScope().empty());
    const auto on_screen = [&](const QString &node) {
        QGraphicsItem *item = nodeItem(scene, node);
        REQUIRE(item != nullptr);
        const QRectF visible =
            view->mapToScene(view->viewport()->rect()).boundingRect();
        return visible.contains(item->sceneBoundingRect().center());
    };

    SECTION("a node inside a root instance") {
        REQUIRE(PelicanStudio::openGraphNodeInTabs(
            tabs, *graph, QStringLiteral("gain_curve"), QStringLiteral("sum"),
            QStringLiteral("left")));
        CHECK(tabs.currentWidget() == graph);
        CHECK(model.currentScope() == "gain_curve");
        CHECK(model.breadcrumb() == std::vector<std::string>{"", "gain_curve"});
        CHECK(model.watchedInstance() == "left");
        REQUIRE(scene.selectedNode().has_value());
        CHECK(*scene.selectedNode() == "sum");
        CHECK(on_screen(QStringLiteral("sum")));
    }

    SECTION("a node two instances down: the trail runs through both") {
        REQUIRE(PelicanStudio::openGraphNodeInTabs(
            tabs, *graph, QStringLiteral("gain_curve"), QStringLiteral("buffer"),
            QStringLiteral("wrap/inner")));
        CHECK(tabs.currentWidget() == graph);
        CHECK(model.breadcrumb() ==
              std::vector<std::string>{"", "outer", "gain_curve"});
        CHECK(model.watchedInstance() == "wrap");
        REQUIRE(scene.selectedNode().has_value());
        CHECK(*scene.selectedNode() == "buffer");
        CHECK(on_screen(QStringLiteral("buffer")));
    }

    SECTION("a node of the root document") {
        REQUIRE(PelicanStudio::openGraphNodeInTabs(
            tabs, *graph, QStringLiteral("root"), QStringLiteral("clock"), {}));
        CHECK(tabs.currentWidget() == graph);
        CHECK(model.currentScope().empty());
        REQUIRE(scene.selectedNode().has_value());
        CHECK(*scene.selectedNode() == "clock");
        CHECK(on_screen(QStringLiteral("clock")));
    }

    SECTION("an instance path that does not lead there opens the scope itself") {
        REQUIRE(PelicanStudio::openGraphNodeInTabs(
            tabs, *graph, QStringLiteral("gain_curve"), QStringLiteral("sum"),
            QStringLiteral("gone")));
        CHECK(model.breadcrumb() == std::vector<std::string>{"", "gain_curve"});
        REQUIRE(scene.selectedNode().has_value());
        CHECK(*scene.selectedNode() == "sum");
    }

    SECTION("a node the document does not hold changes nothing") {
        CHECK_FALSE(PelicanStudio::openGraphNodeInTabs(
            tabs, *graph, QStringLiteral("gain_curve"), QStringLiteral("step"),
            QStringLiteral("left")));
        CHECK_FALSE(PelicanStudio::openGraphNodeInTabs(
            tabs, *graph, QStringLiteral("smooth"), QStringLiteral("sum"),
            QStringLiteral("left")));
        CHECK(tabs.currentWidget() == frame_plan);
        CHECK(model.currentScope().empty());
        CHECK_FALSE(scene.selectedNode().has_value());
    }
}

TEST_CASE("LP-4c the Loop fields and Add loop go out through the capability",
          "[devstudio][graph-canvas][scene][lp4c]") {
    (void)application();
    // A root loop over one body with a boundary port, next to nothing else.
    const Json body{{"nodes", Json::array({{{"id", "target"}, {"op", "graph.input"},
                                            {"default", 0.0}}})},
                    {"outputs", Json::object()}};
    Json document{{"definitions", {{"step", body}}},
                  {"nodes", Json::array({
                      {{"id", "steps"}, {"op", "graph.loop"},
                       {"parameters", {{"over", {{"range", {{"count", 2}}}}},
                                       {"branches", Json::array({{{"body", "step"}}})}}},
                       {"outputs", Json::array()}}})}};
    RecordedCapability transport;
    PelicanStudio::GraphCanvasWidget widget{transport.capability()};
    widget.resize(900, 600);
    widget.show();
    QCoreApplication::processEvents();
    const auto answerRpc = [&](const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        REQUIRE(transport.deliver);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
    };
    answerRpc(QStringLiteral("get_graph_instances"), context(document));
    answerRpc(QStringLiteral("get_graph_catalog"), visualCatalog());

    auto *fields = widget.findChild<QWidget *>(QStringLiteral("pelican.graphCanvasLoop"));
    REQUIRE(fields != nullptr);
    CHECK_FALSE(fields->isVisible());
    // Picking the loop shows its fields, read off its parameters.
    widget.scene().selectNode("steps");
    QCoreApplication::processEvents();
    CHECK(fields->isVisible());
    auto *count = widget.findChild<QSpinBox *>(QStringLiteral("pelican.graphCanvasLoop.count"));
    REQUIRE(count != nullptr);
    CHECK(count->value() == 2);
    REQUIRE(widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasLoop.branch0.body")) !=
            nullptr);
    // The parameters are fields, never a JSON pill on the canvas.
    for (QGraphicsItem *item : widget.scene().items())
        CHECK(item->data(PelicanStudio::NodeGraphItemKindRole).toString() !=
              QLatin1String{PelicanStudio::GraphCanvasValuePillItem});

    // One field, one edit: the count, sent when the field is left.
    count->setValue(4);
    Q_EMIT count->editingFinished();
    QCoreApplication::processEvents();
    REQUIRE(transport.countOf(QStringLiteral("edit_graph_document")) == 1);
    const auto sent = Json::parse(transport.calls.back().body.toStdString());
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].at("op") == "set_node_parameters");
    CHECK(sent[0].at("parameters").at("over") == Json{{"range", {{"count", 4}}}});
    CHECK(sent[0].at("parameters").at("branches") == Json::array({{{"body", "step"}}}));

    // Add loop, from its named entry: a new body and the loop, in one list.
    RecordedCapability second;
    PelicanStudio::GraphCanvasWidget other{second.capability()};
    other.show();
    QCoreApplication::processEvents();
    const auto answerSecond = [&](const QString &method, const Json &reply) {
        const qint64 id = second.idOf(method);
        REQUIRE(id != 0);
        second.deliver(id, QByteArray::fromStdString(reply.dump()));
    };
    answerSecond(QStringLiteral("get_graph_instances"), context(document));
    answerSecond(QStringLiteral("get_graph_catalog"), visualCatalog());
    QAction *add_loop = nullptr;
    for (QAction *action : other.actions())
        if (action->objectName() == QLatin1String{"pelican.graphCanvas.addLoop"}) add_loop = action;
    REQUIRE(add_loop != nullptr);
    REQUIRE(add_loop->isEnabled());
    add_loop->trigger();
    QCoreApplication::processEvents();
    REQUIRE(second.countOf(QStringLiteral("edit_graph_document")) == 1);
    const auto added = Json::parse(second.calls.back().body.toStdString());
    REQUIRE(added.size() == 2);
    CHECK(added[0] == Json{{"op", "add_definition"}, {"name", "loop_body"}});
    CHECK(added[1].at("node").at("op") == "graph.loop");
    CHECK(added[1].at("node").at("parameters").at("branches") ==
          Json::array({{{"body", "loop_body"}}}));
}
