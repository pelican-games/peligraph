#include "graphcanvasloopfields.hpp"
#include "graphcanvaswidget.hpp"
#include "graphcomposition.hpp"
#include "graphmodule.hpp"
#include "graphpartition.hpp"
#include "studiotheme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDialog>
#include <QFocusEvent>
#include <QFont>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QKeyEvent>
#include <QLineF>
#include <QRectF>
#include <QScrollBar>
#include <QTabWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QToolButton>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QWidget>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
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
    // GL-1: the plan read that follows every document read is not the edit.
    std::erase_if(requests, [](const PelicanStudio::InspectorRpcRequest &request) {
        return request.method == "get_frame_plan";
    });
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
        // GL-1d: the material_route_default it carried beside the ops.
        QString route_default;
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
        seam.requestEditGraphDocument = [this](const QString &, const QByteArray &ops,
                                               const QString &route_default, QString *) {
            calls.push_back(
                Call{QStringLiteral("edit_graph_document"), {}, 0, ops, route_default});
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
    // GL-1, decision 5: the root shows Render to begin with; the clock this
    // test draws lines from is a value node, shown the way a person does.
    auto *show_values =
        widget.findChild<QCheckBox *>(QStringLiteral("pelican.graphCanvasShow.value"));
    REQUIRE(show_values != nullptr);
    show_values->setChecked(true);
    QCoreApplication::processEvents();

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
    // GL-1d, decision 2: Over offers Draw items only to a loop a material
    // pass reads; this one's draws go nowhere, and the item says so.
    auto *over = widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasLoop.over"));
    REQUIRE(over != nullptr);
    const int draw_items = over->findData(QStringLiteral("draw_items"));
    REQUIRE(draw_items >= 0);
    auto *over_items = qobject_cast<QStandardItemModel *>(over->model());
    REQUIRE(over_items != nullptr);
    CHECK_FALSE(over_items->item(draw_items)->isEnabled());
    CHECK(over->itemData(draw_items, Qt::ToolTipRole).toString().contains(
        QStringLiteral("No material pass reads")));
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

namespace {
// GL-1d, decision 5: the palette on screen, if one is.
QDialog *paletteOnScreen() {
    for (QWidget *top : QApplication::topLevelWidgets())
        if (top->objectName() == QLatin1String{"pelican.graphCanvasPalette"} && top->isVisible())
            return qobject_cast<QDialog *>(top);
    return nullptr;
}

void sendKey(QWidget *to, int key, const QString &text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
    QCoreApplication::sendEvent(to, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
    QCoreApplication::sendEvent(to, &release);
}

// Clicks Add and runs `inside` while the modal palette it opens is up.
bool whilePaletteOpen(QPushButton &add, const std::function<void(QDialog &)> &inside) {
    bool ran = false;
    int ticks = 0;
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        if (QDialog *dialog = paletteOnScreen()) {
            poll.stop();
            ran = true;
            inside(*dialog);
            if (dialog->isVisible()) dialog->reject();
        } else if (++ticks > 2000) {
            poll.stop();
        }
    });
    poll.start();
    add.click();
    while (poll.isActive()) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QCoreApplication::processEvents();
    return ran;
}
} // namespace

TEST_CASE("GL-1d the Add node palette opens with the keyboard on its filter",
          "[devstudio][graph-canvas][scene][gl1d]") {
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
    auto *add = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasAddNode"));
    REQUIRE(add != nullptr);
    REQUIRE(add->isEnabled());

    // Esc is Cancel: typed into, walked, and left with nothing sent.
    QString focused;
    QString typed;
    int row_before = -1;
    int row_after = -1;
    bool closed = false;
    REQUIRE(whilePaletteOpen(*add, [&](QDialog &dialog) {
        QWidget *keyboard = dialog.focusWidget();
        REQUIRE(keyboard != nullptr);
        focused = keyboard->objectName();
        // Typed straight away, without a click: the filter has it.
        for (const QChar c : QStringLiteral("value."))
            sendKey(keyboard, c == QLatin1Char{'.'} ? Qt::Key_Period : Qt::Key_A + (c.unicode() - 'a'),
                    QString{c});
        auto *filter = dialog.findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasPaletteFilter"));
        auto *list = dialog.findChild<QListWidget *>(QStringLiteral("pelican.graphCanvasPaletteList"));
        REQUIRE(filter != nullptr);
        REQUIRE(list != nullptr);
        typed = filter->text();
        // The arrows walk the list; the keyboard stays on the filter.
        row_before = list->currentRow();
        sendKey(dialog.focusWidget(), Qt::Key_Down);
        row_after = list->currentRow();
        sendKey(dialog.focusWidget(), Qt::Key_Escape);
        closed = !dialog.isVisible();
    }));
    CHECK(focused == QStringLiteral("pelican.graphCanvasPaletteFilter"));
    CHECK(typed == QStringLiteral("value."));
    CHECK(row_before == 0);
    CHECK(row_after == 1);
    CHECK(closed);
    CHECK(transport.countOf(QStringLiteral("edit_graph_document")) == 0);

    // Enter is OK: the row the arrows reached is added.
    QString chosen;
    REQUIRE(whilePaletteOpen(*add, [&](QDialog &dialog) {
        auto *list = dialog.findChild<QListWidget *>(QStringLiteral("pelican.graphCanvasPaletteList"));
        REQUIRE(list != nullptr);
        REQUIRE(list->count() > 1);
        sendKey(dialog.focusWidget(), Qt::Key_Down);
        chosen = list->currentItem()->data(Qt::UserRole).toString();
        sendKey(dialog.focusWidget(), Qt::Key_Return);
        closed = !dialog.isVisible();
    }));
    CHECK(closed);
    REQUIRE(transport.countOf(QStringLiteral("edit_graph_document")) == 1);
    const auto sent = Json::parse(transport.calls.back().body.toStdString());
    CHECK(std::ranges::any_of(sent, [&](const Json &op) {
        return op.value("op", std::string{}) == "add_node" &&
               op.at("node").value("op", std::string{}) == chosen.toStdString();
    }));
}

TEST_CASE("LP-4e a draw_loop's Loop fields: the rows it takes, the rows it does not, and a "
          "branch's material",
          "[devstudio][graph-canvas][scene][lp4e]") {
    (void)application();
    const Json color{{"semantic", "pelican.render.color_signal@1"},
                     {"arguments", {{"reference", "scene"}, {"transfer", "linear"},
                                    {"range", "extended"}}}};
    const Json depth{{"semantic", "pelican.render.depth@1"},
                     {"arguments", {{"representation", "device"}, {"space", "projection"}}}};
    // decompose_material_pass's forward pass, one branch for one material
    // that draws nothing, then the else.
    Json document{
        {"definitions", {{"forward_opaque_draw",
            {{"nodes", Json::array({{{"id", "step"}, {"op", "draw.material"},
                                     {"parameters", Json::object()}, {"outputs", {"draws"}}}})},
             {"outputs", {{"draws", {{"node", "step"}, {"port", "draws"}}}}}}}}},
        {"nodes", Json::array({
            {{"id", "scene_color_ref"}, {"op", "render.target_ref"},
             {"parameters", {{"name", "scene_color"}, {"type", color}}}, {"outputs", {"resource"}}},
            {{"id", "scene_depth_ref"}, {"op", "render.target_ref"},
             {"parameters", {{"name", "scene_depth"}, {"type", depth}}}, {"outputs", {"resource"}}},
            {{"id", "forward_opaque_draws"}, {"op", "graph.loop"},
             {"parameters", {{"over", {{"draw_items", {{"contract", "forward_opaque_v1"}}}}},
                             {"order", {{"by", "phase_default"}}},
                             {"where", {{"tags", {{"exclude", {"decal"}}}}}},
                             {"branches", Json::array({
                                 {{"when", {{"material", "project://m.material.json#metal"}}},
                                  {"body", nullptr}},
                                 {{"body", "forward_opaque_draw"}}})}}},
             {"outputs", {"draws"}}},
            {{"id", "forward_opaque"}, {"op", "render.material_pass"},
             {"parameters", {{"material_contract", "forward_opaque_v1"}, {"pass_set", "main_render"}}},
             {"inputs", {{"color", {{"node", "scene_color_ref"}, {"port", "resource"}}},
                         {"depth", {{"node", "scene_depth_ref"}, {"port", "resource"}}},
                         {"draws", {{"node", "forward_opaque_draws"}, {"port", "draws"}}}}},
             {"outputs", Json::array()}}})}};
    Json catalog = visualCatalog();
    for (const char *op : {"render.target_ref", "render.material_pass", "draw.material",
                           "draw.variant", "draw.override"})
        catalog["modules"][1]["operations"].push_back(
            {{"op", op}, {"domain", "rendering"}, {"evaluation", "direct"}, {"sink", false},
             {"ports", "parameter_dependent"}});
    RecordedCapability transport;
    auto capability = transport.capability();
    capability.requestFramePlan = [&transport](QString *) {
        transport.calls.push_back({QStringLiteral("get_frame_plan")});
        return transport.next++;
    };
    PelicanStudio::GraphCanvasWidget widget{capability};
    widget.resize(1100, 700);
    widget.show();
    QCoreApplication::processEvents();
    const auto answerRpc = [&](const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        REQUIRE(transport.deliver);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    };
    answerRpc(QStringLiteral("get_graph_instances"), context(document));
    answerRpc(QStringLiteral("get_graph_catalog"), catalog);
    answerRpc(QStringLiteral("get_frame_plan"),
              Json{{"loops", Json::array({{{"node", "forward_opaque_draws"}, {"tier", "draw_loop"},
                                           {"pass", "forward_opaque"}, {"count", 2},
                                           {"elements", Json::array({
                                               {{"element", "project://m.material.json#metal"},
                                                {"index", 0}, {"branch", 0}},
                                               {{"element", "project://m.material.json#glass"},
                                                {"index", 1}, {"branch", 1}}})},
                                           {"unmatched_branches", Json::array()},
                                           {"total", 2}, {"offset", 0}, {"limit", 256}}})}});

    widget.scene().selectNode("forward_opaque_draws");
    QCoreApplication::processEvents();
    const auto shown = [&](const char *name) {
        auto *field = widget.findChild<QWidget *>(QString::fromLatin1(name));
        REQUIRE(field != nullptr);
        return field->isVisibleTo(&widget);
    };
    // The rows it does not take: the count, the order, the limit, the place
    // and an element `where`; the outputs and the carry.
    CHECK_FALSE(shown("pelican.graphCanvasLoop.count"));
    CHECK_FALSE(shown("pelican.graphCanvasLoop.order"));
    CHECK_FALSE(shown("pelican.graphCanvasLoop.limit"));
    CHECK_FALSE(shown("pelican.graphCanvasLoop.place"));
    CHECK_FALSE(shown("pelican.graphCanvasLoop.whereKind"));
    CHECK(widget.findChild<QWidget *>(QStringLiteral("pelican.graphCanvasLoop.output.draws")) == nullptr);
    CHECK(widget.findChild<QWidget *>(QStringLiteral("pelican.graphCanvasLoop.addCarry")) == nullptr);
    // The rows it does: its contract (read only) and a tag `where`.
    CHECK(shown("pelican.graphCanvasLoop.contract"));
    auto *contract = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasLoop.contract"));
    CHECK(contract->text() == QStringLiteral("forward_opaque_v1 (the pass forward_opaque)"));
    auto *exclude = widget.findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasLoop.whereTagsExclude"));
    REQUIRE(exclude != nullptr);
    CHECK(exclude->isVisibleTo(&widget));
    CHECK(exclude->text() == QStringLiteral("decal"));
    // The branch table: the material is a choice of the materials the report
    // names, and each branch lists the materials it took.
    auto *material = widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasLoop.branch0.material"));
    REQUIRE(material != nullptr);
    CHECK(material->currentText() == QStringLiteral("project://m.material.json#metal"));
    CHECK(material->findText(QStringLiteral("project://m.material.json#glass")) >= 0);
    CHECK(widget.findChild<QWidget *>(QStringLiteral("pelican.graphCanvasLoop.branch0.values")) == nullptr);
    auto *matched = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasLoop.branch1.matched"));
    REQUIRE(matched != nullptr);
    CHECK(matched->text() == QStringLiteral("materials: project://m.material.json#glass"));

    // One field, one edit: the tags, and a branch's material.
    exclude->setText(QStringLiteral("decal, sky"));
    Q_EMIT exclude->returnPressed();
    QCoreApplication::processEvents();
    REQUIRE(transport.countOf(QStringLiteral("edit_graph_document")) == 1);
    auto sent = Json::parse(transport.calls.back().body.toStdString());
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].at("parameters").at("where") == Json{{"tags", {{"exclude", {"decal", "sky"}}}}});
}

// Q-17. The Loop fields were built for a model that could edit when they were
// built, and an edit in flight does not rebuild them: nothing they show has
// changed until the document is read back. A body picked in between reached a
// model that could only refuse, and the choice was lost without a word -- the
// row still said "(nothing)" while the document kept its body. Each step of
// the edit's round trip is held here by not answering it.
TEST_CASE("Q-17 a branch's body picked while an edit is in flight is sent, or the row cannot be "
          "picked in",
          "[devstudio][graph-canvas][scene][q17]") {
    (void)application();
    const Json color{{"semantic", "pelican.render.color_signal@1"},
                     {"arguments", {{"reference", "scene"}, {"transfer", "linear"},
                                    {"range", "extended"}}}};
    const Json depth{{"semantic", "pelican.render.depth@1"},
                     {"arguments", {{"representation", "device"}, {"space", "projection"}}}};
    const auto documentFor = [&](const std::string &material) {
        return Json{
            {"definitions", {{"forward_opaque_draw",
                {{"nodes", Json::array({{{"id", "step"}, {"op", "draw.material"},
                                         {"parameters", Json::object()}, {"outputs", {"draws"}}}})},
                 {"outputs", {{"draws", {{"node", "step"}, {"port", "draws"}}}}}}}}},
            {"nodes", Json::array({
                {{"id", "scene_color_ref"}, {"op", "render.target_ref"},
                 {"parameters", {{"name", "scene_color"}, {"type", color}}}, {"outputs", {"resource"}}},
                {{"id", "scene_depth_ref"}, {"op", "render.target_ref"},
                 {"parameters", {{"name", "scene_depth"}, {"type", depth}}}, {"outputs", {"resource"}}},
                {{"id", "forward_opaque_draws"}, {"op", "graph.loop"},
                 {"parameters", {{"over", {{"draw_items", {{"contract", "forward_opaque_v1"}}}}},
                                 {"order", {{"by", "phase_default"}}},
                                 {"branches", Json::array({
                                     {{"when", {{"material", material}}},
                                      {"body", "forward_opaque_draw"}},
                                     {{"body", "forward_opaque_draw"}}})}}},
                 {"outputs", {"draws"}}},
                {{"id", "forward_opaque"}, {"op", "render.material_pass"},
                 {"parameters", {{"material_contract", "forward_opaque_v1"},
                                 {"pass_set", "main_render"}}},
                 {"inputs", {{"color", {{"node", "scene_color_ref"}, {"port", "resource"}}},
                             {"depth", {{"node", "scene_depth_ref"}, {"port", "resource"}}},
                             {"draws", {{"node", "forward_opaque_draws"}, {"port", "draws"}}}}},
                 {"outputs", Json::array()}}})}};
    };
    const std::string metal = "project://m.material.json#metal";
    const std::string glass = "project://m.material.json#glass";
    Json catalog = visualCatalog();
    for (const char *op : {"render.target_ref", "render.material_pass", "draw.material"})
        catalog["modules"][1]["operations"].push_back(
            {{"op", op}, {"domain", "rendering"}, {"evaluation", "direct"}, {"sink", false},
             {"ports", "parameter_dependent"}});
    RecordedCapability transport;
    auto capability = transport.capability();
    capability.requestFramePlan = [&transport](QString *) {
        transport.calls.push_back({QStringLiteral("get_frame_plan")});
        return transport.next++;
    };
    PelicanStudio::GraphCanvasWidget widget{capability};
    widget.resize(1100, 700);
    widget.show();
    QCoreApplication::processEvents();
    const auto answerRpc = [&](const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        REQUIRE(transport.deliver);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    };
    answerRpc(QStringLiteral("get_graph_instances"), context(documentFor(metal)));
    answerRpc(QStringLiteral("get_graph_catalog"), catalog);
    answerRpc(QStringLiteral("get_frame_plan"),
              Json{{"loops", Json::array({{{"node", "forward_opaque_draws"}, {"tier", "draw_loop"},
                                           {"pass", "forward_opaque"}, {"count", 2},
                                           {"elements", Json::array({
                                               {{"element", metal}, {"index", 0}, {"branch", 0}},
                                               {{"element", glass}, {"index", 1}, {"branch", 1}}})},
                                           {"unmatched_branches", Json::array()},
                                           {"total", 2}, {"offset", 0}, {"limit", 256}}})}});
    widget.scene().selectNode("forward_opaque_draws");
    QCoreApplication::processEvents();

    const auto field = [&](const char *name) {
        auto *combo = widget.findChild<QComboBox *>(QString::fromLatin1(name));
        REQUIRE(combo != nullptr);
        return combo;
    };
    const auto edits = [&] { return transport.countOf(QStringLiteral("edit_graph_document")); };
    // What a person (or a scenario) can do with the body at this moment: when
    // the row can be picked in, the pick goes out; when it cannot, nothing is
    // lost. A pick that neither went out nor was refused is the bug.
    const auto bodyPickNotLost = [&] {
        auto *body = field("pelican.graphCanvasLoop.branch0.body");
        if (!body->isEnabled()) return true;
        const int before = edits();
        body->setCurrentIndex(body->findText(QStringLiteral("(nothing)")));
        QCoreApplication::processEvents();
        return edits() == before + 1;
    };

    auto *material = field("pelican.graphCanvasLoop.branch0.material");
    REQUIRE(material->isEnabled());
    REQUIRE(field("pelican.graphCanvasLoop.branch0.body")->isEnabled());
    material->setCurrentIndex(material->findData(QString::fromStdString(glass)));
    QCoreApplication::processEvents();
    REQUIRE(edits() == 1);
    // Sent, not yet accepted.
    CHECK(bodyPickNotLost());
    CHECK_FALSE(field("pelican.graphCanvasLoop.branch0.body")->isEnabled());
    // Accepted, its ticket not yet committed.
    answerRpc(QStringLiteral("edit_graph_document"), Json{{"status", "accepted"}, {"ticket", "q17"}});
    REQUIRE(transport.countOf(QStringLiteral("get_edit_result")) == 1);
    CHECK(bodyPickNotLost());
    // Committed, the document not yet read back.
    const int reads = transport.countOf(QStringLiteral("get_graph_instances"));
    answerRpc(QStringLiteral("get_edit_result"), Json{{"status", "committed"}, {"committed", true}});
    REQUIRE(transport.countOf(QStringLiteral("get_graph_instances")) == reads + 1);
    CHECK(bodyPickNotLost());
    REQUIRE(edits() == 1);
    // Read back: the row is on again, and a pick goes out.
    answerRpc(QStringLiteral("get_graph_instances"), context(documentFor(glass)));
    auto *body = field("pelican.graphCanvasLoop.branch0.body");
    REQUIRE(body->isEnabled());
    CHECK(field("pelican.graphCanvasLoop.branch0.material")->currentText() ==
          QString::fromStdString(glass));
    body->setCurrentIndex(body->findText(QStringLiteral("(nothing)")));
    QCoreApplication::processEvents();
    REQUIRE(edits() == 2);
    const auto sent = Json::parse(transport.calls.back().body.toStdString());
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].at("parameters").at("branches").at(0).at("body").is_null());
}

