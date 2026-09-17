#include "graphcanvaswidget.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <algorithm>
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

Json fixtureDocument() {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" /
                      "fixtures" / "graph_instance_inputs" / "passes" /
                      "main.json";
    std::ifstream stream{path};
    REQUIRE(stream.good());
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return Json::parse(buffer.str());
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

void answer(GraphCanvasModel &model, const Json &document) {
    for (const auto &request : model.takeRpcRequests()) {
        if (request.method == "get_graph_instances")
            model.receiveRpcResult(request.request_id, context(document).dump());
        else if (request.method == "get_graph_catalog")
            model.receiveRpcResult(
                request.request_id,
                Json{{"schema", "pelican.graph_catalog"}, {"version", 1},
                     {"modules", Json::array()}}
                    .dump());
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
