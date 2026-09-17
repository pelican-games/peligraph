#include "graphcanvaswidget.hpp"
#include "studiotheme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QFont>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QKeyEvent>
#include <QRectF>
#include <QScrollBar>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QWidget>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <algorithm>
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
            {"definitions", document.at("definitions")}};
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

void pressKey(QGraphicsScene &scene, int key) {
    QKeyEvent event{QEvent::KeyPress, key, Qt::NoModifier};
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

void typeKey(QWidget *widget, int key) {
    REQUIRE(widget != nullptr);
    QKeyEvent event{QEvent::KeyPress, key, Qt::NoModifier};
    QApplication::sendEvent(widget, &event);
    // The editor reports its decision from the event loop: it cannot delete
    // itself inside the event it is still dispatching.
    QCoreApplication::processEvents();
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

    scene.autoLayout();
    REQUIRE(nodeItem(scene, QStringLiteral("left"))->pos() == laid_out);
    // The document said nothing about any of this.
    requireNoEdit(model);

    REQUIRE(model.enterInstance("left"));
    scene.repopulate();
    REQUIRE(nodeItem(scene, QStringLiteral("sum"))->pos() == definition_moved);
    scene.autoLayout();
    REQUIRE(nodeItem(scene, QStringLiteral("sum"))->pos() ==
            definition_laid_out);
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
        seam.requestEditGraphDocument = [note](const QString &, const QByteArray &,
                                               QString *) {
            return note(QStringLiteral("edit_graph_document"));
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