// Q-17. The second guard: a pick the model refuses all the same does not stay
// on screen. The row goes back to what the document says, so neither a person
// nor automation's select is told the choice was made.
TEST_CASE("Q-17 a branch choice the model refuses goes back to what the document says",
          "[devstudio][graph-canvas][scene][q17]") {
    (void)application();
    const Json body{{"nodes", Json::array({{{"id", "target"}, {"op", "graph.input"},
                                            {"default", 0.0}}})},
                    {"outputs", Json::object()}};
    Json document{{"definitions", {{"step", body}, {"paint", body}}},
                  {"nodes", Json::array({
                      {{"id", "steps"}, {"op", "graph.loop"},
                       {"parameters", {{"over", {{"range", {{"count", 2}}}}},
                                       {"branches", Json::array({
                                           {{"when", {{"element", 0}}}, {"body", "paint"}},
                                           {{"body", "step"}}})}}},
                       {"outputs", Json::array()}}})}};
    RecordedCapability transport;
    PelicanStudio::GraphCanvasWidget widget{transport.capability()};
    widget.resize(900, 600);
    widget.show();
    QCoreApplication::processEvents();
    const auto answerRpc = [&](const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    };
    answerRpc(QStringLiteral("get_graph_instances"), context(document));
    answerRpc(QStringLiteral("get_graph_catalog"), visualCatalog());
    widget.scene().selectNode("steps");
    QCoreApplication::processEvents();

    // A read of the document in flight: the model takes no edit, and the
    // fields say so.
    widget.refresh();
    QCoreApplication::processEvents();
    auto *fields = widget.findChild<QWidget *>(QStringLiteral("pelican.graphCanvasLoop.fields"));
    REQUIRE(fields != nullptr);
    CHECK_FALSE(fields->isEnabled());
    // Forced on, as the fields were before: each refused choice goes back.
    fields->setEnabled(true);
    auto *chosen = widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasLoop.branch0.body"));
    auto *when = widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasLoop.branch0.when"));
    REQUIRE(chosen != nullptr);
    REQUIRE(when != nullptr);
    REQUIRE(chosen->isEnabled());
    chosen->setCurrentIndex(chosen->findText(QStringLiteral("(nothing)")));
    QCoreApplication::processEvents();
    CHECK(chosen->currentText() == QStringLiteral("paint"));
    when->setCurrentIndex(when->findData(QStringLiteral("element_in")));
    QCoreApplication::processEvents();
    CHECK(when->currentData().toString() == QStringLiteral("element"));
    CHECK(transport.countOf(QStringLiteral("edit_graph_document")) == 0);

    // The read lands: the fields are on, and a choice goes out.
    answerRpc(QStringLiteral("get_graph_instances"), context(document));
    CHECK(fields->isEnabled());
    chosen = widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasLoop.branch0.body"));
    chosen->setCurrentIndex(chosen->findText(QStringLiteral("(nothing)")));
    QCoreApplication::processEvents();
    CHECK(transport.countOf(QStringLiteral("edit_graph_document")) == 1);
}

TEST_CASE("LP-4e the Graph tab selects the loop a decompose made once a document holds it",
          "[devstudio][graph-canvas][scene][lp4e]") {
    (void)application();
    const Json before{{"definitions", Json::object()}, {"nodes", Json::array()}};
    Json after{{"definitions", Json::object()},
               {"nodes", Json::array({{{"id", "forward_opaque_draws"}, {"op", "graph.loop"},
                                       {"parameters", {{"over", {{"draw_items", {{"contract", "forward_opaque_v1"}}}}},
                                                       {"branches", Json::array({{{"body", nullptr}}})}}},
                                       {"outputs", {"draws"}}}})}};
    RecordedCapability transport;
    PelicanStudio::GraphCanvasWidget widget{transport.capability()};
    widget.show();
    QCoreApplication::processEvents();
    const auto answerRpc = [&](const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    };
    answerRpc(QStringLiteral("get_graph_instances"), context(before));
    answerRpc(QStringLiteral("get_graph_catalog"), visualCatalog());
    // The document in hand predates the edit: it is read again, and the loop
    // is picked when the new one arrives.
    widget.revealNodeWhenPresent(QStringLiteral("forward_opaque_draws"));
    QCoreApplication::processEvents();
    REQUIRE(transport.countOf(QStringLiteral("get_graph_instances")) == 2);
    CHECK_FALSE(widget.scene().selectedNode().has_value());
    auto moved = context(after);
    moved["source_digest"] = {{"hex", "source-b"}};
    answerRpc(QStringLiteral("get_graph_instances"), moved);
    REQUIRE(widget.scene().selectedNode().has_value());
    CHECK(*widget.scene().selectedNode() == "forward_opaque_draws");
}

// --- LP-4d1: the loop zone.

namespace {

// A root with a constant `a`, a curve `curve` and a loop `steps` over two
// branches: element 0 runs `first` (a boundary input `target`, a constant
// `a` and a sum of the two -- the body's output `out`), the else runs `rest`
// (the same boundary, a constant `c` and a curve `curve`). The root's `a` and
// `curve` share their ids with body nodes on purpose.
Json zoneDocument(bool else_only = false) {
    const auto constant = [](const char *id, double value) {
        return Json{{"id", id}, {"op", "value.constant"},
                    {"parameters", {{"value", value}}}, {"outputs", Json::array({"value"})}};
    };
    const auto curve = [](const char *id, double time) {
        return Json{{"id", id}, {"op", "value.curve"},
                    {"parameters", {{"time", time}}}, {"outputs", Json::array({"value"})}};
    };
    const Json boundary{{"id", "target"}, {"op", "graph.input"}, {"default", 0.0}};
    const Json first{
        {"nodes", Json::array({boundary, constant("a", 1.0),
                               {{"id", "sum"}, {"op", "value.add"},
                                {"inputs", {{"a", {{"node", "target"}, {"port", "value"}}},
                                            {"b", {{"node", "a"}, {"port", "value"}}}}},
                                {"outputs", Json::array({"value"})}}})},
        {"outputs", {{"out", {{"node", "sum"}, {"port", "value"}}}}}};
    const Json rest{
        {"nodes", Json::array({boundary, constant("c", 2.0), curve("curve", 0.5)})},
        {"outputs", {{"out", {{"node", "c"}, {"port", "value"}}}}}};
    const Json branches =
        else_only ? Json::array({{{"body", "first"}}})
                  : Json::array({{{"when", {{"element", 0}}}, {"body", "first"}}, {{"body", "rest"}}});
    return Json{{"schema", "pelican.graph"}, {"version", 1}, {"outputs", Json::object()},
                {"definitions", {{"first", first}, {"rest", rest}}},
                {"nodes", Json::array({constant("a", 5.0), curve("curve", 0.25),
                                       {{"id", "steps"}, {"op", "graph.loop"},
                                        {"parameters", {{"over", {{"range", {{"count", 2}}}}},
                                                        {"branches", branches}}},
                                        {"inputs", {{"target", {{"node", "a"}, {"port", "value"}}}}},
                                        {"outputs", Json::array({"out", "out[]"})}}})}};
}

// One model and one scene over a document the engine's own edit function
// keeps: every committed op list is applied to it, and every commit moves the
// digest, so Undo and Redo run against what the engine would hold.
struct ZoneHarness {
    GraphCanvasModel model;
    GraphCanvasScene scene;
    Json document;
    int generation = 0;
    // LP-4d2: what get_frame_plan's loops[] says, when a test gives one.
    Json loops = Json::array();
    // LP-4f1: the rest of the plan (its nodes' view axes, its resources).
    Json plan = Json::object();
    // LP-4f2: the operation catalog.
    Json catalog = visualCatalog();
    // LP-4f3: every read the model sent, by method, in order.
    std::vector<std::string> sent;

    explicit ZoneHarness(Json initial = zoneDocument()) : document(std::move(initial)) {}

    [[nodiscard]] std::string digest() const { return "zone-" + std::to_string(generation); }
    [[nodiscard]] Json snapshot() const {
        return {{"source_reference", "fixture"}, {"root_schema", "pelican.graph"},
                {"source_digest", {{"hex", digest()}}},
                {"nodes", document.at("nodes")},
                {"definitions", document.at("definitions")},
                {"layout", document.value("layout", Json::object())}};
    }
    // Answers the reads in the outbox; returns the edits it found there.
    std::vector<PelicanStudio::InspectorRpcRequest> answerReads() {
        std::vector<PelicanStudio::InspectorRpcRequest> edits;
        for (const auto &request : model.takeRpcRequests()) {
            sent.push_back(request.method);
            if (request.method == "get_graph_instances")
                model.receiveRpcResult(request.request_id, snapshot().dump());
            else if (request.method == "get_graph_catalog")
                model.receiveRpcResult(request.request_id, catalog.dump());
            else if (request.method == "get_frame_plan") {
                auto reply = plan;
                reply["loops"] = loops;
                model.receiveRpcResult(request.request_id, reply.dump());
            }
            else if (request.method == "edit_graph_document")
                edits.push_back(request);
        }
        return edits;
    }
    void open() {
        model.setConnected(true);
        answerReads();
        scene.setModel(&model);
    }
    // The one edit a gesture sent. With `apply`, the engine's own edit is
    // run on the document; either way it commits, and the document is read
    // again and drawn.
    Json commit(bool apply = true) {
        const auto edits = answerReads();
        REQUIRE(edits.size() == 1);
        const auto &submit = edits.front();
        REQUIRE(submit.params.at("base_source_digest") == digest());
        const Json ops = submit.params.at("ops");
        Json edited = document;
        if (apply) edited = Pelican::editGraphDocument(document, ops);
        model.receiveRpcResult(submit.request_id,
                               Json{{"status", "accepted"}, {"ticket", "zone-edit"}}.dump());
        for (const auto &request : model.takeRpcRequests()) {
            if (request.method != "get_edit_result") continue;
            document = std::move(edited);
            ++generation;
            model.receiveRpcResult(request.request_id,
                                   Json{{"status", "committed"}, {"committed", true},
                                        {"source_digest", {{"hex", digest()}}}}
                                       .dump());
        }
        answerReads();
        scene.repopulate();
        return ops;
    }
};

QGraphicsItem *scopedNode(QGraphicsScene &scene, const QString &scope, const QString &name) {
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasNodeItem} &&
            item->data(PelicanStudio::GraphCanvasScopeRole).toString() == scope &&
            item->data(PelicanStudio::NodeGraphNameRole).toString() == name)
            return item;
    return nullptr;
}

QGraphicsItem *scopedItem(QGraphicsScene &scene, const char *kind, const QString &scope,
                          const QString &name, const QString &port = {},
                          const QString &direction = {}) {
    for (QGraphicsItem *item : scene.items()) {
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() != QLatin1String{kind} ||
            item->data(PelicanStudio::GraphCanvasScopeRole).toString() != scope ||
            item->data(PelicanStudio::NodeGraphNameRole).toString() != name)
            continue;
        if (!port.isNull() && item->data(PelicanStudio::GraphCanvasPortRole).toString() != port)
            continue;
        if (!direction.isNull() &&
            item->data(PelicanStudio::GraphCanvasSocketDirectionRole).toString() != direction)
            continue;
        return item;
    }
    return nullptr;
}

QGraphicsItem *scopedEdge(QGraphicsScene &scene, const QString &scope, const QString &from,
                          const QString &from_port, const QString &to, const QString &to_port) {
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasEdgeItem} &&
            item->data(PelicanStudio::GraphCanvasScopeRole).toString() == scope &&
            item->data(PelicanStudio::NodeGraphFromNameRole).toString() == from &&
            item->data(PelicanStudio::GraphCanvasFromPortRole).toString() == from_port &&
            item->data(PelicanStudio::NodeGraphToNameRole).toString() == to &&
            item->data(PelicanStudio::GraphCanvasToPortRole).toString() == to_port)
            return item;
    return nullptr;
}

// The tabs of a zone, by the text they show, in branch order.
std::vector<std::pair<std::size_t, QString>> zoneTabs(QGraphicsScene &scene) {
    std::vector<std::pair<std::size_t, QString>> tabs;
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasZoneTabItem} &&
            !item->data(PelicanStudio::GraphCanvasLabelRole).toString().isEmpty())
            tabs.emplace_back(
                static_cast<std::size_t>(item->data(PelicanStudio::GraphCanvasBranchRole).toULongLong()),
                item->data(PelicanStudio::GraphCanvasLabelRole).toString());
    std::ranges::sort(tabs);
    return tabs;
}

QPointF centreOf(const QGraphicsItem *item) { return item->sceneBoundingRect().center(); }

// A press on a node's heading, which picks it.
QPointF headingOf(const QGraphicsItem *node) {
    return node->mapToScene(QPointF{node->boundingRect().width() / 2.0, 4.0});
}

} // namespace

TEST_CASE("LP-4d1 the root draws a loop as a zone around its body, in the body's own scope",
          "[devstudio][graph-canvas][scene][lp4d1]") {
    (void)application();
    ZoneHarness h;
    h.open();
    auto &scene = h.scene;
    REQUIRE(scene.zones() == std::map<std::string, std::string>{{"steps", "first"}});

    // The root's nodes and the frame, which is the loop's node.
    QGraphicsItem *frame = scopedNode(scene, QString{}, QStringLiteral("steps"));
    REQUIRE(frame != nullptr);
    REQUIRE(scopedNode(scene, QString{}, QStringLiteral("a")) != nullptr);
    CHECK(scopedItem(scene, PelicanStudio::GraphCanvasZoneItem, QString{}, QStringLiteral("steps")) !=
          nullptr);
    // The body's two nodes, in the definition and inside the frame.
    QGraphicsItem *body_a = scopedNode(scene, QStringLiteral("first"), QStringLiteral("a"));
    QGraphicsItem *sum = scopedNode(scene, QStringLiteral("first"), QStringLiteral("sum"));
    REQUIRE(body_a != nullptr);
    REQUIRE(sum != nullptr);
    CHECK(body_a->data(PelicanStudio::GraphCanvasZoneScopeRole).toString() == QStringLiteral("first"));
    CHECK(frame->sceneBoundingRect().contains(body_a->sceneBoundingRect()));
    CHECK(frame->sceneBoundingRect().contains(sum->sceneBoundingRect()));
    // The boundary input is the frame's left edge, not a node: the loop's
    // input outside, the graph.input's value inside.
    CHECK(scopedNode(scene, QStringLiteral("first"), QStringLiteral("target")) == nullptr);
    QGraphicsItem *inner = scopedItem(scene, PelicanStudio::GraphCanvasSocketItem, QStringLiteral("first"),
                                      QStringLiteral("target"), QStringLiteral("value"),
                                      QStringLiteral("output"));
    QGraphicsItem *outer = scopedItem(scene, PelicanStudio::GraphCanvasSocketItem, QString{},
                                      QStringLiteral("steps"), QStringLiteral("target"),
                                      QStringLiteral("input"));
    REQUIRE(inner != nullptr);
    REQUIRE(outer != nullptr);
    CHECK(std::abs(centreOf(inner).y() - centreOf(outer).y()) < 0.5);
    CHECK(centreOf(inner).x() > centreOf(outer).x());
    // The right edge: the body's output inside, the loop's p and p[] outside.
    CHECK(scopedItem(scene, PelicanStudio::GraphCanvasZoneSocketItem, QString{}, QStringLiteral("steps"),
                     QStringLiteral("out:out")) != nullptr);
    CHECK(scopedItem(scene, PelicanStudio::GraphCanvasSocketItem, QString{}, QStringLiteral("steps"),
                     QStringLiteral("out"), QStringLiteral("output")) != nullptr);
    CHECK(scopedItem(scene, PelicanStudio::GraphCanvasSocketItem, QString{}, QStringLiteral("steps"),
                     QStringLiteral("out[]"), QStringLiteral("output")) != nullptr);
    // The body's edges are the definition's, the root's the root's.
    CHECK(scopedEdge(scene, QStringLiteral("first"), QStringLiteral("target"), QStringLiteral("value"),
                     QStringLiteral("sum"), QStringLiteral("a")) != nullptr);
    CHECK(scopedEdge(scene, QStringLiteral("first"), QStringLiteral("a"), QStringLiteral("value"),
                     QStringLiteral("sum"), QStringLiteral("b")) != nullptr);
    CHECK(scopedEdge(scene, QString{}, QStringLiteral("a"), QStringLiteral("value"),
                     QStringLiteral("steps"), QStringLiteral("target")) != nullptr);
    // The heading: the stage, and a tab per branch (if / else).
    auto *stage = dynamic_cast<QGraphicsSimpleTextItem *>(
        scopedItem(scene, PelicanStudio::GraphCanvasZoneLabelItem, QString{}, QStringLiteral("steps"),
                   QStringLiteral("stage")));
    REQUIRE(stage != nullptr);
    CHECK(stage->text() == QStringLiteral("loop"));
    const auto tabs = zoneTabs(scene);
    REQUIRE(tabs.size() == 2);
    CHECK(tabs[0].second == QStringLiteral("if element = 0"));
    CHECK(tabs[1].second == QStringLiteral("else"));

    // A click meant for the loop (the automation surface aims at
    // NodeGraphHotspotRole) lands on its heading, not on a body node.
    const QVariant spot = frame->data(PelicanStudio::NodeGraphHotspotRole);
    REQUIRE(spot.isValid());
    const auto under = scene.items(frame->mapToScene(spot.toPointF()));
    REQUIRE_FALSE(under.isEmpty());
    CHECK(under.front()->data(PelicanStudio::GraphCanvasHeadingRole).toBool());
    CHECK(under.front()->data(PelicanStudio::NodeGraphNameRole).toString() == QStringLiteral("steps"));
    CHECK(under.front()->data(PelicanStudio::GraphCanvasScopeRole).toString().isEmpty());

    // The body comes along with its frame.
    const QPointF before = sum->scenePos();
    frame->setPos(frame->pos() + QPointF{40.0, 30.0});
    CHECK(sum->scenePos() == before + QPointF{40.0, 30.0});

    // A tab shows that branch's body: session state, nothing is sent.
    QGraphicsItem *else_tab = nullptr;
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasZoneTabItem} &&
            item->data(PelicanStudio::GraphCanvasBranchRole).toULongLong() == 1 &&
            !item->data(PelicanStudio::GraphCanvasLabelRole).toString().isEmpty())
            else_tab = item;
    REQUIRE(else_tab != nullptr);
    click(scene, centreOf(else_tab));
    CHECK(scene.zones() == std::map<std::string, std::string>{{"steps", "rest"}});
    CHECK(scopedNode(scene, QStringLiteral("rest"), QStringLiteral("c")) != nullptr);
    CHECK(scopedNode(scene, QStringLiteral("rest"), QStringLiteral("curve")) != nullptr);
    CHECK(scopedNode(scene, QStringLiteral("first"), QStringLiteral("sum")) == nullptr);
    for (const auto &request : h.model.takeRpcRequests())
        CHECK(request.params.value("ops", Json::array()).dump().find("\"rest\"") == std::string::npos);

    // Folded, the zone is the loop's node again.
    scene.setCollapsed(std::string{}, "steps", true);
    CHECK(scene.zones().empty());
    REQUIRE(scopedNode(scene, QString{}, QStringLiteral("steps")) != nullptr);
    for (QGraphicsItem *item : scene.items())
        CHECK(item->data(PelicanStudio::GraphCanvasZoneScopeRole).toString().isEmpty());

    // A loop whose only branch is the else has no tabs.
    ZoneHarness single{zoneDocument(true)};
    single.open();
    CHECK(single.scene.zones() == std::map<std::string, std::string>{{"steps", "first"}});
    CHECK(zoneTabs(single.scene).empty());
}

