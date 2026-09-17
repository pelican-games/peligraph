#include "graphcanvaswidget.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QFont>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
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
        REQUIRE(request.method == "get_graph_values");
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