TEST_CASE("LP-4d1 an edit inside a zone goes to the body's definition, and moving the zone to the root",
          "[devstudio][graph-canvas][scene][lp4d1]") {
    (void)application();
    ZoneHarness h;
    h.open();
    auto &scene = h.scene;

    // A body node moved: the definition's placement, in the body's own
    // coordinates.
    QGraphicsItem *sum = scopedNode(scene, QStringLiteral("first"), QStringLiteral("sum"));
    REQUIRE(sum != nullptr);
    const QPointF moved = sum->pos() + QPointF{30.0, 20.0};
    sum->setPos(moved);
    scene.flushLayout();
    auto ops = h.commit();
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].at("op") == "set_layout");
    CHECK(ops[0].at("scope") == "first");
    CHECK(ops[0].at("layout").at("nodes").at("sum") == Json{{"x", moved.x()}, {"y", moved.y()}});
    CHECK_FALSE(ops[0].at("layout").at("nodes").contains("steps"));

    // The zone moved: the root's placement, and the body is not written.
    QGraphicsItem *frame = scopedNode(scene, QString{}, QStringLiteral("steps"));
    REQUIRE(frame != nullptr);
    const QPointF frame_at = frame->pos() + QPointF{-50.0, 60.0};
    frame->setPos(frame_at);
    scene.flushLayout();
    ops = h.commit();
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].at("op") == "set_layout");
    CHECK(ops[0].at("scope") == "root");
    CHECK(ops[0].at("layout").at("nodes").at("steps") == Json{{"x", frame_at.x()}, {"y", frame_at.y()}});
    CHECK_FALSE(ops[0].at("layout").at("nodes").contains("sum"));

    // A line inside the zone: from the boundary input to a body input.
    QGraphicsItem *boundary = scopedItem(scene, PelicanStudio::GraphCanvasSocketItem,
                                         QStringLiteral("first"), QStringLiteral("target"),
                                         QStringLiteral("value"), QStringLiteral("output"));
    QGraphicsItem *sum_b = scopedItem(scene, PelicanStudio::GraphCanvasSocketItem, QStringLiteral("first"),
                                      QStringLiteral("sum"), QStringLiteral("b"), QStringLiteral("input"));
    REQUIRE(boundary != nullptr);
    REQUIRE(sum_b != nullptr);
    drag(scene, centreOf(boundary), centreOf(sum_b));
    ops = h.commit(false);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0] == Json{{"op", "set_input"}, {"scope", "first"}, {"node", "sum"}, {"port", "b"},
                         {"source", {{"node", "target"}, {"port", "value"}}}});

    // Cut: the picked body edge, Delete.
    scene.selectEdge(std::string{"first"},
                     PelicanStudio::GraphCanvasEdge{"a", "value", "sum", "b"}, false);
    pressKey(scene, Qt::Key_Delete);
    ops = h.commit(false);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0] == Json{{"op", "set_input"}, {"scope", "first"}, {"node", "sum"}, {"port", "b"},
                         {"source", nullptr}});

    // The label of a body node, in place.
    REQUIRE(scene.beginLabelEdit("first", "sum"));
    scene.setInlineEditText(QStringLiteral("Sum"));
    REQUIRE(scene.commitInlineEdit());
    ops = h.commit(false);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0] == Json{{"op", "set_node_label"}, {"scope", "first"}, {"node", "sum"},
                         {"label", "Sum"}});

    // A body node removed.
    scene.selectNode("first", "a");
    pressKey(scene, Qt::Key_Delete);
    ops = h.commit(false);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0] == Json{{"op", "remove_node"}, {"scope", "first"}, {"node", "a"}});

    // Adding, with the zone picked: into its body, from the body's palette --
    // which offers neither a loop nor an instance.
    scene.selectNode("", "steps");
    CHECK(scene.addScope() == "first");
    const auto palette = h.model.paletteOperationsIn("first");
    CHECK(std::ranges::none_of(palette, [](const auto &entry) {
        return entry.op == "graph.loop" || entry.op == "graph.instance";
    }));
    CHECK(std::ranges::any_of(palette, [](const auto &entry) { return entry.op == "value.add"; }));
    REQUIRE(scene.addFromPalette("value.constant"));
    ops = h.commit(false);
    REQUIRE_FALSE(ops.empty());
    CHECK(ops.back().at("op") == "add_node");
    CHECK(ops.back().at("scope") == "first");
    CHECK(ops.back().at("node").at("op") == "value.constant");
    // Nothing picked: the root, as ever.
    scene.selectNode("", "a");
    CHECK(scene.addScope().empty());
}

TEST_CASE("LP-4d1 a root node and a body node with one id are picked, edited and removed apart",
          "[devstudio][graph-canvas][scene][lp4d1]") {
    (void)application();
    ZoneHarness h;
    h.open();
    auto &scene = h.scene;

    // `a` in the body, by a press on its heading.
    QGraphicsItem *body_a = scopedNode(scene, QStringLiteral("first"), QStringLiteral("a"));
    QGraphicsItem *root_a = scopedNode(scene, QString{}, QStringLiteral("a"));
    REQUIRE(body_a != nullptr);
    REQUIRE(root_a != nullptr);
    click(scene, headingOf(body_a));
    REQUIRE(scene.selectedNode().has_value());
    CHECK(*scene.selectedNode() == "a");
    CHECK(scene.selectedScope() == "first");
    pressKey(scene, Qt::Key_Delete);
    auto ops = h.commit(false);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0] == Json{{"op", "remove_node"}, {"scope", "first"}, {"node", "a"}});

    // `a` in the root, the same way.
    root_a = scopedNode(scene, QString{}, QStringLiteral("a"));
    REQUIRE(root_a != nullptr);
    click(scene, headingOf(root_a));
    CHECK(scene.selectedScope().empty());
    pressKey(scene, Qt::Key_Delete);
    ops = h.commit(false);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0] == Json{{"op", "remove_node"}, {"scope", "root"}, {"node", "a"}});

    // `curve` in both: each pill edits its own node's value.
    REQUIRE(scene.showLoopBranch("steps", 1));
    const auto editPill = [&](const QString &scope, const QString &text) {
        QGraphicsItem *pill = scopedItem(scene, PelicanStudio::GraphCanvasValuePillItem, scope,
                                         QStringLiteral("curve"), QStringLiteral("time"));
        REQUIRE(pill != nullptr);
        click(scene, centreOf(pill));
        REQUIRE(scene.editingInline());
        scene.setInlineEditText(text);
        REQUIRE(scene.commitInlineEdit());
        return h.commit(false);
    };
    ops = editPill(QStringLiteral("rest"), QStringLiteral("0.75"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0] == Json{{"op", "set_node_parameters"}, {"scope", "rest"}, {"node", "curve"},
                         {"parameters", {{"time", 0.75}}}});
    CHECK(scene.selectedScope() == "rest");
    ops = editPill(QString{}, QStringLiteral("0.125"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0] == Json{{"op", "set_node_parameters"}, {"scope", "root"}, {"node", "curve"},
                         {"parameters", {{"time", 0.125}}}});
    CHECK(scene.selectedScope().empty());
}

TEST_CASE("LP-4d1 Undo and Redo take a zone's edits and its move back, byte for byte",
          "[devstudio][graph-canvas][scene][lp4d1]") {
    (void)application();
    ZoneHarness h;
    h.open();
    auto &scene = h.scene;
    const auto before = h.document.dump();

    // Inside the zone: a node moved, then a label; then the zone moved.
    QGraphicsItem *sum = scopedNode(scene, QStringLiteral("first"), QStringLiteral("sum"));
    REQUIRE(sum != nullptr);
    sum->setPos(sum->pos() + QPointF{25.0, 45.0});
    scene.flushLayout();
    CHECK(h.commit().at(0).at("scope") == "first");
    REQUIRE(scene.beginLabelEdit("first", "sum"));
    scene.setInlineEditText(QStringLiteral("Sum"));
    REQUIRE(scene.commitInlineEdit());
    h.commit();
    QGraphicsItem *frame = scopedNode(scene, QString{}, QStringLiteral("steps"));
    REQUIRE(frame != nullptr);
    frame->setPos(frame->pos() + QPointF{70.0, -20.0});
    scene.flushLayout();
    CHECK(h.commit().at(0).at("scope") == "root");
    const auto after = h.document.dump();
    REQUIRE(after != before);
    REQUIRE_FALSE(scene.layoutPending());

    for (int step = 0; step < 3; ++step) {
        REQUIRE(h.model.canUndo());
        scene.undoEdit();
        h.commit();
    }
    CHECK(h.document.dump() == before);
    for (int step = 0; step < 3; ++step) {
        REQUIRE(h.model.canRedo());
        scene.redoEdit();
        h.commit();
    }
    CHECK(h.document.dump() == after);
}

// --- LP-4d2: lines across a zone's edge, the carry, the expansion.

namespace {

// zoneDocument() with both branches running `first`: one definition, shared.
Json sharedBodyDocument() {
    auto document = zoneDocument();
    document["nodes"][2]["parameters"]["branches"] =
        Json::array({{{"when", {{"element", 0}}}, {"body", "first"}}, {{"body", "first"}}});
    return document;
}

// zoneDocument() whose else body `rest` also has a `sum` (value.add of its
// boundary and `c`): the same wiring the body a copy was made from has.
Json twinBodyDocument() {
    auto document = zoneDocument();
    document["definitions"]["rest"]["nodes"].push_back(
        {{"id", "sum"}, {"op", "value.add"},
         {"inputs", {{"a", {{"node", "target"}, {"port", "value"}}},
                     {"b", {{"node", "c"}, {"port", "value"}}}}},
         {"outputs", Json::array({"value"})}});
    return document;
}

QGraphicsItem *zoneSlot(QGraphicsScene &scene, const char *slot) {
    return scopedItem(scene, PelicanStudio::GraphCanvasZoneSocketItem, QString{},
                      QStringLiteral("steps"), QString::fromLatin1(slot));
}

QGraphicsItem *socketOf(QGraphicsScene &scene, const QString &scope, const char *node,
                        const char *port, const char *direction) {
    return scopedItem(scene, PelicanStudio::GraphCanvasSocketItem, scope,
                      QString::fromLatin1(node), QString::fromLatin1(port),
                      QString::fromLatin1(direction));
}

Json graphInput(const char *scope, const char *id) {
    return {{"op", "add_node"}, {"scope", scope}, {"node", {{"id", id}, {"op", "graph.input"}}}};
}

} // namespace

TEST_CASE("LP-4d2 a line from outside into a zone is a loop input and a boundary in every body, "
          "in one list",
          "[devstudio][graph-canvas][scene][lp4d2]") {
    (void)application();

    SECTION("dropped on the left edge's slot: an input named after the port, in both bodies") {
        ZoneHarness h;
        h.open();
        QGraphicsItem *curve = socketOf(h.scene, QString{}, "curve", "value", "output");
        QGraphicsItem *slot = zoneSlot(h.scene, "in:+");
        REQUIRE(curve != nullptr);
        REQUIRE(slot != nullptr);
        // The slot is on the left edge, under the loop's own input.
        QGraphicsItem *frame = scopedNode(h.scene, QString{}, QStringLiteral("steps"));
        CHECK(std::abs(centreOf(slot).x() - frame->sceneBoundingRect().left()) < 8.0);
        drag(h.scene, centreOf(curve), centreOf(slot));
        const auto ops = h.commit();
        CHECK(ops == Json::array({graphInput("first", "value"), graphInput("rest", "value"),
                                  {{"op", "set_input"}, {"scope", "root"}, {"node", "steps"},
                                   {"port", "value"},
                                   {"source", {{"node", "curve"}, {"port", "value"}}}}}));
        // The engine took it: the loop reads curve, and both bodies have it.
        CHECK(h.document.at("nodes")[2].at("inputs").at("value") ==
              Json{{"node", "curve"}, {"port", "value"}});
        // Drawn: a row of the left edge, the loop's socket outside and the
        // boundary inside.
        CHECK(socketOf(h.scene, QString{}, "steps", "value", "input") != nullptr);
        CHECK(socketOf(h.scene, QStringLiteral("first"), "value", "value", "output") != nullptr);
        // A second line from the same port is given a name of its own.
        CHECK(h.model.suggestLoopInputName("steps", "value") == "value_2");
    }

    SECTION("dropped on a body node's input: the same, and that input reads it") {
        ZoneHarness h;
        h.open();
        QGraphicsItem *root_a = socketOf(h.scene, QString{}, "a", "value", "output");
        QGraphicsItem *sum_b = socketOf(h.scene, QStringLiteral("first"), "sum", "b", "input");
        REQUIRE(root_a != nullptr);
        REQUIRE(sum_b != nullptr);
        drag(h.scene, centreOf(root_a), centreOf(sum_b));
        const auto ops = h.commit();
        CHECK(ops == Json::array({graphInput("first", "value"), graphInput("rest", "value"),
                                  {{"op", "set_input"}, {"scope", "root"}, {"node", "steps"},
                                   {"port", "value"},
                                   {"source", {{"node", "a"}, {"port", "value"}}}},
                                  {{"op", "set_input"}, {"scope", "first"}, {"node", "sum"},
                                   {"port", "b"},
                                   {"source", {{"node", "value"}, {"port", "value"}}}}}));
        CHECK(scopedEdge(h.scene, QStringLiteral("first"), QStringLiteral("value"),
                         QStringLiteral("value"), QStringLiteral("sum"),
                         QStringLiteral("b")) != nullptr);
    }

    SECTION("dropped on an input the loop already has: that edge is replaced, nothing else") {
        ZoneHarness h;
        h.open();
        QGraphicsItem *curve = socketOf(h.scene, QString{}, "curve", "value", "output");
        QGraphicsItem *target = socketOf(h.scene, QString{}, "steps", "target", "input");
        REQUIRE(curve != nullptr);
        REQUIRE(target != nullptr);
        drag(h.scene, centreOf(curve), centreOf(target));
        CHECK(h.commit() == Json::array({{{"op", "set_input"}, {"scope", "root"},
                                          {"node", "steps"}, {"port", "target"},
                                          {"source", {{"node", "curve"}, {"port", "value"}}}}}));
    }

    SECTION("two branches running one definition: its boundary is added once") {
        ZoneHarness h{sharedBodyDocument()};
        h.open();
        QGraphicsItem *curve = socketOf(h.scene, QString{}, "curve", "value", "output");
        QGraphicsItem *slot = zoneSlot(h.scene, "in:+");
        REQUIRE(curve != nullptr);
        REQUIRE(slot != nullptr);
        drag(h.scene, centreOf(curve), centreOf(slot));
        CHECK(h.commit() == Json::array({graphInput("first", "value"),
                                         {{"op", "set_input"}, {"scope", "root"},
                                          {"node", "steps"}, {"port", "value"},
                                          {"source", {{"node", "curve"}, {"port", "value"}}}}}));
    }
}

TEST_CASE("LP-4d2 a body output drawn to the right edge is an output of every body and the loop's p",
          "[devstudio][graph-canvas][scene][lp4d2]") {
    (void)application();

    SECTION("every body gives it from its node of that id: one list, set_outputs each") {
        ZoneHarness h{twinBodyDocument()};
        h.open();
        QGraphicsItem *sum = socketOf(h.scene, QStringLiteral("first"), "sum", "value", "output");
        QGraphicsItem *slot = zoneSlot(h.scene, "out:+");
        REQUIRE(sum != nullptr);
        REQUIRE(slot != nullptr);
        drag(h.scene, centreOf(sum), centreOf(slot));
        const auto ops = h.commit();
        auto loop = twinBodyDocument().at("nodes")[2];
        loop["outputs"] = Json::array({"out", "out[]", "value"});
        CHECK(ops == Json::array({
                         {{"op", "set_outputs"}, {"scope", "first"},
                          {"outputs", {{"out", {{"node", "sum"}, {"port", "value"}}},
                                       {"value", {{"node", "sum"}, {"port", "value"}}}}}},
                         {{"op", "set_outputs"}, {"scope", "rest"},
                          {"outputs", {{"out", {{"node", "c"}, {"port", "value"}}},
                                       {"value", {{"node", "sum"}, {"port", "value"}}}}}},
                         {{"op", "replace_node"}, {"scope", "root"}, {"node", loop}}}));
        // The engine took it, and the zone draws the new row: the body's end
        // inside, the loop's p outside.
        CHECK(h.document.at("definitions").at("rest").at("outputs").at("value") ==
              Json{{"node", "sum"}, {"port", "value"}});
        CHECK(scopedItem(h.scene, PelicanStudio::GraphCanvasZoneSocketItem, QString{},
                         QStringLiteral("steps"), QStringLiteral("out:value")) != nullptr);
        CHECK(socketOf(h.scene, QString{}, "steps", "value", "output") != nullptr);
    }

    SECTION("a body without that node: nothing is sent, and the notice says which") {
        ZoneHarness h;
        h.open();
        QGraphicsItem *sum = socketOf(h.scene, QStringLiteral("first"), "sum", "value", "output");
        QGraphicsItem *slot = zoneSlot(h.scene, "out:+");
        REQUIRE(sum != nullptr);
        REQUIRE(slot != nullptr);
        drag(h.scene, centreOf(sum), centreOf(slot));
        CHECK(h.answerReads().empty());
        CHECK(h.model.notice().message ==
              "Branch 1 of loop steps runs rest, which has no sum.value to give value from: "
              "add the output there first.");
    }
}

TEST_CASE("LP-4d2 a carry is drawn from a body output back to a boundary input, and Delete takes it "
          "out",
          "[devstudio][graph-canvas][scene][lp4d2]") {
    (void)application();
    ZoneHarness h;
    h.open();
    auto &scene = h.scene;
    const auto parameters = h.document.at("nodes")[2].at("parameters");

    QGraphicsItem *out = scopedItem(scene, PelicanStudio::GraphCanvasZoneSocketItem, QString{},
                                    QStringLiteral("steps"), QStringLiteral("out:out"));
    QGraphicsItem *target = socketOf(scene, QStringLiteral("first"), "target", "value", "output");
    REQUIRE(out != nullptr);
    REQUIRE(target != nullptr);
    drag(scene, centreOf(out), centreOf(target));
    auto carried = parameters;
    carried["carry"] = Json::array({{{"from", "out"}, {"to", "target"}}});
    CHECK(h.commit() == Json::array({{{"op", "set_node_parameters"}, {"scope", "root"},
                                      {"node", "steps"}, {"parameters", carried}}}));

    // The arrow runs round the bottom of the frame, below the body.
    auto *arrow = dynamic_cast<QGraphicsPathItem *>(
        scopedItem(scene, PelicanStudio::GraphCanvasZoneCarryItem, QString{},
                   QStringLiteral("steps"), QStringLiteral("out/target")));
    REQUIRE(arrow != nullptr);
    REQUIRE_FALSE(arrow->path().isEmpty());
    QGraphicsItem *sum = scopedNode(scene, QStringLiteral("first"), QStringLiteral("sum"));
    QGraphicsItem *frame = scopedNode(scene, QString{}, QStringLiteral("steps"));
    REQUIRE(sum != nullptr);
    CHECK(arrow->sceneBoundingRect().bottom() > sum->sceneBoundingRect().bottom());
    // (The line as drawn: its bounding box is the wider shape a press picks.)
    CHECK(frame->sceneBoundingRect().contains(arrow->mapRectToScene(arrow->path().boundingRect())));
    // target is fed by the loop's edge from a: no warning.
    CHECK(scopedItem(scene, PelicanStudio::GraphCanvasZoneCarryMarkItem, QString{},
                     QStringLiteral("steps"), QStringLiteral("out/target")) == nullptr);

    // Picked by a press on it, taken out by Delete.
    const QPointF on = arrow->mapToScene(arrow->path().pointAtPercent(0.5));
    click(scene, on);
    REQUIRE(scene.selectedCarry().has_value());
    CHECK(*scene.selectedCarry() ==
          PelicanStudio::GraphCanvasScene::CarryPick{"steps", "out", "target"});
    CHECK_FALSE(scene.selectedNode().has_value());
    pressKey(scene, Qt::Key_Delete);
    CHECK(h.commit() == Json::array({{{"op", "set_node_parameters"}, {"scope", "root"},
                                      {"node", "steps"}, {"parameters", parameters}}}));
    CHECK(scopedItem(scene, PelicanStudio::GraphCanvasZoneCarryItem, QString{},
                     QStringLiteral("steps"), QStringLiteral("out/target")) == nullptr);

    // A carry into an input the loop feeds nothing: the arrow is marked, with
    // the Loop fields' sentence.
    auto unfed = zoneDocument();
    unfed["nodes"][2].erase("inputs");
    unfed["nodes"][2]["parameters"]["carry"] = Json::array({{{"from", "out"}, {"to", "target"}}});
    ZoneHarness warned{unfed};
    warned.open();
    QGraphicsItem *mark = scopedItem(warned.scene, PelicanStudio::GraphCanvasZoneCarryMarkItem,
                                     QString{}, QStringLiteral("steps"),
                                     QStringLiteral("out/target"));
    REQUIRE(mark != nullptr);
    CHECK(mark->toolTip() ==
          QStringLiteral("target has no loop input: connect one for the first element to start from."));
}

TEST_CASE("LP-4d2 a line reading one element of a p[] is dashed and says which element",
          "[devstudio][graph-canvas][scene][lp4d2]") {
    (void)application();
    auto document = zoneDocument();
    document["nodes"].push_back({{"id", "other"}, {"op", "graph.loop"},
                                 {"parameters", {{"over", {{"range", {{"count", 2}}}}},
                                                 {"branches", Json::array({{{"body", "rest"}}})}}},
                                 {"inputs", {{"target", {{"node", "a"}, {"port", "value"}}}}},
                                 {"outputs", Json::array({"out", "out[]"})}});
    document["nodes"][2]["inputs"]["target"] = {
        {"node", "other"}, {"port", "out[]"}, {"element", {{"$expr", "element"}}}};
    ZoneHarness h{document};
    h.open();
    auto *line = dynamic_cast<QGraphicsPathItem *>(
        scopedEdge(h.scene, QString{}, QStringLiteral("other"), QStringLiteral("out[]"),
                   QStringLiteral("steps"), QStringLiteral("target")));
    REQUIRE(line != nullptr);
    CHECK(line->pen().style() == Qt::DashLine);
    CHECK(line->data(PelicanStudio::GraphCanvasEdgeElementRole).toString() ==
          QStringLiteral("element"));
    auto *label = dynamic_cast<QGraphicsSimpleTextItem *>(
        scopedItem(h.scene, PelicanStudio::GraphCanvasEdgeLabelItem, QString{},
                   QStringLiteral("steps"), QStringLiteral("target")));
    REQUIRE(label != nullptr);
    CHECK(label->text() == QStringLiteral("[element]"));
    // Over the middle of its line.
    const QPointF middle = line->mapToScene(line->path().pointAtPercent(0.5));
    CHECK(QLineF{label->sceneBoundingRect().center(), middle}.length() < 30.0);
    // Picking it keeps it dashed.
    h.scene.selectEdge(std::string{}, PelicanStudio::GraphCanvasEdge{"other", "out[]", "steps", "target"},
                       false);
    CHECK(line->pen().style() == Qt::DashLine);
    // An ordinary line is neither.
    auto *plain = dynamic_cast<QGraphicsPathItem *>(
        scopedEdge(h.scene, QString{}, QStringLiteral("a"), QStringLiteral("value"),
                   QStringLiteral("other"), QStringLiteral("target")));
    REQUIRE(plain != nullptr);
    CHECK(plain->pen().style() == Qt::SolidLine);
    CHECK(scopedItem(h.scene, PelicanStudio::GraphCanvasEdgeLabelItem, QString{},
                     QStringLiteral("other"), QStringLiteral("target")) == nullptr);
}

TEST_CASE("LP-4d2 Undo and Redo take a zone's input, output and carry back, byte for byte",
          "[devstudio][graph-canvas][scene][lp4d2]") {
    (void)application();
    ZoneHarness h{twinBodyDocument()};
    h.open();
    auto &scene = h.scene;
    const auto before = h.document.dump();

    // Decision 1: root a into the body's sum.b.
    drag(scene, centreOf(socketOf(scene, QString{}, "a", "value", "output")),
         centreOf(socketOf(scene, QStringLiteral("first"), "sum", "b", "input")));
    h.commit();
    // Decision 2: sum's value out of every body.
    drag(scene, centreOf(socketOf(scene, QStringLiteral("first"), "sum", "value", "output")),
         centreOf(zoneSlot(scene, "out:+")));
    CHECK(h.commit().at(0).at("op") == "set_outputs");
    // Decision 3: carried back into the new input.
    drag(scene,
         centreOf(scopedItem(scene, PelicanStudio::GraphCanvasZoneSocketItem, QString{},
                             QStringLiteral("steps"), QStringLiteral("out:value"))),
         centreOf(socketOf(scene, QStringLiteral("first"), "value", "value", "output")));
    h.commit();
    const auto after = h.document.dump();
    REQUIRE(h.document.at("nodes")[2].at("parameters").at("carry") ==
            Json::array({{{"from", "value"}, {"to", "value"}}}));

    for (int step = 0; step < 3; ++step) {
        REQUIRE(h.model.canUndo());
        scene.undoEdit();
        h.commit();
    }
    CHECK(h.document.dump() == before);
    for (int step = 0; step < 3; ++step) {
        REQUIRE(h.model.canRedo());
        scene.redoEdit();
        h.commit();
    }
    CHECK(h.document.dump() == after);
}

TEST_CASE("LP-4d2 a zone says what the loop became, and a draw_loop's tabs how many materials each",
          "[devstudio][graph-canvas][scene][lp4d2]") {
    (void)application();
    ZoneHarness h;
    h.loops = Json::array({{{"node", "steps"}, {"tier", "expand"}, {"count", 2},
                            {"elements", Json::array({{{"element", 0}, {"index", 0}, {"branch", 0}},
                                                      {{"element", 1}, {"index", 1}, {"branch", 1}}})},
                            {"unmatched_branches", Json::array()}}});
    h.open();
    h.answerReads();
    h.scene.repopulate();
    auto *line = dynamic_cast<QGraphicsSimpleTextItem *>(
        scopedItem(h.scene, PelicanStudio::GraphCanvasZoneLabelItem, QString{},
                   QStringLiteral("steps"), QStringLiteral("expansion")));
    REQUIRE(line != nullptr);
    CHECK(line->text() == QStringLiteral("×2 copies"));
    QGraphicsItem *frame = scopedNode(h.scene, QString{}, QStringLiteral("steps"));
    CHECK(frame->sceneBoundingRect().contains(line->sceneBoundingRect()));
    // Its button asks the owner to show the loop in the Frame Plan.
    QGraphicsItem *button = scopedItem(h.scene, PelicanStudio::GraphCanvasZoneButtonItem, QString{},
                                       QStringLiteral("steps"), QStringLiteral("frame_plan"));
    REQUIRE(button != nullptr);
    std::string asked;
    h.scene.showInFramePlanRequested = [&](const std::string &loop) { asked = loop; };
    click(h.scene, centreOf(button));
    CHECK(asked == "steps");
    CHECK(h.answerReads().empty());

    // A draw_loop: its materials, and each tab's share of them. No button:
    // a draw_loop makes no copies.
    auto document = zoneDocument();
    auto &loop = document["nodes"][2];
    loop["parameters"] = {{"over", {{"draw_items", {{"contract", "forward_opaque_v1"}}}}},
                          {"branches", Json::array({{{"when", {{"material", "m#a"}}}, {"body", "first"}},
                                                    {{"body", "rest"}}})}};
    ZoneHarness draws{document};
    draws.loops = Json::array({{{"node", "steps"}, {"tier", "draw_loop"}, {"count", 3}, {"total", 3},
                                {"elements", Json::array({{{"element", "m#a"}, {"index", 0}, {"branch", 0}},
                                                          {{"element", "m#b"}, {"index", 1}, {"branch", 1}},
                                                          {{"element", "m#c"}, {"index", 2}, {"branch", 1}}})},
                                {"unmatched_branches", Json::array()}}});
    draws.open();
    draws.answerReads();
    draws.scene.repopulate();
    auto *materials = dynamic_cast<QGraphicsSimpleTextItem *>(
        scopedItem(draws.scene, PelicanStudio::GraphCanvasZoneLabelItem, QString{},
                   QStringLiteral("steps"), QStringLiteral("expansion")));
    REQUIRE(materials != nullptr);
    CHECK(materials->text() == QStringLiteral("3 materials"));
    CHECK(scopedItem(draws.scene, PelicanStudio::GraphCanvasZoneButtonItem, QString{},
                     QStringLiteral("steps"), QStringLiteral("frame_plan")) == nullptr);
    const auto tabs = zoneTabs(draws.scene);
    REQUIRE(tabs.size() == 2);
    CHECK(tabs[0].second == QStringLiteral("if m#a (1)"));
    CHECK(tabs[1].second == QStringLiteral("else (2)"));
}

// Q-17, decision 4. "Shared" warns that an edit in a body reaches another
// loop or instance. Two branches of one loop running the same body are one
// place to edit it from, and are not counted twice; the count is of nodes.
TEST_CASE("Q-17 a body two branches of one loop run is not shared; another loop running it is",
          "[devstudio][graph-canvas][scene][q17]") {
    (void)application();
    auto document = zoneDocument();
    document["nodes"][2]["parameters"]["branches"] =
        Json::array({{{"when", {{"element", 0}}}, {"body", "first"}}, {{"body", "first"}}});
    const auto sharedMark = [](GraphCanvasScene &scene) {
        return dynamic_cast<QGraphicsSimpleTextItem *>(
            scopedItem(scene, PelicanStudio::GraphCanvasZoneLabelItem, QString{},
                       QStringLiteral("steps"), QStringLiteral("shared")));
    };
    {
        ZoneHarness one{document};
        one.open();
        one.answerReads();
        one.scene.repopulate();
        CHECK(one.model.definitionUseCount("first") == 1);
        CHECK(sharedMark(one.scene) == nullptr);
    }
    // The control: a second loop over the same body is a second user.
    auto again = document["nodes"][2];
    again["id"] = "again";
    document["nodes"].push_back(again);
    ZoneHarness two{document};
    two.open();
    two.answerReads();
    two.scene.repopulate();
    CHECK(two.model.definitionUseCount("first") == 2);
    auto *mark = sharedMark(two.scene);
    REQUIRE(mark != nullptr);
    CHECK(mark->text() == QStringLiteral("shared (2 users)"));
}

TEST_CASE("LP-4d2 the frame follows a body node while it is dragged, and a long pill keeps to its node",
          "[devstudio][graph-canvas][scene][lp4d2]") {
    (void)application();
    auto document = zoneDocument();
    // A fullscreen pass's parameters, which no catalog here declares: its
    // shader is a pill of JSON.
    document["definitions"]["first"]["nodes"].push_back(
        {{"id", "pass"}, {"op", "render.fullscreen"},
         {"parameters", {{"shader", {{"vertex", "engine://fullscreen"},
                                     {"fragment", "engine://passthrough_with_a_long_name"}}}}},
         {"outputs", Json::array({"color"})}});
    ZoneHarness h{document};
    h.open();
    auto &scene = h.scene;

    // The pill is cut to the node, and says the whole of it when hovered.
    QGraphicsItem *pass = scopedNode(scene, QStringLiteral("first"), QStringLiteral("pass"));
    REQUIRE(pass != nullptr);
    QGraphicsSimpleTextItem *pill_text = nullptr;
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasValuePillItem} &&
            item->data(PelicanStudio::GraphCanvasScopeRole).toString() == QStringLiteral("first") &&
            item->data(PelicanStudio::NodeGraphNameRole).toString() == QStringLiteral("pass"))
            if (auto *text = dynamic_cast<QGraphicsSimpleTextItem *>(item)) pill_text = text;
    REQUIRE(pill_text != nullptr);
    CHECK(pill_text->text().endsWith(QChar{0x2026}));
    CHECK(pill_text->toolTip().contains(QStringLiteral("engine://passthrough_with_a_long_name")));
    CHECK(pass->sceneBoundingRect().contains(pill_text->parentItem()->sceneBoundingRect()));

    // A body node taken past the frame: the frame is already round it, and
    // the right edge's sockets with it -- before the hand lets go, and
    // without the frame being drawn again.
    QGraphicsItem *frame = scopedNode(scene, QString{}, QStringLiteral("steps"));
    QGraphicsItem *sum = scopedNode(scene, QStringLiteral("first"), QStringLiteral("sum"));
    QGraphicsItem *out = socketOf(scene, QString{}, "steps", "out", "output");
    REQUIRE(frame != nullptr);
    REQUIRE(sum != nullptr);
    REQUIRE(out != nullptr);
    const qreal right_before = frame->sceneBoundingRect().right();
    sum->setPos(sum->pos() + QPointF{500.0, 300.0});
    CHECK(scopedNode(scene, QString{}, QStringLiteral("steps")) == frame);
    CHECK(frame->sceneBoundingRect().contains(sum->sceneBoundingRect()));
    CHECK(frame->sceneBoundingRect().right() > right_before + 400.0);
    CHECK(std::abs(centreOf(out).x() - frame->sceneBoundingRect().right()) < 8.0);
    // And the heading's hotspot is still on the heading.
    const QVariant spot = frame->data(PelicanStudio::NodeGraphHotspotRole);
    const auto under = scene.items(frame->mapToScene(spot.toPointF()));
    REQUIRE_FALSE(under.isEmpty());
    CHECK(under.front()->data(PelicanStudio::GraphCanvasHeadingRole).toBool());
}

TEST_CASE("LP-4d2 Show in Frame Plan asks the owner, and Open in Graph on a loop's copy picks the "
          "body node in the root's zone",
          "[devstudio][graph-canvas][scene][lp4d2]") {
    (void)application();
    RecordedCapability transport;
    auto seam = transport.capability();
    // A fake frame plan: loops[] only, two elements, the second taking the
    // else.
    seam.requestFramePlan = [&transport](QString *) {
        transport.calls.push_back(RecordedCapability::Call{QStringLiteral("get_frame_plan")});
        return transport.next++;
    };
    PelicanStudio::GraphCanvasWidget widget{seam};
    widget.show();
    QCoreApplication::processEvents();
    const auto answerRpc = [&](const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    };
    answerRpc(QStringLiteral("get_graph_instances"), context(zoneDocument()));
    answerRpc(QStringLiteral("get_graph_catalog"), visualCatalog());
    answerRpc(QStringLiteral("get_frame_plan"),
              Json{{"loops", Json::array({{{"node", "steps"}, {"tier", "expand"}, {"count", 2},
                                           {"elements", Json::array({
                                               {{"element", 0}, {"index", 0}, {"branch", 0}},
                                               {{"element", 1}, {"index", 1}, {"branch", 1}}})},
                                           {"unmatched_branches", Json::array()}}})}});
    auto &scene = widget.scene();
    REQUIRE(scene.zones() == std::map<std::string, std::string>{{"steps", "first"}});

    // Decision 5: the zone's button is the owner's Show in Frame Plan.
    QString asked;
    widget.setFramePlanLoopAction([&](const QString &loop) {
        asked = loop;
        return true;
    });
    QGraphicsItem *button = scopedItem(scene, PelicanStudio::GraphCanvasZoneButtonItem, QString{},
                                       QStringLiteral("steps"), QStringLiteral("frame_plan"));
    REQUIRE(button != nullptr);
    click(scene, centreOf(button));
    CHECK(asked == QStringLiteral("steps"));

    // Decision 6: element 1 ran the else. The canvas stays on the root, the
    // zone's tab goes to the else, and c is picked inside it.
    REQUIRE(widget.revealLoopOrigin(QStringLiteral("steps"), 1, QStringLiteral("c")));
    CHECK(widget.model().currentScope().empty());
    CHECK(widget.model().breadcrumb().size() == 1);
    CHECK(scene.zones() == std::map<std::string, std::string>{{"steps", "rest"}});
    CHECK(scene.selectedScope() == "rest");
    REQUIRE(scene.selectedNode().has_value());
    CHECK(*scene.selectedNode() == "c");
    CHECK(scopedNode(scene, QStringLiteral("rest"), QStringLiteral("c")) != nullptr);

    // A folded zone opens for it; element 0 is the first branch's.
    scene.setCollapsed(std::string{}, "steps", true);
    REQUIRE(scene.zones().empty());
    REQUIRE(widget.revealLoopOrigin(QStringLiteral("steps"), 0, QStringLiteral("sum")));
    CHECK(scene.zones() == std::map<std::string, std::string>{{"steps", "first"}});
    CHECK(scene.selectedScope() == "first");
    CHECK(*scene.selectedNode() == "sum");
    // A node that body does not hold: nothing moves.
    CHECK_FALSE(widget.revealLoopOrigin(QStringLiteral("steps"), 0, QStringLiteral("c")));
    CHECK(*scene.selectedNode() == "sum");
}

// --- LP-4f1: loops over a view axis, the Graph tab's View field, and the
// fields that no longer ask for typing.

namespace {

// What get_frame_plan lists on its passes: the cube's face (labelled), the
// directional shadow's light (decided each frame) and cascade, and $main,
// which has no axes.
Json lp4f1PlanNodes() {
    const Json face{{"axis", "face"},
                    {"length", 6},
                    {"runtime", false},
                    {"labels", Json::array({"+X", "-X", "+Y", "-Y", "+Z", "-Z"})}};
    return Json::array({
        {{"name", "cube_capture_ssao"}, {"kind", "render"}, {"view_family", "$capture/cube"},
         {"view_axes", Json::array({face})}},
        {{"name", "shadow_depth"},
         {"kind", "render"},
         {"view_family", "$shadow/directional"},
         {"view_axes", Json::array({{{"axis", "light"}, {"length", nullptr}, {"runtime", true}},
                                    {{"axis", "cascade"}, {"length", 3}, {"runtime", false}}})}},
        {{"name", "present"}, {"kind", "render"}, {"view_family", "$main"},
         {"view_axes", Json::array()}},
        {{"name", "outside"}, {"kind", "render"}, {"view_family", "$capture/cube"},
         {"view_axes", Json::array({face})}}});
}

Json lp4f1Plan() {
    return Json{{"nodes", lp4f1PlanNodes()},
                {"resources", Json::array({{{"name", "bloom_chain"}, {"kind", "render_target"}},
                                           {{"name", "scratch"}, {"kind", "buffer"}}})}};
}

// A loop over the cube's faces running `faces`, a loop over a count running
// `counted_body`, both bodies one fullscreen pass, and a fullscreen pass in
// the root the frame plan lists in the cube's family.
Json lp4f1Document() {
    const Json pass{{"id", "pass"},
                    {"op", "render.fullscreen"},
                    {"parameters",
                     {{"shader", {{"vertex", "engine://fullscreen"},
                                  {"fragment", "engine://passthrough"}}}}},
                    {"outputs", Json::array({"color"})}};
    Json outside = pass;
    outside["id"] = "outside";
    outside["parameters"]["view_family"] = "$capture/cube";
    return Json{{"schema", "pelican.graph"},
                {"version", 1},
                {"outputs", Json::object()},
                {"definitions",
                 {{"faces", {{"nodes", Json::array({pass})}, {"outputs", Json::object()}}},
                  {"counted_body", {{"nodes", Json::array({pass})}, {"outputs", Json::object()}}}}},
                {"nodes",
                 Json::array({{{"id", "faces_loop"},
                               {"op", "graph.loop"},
                               {"parameters",
                                {{"over", {{"view_axis", {{"family", "$capture/cube"},
                                                          {"axis", "face"}}}}},
                                 {"branches", Json::array({{{"body", "faces"}}})}}},
                               {"outputs", Json::array()}},
                              {{"id", "counted"},
                               {"op", "graph.loop"},
                               {"parameters", {{"over", {{"range", {{"count", 2}}}}},
                                               {"branches", Json::array({{{"body", "counted_body"}}})}}},
                               {"outputs", Json::array()}},
                              outside})}};
}

PelicanStudio::GraphCanvasNode lp4f1Node(const GraphCanvasModel &model, const std::string &scope,
                                          const std::string &id) {
    const auto nodes = model.nodes(scope);
    const auto found = std::ranges::find(nodes, id, &PelicanStudio::GraphCanvasNode::id);
    REQUIRE(found != nodes.end());
    return *found;
}

template <typename T> T *lp4f1Field(QWidget &fields, const char *name) {
    auto *found = fields.findChild<T *>(QString::fromLatin1(name));
    REQUIRE(found != nullptr);
    return found;
}

} // namespace

TEST_CASE("LP-4f1 the Loop fields run over a view axis the frame plan lists, never one "
          "decided each frame",
          "[devstudio][graph-canvas][scene][lp4f1]") {
    (void)application();
    ZoneHarness h;
    h.plan = lp4f1Plan();
    h.open();
    h.answerReads();
    PelicanStudio::GraphCanvasLoopFields fields{h.model, [] {}, {}};
    fields.showLoop("steps");
    auto *over = lp4f1Field<QComboBox>(fields, "pelican.graphCanvasLoop.over");
    CHECK(over->currentData().toString() == QStringLiteral("range"));
    // The candidates are the families a pass of the plan runs in, with
    // axes: $main has none and is not one.
    REQUIRE(h.model.viewFamilies().size() == 2);

    // View axis: the first family's first axis a loop can run over, in one
    // edit that keeps the rest of the loop.
    over->setCurrentIndex(over->findData(QStringLiteral("view_axis")));
    auto ops = h.commit();
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].at("op") == "set_node_parameters");
    CHECK(ops[0].at("parameters").at("over") ==
          Json{{"view_axis", {{"family", "$capture/cube"}, {"axis", "face"}}}});
    CHECK(ops[0].at("parameters").at("branches") ==
          zoneDocument().at("nodes")[2].at("parameters").at("branches"));
    fields.showLoop("steps");
    auto *family = lp4f1Field<QComboBox>(fields, "pelican.graphCanvasLoop.viewFamily");
    auto *axis = lp4f1Field<QComboBox>(fields, "pelican.graphCanvasLoop.viewAxis");
    auto *length = lp4f1Field<QLabel>(fields, "pelican.graphCanvasLoop.viewCount");
    REQUIRE(family->count() == 2);
    CHECK(family->itemData(0).toString() == QStringLiteral("$capture/cube"));
    CHECK(family->itemData(1).toString() == QStringLiteral("$shadow/directional"));
    CHECK(family->findData(QStringLiteral("$main")) < 0);
    CHECK(axis->currentData().toString() == QStringLiteral("face"));
    // The count is the axis's length, read only; the count field goes.
    CHECK(length->text() == QStringLiteral("×6 (face)"));
    CHECK(length->isVisibleTo(&fields));
    CHECK_FALSE(lp4f1Field<QSpinBox>(fields, "pelican.graphCanvasLoop.count")->isVisibleTo(&fields));
    // One line under the field says why a family is not there.
    CHECK(lp4f1Field<QLabel>(fields, "pelican.graphCanvasLoop.viewAxisNote")->isVisibleTo(&fields));

    // Another family: its first axis a loop can run over (cascade, not light).
    family->setCurrentIndex(1);
    ops = h.commit();
    CHECK(ops[0].at("parameters").at("over") ==
          Json{{"view_axis", {{"family", "$shadow/directional"}, {"axis", "cascade"}}}});
    fields.showLoop("steps");
    REQUIRE(axis->count() == 2);
    const auto *items = qobject_cast<QStandardItemModel *>(axis->model());
    REQUIRE(items != nullptr);
    CHECK(axis->itemData(0).toString() == QStringLiteral("light"));
    CHECK_FALSE(items->item(0)->isEnabled());
    CHECK(axis->itemText(0).contains(QStringLiteral("decided each frame")));
    CHECK(items->item(1)->isEnabled());
    CHECK(axis->currentData().toString() == QStringLiteral("cascade"));
    CHECK(length->text() == QStringLiteral("×3 (cascade)"));
    // Forced onto the axis decided each frame, nothing is sent.
    axis->setCurrentIndex(0);
    CHECK(h.answerReads().empty());

    // Back to a count: as many elements as the axis had.
    fields.showLoop("steps");
    over->setCurrentIndex(over->findData(QStringLiteral("range")));
    ops = h.commit();
    CHECK(ops[0].at("parameters").at("over") == Json{{"range", {{"count", 3}}}});
}

TEST_CASE("LP-4f1 a plan without a family with axes offers no view axis, and says so",
          "[devstudio][graph-canvas][scene][lp4f1]") {
    (void)application();
    ZoneHarness h;
    h.plan = Json{{"nodes", Json::array({lp4f1PlanNodes()[2]})}};
    h.open();
    h.answerReads();
    PelicanStudio::GraphCanvasLoopFields fields{h.model, [] {}, {}};
    fields.showLoop("steps");
    auto *over = lp4f1Field<QComboBox>(fields, "pelican.graphCanvasLoop.over");
    over->setCurrentIndex(over->findData(QStringLiteral("view_axis")));
    CHECK(h.answerReads().empty());
    CHECK(over->currentData().toString() == QStringLiteral("range"));
    auto *note = lp4f1Field<QLabel>(fields, "pelican.graphCanvasLoop.viewAxisNote");
    CHECK(note->isVisibleTo(&fields));
    CHECK_FALSE(note->text().isEmpty());
}

TEST_CASE("LP-4f1 where, the branch conditions and the count are chosen, not typed",
          "[devstudio][graph-canvas][scene][lp4f1]") {
    (void)application();
    ZoneHarness h;
    h.plan = lp4f1Plan();
    h.open();
    h.answerReads();
    PelicanStudio::GraphCanvasLoopFields fields{h.model, [] {}, {}};
    fields.showLoop("steps");

    // Where: over a count of 2, a check per element instead of a line.
    CHECK_FALSE(lp4f1Field<QLineEdit>(fields, "pelican.graphCanvasLoop.whereValues")->isVisibleTo(&fields));
    auto *kind = lp4f1Field<QComboBox>(fields, "pelican.graphCanvasLoop.whereKind");
    auto *second = lp4f1Field<QCheckBox>(fields, "pelican.graphCanvasLoop.whereValue1");
    CHECK(second->text() == QStringLiteral("1"));
    CHECK_FALSE(second->isEnabled());
    // "element in" with nothing checked yet sends nothing; the check does.
    kind->setCurrentIndex(kind->findData(QStringLiteral("element_in")));
    CHECK(h.answerReads().empty());
    CHECK(second->isEnabled());
    second->setChecked(true);
    auto ops = h.commit();
    CHECK(ops[0].at("parameters").at("where") == Json{{"element_in", {1}}});

    // The branch table: `element =` is a spin.
    fields.showLoop("steps");
    CHECK(fields.findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasLoop.branch0.values")) ==
          nullptr);
    auto *value = lp4f1Field<QSpinBox>(fields, "pelican.graphCanvasLoop.branch0.value");
    CHECK(value->value() == 0);
    CHECK(value->maximum() == 1);
    value->setValue(1);
    ops = h.commit();
    CHECK(ops[0].at("parameters").at("branches")[0].at("when") == Json{{"element", 1}});
    // `element in`: the spin's value is the first check, and a check adds one.
    fields.showLoop("steps");
    auto *when = lp4f1Field<QComboBox>(fields, "pelican.graphCanvasLoop.branch0.when");
    when->setCurrentIndex(when->findData(QStringLiteral("element_in")));
    ops = h.commit();
    CHECK(ops[0].at("parameters").at("branches")[0].at("when") == Json{{"element_in", {1}}});
    fields.showLoop("steps");
    CHECK(lp4f1Field<QCheckBox>(fields, "pelican.graphCanvasLoop.branch0.check1")->isChecked());
    auto *first = lp4f1Field<QCheckBox>(fields, "pelican.graphCanvasLoop.branch0.check0");
    CHECK_FALSE(first->isChecked());
    first->setChecked(true);
    ops = h.commit();
    CHECK(ops[0].at("parameters").at("branches")[0].at("when") == Json{{"element_in", {0, 1}}});

    // The count expression: a target of the plan (render targets only), and
    // its mip levels put in with a button.
    fields.showLoop("steps");
    auto *toggle = lp4f1Field<QToolButton>(fields, "pelican.graphCanvasLoop.countExpressionToggle");
    auto *insert_row = lp4f1Field<QPushButton>(fields, "pelican.graphCanvasLoop.insertMipLevels");
    CHECK_FALSE(insert_row->isVisibleTo(&fields));
    toggle->setChecked(true);
    CHECK(insert_row->isVisibleTo(&fields));
    auto *target = lp4f1Field<QComboBox>(fields, "pelican.graphCanvasLoop.countTarget");
    REQUIRE(target->count() == 1);
    CHECK(target->itemText(0) == QStringLiteral("bloom_chain"));
    insert_row->click();
    ops = h.commit();
    CHECK(ops[0].at("parameters").at("over") ==
          Json{{"range", {{"count", {{"$expr", "mip_levels_of(\"bloom_chain\")"}}}}}});
    // Into an expression already there, it goes where the cursor is.
    fields.showLoop("steps");
    auto *expression = lp4f1Field<QLineEdit>(fields, "pelican.graphCanvasLoop.countExpression");
    expression->setText(QStringLiteral(" - 1"));
    expression->setCursorPosition(0);
    lp4f1Field<QPushButton>(fields, "pelican.graphCanvasLoop.insertLayers")->click();
    ops = h.commit();
    CHECK(ops[0].at("parameters").at("over") ==
          Json{{"range", {{"count", {{"$expr", "layers_of(\"bloom_chain\") - 1"}}}}}});

    // Per iteration: element, element - 1, element + 1, or Custom... .
    PelicanStudio::GraphCanvasElementChoice choice{QStringLiteral("choice")};
    auto *combo = lp4f1Field<QComboBox>(choice, "choice");
    auto *custom = lp4f1Field<QLineEdit>(choice, "choice.custom");
    CHECK(choice.expression() == QStringLiteral("element"));
    combo->setCurrentIndex(1);
    CHECK(choice.expression() == QStringLiteral("element - 1"));
    combo->setCurrentIndex(2);
    CHECK(choice.expression() == QStringLiteral("element + 1"));
    CHECK_FALSE(custom->isVisibleTo(&choice));
    combo->setCurrentIndex(3);
    CHECK(custom->isVisibleTo(&choice));
    CHECK(choice.expression() == QStringLiteral("element"));
    custom->setText(QStringLiteral("element * 2"));
    CHECK(choice.expression() == QStringLiteral("element * 2"));
    choice.setExpression(QStringLiteral("element + 1"));
    CHECK(combo->currentIndex() == 2);
}

TEST_CASE("LP-4f1 a view-axis loop's body offers Each element on its axis, and nothing else does",
          "[devstudio][graph-canvas][scene][lp4f1]") {
    (void)application();
    ZoneHarness h{lp4f1Document()};
    h.plan = lp4f1Plan();
    h.open();
    h.answerReads();
    int submitted = 0;
    PelicanStudio::GraphCanvasViewFields view{h.model, [&] { ++submitted; }};

    // In the body of the loop over the cube's faces.
    auto pass = lp4f1Node(h.model, "faces", "pass");
    view.showNode(&pass, "faces");
    CHECK_FALSE(view.isHidden());
    auto *face = lp4f1Field<QComboBox>(view, "pelican.graphCanvasRender.view.face");
    CHECK(face->currentText() == QStringLiteral("All views"));
    CHECK(face->findText(QStringLiteral("2 (+Y)")) >= 0);
    const int each = face->findText(QStringLiteral("Each element (loop)"));
    REQUIRE(each >= 0);
    face->setCurrentIndex(each);
    CHECK(submitted == 1);
    auto ops = h.commit();
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].at("op") == "set_node_parameters");
    CHECK(ops[0].at("scope") == "faces");
    CHECK(ops[0].at("node") == "pass");
    CHECK(ops[0].at("parameters").at("view_select") == Json{{"face", {{"$expr", "element"}}}});
    // The pass named no family: the loop's is written with it.
    CHECK(ops[0].at("parameters").at("view_family") == "$capture/cube");
    CHECK(ops[0].at("parameters").at("shader") ==
          lp4f1Document().at("definitions").at("faces").at("nodes")[0].at("parameters").at("shader"));

    // Read back, Each element is what the field shows; a value replaces it.
    pass = lp4f1Node(h.model, "faces", "pass");
    view.showNode(&pass, "faces");
    face = lp4f1Field<QComboBox>(view, "pelican.graphCanvasRender.view.face");
    CHECK(face->currentText() == QStringLiteral("Each element (loop)"));
    face->setCurrentIndex(face->findText(QStringLiteral("2 (+Y)")));
    ops = h.commit();
    CHECK(ops[0].at("parameters").at("view_select") == Json{{"face", 2}});
    // All views takes the member out.
    pass = lp4f1Node(h.model, "faces", "pass");
    view.showNode(&pass, "faces");
    face = lp4f1Field<QComboBox>(view, "pelican.graphCanvasRender.view.face");
    face->setCurrentIndex(0);
    ops = h.commit();
    CHECK_FALSE(ops[0].at("parameters").contains("view_select"));

    // Outside the loop: the root pass the plan lists in the cube's family
    // has its View field, without Each element.
    auto outside = lp4f1Node(h.model, "", "outside");
    view.showNode(&outside, "");
    CHECK_FALSE(view.isHidden());
    face = lp4f1Field<QComboBox>(view, "pelican.graphCanvasRender.view.face");
    CHECK(face->findText(QStringLiteral("Each element (loop)")) < 0);
    CHECK(face->findText(QStringLiteral("5 (-Z)")) >= 0);
    // In the body of a loop over a count, a pass with no family: no field.
    auto counted = lp4f1Node(h.model, "counted_body", "pass");
    view.showNode(&counted, "counted_body");
    CHECK(view.isHidden());
    // Nor for a node that takes no view_select.
    view.showNode(nullptr, {});
    CHECK(view.isHidden());
}

// LP-4f3 (VF-2): the engine holds a view family's passes to one size, and
// get_frame_plan says which on every node of the family (family_extent,
// family_layers, family_extent_pass). A pass put in the body of a loop over
// that family's axis draws into an image made at that size.

namespace {

// The render operations, which declare no ports of their own, as the engine
// publishes them.
void addRenderOperations(ZoneHarness &h) {
    for (const char *op : {"render.image", "render.fullscreen"})
        h.catalog["modules"][1]["operations"].push_back({{"op", op},
                                                         {"domain", "rendering"},
                                                         {"evaluation", "direct"},
                                                         {"sink", false},
                                                         {"ports", "parameter_dependent"}});
}

// lp4f1PlanNodes with the sizes VF-2 writes: the cube's 256 x 256, six
// layers, set by cube_capture_geometry; the shadow's half the output's size,
// three cascades. $main carries none.
Json lp4f3PlanNodes(const Json &cube_extent = Json::array({256, 256})) {
    auto nodes = lp4f1PlanNodes();
    for (const auto index : {0, 3}) {
        nodes[index]["family_extent"] = cube_extent;
        nodes[index]["family_layers"] = 6;
        nodes[index]["family_extent_pass"] = "cube_capture_geometry";
    }
    nodes[1]["family_extent"] = {{"output_scale", 0.5}};
    nodes[1]["family_layers"] = 3;
    nodes[1]["family_extent_pass"] = "shadow_depth";
    return nodes;
}

} // namespace

TEST_CASE("LP-4f3 Add fullscreen pass in a view-axis loop's body makes its image at the "
          "family_extent the frame plan gives",
          "[devstudio][graph-canvas][scene][lp4f3][vf2]") {
    (void)application();
    ZoneHarness h{lp4f1Document()};
    addRenderOperations(h);
    h.plan = Json{{"nodes", lp4f3PlanNodes()}};
    h.open();
    h.answerReads();
    CHECK(h.model.viewAxisFamilyOf("faces") == "$capture/cube");
    CHECK(h.model.viewAxisFamilyOf("counted_body").empty());
    CHECK(h.model.viewAxisFamilyOf("").empty());
    CHECK(h.model.viewFamilyImageExtent("$capture/cube") ==
          Json{{"width", 256}, {"height", 256}, {"layers", 6}});
    CHECK(h.model.viewFamilyImageExtent("$shadow/directional") ==
          Json{{"extent_scale", 0.5}, {"layers", 3}});
    CHECK_FALSE(h.model.viewFamilyImageExtent("$main").has_value());

    // Add fullscreen pass, in the cube loop's body: a 256 x 256 image of six
    // layers, and the pass in the cube's family, resolved on its own.
    REQUIRE(h.scene.addFromPalette("faces", "render.fullscreen", {}));
    const auto ops = h.commit(false);
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].at("scope") == "faces");
    const auto image = ops[0].at("node").at("parameters");
    INFO(image.dump());
    CHECK(ops[0].at("node").at("op") == "render.image");
    CHECK(image.at("width") == 256);
    CHECK(image.at("height") == 256);
    CHECK(image.at("layers") == 6);
    const auto pass = ops[1].at("node").at("parameters");
    CHECK(ops[1].at("node").at("op") == "render.fullscreen");
    CHECK(pass.at("view_family") == "$capture/cube");
    CHECK(pass.at("resolution_domain") == "independent");

    // In the body of a loop over a count: as before -- the screen's size, and
    // no family named.
    const auto counted = h.model.addNodeOpsIn("counted_body", "render.fullscreen", {});
    REQUIRE(counted.size() == 2);
    CHECK_FALSE(counted[0].at("node").at("parameters").contains("width"));
    CHECK(counted[0].at("node").at("parameters").at("extent_scale") == 1.0);
    CHECK_FALSE(counted[1].at("node").at("parameters").contains("view_family"));
    CHECK_FALSE(counted[1].at("node").at("parameters").contains("resolution_domain"));
}

TEST_CASE("LP-4f3 a family_extent relative to the output makes the image at that scale",
          "[devstudio][graph-canvas][scene][lp4f3][vf2]") {
    (void)application();
    ZoneHarness h{lp4f1Document()};
    addRenderOperations(h);
    h.plan = Json{{"nodes", lp4f3PlanNodes(Json{{"output_scale", 0.5}})}};
    h.open();
    h.answerReads();
    const auto ops = h.model.addNodeOpsIn("faces", "render.fullscreen", {});
    REQUIRE(ops.size() == 2);
    const auto image = ops[0].at("node").at("parameters");
    INFO(image.dump());
    CHECK(image.at("extent_scale") == 0.5);
    CHECK_FALSE(image.contains("width"));
    CHECK(image.at("layers") == 6);
    // Relative to the screen, the pass is resolved with it.
    CHECK(ops[1].at("node").at("parameters").at("view_family") == "$capture/cube");
    CHECK_FALSE(ops[1].at("node").at("parameters").contains("resolution_domain"));
}

TEST_CASE("LP-4f3 without family_extent the image is made as before, in the loop's family",
          "[devstudio][graph-canvas][scene][lp4f3][vf2]") {
    (void)application();
    // A plan that carries none, and one whose family_extent Studio cannot read.
    auto unreadable = lp4f1PlanNodes();
    unreadable[0]["family_extent"] = "256x256";
    unreadable[3]["family_extent"] = Json::array({256});
    for (const auto &plan : {lp4f1Plan(), Json{{"nodes", unreadable}}}) {
        ZoneHarness h{lp4f1Document()};
        addRenderOperations(h);
        h.plan = plan;
        h.open();
        h.answerReads();
        CHECK_FALSE(h.model.viewFamilyImageExtent("$capture/cube").has_value());
        const auto ops = h.model.addNodeOpsIn("faces", "render.fullscreen", {});
        REQUIRE(ops.size() == 2);
        // The engine names a mismatch, if there is one
        // (render.view_family_extent_mismatch@1).
        CHECK_FALSE(ops[0].at("node").at("parameters").contains("width"));
        CHECK(ops[0].at("node").at("parameters").at("extent_scale") == 1.0);
        CHECK(ops[1].at("node").at("parameters").at("view_family") == "$capture/cube");
        CHECK_FALSE(ops[1].at("node").at("parameters").contains("resolution_domain"));
    }
}

// Q-20: the Graph tab reads family_extent with the Frame Plan's own reader.
// A node whose value that reader refuses is passed over without a word -- the
// Frame Plan tab is where it is named -- and the rest of the plan is read: the
// next node of the same family gives the size, and the other families theirs.
TEST_CASE("Q-20 a family_extent the Frame Plan's reader refuses is passed over silently, and the "
          "rest of the plan is read",
          "[devstudio][graph-canvas][scene][lp4f3][q20]") {
    (void)application();
    auto nodes = lp4f3PlanNodes();
    nodes[0]["family_extent"] = "256x256";
    nodes[3]["family_extent"] = Json::array({128, 128});
    ZoneHarness h{lp4f1Document()};
    addRenderOperations(h);
    h.plan = Json{{"nodes", nodes}};
    h.open();
    h.answerReads();
    CHECK(h.model.viewFamilyImageExtent("$capture/cube") ==
          Json{{"width", 128}, {"height", 128}, {"layers", 6}});
    CHECK(h.model.viewFamilyImageExtent("$shadow/directional") ==
          Json{{"extent_scale", 0.5}, {"layers", 3}});
    CHECK(h.model.notice().kind != PelicanStudio::InspectorNoticeKind::Error);
}

// LP-4f3, decision 3: the size comes with the frame plan the Graph tab reads
// already. A document holding a loop over a view axis asks for nothing else.
TEST_CASE("LP-4f3 a view-axis loop does not make the Graph tab read the render authoring "
          "context",
          "[devstudio][graph-canvas][scene][lp4f3][vf2]") {
    (void)application();
    ZoneHarness h{lp4f1Document()};
    addRenderOperations(h);
    h.plan = Json{{"nodes", lp4f3PlanNodes()}};
    h.open();
    h.answerReads();
    // An edit reads the document and the plan again: still nothing else.
    REQUIRE(h.model.setLoopCount("counted", 3));
    (void)h.commit(false);
    h.answerReads();
    INFO(Json(h.sent).dump());
    CHECK(std::ranges::count(h.sent, std::string{"get_frame_plan"}) >= 2);
    CHECK(std::ranges::find(h.sent, std::string{"get_render_authoring_context"}) ==
          h.sent.end());
    // And the size is there all the same.
    CHECK(h.model.viewFamilyImageExtent("$capture/cube").has_value());
}

// VF-1's refusal of a copy a loop made marks the loop and its body node.
TEST_CASE("LP-4f2 a view family's size refusal of a loop's copy marks the zone's node",
          "[devstudio][graph-canvas][scene][lp4f2][vf1][refusal]") {
    (void)application();
    ZoneHarness h{lp4f1Document()};
    h.plan = lp4f1Plan();
    h.open();
    h.answerReads();
    const Json error{{"code", "render.view_family_extent_mismatch@1"},
                     {"family", "$capture/cube"},
                     {"pass", "faces_loop/0/pass"},
                     {"target", "faces_loop/0/pass__color"},
                     {"extent", {{"output_scale", 1.0}}},
                     {"expected_pass", "cube_capture_ssao"},
                     {"expected_target", "cube_capture_ao"},
                     {"expected_extent", Json::array({256, 256})},
                     {"loop_origin", {{"loop", "faces_loop"}, {"element", 0}, {"node", "pass"}}}};
    REQUIRE(h.model.setLoopCount("counted", 3));
    const auto edits = h.answerReads();
    REQUIRE(edits.size() == 1);
    h.model.receiveRpcResult(edits.front().request_id,
                             Json{{"status", "rejected"}, {"error", error}}.dump());
    // The sentence, and the marks on the loop and on the body node it copies.
    CHECK(h.model.notice().message == PelicanStudio::viewFamilyExtentRefusalDescription(error));
    const auto &marks = h.model.refusalMarks();
    CHECK(std::ranges::find(marks, PelicanStudio::GraphCanvasRefusalMark{"", "faces_loop", h.model.notice().message}) !=
          marks.end());
    CHECK(std::ranges::find(marks, PelicanStudio::GraphCanvasRefusalMark{"faces", "pass", h.model.notice().message}) !=
          marks.end());
}

TEST_CASE("LP-4f1 a view-axis zone says how many views along which axis, and names them",
          "[devstudio][graph-canvas][scene][lp4f1]") {
    (void)application();
    ZoneHarness h{lp4f1Document()};
    h.plan = lp4f1Plan();
    h.open();
    h.answerReads();
    h.scene.repopulate();
    auto *stage = dynamic_cast<QGraphicsSimpleTextItem *>(
        scopedItem(h.scene, PelicanStudio::GraphCanvasZoneLabelItem, QString{},
                   QStringLiteral("faces_loop"), QStringLiteral("stage")));
    REQUIRE(stage != nullptr);
    CHECK(stage->text() == QStringLiteral("×6 (face)"));
    CHECK(zoneTabs(h.scene).empty());
    const QStringList expected{QStringLiteral("+X"), QStringLiteral("-X"), QStringLiteral("+Y"),
                               QStringLiteral("-Y"), QStringLiteral("+Z"), QStringLiteral("-Z")};
    QGraphicsItem *frame = scopedNode(h.scene, QString{}, QStringLiteral("faces_loop"));
    REQUIRE(frame != nullptr);
    for (int index = 0; index < expected.size(); ++index) {
        auto *label = dynamic_cast<QGraphicsSimpleTextItem *>(
            scopedItem(h.scene, PelicanStudio::GraphCanvasZoneLabelItem, QString{},
                       QStringLiteral("faces_loop"), QStringLiteral("axis%1").arg(index)));
        REQUIRE(label != nullptr);
        CHECK(label->text() == expected[index]);
        CHECK(frame->sceneBoundingRect().contains(label->sceneBoundingRect()));
    }
    // The loop over a count keeps its old heading, and names nothing.
    CHECK(scopedItem(h.scene, PelicanStudio::GraphCanvasZoneLabelItem, QString{},
                     QStringLiteral("counted"), QStringLiteral("axis0")) == nullptr);
}

TEST_CASE("LP-4f1 a node added beside a zone lands clear of its frame and of every other box",
          "[devstudio][graph-canvas][scene][lp4f1]") {
    (void)application();
    ZoneHarness h;
    h.open();
    h.answerReads();
    h.scene.repopulate();
    QGraphicsItem *frame = scopedNode(h.scene, QString{}, QStringLiteral("steps"));
    REQUIRE(frame != nullptr);
    // The frame is wider than a node: the old 240 would land inside it.
    REQUIRE(frame->sceneBoundingRect().width() > PelicanStudio::NodeGraphNodeWidth +
                                                     PelicanStudio::NodeGraphHorizontalGap);
    h.scene.selectNode(std::string{}, "steps");
    REQUIRE(h.scene.addFromPalette(std::string{}, "value.constant", std::string{}));
    const auto ops = h.commit();
    const std::string added = ops.back().at("node").at("id").get<std::string>();
    QGraphicsItem *node = scopedNode(h.scene, QString{}, QString::fromStdString(added));
    REQUIRE(node != nullptr);
    frame = scopedNode(h.scene, QString{}, QStringLiteral("steps"));
    REQUIRE(frame != nullptr);
    CHECK(node->sceneBoundingRect().left() >= frame->sceneBoundingRect().right());
    for (QGraphicsItem *item : h.scene.items())
        if (item != node &&
            item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasNodeItem} &&
            item->data(PelicanStudio::GraphCanvasScopeRole).toString().isEmpty())
            CHECK_FALSE(node->sceneBoundingRect().intersects(item->sceneBoundingRect()));
}

// --- Q-18. A Graph tab edit that commits can change the frame plan: the tab
// reads the plan again after it, and tells its owner (who has the Frame Plan
// read it too). A placement changes no plan, and tells nobody. The Draw
// fields and a render node's View field follow the edit's round trip as the
// Loop fields do (Q-17).

namespace {

Json q18Plan(int count) {
    Json elements = Json::array();
    for (int element = 0; element < count; ++element)
        elements.push_back({{"element", element}, {"index", element}, {"branch", element == 0 ? 0 : 1}});
    return Json{{"loops", Json::array({{{"node", "steps"}, {"tier", "expand"}, {"count", count},
                                        {"elements", elements},
                                        {"unmatched_branches", Json::array()}}})}};
}

Json q18Context(const Json &document, const std::string &digest) {
    auto read = context(document);
    read["source_digest"]["hex"] = digest;
    return read;
}

// The widget over a RecordedCapability that also reads the frame plan.
struct Q18Tab {
    RecordedCapability transport;
    std::unique_ptr<PelicanStudio::GraphCanvasWidget> widget;
    int told = 0;

    Q18Tab() {
        auto capability = transport.capability();
        capability.requestFramePlan = [this](QString *) {
            transport.calls.push_back({QStringLiteral("get_frame_plan")});
            return transport.next++;
        };
        widget = std::make_unique<PelicanStudio::GraphCanvasWidget>(capability);
        widget->setPlanCommitAction([this] { ++told; });
        widget->resize(1100, 700);
        widget->show();
        QCoreApplication::processEvents();
    }
    void answer(const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        REQUIRE(transport.deliver);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    }
    [[nodiscard]] int reads(const char *method) const {
        return transport.countOf(QString::fromLatin1(method));
    }
    // The edit just sent is accepted and committed at `digest`.
    void commit(const std::string &digest) {
        answer(QStringLiteral("edit_graph_document"), Json{{"status", "accepted"}, {"ticket", "q18"}});
        answer(QStringLiteral("get_edit_result"),
               Json{{"status", "committed"}, {"committed", true},
                    {"source_digest", {{"hex", digest}}}});
    }
    [[nodiscard]] QString expansion() {
        auto *line = dynamic_cast<QGraphicsSimpleTextItem *>(
            scopedItem(widget->scene(), PelicanStudio::GraphCanvasZoneLabelItem, QString{},
                       QStringLiteral("steps"), QStringLiteral("expansion")));
        REQUIRE(line != nullptr);
        return line->text();
    }
    template <typename T> T *field(const char *name) {
        auto *found = widget->findChild<T *>(QString::fromLatin1(name));
        REQUIRE(found != nullptr);
        return found;
    }
};

Json withCount(Json document, int count) {
    document["nodes"][2]["parameters"]["over"] = {{"range", {{"count", count}}}};
    return document;
}

} // namespace

TEST_CASE("Q-18 a Graph tab commit reads the plan again and tells the owner; a placement tells "
          "nobody",
          "[devstudio][graph-canvas][scene][q18]") {
    (void)application();
    Q18Tab tab;
    auto document = zoneDocument();
    tab.answer(QStringLiteral("get_graph_instances"), q18Context(document, "q18-0"));
    tab.answer(QStringLiteral("get_graph_catalog"), visualCatalog());
    REQUIRE(tab.reads("get_frame_plan") == 1);
    tab.answer(QStringLiteral("get_frame_plan"), q18Plan(2));
    CHECK(tab.expansion() == QStringLiteral("×2 copies"));

    // The count, from the Loop fields: one edit.
    tab.widget->scene().selectNode("steps");
    QCoreApplication::processEvents();
    auto *count = tab.field<QSpinBox>("pelican.graphCanvasLoop.count");
    count->setValue(3);
    Q_EMIT count->editingFinished();
    QCoreApplication::processEvents();
    REQUIRE(tab.reads("edit_graph_document") == 1);
    // Accepted is not committed: nobody is told yet.
    tab.answer(QStringLiteral("edit_graph_document"), Json{{"status", "accepted"}, {"ticket", "q18"}});
    CHECK(tab.told == 0);
    // Committed: the owner is told once, at the commit.
    const int plans = tab.reads("get_frame_plan");
    tab.answer(QStringLiteral("get_edit_result"),
               Json{{"status", "committed"}, {"committed", true},
                    {"source_digest", {{"hex", "q18-1"}}}});
    CHECK(tab.told == 1);
    // The document read back, the plan follows it, and the zone says so.
    document = withCount(document, 3);
    tab.answer(QStringLiteral("get_graph_instances"), q18Context(document, "q18-1"));
    REQUIRE(tab.reads("get_frame_plan") == plans + 1);
    tab.answer(QStringLiteral("get_frame_plan"), q18Plan(3));
    CHECK(tab.expansion() == QStringLiteral("×3 copies"));

    // A placement -- the Frame Plan's node positions are written this way --
    // commits, and changes no plan: nobody is told, and the plan is not read.
    const Json placed{{"main_render", {{"nodes", {{"pass", {{"x", 12.0}, {"y", 4.0}}}}}}}};
    tab.widget->scene().setLayoutMember("frame_plan", placed);
    tab.widget->scene().flushLayout();
    tab.widget->poll();
    QCoreApplication::processEvents();
    REQUIRE(tab.reads("edit_graph_document") == 2);
    for (const auto &op : Json::parse(tab.transport.calls.back().body.toStdString()))
        REQUIRE(op.at("op") == "set_layout");
    const int before_placement = tab.reads("get_frame_plan");
    tab.commit("q18-2");
    CHECK(tab.told == 1);
    document["layout"] = {{"frame_plan", placed}};
    tab.answer(QStringLiteral("get_graph_instances"), q18Context(document, "q18-2"));
    CHECK(tab.reads("get_frame_plan") == before_placement);

    // Decision 2: it is the commit, not the read, that says the plan may
    // have moved. The count goes to 4, and the document reads back with its
    // nodes as they were and only a placement changed -- the read the tab
    // otherwise takes for a drag's echo, which reads no plan.
    tab.widget->scene().selectNode("steps");
    QCoreApplication::processEvents();
    count = tab.field<QSpinBox>("pelican.graphCanvasLoop.count");
    REQUIRE(count->isEnabled());
    count->setValue(4);
    Q_EMIT count->editingFinished();
    QCoreApplication::processEvents();
    REQUIRE(tab.reads("edit_graph_document") == 3);
    const int before_count = tab.reads("get_frame_plan");
    tab.commit("q18-3");
    CHECK(tab.told == 2);
    document["layout"] = {
        {"frame_plan", {{"main_render", {{"nodes", {{"pass", {{"x", 30.0}, {"y", 4.0}}}}}}}}}};
    tab.answer(QStringLiteral("get_graph_instances"), q18Context(document, "q18-3"));
    REQUIRE(tab.reads("get_frame_plan") == before_count + 1);
    tab.answer(QStringLiteral("get_frame_plan"), q18Plan(4));
    CHECK(tab.expansion() == QStringLiteral("×4 copies"));
}

TEST_CASE("Q-18 the Draw fields are off while an edit is in flight, and a refused choice goes back",
          "[devstudio][graph-canvas][scene][q18]") {
    (void)application();
    const Json color{{"semantic", "pelican.render.color_signal@1"},
                     {"arguments", {{"reference", "scene"}, {"transfer", "linear"},
                                    {"range", "extended"}}}};
    const auto documentFor = [&](const std::string &contract) {
        return Json{
            {"definitions", Json::object()},
            {"nodes", Json::array({
                {{"id", "scene_color_ref"}, {"op", "render.target_ref"},
                 {"parameters", {{"name", "scene_color"}, {"type", color}}}, {"outputs", {"resource"}}},
                {{"id", "forward_opaque"}, {"op", "render.material_pass"},
                 {"parameters", {{"material_contract", contract}, {"pass_set", "main_render"}}},
                 {"inputs", {{"color", {{"node", "scene_color_ref"}, {"port", "resource"}}}}},
                 {"outputs", Json::array()}}})}};
    };
    Json catalog = visualCatalog();
    for (const char *op : {"render.target_ref", "render.material_pass"})
        catalog["modules"][1]["operations"].push_back(
            {{"op", op}, {"domain", "rendering"}, {"evaluation", "direct"}, {"sink", false},
             {"ports", "parameter_dependent"}});
    Q18Tab tab;
    tab.answer(QStringLiteral("get_graph_instances"),
               q18Context(documentFor("forward_opaque_v1"), "draw-0"));
    tab.answer(QStringLiteral("get_graph_catalog"), catalog);
    tab.widget->scene().selectNode("forward_opaque");
    QCoreApplication::processEvents();
    const auto edits = [&] { return tab.reads("edit_graph_document"); };
    auto *contract = tab.field<QComboBox>("pelican.graphCanvasDraw.contract");
    REQUIRE(contract->isVisible());
    REQUIRE(contract->isEnabled());
    CHECK(contract->currentData().toString() == QStringLiteral("forward_opaque_v1"));

    contract->setCurrentIndex(contract->findData(QStringLiteral("deferred_geometry_v1")));
    QCoreApplication::processEvents();
    REQUIRE(edits() == 1);
    // Sent: off until it is read back.
    CHECK_FALSE(contract->isEnabled());
    // A choice made anyway (a script sets the index of an off combo) is
    // refused by the model, and the combo says what the document says.
    contract->setCurrentIndex(contract->findData(QStringLiteral("forward_transparent_v1")));
    QCoreApplication::processEvents();
    CHECK(edits() == 1);
    CHECK(contract->currentData().toString() == QStringLiteral("forward_opaque_v1"));
    tab.answer(QStringLiteral("edit_graph_document"), Json{{"status", "accepted"}, {"ticket", "draw"}});
    CHECK_FALSE(contract->isEnabled());
    tab.answer(QStringLiteral("get_edit_result"),
               Json{{"status", "committed"}, {"committed", true}, {"source_digest", {{"hex", "draw-1"}}}});
    // Committed, the document not yet read back.
    CHECK_FALSE(contract->isEnabled());
    tab.answer(QStringLiteral("get_graph_instances"),
               q18Context(documentFor("deferred_geometry_v1"), "draw-1"));
    // Read back: on once, and a choice goes out.
    REQUIRE(contract->isEnabled());
    CHECK(contract->currentData().toString() == QStringLiteral("deferred_geometry_v1"));
    contract->setCurrentIndex(contract->findData(QStringLiteral("forward_transparent_v1")));
    QCoreApplication::processEvents();
    CHECK(edits() == 2);
}

TEST_CASE("Q-18 a render node's View field is off while an edit is in flight, and a refused pick "
          "goes back in the same combo",
          "[devstudio][graph-canvas][scene][q18]") {
    (void)application();
    Q18Tab tab;
    auto document = lp4f1Document();
    tab.answer(QStringLiteral("get_graph_instances"), q18Context(document, "view-0"));
    tab.answer(QStringLiteral("get_graph_catalog"), visualCatalog());
    REQUIRE(tab.reads("get_frame_plan") == 1);
    tab.answer(QStringLiteral("get_frame_plan"), lp4f1Plan());
    tab.widget->scene().selectNode("outside");
    QCoreApplication::processEvents();
    const auto edits = [&] { return tab.reads("edit_graph_document"); };
    auto *face = tab.field<QComboBox>("pelican.graphCanvasRender.view.face");
    REQUIRE(face->isVisible());
    REQUIRE(face->isEnabled());
    CHECK(face->currentText() == QStringLiteral("All views"));

    face->setCurrentIndex(face->findText(QStringLiteral("2 (+Y)")));
    QCoreApplication::processEvents();
    REQUIRE(edits() == 1);
    CHECK_FALSE(face->isEnabled());
    // Picked anyway: refused, and the same combo holds the document's value.
    face->setCurrentIndex(face->findText(QStringLiteral("5 (-Z)")));
    QCoreApplication::processEvents();
    CHECK(edits() == 1);
    CHECK(face->currentText() == QStringLiteral("All views"));
    tab.commit("view-1");
    CHECK_FALSE(tab.field<QComboBox>("pelican.graphCanvasRender.view.face")->isEnabled());
    document["nodes"][2]["parameters"]["view_select"] = {{"face", 2}};
    tab.answer(QStringLiteral("get_graph_instances"), q18Context(document, "view-1"));
    face = tab.field<QComboBox>("pelican.graphCanvasRender.view.face");
    REQUIRE(face->isEnabled());
    CHECK(face->currentText() == QStringLiteral("2 (+Y)"));
    face->setCurrentIndex(face->findText(QStringLiteral("5 (-Z)")));
    QCoreApplication::processEvents();
    CHECK(edits() == 2);
}

// --- Q-19: a render node's input sockets on the canvas, lines into them,
// and a definition renamed or deleted from the Graph tab.

namespace {

// A root image `img` and a loop `steps` (two copies) over `body`: a boundary
// input `target`, an image `canvas`, and a passthrough `pass` drawing into it,
// whose color is the body's output and is carried back into `target`.
Json q19ZoneDocument() {
    const Json type{{"semantic", "pelican.render.color_signal@1"},
                    {"arguments", {{"reference", "scene"}, {"transfer", "linear"},
                                   {"range", "extended"}}}};
    const auto image = [&](const char *id) {
        return Json{{"id", id}, {"op", "render.image"},
                    {"parameters", {{"type", type}, {"format", "R8G8B8A8_UNORM"},
                                    {"extent_scale", 1.0},
                                    {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}},
                    {"outputs", {"resource"}}};
    };
    const Json pass{
        {"id", "pass"}, {"op", "render.fullscreen"},
        {"parameters", {{"shader", {{"vertex", "engine://fullscreen"},
                                    {"fragment", "engine://passthrough"}}},
                        {"ports", {{"color", {{"kind", "image"}, {"type", type},
                                              {"access", "write"}, {"attachment", "color"},
                                              {"location", 0}}}}}}},
        {"inputs", {{"color", {{"node", "canvas"}, {"port", "resource"}}}}},
        {"outputs", {"color"}}};
    const Json body{{"nodes", Json::array({{{"id", "target"}, {"op", "graph.input"}},
                                           image("canvas"), pass})},
                    {"outputs", {{"color", {{"node", "pass"}, {"port", "color"}}}}}};
    return Json{{"schema", "pelican.graph"}, {"version", 1}, {"outputs", Json::object()},
                {"definitions", {{"body", body}}},
                {"nodes", Json::array({image("img"),
                                       {{"id", "steps"}, {"op", "graph.loop"},
                                        {"parameters", {{"over", {{"range", {{"count", 2}}}}},
                                                        {"branches", Json::array({{{"body", "body"}}})},
                                                        // Its arrow is drawn over target's socket.
                                                        {"carry", Json::array({{{"from", "color"},
                                                                                {"to", "target"}}})}}},
                                        {"inputs", {{"target", {{"node", "img"}, {"port", "resource"}}}}},
                                        {"outputs", Json::array({"color"})}}})}};
}

Json q19ZoneLoops() {
    return Json::array({{{"node", "steps"}, {"tier", "expand"}, {"count", 2},
                         {"elements", Json::array({{{"element", 0}, {"index", 0}, {"branch", 0}},
                                                   {{"element", 1}, {"index", 1}, {"branch", 0}}})},
                         {"unmatched_branches", Json::array()}}});
}

// The two copies of `pass`, drawing with the passthrough: one variadic group.
Json q19ZonePlan() {
    auto nodes = Json::array();
    for (int element = 0; element < 2; ++element)
        nodes.push_back(
            {{"name", "steps/" + std::to_string(element) + "/pass"}, {"kind", "fullscreen"},
             {"loop_origin", {{"loop", "steps"}, {"element", element}, {"node", "pass"}}},
             {"pass_inputs", {{"state", "declared"}, {"fragment", "engine://passthrough"},
                              {"declarations", Json::array({{{"binding", 0}, {"type", "sampler2D"},
                                                             {"name", "pelican_inputs"},
                                                             {"variadic", true}}})},
                              {"variadic_range", {{"first", 0}, {"max", 8}}}}}});
    return Json{{"nodes", nodes}};
}

Json q19ReadPort(std::int64_t index) {
    return {{"kind", "image"},
            {"type", {{"semantic", "pelican.render.color_signal@1"},
                      {"arguments", {{"reference", "scene"}, {"transfer", "linear"},
                                     {"range", "extended"}}}}},
            {"access", "read"}, {"binding", "input"}, {"input_index", index}};
}

} // namespace

TEST_CASE("Q-19 a body pass in a zone draws its shader's sockets, and a line onto them is a "
          "numbered port",
          "[devstudio][graph-canvas][scene][q19]") {
    (void)application();
    const QString body = QStringLiteral("body");

    SECTION("without the plan's declarations the pass keeps its rows") {
        ZoneHarness h{q19ZoneDocument()};
        addRenderOperations(h);
        h.loops = q19ZoneLoops();
        h.open();
        // The document's read asks for the plan; answer that too.
        h.answerReads();
        h.scene.repopulate();
        CHECK(socketOf(h.scene, body, "pass", "in:variadic", "input") == nullptr);
        CHECK(socketOf(h.scene, body, "pass", "color", "input") != nullptr);
    }

    ZoneHarness h{q19ZoneDocument()};
    addRenderOperations(h);
    h.loops = q19ZoneLoops();
    h.plan = q19ZonePlan();
    h.open();
    h.answerReads();
    h.scene.repopulate();
    const auto pass_parameters =
        h.document.at("definitions").at("body").at("nodes").at(2).at("parameters");

    // The variadic band's end, named by what it is, over a band of its own.
    QGraphicsItem *drop = socketOf(h.scene, body, "pass", "in:variadic", "input");
    REQUIRE(drop != nullptr);
    CHECK(drop->data(PelicanStudio::GraphCanvasSocketKeyRole).toString() ==
          QStringLiteral("in:variadic"));
    CHECK(scopedItem(h.scene, PelicanStudio::GraphCanvasNodeLabelItem, body,
                     QStringLiteral("pass"), QStringLiteral("in:band")) != nullptr);
    // The write port is still a row of its own.
    CHECK(socketOf(h.scene, body, "pass", "color", "input") != nullptr);

    // Decision 3: the boundary input, inside the zone, onto the band's end --
    // from under the carry's arrow, which ends on that socket.
    QGraphicsItem *target = socketOf(h.scene, body, "target", "value", "output");
    REQUIRE(target != nullptr);
    CHECK(std::ranges::any_of(h.scene.items(centreOf(target)), [](const QGraphicsItem *item) {
        return item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
               QLatin1String{PelicanStudio::GraphCanvasZoneCarryItem};
    }));
    drag(h.scene, centreOf(target), centreOf(drop));
    auto expected = pass_parameters;
    expected["ports"]["target"] = q19ReadPort(0);
    CHECK(h.commit() ==
          Json::array({{{"op", "set_node_parameters"}, {"scope", "body"}, {"node", "pass"},
                        {"parameters", expected}},
                       {{"op", "set_input"}, {"scope", "body"}, {"node", "pass"},
                        {"port", "target"},
                        {"source", {{"node", "target"}, {"port", "value"}}}}}));
    // Drawn as the band's member at binding 0, with the edge ending on it.
    QGraphicsItem *member = socketOf(h.scene, body, "pass", "in@0", "input");
    REQUIRE(member != nullptr);
    CHECK(member->data(PelicanStudio::GraphCanvasBoundPortRole).toString() ==
          QStringLiteral("target"));
    auto *line = dynamic_cast<QGraphicsPathItem *>(
        scopedEdge(h.scene, body, QStringLiteral("target"), QStringLiteral("value"),
                   QStringLiteral("pass"), QStringLiteral("target")));
    REQUIRE(line != nullptr);
    const QPointF end = line->mapToScene(line->path().currentPosition());
    CHECK(QLineF{end, centreOf(member)}.length() < 2.0);

    // Dragged off: the port goes with its line.
    drag(h.scene, centreOf(member), QPointF{-5000.0, -5000.0});
    CHECK(h.commit() ==
          Json::array({{{"op", "set_input"}, {"scope", "body"}, {"node", "pass"},
                        {"port", "target"}, {"source", nullptr}},
                       {{"op", "set_node_parameters"}, {"scope", "body"}, {"node", "pass"},
                        {"parameters", pass_parameters}}}));
    CHECK(socketOf(h.scene, body, "pass", "in@0", "input") == nullptr);

    // From outside the zone onto the band's end: the loop input, its
    // graph.input and the port reading it, in one list.
    drop = socketOf(h.scene, body, "pass", "in:variadic", "input");
    QGraphicsItem *img = socketOf(h.scene, QString{}, "img", "resource", "output");
    REQUIRE(drop != nullptr);
    REQUIRE(img != nullptr);
    drag(h.scene, centreOf(img), centreOf(drop));
    expected = pass_parameters;
    expected["ports"]["resource"] = q19ReadPort(0);
    CHECK(h.commit() ==
          Json::array({graphInput("body", "resource"),
                       {{"op", "set_input"}, {"scope", "root"}, {"node", "steps"},
                        {"port", "resource"},
                        {"source", {{"node", "img"}, {"port", "resource"}}}},
                       {{"op", "set_node_parameters"}, {"scope", "body"}, {"node", "pass"},
                        {"parameters", expected}},
                       {{"op", "set_input"}, {"scope", "body"}, {"node", "pass"},
                        {"port", "resource"},
                        {"source", {{"node", "resource"}, {"port", "value"}}}}}));
}

TEST_CASE("Q-19 Rename definition… and Delete definition act on the zone picked, and a name "
          "that cannot be one is not sent",
          "[devstudio][graph-canvas][scene][q19]") {
    (void)application();
    RecordedCapability transport;
    PelicanStudio::GraphCanvasWidget widget{transport.capability()};
    widget.resize(1100, 700);
    widget.show();
    QCoreApplication::processEvents();
    const auto deliver = [&](const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        REQUIRE(transport.deliver);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    };
    auto document = q19ZoneDocument();
    deliver(QStringLiteral("get_graph_instances"), context(document));
    auto catalog = visualCatalog();
    for (const char *op : {"render.image", "render.fullscreen"})
        catalog["modules"][1]["operations"].push_back(
            {{"op", op}, {"domain", "rendering"}, {"evaluation", "direct"}, {"sink", false},
             {"ports", "parameter_dependent"}});
    deliver(QStringLiteral("get_graph_catalog"), catalog);

    auto *rename = widget.findChild<QAction *>(QStringLiteral("pelican.graphCanvas.renameDefinition"));
    auto *remove = widget.findChild<QAction *>(QStringLiteral("pelican.graphCanvas.deleteDefinition"));
    auto *row = widget.findChild<QWidget *>(QStringLiteral("pelican.graphCanvasDefinitionRow"));
    auto *name = widget.findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasDefinitionName"));
    auto *apply = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasDefinitionApply"));
    auto *line = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasDefinitionLine"));
    auto *notice = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasNotice"));
    REQUIRE(rename != nullptr);
    REQUIRE(remove != nullptr);
    REQUIRE(row != nullptr);
    REQUIRE(name != nullptr);
    REQUIRE(apply != nullptr);
    REQUIRE(line != nullptr);
    REQUIRE(notice != nullptr);
    // Nothing picked in the root: nothing for them to be about.
    widget.poll();
    QCoreApplication::processEvents();
    CHECK_FALSE(rename->isEnabled());

    // The zone picked: about the definition its tab shows.
    widget.scene().selectNode(std::string{}, "steps");
    widget.poll();
    QCoreApplication::processEvents();
    REQUIRE(rename->isEnabled());
    rename->trigger();
    QCoreApplication::processEvents();
    REQUIRE(row->isVisible());
    CHECK(name->text() == QStringLiteral("body"));
    CHECK(name->selectedText() == QStringLiteral("body"));
    // The same name renames nothing.
    CHECK_FALSE(apply->isEnabled());

    // "root" names the root document: said, and not sent.
    name->setText(QStringLiteral("root"));
    CHECK(line->isVisible());
    CHECK(line->text() ==
          QStringLiteral("A definition cannot be named root: that name is the root document's."));
    CHECK_FALSE(apply->isEnabled());
    QMetaObject::invokeMethod(name, "returnPressed");
    QCoreApplication::processEvents();
    CHECK(transport.countOf(QStringLiteral("edit_graph_document")) == 0);
    CHECK(row->isVisible());

    name->setText(QStringLiteral("stroke"));
    CHECK_FALSE(line->isVisible());
    REQUIRE(apply->isEnabled());
    apply->click();
    QCoreApplication::processEvents();
    REQUIRE(transport.countOf(QStringLiteral("edit_graph_document")) == 1);
    const auto sent = Json::parse(transport.calls.back().body.toStdString());
    CHECK(sent == Json::array({{{"op", "rename_definition"}, {"from", "body"}, {"to", "stroke"}}}));
    CHECK_FALSE(row->isVisible());

    // Delete: the loop still runs it, so the notice says so and nothing goes.
    deliver(QStringLiteral("edit_graph_document"),
            Json{{"status", "rejected"}, {"committed", false},
                 {"error", {{"code", "invalid_graph_document"}, {"message", "not this time"}}}});
    deliver(QStringLiteral("get_graph_instances"), context(document));
    widget.scene().selectNode(std::string{}, "steps");
    widget.poll();
    QCoreApplication::processEvents();
    REQUIRE(remove->isEnabled());
    remove->trigger();
    QCoreApplication::processEvents();
    CHECK(transport.countOf(QStringLiteral("edit_graph_document")) == 1);
    CHECK_FALSE(row->isVisible());
    CHECK(notice->text() ==
          QStringLiteral("Definition body is used by steps: make them run another definition first."));
}

// --- GL-1b. The Graph's root is placed the way the Frame Plan is: one
// layoutNodeGraph over the graph's nodes and the folded ones, with the lines
// the plan implies (decision 1). A commit that asks for an engine restart is
// handed to the owner's restart, the Frame Plan's (decision 2); an engine
// that cannot ask yet refuses in words, and the line says what it needed
// (decision 3).

namespace {

// The root: a target_ref naming scene_color and the fullscreen pass reading
// it. The plan: the preset's gbuffer and lighting write scene_color, the
// graph's own blur reads it, and the rendering section's present reads blur.
Json gl1bRead(const Json &layout = Json::object()) {
    Json blur_inputs = Json::object();
    blur_inputs["source"] = Json{{"node", "bloomed"}, {"port", "resource"}};
    Json document{{"nodes", Json::array({
                       {{"id", "bloomed"}, {"op", "render.target_ref"},
                        {"parameters", {{"name", "scene_color"}}},
                        {"outputs", Json::array({"resource"})}},
                       {{"id", "blur"}, {"op", "render.fullscreen"}, {"parameters", Json::object()},
                        {"inputs", blur_inputs}, {"outputs", Json::array({"color"})}}})},
                  {"definitions", Json::object()},
                  {"layout", layout}};
    auto read = context(document);
    read["root_schema"] = "pelican.graph";
    read["source_reference"] = "fixture";
    return read;
}

Json gl1bPass(const std::string &name, std::size_t order, const std::string &source,
              const Json &reads, const Json &writes) {
    return {{"name", name},     {"kind", "render"}, {"order", order},   {"level", order},
            {"source", source}, {"reads", reads},   {"writes", writes}, {"reads_history", Json::array()},
            {"declaration_index", order}};
}

Json gl1bPlan() {
    return {{"schema", "pelican.frame_plan"},
            {"version", 1},
            {"graph", "main_render"},
            {"loops", Json::array()},
            {"nodes", Json::array({
                 gl1bPass("gbuffer", 0, "engine", Json::array(), Json::array({"g_albedo"})),
                 gl1bPass("lighting", 1, "engine", Json::array({"g_albedo"}),
                          Json::array({"scene_color"})),
                 gl1bPass("blur", 2, "engine", Json::array({"scene_color"}),
                          Json::array({"blur_image"})),
                 gl1bPass("present", 3, "project", Json::array({"blur_image"}),
                          Json::array({"display"}))})}};
}

void openGl1b(GraphCanvasModel &model, GraphCanvasScene &scene, const Json &read) {
    model.setConnected(true);
    for (const auto &request : model.takeRpcRequests())
        if (request.method == "get_graph_instances")
            model.receiveRpcResult(request.request_id, read.dump());
    // The plan is read with the document.
    bool planned = false;
    for (const auto &request : model.takeRpcRequests())
        if (request.method == "get_frame_plan") {
            model.receiveRpcResult(request.request_id, gl1bPlan().dump());
            planned = true;
        }
    REQUIRE(planned);
    scene.setModel(&model);
    REQUIRE(scene.property("pelicanGraphFoldedCount").toULongLong() == 3);
}

} // namespace

TEST_CASE("GL-1b the root's folded nodes are placed by the same one layout as the graph's, "
          "in the plan's order left to right",
          "[devstudio][graph-canvas][scene][gl1b]") {
    (void)application();
    const QStringList order{QStringLiteral("gbuffer"), QStringLiteral("lighting"),
                            QStringLiteral("bloomed"), QStringLiteral("blur"),
                            QStringLiteral("present")};

    SECTION("a document that places nothing: one placement for everything") {
        GraphCanvasModel model;
        GraphCanvasScene scene;
        openGl1b(model, scene, gl1bRead());
        CHECK(itemsOfKind(scene, PelicanStudio::GraphCanvasDerivedEdgeItem).size() == 3);
        // The Frame Plan's column rule: the longest path over the plan's
        // dependencies. present reads what the graph's own blur writes, so it
        // is right of it, not in a column of folded nodes on the left (GL-1).
        std::map<QString, QPointF> placed;
        for (int column = 0; column < order.size(); ++column) {
            QGraphicsItem *item = nodeItem(scene, order[column]);
            REQUIRE(item != nullptr);
            CHECK(item->data(PelicanStudio::GraphCanvasColumnRole).toInt() == column);
            if (column > 0) CHECK(item->pos().x() > nodeItem(scene, order[column - 1])->pos().x());
            placed[order[column]] = item->pos();
        }

        // Auto layout is the same one placement: nothing moves, and only the
        // graph's own nodes are written to the document.
        scene.autoLayout();
        for (const auto &name : order) CHECK(nodeItem(scene, name)->pos() == placed.at(name));
        CHECK(scene.layout().nodes.contains("bloomed"));
        CHECK(scene.layout().nodes.contains("blur"));
        CHECK_FALSE(scene.layout().nodes.contains("gbuffer"));
        CHECK_FALSE(scene.layout().nodes.contains("present"));
        // Drawn again from the document's placement, the folded nodes keep
        // the places it gave them (session state).
        scene.repopulate();
        for (const auto &name : order) CHECK(nodeItem(scene, name)->pos() == placed.at(name));
    }

    SECTION("a document that places the graph's nodes: the folded ones go left of them") {
        GraphCanvasModel model;
        GraphCanvasScene scene;
        const Json layout{{"nodes", {{"bloomed", {{"x", 500.0}, {"y", 40.0}}},
                                     {"blur", {{"x", 900.0}, {"y", 40.0}}}}}};
        openGl1b(model, scene, gl1bRead(layout));
        CHECK(nodeItem(scene, QStringLiteral("bloomed"))->pos() == QPointF{500.0, 40.0});
        for (const auto &name : {QStringLiteral("gbuffer"), QStringLiteral("lighting"),
                                 QStringLiteral("present")})
            CHECK(nodeItem(scene, name)->sceneBoundingRect().right() < 500.0);
        // Among themselves, still the plan's order.
        CHECK(nodeItem(scene, QStringLiteral("gbuffer"))->pos().x() <
              nodeItem(scene, QStringLiteral("lighting"))->pos().x());
    }
}

TEST_CASE("GL-1b a Graph commit that asks for a restart goes to the owner's restart; an older "
          "engine's refusal says a restart is what it needed",
          "[devstudio][graph-canvas][scene][gl1b]") {
    (void)application();
    Q18Tab tab;
    QStringList restarts;
    tab.widget->setEngineRestartAction([&](const QString &reason) { restarts.push_back(reason); });
    const auto document = zoneDocument();
    tab.answer(QStringLiteral("get_graph_instances"), q18Context(document, "gl1b-0"));
    tab.answer(QStringLiteral("get_graph_catalog"), visualCatalog());
    tab.answer(QStringLiteral("get_frame_plan"), q18Plan(2));
    auto *notice = tab.field<QLabel>("pelican.graphCanvasNotice");
    const auto edit = [&](int count) {
        const int sent = tab.reads("edit_graph_document");
        tab.widget->scene().selectNode("steps");
        QCoreApplication::processEvents();
        auto *field = tab.field<QSpinBox>("pelican.graphCanvasLoop.count");
        field->setValue(count);
        Q_EMIT field->editingFinished();
        QCoreApplication::processEvents();
        REQUIRE(tab.reads("edit_graph_document") == sent + 1);
        tab.answer(QStringLiteral("edit_graph_document"),
                   Json{{"status", "accepted"}, {"ticket", "gl1b"}});
    };

    // Decision 3: an engine without RS-1 refuses in words (and no code).
    edit(3);
    const std::string refusal =
        "render-pipeline candidate pass 'render_material_pass' changes the physical rendering "
        "contract of live material 0; require a transactional material pipeline rebuild";
    tab.answer(QStringLiteral("get_edit_result"),
               Json{{"status", "rejected"}, {"committed", false}, {"error", {{"message", refusal}}}});
    const QString hint = QString::fromUtf8(
        " \xE2\x80\x94 this change needs an engine restart, which the engine does not offer for "
        "graph edits yet");
    CHECK(restarts.isEmpty());
    CHECK(notice->toolTip() == QString::fromStdString(refusal) + hint);
    // The sentence is cut to one line; the hint is not.
    CHECK(notice->text().endsWith(hint));
    tab.answer(QStringLiteral("get_graph_instances"), q18Context(document, "gl1b-0"));
    tab.answer(QStringLiteral("get_frame_plan"), q18Plan(2));

    // Decision 2: RS-1's answer. Written, and the engine starts again through
    // the owner's restart; nothing is read from the engine about to stop.
    edit(4);
    const int reads = tab.reads("get_graph_instances");
    tab.answer(QStringLiteral("get_edit_result"),
               Json{{"status", "committed"},
                    {"committed", true},
                    {"source_digest", {{"hex", "gl1b-1"}}},
                    {"restart_required", true},
                    {"restart_reason",
                     // The engine's own answer (RS-1): `detail` is a list.
                     {{"code", "material_physical_contract_changed"},
                      {"material", "pelican.builtin.transparent"},
                      {"material_id", 0},
                      {"pass", "render_material_pass"},
                      {"detail", Json::array({"color_formats"})},
                      {"message", "render-pipeline candidate pass 'render_material_pass' "
                                  "changes the physical rendering contract of live material 0"}}}});
    REQUIRE(restarts.size() == 1);
    CHECK(restarts.front() ==
          QStringLiteral("Restarting the engine to apply this change "
                         "(material_physical_contract_changed)..."));
    CHECK(notice->text() == restarts.front());
    CHECK(tab.reads("get_graph_instances") == reads);

    // Declined (unsaved scene edits, Cancel): the running engine has the edit;
    // the tab reads it and says why nothing restarted. A decline of a restart
    // another panel asked for is not this tab's to report.
    tab.widget->engineRestartDeclined(QStringLiteral("The engine was not restarted."));
    CHECK(tab.reads("get_graph_instances") == reads + 1);
    CHECK(notice->text() == QStringLiteral("The engine was not restarted."));
    tab.widget->engineRestartDeclined(QStringLiteral("Someone else's."));
    CHECK(tab.reads("get_graph_instances") == reads + 1);
}

// --- GL-1c. The folded read-only nodes never lie on what the root draws: a
// loop's zone is one box of its frame's size in the one placement (decision
// 2), and once the document places the graph's nodes the folded ones stand
// left of everything the root draws -- frames, their bottom rows and carries
// included -- even after a zone has grown under where they were (decision 1).
// Checked on the drawing itself (decision 3).

namespace {

// The engine's passes around q19's loop: the preset's gbuffer and lighting,
// and the rendering section's picking and present.
Json gl1cPlan() {
    auto plan = q19ZonePlan();
    plan["graph"] = "main_render";
    plan["nodes"].push_back(gl1bPass("gbuffer", 0, "engine", Json::array(), Json::array({"g_albedo"})));
    plan["nodes"].push_back(gl1bPass("lighting", 1, "engine", Json::array({"g_albedo"}),
                                     Json::array({"scene_color"})));
    plan["nodes"].push_back(gl1bPass("picking", 2, "project", Json::array(), Json::array({"picking_id"})));
    plan["nodes"].push_back(gl1bPass("present", 3, "project", Json::array({"scene_color"}),
                                     Json::array({"display"})));
    return plan;
}

// A node or a zone's frame with all it holds: sockets, a value's text running
// past the box, the zone's bottom row, the carry's arrow, the body.
QRectF gl1cDrawn(QGraphicsItem &item) {
    return item.sceneBoundingRect().united(item.mapRectToScene(item.childrenBoundingRect()));
}

// Nothing folded lies on another folded node, on a graph node or a zone; with
// `left_of`, all of them stand left of what the graph's nodes draw.
void gl1cCheckClear(QGraphicsScene &scene, bool left_of) {
    std::vector<QGraphicsItem *> folded;
    std::vector<QGraphicsItem *> graph;
    for (QGraphicsItem *item : itemsOfKind(scene, PelicanStudio::GraphCanvasNodeItem)) {
        if (item->parentItem() != nullptr) continue;
        if (item->data(PelicanStudio::GraphCanvasDerivedRole).toBool()) folded.push_back(item);
        else if (item->data(PelicanStudio::GraphCanvasScopeRole).toString().isEmpty())
            graph.push_back(item);
    }
    REQUIRE(folded.size() == 4);
    REQUIRE(graph.size() == 2);
    QRectF bounds;
    for (QGraphicsItem *item : graph) bounds = bounds.united(gl1cDrawn(*item));
    for (std::size_t index = 0; index < folded.size(); ++index) {
        const QRectF box = folded[index]->sceneBoundingRect();
        INFO(folded[index]->data(PelicanStudio::NodeGraphNameRole).toString().toStdString());
        for (std::size_t other = index + 1; other < folded.size(); ++other)
            CHECK_FALSE(box.intersects(folded[other]->sceneBoundingRect()));
        for (QGraphicsItem *item : graph) CHECK_FALSE(box.intersects(gl1cDrawn(*item)));
        if (left_of) CHECK(box.right() < bounds.left());
    }
}

} // namespace

TEST_CASE("GL-1c the folded nodes never lie on a loop's zone, placed or not, grown or not",
          "[devstudio][graph-canvas][scene][gl1c]") {
    (void)application();

    SECTION("a document that places nothing: the zone is one box in the one placement") {
        ZoneHarness h{q19ZoneDocument()};
        h.loops = q19ZoneLoops();
        h.plan = gl1cPlan();
        h.open();
        h.answerReads();
        h.scene.repopulate();
        // The zone's bottom row is drawn: the copies and the button.
        REQUIRE(scopedItem(h.scene, PelicanStudio::GraphCanvasZoneButtonItem, QString{},
                           QStringLiteral("steps"), QStringLiteral("frame_plan")) != nullptr);
        gl1cCheckClear(h.scene, false);
    }

    SECTION("a document that places the graph's nodes: the folded ones stand left of it all") {
        auto document = q19ZoneDocument();
        document["layout"] = {{"nodes", {{"img", {{"x", 600.0}, {"y", 40.0}}},
                                         {"steps", {{"x", 1000.0}, {"y", 40.0}}}}}};
        ZoneHarness h{document};
        h.loops = q19ZoneLoops();
        h.plan = gl1cPlan();
        h.open();
        h.answerReads();
        h.scene.repopulate();
        gl1cCheckClear(h.scene, true);
    }

    SECTION("Auto layout, then the zone grows down over where the folded nodes were") {
        ZoneHarness h{q19ZoneDocument()};
        h.loops = q19ZoneLoops();
        h.plan = gl1cPlan();
        h.open();
        h.answerReads();
        h.scene.repopulate();
        h.scene.autoLayout();
        h.commit();
        REQUIRE(h.scene.layout().nodes.contains("steps"));
        gl1cCheckClear(h.scene, false);
        const QRectF before =
            scopedNode(h.scene, QString{}, QStringLiteral("steps"))->sceneBoundingRect();
        // Four more images in the body, one under the other: the frame
        // reaches much further down.
        auto &body = h.document["definitions"]["body"]["nodes"];
        for (const char *id : {"extra_a", "extra_b", "extra_c", "extra_d"}) {
            auto image = body[1];
            image["id"] = id;
            body.push_back(std::move(image));
        }
        ++h.generation;
        h.model.refresh();
        h.answerReads();
        h.scene.repopulate();
        REQUIRE(scopedNode(h.scene, QString{}, QStringLiteral("steps"))->sceneBoundingRect().height() >
                before.height() + 200.0);
        gl1cCheckClear(h.scene, true);
    }
}

// --- GL-1e. A pass of the rendering section takes lines on the Graph:
// present (a fullscreen pass the root declares, drawing with
// engine://passthrough) shows its `input` as sockets and an empty `+ input`
// (decision 1); a graph image's output dropped on one sends the
// edit_preset_pass the Frame Plan's socket drop sends -- in place of what is
// there, or at the end -- and the line is drawn solid; Delete takes it out
// (decision 2). The Graph tab sends it, polls its ticket, takes it back with
// its own Undo through the render edit history, and an engine before RS-3
// refuses it with its own sentence and one phrase more (decision 3).

namespace {

// The root: an image and the graph's fullscreen pass drawing into it.
Json gl1eRead() {
    Json document{
        {"nodes",
         Json::array({{{"id", "shade__color"}, {"op", "render.image"},
                       {"parameters", {{"format", "R8G8B8A8_UNORM"}}},
                       {"outputs", Json::array({"resource"})}},
                      {{"id", "shade"}, {"op", "render.fullscreen"},
                       {"parameters", Json::object()},
                       {"inputs", {{"color", {{"node", "shade__color"}, {"port", "resource"}}}}},
                       {"outputs", Json::array({"color"})}}})},
        {"definitions", Json::object()},
        {"layout", Json::object()}};
    auto read = context(document);
    read["source_digest"]["hex"] = "gl1e-root";
    read["root_schema"] = "pelican.graph";
    read["source_reference"] = "fixture";
    return read;
}

// engine://passthrough as get_frame_plan publishes it: eight variadic inputs.
Json gl1ePassthrough() {
    auto declarations = Json::array();
    for (std::size_t slot = 0; slot < 8; ++slot)
        declarations.push_back({{"binding", slot},
                                {"type", "sampled_image"},
                                {"name", "pelican_variadic_input_" + std::to_string(slot)},
                                {"variadic", true}});
    return {{"state", "declared"},
            {"fragment", "engine://passthrough"},
            {"declarations", std::move(declarations)},
            {"variadic_range", {{"first", 0}, {"max", 8}}},
            {"pass_texture_inputs", 0},
            {"pass_buffer_inputs", 0},
            {"buffer_binding_base", 8}};
}

// The graph's pass first (RS-3 puts it before the pass that reads it), then
// the rendering section's present reading what its `input` names.
Json gl1ePlan(const std::vector<std::string> &inputs) {
    auto present = gl1bPass("present", 1, "project", Json(inputs), Json::array({"display"}));
    present["pass_inputs"] = gl1ePassthrough();
    return {{"schema", "pelican.frame_plan"},
            {"version", 1},
            {"graph", "main_render"},
            {"loops", Json::array()},
            {"resources", Json::array({{{"name", "shade__color"}, {"kind", "image"}},
                                       {{"name", "old_image"}, {"kind", "image"}},
                                       {{"name", "display"}, {"kind", "image"}}})},
            {"nodes", Json::array({gl1bPass("shade", 0, "project", Json::array(),
                                            Json::array({"shade__color"})),
                                   present})}};
}

Json gl1ePresent(const std::vector<std::string> &inputs) {
    return {{"type", "fullscreen"},
            {"input", inputs},
            {"output", {{"color", "display"}}},
            {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}};
}

// get_render_authoring_context of a Blank project: present is the root's own.
Json gl1eAuthoring(const std::vector<std::string> &inputs) {
    const auto present = gl1ePresent(inputs);
    return {{"source_digest", {{"hex", "gl1e-root"}}},
            {"config_kind", "direct"},
            {"source_reference", "project://passes/main_rendering_config.json"},
            {"graphs",
             Json::array({{{"name", "main_render"},
                           {"passes", Json::array({{{"name", "present"},
                                                    {"declaration_index", 0},
                                                    {"editable_root_pass", true},
                                                    {"declaration", present},
                                                    {"root_declaration", present}}})}}})}};
}

// The document, its plan and the context, answered in the order the model
// asks for them.
void openGl1e(GraphCanvasModel &model, GraphCanvasScene &scene,
              const std::vector<std::string> &inputs) {
    model.setConnected(true);
    for (const auto &request : model.takeRpcRequests())
        if (request.method == "get_graph_instances")
            model.receiveRpcResult(request.request_id, gl1eRead().dump());
    bool planned = false;
    for (const auto &request : model.takeRpcRequests())
        if (request.method == "get_frame_plan") {
            model.receiveRpcResult(request.request_id, gl1ePlan(inputs).dump());
            planned = true;
        }
    REQUIRE(planned);
    // Decision 1: a plan with a pass of the rendering section asks for the
    // authoring context, which says what the pass's `input` is.
    bool authored = false;
    for (const auto &request : model.takeRpcRequests())
        if (request.method == "get_render_authoring_context") {
            model.receiveRpcResult(request.request_id, gl1eAuthoring(inputs).dump());
            authored = true;
        }
    REQUIRE(authored);
    scene.setModel(&model);
}

QGraphicsItem *passSocket(QGraphicsScene &scene, const QString &pass, const QString &port) {
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasDerivedSocketItem} &&
            item->data(PelicanStudio::NodeGraphNameRole).toString() == pass &&
            item->data(PelicanStudio::GraphCanvasPortRole).toString() == port &&
            item->data(PelicanStudio::GraphCanvasSocketDirectionRole).toString() ==
                QLatin1String{"input"})
            return item;
    return nullptr;
}

// The edit_preset_pass params the Pass Editor saves a root fullscreen pass
// with, for `inputs`: the root's digest alone, the pass, and its whole input,
// output and shader.
Json gl1eSaved(const std::vector<std::string> &inputs) {
    const auto present = gl1ePresent(inputs);
    return {{"base_source_digest", "gl1e-root"},
            {"graph", "main_render"},
            {"pass_name", "present"},
            {"input", inputs},
            {"output", present.at("output")},
            {"shader", present.at("shader")}};
}

} // namespace

TEST_CASE("GL-1e a rendering-section pass takes lines: + input, replace, append, the solid line, "
          "Delete",
          "[devstudio][graph-canvas][scene][gl1e]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    std::vector<PelicanStudio::FramePlanEditRequest> sent;
    scene.passInputEditRequested = [&](const PelicanStudio::FramePlanEditRequest &request,
                                       std::function<PelicanStudio::FramePlanEditRequest()>) {
        sent.push_back(request);
    };
    const auto image = [&] {
        QGraphicsItem *socket = socketItem(scene, QStringLiteral("shade__color"),
                                           QStringLiteral("resource"), QStringLiteral("output"));
        REQUIRE(socket != nullptr);
        return socket->sceneBoundingRect().center();
    };

    SECTION("an empty present: + input only, and a drop there sends the Frame Plan's edit") {
        openGl1e(model, scene, {});
        // Decision 1: the empty slot, and nothing else to drop on.
        QGraphicsItem *slot = passSocket(scene, QStringLiteral("present"), QStringLiteral("in:+"));
        REQUIRE(slot != nullptr);
        CHECK(slot->data(PelicanStudio::GraphCanvasPassInputRole).toLongLong() == -1);
        CHECK(slot->data(PelicanStudio::GraphCanvasSocketKeyRole).toString() ==
              QStringLiteral("in:variadic"));
        CHECK(itemsOfKind(scene, PelicanStudio::GraphCanvasPassInputEdgeItem).empty());
        const auto sockets = scene.passInputSockets("present");
        REQUIRE(sockets.has_value());
        REQUIRE(sockets->sockets.size() == 1);

        // Decision 2: the image's output onto it -- one edit_preset_pass,
        // the shape the Pass Editor saves present with, and exactly what
        // the Frame Plan's drop on the same slot builds.
        drag(scene, image(), slot->sceneBoundingRect().center());
        REQUIRE(sent.size() == 1);
        CHECK(sent[0].method == "edit_preset_pass");
        CHECK(sent[0].params == gl1eSaved({"shade__color"}));
        const auto frame_plan = PelicanStudio::framePlanSocketConnectRequest(
            PelicanStudio::framePlanAuthoringFromContext(gl1eAuthoring({})),
            model.passInputPlan(), "main_render", "present", "shade__color",
            PelicanStudio::FramePlanSocketDrop{PelicanStudio::FramePlanSocketDrop::Kind::variadic,
                                               0});
        CHECK(sent[0].method == frame_plan.method);
        CHECK(sent[0].params == frame_plan.params);
        // Nothing went out as a graph edit.
        for (const auto &request : model.takeRpcRequests())
            CHECK(request.method != "edit_graph_document");
    }

    SECTION("a connected present: the solid line, a drop on it replaces, on + input appends") {
        openGl1e(model, scene, {"old_image"});
        QGraphicsItem *connected =
            passSocket(scene, QStringLiteral("present"), QStringLiteral("old_image"));
        REQUIRE(connected != nullptr);
        CHECK(connected->data(PelicanStudio::GraphCanvasPassInputRole).toLongLong() == 0);
        CHECK(connected->data(PelicanStudio::GraphCanvasSocketKeyRole).toString() ==
              QStringLiteral("in@0"));
        REQUIRE(passSocket(scene, QStringLiteral("present"), QStringLiteral("in:+")) != nullptr);

        drag(scene, image(), connected->sceneBoundingRect().center());
        REQUIRE(sent.size() == 1);
        CHECK(sent[0].params == gl1eSaved({"shade__color"}));

        QGraphicsItem *slot = passSocket(scene, QStringLiteral("present"), QStringLiteral("in:+"));
        drag(scene, image(), slot->sceneBoundingRect().center());
        REQUIRE(sent.size() == 2);
        CHECK(sent[1].params == gl1eSaved({"old_image", "shade__color"}));
    }

    SECTION("present reads the image: one solid line from its output, no dashed one; Delete") {
        openGl1e(model, scene, {"shade__color"});
        const auto lines = itemsOfKind(scene, PelicanStudio::GraphCanvasPassInputEdgeItem);
        REQUIRE(lines.size() == 1);
        auto *line = dynamic_cast<QGraphicsPathItem *>(lines.front());
        REQUIRE(line != nullptr);
        CHECK(line->data(PelicanStudio::NodeGraphFromNameRole).toString() ==
              QStringLiteral("shade__color"));
        CHECK(line->data(PelicanStudio::NodeGraphToNameRole).toString() ==
              QStringLiteral("present"));
        CHECK(line->pen().style() == Qt::SolidLine);
        // It runs from the image's output to present's socket.
        CHECK(QLineF(line->mapToScene(line->path().pointAtPercent(0.0)), image()).length() < 1.0);
        QGraphicsItem *end =
            passSocket(scene, QStringLiteral("present"), QStringLiteral("shade__color"));
        REQUIRE(end != nullptr);
        CHECK(QLineF(line->mapToScene(line->path().pointAtPercent(1.0)),
                     end->sceneBoundingRect().center())
                  .length() < 1.0);
        // The plan's dashed line for the same read is not drawn beside it.
        for (QGraphicsItem *dashed : itemsOfKind(scene, PelicanStudio::GraphCanvasDerivedEdgeItem))
            CHECK(dashed->data(PelicanStudio::NodeGraphToNameRole).toString() !=
                  QStringLiteral("present"));

        // Picked and Delete: the input comes out.
        // Where nothing is drawn over it (the graph's pass stands between).
        std::optional<QPointF> bare;
        for (int step = 1; step < 20 && !bare; ++step) {
            const QPointF at = line->mapToScene(line->path().pointAtPercent(step / 20.0));
            const auto under = scene.items(at);
            if (!under.isEmpty() && under.front() == line) bare = at;
        }
        REQUIRE(bare.has_value());
        click(scene, *bare);
        REQUIRE(scene.selectedPassInput().has_value());
        CHECK(scene.selectedPassInput()->pass == "present");
        CHECK(scene.selectedPassInput()->input_index == 0);
        pressKey(scene, Qt::Key_Delete);
        REQUIRE(sent.size() == 1);
        CHECK(sent[0].params == gl1eSaved({}));
        CHECK_FALSE(scene.selectedPassInput().has_value());
    }

    SECTION("a pass the root does not declare takes no lines") {
        model.setConnected(true);
        for (const auto &request : model.takeRpcRequests())
            if (request.method == "get_graph_instances")
                model.receiveRpcResult(request.request_id, gl1eRead().dump());
        for (const auto &request : model.takeRpcRequests())
            if (request.method == "get_frame_plan")
                model.receiveRpcResult(request.request_id, gl1ePlan({}).dump());
        auto authoring = gl1eAuthoring({});
        authoring["graphs"][0]["passes"][0]["editable_root_pass"] = false;
        for (const auto &request : model.takeRpcRequests())
            if (request.method == "get_render_authoring_context")
                model.receiveRpcResult(request.request_id, authoring.dump());
        scene.setModel(&model);
        CHECK_FALSE(scene.passInputSockets("present").has_value());
        CHECK(passSocket(scene, QStringLiteral("present"), QStringLiteral("in:+")) == nullptr);
    }
}

TEST_CASE("GL-1e the Graph tab sends a pass's input edit, its Undo reaches the render history, "
          "and an engine before RS-3 says why it refused",
          "[devstudio][graph-canvas][scene][gl1e]") {
    (void)application();
    RecordedCapability transport;
    auto capability = transport.capability();
    capability.requestFramePlan = [&](QString *) {
        transport.calls.push_back({QStringLiteral("get_frame_plan")});
        return transport.next++;
    };
    capability.requestRenderAuthoringContext = [&](QString *) {
        transport.calls.push_back({QStringLiteral("get_render_authoring_context")});
        return transport.next++;
    };
    capability.requestEditPresetPass = [&](const QJsonObject &params, QString *) {
        RecordedCapability::Call call{QStringLiteral("edit_preset_pass")};
        call.body = QJsonDocument(params).toJson(QJsonDocument::Compact);
        transport.calls.push_back(call);
        return transport.next++;
    };
    PelicanStudio::GraphCanvasWidget widget{capability};
    // RU-1's two actions, as the window hands them over.
    QAction render_undo{QStringLiteral("Undo Render Edit")};
    QAction render_redo{QStringLiteral("Redo Render Edit")};
    int undone = 0;
    QObject::connect(&render_undo, &QAction::triggered, [&] { ++undone; });
    render_undo.setEnabled(false);
    render_redo.setEnabled(false);
    widget.setRenderHistoryActions(&render_undo, &render_redo);
    widget.resize(1100, 700);
    widget.show();
    QCoreApplication::processEvents();
    const auto answer = [&](const char *method, const Json &reply) {
        const qint64 id = transport.idOf(QString::fromLatin1(method));
        REQUIRE(id != 0);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    };
    const auto open = [&](const std::vector<std::string> &inputs) {
        answer("get_graph_instances", gl1eRead());
        answer("get_frame_plan", gl1ePlan(inputs));
        answer("get_render_authoring_context", gl1eAuthoring(inputs));
    };
    answer("get_graph_catalog", visualCatalog());
    open({});
    auto &scene = widget.scene();
    auto *notice = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasNotice"));
    REQUIRE(notice != nullptr);
    const auto drop = [&] {
        QGraphicsItem *from = socketItem(scene, QStringLiteral("shade__color"),
                                         QStringLiteral("resource"), QStringLiteral("output"));
        QGraphicsItem *slot = passSocket(scene, QStringLiteral("present"), QStringLiteral("in:+"));
        REQUIRE(from != nullptr);
        REQUIRE(slot != nullptr);
        const int before = transport.countOf(QStringLiteral("edit_preset_pass"));
        drag(scene, from->sceneBoundingRect().center(), slot->sceneBoundingRect().center());
        QCoreApplication::processEvents();
        REQUIRE(transport.countOf(QStringLiteral("edit_preset_pass")) == before + 1);
        CHECK(Json::parse(transport.calls.back().body.toStdString()) ==
              gl1eSaved({"shade__color"}));
        answer("edit_preset_pass", Json{{"status", "accepted"}, {"ticket", "gl1e"}});
    };

    // Decision 3: an engine before RS-3 runs the graph's pass after present.
    drop();
    const std::string refusal =
        "Pass input target is not produced as an earlier output: present reads shade__color";
    answer("get_edit_result", Json{{"status", "rejected"},
                                   {"committed", false},
                                   {"error", {{"code", "render_pipeline_preflight_failed"},
                                              {"message", refusal}}}});
    CHECK(notice->toolTip() ==
          QString::fromStdString("render_pipeline_preflight_failed: " + refusal) +
              QString::fromUtf8(" \xE2\x80\x94 the graph pass that draws it runs after this pass"));
    // The sentence is cut to one line; the phrase is not.
    CHECK(notice->text().endsWith(
        QString::fromUtf8(" \xE2\x80\x94 the graph pass that draws it runs after this pass")));

    // Committed: the tab says so, reads the document again, and its Undo
    // takes the edit back through the render edit history.
    drop();
    const int reads = transport.countOf(QStringLiteral("get_graph_instances"));
    answer("get_edit_result", Json{{"status", "committed"},
                                   {"committed", true},
                                   {"source_digest", {{"hex", "gl1e-root-2"}}}});
    CHECK(notice->text() == QStringLiteral("Pass inputs saved."));
    CHECK(transport.countOf(QStringLiteral("get_graph_instances")) == reads + 1);
    render_undo.setEnabled(true);
    QCoreApplication::processEvents();
    auto *undo = widget.findChild<QAction *>(QStringLiteral("pelican.graphCanvas.undo"));
    REQUIRE(undo != nullptr);
    CHECK(undo->isEnabled());
    undo->trigger();
    QCoreApplication::processEvents();
    CHECK(undone == 1);
}
