#include <QPointer>
#include "graphcanvasaudioanalysis.hpp"
#include "frameplanedit.hpp"
#include "fullscreenshadertemplate.hpp"
#include "graphcanvascomputefields.hpp"
#include "graphcanvasloopfields.hpp"
#include "graphcanvasparameterfields.hpp"
#include "graphcanvasposefields.hpp"
#include "graphcanvasrenderfields.hpp"
#include "graphcanvasvaluefields.hpp"
#include "graphcanvaswidget.hpp"
#include "rendertargetform.hpp"
#include "renderedithistory.hpp"
#include "renderconfigdigest.hpp"
#include "graphfoldgroups.hpp"
#include "inspectormodel.hpp"
#include "graphcomposition.hpp"
#include "graphmodule.hpp"
#include "graphpartition.hpp"
#include "panelhelp.hpp"
#include "problemsmodel.hpp"
#include "studiotheme.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
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
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QKeyEvent>
#include <QLineF>
#include <QRectF>
#include <QScrollBar>
#include <QTabWidget>
#include <QCheckBox>
#include <QClipboard>
#include <QGuiApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QSpinBox>
#include <QThread>
#include <QStandardItemModel>
#include <QToolButton>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QWidget>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <thread>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <tuple>
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

// UX-6b, decisions 2 and 4: a root node's band says what the plan says its
// pass does for nothing, then its GPU time -- which changes in place, on the
// band that is there.
TEST_CASE("UX-6b a root node's band carries its unread output and its GPU time",
          "[devstudio][graph-canvas][scene][ux6b]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    const Json document{{"schema", "pelican.graph"}, {"version", 1},
                        {"definitions", Json::object()}, {"outputs", Json::object()},
                        {"nodes", Json::array({{{"id", "blur"},
                                                {"op", "render.fullscreen"},
                                                {"parameters", Json::object()},
                                                {"outputs", Json::array({"color"})}}})}};
    answer(model, document);
    const Json plan{
        {"schema", "pelican.frame_plan"},
        {"version", 1},
        {"graph", "main_render"},
        {"loops", Json::array()},
        {"nodes", Json::array({{{"name", "blur"}, {"kind", "render"}, {"order", 0}, {"level", 0},
                                {"source", "engine"}, {"reads", Json::array()},
                                {"writes", Json::array({"blur__color"})},
                                {"reads_history", Json::array()}, {"declaration_index", 0},
                                {"origin", {{"graph", "passes/main.json"}, {"node", "blur"}}}}})},
        {"barriers", Json::array()},
        {"diagnostics", Json::array({{{"kind", "unread_output"},
                                      {"subject", "blur"},
                                      {"detail", {{"resource", "blur__color"}}}}})},
        {"deactivated_graph_nodes", Json::array()}};
    bool planned = false;
    for (const auto &request : model.takeRpcRequests())
        if (request.method == "get_frame_plan") {
            model.receiveRpcResult(request.request_id, plan.dump());
            planned = true;
        }
    REQUIRE(planned);
    for (const auto &domain : GraphCanvasModel::domainNames())
        model.setDomainShown(domain, domain == "render");
    scene.setModel(&model);
    const auto band = [&]() -> QString {
        const auto bands = itemsOfKind(scene, PelicanStudio::GraphCanvasLayerBandItem);
        if (bands.size() != 1) return QStringLiteral("<%1 bands>").arg(bands.size());
        return dynamic_cast<QGraphicsSimpleTextItem *>(bands.front())->text();
    };
    CHECK(band() == QStringLiteral("output unread"));
    const auto *node = nodeItem(scene, QStringLiteral("blur"));
    REQUIRE(node != nullptr);
    scene.setPlanNodeTimes({{"blur", 0.0213}, {"present", 0.5}});
    CHECK(band() == QStringLiteral("output unread \u00b7 0.02 ms"));
    // In place: the node is the one that was drawn.
    CHECK(nodeItem(scene, QStringLiteral("blur")) == node);
    scene.setPlanNodeTimes({});
    CHECK(band() == QStringLiteral("output unread"));
}

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

// UX-5a, item 3. A Fit view in a small pane (the bottom dock) shrank a big
// graph until no word could be read nor a band pressed. The Graph's fit stops
// at a readable zoom and then looks at the node in hand.
TEST_CASE("UX-5a Fit view stops at a readable zoom and looks at the picked node",
          "[devstudio][graph-canvas][scene][ux5a][fit]") {
    (void)application();
    QGraphicsScene scene;
    // Far wider than the pane, with the node in hand at its right end.
    scene.addRect(QRectF{0.0, 0.0, 4000.0, 300.0});
    const QRectF picked{3700.0, 100.0, 200.0, 120.0};
    scene.addRect(picked);
    PelicanStudio::NodeCanvasView view{&scene};
    view.resize(600, 400);
    view.show();
    QCoreApplication::processEvents();

    // The control: no floor, and the fit shrinks to the clamp's minimum.
    view.fitView();
    QCoreApplication::processEvents();
    CHECK(view.zoomScale() < PelicanStudio::GraphCanvasFitFloor);

    view.setFitFloor(PelicanStudio::GraphCanvasFitFloor, [&] { return std::optional<QRectF>{picked}; });
    view.fitView();
    QCoreApplication::processEvents();
    CHECK(std::abs(view.zoomScale() - PelicanStudio::GraphCanvasFitFloor) < 0.000001);
    const QRectF visible = view.mapToScene(view.viewport()->rect()).boundingRect();
    CHECK(visible.contains(picked.center()));

    // Content that fits above the floor is fitted as before.
    QGraphicsScene small;
    small.addRect(QRectF{0.0, 0.0, 300.0, 200.0});
    view.setScene(&small);
    view.fitView();
    QCoreApplication::processEvents();
    CHECK(view.zoomScale() > PelicanStudio::GraphCanvasFitFloor);
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
        seam.requestEngineShaders = [note](QString *) {
            return note(QStringLiteral("get_engine_shaders"));
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

// Runs `open` and then `inside` while the modal palette it opens is up.
bool whilePaletteOpenAfter(const std::function<void()> &open,
                           const std::function<void(QDialog &)> &inside) {
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
    open();
    while (poll.isActive()) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QCoreApplication::processEvents();
    return ran;
}

// Clicks Add and runs `inside` while the modal palette it opens is up.
bool whilePaletteOpen(QPushButton &add, const std::function<void(QDialog &)> &inside) {
    return whilePaletteOpenAfter([&add] { add.click(); }, inside);
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
            {"access", "read"}, {"binding", "input"}, {"input_index", index},
            // UX-7, decision 1: a fullscreen pass's input is clamped at its edge.
            {"sampling", {{"address", "clamp_to_edge"}}}};
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
    // UX-3, decision 4: the name Studio proposes, after the loop that runs
    // it, chosen.
    CHECK(name->text() == QStringLiteral("steps_body"));
    CHECK(name->selectedText() == QStringLiteral("steps_body"));
    CHECK(apply->isEnabled());
    // The same name renames nothing.
    name->setText(QStringLiteral("body"));
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

// UX-5a, item 4: Add loop's new zone was put on the folded present. It goes
// clear of all that is drawn, the folded nodes included.
TEST_CASE("UX-5a Add loop puts the new zone where nothing drawn is, folded nodes included",
          "[devstudio][graph-canvas][scene][ux5a][add-loop]") {
    (void)application();
    auto document = q19ZoneDocument();
    document["nodes"].erase(1);
    auto plan = gl1cPlan();
    auto &nodes = plan["nodes"];
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [](const Json &node) {
                                   return node.value("name", std::string{}).starts_with("steps/");
                               }),
                nodes.end());
    ZoneHarness h{document};
    h.plan = plan;
    h.open();
    h.answerReads();
    h.scene.repopulate();
    REQUIRE(h.scene.addLoop({}, "fresh_body"));
    h.commit();
    gl1cCheckClear(h.scene, false);
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

// RT-D: a tab and the real shared history over one fake engine document.
// Replies carry snapshots and written digests; assertions inspect the restore
// payload, so a QAction firing is never treated as a successful Undo.
namespace {
struct RtdTab {
    struct Request {
        std::int64_t id;
        std::string method;
        Json params;
    };
    QApplication &app = application();
    RecordedCapability transport;
    std::vector<Request> history_requests;
    std::set<std::int64_t> answered;
    std::set<qint64> tab_reads;
    std::int64_t next_history = 100000;
    int generation = 0;
    Json document;
    QAction render_undo{QStringLiteral("Undo Render Edit")};
    QAction render_redo{QStringLiteral("Redo Render Edit")};
    PelicanStudio::RenderEditHistory history{
        PelicanStudio::RenderEditHistory::Transport{
            .request = [this](const std::string &method, const Json &params, std::string *) {
                const auto id = next_history++;
                history_requests.push_back({id, method, params});
                return id;
            },
            .schedule = [](int, std::function<void()>) {},
        }};
    std::unique_ptr<PelicanStudio::GraphCanvasWidget> widget;
    std::function<void(bool, QString)> ready_changed;
    std::function<void(qint64, QString)> failure;

    RtdTab() {
        (void)application();
        const auto read = gl1eRead();
        document = {{"schema", "pelican.graph"}, {"version", 1},
                    {"nodes", read.at("nodes")}, {"definitions", read.at("definitions")},
                    {"layout", Json::object()}, {"outputs", Json::object()},
                    {"rendering", {{"present", gl1ePresent({})}}}};
        document["nodes"][1]["parameters"] = {{"shader", {{"fragment", "engine://passthrough"}}}};
        auto capability = transport.capability();
        capability.ready = [this](QObject *, auto handler) {
            ready_changed = handler;
            handler(true, {});
        };
        capability.failure = [this](QObject *, auto handler) { failure = handler; };
        capability.requestFramePlan = [this](QString *) { return record("get_frame_plan"); };
        capability.requestRenderAuthoringContext = [this](QString *) {
            return record("get_render_authoring_context");
        };
        capability.requestEditPresetPass = [this](const QJsonObject &params, QString *) {
            const auto body = Json::parse(QJsonDocument(params).toJson(QJsonDocument::Compact).toStdString());
            history.beforeRequest("edit_preset_pass", body);
            const auto id = record("edit_preset_pass", body);
            history.requestSent(id, "edit_preset_pass", body);
            return id;
        };
        QObject::connect(&render_undo, &QAction::triggered, [&] { history.undo(); });
        QObject::connect(&render_redo, &QAction::triggered, [&] { history.redo(); });
        history.changed = [this] {
            render_undo.setEnabled(history.canUndo());
            render_redo.setEnabled(history.canRedo());
        };
        history.restored = [this](const Json &reply, bool) {
            widget->renderHistoryRestored(reply.at("source_digest").at("hex").get<std::string>());
            widget->refresh();
        };
        render_undo.setEnabled(false);
        render_redo.setEnabled(false);
        widget = std::make_unique<PelicanStudio::GraphCanvasWidget>(capability);
        widget->setRenderHistory(&history);
        widget->setRenderHistoryActions(&render_undo, &render_redo);
        widget->resize(1100, 800);
        widget->show();
        readTab();
    }

    qint64 record(const char *method, const Json &params = Json::object()) {
        RecordedCapability::Call call{QString::fromLatin1(method)};
        call.body = QByteArray::fromStdString(params.dump());
        transport.calls.push_back(std::move(call));
        return transport.next++;
    }
    std::string digest() const { return std::string(63, 'a') + "0123456789abcdef"[generation % 16]; }
    Json snapshot() const {
        return {{"documents", Json::array({{{"reference", "project://passes/main_rendering_config.json"},
                                            {"kind", "root"}, {"digest", digest()},
                                            {"content", document.dump()}}})}};
    }
    Json committed() const {
        return {{"status", "committed"}, {"committed", true},
                {"source_digest", {{"hex", digest()}}},
                {"documents", {{"project://passes/main_rendering_config.json", digest()}}}};
    }
    void reply(qint64 id, const Json &result) {
        history.receiveResult(id, result);
        transport.deliver(id, QByteArray::fromStdString(result.dump()));
    }
    void answerTab(const char *method, const Json &result) {
        const auto id = transport.idOf(QString::fromLatin1(method));
        REQUIRE(id != 0);
        reply(id, result);
    }
    void readHistory() {
        for (std::size_t i = 0; i < history_requests.size(); ++i) {
            const auto request = history_requests[i];
            if (request.method == "get_render_documents" && answered.insert(request.id).second) {
                // A refused or abandoned edit no longer needs its snapshot.
                if (history.waitingFor(request.id)) REQUIRE(history.receiveResult(request.id, snapshot()));
            }
        }
    }
    void readTab() {
        for (std::size_t i = 0; i < transport.calls.size(); ++i) {
            const auto call = transport.calls[i];
            const auto id = static_cast<qint64>(i + 1);
            if (!tab_reads.insert(id).second) continue;
            if (call.method == QStringLiteral("get_graph_instances")) {
                auto read = context(document);
                read["source_digest"]["hex"] = digest();
                read["root_schema"] = "pelican.graph";
                read["source_reference"] = "project://passes/main_rendering_config.json";
                reply(id, read);
            } else if (call.method == QStringLiteral("get_graph_catalog")) {
                reply(id, visualCatalog());
            } else if (call.method == QStringLiteral("get_frame_plan")) {
                reply(id, gl1ePlan(document.at("rendering").at("present").at("input")
                                      .get<std::vector<std::string>>()));
            } else if (call.method == QStringLiteral("get_render_authoring_context")) {
                auto authoring = gl1eAuthoring(document.at("rendering").at("present").at("input")
                                                  .get<std::vector<std::string>>());
                authoring["source_digest"]["hex"] = digest();
                authoring["graphs"][0]["passes"][0]["declaration"] = document["rendering"]["present"];
                authoring["graphs"][0]["passes"][0]["root_declaration"] = document["rendering"]["present"];
                reply(id, authoring);
            }
        }
        QCoreApplication::processEvents();
    }
    Request lastHistory(const char *method) const {
        for (auto it = history_requests.rbegin(); it != history_requests.rend(); ++it)
            if (it->method == method) return *it;
        FAIL("Missing history request " << method);
        return {};
    }
    int historyCount(const char *method) const {
        return static_cast<int>(std::ranges::count(history_requests, std::string{method}, &Request::method));
    }
    Json tabEdit() const {
        const auto id = transport.idOf(QStringLiteral("edit_graph_document"));
        REQUIRE(id != 0);
        return Json::parse(transport.calls[static_cast<std::size_t>(id - 1)].body.toStdString());
    }
    void graphCommit() {
        const auto ops = tabEdit();
        answerTab("edit_graph_document", {{"status", "accepted"}, {"ticket", "graph"}});
        document = Pelican::editGraphDocument(document, ops);
        ++generation;
        answerTab("get_edit_result", committed());
        readTab();
        REQUIRE(widget->model().canEdit());
    }
    void graphLabel() {
        auto &scene = widget->scene();
        REQUIRE(scene.beginLabelEdit("shade"));
        scene.setInlineEditText(QStringLiteral("Graph edit"));
        REQUIRE(scene.commitInlineEdit());
        graphCommit();
    }
    void renderEdit(bool from_tab = true) {
        if (from_tab) {
            auto &scene = widget->scene();
            auto *from = socketItem(scene, QStringLiteral("shade__color"), QStringLiteral("resource"),
                                    QStringLiteral("output"));
            auto *to = passSocket(scene, QStringLiteral("present"), QStringLiteral("in:+"));
            REQUIRE(from != nullptr);
            REQUIRE(to != nullptr);
            drag(scene, from->sceneBoundingRect().center(), to->sceneBoundingRect().center());
            REQUIRE(transport.idOf(QStringLiteral("edit_preset_pass")) != 0);
            readHistory();
            answerTab("edit_preset_pass", {{"status", "accepted"}, {"ticket", "render-tab"}});
        } else {
            history.beforeRequest("edit_preset_pass", {{"pass_name", "other-panel"}});
            history.requestSent(90000, "edit_preset_pass", {});
            readHistory();
            REQUIRE(history.receiveResult(90000, {{"status", "accepted"}, {"ticket", "render-other"}}));
        }
        auto &present = document["rendering"]["present"];
        if (from_tab) present["input"] = Json::array({"shade__color"});
        else present["shader"]["fragment"] = "engine://scene_present";
        ++generation;
        REQUIRE(history.receiveResult(lastHistory("get_edit_result").id, committed()));
        readHistory();
        if (from_tab) {
            answerTab("get_edit_result", committed());
            readTab();
        }
        REQUIRE(history.undoStack().size() >= 1);
    }
    Json finishRestore(bool reject = false) {
        readHistory();
        const auto restore = lastHistory("restore_render_documents");
        const auto documents = restore.params.at("documents");
        REQUIRE(documents.size() == 1);
        REQUIRE(documents[0].at("reference") == "project://passes/main_rendering_config.json");
        if (reject) {
            REQUIRE(history.receiveResult(restore.id, {{"status", "rejected"},
                       {"error", {{"code", "external_modification"},
                                  {"reference", "project://passes/main_rendering_config.json"}}}}));
        } else {
            REQUIRE(documents[0].at("base_digest") == digest());
            document = Json::parse(documents[0].at("content").get<std::string>());
            ++generation;
            REQUIRE(history.receiveResult(restore.id, {{"status", "accepted"}, {"ticket", "restore"}}));
            REQUIRE(history.receiveResult(lastHistory("get_edit_result").id, committed()));
            readTab();
        }
        return documents;
    }
};
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

    SECTION("GL-1f: the pass's own output onto + input names the image it resolves to") {
        openGl1e(model, scene, {});
        QGraphicsItem *from = socketItem(scene, QStringLiteral("shade"), QStringLiteral("color"),
                                         QStringLiteral("output"));
        QGraphicsItem *slot = passSocket(scene, QStringLiteral("present"), QStringLiteral("in:+"));
        REQUIRE(from != nullptr);
        REQUIRE(slot != nullptr);
        drag(scene, from->sceneBoundingRect().center(), slot->sceneBoundingRect().center());
        REQUIRE(sent.size() == 1);
        CHECK(sent[0].params == gl1eSaved({"shade__color"}));
    }

    SECTION("present reads the image: one solid line from the pass that wrote it; Delete") {
        openGl1e(model, scene, {"shade__color"});
        const auto lines = itemsOfKind(scene, PelicanStudio::GraphCanvasPassInputEdgeItem);
        REQUIRE(lines.size() == 1);
        auto *line = dynamic_cast<QGraphicsPathItem *>(lines.front());
        REQUIRE(line != nullptr);
        // GL-1f, decision 3: from the pass's output that hands the image on,
        // not from the image node.
        CHECK(line->data(PelicanStudio::NodeGraphFromNameRole).toString() ==
              QStringLiteral("shade"));
        CHECK(line->data(PelicanStudio::NodeGraphToNameRole).toString() ==
              QStringLiteral("present"));
        CHECK(line->pen().style() == Qt::SolidLine);
        QGraphicsItem *writer = socketItem(scene, QStringLiteral("shade"), QStringLiteral("color"),
                                           QStringLiteral("output"));
        REQUIRE(writer != nullptr);
        CHECK(QLineF(line->mapToScene(line->path().pointAtPercent(0.0)),
                     writer->sceneBoundingRect().center())
                  .length() < 1.0);
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
    RtdTab tab;
    auto &widget = *tab.widget;
    auto &transport = tab.transport;
    auto &scene = widget.scene();
    auto *notice = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasNotice"));
    REQUIRE(notice != nullptr);
    const auto original = tab.document.dump();
    const auto answer = [&](const char *method, const Json &reply) {
        if (std::string_view{method} == "get_edit_result")
            REQUIRE(tab.history.receiveResult(tab.lastHistory("get_edit_result").id, reply));
        tab.answerTab(method, reply);
    };
    const auto drop = [&] {
        auto *from = socketItem(scene, QStringLiteral("shade__color"), QStringLiteral("resource"),
                                QStringLiteral("output"));
        auto *slot = passSocket(scene, QStringLiteral("present"), QStringLiteral("in:+"));
        REQUIRE(from != nullptr);
        REQUIRE(slot != nullptr);
        const auto before = transport.countOf(QStringLiteral("edit_preset_pass"));
        drag(scene, from->sceneBoundingRect().center(), slot->sceneBoundingRect().center());
        REQUIRE(transport.countOf(QStringLiteral("edit_preset_pass")) == before + 1);
        const auto id = transport.idOf(QStringLiteral("edit_preset_pass"));
        auto expected = gl1eSaved({"shade__color"});
        expected["base_source_digest"] = tab.digest();
        CHECK(Json::parse(transport.calls[static_cast<std::size_t>(id - 1)].body.toStdString()) == expected);
        tab.readHistory();
        answer("edit_preset_pass", {{"status", "accepted"}, {"ticket", "gl1e"}});
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

    // UX-12d, decision 5 (engine FR-3): a feature's target nobody writes is
    // read by its code -- the outputs that could write it, not the phrase
    // about the graph pass's order, though the message still says the frame
    // planner's words and names an image a graph pass writes.
    drop();
    answer("get_edit_result",
           Json{{"status", "rejected"},
                {"committed", false},
                {"error",
                 {{"code", "feature.required_target_unwritten@1"},
                  {"message", "Pass input target is not produced as an earlier output: shade__color"},
                  {"feature", "sky_ambient"},
                  {"target", "scene_depth"},
                  {"type", {{"semantic", "pelican.render.depth@1"}, {"arguments", Json::object()}}},
                  {"writer_candidates", Json::array({{{"node", "shade"},
                                                      {"pass", "shade"},
                                                      {"port", "depth"},
                                                      {"resource", "shade__depth"}}})},
                  {"document", "engine://features/sky_ambient.json"}}}});
    CHECK(notice->toolTip().startsWith(QStringLiteral(
        "feature.required_target_unwritten@1: sky_ambient reads scene_depth, which no pass writes "
        "before it. Write it from shade.depth.")));
    CHECK_FALSE(notice->toolTip().contains(QStringLiteral("runs after this pass")));

    drop();
    const int reads = transport.countOf(QStringLiteral("get_graph_instances"));
    tab.document["rendering"]["present"]["input"] = Json::array({"shade__color"});
    ++tab.generation;
    answer("get_edit_result", tab.committed());
    CHECK(notice->text() == QStringLiteral("Pass inputs saved."));
    CHECK(transport.countOf(QStringLiteral("get_graph_instances")) == reads + 1);
    tab.readHistory();
    tab.readTab();
    REQUIRE(tab.history.undoStack().size() == 1);
    const auto serial = tab.history.undoStack().back().serial;

    SECTION("the connected history restores the exact document and content") {
        auto *undo = widget.findChild<QAction *>(QStringLiteral("pelican.graphCanvas.undo"));
        REQUIRE(undo != nullptr);
        REQUIRE(undo->isEnabled());
        undo->trigger();
        REQUIRE(tab.history.busy());
        const auto restored = tab.finishRestore();
        CHECK(restored[0].at("content") == original);
        REQUIRE(tab.history.redoStack().size() == 1);
        CHECK(tab.history.redoStack().back().serial == serial);
    }
    SECTION("disconnecting the history removes the restore path even with enabled actions") {
        widget.setRenderHistory(nullptr);
        REQUIRE(tab.render_undo.isEnabled());
        const auto sent = tab.history_requests.size();
        scene.undoEdit();
        CHECK(tab.history_requests.size() == sent);
        CHECK_FALSE(scene.canUndoAny());
        REQUIRE(tab.history.undoStack().back().serial == serial);
        REQUIRE(tab.history.undo());
        CHECK(tab.finishRestore()[0].at("content") == original);
    }
    SECTION("disconnecting the history actions removes the restore path") {
        widget.setRenderHistoryActions(nullptr, nullptr);
        const auto sent = tab.history_requests.size();
        scene.undoEdit();
        CHECK(tab.history_requests.size() == sent);
        CHECK_FALSE(tab.history.busy());
        REQUIRE(tab.history.undoStack().size() == 1);
        CHECK(tab.history.undoStack().back().serial == serial);
        // The control disconnects only the tab: the shared history itself
        // still restores the same bytes through its public operation.
        REQUIRE(tab.history.undo());
        CHECK(tab.finishRestore()[0].at("content") == original);
    }
}

// --- GL-5a: a folded pass of the rendering section, edited beside the canvas.

TEST_CASE("GL-5a a rendering-section pass picked on the Graph is edited in the detail column: "
          "a shader pick is one edit_preset_pass with Undo, Serial one edit_target_planning, "
          "and a refusal reads as the inspector's sentence",
          "[devstudio][graph-canvas][scene][gl5a]") {
    (void)application();
    RecordedCapability transport;
    auto capability = transport.capability();
    std::vector<RtdTab::Request> history_requests;
    std::set<std::int64_t> history_answered;
    std::int64_t history_next = 100000;
    std::string history_digest = "gl1e-root";
    Json history_document{{"schema", "pelican.graph"},
                          {"rendering", {{"present", gl1ePresent({})}}}};
    const auto original_render = history_document.dump();
    PelicanStudio::RenderEditHistory history{PelicanStudio::RenderEditHistory::Transport{
        .request = [&](const std::string &method, const Json &params, std::string *) {
            const auto id = history_next++;
            history_requests.push_back({id, method, params});
            return id;
        },
        .schedule = [](int, std::function<void()>) {},
    }};
    const auto last_history = [&](const char *method) {
        const auto found = std::find_if(history_requests.rbegin(), history_requests.rend(),
            [&](const auto &request) { return request.method == method; });
        REQUIRE(found != history_requests.rend());
        return *found;
    };
    const auto read_history = [&] {
        for (std::size_t i = 0; i < history_requests.size(); ++i) {
            const auto request = history_requests[i];
            if (request.method == "get_render_documents" && history_answered.insert(request.id).second)
                REQUIRE(history.receiveResult(request.id, {{"documents", Json::array({
                    {{"reference", "fixture"}, {"kind", "root"}, {"digest", history_digest},
                     {"content", history_document.dump()}}})}}));
        }
    };
    Json history_params;
    std::string history_method;
    const auto record = [&](const char *method, const QJsonObject &params) {
        history_params = Json::parse(QJsonDocument(params).toJson(QJsonDocument::Compact).toStdString());
        history_method = method;
        history.beforeRequest(method, history_params);
        RecordedCapability::Call call{QString::fromLatin1(method)};
        call.body = QJsonDocument(params).toJson(QJsonDocument::Compact);
        transport.calls.push_back(call);
        const auto id = transport.next++;
        history.requestSent(id, method, history_params);
        read_history();
        return id;
    };
    capability.requestFramePlan = [&](QString *) {
        transport.calls.push_back({QStringLiteral("get_frame_plan")});
        return transport.next++;
    };
    capability.requestRenderAuthoringContext = [&](QString *) {
        transport.calls.push_back({QStringLiteral("get_render_authoring_context")});
        return transport.next++;
    };
    capability.requestEditPresetPass = [&](const QJsonObject &params, QString *) {
        return record("edit_preset_pass", params);
    };
    capability.requestEditTargetPlanning = [&](const QJsonObject &params, QString *) {
        return record("edit_target_planning", params);
    };
    capability.requestRootDocument = [&](const QString &reference, QString *) {
        transport.calls.push_back({QStringLiteral("get_render_documents"), reference});
        return transport.next++;
    };
    PelicanStudio::GraphCanvasWidget widget{capability};
    QAction render_undo{QStringLiteral("Undo Render Edit")};
    QAction render_redo{QStringLiteral("Redo Render Edit")};
    QObject::connect(&render_undo, &QAction::triggered, [&] { history.undo(); });
    QObject::connect(&render_redo, &QAction::triggered, [&] { history.redo(); });
    history.changed = [&] {
        render_undo.setEnabled(history.canUndo());
        render_redo.setEnabled(history.canRedo());
    };
    widget.setRenderHistory(&history);
    render_undo.setEnabled(false);
    render_redo.setEnabled(false);
    widget.setRenderHistoryActions(&render_undo, &render_redo);
    widget.resize(1100, 700);
    widget.show();
    QCoreApplication::processEvents();
    const auto answer = [&](const char *method, const Json &reply) {
        const qint64 id = transport.idOf(QString::fromLatin1(method));
        REQUIRE(id != 0);
        auto result = reply;
        if (std::string_view{method} == "get_edit_result") {
            if (result.value("committed", false)) {
                history_digest = result.at("source_digest").at("hex").get<std::string>();
                if (history_method == "edit_preset_pass")
                    history_document["rendering"]["present"]["shader"] = history_params.at("shader");
                else
                    history_document["rendering"]["target_planning"] = {
                        {"graphs", {{"main_render", {{"nodes", {{"present", {{"serial", true}}}}}}}}}};
                result["documents"] = {{"fixture", history_digest}};
            }
            REQUIRE(history.receiveResult(last_history("get_edit_result").id, result));
            read_history();
        } else if (history.waitingFor(id)) {
            REQUIRE(history.receiveResult(id, result));
        }
        transport.deliver(id, QByteArray::fromStdString(result.dump()));
        QCoreApplication::processEvents();
    };
    const auto sent = [&](const char *method) {
        const auto id = transport.idOf(QString::fromLatin1(method));
        REQUIRE(id != 0);
        return Json::parse(transport.calls[static_cast<std::size_t>(id - 1)].body.toStdString());
    };
    // The root as get_render_documents gives it, with or without present's
    // Serial in its target_planning.
    const auto root = [](bool serial) {
        Json content{{"schema", "pelican.graph"}, {"rendering", Json::object()}};
        if (serial)
            content["rendering"]["target_planning"] = {
                {"graphs", {{"main_render", {{"nodes", {{"present", {{"serial", true}}}}}}}}}};
        return Json{{"documents",
                     Json::array({{{"reference", "fixture"}, {"content", content.dump()}}})}};
    };
    answer("get_graph_catalog", visualCatalog());
    answer("get_graph_instances", gl1eRead());
    answer("get_frame_plan", gl1ePlan({}));
    answer("get_render_authoring_context", gl1eAuthoring({}));
    auto *notice = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasNotice"));
    REQUIRE(notice != nullptr);

    // Decision 1: present is the root's own; it says it is edited here.
    const auto folded = widget.model().foldedNodes();
    const auto present = std::ranges::find(folded, std::string{"present"},
                                           &PelicanStudio::GraphCanvasFoldedNode::id);
    REQUIRE(present != folded.end());
    CHECK(present->rendering_section);
    CHECK(present->tooltip() == "From the rendering section: edit it here.");
    auto *fields = widget.findChild<QWidget *>(QStringLiteral("pelican.graphCanvasFoldedPass"));
    REQUIRE(fields != nullptr);
    CHECK_FALSE(fields->isVisible());
    widget.scene().selectNode("present");
    QCoreApplication::processEvents();
    REQUIRE(fields->isVisible());
    // Decision 5: the root's decisions are read for its boxes.
    answer("get_render_documents", root(false));
    auto *serial = widget.findChild<QCheckBox *>(QStringLiteral("pelican.graphCanvasFoldedPass.serial"));
    REQUIRE(serial != nullptr);
    CHECK_FALSE(serial->isChecked());

    // Decision 2: a fragment picked from the Pass Editor's list is one
    // edit_preset_pass of the root's pass -- the params presetPassEditParams
    // builds for the Pass Editor's Save.
    auto *fragment =
        widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasFoldedPass.fragment"));
    REQUIRE(fragment != nullptr);
    CHECK(fragment->currentData().toString() == QStringLiteral("engine://passthrough"));
    const auto pick = [&](const char *reference) {
        const int index = fragment->findData(QString::fromLatin1(reference));
        REQUIRE(index >= 0);
        fragment->setCurrentIndex(index);
        QCoreApplication::processEvents();
    };
    pick("engine://fullscreen");
    REQUIRE(transport.countOf(QStringLiteral("edit_preset_pass")) == 1);
    CHECK(sent("edit_preset_pass") ==
          Json{{"base_source_digest", "gl1e-root"},
               {"graph", "main_render"},
               {"pass_name", "present"},
               {"ray_query", false},
               {"input", Json::array()},
               {"output", {{"color", Json::array({"display"})}, {"depth", nullptr}}},
               {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://fullscreen"}}}});
    CHECK(notice->text() == QStringLiteral("present: saving..."));
    answer("edit_preset_pass", Json{{"status", "accepted"}, {"ticket", "gl5a"}});
    answer("get_edit_result", Json{{"status", "committed"},
                                   {"committed", true},
                                   {"source_digest", {{"hex", "gl5a-root-2"}}}});
    CHECK(notice->text() == QStringLiteral("present saved."));
    // RU-1: the tab's Undo takes the pass's edit back.
    QCoreApplication::processEvents();
    auto *undo = widget.findChild<QAction *>(QStringLiteral("pelican.graphCanvas.undo"));
    REQUIRE(undo != nullptr);
    CHECK(undo->isEnabled());
    undo->trigger();
    QCoreApplication::processEvents();
    REQUIRE(history.busy());
    read_history();
    const auto restore = last_history("restore_render_documents");
    const auto &restored = restore.params.at("documents");
    REQUIRE(restored.size() == 1);
    CHECK(restored[0].at("reference") == "fixture");
    CHECK(restored[0].at("base_digest") == history_digest);
    CHECK(restored[0].at("content") == original_render);
    const auto serial_before = history.undoStack().back().serial;
    history_digest = "gl5a-restored";
    history_document = Json::parse(original_render);
    REQUIRE(history.receiveResult(restore.id, {{"status", "committed"}, {"committed", true},
        {"source_digest", {{"hex", history_digest}}}, {"documents", {{"fixture", history_digest}}}}));
    REQUIRE(history.redoStack().size() == 1);
    CHECK(history.redoStack().back().serial == serial_before);

    // Decision 6: a refused edit reads as the Pass Editor's sentence for its
    // code, from the inspector model's table.
    pick("engine://scene_present");
    REQUIRE(transport.countOf(QStringLiteral("edit_preset_pass")) == 2);
    const std::string code = "graph.view_select_needs_target_plan@1";
    const std::string message = code + ": {\"pass\":\"present\"}";
    answer("edit_preset_pass",
           Json{{"status", "rejected"}, {"error", {{"code", code}, {"message", message}}}});
    const auto sentence = PelicanStudio::knownRefusalDescription(code, message);
    REQUIRE_FALSE(sentence.empty());
    CHECK(notice->toolTip().startsWith(QString::fromStdString(sentence)));

    // Decision 5: Serial is the Frame Plan's edit_target_planning of the pass,
    // and the box says what the root holds once it is read again. The reads
    // the edits above asked for are answered first (each edit reads the
    // root again).
    for (int answered = transport.countOf(QStringLiteral("get_render_documents")), round = 0;
         round < 4; ++round) {
        answer("get_render_documents", root(false));
        const int now = transport.countOf(QStringLiteral("get_render_documents"));
        if (now == answered) break;
        answered = now;
    }
    CHECK_FALSE(serial->isChecked());
    serial->click();
    QCoreApplication::processEvents();
    REQUIRE(transport.countOf(QStringLiteral("edit_target_planning")) == 1);
    CHECK(sent("edit_target_planning") ==
          PelicanStudio::graphTargetPlanningParams("gl1e-root", "main_render", "node", "present",
                                                   "serial", true));
    const int reads = transport.countOf(QStringLiteral("get_render_documents"));
    answer("edit_target_planning", Json{{"status", "accepted"}, {"ticket", "gl5a-serial"}});
    answer("get_edit_result", Json{{"status", "committed"},
                                   {"committed", true},
                                   {"source_digest", {{"hex", "gl5a-root-3"}}}});
    CHECK(notice->text() == QStringLiteral("Serial set on present."));
    REQUIRE(transport.countOf(QStringLiteral("get_render_documents")) == reads + 1);
    answer("get_render_documents", root(true));
    CHECK(serial->isChecked());
}

// --- GL-5b: New render target... and a pass's output, from the Graph.

namespace {

// Blank's context with the engine's display, a target the project declared,
// any `extra` names, and the vocabulary the form offers.
Json gl5bAuthoring(const std::vector<std::string> &extra = {},
                   const std::vector<std::string> &inputs = {}) {
    auto context = gl1eAuthoring(inputs);
    auto targets = Json::array(
        {Json{{"name", "display"},
              {"declaration", {{"name", "display"}, {"format_class", "display"}}},
              {"editable", false},
              {"source", "engine"}},
         Json{{"name", "other_color"},
              {"declaration", {{"name", "other_color"}, {"format", "R16G16B16A16_SFLOAT"},
                               {"format_class", "scene"}}},
              {"editable", true},
              {"source", "authoring"}}});
    for (const auto &name : extra)
        targets.push_back({{"name", name},
                           {"declaration", {{"name", name}, {"format", "R16G16B16A16_SFLOAT"},
                                            {"format_class", "scene"},
                                            {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}},
                           {"editable", true},
                           {"source", "authoring"}});
    context["render_targets"] = std::move(targets);
    context["render_target_vocabulary"] = {
        {"formats", Json::array({"R8G8B8A8_UNORM", "R16G16B16A16_SFLOAT"})},
        {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED", "STORAGE"})}};
    return context;
}

// The Graph tab on Blank's root, with what a New render target… sends
// recorded: the reads it makes are answered from `authoring` as they come.
struct Gl5bTab {
    RecordedCapability transport;
    std::vector<std::string> extra;
    // GL-5d: present's `input`, the declarations the plan publishes for the
    // graph's pass `shade` (none when null) and what it reads, and nodes the
    // root holds beside gl1eRead's.
    std::vector<std::string> inputs;
    Json shade_inputs;
    Json shade_reads = Json::array();
    Json nodes = Json::array();
    // GL-5e: what the plan and the context say beside Blank's.
    std::function<void(Json &)> plan_edit;
    std::function<void(Json &)> authoring_edit;
    std::set<qint64> answered;
    std::unique_ptr<PelicanStudio::GraphCanvasWidget> widget;

    Json document() const {
        auto read = gl1eRead();
        for (const auto &node : nodes) read["nodes"].push_back(node);
        return read;
    }

    Json plan() const {
        auto plan = gl1ePlan(inputs);
        if (!shade_inputs.is_null()) {
            plan["nodes"][0]["pass_inputs"] = shade_inputs;
            plan["nodes"][0]["reads"] = shade_reads;
        }
        if (plan_edit) plan_edit(plan);
        return plan;
    }

    qint64 record(const char *method, const QJsonObject &params) {
        RecordedCapability::Call call{QString::fromLatin1(method)};
        call.body = QJsonDocument(params).toJson(QJsonDocument::Compact);
        transport.calls.push_back(call);
        return transport.next++;
    }

    // GL-5c: `operations` the catalog offers beside the visual one's.
    // GL-5d: `configure` sets the members above before anything is read.
    explicit Gl5bTab(const std::vector<std::string> &operations = {},
                     const std::function<void(Gl5bTab &)> &configure = {}) {
        if (configure) configure(*this);
        auto capability = transport.capability();
        capability.requestFramePlan = [this](QString *) {
            transport.calls.push_back({QStringLiteral("get_frame_plan")});
            return transport.next++;
        };
        capability.requestRenderAuthoringContext = [this](QString *) {
            transport.calls.push_back({QStringLiteral("get_render_authoring_context")});
            return transport.next++;
        };
        capability.requestEditPresetPass = [this](const QJsonObject &params, QString *) {
            return record("edit_preset_pass", params);
        };
        capability.requestEditRenderTargets = [this](const QJsonObject &params, QString *) {
            return record("edit_render_targets", params);
        };
        widget = std::make_unique<PelicanStudio::GraphCanvasWidget>(capability);
        widget->resize(1100, 700);
        widget->show();
        QCoreApplication::processEvents();
        auto catalog = visualCatalog();
        catalog["modules"][1]["operations"].push_back(
            {{"op", "render.target_ref"}, {"domain", "rendering"}, {"evaluation", "direct"},
             {"sink", false}, {"ports", "parameter_dependent"}});
        for (const auto &op : operations)
            catalog["modules"][1]["operations"].push_back(
                {{"op", op}, {"domain", "rendering"}, {"evaluation", "direct"},
                 {"sink", false}, {"ports", "parameter_dependent"}});
        answer("get_graph_catalog", catalog);
        reads();
    }

    void answer(const char *method, const Json &reply) {
        const qint64 id = transport.idOf(QString::fromLatin1(method));
        REQUIRE(id != 0);
        answered.insert(id);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    }

    // Every read not answered yet: the document, its plan and the context.
    void reads() {
        for (int round = 0; round < 6; ++round) {
            bool any = false;
            for (std::size_t index = 0; index < transport.calls.size(); ++index) {
                const auto id = static_cast<qint64>(index) + 1;
                const auto &method = transport.calls[index].method;
                if (answered.contains(id)) continue;
                Json reply;
                if (method == QLatin1String{"get_graph_instances"}) reply = document();
                else if (method == QLatin1String{"get_frame_plan"}) reply = plan();
                else if (method == QLatin1String{"get_render_authoring_context"}) {
                    reply = gl5bAuthoring(extra, inputs);
                    if (authoring_edit) authoring_edit(reply);
                } else continue;
                answered.insert(id);
                transport.deliver(id, QByteArray::fromStdString(reply.dump()));
                any = true;
            }
            QCoreApplication::processEvents();
            if (!any) break;
        }
    }

    Json sent(const char *method) {
        const auto id = transport.idOf(QString::fromLatin1(method));
        REQUIRE(id != 0);
        return Json::parse(transport.calls[static_cast<std::size_t>(id - 1)].body.toStdString());
    }

    // The render target form on screen, its name field, OK.
    QWidget *form() {
        return widget->findChild<QWidget *>(QStringLiteral("pelican.renderTargetForm"));
    }

    // OK in New render target, then the engine takes it: the commit, and the
    // context that names the target read again.
    void declare(const std::string &name) {
        auto *ok = form()->findChild<QPushButton *>(QStringLiteral("pelican.renderTargetForm.ok"));
        REQUIRE(ok != nullptr);
        const int before = transport.countOf(QStringLiteral("edit_render_targets"));
        ok->click();
        QCoreApplication::processEvents();
        REQUIRE(transport.countOf(QStringLiteral("edit_render_targets")) == before + 1);
        // The Frame Plan's edit, from the same context.
        PelicanStudio::RenderTargetFormValues values;
        values.name = name;
        CHECK(sent("edit_render_targets") ==
              PelicanStudio::framePlanRenderTargetAddRequest(
                  PelicanStudio::framePlanAuthoringFromContext(gl5bAuthoring(extra, inputs)),
                  values)
                  .params);
        answer("edit_render_targets", Json{{"status", "accepted"}, {"ticket", "gl5b-target"}});
        answer("get_edit_result", Json{{"status", "committed"},
                                       {"committed", true},
                                       {"source_digest", {{"hex", "gl1e-root"}}}});
        extra.push_back(name);
        reads();
    }
};

} // namespace

TEST_CASE("GL-5b the target_ref palette's New render target... opens the Frame Plan's form on a "
          "proposed name, sends its one edit_render_targets, and places the reference once the "
          "target is declared",
          "[devstudio][graph-canvas][scene][gl5b]") {
    (void)application();
    Gl5bTab tab;
    auto &widget = *tab.widget;
    auto *add = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasAddNode"));
    REQUIRE(add != nullptr);
    REQUIRE(add->isEnabled());
    QStringList offered;
    QString note;
    REQUIRE(whilePaletteOpen(*add, [&](QDialog &dialog) {
        auto *filter = dialog.findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasPaletteFilter"));
        REQUIRE(filter != nullptr);
        filter->setText(QStringLiteral("target_ref"));
        QCoreApplication::processEvents();
        auto *targets = dialog.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasPaletteTarget"));
        REQUIRE(targets != nullptr);
        for (int index = 0; index < targets->count(); ++index) offered << targets->itemText(index);
        targets->setCurrentIndex(targets->count() - 1);
        QCoreApplication::processEvents();
        auto *shown = dialog.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasPaletteNote"));
        REQUIRE(shown != nullptr);
        note = shown->text();
        dialog.accept();
    }));
    // Decision 2 (a): the declared targets, and New render target… last --
    // in place of "declare one in the Frame Plan".
    CHECK(offered == QStringList{QStringLiteral("display"), QStringLiteral("other_color"),
                                 QString::fromUtf8("New render target\xE2\x80\xA6")});
    CHECK(note.contains(QStringLiteral("OK opens New render target")));
    QCoreApplication::processEvents();
    // The Frame Plan's form (presentNewRenderTarget), on the name decision 5
    // proposes: nothing to type.
    REQUIRE(tab.form() != nullptr);
    CHECK(tab.form()->isVisible());
    auto *name = tab.form()->findChild<QLineEdit *>(QStringLiteral("pelican.renderTargetForm.name"));
    REQUIRE(name != nullptr);
    CHECK(name->text() == QStringLiteral("render_target"));
    CHECK(tab.transport.countOf(QStringLiteral("edit_graph_document")) == 0);
    tab.declare("render_target");
    // Once the context names it, the reference is placed as a picked one is.
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_graph_document")) == 1);
    const auto ops = tab.sent("edit_graph_document");
    CHECK(std::ranges::any_of(ops, [](const Json &op) {
        return op.value("op", std::string{}) == "add_node" &&
               op.at("node").value("op", std::string{}) == "render.target_ref" &&
               op.at("node").at("parameters").value("name", std::string{}) == "render_target";
    }));
}

TEST_CASE("GL-5b a rendering-section pass's output is picked again from the declared targets, or "
          "New target... declares one first: each is one edit_preset_pass",
          "[devstudio][graph-canvas][scene][gl5b]") {
    (void)application();
    Gl5bTab tab;
    auto &widget = *tab.widget;
    auto *notice = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasNotice"));
    REQUIRE(notice != nullptr);
    widget.scene().selectNode("present");
    QCoreApplication::processEvents();
    const auto output = [&] {
        return widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasFoldedPass.output.0"));
    };
    REQUIRE(output() != nullptr);
    REQUIRE(output()->isVisible());
    QStringList offered;
    for (int index = 0; index < output()->count(); ++index) offered << output()->itemText(index);
    CHECK(offered == QStringList{QStringLiteral("display"), QStringLiteral("other_color"),
                                 QString::fromUtf8("New target: present_color\xE2\x80\xA6")});
    CHECK(output()->currentData().toString() == QStringLiteral("display"));
    // The params the Pass Editor's Save builds for present with that output.
    const auto saved = [](const char *target) {
        return Json{{"base_source_digest", "gl1e-root"},
                    {"graph", "main_render"},
                    {"pass_name", "present"},
                    {"ray_query", false},
                    {"input", Json::array()},
                    {"output", {{"color", Json::array({target})}, {"depth", nullptr}}},
                    {"shader", {{"vertex", "engine://fullscreen"},
                                {"fragment", "engine://passthrough"}}}};
    };

    // Decision 3: a declared target picked is one edit_preset_pass.
    output()->setCurrentIndex(output()->findData(QStringLiteral("other_color")));
    QCoreApplication::processEvents();
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_preset_pass")) == 1);
    CHECK(tab.sent("edit_preset_pass") == saved("other_color"));
    // Refused: the column shows what the root holds again.
    tab.answer("edit_preset_pass",
               Json{{"status", "rejected"},
                    {"error", {{"code", "render_pipeline_preflight_failed"},
                               {"message", "display is not written"}}}});
    QCoreApplication::processEvents();
    REQUIRE(output() != nullptr);
    CHECK(output()->currentData().toString() == QStringLiteral("display"));

    // Decision 2 (b): New target… opens the Frame Plan's form on the Pass
    // Editor's name; once the target is declared the output writes it.
    output()->setCurrentIndex(output()->count() - 1);
    QCoreApplication::processEvents();
    CHECK(tab.transport.countOf(QStringLiteral("edit_preset_pass")) == 1);
    REQUIRE(tab.form() != nullptr);
    CHECK(tab.form()->isVisible());
    auto *name = tab.form()->findChild<QLineEdit *>(QStringLiteral("pelican.renderTargetForm.name"));
    REQUIRE(name != nullptr);
    CHECK(name->text() == QStringLiteral("present_color"));
    tab.declare("present_color");
    CHECK(notice->text() == QStringLiteral("present: saving..."));
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_preset_pass")) == 2);
    CHECK(tab.sent("edit_preset_pass") == saved("present_color"));
}

// --- GL-5c: the Frame Plan's Add and Edit, on the Graph.

TEST_CASE("GL-5c decisions 1, 3 and 4: the Frame Plan's Add opens the palette in the root on "
          "its op, what it places goes into the graph's nodes and is picked once the document "
          "holds it; New render target beside that forward pass opens on the kind it writes",
          "[devstudio][graph-canvas][scene][gl5c]") {
    (void)application();
    Gl5bTab tab{{"render.material_pass", "draw.material"}};
    auto &widget = *tab.widget;
    // Decision 3: the graph node's pass and the rendering section's are
    // edited here; a pass the plan does not hold is not.
    CHECK(widget.editsPlanPass(QStringLiteral("shade")));
    CHECK(widget.editsPlanPass(QStringLiteral("present")));
    CHECK_FALSE(widget.editsPlanPass(QStringLiteral("no_such_pass")));

    // Decision 1: the palette, on the op the Frame Plan's entry names.
    QString filter;
    QString row;
    bool contract_offered = false;
    REQUIRE(whilePaletteOpenAfter(
        [&] { widget.placeFromPalette(QStringLiteral("render.material_pass")); },
        [&](QDialog &dialog) {
            auto *search = dialog.findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasPaletteFilter"));
            auto *list = dialog.findChild<QListWidget *>(QStringLiteral("pelican.graphCanvasPaletteList"));
            auto *contracts =
                dialog.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasPaletteContract"));
            REQUIRE(search != nullptr);
            REQUIRE(list != nullptr);
            REQUIRE(contracts != nullptr);
            filter = search->text();
            REQUIRE(list->currentItem() != nullptr);
            row = list->currentItem()->data(Qt::UserRole).toString();
            contract_offered = contracts->isVisible();
            contracts->setCurrentIndex(contracts->findData(QStringLiteral("forward_opaque_v1")));
            dialog.accept();
        }));
    CHECK(filter == QStringLiteral("render.material_pass"));
    CHECK(row == QStringLiteral("render.material_pass"));
    CHECK(contract_offered);
    QCoreApplication::processEvents();
    // The palette's edit: the pass and its draw loop in the root's nodes.
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_graph_document")) == 1);
    const auto ops = tab.sent("edit_graph_document");
    CHECK(std::ranges::any_of(ops, [](const Json &op) {
        return op.value("op", std::string{}) == "add_node" && op.value("scope", std::string{}) == "root" &&
               op.at("node").value("id", std::string{}) == "render_material_pass" &&
               op.at("node").value("op", std::string{}) == "render.material_pass";
    }));
    CHECK(tab.transport.countOf(QStringLiteral("edit_preset_pass")) == 0);
    CHECK(tab.transport.countOf(QStringLiteral("edit_render_targets")) == 0);
    // Not picked before the document holds it.
    const auto picked = [&] {
        const auto &selected = widget.scene().selectedNode();
        return selected ? *selected : std::string{};
    };
    CHECK(picked() != "render_material_pass");
    tab.answer("edit_graph_document", Json{{"status", "accepted"}, {"ticket", "gl5c"}});
    tab.answer("get_edit_result", Json{{"status", "committed"},
                                       {"committed", true},
                                       {"source_digest", {{"hex", "gl1e-root"}}}});
    auto document = gl1eRead();
    for (const auto &op : ops) {
        if (op.value("op", std::string{}) == "add_node" && op.value("scope", std::string{}) == "root")
            document["nodes"].push_back(op.at("node"));
        if (op.value("op", std::string{}) == "add_definition")
            document["definitions"][op.at("name").get<std::string>()] = op.at("definition");
    }
    tab.answer("get_graph_instances", document);
    tab.reads();
    // Picked, its detail column open on the pass's fields.
    CHECK(picked() == "render_material_pass");
    auto *contract = widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasDraw.contract"));
    REQUIRE(contract != nullptr);
    CHECK(contract->isVisible());

    // Decision 4: a reference placed beside it, New render target…: the form
    // opens on what the forward pass's colour output takes.
    auto *add = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasAddNode"));
    REQUIRE(add != nullptr);
    REQUIRE(add->isEnabled());
    REQUIRE(whilePaletteOpen(*add, [&](QDialog &dialog) {
        auto *search = dialog.findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasPaletteFilter"));
        REQUIRE(search != nullptr);
        search->setText(QStringLiteral("target_ref"));
        QCoreApplication::processEvents();
        auto *targets = dialog.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasPaletteTarget"));
        REQUIRE(targets != nullptr);
        targets->setCurrentIndex(targets->count() - 1);
        QCoreApplication::processEvents();
        dialog.accept();
    }));
    QCoreApplication::processEvents();
    REQUIRE(tab.form() != nullptr);
    CHECK(tab.form()->isVisible());
    auto *kind = tab.form()->findChild<QComboBox *>(QStringLiteral("pelican.renderTargetForm.kind"));
    auto *format = tab.form()->findChild<QComboBox *>(QStringLiteral("pelican.renderTargetForm.format"));
    REQUIRE(kind != nullptr);
    REQUIRE(format != nullptr);
    CHECK(kind->currentData().toString() == QStringLiteral("explicit"));
    CHECK(format->currentText() == QStringLiteral("R16G16B16A16_SFLOAT"));
    tab.form()->close();
}

// --- GL-5d: what only the Frame Plan offered, on the Graph.

namespace {

// The objectNames of the canvas menu a right-click over `item` opens (it is
// closed again at once).
QStringList canvasMenuOver(QGraphicsScene &scene, QGraphicsItem &item) {
    QStringList names;
    bool seen = false;
    QTimer reader;
    reader.setInterval(0);
    QObject::connect(&reader, &QTimer::timeout, &scene, [&] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *menu = qobject_cast<QMenu *>(widget);
            if (menu == nullptr || !menu->isVisible()) continue;
            for (QAction *action : menu->actions())
                if (!action->objectName().isEmpty()) names << action->objectName();
            seen = true;
            menu->close();
        }
    });
    reader.start();
    QGraphicsSceneContextMenuEvent event{QEvent::GraphicsSceneContextMenu};
    event.setScenePos(item.sceneBoundingRect().center());
    event.setScreenPos(QPoint{100, 100});
    QApplication::sendEvent(&scene, &event);
    reader.stop();
    QCoreApplication::processEvents();
    REQUIRE(seen);
    return names;
}

} // namespace

TEST_CASE("GL-5d Order..., Fix sockets and Make sockets variadic are the Frame Plan's on the picked "
          "pass's right-click and detail column; Edit target... and Delete target send the Frame "
          "Plan's edit_render_targets",
          "[devstudio][graph-canvas][scene][gl5d]") {
    (void)application();
    const auto root = std::filesystem::temp_directory_path() / "pelican_gl5d_scene";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "shaders");
    // shade draws with a project shader whose sockets Fix sockets wrote.
    {
        const auto fixed = PelicanStudio::fixShaderSockets(
            *PelicanStudio::engineFragmentSource(PelicanStudio::EnginePassthroughFragment), 0,
            {"old_image"});
        REQUIRE(fixed.ok());
        std::ofstream{root / "shaders" / "shade.frag", std::ios::binary} << fixed.source;
    }
    const auto reference = [](const char *id, const char *target) {
        return Json{{"id", id},
                    {"op", "render.target_ref"},
                    {"parameters", {{"name", target}}},
                    {"outputs", Json::array({"resource"})}};
    };
    Gl5bTab tab{{}, [&](Gl5bTab &self) {
                    // present reads shade's image and other_color; spare is
                    // declared and nothing reads it.
                    self.inputs = {"shade__color", "other_color"};
                    self.extra = {"spare"};
                    self.shade_inputs = gl1ePassthrough();
                    self.shade_inputs["fragment"] = "project://shaders/shade";
                    self.shade_reads = Json::array({"old_image"});
                    self.nodes = Json::array(
                        {reference("other_ref", "other_color"), reference("spare_ref", "spare")});
                }};
    auto &widget = *tab.widget;
    widget.setProjectRoot(root);
    int ordered = 0;
    std::vector<std::pair<QString, QString>> fixed;
    std::vector<std::pair<QString, QString>> variadic;
    std::function<bool(const QString &)> fragment;
    widget.setFramePlanPassActions(
        [&] { ++ordered; },
        [&](const QString &graph, const QString &pass,
            std::function<bool(const QString &)> graph_fragment) {
            fixed.emplace_back(graph, pass);
            fragment = std::move(graph_fragment);
        },
        [&](const QString &graph, const QString &pass) { variadic.emplace_back(graph, pass); });
    auto &scene = widget.scene();
    const auto action = [&](const char *name) {
        auto *found = widget.findChild<QAction *>(QString::fromLatin1(name));
        REQUIRE(found != nullptr);
        return found;
    };
    const auto button = [&](const char *name) {
        auto *found = widget.findChild<QPushButton *>(
            QStringLiteral("pelican.graphCanvasNodeActions.") + QString::fromLatin1(name));
        REQUIRE(found != nullptr);
        return found;
    };
    const auto pick = [&](const char *node) {
        scene.selectNode(node);
        QCoreApplication::processEvents();
    };
    const auto fire = [&](const char *name) {
        REQUIRE(action(name)->isEnabled());
        action(name)->trigger();
        // Queued: a dialog may open, and a menu may still be dispatching.
        QCoreApplication::processEvents();
    };
    auto *notice = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasNotice"));
    REQUIRE(notice != nullptr);

    // Decision 1: Order… on the rendering section's pass, not on a graph node.
    pick("present");
    CHECK(button("orderPasses")->isVisible());
    fire("pelican.graphCanvas.orderPasses");
    CHECK(ordered == 1);
    // Decision 2: and Fix sockets on it, the Frame Plan's on the plan's pass.
    CHECK(button("fixSockets")->isVisible());
    fire("pelican.graphCanvas.fixSockets");
    REQUIRE(fixed.size() == 1);
    CHECK(fixed[0] == std::pair{QStringLiteral("main_render"), QStringLiteral("present")});
    CHECK_FALSE(fragment);
    {
        auto *item = nodeItem(scene, QStringLiteral("present"));
        REQUIRE(item != nullptr);
        const auto names = canvasMenuOver(scene, *item);
        CHECK(names.contains(QStringLiteral("pelican.graphCanvas.orderPasses")));
        CHECK(names.contains(QStringLiteral("pelican.graphCanvas.fixSockets")));
        CHECK_FALSE(names.contains(QStringLiteral("pelican.graphCanvas.expandToEdit")));
    }
    // A line into present offers Fix sockets beside its Delete.
    {
        QGraphicsItem *line = nullptr;
        for (QGraphicsItem *item : scene.items())
            if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasPassInputEdgeItem})
                line = item;
        REQUIRE(line != nullptr);
        const auto names = canvasMenuOver(scene, *line);
        CHECK(names.contains(QStringLiteral("pelican.graphCanvas.fixSockets")));
        CHECK(names.contains(QStringLiteral("pelican.graphCanvas.deletePassInput")));
        CHECK_FALSE(names.contains(QStringLiteral("pelican.graphCanvas.orderPasses")));
    }

    // The graph's own pass: no Order…; Fix sockets moves the node onto the
    // copy with one set_node_parameters; its fixed shader can be variadic again.
    pick("shade");
    CHECK_FALSE(button("orderPasses")->isVisible());
    CHECK_FALSE(action("pelican.graphCanvas.orderPasses")->isEnabled());
    fire("pelican.graphCanvas.fixSockets");
    REQUIRE(fixed.size() == 2);
    CHECK(fixed[1] == std::pair{QStringLiteral("main_render"), QStringLiteral("shade")});
    REQUIRE(fragment);
    const int edits = tab.transport.countOf(QStringLiteral("edit_graph_document"));
    CHECK(fragment(QStringLiteral("project://shaders/shade_2")));
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_graph_document")) == edits + 1);
    const auto moved = tab.sent("edit_graph_document");
    REQUIRE(moved.size() == 1);
    CHECK(moved[0].value("op", std::string{}) == "set_node_parameters");
    CHECK(moved[0].value("node", std::string{}) == "shade");
    CHECK(moved[0].at("parameters").at("shader").at("fragment") == "project://shaders/shade_2");
    tab.answer("edit_graph_document", Json{{"status", "accepted"}, {"ticket", "gl5d-shade"}});
    tab.answer("get_edit_result", Json{{"status", "committed"},
                                       {"committed", true},
                                       {"source_digest", {{"hex", "gl1e-root"}}}});
    tab.reads();
    pick("shade");
    CHECK(button("makeSocketsVariadic")->isVisible());
    fire("pelican.graphCanvas.makeSocketsVariadic");
    REQUIRE(variadic.size() == 1);
    CHECK(variadic[0] == std::pair{QStringLiteral("main_render"), QStringLiteral("shade")});

    // Decision 3: Edit target… opens the Frame Plan's form in edit mode, and
    // OK sends its replace request.
    pick("spare_ref");
    CHECK(button("editTarget")->isVisible());
    CHECK(button("deleteTarget")->isVisible());
    CHECK_FALSE(button("fixSockets")->isVisible());
    {
        auto *item = nodeItem(scene, QStringLiteral("spare_ref"));
        REQUIRE(item != nullptr);
        const auto names = canvasMenuOver(scene, *item);
        CHECK(names.contains(QStringLiteral("pelican.graphCanvas.editTarget")));
        CHECK(names.contains(QStringLiteral("pelican.graphCanvas.deleteTarget")));
    }
    fire("pelican.graphCanvas.editTarget");
    REQUIRE(tab.form() != nullptr);
    CHECK(tab.form()->isVisible());
    auto *name = tab.form()->findChild<QLineEdit *>(QStringLiteral("pelican.renderTargetForm.name"));
    REQUIRE(name != nullptr);
    CHECK(name->text() == QStringLiteral("spare"));
    auto *ok = tab.form()->findChild<QPushButton *>(QStringLiteral("pelican.renderTargetForm.ok"));
    REQUIRE(ok != nullptr);
    ok->click();
    QCoreApplication::processEvents();
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_render_targets")) == 1);
    const auto authoring =
        PelicanStudio::framePlanAuthoringFromContext(gl5bAuthoring(tab.extra, tab.inputs));
    PelicanStudio::RenderTargetFormValues values;
    QString unread;
    REQUIRE(static_cast<PelicanStudio::RenderTargetForm *>(tab.form())->readValues(values, unread));
    CHECK(tab.sent("edit_render_targets") ==
          PelicanStudio::framePlanRenderTargetReplaceRequest(authoring, "spare", values).params);
    tab.answer("edit_render_targets", Json{{"status", "accepted"}, {"ticket", "gl5d-edit"}});
    tab.answer("get_edit_result", Json{{"status", "committed"},
                                       {"committed", true},
                                       {"source_digest", {{"hex", "gl1e-root"}}}});
    CHECK(notice->text() == QStringLiteral("Render target spare saved."));
    tab.reads();

    // Delete target on a target present reads: the Frame Plan's remove
    // request goes as it is, and the engine's refusal is the line.
    pick("other_ref");
    fire("pelican.graphCanvas.deleteTarget");
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_render_targets")) == 2);
    CHECK(tab.sent("edit_render_targets") ==
          PelicanStudio::framePlanRenderTargetRemoveRequest(authoring, "other_color").params);
    CHECK(tab.transport.countOf(QStringLiteral("edit_graph_document")) == edits + 1);
    tab.answer("edit_render_targets",
               Json{{"status", "rejected"},
                    {"error", {{"code", "render_target_referenced"},
                               {"message", "other_color is still used by present"},
                               {"name", "other_color"},
                               {"users", Json::array({"present"})}}}});
    CHECK(notice->toolTip().startsWith(QStringLiteral("other_color is still used by present")));

    // Delete target on one nothing uses: its reference goes first, then,
    // once that committed, the declaration.
    pick("spare_ref");
    fire("pelican.graphCanvas.deleteTarget");
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_graph_document")) == edits + 2);
    const auto removed = tab.sent("edit_graph_document");
    REQUIRE(removed.size() == 1);
    CHECK(removed[0].value("op", std::string{}) == "remove_node");
    CHECK(removed[0].value("node", std::string{}) == "spare_ref");
    CHECK(tab.transport.countOf(QStringLiteral("edit_render_targets")) == 2);
    tab.answer("edit_graph_document", Json{{"status", "accepted"}, {"ticket", "gl5d-ref"}});
    tab.answer("get_edit_result", Json{{"status", "committed"},
                                       {"committed", true},
                                       {"source_digest", {{"hex", "gl1e-root"}}}});
    tab.nodes = Json::array({reference("other_ref", "other_color")});
    tab.reads();
    widget.poll();
    QCoreApplication::processEvents();
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_render_targets")) == 3);
    CHECK(tab.sent("edit_render_targets") ==
          PelicanStudio::framePlanRenderTargetRemoveRequest(authoring, "spare").params);
    std::filesystem::remove_all(root);
}

// --- UX-1: how the Graph feels in the hand.

namespace {

// A mouse gesture through a view, the way a person makes it: the press, a
// few moves, the release, each one turned into scene coordinates by the view
// as it stands at that moment (a scene rect that changes under the drag
// moves what the next move lands on).
void viewMouse(QGraphicsView &view, QEvent::Type type, const QPoint &at,
               Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event{type, QPointF(at), view.viewport()->mapToGlobal(QPointF(at)), button,
                      buttons, Qt::NoModifier};
    QApplication::sendEvent(view.viewport(), &event);
    QCoreApplication::processEvents();
}

void viewDrag(QGraphicsView &view, const QPoint &from, const QPoint &to, int steps = 6) {
    viewMouse(view, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int step = 1; step <= steps; ++step)
        viewMouse(view, QEvent::MouseMove, from + (to - from) * step / steps, Qt::NoButton,
                  Qt::LeftButton);
    viewMouse(view, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

// Where on the view a scene point is drawn.
QPoint onView(const QGraphicsView &view, const QPointF &scene_point) {
    return view.mapFromScene(scene_point);
}

// A view over a harness's scene, set up as the Graph tab's is and fitted,
// the way the tab first shows a scope.
struct UxView {
    PelicanStudio::NodeCanvasView view;
    explicit UxView(QGraphicsScene &scene) : view{&scene} {
        PelicanStudio::configureGraphCanvasView(view);
        view.resize(1200, 720);
        view.show();
        QCoreApplication::processEvents();
        view.fitView();
        QCoreApplication::processEvents();
    }
};

// An else-only draw_loop over a material pass's draws, as the palette's
// material pass makes it: `<pass>_draws` running `<pass>_draw`, whose body is
// one `step`.
Json ux1DrawDocument() {
    return Json{
        {"schema", "pelican.graph"}, {"version", 1}, {"outputs", Json::object()},
        {"definitions",
         {{"forward_draw",
           {{"nodes", Json::array({{{"id", "step"}, {"op", "draw.material"},
                                    {"parameters", Json::object()},
                                    {"outputs", Json::array({"draws"})}}})},
            {"outputs", {{"draws", {{"node", "step"}, {"port", "draws"}}}}}}}}},
        {"nodes",
         Json::array({{{"id", "forward_draws"}, {"op", "graph.loop"},
                       {"parameters", {{"over", {{"draw_items", {{"contract", "forward_opaque_v1"}}}}},
                                       {"tier", "draw_loop"},
                                       {"branches", Json::array({{{"body", "forward_draw"}}})}}},
                       {"outputs", Json::array({"draws"})}},
                      {{"id", "forward"}, {"op", "render.material_pass"},
                       {"parameters", {{"material_contract", "forward_opaque_v1"},
                                       {"pass_set", "main_render"}}},
                       {"inputs", {{"draws", {{"node", "forward_draws"}, {"port", "draws"}}}}},
                       {"outputs", Json::array()}}})}};
}

void addDrawOperations(ZoneHarness &h) {
    for (const char *op : {"render.material_pass", "draw.material"})
        h.catalog["modules"][1]["operations"].push_back(
            {{"op", op}, {"domain", "rendering"}, {"evaluation", "direct"}, {"sink", false},
             {"ports", "parameter_dependent"}});
}

} // namespace

// Decision 2. The zone was pushed back: dragged by its heading, it did not
// end where the hand let go. Through a view, as the person does it.
TEST_CASE("UX-1 a zone dragged by its heading ends where the hand let go, and stays there",
          "[devstudio][graph-canvas][scene][ux1]") {
    (void)application();
    ZoneHarness h;
    h.open();
    UxView shown{h.scene};
    auto &view = shown.view;
    QGraphicsItem *frame = scopedNode(h.scene, QString{}, QStringLiteral("steps"));
    REQUIRE(frame != nullptr);
    const QPointF spot = frame->data(PelicanStudio::NodeGraphHotspotRole).toPointF();
    const QPoint grip = onView(view, frame->mapToScene(spot));
    const QPoint delta{-260, 170};
    viewDrag(view, grip, grip + delta);
    // Under the hand when it lets go.
    frame = scopedNode(h.scene, QString{}, QStringLiteral("steps"));
    REQUIRE(frame != nullptr);
    QPoint at = onView(view, frame->mapToScene(spot));
    CHECK(std::abs(at.x() - (grip + delta).x()) <= 2);
    CHECK(std::abs(at.y() - (grip + delta).y()) <= 2);

    // Sent, committed, read again and drawn again: still there.
    h.scene.flushLayout();
    const auto ops = h.commit();
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].at("op") == "set_layout");
    CHECK(ops[0].at("scope") == "root");
    frame = scopedNode(h.scene, QString{}, QStringLiteral("steps"));
    REQUIRE(frame != nullptr);
    CHECK(ops[0].at("layout").at("nodes").at("steps") ==
          Json{{"x", frame->pos().x()}, {"y", frame->pos().y()}});
    at = onView(view, frame->mapToScene(spot));
    CHECK(std::abs(at.x() - (grip + delta).x()) <= 2);
    CHECK(std::abs(at.y() - (grip + delta).y()) <= 2);
}

// Decision 3. A body node moved without moving its zone: the node goes where
// it is put, the frame only grows round it, and nothing is written to the
// root -- in a loop's zone and in a material pass's draw_loop.
TEST_CASE("UX-1 a body node is dragged inside its zone, and the zone stays where it is",
          "[devstudio][graph-canvas][scene][ux1]") {
    (void)application();
    struct Case {
        const char *name;
        Json document;
        bool draws;
        QString loop;
        QString body;
        QString node;
    };
    const std::vector<Case> cases{
        {"a loop's zone", zoneDocument(), false, QStringLiteral("steps"), QStringLiteral("first"),
         QStringLiteral("sum")},
        {"a material pass's draws", ux1DrawDocument(), true, QStringLiteral("forward_draws"),
         QStringLiteral("forward_draw"), QStringLiteral("step")},
    };
    for (const auto &each : cases) {
        INFO(each.name);
        ZoneHarness h{each.document};
        if (each.draws) addDrawOperations(h);
        h.open();
        REQUIRE(h.scene.zones().contains(each.loop.toStdString()));
        UxView shown{h.scene};
        auto &view = shown.view;
        QGraphicsItem *frame = scopedNode(h.scene, QString{}, each.loop);
        QGraphicsItem *node = scopedNode(h.scene, each.body, each.node);
        REQUIRE(frame != nullptr);
        REQUIRE(node != nullptr);
        const QPointF frame_at = frame->pos();
        const QPoint frame_on_view = onView(view, frame->scenePos());
        const QPointF heading{node->boundingRect().width() / 2.0, 6.0};
        const QPoint grip = onView(view, node->mapToScene(heading));
        // Up and to the left, past the inside's origin: the frame has to
        // grow that way.
        const QPoint delta{-90, -70};
        viewDrag(view, grip, grip + delta);
        // The frame is drawn again round the body once the hand lets go.
        frame = scopedNode(h.scene, QString{}, each.loop);
        node = scopedNode(h.scene, each.body, each.node);
        REQUIRE(frame != nullptr);
        REQUIRE(node != nullptr);
        CHECK(frame->pos() == frame_at);
        QPoint at = onView(view, node->mapToScene(heading));
        CHECK(std::abs(at.x() - (grip + delta).x()) <= 2);
        CHECK(std::abs(at.y() - (grip + delta).y()) <= 2);

        h.scene.flushLayout();
        const auto ops = h.commit();
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].at("op") == "set_layout");
        CHECK(ops[0].at("scope") == each.body.toStdString());
        // Nothing else goes out: the root's placement is not written.
        h.scene.flushLayout();
        CHECK(h.answerReads().empty());
        frame = scopedNode(h.scene, QString{}, each.loop);
        node = scopedNode(h.scene, each.body, each.node);
        REQUIRE(frame != nullptr);
        REQUIRE(node != nullptr);
        CHECK(frame->pos() == frame_at);
        const QPoint frame_now = onView(view, frame->scenePos());
        CHECK(std::abs(frame_now.x() - frame_on_view.x()) <= 2);
        CHECK(std::abs(frame_now.y() - frame_on_view.y()) <= 2);
        at = onView(view, node->mapToScene(heading));
        CHECK(std::abs(at.x() - (grip + delta).x()) <= 2);
        CHECK(std::abs(at.y() - (grip + delta).y()) <= 2);
        CHECK(frame->sceneBoundingRect().contains(node->sceneBoundingRect()));
    }
}

namespace {

QGraphicsPathItem *pendingLine(QGraphicsScene &scene) {
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
            QLatin1String{PelicanStudio::GraphCanvasPendingEdgeItem})
            return dynamic_cast<QGraphicsPathItem *>(item);
    return nullptr;
}

// Where the line being drawn ends (it is drawn from an output).
QPointF lineEnd(QGraphicsScene &scene) {
    QGraphicsPathItem *line = pendingLine(scene);
    REQUIRE(line != nullptr);
    return line->mapToScene(line->path().pointAtPercent(1.0));
}

void sceneMouse(QGraphicsScene &scene, QEvent::Type type, const QPointF &at) {
    QGraphicsSceneMouseEvent event{type};
    event.setScenePos(at);
    event.setButton(type == QEvent::GraphicsSceneMouseMove ? Qt::NoButton : Qt::LeftButton);
    event.setButtons(type == QEvent::GraphicsSceneMouseRelease ? Qt::NoButton : Qt::LeftButton);
    QApplication::sendEvent(&scene, &event);
    QCoreApplication::processEvents();
}

bool near(const QPointF &a, const QPointF &b) { return QLineF{a, b}.length() < 0.5; }

// One zone tab's chip (the item that carries its label) and its text.
QGraphicsPathItem *tabChip(QGraphicsScene &scene, std::size_t branch) {
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasZoneTabItem} &&
            !item->data(PelicanStudio::GraphCanvasLabelRole).toString().isEmpty() &&
            item->data(PelicanStudio::GraphCanvasBranchRole).toULongLong() == branch)
            return dynamic_cast<QGraphicsPathItem *>(item);
    return nullptr;
}

QGraphicsSimpleTextItem *tabText(QGraphicsScene &scene, std::size_t branch) {
    QGraphicsPathItem *chip = tabChip(scene, branch);
    if (chip == nullptr) return nullptr;
    for (QGraphicsItem *child : chip->childItems())
        if (auto *text = dynamic_cast<QGraphicsSimpleTextItem *>(child)) return text;
    return nullptr;
}

bool tabChosen(QGraphicsScene &scene, std::size_t branch) {
    QGraphicsPathItem *chip = tabChip(scene, branch);
    QGraphicsSimpleTextItem *text = tabText(scene, branch);
    REQUIRE(chip != nullptr);
    REQUIRE(text != nullptr);
    const bool ground =
        chip->brush().color() == PelicanStudio::currentStudioTheme().canvas.tab_selected;
    // The ground and the face say the same thing, or neither does.
    CHECK(ground == text->font().bold());
    return ground;
}

} // namespace

// Decision 1. Node Wrangler's lazy connect: while a line is drawn, what it
// can land on is lit and the rest is faint, and near enough is enough.
TEST_CASE("UX-1 a line being drawn lights the sockets it can land on and snaps to the nearest "
          "one in reach",
          "[devstudio][graph-canvas][scene][ux1]") {
    (void)application();

    SECTION("in reach, out of reach, and a socket it cannot land on") {
        ZoneHarness h;
        h.open();
        auto &scene = h.scene;
        QGraphicsItem *curve = socketOf(scene, QString{}, "curve", "value", "output");
        QGraphicsItem *slot = zoneSlot(scene, "in:+");
        // The slot itself, not the `+` drawn on it.
        if (slot != nullptr && slot->parentItem() != nullptr &&
            slot->parentItem()->data(PelicanStudio::NodeGraphItemKindRole) ==
                slot->data(PelicanStudio::NodeGraphItemKindRole))
            slot = slot->parentItem();
        QGraphicsItem *target = socketOf(scene, QString{}, "steps", "target", "input");
        QGraphicsItem *sum_b = socketOf(scene, QStringLiteral("first"), "sum", "b", "input");
        QGraphicsItem *a_out = socketOf(scene, QString{}, "a", "value", "output");
        QGraphicsItem *curve_time = socketOf(scene, QString{}, "curve", "time", "input");
        REQUIRE(curve != nullptr);
        REQUIRE(slot != nullptr);
        REQUIRE(target != nullptr);
        REQUIRE(sum_b != nullptr);
        REQUIRE(a_out != nullptr);
        REQUIRE(curve_time != nullptr);
        const QPointF slot_at = centreOf(slot);

        sceneMouse(scene, QEvent::GraphicsSceneMousePress, centreOf(curve));
        REQUIRE(pendingLine(scene) != nullptr);
        // Lit: a loop's input, the zone's empty slot, a body node's input
        // (each a drop the canvas takes today). Faint: another output, and
        // the line's own node.
        CHECK(slot->scale() > 1.0);
        CHECK(target->scale() > 1.0);
        CHECK(sum_b->scale() > 1.0);
        CHECK(slot->opacity() == 1.0);
        CHECK(a_out->opacity() < 0.5);
        CHECK(curve_time->opacity() < 0.5);
        CHECK(a_out->scale() == 1.0);

        // Twenty pixels short of the slot, over nothing: drawn onto it...
        const QPointF short_of = slot_at + QPointF{-20.0, 5.0};
        sceneMouse(scene, QEvent::GraphicsSceneMouseMove, short_of);
        CHECK(near(lineEnd(scene), slot_at));
        // ...and let go there, it lands there.
        sceneMouse(scene, QEvent::GraphicsSceneMouseRelease, short_of);
        CHECK(pendingLine(scene) == nullptr);
        CHECK(a_out->opacity() == 1.0);
        CHECK(slot->scale() == 1.0);
        CHECK(h.commit() ==
              Json::array({graphInput("first", "value"), graphInput("rest", "value"),
                           {{"op", "set_input"}, {"scope", "root"}, {"node", "steps"},
                            {"port", "value"},
                            {"source", {{"node", "curve"}, {"port", "value"}}}}}));

        // Out of reach of everything: the line follows the hand and lands
        // nowhere.
        curve = socketOf(scene, QString{}, "curve", "value", "output");
        REQUIRE(curve != nullptr);
        const QPointF far{-5000.0, -5000.0};
        sceneMouse(scene, QEvent::GraphicsSceneMousePress, centreOf(curve));
        sceneMouse(scene, QEvent::GraphicsSceneMouseMove, far);
        CHECK(near(lineEnd(scene), far));
        sceneMouse(scene, QEvent::GraphicsSceneMouseRelease, far);
        CHECK(h.answerReads().empty());

        // Right beside a socket it cannot land on (another output): not
        // drawn onto it, and nothing is sent.
        curve = socketOf(scene, QString{}, "curve", "value", "output");
        a_out = socketOf(scene, QString{}, "a", "value", "output");
        REQUIRE(curve != nullptr);
        REQUIRE(a_out != nullptr);
        const QPointF beside = centreOf(a_out) + QPointF{3.0, 0.0};
        sceneMouse(scene, QEvent::GraphicsSceneMousePress, centreOf(curve));
        sceneMouse(scene, QEvent::GraphicsSceneMouseMove, beside);
        CHECK_FALSE(near(lineEnd(scene), centreOf(a_out)));
        sceneMouse(scene, QEvent::GraphicsSceneMouseRelease, beside);
        CHECK(h.answerReads().empty());
    }

    SECTION("a socket of another type is faint and is not snapped to") {
        GraphCanvasModel typed;
        GraphCanvasScene scene;
        typed.setConnected(true);
        answer(typed, connectedDocument(), visualCatalog());
        scene.setModel(&typed);
        REQUIRE(typed.enterInstance("left"));
        scene.repopulate();
        QGraphicsItem *buffer = socketItem(scene, QStringLiteral("buffer"),
                                           QStringLiteral("resource"), QStringLiteral("output"));
        QGraphicsItem *scalar =
            socketItem(scene, QStringLiteral("sum"), QStringLiteral("a"), QStringLiteral("input"));
        REQUIRE(buffer != nullptr);
        REQUIRE(scalar != nullptr);
        REQUIRE_FALSE(typed.canConnect("buffer", "resource", "sum", "a"));
        sceneMouse(scene, QEvent::GraphicsSceneMousePress, centreOf(buffer));
        CHECK(scalar->opacity() < 0.5);
        sceneMouse(scene, QEvent::GraphicsSceneMouseMove, centreOf(scalar) + QPointF{4.0, 0.0});
        CHECK_FALSE(near(lineEnd(scene), centreOf(scalar)));
        sceneMouse(scene, QEvent::GraphicsSceneMouseRelease, centreOf(scalar));
        requireNoEdit(typed);
    }
}

// Decision 4. Add branch: a body of the new branch's own, so its tab shows
// and edits something; the chosen tab is plain to see, also between two
// branches that run one body.
TEST_CASE("UX-1 Add branch gives the branch a body of its own, and the chosen tab is plain",
          "[devstudio][graph-canvas][scene][ux1]") {
    (void)application();

    SECTION("Add branch, the tabs, an edit in the new body, and one Undo") {
        ZoneHarness h{zoneDocument(true)};
        h.open();
        auto &scene = h.scene;
        const auto before = h.document.dump();
        REQUIRE(zoneTabs(scene).empty());

        REQUIRE(h.model.addLoopBranch("steps"));
        const auto ops = h.commit();
        REQUIRE(ops.size() == 2);
        CHECK(ops[0] == Json{{"op", "add_definition"},
                             {"name", "steps_body"},
                             {"definition", zoneDocument().at("definitions").at("first")}});
        CHECK(h.document.at("nodes")[2].at("parameters").at("branches") ==
              Json::array({{{"when", {{"element", 0}}}, {"body", "steps_body"}},
                           {{"body", "first"}}}));
        REQUIRE(zoneTabs(scene).size() == 2);
        // The new branch's body is shown, and its tab is the chosen one.
        CHECK(scene.zones().at("steps") == "steps_body");
        CHECK(tabChosen(scene, 0));
        CHECK_FALSE(tabChosen(scene, 1));
        CHECK(tabChip(scene, 0)->data(PelicanStudio::GraphCanvasValueTextRole).toString().isEmpty());

        // The else's tab: its body, drawn and chosen.
        click(scene, centreOf(tabChip(scene, 1)));
        CHECK(scene.zones().at("steps") == "first");
        CHECK(scopedNode(scene, QStringLiteral("first"), QStringLiteral("sum")) != nullptr);
        CHECK(scopedNode(scene, QStringLiteral("steps_body"), QStringLiteral("sum")) == nullptr);
        CHECK(tabChosen(scene, 1));
        CHECK_FALSE(tabChosen(scene, 0));

        // Back on the new branch, an edit there is its body's alone.
        click(scene, centreOf(tabChip(scene, 0)));
        CHECK(scene.zones().at("steps") == "steps_body");
        REQUIRE(scene.beginLabelEdit("steps_body", "sum"));
        scene.setInlineEditText(QStringLiteral("Mine"));
        REQUIRE(scene.commitInlineEdit());
        CHECK(h.commit() == Json::array({{{"op", "set_node_label"}, {"scope", "steps_body"},
                                          {"node", "sum"}, {"label", "Mine"}}}));
        CHECK(h.document.at("definitions").at("first") ==
              zoneDocument().at("definitions").at("first"));

        // Two Undos: the label, then the branch and its body together.
        for (int step = 0; step < 2; ++step) {
            REQUIRE(h.model.canUndo());
            scene.undoEdit();
            h.commit();
        }
        CHECK(h.document.dump() == before);
    }

    SECTION("two branches that run one body: the tabs say so, and the chosen one still moves") {
        ZoneHarness h{sharedBodyDocument()};
        h.open();
        auto &scene = h.scene;
        QGraphicsPathItem *first = tabChip(scene, 0);
        QGraphicsPathItem *other = tabChip(scene, 1);
        REQUIRE(first != nullptr);
        REQUIRE(other != nullptr);
        CHECK(first->data(PelicanStudio::GraphCanvasValueTextRole).toString() ==
              QStringLiteral("else"));
        CHECK(tabText(scene, 0)->text().contains(QStringLiteral("shared with else")));
        CHECK(other->data(PelicanStudio::GraphCanvasValueTextRole).toString() ==
              QStringLiteral("branch 0"));
        CHECK(first->toolTip().contains(QStringLiteral("same body as else")));
        CHECK(tabChosen(scene, 0));
        click(scene, centreOf(tabChip(scene, 1)));
        CHECK(tabChosen(scene, 1));
        CHECK_FALSE(tabChosen(scene, 0));
        CHECK(h.model.loopBranch("steps") == 1);
    }
}

// Decision 5: the two entries the canvas menu had to itself.
TEST_CASE("UX-1 Auto layout and Fit view stand in the Graph tab's row beside Undo and Redo",
          "[devstudio][graph-canvas][scene][ux1]") {
    (void)application();
    RecordedCapability transport;
    PelicanStudio::GraphCanvasWidget widget{transport.capability()};
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
    answerRpc(QStringLiteral("get_graph_instances"), context(connectedDocument()));
    answerRpc(QStringLiteral("get_graph_catalog"), visualCatalog());

    auto *undo = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvas.undoButton"));
    auto *redo = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvas.redoButton"));
    auto *arrange =
        widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvas.autoLayoutButton"));
    auto *fit = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvas.fitViewButton"));
    REQUIRE(undo != nullptr);
    REQUIRE(redo != nullptr);
    REQUIRE(arrange != nullptr);
    REQUIRE(fit != nullptr);
    CHECK(arrange->text() == QStringLiteral("Auto layout"));
    CHECK(fit->text() == QStringLiteral("Fit view"));
    // In the row, after Redo.
    CHECK(std::abs(arrange->geometry().center().y() - undo->geometry().center().y()) <= 2);
    CHECK(arrange->x() > redo->x());
    CHECK(fit->x() > arrange->x());

    // The menu's own actions.
    auto *arrange_action = widget.findChild<QAction *>(QStringLiteral("pelican.graphCanvas.autoLayout"));
    REQUIRE(arrange_action != nullptr);
    int arranged = 0;
    QObject::connect(arrange_action, &QAction::triggered, [&] { ++arranged; });
    arrange->click();
    QCoreApplication::processEvents();
    CHECK(arranged == 1);
    auto *view = widget.findChild<QGraphicsView *>(QStringLiteral("pelican.graphCanvasView"));
    REQUIRE(view != nullptr);
    const auto fits = view->property("pelicanFitCount").toULongLong();
    fit->click();
    QCoreApplication::processEvents();
    CHECK(view->property("pelicanFitCount").toULongLong() == fits + 1);
}

// Decision 6: the frame's ends are the last column.
TEST_CASE("UX-1 what writes to the screen and a sink stand in the last column, however short "
          "the path to them",
          "[devstudio][graph-canvas][scene][ux1]") {
    (void)application();
    // shade__color -> shade -> post is the longest path; the probe (a sink)
    // reads shade__color, and the Blank's present reads nothing yet.
    Json document{
        {"nodes",
         Json::array({{{"id", "shade__color"}, {"op", "render.image"},
                       {"parameters", {{"format", "R8G8B8A8_UNORM"}}},
                       {"outputs", Json::array({"resource"})}},
                      {{"id", "shade"}, {"op", "render.fullscreen"},
                       {"parameters", Json::object()},
                       {"inputs", {{"color", {{"node", "shade__color"}, {"port", "resource"}}}}},
                       {"outputs", Json::array({"color"})}},
                      {{"id", "post"}, {"op", "render.fullscreen"},
                       {"parameters", Json::object()},
                       {"inputs", {{"color", {{"node", "shade"}, {"port", "color"}}}}},
                       {"outputs", Json::array({"color"})}},
                      {{"id", "probe"}, {"op", "render.ux1_probe"},
                       {"parameters", Json::object()},
                       {"inputs", {{"source", {{"node", "shade__color"}, {"port", "resource"}}}}},
                       {"outputs", Json::array()}}})},
        {"definitions", Json::object()},
        {"layout", Json::object()}};
    auto read = context(document);
    read["source_digest"]["hex"] = "gl1e-root";
    read["root_schema"] = "pelican.graph";
    read["source_reference"] = "fixture";
    auto catalog = visualCatalog();
    catalog["modules"][1]["operations"].push_back(declaredOperation(
        "render.ux1_probe", "rendering",
        Json::array({declaredInput("source", "pelican.render.storage_buffer@1")}), Json::array(),
        true));

    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    for (int round = 0; round < 4; ++round)
        for (const auto &request : model.takeRpcRequests()) {
            if (request.method == "get_graph_instances")
                model.receiveRpcResult(request.request_id, read.dump());
            else if (request.method == "get_graph_catalog")
                model.receiveRpcResult(request.request_id, catalog.dump());
            else if (request.method == "get_frame_plan")
                model.receiveRpcResult(request.request_id, gl1ePlan({}).dump());
            else if (request.method == "get_render_authoring_context")
                model.receiveRpcResult(request.request_id, gl1eAuthoring({}).dump());
        }
    scene.setModel(&model);

    const auto columnOf = [&](const QString &id) {
        for (QGraphicsItem *item : scene.items())
            if (item->parentItem() == nullptr &&
                item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                    QLatin1String{PelicanStudio::GraphCanvasNodeItem} &&
                item->data(PelicanStudio::NodeGraphNameRole).toString() == id)
                return item->data(PelicanStudio::GraphCanvasColumnRole).toInt();
        FAIL("no node " << id.toStdString());
        return -1;
    };
    REQUIRE(scene.property("pelicanGraphFoldedCount").toULongLong() >= 1);
    CHECK(columnOf(QStringLiteral("shade__color")) == 0);
    CHECK(columnOf(QStringLiteral("shade")) == 1);
    CHECK(columnOf(QStringLiteral("post")) == 2);
    // The sink, one step from its source, and present, with nothing before
    // it: both in the last column.
    CHECK(columnOf(QStringLiteral("probe")) == 2);
    CHECK(columnOf(QStringLiteral("present")) == 2);
    // And drawn there: no node of the graph is right of them.
    QGraphicsItem *present = nullptr;
    QGraphicsItem *post = scopedNode(scene, QString{}, QStringLiteral("post"));
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::GraphCanvasDerivedRole).toBool() &&
            item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasNodeItem} &&
            item->data(PelicanStudio::NodeGraphNameRole).toString() == QStringLiteral("present"))
            present = item;
    REQUIRE(present != nullptr);
    REQUIRE(post != nullptr);
    CHECK(present->scenePos().x() >= post->scenePos().x() - 1.0);
}

// Decision 7: an engine's refusal can be taken away whole.
TEST_CASE("UX-1 a notice's Copy message puts the whole message on the clipboard",
          "[devstudio][graph-canvas][scene][ux1]") {
    (void)application();
    RecordedCapability transport;
    PelicanStudio::GraphCanvasWidget widget{transport.capability()};
    widget.resize(900, 600);
    widget.show();
    QCoreApplication::processEvents();
    auto *notice = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasNotice"));
    REQUIRE(notice != nullptr);
    auto *copy = notice->findChild<QAction *>(QStringLiteral("pelican.panelNotice.copyMessage"));
    REQUIRE(copy != nullptr);
    CHECK(copy->text() == QStringLiteral("Copy message"));
    // Offered on a right-click, and the words can be selected.
    CHECK(notice->contextMenuPolicy() == Qt::ActionsContextMenu);
    CHECK(notice->actions().contains(copy));
    CHECK((notice->textInteractionFlags() & Qt::TextSelectableByMouse) != 0);

    // What the tab shows for an engine refusal: the line cut to fit, the
    // whole in the tooltip.
    const QString whole = QString::fromUtf8(
        "render_pipeline_preflight_failed: Pass input target is not produced as an earlier "
        "output: present reads shade__color. The pass that writes it runs later. "
        "\xE2\x80\x94 the graph pass that draws it runs after this pass");
    notice->setToolTip(whole);
    notice->setText(PelicanStudio::oneLine(whole));
    REQUIRE(notice->text() != whole);
    QGuiApplication::clipboard()->clear();
    copy->trigger();
    CHECK(QGuiApplication::clipboard()->text() == whole);

    // A row with no tooltip copies itself; one whose tooltip is a detail
    // the line does not say copies both.
    QLabel plain;
    PelicanStudio::applyPanelNoticeStyle(&plain);
    plain.setText(QStringLiteral("Saved."));
    auto *plain_copy = plain.findChild<QAction *>(QStringLiteral("pelican.panelNotice.copyMessage"));
    REQUIRE(plain_copy != nullptr);
    plain_copy->trigger();
    CHECK(QGuiApplication::clipboard()->text() == QStringLiteral("Saved."));
    plain.setToolTip(QStringLiteral("Written to project://graph.json"));
    plain_copy->trigger();
    CHECK(QGuiApplication::clipboard()->text() ==
          QStringLiteral("Saved.\nWritten to project://graph.json"));
    // Styled twice, still one entry.
    PelicanStudio::applyPanelNoticeStyle(&plain);
    CHECK(plain.actions().size() == 1);
}

// --- UX-2: a node's values picked rather than typed (the render.image form,
// a render pass's shaders, the pills that open a list, the JSON that stays).

namespace {

// The catalog's two image types as the engine's schemaJson writes them.
Json ux2ImageTypeSchema(const std::string &id, const Json &traits, const Json &parameters) {
    return {{"id", id}, {"constructor", "image"}, {"traits", traits}, {"parameters", parameters}};
}

Json ux2Catalog() {
    Json catalog = visualCatalog();
    auto &render = catalog["modules"][1];
    for (const char *op : {"render.image", "render.fullscreen"})
        render["operations"].push_back({{"op", op},
                                        {"domain", "rendering"},
                                        {"evaluation", "direct"},
                                        {"sink", false},
                                        {"ports", "parameter_dependent"}});
    const auto argument = [](const char *name, const Json &allowed, const char *fallback = nullptr) {
        Json entry{{"name", name}, {"role", "identity"}, {"kind", "enum_value"}, {"allowed", allowed}};
        if (fallback != nullptr) entry["default"] = {{"kind", "enum_value"}, {"value", fallback}};
        return entry;
    };
    const auto topology = argument("topology", {"two_d", "two_d_array", "cube", "volume"}, "two_d");
    render["types"].push_back(ux2ImageTypeSchema(
        "pelican.render.color_signal@1", Json::array({"color_like"}),
        Json::array({argument("range", {"extended", "normalized"}),
                     argument("reference", {"display", "scene"}), topology,
                     argument("transfer", {"linear", "output_encoded"})})));
    // The depth type as an engine with EL-1 writes it: the arguments' table,
    // not the parameter list.
    auto depth = ux2ImageTypeSchema("pelican.render.depth@1", Json::array({"depth_like"}), Json::array());
    depth["arguments"] = {
        {"representation", {{"kind", "enum_value"}, {"required", true}, {"values", {"device", "linear_distance"}}}},
        {"space", {{"kind", "enum_value"}, {"required", true}, {"values", {"projection", "view"}}}},
        {"topology", {{"kind", "enum_value"}, {"required", false},
                      {"values", {"two_d", "two_d_array", "cube", "volume"}}, {"default", "two_d"}}}};
    render["types"].push_back(std::move(depth));
    render["types"].push_back(ux2ImageTypeSchema("pelican.render.legacy_opaque_image@1",
                                                 Json::array({"legacy_untyped"}), Json::array()));
    return catalog;
}

Json ux2ColorType() {
    return {{"semantic", "pelican.render.color_signal@1"},
            {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
}

// A fullscreen pass and the image it writes, as the palette adds them, and a
// key the fields do not show.
Json ux2Image() {
    return {{"type", ux2ColorType()},
            {"format", "R8G8B8A8_UNORM"},
            {"extent_scale", 1.0},
            {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
            {"note", "kept"}};
}

Json ux2Pass() {
    return {{"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}},
            {"ports", {{"color", {{"kind", "image"}, {"type", ux2ColorType()}, {"access", "write"},
                                  {"attachment", "color"}, {"location", 0}}}}}};
}

Json ux2Document(const Json &image = ux2Image(), const Json &pass = ux2Pass()) {
    return {{"definitions", Json::object()},
            {"nodes", Json::array({{{"id", "shade__color"}, {"op", "render.image"},
                                    {"parameters", image}, {"outputs", Json::array({"resource"})}},
                                   {{"id", "shade"}, {"op", "render.fullscreen"},
                                    {"parameters", pass},
                                    {"inputs", {{"color", {{"node", "shade__color"}, {"port", "resource"}}}}},
                                    {"outputs", Json::array({"color"})}}})}};
}

// get_render_authoring_context with the vocabulary the engine's parser has
// (a few of its formats; enough to tell the depth ones apart).
Json ux2Authoring() {
    return {{"source_digest", {{"hex", "ux2-root"}}},
            {"graphs", Json::array()},
            {"render_target_vocabulary",
             {{"formats", Json::array({"R8G8B8A8_UNORM", "R16G16B16A16_SFLOAT", "B10G11R11_UFLOAT_PACK32",
                                       "D16_UNORM", "D32_SFLOAT"})},
              {"usage", Json::array({"COLOR_ATTACHMENT", "DEPTH_STENCIL_ATTACHMENT", "SAMPLED",
                                     "STORAGE", "TRANSFER_DST", "TRANSFER_SRC"})},
              {"format_classes", Json::array({"scene", "display", "data", "explicit"})}}}};
}

// The Graph tab over a recorded transport; `authoring` says whether the
// engine answers get_render_authoring_context at all.
struct Ux2Tab {
    RecordedCapability transport;
    std::unique_ptr<PelicanStudio::GraphCanvasWidget> widget;

    explicit Ux2Tab(bool authoring) {
        auto capability = transport.capability();
        if (authoring) {
            capability.requestRenderAuthoringContext = [this](QString *) {
                transport.calls.push_back({QStringLiteral("get_render_authoring_context")});
                return transport.next++;
            };
            // RTL-2s-c: asked after a context that names a ray layout.
            capability.requestRayLayout = [this](QString *) {
                transport.calls.push_back({QStringLiteral("get_ray_layout")});
                return transport.next++;
            };
        }
        widget = std::make_unique<PelicanStudio::GraphCanvasWidget>(capability);
        widget->resize(1100, 800);
        widget->show();
        QCoreApplication::processEvents();
    }
    void answer(const char *method, const Json &reply) {
        const qint64 id = transport.idOf(QString::fromLatin1(method));
        REQUIRE(id != 0);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    }
    [[nodiscard]] int reads(const char *method) const {
        return transport.countOf(QString::fromLatin1(method));
    }
    void read(const Json &document, const std::string &digest) {
        auto reply = context(document);
        reply["source_digest"]["hex"] = digest;
        answer("get_graph_instances", reply);
    }
    // The edit just sent, committed at `digest`, and the document read back.
    void commit(const Json &document, const std::string &digest) {
        answer("edit_graph_document", Json{{"status", "accepted"}, {"ticket", "ux2"}});
        answer("get_edit_result", Json{{"status", "committed"}, {"committed", true},
                                       {"source_digest", {{"hex", digest}}}});
        read(document, digest);
    }
    // The ops of the last edit, which has to be the `count`th.
    [[nodiscard]] Json sent(int count) const {
        REQUIRE(reads("edit_graph_document") == count);
        const auto ops = Json::parse(transport.calls[static_cast<std::size_t>(
                                                         transport.idOf(QStringLiteral("edit_graph_document")) - 1)]
                                         .body.toStdString());
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].at("op") == "set_node_parameters");
        return ops[0];
    }
    template <typename T> T *field(const char *name) {
        auto *found = widget->findChild<T *>(QString::fromLatin1(name));
        REQUIRE(found != nullptr);
        return found;
    }
    void pick(QComboBox *combo, const QString &data) {
        const int index = combo->findData(data);
        REQUIRE(index >= 0);
        combo->setCurrentIndex(index);
        Q_EMIT combo->activated(index);
        QCoreApplication::processEvents();
    }
};

QComboBox *ux2PillChoice(QGraphicsScene &scene) {
    for (QGraphicsItem *item : scene.items())
        if (auto *proxy = dynamic_cast<QGraphicsProxyWidget *>(item))
            if (auto *combo = qobject_cast<QComboBox *>(proxy->widget());
                combo != nullptr &&
                combo->objectName() == QLatin1String{"pelican.graphCanvasValueChoice"})
                return combo;
    return nullptr;
}

QStringList ux2Items(const QComboBox &combo) {
    QStringList items;
    for (int index = 0; index < combo.count(); ++index) items << combo.itemText(index);
    return items;
}

} // namespace

TEST_CASE("UX-2 a render.image's format, type, usage and size are picked in its fields, each one "
          "set_node_parameters; its pills open the list; the details still take JSON; an engine "
          "without the vocabulary falls back to text",
          "[devstudio][graph-canvas][scene][ux2]") {
    (void)application();
    SECTION("with the engine's vocabulary") {
        Ux2Tab tab{true};
        auto image = ux2Image();
        tab.read(ux2Document(image), "ux2-0");
        tab.answer("get_graph_catalog", ux2Catalog());
        tab.widget->scene().selectNode("shade__color");
        QCoreApplication::processEvents();
        auto *fields = tab.field<QWidget>("pelican.graphCanvasImage");
        REQUIRE(fields->isVisible());
        // The plan names no pass of the rendering section here, so the
        // fields ask for the context themselves, once.
        tab.widget->poll();
        REQUIRE(tab.reads("get_render_authoring_context") == 1);
        tab.answer("get_render_authoring_context", ux2Authoring());
        tab.widget->poll();
        CHECK(tab.reads("get_render_authoring_context") == 1);

        // Decision 1, format: the engine's colour formats, no depth one, not typed.
        auto *format = tab.field<QComboBox>("pelican.graphCanvasImage.format");
        CHECK_FALSE(format->isEditable());
        CHECK(ux2Items(*format) == QStringList{"R8G8B8A8_UNORM", "R16G16B16A16_SFLOAT",
                                               "B10G11R11_UFLOAT_PACK32"});
        CHECK(format->currentText() == QStringLiteral("R8G8B8A8_UNORM"));
        tab.pick(format, QStringLiteral("R16G16B16A16_SFLOAT"));
        auto op = tab.sent(1);
        CHECK(op.at("node") == "shade__color");
        CHECK(op.at("scope") == "root");
        image["format"] = "R16G16B16A16_SFLOAT";
        // The whole object, the key the fields do not show included.
        CHECK(op.at("parameters") == image);
        tab.commit(ux2Document(image), "ux2-1");

        // Type: by name, and an argument from what its schema allows.
        auto *semantic = tab.field<QComboBox>("pelican.graphCanvasImage.semantic");
        CHECK(semantic->currentText() == QStringLiteral("Color signal"));
        CHECK(ux2Items(*semantic) == QStringList{"Color signal", "Depth"});
        auto *range = tab.field<QComboBox>("pelican.graphCanvasImage.argument.range");
        CHECK(ux2Items(*range) == QStringList{"extended", "normalized"});
        auto *topology = tab.field<QComboBox>("pelican.graphCanvasImage.argument.topology");
        CHECK(topology->currentText() == QStringLiteral("(default: two_d)"));
        tab.pick(range, QStringLiteral("normalized"));
        op = tab.sent(2);
        image["type"]["arguments"]["range"] = "normalized";
        CHECK(op.at("parameters") == image);
        tab.commit(ux2Document(image), "ux2-2");

        // Usage: a check per flag the engine names.
        auto *storage = tab.field<QCheckBox>("pelican.graphCanvasImage.usage.STORAGE");
        CHECK(tab.field<QCheckBox>("pelican.graphCanvasImage.usage.SAMPLED")->isChecked());
        CHECK_FALSE(storage->isChecked());
        storage->click();
        QCoreApplication::processEvents();
        op = tab.sent(3);
        image["usage"] = Json::array({"COLOR_ATTACHMENT", "SAMPLED", "STORAGE"});
        CHECK(op.at("parameters") == image);
        tab.commit(ux2Document(image), "ux2-3");

        // Size: fixed, width and height beside the scale the parser wants.
        tab.field<QRadioButton>("pelican.graphCanvasImage.sizeFixed")->click();
        QCoreApplication::processEvents();
        op = tab.sent(4);
        image["width"] = 256;
        image["height"] = 256;
        CHECK(op.at("parameters") == image);
        tab.commit(ux2Document(image), "ux2-4");
        CHECK(tab.field<QSpinBox>("pelican.graphCanvasImage.width")->isEnabled());
        CHECK_FALSE(tab.field<QDoubleSpinBox>("pelican.graphCanvasImage.extentScale")->isEnabled());

        // Depth: the format and the attachment flag cross with it, in one edit.
        tab.pick(tab.field<QComboBox>("pelican.graphCanvasImage.semantic"),
                 QStringLiteral("pelican.render.depth@1"));
        op = tab.sent(5);
        auto depth = image;
        depth["type"] = {{"semantic", "pelican.render.depth@1"},
                         {"arguments", {{"representation", "device"}, {"space", "projection"}}}};
        depth["format"] = "D32_SFLOAT";
        depth["usage"] = Json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED", "STORAGE"});
        CHECK(op.at("parameters") == depth);
        tab.commit(ux2Document(depth), "ux2-5");
        CHECK(ux2Items(*tab.field<QComboBox>("pelican.graphCanvasImage.format")) ==
              QStringList{"D16_UNORM", "D32_SFLOAT"});
        // Its arguments from EL-1's table, the left-out one as its default.
        CHECK(ux2Items(*tab.field<QComboBox>("pelican.graphCanvasImage.argument.space")) ==
              QStringList{"projection", "view"});
        CHECK(tab.field<QComboBox>("pelican.graphCanvasImage.argument.topology")->currentText() ==
              QStringLiteral("(default: two_d)"));

        // Decision 4: the format's pill opens the same list, not a line.
        auto &scene = tab.widget->scene();
        REQUIRE(scene.beginValueEdit("shade__color", "format"));
        auto *pill = ux2PillChoice(scene);
        REQUIRE(pill != nullptr);
        CHECK(ux2Items(*pill) == QStringList{"D16_UNORM", "D32_SFLOAT"});
        CHECK(scene.inlineEditText() == QStringLiteral("D32_SFLOAT"));
        scene.setInlineEditText(QStringLiteral("D16_UNORM"));
        REQUIRE(scene.commitInlineEdit());
        op = tab.sent(6);
        depth["format"] = "D16_UNORM";
        CHECK(op.at("parameters") == depth);
        tab.commit(ux2Document(depth), "ux2-6");
        // A number is still a number.
        REQUIRE(scene.beginValueEdit("shade__color", "extent_scale"));
        CHECK(ux2PillChoice(scene) == nullptr);
        scene.cancelInlineEdit();

        // Decision 5: the details' parameters still take JSON, one key.
        tab.widget->scene().selectNode("shade__color");
        QCoreApplication::processEvents();
        auto *details = tab.field<QTreeWidget>("pelican.graphCanvasDetails");
        // The row under "parameters" (the port table names the key too).
        auto rows = details->findItems(QStringLiteral("note"), Qt::MatchExactly | Qt::MatchRecursive);
        rows.removeIf([](const QTreeWidgetItem *row) {
            return row->parent() == nullptr ||
                   row->parent()->text(0) != QStringLiteral("parameters");
        });
        REQUIRE(rows.size() == 1);
        Q_EMIT details->itemDoubleClicked(rows.front(), 1);
        rows.front()->setText(1, QStringLiteral("{\"by\": \"hand\"}"));
        QCoreApplication::processEvents();
        op = tab.sent(7);
        depth["note"] = {{"by", "hand"}};
        CHECK(op.at("parameters") == depth);
    }

    SECTION("an engine without the vocabulary: the format is typed, the usage a line") {
        Ux2Tab tab{false};
        auto image = ux2Image();
        tab.read(ux2Document(image), "ux2-0");
        tab.answer("get_graph_catalog", ux2Catalog());
        tab.widget->scene().selectNode("shade__color");
        QCoreApplication::processEvents();
        tab.widget->poll();
        QCoreApplication::processEvents();
        auto *format = tab.field<QComboBox>("pelican.graphCanvasImage.format");
        REQUIRE(format->isVisible());
        CHECK(format->isEditable());
        CHECK(format->currentText() == QStringLiteral("R8G8B8A8_UNORM"));
        CHECK(tab.field<QLineEdit>("pelican.graphCanvasImage.usageText")->isVisible());
        CHECK(tab.field<QLineEdit>("pelican.graphCanvasImage.usageText")->text() ==
              QStringLiteral("COLOR_ATTACHMENT, SAMPLED"));
        CHECK(tab.field<QLabel>("pelican.graphCanvasImage.note")->isVisible());
        // The pill is the line it was.
        REQUIRE(tab.widget->scene().beginValueEdit("shade__color", "format"));
        CHECK(ux2PillChoice(tab.widget->scene()) == nullptr);
        tab.widget->scene().cancelInlineEdit();
        format->lineEdit()->setText(QStringLiteral("R16G16B16A16_SFLOAT"));
        Q_EMIT format->lineEdit()->returnPressed();
        QCoreApplication::processEvents();
        const auto op = tab.sent(1);
        image["format"] = "R16G16B16A16_SFLOAT";
        CHECK(op.at("parameters") == image);
    }
}

TEST_CASE("UX-2 a render pass's shaders are picked from the Pass Editor's list, its ports shown as "
          "declared, and its shader pill opens the fragment list",
          "[devstudio][graph-canvas][scene][ux2]") {
    (void)application();
    Ux2Tab tab{false};
    auto pass = ux2Pass();
    tab.read(ux2Document(ux2Image(), pass), "ux2-0");
    tab.answer("get_graph_catalog", ux2Catalog());
    tab.widget->scene().selectNode("shade");
    QCoreApplication::processEvents();
    REQUIRE(tab.field<QWidget>("pelican.graphCanvasPass")->isVisible());
    CHECK_FALSE(tab.field<QWidget>("pelican.graphCanvasImage")->isVisible());

    // The Pass Editor's own list (no project open: the engine's shaders).
    auto *fragment = tab.field<QComboBox>("pelican.graphCanvasPass.shader.fragment");
    auto *vertex = tab.field<QComboBox>("pelican.graphCanvasPass.shader.vertex");
    QStringList expected;
    for (const auto &reference :
         PelicanStudio::fullscreenShaderChoices(PelicanStudio::FullscreenShaderStage::fragment, std::nullopt))
        expected << QString::fromStdString(reference);
    CHECK(ux2Items(*fragment) == expected);
    CHECK(fragment->currentText() == QStringLiteral("engine://passthrough"));
    CHECK(vertex->currentText() == QStringLiteral("engine://fullscreen"));
    CHECK(tab.field<QLabel>("pelican.graphCanvasPass.ports")->text() ==
          QStringLiteral("color: image write color"));

    tab.pick(fragment, QStringLiteral("engine://scene_present"));
    auto op = tab.sent(1);
    CHECK(op.at("node") == "shade");
    pass["shader"]["fragment"] = "engine://scene_present";
    CHECK(op.at("parameters") == pass);
    tab.commit(ux2Document(ux2Image(), pass), "ux2-1");
    CHECK(tab.field<QComboBox>("pelican.graphCanvasPass.shader.fragment")->currentText() ==
          QStringLiteral("engine://scene_present"));

    // Decision 4: the shader's pill is the fragment list too.
    auto &scene = tab.widget->scene();
    REQUIRE(scene.beginValueEdit("shade", "shader"));
    auto *pill = ux2PillChoice(scene);
    REQUIRE(pill != nullptr);
    CHECK(ux2Items(*pill) == expected);
    CHECK(pill->currentText() == QStringLiteral("engine://scene_present"));
    // The document's own value sends nothing.
    REQUIRE_FALSE(scene.commitInlineEdit());
    CHECK(tab.reads("edit_graph_document") == 1);
    REQUIRE(scene.beginValueEdit("shade", "shader"));
    scene.setInlineEditText(QStringLiteral("engine://passthrough"));
    REQUIRE(scene.commitInlineEdit());
    op = tab.sent(2);
    pass["shader"]["fragment"] = "engine://passthrough";
    CHECK(op.at("parameters") == pass);
}

// --- UX-3: the palette's other render nodes, Place, a socket's settings,
// a definition's proposed name and depth_compare, the engine's lists.

namespace {

// A fullscreen pass and its image, as a document the engine's graph
// validation reads (partitionGraphDocument), its image copyable.
Json ux3Document() {
    auto image = ux2Image();
    image.erase("note");
    image["usage"] = Json::array({"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"});
    auto document = ux2Document(image);
    document["schema"] = "pelican.graph";
    document["version"] = 1;
    document["outputs"] = Json::object();
    return document;
}

// get_render_authoring_context as an engine with EL-1 answers it: two
// declared targets (and the graph's own image, which is not one), two
// families, the materials with their tags and variants, the providers.
Json ux3Authoring() {
    auto authoring = ux2Authoring();
    authoring["graphs"] = Json::array({{{"name", "main_render"}, {"passes", Json::array()}},
                                       {{"name", "overlay"}, {"passes", Json::array()}}});
    authoring["render_targets"] = Json::array({
        {{"name", "scene_color"},
         {"declaration", {{"name", "scene_color"}, {"format", "R16G16B16A16_SFLOAT"},
                          {"extent_scale", 1.0}}},
         {"editable", true}, {"source", "authoring"}},
        {{"name", "shadow_map"},
         {"declaration", {{"name", "shadow_map"}, {"format", "D32_SFLOAT"}, {"layers", 4},
                          {"width", 1024}, {"height", 1024}}},
         {"editable", false}, {"source", "preset"}},
        {{"name", "shade__color"},
         {"declaration", {{"name", "shade__color"}, {"format", "R8G8B8A8_UNORM"}}},
         {"editable", false}, {"source", "composed"}}});
    authoring["materials"] = Json::array({
        {{"id", "materials/m.material.json#metal"}, {"name", "metal"},
         {"tags", {"hero"}}, {"variants", {"outline"}}, {"builtin", false}},
        {{"id", "materials/m.material.json#glass"}, {"name", "glass"},
         {"tags", {"crowd", "hero"}}, {"variants", {"depth_only", "outline"}}, {"builtin", false}},
        {{"id", "pelican.builtin.transparent"}, {"name", "pelican.builtin.transparent"},
         {"tags", Json::array()}, {"variants", Json::array()}, {"builtin", true}}});
    authoring["material_tags"] = Json::array({"crowd", "hero"});
    authoring["region_providers"] =
        Json::array({{{"name", "builtin.identity_v1"}, {"builtin", true}, {"provider_version", 1}}});
    return authoring;
}

bool ux3Offered(const GraphCanvasModel &model, std::string_view op) {
    return std::ranges::any_of(model.paletteOperationsIn({}, op),
                               [&](const auto &entry) { return entry.op == op; });
}

} // namespace

TEST_CASE("UX-3 decision 1: the palette places target_ref, image, buffer, attribute, raster, "
          "compute, shadow_depth and snapshot_copy seeded, each in one edit the engine's graph "
          "validation takes",
          "[devstudio][graph-canvas][scene][ux3]") {
    (void)application();
    // UX-5d: an engine whose shader list is known and holds no ray tracing
    // pipeline (until the list is known, a compute task is offered: the
    // engine's pipelines are read when it is picked).
    auto catalog = engineCatalog();
    catalog["engine_shaders"] = Json::array();
    const auto registry = Pelican::makeBuiltinGraphModuleRegistry();
    const auto document = ux3Document();
    const auto placedWith = [&](const Json &ops) {
        auto placed = document;
        for (const auto &op : ops) {
            REQUIRE(op.at("op") == "add_node");
            REQUIRE(op.at("scope") == "root");
            placed["nodes"].push_back(op.at("node"));
        }
        return placed;
    };
    // What the palette's plain path placed before: a copy naming no pass to
    // copy after (and no parameters at all) is refused by the engine.
    {
        auto bare = document;
        bare["nodes"].push_back({{"id", "copy"}, {"op", "render.snapshot_copy"},
                                 {"inputs", {{"source", {{"node", "shade__color"}, {"port", "resource"}}},
                                             {"destination", {{"node", "shade__color"}, {"port", "resource"}}}}},
                                 {"outputs", {"destination"}}});
        CHECK_THROWS(Pelican::partitionGraphDocument(bare, registry));
        bare["nodes"].back()["parameters"] = Json::object();
        CHECK_THROWS_WITH(Pelican::partitionGraphDocument(bare, registry),
                          Catch::Matchers::ContainsSubstring("snapshot_after"));
    }

    GraphCanvasModel model;
    model.setConnected(true);
    answer(model, document, catalog);
    REQUIRE(model.canEdit());
    // Without the shaders they start from, no raster pass and no compute task.
    CHECK_FALSE(ux3Offered(model, "render.raster"));
    CHECK_FALSE(ux3Offered(model, "render.compute"));
    model.setShaderChoices({"engine://fullscreen"}, {"engine://passthrough", "engine://fullscreen"});
    model.setComputeShaderChoices({"project://shaders/blur", "project://shaders/compute"});
    for (const char *op : {"render.target_ref", "render.image", "render.buffer", "render.attribute",
                           "render.raster", "render.compute", "render.shadow_depth",
                           "render.snapshot_copy"}) {
        INFO(op);
        CHECK(ux3Offered(model, op));
    }
    // The declared targets come with the authoring context.
    model.askRenderTargetVocabulary();
    for (const auto &request : model.takeRpcRequests())
        if (request.method == "get_render_authoring_context")
            model.receiveRpcResult(request.request_id, ux3Authoring().dump());
    const auto targets = model.declaredRenderTargets();
    REQUIRE(targets.size() == 2);
    CHECK(targets[0].name == "scene_color");
    CHECK(targets[1].name == "shadow_map");
    CHECK(targets[1].depth);

    struct Case {
        const char *op;
        Json seed;
    };
    const std::vector<Case> cases{
        {"render.target_ref", {{"name", "shadow_map"}}},
        {"render.image", Json::object()},
        {"render.buffer", {{"element", "vec4"}, {"count", 4}}},
        {"render.attribute", {{"element", "vec3"}, {"domain", "index"}, {"count", 6}}},
        {"render.raster", Json::object()},
        {"render.compute", Json::object()},
        {"render.shadow_depth", Json::object()},
        {"render.snapshot_copy", {{"snapshot_after", "shade"}}}};
    std::map<std::string, Json> added;
    for (const auto &[op, seed] : cases) {
        INFO(op);
        const auto ops = model.addNodeOpsIn({}, op, {}, seed);
        REQUIRE_FALSE(ops.empty());
        CHECK_NOTHROW(Pelican::partitionGraphDocument(placedWith(ops), registry));
        added[op] = ops.back().at("node");
    }
    // The reference: by name, its type from the declaration (a layered depth).
    CHECK(added["render.target_ref"].at("id") == "shadow_map_ref");
    CHECK(added["render.target_ref"].at("parameters") ==
          Json{{"name", "shadow_map"},
               {"type", {{"semantic", "pelican.render.depth@1"},
                         {"arguments", {{"representation", "device"}, {"space", "projection"},
                                        {"topology", "two_d_array"}}}}}});
    // The image: the size of the screen, scene colour, sixteen bits a channel.
    CHECK(added["render.image"].at("parameters").at("format") == "R16G16B16A16_SFLOAT");
    CHECK(added["render.image"].at("parameters").at("extent_scale") == 1.0);
    // A buffer's bytes; an index attribute is uint, its size its count's.
    CHECK(added["render.buffer"].at("parameters") == Json{{"element", "vec4"}, {"size", 64}});
    CHECK(added["render.attribute"].at("parameters") ==
          Json{{"element", "uint"}, {"domain", "index"}, {"count", 6}});
    // The raster pass draws the screen-covering triangle with the engine's
    // shaders; the compute task starts from the project's `compute`.
    CHECK(added["render.raster"].at("parameters").at("draw") == Json{{"vertex_count", 3}});
    CHECK(added["render.compute"].at("parameters").at("shader") == "project://shaders/compute");
    // The shadow pass draws into a depth image of its own.
    CHECK(added["render.shadow_depth"].at("inputs").at("depth").at("node") ==
          "render_shadow_depth__depth");
    // The copy: after `shade`, of what `shade` wrote, into an image shaped
    // like it.
    const auto &copy = added["render.snapshot_copy"];
    CHECK(copy.at("parameters") == Json{{"snapshot_after", "shade"}});
    CHECK(copy.at("inputs").at("source") == Json{{"node", "shade"}, {"port", "color"}});
    const auto copy_ops = model.addNodeOpsIn({}, "render.snapshot_copy", {}, {{"snapshot_after", "shade"}});
    REQUIRE(copy_ops.size() == 2);
    CHECK(copy_ops[0].at("node").at("parameters").at("format") == "R8G8B8A8_UNORM");
    CHECK(copy_ops[0].at("node").at("parameters").at("usage") ==
          Json::array({"TRANSFER_DST", "SAMPLED"}));
    // Nothing to name: nothing is placed.
    CHECK(model.addNodeOpsIn({}, "render.target_ref", {}, {{"name", "nowhere"}}).empty());
    CHECK(model.addNodeOpsIn({}, "render.snapshot_copy", {}, Json::object()).empty());

    // One gesture is one edit: the copy and its image together (one Undo).
    REQUIRE(model.addNodeIn({}, "render.snapshot_copy", {}, {{"snapshot_after", "shade"}}));
    const auto edit = takeEdit(model);
    CHECK(edit.at("ops") == copy_ops);
}

TEST_CASE("UX-3 decision 2: a render node's Place and family are picked, each one "
          "set_node_parameters; a compute task takes none",
          "[devstudio][graph-canvas][scene][ux3]") {
    (void)application();
    Ux2Tab tab{true};
    auto document = ux2Document();
    tab.read(document, "ux3-0");
    tab.answer("get_graph_catalog", ux2Catalog());
    tab.widget->scene().selectNode("shade");
    QCoreApplication::processEvents();
    auto *fields = tab.field<QWidget>("pelican.graphCanvasPlace");
    REQUIRE(fields->isVisible());
    tab.widget->poll();
    REQUIRE(tab.reads("get_render_authoring_context") == 1);
    tab.answer("get_render_authoring_context", ux3Authoring());
    tab.widget->poll();
    QCoreApplication::processEvents();
    auto *place = tab.field<QComboBox>("pelican.graphCanvasPlace.place");
    auto *family = tab.field<QComboBox>("pelican.graphCanvasPlace.passSet");
    CHECK(place->currentData().toString() == QStringLiteral("automatic|"));
    CHECK(place->findData(QStringLiteral("anchor|post_ldr")) >= 0);
    CHECK(family->findData(QStringLiteral("overlay")) >= 0);

    // After an anchor: one edit, the anchor and nothing else changed.
    tab.pick(place, QStringLiteral("anchor|post_ldr"));
    auto op = tab.sent(1);
    auto expected = ux2Pass();
    expected["canonical_anchor"] = "post_ldr";
    CHECK(op.at("parameters") == expected);
    document["nodes"][1]["parameters"] = expected;
    tab.commit(document, "ux3-1");
    QCoreApplication::processEvents();
    // The family: the place stays.
    family = tab.field<QComboBox>("pelican.graphCanvasPlace.passSet");
    tab.pick(family, QStringLiteral("overlay"));
    op = tab.sent(2);
    expected["pass_set"] = "overlay";
    CHECK(op.at("parameters") == expected);

    // An image has no place.
    tab.widget->scene().selectNode("shade__color");
    QCoreApplication::processEvents();
    CHECK_FALSE(fields->isVisible());

    // A compute task shows the field, and takes no place.
    CHECK(GraphCanvasModel::showsPlace("render.compute"));
    CHECK_FALSE(GraphCanvasModel::takesPlace("render.compute"));
    CHECK_FALSE(GraphCanvasModel::showsPlace("render.image"));
    CHECK(PelicanStudio::GraphCanvasPlaceFields::placeData({"before", "lighting", true, {}}) ==
          QStringLiteral("before|lighting"));
}

TEST_CASE("UX-3 decision 3: a render node's socket menu writes its port's previous frame, "
          "sampling, mip / layer and location, each one set_node_parameters",
          "[devstudio][graph-canvas][scene][ux3]") {
    (void)application();
    Ux2Tab tab{true};
    auto pass = ux2Pass();
    // A port the shader samples by a binding of its own, and the attachment.
    pass["ports"]["previous"] = {{"kind", "image"}, {"type", ux2ColorType()}, {"access", "read"}};
    auto document = ux2Document(ux2Image(), pass);
    document["nodes"][1]["inputs"]["previous"] = {{"node", "shade__color"}, {"port", "resource"}};
    tab.read(document, "ux3-0");
    tab.answer("get_graph_catalog", ux2Catalog());
    auto &model = tab.widget->model();
    REQUIRE(model.canEdit());
    int submitted = 0;

    // The read port: everything but a location.
    QMenu menu;
    PelicanStudio::addGraphCanvasSocketEntries(menu, model, {}, "shade", "previous",
                                               [&] {
                                                   ++submitted;
                                                   // What the canvas does after an edit: posts it.
                                                   tab.widget->poll();
                                               });
    auto *history = menu.findChild<QAction *>(QStringLiteral("pelican.graphCanvasSocket.history"));
    REQUIRE(history != nullptr);
    CHECK(history->isEnabled());
    CHECK_FALSE(history->isChecked());
    auto *location = menu.findChild<QSpinBox *>(QStringLiteral("pelican.graphCanvasSocket.location"));
    REQUIRE(location != nullptr);
    CHECK_FALSE(location->isEnabled());
    history->trigger();
    QCoreApplication::processEvents();
    CHECK(submitted == 1);
    auto op = tab.sent(1);
    auto expected = pass;
    expected["ports"]["previous"]["history"] = true;
    CHECK(op.at("parameters") == expected);
    document["nodes"][1]["parameters"] = expected;
    tab.commit(document, "ux3-1");

    // Its sampling: one pick, the history kept.
    QMenu sampling;
    PelicanStudio::addGraphCanvasSocketEntries(sampling, model, {}, "shade", "previous",
                                               [&] {
                                                   ++submitted;
                                                   // What the canvas does after an edit: posts it.
                                                   tab.widget->poll();
                                               });
    auto *nearest = sampling.findChild<QAction *>(QStringLiteral("pelican.graphCanvasSocket.filter.nearest"));
    REQUIRE(nearest != nullptr);
    nearest->trigger();
    QCoreApplication::processEvents();
    op = tab.sent(2);
    expected["ports"]["previous"]["sampling"] = {{"filter", "nearest"}};
    CHECK(op.at("parameters") == expected);
    document["nodes"][1]["parameters"] = expected;
    tab.commit(document, "ux3-2");

    // The mip it reads.
    QMenu mips;
    PelicanStudio::addGraphCanvasSocketEntries(mips, model, {}, "shade", "previous",
                                               [&] {
                                                   ++submitted;
                                                   // What the canvas does after an edit: posts it.
                                                   tab.widget->poll();
                                               });
    auto *mip = mips.findChild<QSpinBox *>(QStringLiteral("pelican.graphCanvasSocket.mip"));
    REQUIRE(mip != nullptr);
    REQUIRE(mip->isEnabled());
    mip->setValue(2);
    QCoreApplication::processEvents();
    op = tab.sent(3);
    expected["ports"]["previous"]["subresource"] = {{"mip", 2}};
    CHECK(op.at("parameters") == expected);
    document["nodes"][1]["parameters"] = expected;
    tab.commit(document, "ux3-3");

    // The attachment: a location, and no previous frame.
    QMenu attachment;
    PelicanStudio::addGraphCanvasSocketEntries(attachment, model, {}, "shade", "color",
                                               [&] {
                                                   ++submitted;
                                                   // What the canvas does after an edit: posts it.
                                                   tab.widget->poll();
                                               });
    auto *written = attachment.findChild<QAction *>(QStringLiteral("pelican.graphCanvasSocket.history"));
    REQUIRE(written != nullptr);
    CHECK_FALSE(written->isEnabled());
    auto *at = attachment.findChild<QSpinBox *>(QStringLiteral("pelican.graphCanvasSocket.location"));
    REQUIRE(at != nullptr);
    REQUIRE(at->isEnabled());
    at->setValue(1);
    QCoreApplication::processEvents();
    op = tab.sent(4);
    expected["ports"]["color"]["location"] = 1;
    CHECK(op.at("parameters") == expected);
    CHECK(submitted == 4);

    // A port the node does not declare offers nothing.
    QMenu none;
    PelicanStudio::addGraphCanvasSocketEntries(none, model, {}, "shade", "missing", {});
    CHECK(none.actions().isEmpty());
}

TEST_CASE("UX-6a decision 5: the Pass fields show each read image's Filter, Address and mip as "
          "combos, each pick one set_node_parameters; UX-7 decision 1: a numbered input's are "
          "picked too",
          "[devstudio][graph-canvas][scene][ux6a]") {
    (void)application();
    Ux2Tab tab{true};
    auto pass = ux2Pass();
    pass["ports"]["previous"] = {{"kind", "image"}, {"type", ux2ColorType()}, {"access", "read"}};
    pass["ports"]["input"] = {{"kind", "image"}, {"type", ux2ColorType()}, {"access", "read"},
                              {"binding", "input"}, {"input_index", 0}};
    auto document = ux2Document(ux2Image(), pass);
    document["nodes"][1]["inputs"]["previous"] = {{"node", "shade__color"}, {"port", "resource"}};
    document["nodes"][1]["inputs"]["input"] = {{"node", "shade__color"}, {"port", "resource"}};
    tab.read(document, "ux6a-0");
    tab.answer("get_graph_catalog", ux2Catalog());
    tab.widget->scene().selectNode("shade");
    QCoreApplication::processEvents();

    auto *address = tab.field<QComboBox>("pelican.graphCanvasPass.input.previous.address");
    CHECK(address->isEnabled());
    CHECK(address->currentText() == QStringLiteral("default (repeat)"));
    CHECK(tab.field<QComboBox>("pelican.graphCanvasPass.input.previous.mip")->isEnabled());
    // UX-7, decision 1 (engine RS-14): the numbered input takes them too.
    auto *numbered = tab.field<QComboBox>("pelican.graphCanvasPass.input.input.address");
    CHECK(numbered->isEnabled());
    CHECK(tab.field<QComboBox>("pelican.graphCanvasPass.input.input.mip")->isEnabled());
    // The attachment is not an input.
    CHECK(tab.widget->findChild<QComboBox *>(
              QStringLiteral("pelican.graphCanvasPass.input.color.address")) == nullptr);

    tab.pick(address, QStringLiteral("clamp_to_edge"));
    auto op = tab.sent(1);
    auto expected = pass;
    expected["ports"]["previous"]["sampling"] = {{"address", "clamp_to_edge"}};
    CHECK(op.at("parameters") == expected);
    document["nodes"][1]["parameters"] = expected;
    tab.commit(document, "ux6a-1");
    QCoreApplication::processEvents();
    // The rows read the document again: the pick is what they show.
    CHECK(tab.field<QComboBox>("pelican.graphCanvasPass.input.previous.address")->currentText() ==
          QStringLiteral("clamp_to_edge"));
    tab.pick(tab.field<QComboBox>("pelican.graphCanvasPass.input.previous.mip"),
             QStringLiteral("2"));
    op = tab.sent(2);
    expected["ports"]["previous"]["subresource"] = {{"mip", 2}};
    CHECK(op.at("parameters") == expected);
    document["nodes"][1]["parameters"] = expected;
    tab.commit(document, "ux6a-2");
    QCoreApplication::processEvents();

    // UX-7, decision 1: the numbered input's, written in the engine's form
    // on its port (the lowering puts them in input_settings).
    tab.pick(tab.field<QComboBox>("pelican.graphCanvasPass.input.input.address"),
             QStringLiteral("clamp_to_edge"));
    op = tab.sent(3);
    expected["ports"]["input"]["sampling"] = {{"address", "clamp_to_edge"}};
    CHECK(op.at("parameters") == expected);
    document["nodes"][1]["parameters"] = expected;
    tab.commit(document, "ux6a-3");
    QCoreApplication::processEvents();
    tab.pick(tab.field<QComboBox>("pelican.graphCanvasPass.input.input.filter"),
             QStringLiteral("nearest"));
    op = tab.sent(4);
    expected["ports"]["input"]["sampling"]["filter"] = "nearest";
    CHECK(op.at("parameters") == expected);
    document["nodes"][1]["parameters"] = expected;
    tab.commit(document, "ux6a-4");
    QCoreApplication::processEvents();
    tab.pick(tab.field<QComboBox>("pelican.graphCanvasPass.input.input.mip"), QStringLiteral("1"));
    op = tab.sent(5);
    expected["ports"]["input"]["subresource"] = {{"mip", 1}};
    CHECK(op.at("parameters") == expected);
    document["nodes"][1]["parameters"] = expected;
    tab.commit(document, "ux6a-5");
    QCoreApplication::processEvents();
    // The defaults picked again leave no key: the pass is byte for byte the
    // one it was.
    tab.pick(tab.field<QComboBox>("pelican.graphCanvasPass.input.input.address"), QString{});
    op = tab.sent(6);
    expected["ports"]["input"]["sampling"].erase("address");
    CHECK(op.at("parameters") == expected);
}

TEST_CASE("UX-3 decisions 4 and 5: the engine's materials, tags and variants are picked in the "
          "draw_loop's fields, the draw steps' and the material pass's; draw.override's "
          "depth_compare is a combo",
          "[devstudio][graph-canvas][scene][ux3]") {
    (void)application();
    const Json color{{"semantic", "pelican.render.color_signal@1"},
                     {"arguments", {{"reference", "scene"}, {"transfer", "linear"},
                                    {"range", "extended"}}}};
    const Json depth{{"semantic", "pelican.render.depth@1"},
                     {"arguments", {{"representation", "device"}, {"space", "projection"}}}};
    const std::string metal = "materials/m.material.json#metal";
    Json document{
        {"definitions", {{"forward_opaque_draw",
            {{"nodes", Json::array({
                 {{"id", "outline"}, {"op", "draw.variant"}, {"parameters", {{"variant", "outline"}}},
                  {"outputs", {"draws"}}},
                 {{"id", "over"}, {"op", "draw.override"},
                  {"parameters", {{"render_state", {{"cull", "none"}}}}},
                  {"inputs", {{"after", {{"node", "outline"}, {"port", "draws"}}}}},
                  {"outputs", {"draws"}}},
                 {{"id", "step"}, {"op", "draw.material"}, {"parameters", Json::object()},
                  {"inputs", {{"after", {{"node", "over"}, {"port", "draws"}}}}},
                  {"outputs", {"draws"}}}})},
             {"outputs", {{"draws", {{"node", "step"}, {"port", "draws"}}}}}}}}},
        {"nodes", Json::array({
            {{"id", "scene_color_ref"}, {"op", "render.target_ref"},
             {"parameters", {{"name", "scene_color"}, {"type", color}}}, {"outputs", {"resource"}}},
            {{"id", "scene_depth_ref"}, {"op", "render.target_ref"},
             {"parameters", {{"name", "scene_depth"}, {"type", depth}}}, {"outputs", {"resource"}}},
            {{"id", "forward_opaque_draws"}, {"op", "graph.loop"},
             {"parameters", {{"over", {{"draw_items", {{"contract", "forward_opaque_v1"}}}}},
                             {"order", {{"by", "phase_default"}}},
                             {"where", {{"tags", {{"exclude", {"crowd"}}}}}},
                             {"branches", Json::array({
                                 {{"when", {{"material", metal}}}, {"body", nullptr}},
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
    Ux2Tab tab{true};
    tab.read(document, "ux3-0");
    tab.answer("get_graph_catalog", catalog);
    tab.widget->scene().selectNode("forward_opaque_draws");
    QCoreApplication::processEvents();
    tab.widget->poll();
    REQUIRE(tab.reads("get_render_authoring_context") == 1);
    tab.answer("get_render_authoring_context", ux3Authoring());
    tab.widget->poll();
    QCoreApplication::processEvents();
    int edits = 0;

    // The where tags: picks among the engine's tags, the line out of sight.
    auto *exclude_line = tab.field<QLineEdit>("pelican.graphCanvasLoop.whereTagsExclude");
    CHECK_FALSE(exclude_line->isVisibleTo(tab.widget.get()));
    auto *exclude = tab.field<QToolButton>("pelican.graphCanvasLoop.whereTagsExcludePick");
    CHECK(exclude->isVisibleTo(tab.widget.get()));
    CHECK(exclude->text() == QStringLiteral("crowd"));
    auto *hero = tab.field<QAction>("pelican.graphCanvasLoop.whereTagsExcludePick.hero");
    hero->trigger();
    QCoreApplication::processEvents();
    auto op = tab.sent(++edits);
    CHECK(op.at("parameters").at("where") == Json{{"tags", {{"exclude", {"crowd", "hero"}}}}});
    document["nodes"][2]["parameters"] = op.at("parameters");
    tab.commit(document, "ux3-1");
    QCoreApplication::processEvents();

    // The branch's material by name, matched by its id; has_variant one of
    // the variants by name.
    auto *material = tab.field<QComboBox>("pelican.graphCanvasLoop.branch0.material");
    CHECK(material->currentText() == QStringLiteral("metal"));
    CHECK(material->currentData().toString() == QString::fromStdString(metal));
    CHECK(material->findText(QStringLiteral("pelican.builtin.transparent (built in)")) >= 0);
    auto *has_variant = tab.field<QComboBox>("pelican.graphCanvasLoop.branch0.hasVariant");
    const int outline = has_variant->findData(QStringLiteral("outline"));
    REQUIRE(outline >= 0);
    CHECK(has_variant->findData(QStringLiteral("depth_only")) >= 0);
    has_variant->setCurrentIndex(outline);
    QCoreApplication::processEvents();
    op = tab.sent(++edits);
    CHECK(op.at("parameters").at("branches").at(0).at("when") ==
          Json{{"material", metal}, {"has_variant", "outline"}});
    document["nodes"][2]["parameters"] = op.at("parameters");
    tab.commit(document, "ux3-2");
    QCoreApplication::processEvents();
    // Read back, the row shows it.
    CHECK(tab.field<QComboBox>("pelican.graphCanvasLoop.branch0.hasVariant")->currentData() ==
          QStringLiteral("outline"));

    // The material pass's variant: none, or one the materials have.
    tab.widget->scene().selectNode("forward_opaque");
    QCoreApplication::processEvents();
    auto *pass_variant = tab.field<QComboBox>("pelican.graphCanvasDraw.materialVariant");
    REQUIRE(pass_variant->isVisibleTo(tab.widget.get()));
    CHECK(ux2Items(*pass_variant) ==
          QStringList({QStringLiteral("(each material's own)"), QStringLiteral("depth_only"),
                       QStringLiteral("outline")}));
    pass_variant->setCurrentIndex(pass_variant->findData(QStringLiteral("depth_only")));
    QCoreApplication::processEvents();
    op = tab.sent(++edits);
    CHECK(op.at("parameters").at("material_variant") == "depth_only");
    document["nodes"][3]["parameters"] = op.at("parameters");
    tab.commit(document, "ux3-3");
    QCoreApplication::processEvents();

    // draw.variant: picked, not typed.
    tab.widget->scene().selectNode("forward_opaque_draw", "outline");
    QCoreApplication::processEvents();
    auto *variant = tab.field<QComboBox>("pelican.graphCanvasDraw.variant");
    CHECK_FALSE(variant->isEditable());
    CHECK(ux2Items(*variant) == QStringList({QStringLiteral("depth_only"), QStringLiteral("outline")}));

    // Decision 4: draw.override's depth_compare.
    tab.widget->scene().selectNode("forward_opaque_draw", "over");
    QCoreApplication::processEvents();
    auto *compare = tab.field<QComboBox>("pelican.graphCanvasDraw.depthCompare");
    REQUIRE(compare->isVisibleTo(tab.widget.get()));
    compare->setCurrentIndex(compare->findData(QStringLiteral("greater_equal")));
    QCoreApplication::processEvents();
    op = tab.sent(++edits);
    CHECK(op.at("scope") == "forward_opaque_draw");
    CHECK(op.at("parameters").at("render_state") ==
          Json{{"cull", "none"}, {"depth_compare", "greater_equal"}});
}

// --- UX-5b: the Graph's fullscreen pass used as the Pass Editor's is.

namespace {

// get_engine_shaders as an EL-4b engine answers it: the one vertex, the
// passthrough's variadic group, a shader whose ports come from the pass that
// runs it, one that declares two inputs and a resource port, and a compute
// shader (no list shows it).
Json ux5bEngineShaders() {
    return {{"engine_shaders",
             Json::array({
                 {{"reference", "engine://fullscreen"}, {"stage", "vertex"},
                  {"forms", {"source"}}, {"loads", "source"}, {"resource_port_include", false},
                  {"reflected_from", "source"}, {"pass_inputs", Json::array()},
                  {"variadic_inputs", nullptr}, {"resource_ports", Json::array()}},
                 {{"reference", "engine://passthrough"}, {"stage", "fragment"},
                  {"forms", {"source"}}, {"loads", "source"}, {"resource_port_include", false},
                  {"reflected_from", "source"},
                  {"pass_inputs", Json::array({{{"binding", 0}, {"type", "sampler2D"},
                                                {"name", "pelican_variadic_inputs"},
                                                {"variadic", true}}})},
                  {"variadic_inputs", {{"first", 0}, {"max", 8}}},
                  {"resource_ports", Json::array()}},
                 {{"reference", "engine://bloom_composite"}, {"stage", "fragment"},
                  {"forms", {"source"}}, {"loads", "source"}, {"resource_port_include", true},
                  {"reflection_error", "cannot open pelican_resource_ports.glsl"}},
                 {{"reference", "engine://lit"}, {"stage", "fragment"}, {"forms", {"source"}},
                  {"loads", "source"}, {"resource_port_include", false},
                  {"reflected_from", "source"},
                  {"pass_inputs",
                   Json::array({{{"binding", 0}, {"type", "sampler2D"}, {"name", "in_albedo"},
                                 {"variadic", false}},
                                {{"binding", 1}, {"type", "sampler2D"}, {"name", "in_normal"},
                                 {"variadic", false}}})},
                  {"variadic_inputs", nullptr},
                  {"resource_ports",
                   Json::array({{{"port", "lights"}, {"binding", 9}, {"type", "storage_buffer"}}})}},
                 {{"reference", "engine://cull"}, {"stage", "compute"}, {"forms", {"source"}},
                  {"loads", "source"}, {"resource_port_include", false}}})}};
}

// The ops of the `count`th edit, all of them.
Json ux5bOps(const Ux2Tab &tab, int count) {
    REQUIRE(tab.reads("edit_graph_document") == count);
    return Json::parse(tab.transport.calls[static_cast<std::size_t>(
                                               tab.transport.idOf(QStringLiteral("edit_graph_document")) - 1)]
                           .body.toStdString());
}

// A deferred material pass `mp` and the images it draws into, beside
// ux2Document's fullscreen pass `shade` and its image.
Json ux5bDeferredDocument(const Json &pass = ux2Pass()) {
    Json nodes = Json::array();
    Json inputs = Json::object();
    for (const char *socket : {"albedo", "normal", "material", "worldpos", "emissive"}) {
        const std::string id = std::string{"mp__"} + socket;
        nodes.push_back({{"id", id}, {"op", "render.image"},
                         {"parameters", {{"type", ux2ColorType()},
                                         {"format", std::string_view{socket} == "material"
                                                        ? "R8G8B8A8_UNORM"
                                                        : "R16G16B16A16_SFLOAT"},
                                         {"extent_scale", 1.0},
                                         {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}},
                         {"outputs", Json::array({"resource"})}});
        inputs[socket] = {{"node", id}, {"port", "resource"}};
    }
    nodes.push_back({{"id", "mp__depth"}, {"op", "render.image"},
                     {"parameters", {{"type", {{"semantic", "pelican.render.depth@1"},
                                               {"arguments", {{"representation", "device"},
                                                              {"space", "projection"}}}}},
                                     {"format", "D32_SFLOAT"}, {"extent_scale", 1.0},
                                     {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT"})}}},
                     {"outputs", Json::array({"resource"})}});
    inputs["depth"] = {{"node", "mp__depth"}, {"port", "resource"}};
    nodes.push_back({{"id", "mp"}, {"op", "render.material_pass"},
                     {"parameters", {{"material_contract", "deferred_geometry_v1"},
                                     {"pass_set", "main_render"}}},
                     {"inputs", inputs}, {"outputs", Json::array()}});
    const auto fullscreen = ux2Document(ux2Image(), pass);
    for (const auto &node : fullscreen.at("nodes")) nodes.push_back(node);
    return {{"definitions", Json::object()}, {"nodes", std::move(nodes)}};
}

// The catalog with the material pass and EL-4's contract table for it.
Json ux5bCatalog() {
    auto catalog = ux2Catalog();
    catalog["modules"][1]["operations"].push_back({{"op", "render.material_pass"},
                                                   {"domain", "rendering"},
                                                   {"evaluation", "direct"},
                                                   {"sink", false},
                                                   {"ports", "parameter_dependent"}});
    const Json wide = "R16G16B16A16_SFLOAT";
    catalog["material_pass_contracts"] = Json::array({
        {{"contract", "deferred_geometry_v1"},
         {"material_outputs", false},
         {"sockets", Json::array({
             {{"socket", "albedo"}, {"aspect", "color"}, {"formats", {"B8G8R8A8_SRGB", wide}}},
             {{"socket", "normal"}, {"aspect", "color"}, {"formats", {wide}}},
             {{"socket", "material"}, {"aspect", "color"}, {"formats", {"R8G8B8A8_UNORM"}}},
             {{"socket", "worldpos"}, {"aspect", "color"}, {"formats", {wide}}},
             {{"socket", "emissive"}, {"aspect", "color"}, {"formats", {"B8G8R8A8_SRGB", wide}}},
             {{"socket", "depth"}, {"aspect", "depth"}, {"formats", {"D32_SFLOAT"}}}})},
         {"combinations",
          Json::array({{{"albedo", "B8G8R8A8_SRGB"}, {"normal", wide}, {"material", "R8G8B8A8_UNORM"},
                        {"worldpos", wide}, {"emissive", "B8G8R8A8_SRGB"}, {"depth", "D32_SFLOAT"}},
                       {{"albedo", wide}, {"normal", wide}, {"material", "R8G8B8A8_UNORM"},
                        {"worldpos", wide}, {"emissive", wide}, {"depth", "D32_SFLOAT"}}})}}});
    return catalog;
}

} // namespace

TEST_CASE("UX-5b decision 1: the shader lists are the engine's (get_engine_shaders, asked only "
          "when a list is opened), a shader whose ports are connected by hand is marked, and "
          "a picked engine shader's declarations are the pass's sockets before the plan",
          "[devstudio][graph-canvas][scene][ux5b]") {
    (void)application();
    PelicanStudio::setEngineShaderList({});
    {
        Ux2Tab tab{false};
        auto pass = ux2Pass();
        tab.read(ux2Document(ux2Image(), pass), "ux5b-0");
        tab.answer("get_graph_catalog", ux2Catalog());
        tab.widget->poll();
        // Not on connecting, nor with the catalog: the engine compiles its
        // shaders to answer.
        CHECK(tab.reads("get_engine_shaders") == 0);
        tab.widget->scene().selectNode("shade");
        QCoreApplication::processEvents();
        tab.widget->poll();
        REQUIRE(tab.reads("get_engine_shaders") == 1);
        auto *fragment = tab.field<QComboBox>("pelican.graphCanvasPass.shader.fragment");
        // Until it answers, the fixed list.
        QStringList fixed;
        for (const auto &reference : PelicanStudio::fullscreenShaderChoices(
                 PelicanStudio::FullscreenShaderStage::fragment, std::nullopt))
            fixed << QString::fromStdString(reference);
        CHECK(ux2Items(*fragment) == fixed);

        tab.answer("get_engine_shaders", ux5bEngineShaders());
        tab.widget->poll();
        CHECK(tab.reads("get_engine_shaders") == 1);
        fragment = tab.field<QComboBox>("pelican.graphCanvasPass.shader.fragment");
        CHECK(ux2Items(*fragment) ==
              QStringList({QStringLiteral("engine://passthrough"),
                           QStringLiteral("engine://bloom_composite  (ports by hand)"),
                           QStringLiteral("engine://lit")}));
        const int bloom = fragment->findData(QStringLiteral("engine://bloom_composite"));
        REQUIRE(bloom >= 0);
        CHECK(fragment->itemData(bloom, Qt::ToolTipRole)
                  .toString()
                  .contains(QStringLiteral("connect its ports yourself")));
        CHECK(ux2Items(*tab.field<QComboBox>("pelican.graphCanvasPass.shader.vertex")) ==
              QStringList({QStringLiteral("engine://fullscreen")}));
        // The Pass Editor's list is the same one.
        CHECK(PelicanStudio::fullscreenShaderChoices(PelicanStudio::FullscreenShaderStage::fragment,
                                                     std::nullopt) ==
              std::vector<std::string>{"engine://passthrough", "engine://bloom_composite",
                                       "engine://lit"});

        // A picked engine shader: its sockets are its declarations at once.
        tab.pick(fragment, QStringLiteral("engine://lit"));
        const auto op = tab.sent(1);
        pass["shader"]["fragment"] = "engine://lit";
        CHECK(op.at("parameters") == pass);
        tab.commit(ux2Document(ux2Image(), pass), "ux5b-1");
        const auto *declared = tab.widget->model().passInputs({}, "shade");
        REQUIRE(declared != nullptr);
        REQUIRE(declared->declarations.size() == 2);
        CHECK(declared->declarations[1].name == "in_normal");
        CHECK(tab.field<QLabel>("pelican.graphCanvasPass.ports")
                  ->text()
                  .contains(QStringLiteral("resource port lights (storage_buffer)")));
    }
    PelicanStudio::setEngineShaderList({});
}

TEST_CASE("UX-5b decisions 2 and 3: New... in the Graph's Pass fields writes the Pass Editor's "
          "Deferred lighting template and puts it on the pass in one edit, with the scene "
          "lights, Camera position and a line from the material pass to each G-buffer input; "
          "Frame data, blend and the colour output's Load are fields",
          "[devstudio][graph-canvas][scene][ux5b]") {
    (void)application();
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_ux5b_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    {
        Ux2Tab tab{false};
        tab.widget->setProjectRoot(root);
        auto pass = ux2Pass();
        tab.read(ux5bDeferredDocument(pass), "ux5b-0");
        tab.answer("get_graph_catalog", ux5bCatalog());
        tab.widget->scene().selectNode("shade");
        QCoreApplication::processEvents();

        auto *new_shader = tab.field<QPushButton>("pelican.graphCanvasPass.newFragmentShader");
        REQUIRE(new_shader->isVisibleTo(tab.widget.get()));
        REQUIRE(new_shader->isEnabled());
        new_shader->click();
        QCoreApplication::processEvents();
        tab.widget->poll();
        QCoreApplication::processEvents();
        CHECK(new_shader->isChecked());
        REQUIRE(tab.field<QComboBox>("pelican.graphCanvasPass.shaderTemplateKind")
                    ->isVisibleTo(tab.widget.get()));
        tab.pick(tab.field<QComboBox>("pelican.graphCanvasPass.shaderTemplateKind"),
                 QStringLiteral("deferred_lighting"));
        // Named after the template, nothing typed.
        CHECK(tab.field<QLineEdit>("pelican.graphCanvasPass.shaderTemplateName")->text() ==
              QStringLiteral("deferred_lighting"));
        tab.field<QPushButton>("pelican.graphCanvasPass.createFragmentShader")->click();
        QCoreApplication::processEvents();

        // The Pass Editor's template, reading the five images in role order.
        const auto file = root / "shaders" / "deferred_lighting.frag";
        REQUIRE(std::filesystem::exists(file));
        std::stringstream source;
        source << std::ifstream{file}.rdbuf();
        std::vector<PelicanStudio::FullscreenShaderTemplateInput> inputs;
        for (const char *socket : {"albedo", "normal", "material", "worldpos", "emissive"})
            inputs.push_back({std::string{"mp__"} + socket, true});
        CHECK(source.str() == PelicanStudio::fullscreenFragmentShaderTemplate(
                                  "shade", inputs,
                                  PelicanStudio::FullscreenShaderTemplateKind::deferred_lighting));

        // One edit: the shader, the settings it needs, and the five lines.
        // (The material pass lists each output read from it: GL-1f's
        // replace_node, beside each line.)
        auto ops = ux5bOps(tab, 1);
        INFO(ops.dump());
        Json lines = Json::array();
        for (const auto &entry : ops)
            if (entry.at("op") == "set_input") lines.push_back(entry);
        REQUIRE(lines.size() == 5);
        CHECK(ops[0].at("op") == "set_node_parameters");
        CHECK(ops[0].at("node") == "shade");
        const auto &parameters = ops[0].at("parameters");
        CHECK(parameters.at("shader").at("fragment") == "project://shaders/deferred_lighting");
        CHECK(parameters.at("uses_light_data") == true);
        CHECK(parameters.at("push_constants") == "camera_position");
        int index = 0;
        for (const char *socket : {"albedo", "normal", "material", "worldpos", "emissive"}) {
            const auto &port = parameters.at("ports").at(socket);
            CHECK(port.at("binding") == "input");
            CHECK(port.at("input_index") == index);
            const auto &line = lines[static_cast<std::size_t>(index)];
            CHECK(line.at("port") == socket);
            // From the pass that writes it (GL-1f), not the image.
            CHECK(line.at("source") == Json{{"node", "mp"}, {"port", socket}});
            ++index;
        }
        pass = parameters;
        auto document = ux5bDeferredDocument(pass);
        for (const char *socket : {"albedo", "normal", "material", "worldpos", "emissive"})
            document["nodes"].back()["inputs"][socket] = {{"node", "mp"}, {"port", socket}};
        tab.commit(document, "ux5b-1");
        QCoreApplication::processEvents();

        // Decision 3: the Pass Editor's settings, as fields.
        CHECK(tab.field<QCheckBox>("pelican.graphCanvasPass.usesLightData")->isChecked());
        CHECK(tab.field<QComboBox>("pelican.graphCanvasPass.pushConstants")->currentData() ==
              QStringLiteral("camera_position"));
        tab.pick(tab.field<QComboBox>("pelican.graphCanvasPass.rasterState.blend.0"),
                 QStringLiteral("additive"));
        auto op = tab.sent(2);
        CHECK(op.at("parameters").at("raster_state") ==
              Json{{"color_attachments", Json::array({{{"blend", "additive"}}})}});
        pass = op.at("parameters");
        document["nodes"].back()["parameters"] = pass;
        tab.commit(document, "ux5b-2");
        QCoreApplication::processEvents();
        tab.pick(tab.field<QComboBox>("pelican.graphCanvasPass.outputLoad.0"),
                 QStringLiteral("load"));
        op = tab.sent(3);
        CHECK(op.at("parameters").at("color_load_op") == "load");
        CHECK(op.at("parameters").at("raster_state") == pass.at("raster_state"));
        document["nodes"].back()["parameters"] = op.at("parameters");
        tab.commit(document, "ux5b-3");
        QCoreApplication::processEvents();
        tab.field<QCheckBox>("pelican.graphCanvasPass.usesLightData")->click();
        QCoreApplication::processEvents();
        op = tab.sent(4);
        CHECK_FALSE(op.at("parameters").contains("uses_light_data"));
        CHECK(op.at("parameters").at("color_load_op") == "load");
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("UX-6a decision 1: after a Create, picking another template names the new shader "
          "after that template (a free name past the files already there); a typed name stays",
          "[devstudio][graph-canvas][scene][ux6a]") {
    (void)application();
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_ux6a_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    {
        Ux2Tab tab{false};
        tab.widget->setProjectRoot(root);
        tab.read(ux5bDeferredDocument(ux2Pass()), "ux6a-0");
        tab.answer("get_graph_catalog", ux5bCatalog());
        tab.widget->scene().selectNode("shade");
        QCoreApplication::processEvents();
        tab.field<QPushButton>("pelican.graphCanvasPass.newFragmentShader")->click();
        QCoreApplication::processEvents();
        auto *kind = tab.field<QComboBox>("pelican.graphCanvasPass.shaderTemplateKind");
        auto *name = tab.field<QLineEdit>("pelican.graphCanvasPass.shaderTemplateName");
        tab.pick(kind, QStringLiteral("deferred_lighting"));
        REQUIRE(name->text() == QStringLiteral("deferred_lighting"));
        tab.field<QPushButton>("pelican.graphCanvasPass.createFragmentShader")->click();
        QCoreApplication::processEvents();
        REQUIRE(std::filesystem::exists(root / "shaders" / "deferred_lighting.frag"));

        // GB-2's stop: the name the first Create used is not kept.
        tab.pick(kind, QStringLiteral("tone_map"));
        CHECK(name->text() == QStringLiteral("tone_map"));
        tab.pick(kind, QStringLiteral("deferred_lighting"));
        CHECK(name->text() == QStringLiteral("deferred_lighting_2"));
        // A name the author typed is theirs.
        name->setText(QStringLiteral("my_light"));
        tab.pick(kind, QStringLiteral("tone_map"));
        CHECK(name->text() == QStringLiteral("my_light"));
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("UX-5b decision 4: a material pass's image offers its contract's formats, and a new "
          "albedo format moves the other images to the combination that holds it, in one edit; "
          "depth is the engine's format_aspects, not the name",
          "[devstudio][graph-canvas][scene][ux5b]") {
    (void)application();
    const auto document = ux5bDeferredDocument();
    REQUIRE(document.at("nodes").size() == 9);
    const auto catalog = ux5bCatalog();
    REQUIRE(catalog.at("material_pass_contracts").size() == 1);
    Ux2Tab tab{true};
    tab.read(document, "ux5b-0");
    REQUIRE(tab.widget->model().nodes({}).size() == 9);
    tab.answer("get_graph_catalog", catalog);
    REQUIRE(tab.reads("get_graph_catalog") == 1);
    tab.widget->scene().selectNode("mp__albedo");
    QCoreApplication::processEvents();
    tab.widget->poll();
    auto authoring = ux2Authoring();
    // A depth format whose name does not start with D and a digit.
    authoring["render_target_vocabulary"]["formats"].push_back("X8_D24_UNORM_PACK32");
    authoring["render_target_vocabulary"]["format_aspects"] = {
        {"R8G8B8A8_UNORM", "color"}, {"R16G16B16A16_SFLOAT", "color"},
        {"B10G11R11_UFLOAT_PACK32", "color"}, {"D16_UNORM", "depth"}, {"D32_SFLOAT", "depth"},
        {"X8_D24_UNORM_PACK32", "depth"}};
    tab.answer("get_render_authoring_context", authoring);
    tab.widget->poll();
    auto *format = tab.field<QComboBox>("pelican.graphCanvasImage.format");
    CHECK(ux2Items(*format) == QStringList({QStringLiteral("B8G8R8A8_SRGB"),
                                            QStringLiteral("R16G16B16A16_SFLOAT")}));
    tab.pick(format, QStringLiteral("B8G8R8A8_SRGB"));
    const auto ops = ux5bOps(tab, 1);
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].at("node") == "mp__albedo");
    CHECK(ops[0].at("parameters").at("format") == "B8G8R8A8_SRGB");
    CHECK(ops[1].at("node") == "mp__emissive");
    CHECK(ops[1].at("parameters").at("format") == "B8G8R8A8_SRGB");

    // An image no material pass draws into: the engine's formats, its side
    // of the depth line by the aspects.
    const auto &model = tab.widget->model();
    CHECK(model.isDepthFormat("X8_D24_UNORM_PACK32"));
    CHECK_FALSE(model.isDepthFormat("R16G16B16A16_SFLOAT"));
    const auto depth_choices = model.imageFormatChoices(
        {{"type", {{"semantic", "pelican.render.depth@1"}, {"arguments", Json::object()}}}});
    CHECK(depth_choices ==
          std::vector<std::string>{"D16_UNORM", "D32_SFLOAT", "X8_D24_UNORM_PACK32"});
    CHECK(model.imageFormatChoicesFor({}, "mp__depth") == std::vector<std::string>{"D32_SFLOAT"});
}

// --- UX-5c, decision 1. A loop's output dropped on present's + input is
// named as the loop's (`steps.color`, RS-13), not as the image its last copy
// draws; the engine resolves it, and present's row shows both names from the
// plan's loop_outputs. `p[]` names no resource.

namespace {

// The plan of q19ZoneDocument's loop (two copies of `pass`, each writing its
// own canvas) and present, reading `present_reads`.
Json ux5cPlan(const Json &present_reads, const Json &loop_outputs) {
    auto plan = q19ZonePlan();
    plan["graph"] = "main_render";
    for (int element = 0; element < 2; ++element) {
        auto &copy = plan["nodes"][static_cast<std::size_t>(element)];
        copy["order"] = element;
        copy["writes"] = Json::array({"steps/" + std::to_string(element) + "/canvas"});
    }
    auto present = gl1bPass("present", 2, "project", present_reads, Json::array({"display"}));
    present["pass_inputs"] = gl1ePassthrough();
    if (!loop_outputs.is_null()) present["loop_outputs"] = loop_outputs;
    plan["nodes"].push_back(std::move(present));
    return plan;
}

// The context: what the engine composed for present's `input` (`composed`)
// beside what the root declares (`root`).
Json ux5cAuthoring(const std::vector<std::string> &composed,
                   const std::vector<std::string> &root) {
    auto context = gl1eAuthoring(root);
    context["graphs"][0]["passes"][0]["declaration"]["input"] = composed;
    return context;
}

void openUx5c(ZoneHarness &h, const Json &plan, const Json &authoring) {
    h.model.setConnected(true);
    for (int round = 0; round < 6; ++round)
        for (const auto &request : h.model.takeRpcRequests()) {
            if (request.method == "get_graph_instances")
                h.model.receiveRpcResult(request.request_id, h.snapshot().dump());
            else if (request.method == "get_graph_catalog")
                h.model.receiveRpcResult(request.request_id, h.catalog.dump());
            else if (request.method == "get_frame_plan") {
                auto reply = plan;
                reply["loops"] = q19ZoneLoops();
                h.model.receiveRpcResult(request.request_id, reply.dump());
            } else if (request.method == "get_render_authoring_context")
                h.model.receiveRpcResult(request.request_id, authoring.dump());
        }
    h.scene.setModel(&h.model);
}

} // namespace

TEST_CASE("UX-5c a loop's output onto present's + input sends <loop>.<output>, and present's row "
          "shows what the engine read for it",
          "[devstudio][graph-canvas][scene][ux5c]") {
    (void)application();
    ZoneHarness h{q19ZoneDocument()};
    addRenderOperations(h);
    std::vector<PelicanStudio::FramePlanEditRequest> sent;
    h.scene.passInputEditRequested = [&](const PelicanStudio::FramePlanEditRequest &request,
                                         std::function<PelicanStudio::FramePlanEditRequest()>) {
        sent.push_back(request);
    };

    SECTION("the drop names the loop's output, whatever the plan expanded") {
        openUx5c(h, ux5cPlan(Json::array(), Json()), ux5cAuthoring({}, {}));
        CHECK(h.model.outputResource("steps", "color") == "steps.color");
        // p[] is one value per element: no pass's input names it.
        CHECK(h.model.outputResource("steps", "color[]").empty());
        // Not the loop's input of the same name either (UX-5a).
        CHECK(h.model.outputResource("steps", "target").empty());
        QGraphicsItem *from = socketItem(h.scene, QStringLiteral("steps"), QStringLiteral("color"),
                                         QStringLiteral("output"));
        QGraphicsItem *slot = passSocket(h.scene, QStringLiteral("present"), QStringLiteral("in:+"));
        REQUIRE(from != nullptr);
        REQUIRE(slot != nullptr);
        drag(h.scene, from->sceneBoundingRect().center(), slot->sceneBoundingRect().center());
        REQUIRE(sent.size() == 1);
        CHECK(sent[0].method == "edit_preset_pass");
        CHECK(sent[0].params.at("input") == Json::array({"steps.color"}));
    }

    SECTION("present reading it: the row says both names, and the line is from the loop") {
        const Json resolved = Json::array(
            {{{"reference", "steps.color"}, {"resource", "steps/1/canvas"}}});
        openUx5c(h, ux5cPlan(Json::array({"steps/1/canvas"}), resolved),
                 ux5cAuthoring({"steps/1/canvas"}, {"steps.color"}));
        const auto sockets = h.scene.passInputSockets("present");
        REQUIRE(sockets.has_value());
        REQUIRE(sockets->sockets.size() == 2);
        // GL-5e, decision 2: present draws with engine://passthrough, which
        // shows this input -- marked as the Frame Plan marks it.
        CHECK(sockets->sockets[0].label == "← steps.color → steps/1/canvas (shown)");
        const auto lines = itemsOfKind(h.scene, PelicanStudio::GraphCanvasPassInputEdgeItem);
        REQUIRE(lines.size() == 1);
        CHECK(lines.front()->data(PelicanStudio::NodeGraphFromNameRole).toString() ==
              QStringLiteral("steps"));
        CHECK(lines.front()->data(PelicanStudio::NodeGraphToNameRole).toString() ==
              QStringLiteral("present"));

        // The Frame Plan tab's rows are the same function's; a plan without
        // loop_outputs (an engine before RS-13) shows the resource alone.
        const auto bare = PelicanStudio::framePlanInputSockets(
            h.model.passInputPlan(), h.model.passInputPlan().nodes.back(),
            std::vector<std::string>{"steps/1/canvas"});
        REQUIRE_FALSE(bare.sockets.empty());
        CHECK(bare.sockets[0].label == "← steps.color → steps/1/canvas");
        auto older = h.model.passInputPlan();
        older.nodes.back().loop_outputs.clear();
        const auto unresolved = PelicanStudio::framePlanInputSockets(
            older, older.nodes.back(), std::vector<std::string>{"steps/1/canvas"});
        REQUIRE_FALSE(unresolved.sockets.empty());
        CHECK(unresolved.sockets[0].label == "← steps/1/canvas");
    }
}

// --- UX-5d. A body's image sized by an expression, a ray tracing pipeline
// from the palette, and the phrase a refusal gets only when a graph pass
// writes the image.

// Decision 1: behind the Size row's f button the size is {"$expr": ...};
// "Half each element" puts the usual form in without a key typed.
TEST_CASE("UX-5d a render.image's size is an expression behind f, and Half each element writes "
          "1 / pow2(element + 1)",
          "[devstudio][graph-canvas][scene][ux5d]") {
    (void)application();
    Ux2Tab tab{true};
    auto image = ux2Image();
    tab.read(ux2Document(image), "ux5d-0");
    tab.answer("get_graph_catalog", ux2Catalog());
    tab.widget->scene().selectNode("shade__color");
    QCoreApplication::processEvents();
    tab.widget->poll();
    tab.answer("get_render_authoring_context", ux2Authoring());

    auto *toggle = tab.field<QToolButton>("pelican.graphCanvasImage.sizeExpressionToggle");
    auto *expression = tab.field<QLineEdit>("pelican.graphCanvasImage.extentScaleExpression");
    auto *half = tab.field<QPushButton>("pelican.graphCanvasImage.sizeHalfEachElement");
    CHECK_FALSE(toggle->isChecked());
    CHECK_FALSE(expression->isVisible());
    CHECK_FALSE(half->isVisible());
    toggle->click();
    QCoreApplication::processEvents();
    CHECK(expression->isVisible());
    CHECK(half->isVisible());
    // Nothing is sent until there is an expression.
    CHECK(tab.reads("edit_graph_document") == 0);

    half->click();
    QCoreApplication::processEvents();
    auto op = tab.sent(1);
    image["extent_scale"] = Json{{"$expr", "1 / pow2(element + 1)"}};
    CHECK(op.at("parameters") == image);
    tab.commit(ux2Document(image), "ux5d-1");
    // Read back: the f is on and the expression is shown, not a number.
    toggle = tab.field<QToolButton>("pelican.graphCanvasImage.sizeExpressionToggle");
    CHECK(toggle->isChecked());
    CHECK(tab.field<QLineEdit>("pelican.graphCanvasImage.extentScaleExpression")->text() ==
          QStringLiteral("1 / pow2(element + 1)"));
    CHECK_FALSE(tab.field<QDoubleSpinBox>("pelican.graphCanvasImage.extentScale")->isVisible());

    // Double each element, and a fixed size takes the forms in pixels.
    tab.field<QPushButton>("pelican.graphCanvasImage.sizeDoubleEachElement")->click();
    QCoreApplication::processEvents();
    op = tab.sent(2);
    image["extent_scale"] = Json{{"$expr", "pow2(element)"}};
    CHECK(op.at("parameters") == image);
    tab.commit(ux2Document(image), "ux5d-2");
    tab.field<QRadioButton>("pelican.graphCanvasImage.sizeFixed")->click();
    QCoreApplication::processEvents();
    op = tab.sent(3);
    image["extent_scale"] = 1.0;
    image["width"] = 256;
    image["height"] = 256;
    CHECK(op.at("parameters") == image);
    tab.commit(ux2Document(image), "ux5d-3");
    tab.field<QPushButton>("pelican.graphCanvasImage.sizeHalfEachElement")->click();
    QCoreApplication::processEvents();
    op = tab.sent(4);
    image["width"] = Json{{"$expr", "256 / pow2(element)"}};
    image["height"] = Json{{"$expr", "256 / pow2(element)"}};
    CHECK(op.at("parameters") == image);

    // f off: the numbers the fields hold.
    tab.commit(ux2Document(image), "ux5d-4");
    tab.field<QToolButton>("pelican.graphCanvasImage.sizeExpressionToggle")->click();
    QCoreApplication::processEvents();
    op = tab.sent(5);
    image["width"] = 256;
    image["height"] = 256;
    CHECK(op.at("parameters") == image);
}

namespace {

// get_engine_shaders' entries of a ray tracing pipeline, as EL-4b lists them.
Json ux5dEngineShaders() {
    const auto port = [](const char *name, int binding, const char *type) {
        return Json{{"port", name}, {"binding", binding}, {"type", type}};
    };
    // EL-4c: a storage image's format, as the engine read it from the shader.
    auto mask = port("shadow_mask", 2, "storage_image");
    mask["format"] = "R8_UNORM";
    const Json ports = Json::array({port("world_position", 0, "sampled_image"),
                                    port("normal", 1, "sampled_image"), mask});
    return Json::array({{{"reference", "engine://passthrough"}, {"stage", "fragment"}},
                        {{"reference", "engine://rt_mask"}, {"stage", "raygen"},
                         {"resource_ports", ports}},
                        {{"reference", "engine://rt_mask"}, {"stage", "miss"},
                         {"resource_ports", Json::array()}},
                        {{"reference", "engine://rt_mask"}, {"stage", "closesthit"},
                         {"resource_ports", Json::array()}},
                        // A raygen with no miss is no pipeline.
                        {{"reference", "engine://half"}, {"stage", "raygen"}}});
}

} // namespace

// Decision 2: the palette's render.compute traces with an engine pipeline:
// its ports from the shader's resource ports, the image it writes beside it,
// the images it samples read from the scope's images of the same name, and
// the rays as many as the written image's pixels.
TEST_CASE("UX-5d a render.compute from the palette traces with an engine ray tracing pipeline",
          "[devstudio][graph-canvas][scene][ux5d]") {
    (void)application();
    const auto image = [](const char *id) {
        return Json{{"id", id}, {"op", "render.image"},
                    {"parameters", {{"type", {{"semantic", "pelican.render.color_signal@1"}, {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}}}, {"format", "R16G16B16A16_SFLOAT"},
                                    {"extent_scale", 1.0}}},
                    {"outputs", {"resource"}}};
    };
    const Json document{{"schema", "pelican.graph"}, {"version", 1}, {"outputs", Json::object()},
                        {"definitions", Json::object()},
                        {"nodes", Json::array({image("gb__worldpos"), image("gb__normal"),
                                               image("gb__albedo")})}};
    const Json compute{{"op", "render.compute"}, {"domain", "rendering"}, {"evaluation", "direct"},
                       {"sink", false}, {"ports", "parameter_dependent"}};
    const auto offered = [](GraphCanvasModel &model) {
        return std::ranges::any_of(model.paletteOperationsIn({}, "render.compute", {}),
                                   [](const auto &entry) { return entry.op == "render.compute"; });
    };
    // Before the engine's list is known a compute task is offered: picking
    // it asks for the list. A list with no pipeline, and no project compute
    // shader, leaves nothing to run.
    ZoneHarness unknown{document};
    unknown.catalog["modules"][1]["operations"].push_back(compute);
    unknown.open();
    CHECK(offered(unknown.model));
    ZoneHarness bare{document};
    bare.catalog["modules"][1]["operations"].push_back(compute);
    bare.catalog["engine_shaders"] = Json::array();
    bare.open();
    CHECK_FALSE(offered(bare.model));

    ZoneHarness h{document};
    h.catalog["modules"][1]["operations"].push_back(compute);
    h.catalog["engine_shaders"] = ux5dEngineShaders();
    h.open();
    const auto pipelines = h.model.rayTracingPipelines();
    REQUIRE(pipelines.size() == 1);
    CHECK(pipelines[0].reference == "engine://rt_mask");
    CHECK(offered(h.model));

    REQUIRE(h.scene.addFromPalette({}, "render.compute", {},
                                   Json{{"ray_tracing", "engine://rt_mask"}}));
    const auto ops = h.commit(false);
    REQUIRE(ops.size() == 2);
    const auto &written = ops[0].at("node");
    CHECK(written.at("op") == "render.image");
    CHECK(written.at("parameters").at("usage") == Json::array({"STORAGE", "SAMPLED"}));
    CHECK(written.at("parameters").at("format") == "R8_UNORM");
    const auto &task = ops[1].at("node");
    CHECK(task.at("op") == "render.compute");
    CHECK(task.at("parameters").at("ray_tracing") ==
          Json{{"raygen", "engine://rt_mask"}, {"miss", "engine://rt_mask"},
               {"closesthit", "engine://rt_mask"}});
    CHECK(task.at("parameters").at("dispatch") ==
          Json{{"rays_from", {{"port", "shadow_mask"}}}});
    const auto &ports = task.at("parameters").at("ports");
    CHECK(ports.at("world_position").at("access") == "read");
    CHECK(ports.at("world_position").at("binding") == "sampled");
    CHECK(ports.at("shadow_mask").at("access") == "write");
    CHECK(ports.at("shadow_mask").at("binding") == "storage");
    CHECK(task.at("inputs").at("world_position") == Json{{"node", "gb__worldpos"}, {"port", "resource"}});
    CHECK(task.at("inputs").at("normal") == Json{{"node", "gb__normal"}, {"port", "resource"}});
    CHECK(task.at("inputs").at("shadow_mask") ==
          Json{{"node", written.at("id")}, {"port", "resource"}});
    CHECK(task.at("outputs") == Json::array({"shadow_mask"}));

    // EL-4c: get_engine_shaders answers at once. "building" is asked again
    // half a second later (from poll); "ready" carries the list; "failed"
    // says why on the notice line.
    const auto shadersAsked = [](ZoneHarness &harness) -> std::uint64_t {
        for (const auto &request : harness.model.takeRpcRequests())
            if (request.method == "get_engine_shaders") return request.request_id;
        return 0;
    };
    ZoneHarness later{document};
    later.open();
    later.model.askEngineShaders();
    auto asked = shadersAsked(later);
    REQUIRE(asked != 0);
    later.model.receiveRpcResult(asked, Json{{"status", "building"}}.dump());
    CHECK_FALSE(later.model.engineShadersKnown());
    later.model.poll();
    CHECK(shadersAsked(later) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds{600});
    later.model.poll();
    asked = shadersAsked(later);
    REQUIRE(asked != 0);
    later.model.receiveRpcResult(
        asked, Json{{"status", "ready"}, {"engine_shaders", ux5dEngineShaders()}}.dump());
    CHECK(later.model.engineShadersKnown());
    CHECK(later.model.rayTracingPipelines().size() == 1);

    ZoneHarness failed{document};
    failed.open();
    failed.model.askEngineShaders();
    asked = shadersAsked(failed);
    REQUIRE(asked != 0);
    failed.model.receiveRpcResult(asked, Json{{"status", "failed"}, {"error", "no compiler"}}.dump());
    CHECK(failed.model.engineShadersKnown());
    CHECK(failed.model.rayTracingPipelines().empty());
    CHECK(failed.model.editNotice().kind == PelicanStudio::InspectorNoticeKind::Error);
    CHECK(failed.model.editNotice().message.find("no compiler") != std::string::npos);
}

// --- EL-5s. A project's compute shader placed from the palette takes its
// ports from its declarations (get_shader_interface, EL-5).

namespace {

Json el5sImage(const char *id, const char *format = "R16G16B16A16_SFLOAT") {
    return Json{{"id", id}, {"op", "render.image"},
                {"parameters", {{"type", {{"semantic", "pelican.render.color_signal@1"},
                                          {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}}},
                                {"format", format}, {"extent_scale", 1.0},
                                {"usage", Json::array({"STORAGE", "SAMPLED"})}}},
                {"outputs", {"resource"}}};
}

Json el5sComputeCatalogEntry() {
    return Json{{"op", "render.compute"}, {"domain", "rendering"}, {"evaluation", "direct"},
                {"sink", false}, {"ports", "parameter_dependent"}};
}

// The get_shader_interface questions in the outbox, by request id; the rest
// is left for the harness.
std::vector<std::pair<std::uint64_t, Json>> el5sAsked(ZoneHarness &harness) {
    std::vector<std::pair<std::uint64_t, Json>> asked;
    for (const auto &request : harness.model.takeRpcRequests())
        if (request.method == "get_shader_interface") asked.emplace_back(request.request_id, request.params);
    return asked;
}

Json el5sReady(const std::string &reference, Json ports, Json local_size = Json::array({8, 8, 1})) {
    return Json{{"status", "ready"},
                {"reference", reference},
                {"stages", Json::array({Json{{"reference", reference},
                                             {"stage", "compute"},
                                             {"resource_ports", std::move(ports)},
                                             {"local_size", std::move(local_size)}}})}};
}

} // namespace

// Decision 1: the palette's render.compute with a project .comp. The engine is
// asked (building is asked again half a second later); the ready answer
// places the task with its written image (the shader's format, STORAGE and
// SAMPLED), connected, dispatched from that image's size, in one edit; the
// sampled image it declares is an empty socket, declared when a line lands.
TEST_CASE("EL-5s a project compute shader from the palette arrives with its ports and images",
          "[devstudio][graph-canvas][scene][el5s]") {
    (void)application();
    const std::string reference = "project://shaders/ocean_h0";
    const Json document{{"schema", "pelican.graph"}, {"version", 1}, {"outputs", Json::object()},
                        {"definitions", Json::object()},
                        {"nodes", Json::array({el5sImage("spectrum")})}};
    ZoneHarness h{document};
    h.catalog["modules"][1]["operations"].push_back(el5sComputeCatalogEntry());
    h.catalog["engine_shaders"] = Json::array();
    h.open();
    h.model.setComputeShaderChoices({reference});
    // Unknown, the palette's old placement stands (no ports, one group).
    CHECK(h.model.addNodeOpsIn({}, "render.compute", {}, Json{{"shader", reference}})
              .back().at("node").at("parameters").at("dispatch") ==
          Json{{"groups", Json::array({1, 1, 1})}});

    h.model.askShaderInterface(reference);
    auto asked = el5sAsked(h);
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].second == Json{{"reference", reference}});
    h.model.receiveRpcResult(asked[0].first, Json{{"status", "building"}}.dump());
    CHECK(h.model.shaderInterfaceWaiting(reference));
    h.model.poll();
    CHECK(el5sAsked(h).empty());
    std::this_thread::sleep_for(std::chrono::milliseconds{600});
    h.model.poll();
    asked = el5sAsked(h);
    REQUIRE(asked.size() == 1);
    h.model.receiveRpcResult(
        asked[0].first,
        el5sReady(reference, Json::array({Json{{"port", "h0"}, {"binding", 0}, {"type", "storage_image"},
                                               {"format", "R32G32B32A32_SFLOAT"}},
                                          Json{{"port", "previous"}, {"binding", 1}, {"type", "sampled_image"}}}))
            .dump());
    REQUIRE(h.model.shaderInterface(reference));
    CHECK(h.model.shaderInterface(reference)->ready());
    CHECK_FALSE(h.model.shaderInterfaceWaiting(reference));

    REQUIRE(h.scene.addFromPalette({}, "render.compute", {}, Json{{"shader", reference}}));
    const auto ops = h.commit();
    REQUIRE(ops.size() == 2);
    const auto &image = ops[0].at("node");
    CHECK(image.at("op") == "render.image");
    CHECK(image.at("id") == "render_compute__h0");
    CHECK(image.at("parameters").at("format") == "R32G32B32A32_SFLOAT");
    CHECK(image.at("parameters").at("usage") == Json::array({"STORAGE", "SAMPLED"}));
    const auto &task = ops[1].at("node");
    CHECK(task.at("op") == "render.compute");
    CHECK(task.at("parameters").at("shader") == reference);
    CHECK(task.at("parameters").at("dispatch") == Json{{"groups_from", {{"port", "h0"}}}});
    CHECK(task.at("parameters").at("ports").at("h0").at("access") == "write");
    CHECK(task.at("parameters").at("ports").at("h0").at("binding") == "storage");
    CHECK_FALSE(task.at("parameters").at("ports").contains("previous"));
    CHECK(task.at("inputs").at("h0") == Json{{"node", "render_compute__h0"}, {"port", "resource"}});
    CHECK(task.at("outputs") == Json::array({"h0"}));

    // The sampled image: an empty socket, declared as the line lands.
    CHECK(h.model.computeOpenSockets({}, "render_compute") == std::vector<std::string>{"previous"});
    const auto drawn = h.model.nodes({});
    const auto compute = std::ranges::find(drawn, std::string{"render_compute"}, &PelicanStudio::GraphCanvasNode::id);
    REQUIRE(compute != drawn.end());
    const auto socket = std::ranges::find(compute->inputs, std::string{"previous"}, &PelicanStudio::GraphCanvasPort::port);
    REQUIRE(socket != compute->inputs.end());
    CHECK_FALSE(socket->connected);
    REQUIRE(h.model.connectInput({}, "render_compute", "previous", "spectrum", "resource"));
    const auto connect = h.commit();
    REQUIRE(connect.size() == 2);
    CHECK(connect[0].at("op") == "set_node_parameters");
    CHECK(connect[0].at("parameters").at("ports").at("previous").at("binding") == "sampled");
    CHECK(connect[0].at("parameters").at("ports").at("previous").at("access") == "read");
    CHECK(connect[1] == Json{{"op", "set_input"}, {"scope", "root"}, {"node", "render_compute"},
                             {"port", "previous"},
                             {"source", {{"node", "spectrum"}, {"port", "resource"}}}});
    CHECK(h.model.computeOpenSockets({}, "render_compute").empty());
}

// Decision 2: the Shader picked again. Asked first when the engine has not
// read it; sent from poll once it has: the port of the same name keeps its
// line, a new one gets its image, and a port the shader dropped loses the
// lines read from it.
TEST_CASE("EL-5s a compute node's Shader picked again makes its ports the new shader's",
          "[devstudio][graph-canvas][scene][el5s]") {
    (void)application();
    const Json write{{"kind", "image"},
                     {"type", {{"semantic", "pelican.render.color_signal@1"},
                               {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}}},
                     {"access", "write"}, {"binding", "storage"}};
    Json read = write;
    read["access"] = "read";
    read["binding"] = "sampled";
    const Json document{
        {"schema", "pelican.graph"}, {"version", 1}, {"outputs", Json::object()},
        {"definitions", Json::object()},
        {"nodes", Json::array({el5sImage("task__keep"), el5sImage("task__gone"),
                               Json{{"id", "task"}, {"op", "render.compute"},
                                    {"parameters", {{"shader", "project://shaders/a"},
                                                    {"dispatch", {{"groups_from", {{"port", "keep"}}}}},
                                                    {"ports", {{"keep", write}, {"gone", write}}}}},
                                    {"inputs", {{"keep", {{"node", "task__keep"}, {"port", "resource"}}},
                                                {"gone", {{"node", "task__gone"}, {"port", "resource"}}}}},
                                    {"outputs", {"keep", "gone"}}},
                               Json{{"id", "reader"}, {"op", "render.compute"},
                                    {"parameters", {{"shader", "project://shaders/r"},
                                                    {"dispatch", {{"groups", {1, 1, 1}}}},
                                                    {"ports", {{"from_gone", read}}}}},
                                    {"inputs", {{"from_gone", {{"node", "task"}, {"port", "gone"}}}}},
                                    {"outputs", Json::array()}}})}};
    ZoneHarness h{document};
    h.catalog["modules"][1]["operations"].push_back(el5sComputeCatalogEntry());
    h.open();
    const std::string reference = "project://shaders/b";
    REQUIRE(h.model.setComputeShader({}, "task", reference));
    auto asked = el5sAsked(h);
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].second.at("reference") == reference);
    h.model.receiveRpcResult(
        asked[0].first,
        el5sReady(reference, Json::array({Json{{"port", "keep"}, {"binding", 0}, {"type", "storage_image"}},
                                          Json{{"port", "fresh"}, {"binding", 1}, {"type", "storage_image"},
                                               {"format", "R8_UNORM"}}}))
            .dump());
    h.model.poll();
    const auto ops = h.commit(false);
    REQUIRE(ops.size() == 3);
    CHECK(ops[0] == Json{{"op", "set_input"}, {"scope", "root"}, {"node", "reader"}, {"port", "from_gone"},
                         {"source", nullptr}});
    CHECK(ops[1].at("op") == "add_node");
    CHECK(ops[1].at("node").at("id") == "task__fresh");
    CHECK(ops[1].at("node").at("parameters").at("format") == "R8_UNORM");
    CHECK(ops[2].at("op") == "replace_node");
    const auto &task = ops[2].at("node");
    CHECK(task.at("parameters").at("shader") == reference);
    CHECK(task.at("parameters").at("ports").at("keep") == write);
    CHECK(task.at("parameters").at("ports").contains("fresh"));
    CHECK_FALSE(task.at("parameters").at("ports").contains("gone"));
    CHECK(task.at("inputs") ==
          Json{{"keep", {{"node", "task__keep"}, {"port", "resource"}}},
               {"fresh", {{"node", "task__fresh"}, {"port", "resource"}}}});
    CHECK(task.at("outputs") == Json::array({"keep", "fresh"}));
    CHECK(task.at("parameters").at("dispatch") == Json{{"groups_from", {{"port", "keep"}}}});
}

// Decision 3: a stage the engine could not compile on its own is said in the
// node's fields in one sentence, with Copy message for the whole message;
// a message that reads like a missing define says the reflection passes none.
TEST_CASE("EL-5s a compute shader's reflection error is said in its fields with Copy message",
          "[devstudio][graph-canvas][scene][el5s]") {
    (void)application();
    const std::string reference = "project://shaders/needs_define";
    const Json document{{"schema", "pelican.graph"}, {"version", 1}, {"outputs", Json::object()},
                        {"definitions", Json::object()},
                        {"nodes", Json::array({Json{{"id", "task"}, {"op", "render.compute"},
                                                    {"parameters", {{"shader", reference},
                                                                    {"dispatch", {{"groups", {1, 1, 1}}}},
                                                                    {"ports", Json::object()}}},
                                                    {"outputs", Json::array()}}})}};
    ZoneHarness h{document};
    h.catalog["modules"][1]["operations"].push_back(el5sComputeCatalogEntry());
    h.open();
    const std::string message =
        "project://shaders/needs_define.comp:9: error: 'TILE' : undeclared identifier\nERROR: 1 compilation errors";
    PelicanStudio::GraphCanvasComputeFields fields{h.model, {}};
    const auto drawn = h.model.nodes({});
    REQUIRE(drawn.size() == 1);
    fields.showNode(&drawn[0], {});
    // Shown, it asks for the shader's interface.
    auto asked = el5sAsked(h);
    REQUIRE(asked.size() == 1);
    auto *status = fields.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasCompute.status"));
    auto *copy = fields.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasCompute.copyMessage"));
    REQUIRE(status != nullptr);
    REQUIRE(copy != nullptr);
    CHECK(copy->isHidden());
    h.model.receiveRpcResult(asked[0].first,
                             Json{{"status", "ready"},
                                  {"reference", reference},
                                  {"stages", Json::array({Json{{"reference", reference},
                                                               {"stage", "compute"},
                                                               {"reflection_error", message}}})}}
                                 .dump());
    fields.showNode(&drawn[0], {});
    CHECK_FALSE(status->isHidden());
    CHECK(status->text().contains(QStringLiteral("'TILE' : undeclared identifier")));
    CHECK(status->text().contains(QString::fromUtf8(GraphCanvasModel::kReflectionDefinesHint.data(),
                                                    static_cast<int>(GraphCanvasModel::kReflectionDefinesHint.size()))));
    CHECK_FALSE(status->text().contains(QStringLiteral("compilation errors")));
    CHECK_FALSE(copy->isHidden());
    copy->click();
    CHECK(QGuiApplication::clipboard()->text() == QString::fromStdString(message));
    // A message that is not about a missing name says nothing of defines.
    CHECK(GraphCanvasModel::reflectionErrorSentence(reference, "x.comp:3: error: syntax error")
              .find(GraphCanvasModel::kReflectionDefinesHint) == std::string::npos);
}

// Decision 4: the shader file changed. Every compute node's stem is asked
// again with refresh; an answer whose ports differ is a change and a notice
// line ("+ new / − removed"); Update ports makes the ports the shader's.
TEST_CASE("EL-5s a compute shader whose ports changed on disk is said, and Update ports follows it",
          "[devstudio][graph-canvas][scene][el5s]") {
    (void)application();
    const std::string reference = "project://shaders/a";
    const Json write{{"kind", "image"},
                     {"type", {{"semantic", "pelican.render.color_signal@1"},
                               {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}}},
                     {"access", "write"}, {"binding", "storage"}};
    const Json document{{"schema", "pelican.graph"}, {"version", 1}, {"outputs", Json::object()},
                        {"definitions", Json::object()},
                        {"nodes", Json::array({el5sImage("task__keep"),
                                               Json{{"id", "task"}, {"op", "render.compute"},
                                                    {"parameters", {{"shader", reference},
                                                                    {"dispatch", {{"groups_from", {{"port", "keep"}}}}},
                                                                    {"ports", {{"keep", write}}}}},
                                                    {"inputs", {{"keep", {{"node", "task__keep"}, {"port", "resource"}}}}},
                                                    {"outputs", {"keep"}}}})}};
    ZoneHarness h{document};
    h.catalog["modules"][1]["operations"].push_back(el5sComputeCatalogEntry());
    h.open();
    const auto keep = Json{{"port", "keep"}, {"binding", 0}, {"type", "storage_image"}};
    h.model.askShaderInterface(reference);
    auto asked = el5sAsked(h);
    REQUIRE(asked.size() == 1);
    h.model.receiveRpcResult(asked[0].first, el5sReady(reference, Json::array({keep})).dump());
    // Nothing changed: no notice.
    h.model.refreshComputeShaderInterfaces();
    asked = el5sAsked(h);
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].second == Json{{"reference", reference}, {"refresh", true}});
    h.model.receiveRpcResult(asked[0].first, el5sReady(reference, Json::array({keep})).dump());
    CHECK(h.model.computePortChanges().empty());
    // The file now declares one more port: building first, then the answer.
    h.model.refreshComputeShaderInterfaces();
    asked = el5sAsked(h);
    REQUIRE(asked.size() == 1);
    h.model.receiveRpcResult(asked[0].first, Json{{"status", "building"}}.dump());
    CHECK(h.model.computePortChanges().empty());
    std::this_thread::sleep_for(std::chrono::milliseconds{600});
    h.model.poll();
    asked = el5sAsked(h);
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].second == Json{{"reference", reference}});
    h.model.receiveRpcResult(
        asked[0].first,
        el5sReady(reference, Json::array({keep, Json{{"port", "foam"}, {"binding", 1}, {"type", "storage_image"}}}))
            .dump());
    REQUIRE(h.model.computePortChanges().size() == 1);
    CHECK(h.model.computePortChanges()[0] ==
          PelicanStudio::GraphCanvasPortChange{"", "task", reference, {"foam"}, {}});
    CHECK(h.model.editNotice().message == reference + " changed its ports: + foam");
    // Nothing is written until Update ports.
    CHECK(h.answerReads().empty());
    REQUIRE(h.model.updateComputePorts());
    const auto ops = h.commit(false);
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].at("node").at("id") == "task__foam");
    CHECK(ops[1].at("op") == "replace_node");
    CHECK(ops[1].at("node").at("outputs") == Json::array({"keep", "foam"}));
    CHECK(h.model.computePortChanges().empty());
}

// Decision 3: "the graph pass that draws it runs after this pass" only when a
// graph node's pass writes the image the refusal names.
TEST_CASE("UX-5d a refusal names a graph pass's image only when a graph pass writes it",
          "[devstudio][graph-canvas][scene][ux5d]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    openGl1e(model, scene, {});
    // gl1ePlan: the graph's `shade` writes shade__color.
    CHECK(model.graphWritesResourceNamedIn(
        "Pass input target is not produced as an earlier output: shade__color in pass: present"));
    CHECK(model.graphWritesResourceNamedIn(
        "Pass input target is not produced as an earlier output: present reads shade__color."));
    // A feature's task writes the mask; the plan knows no writer of `nothing`.
    auto plan = gl1ePlan({});
    auto task = gl1bPass("rt_mask", 2, "feature:rt_mask", Json::array(), Json::array({"rt_mask"}));
    task["kind"] = "compute";
    task["provider_feature"] = "rt_mask";
    plan["nodes"].push_back(task);
    for (const auto &request : model.takeRpcRequests()) (void)request;
    model.refresh();
    for (int round = 0; round < 3; ++round)
        for (const auto &request : model.takeRpcRequests()) {
            if (request.method == "get_graph_instances")
                model.receiveRpcResult(request.request_id, gl1eRead().dump());
            else if (request.method == "get_frame_plan")
                model.receiveRpcResult(request.request_id, plan.dump());
            else if (request.method == "get_render_authoring_context")
                model.receiveRpcResult(request.request_id, gl1eAuthoring({}).dump());
        }
    REQUIRE(model.planPasses().size() == 3);
    CHECK_FALSE(model.graphWritesResourceNamedIn(
        "Pass input target is not produced as an earlier output: rt_mask in pass: present"));
    CHECK_FALSE(model.graphWritesResourceNamedIn(
        "Pass input target is not produced as an earlier output: nothing in pass: present"));
    // present (the rendering section's) writes display: no graph pass either.
    CHECK_FALSE(model.graphWritesResourceNamedIn("display in pass: output_transform"));
}

// --- UX-7: what GB-3 typed or could not write.

namespace {

// A raster pass `sea` writing the image `shade__color`, as the palette adds
// one, in a document the engine's graph validation reads.
Json ux7RasterPass() {
    return {{"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}},
            {"ports", {{"color", {{"kind", "image"}, {"type", ux2ColorType()}, {"access", "write"},
                                  {"attachment", "color"}, {"location", 0}}}}},
            {"draw", {{"vertex_count", 3}}}};
}

Json ux7RasterDocument(const Json &pass = ux7RasterPass()) {
    auto document = ux3Document();
    document["nodes"][1] = {{"id", "sea"}, {"op", "render.raster"}, {"parameters", pass},
                            {"inputs", {{"color", {{"node", "shade__color"}, {"port", "resource"}}}}},
                            {"outputs", Json::array({"color"})}};
    return document;
}

Json ux7Catalog() {
    auto catalog = ux2Catalog();
    catalog["modules"][1]["operations"].push_back({{"op", "render.raster"},
                                                   {"domain", "rendering"},
                                                   {"evaluation", "direct"},
                                                   {"sink", false},
                                                   {"ports", "parameter_dependent"}});
    return catalog;
}

// The ops of the `count`th edit, however many there are.
Json ux7Ops(const Ux2Tab &tab, int count) {
    REQUIRE(tab.reads("edit_graph_document") == count);
    return Json::parse(tab.transport.calls[static_cast<std::size_t>(
                                               tab.transport.idOf(QStringLiteral("edit_graph_document")) - 1)]
                           .body.toStdString());
}

void ux7Click(QAbstractButton *button) {
    button->click();
    QCoreApplication::processEvents();
}

} // namespace

TEST_CASE("UX-7 decision 2: a pass's Defines rows are its shader.defines, written in one "
          "set_node_parameters; a row the engine refuses is red and not sent",
          "[devstudio][graph-canvas][scene][ux7]") {
    (void)application();
    Ux2Tab tab{false};
    auto pass = ux2Pass();
    auto document = ux2Document(ux2Image(), pass);
    tab.read(document, "ux7-d-0");
    tab.answer("get_graph_catalog", ux2Catalog());
    tab.widget->scene().selectNode("shade");
    QCoreApplication::processEvents();
    auto *table = tab.field<QTableWidget>("pelican.graphCanvasPass.shaderDefines.table");
    REQUIRE(table->isVisible());
    CHECK(table->rowCount() == 0);

    // Add puts a row; nothing is sent for an empty one.
    ux7Click(tab.field<QPushButton>("pelican.graphCanvasPass.shaderDefines.add"));
    REQUIRE(table->rowCount() == 1);
    CHECK(tab.reads("edit_graph_document") == 0);
    // The value first: a row with no name yet is refused, and not sent.
    table->item(0, 1)->setText(QStringLiteral("3"));
    QCoreApplication::processEvents();
    CHECK(tab.reads("edit_graph_document") == 0);
    table->item(0, 0)->setText(QStringLiteral("OCEAN_STAGE"));
    QCoreApplication::processEvents();
    auto op = tab.sent(1);
    pass["shader"]["defines"] = Json::array({"OCEAN_STAGE=3"});
    CHECK(op.at("parameters") == pass);
    document["nodes"][1]["parameters"] = pass;
    tab.commit(document, "ux7-d-1");
    QCoreApplication::processEvents();
    table = tab.field<QTableWidget>("pelican.graphCanvasPass.shaderDefines.table");
    REQUIRE(table->rowCount() == 1);
    CHECK(table->item(0, 0)->text() == QStringLiteral("OCEAN_STAGE"));
    CHECK(table->item(0, 1)->text() == QStringLiteral("3"));

    // A name the engine defines itself: red, with the engine's sentence.
    table->item(0, 0)->setText(QStringLiteral("PELICAN_INPUT_0"));
    QCoreApplication::processEvents();
    CHECK(tab.reads("edit_graph_document") == 1);
    CHECK(table->item(0, 0)->toolTip().contains(QStringLiteral("defines itself")));

    // Remove takes the row away: the key goes.
    table->setCurrentCell(0, 0);
    ux7Click(tab.field<QPushButton>("pelican.graphCanvasPass.shaderDefines.remove"));
    op = tab.sent(2);
    pass["shader"].erase("defines");
    CHECK(op.at("parameters") == pass);
}

TEST_CASE("UX-7 decisions 3 and 4: a raster pass's Depth is picked (a new D32_SFLOAT image in "
          "one edit the engine takes), its raster_state from the Raster form, and its draw "
          "counted, Grid NxN writing 6(N-1)^2 vertices",
          "[devstudio][graph-canvas][scene][ux7]") {
    (void)application();
    Ux2Tab tab{true};
    auto pass = ux7RasterPass();
    auto document = ux7RasterDocument(pass);
    tab.read(document, "ux7-r-0");
    tab.answer("get_graph_catalog", ux7Catalog());
    tab.widget->scene().selectNode("sea");
    QCoreApplication::processEvents();
    REQUIRE(tab.field<QWidget>("pelican.graphCanvasPass")->isVisible());

    // Decision 4: the draw, counted.
    auto *vertices = tab.field<QSpinBox>("pelican.graphCanvasPass.vertexCount");
    REQUIRE(vertices->isVisible());
    CHECK(vertices->value() == 3);
    CHECK(tab.field<QSpinBox>("pelican.graphCanvasPass.instanceCount")->value() == 1);
    CHECK(tab.field<QSpinBox>("pelican.graphCanvasPass.gridSize")->value() == 257);
    ux7Click(tab.field<QPushButton>("pelican.graphCanvasPass.grid"));
    auto op = tab.sent(1);
    pass["draw"]["vertex_count"] = 6 * 256 * 256;
    CHECK(op.at("parameters") == pass);
    // A count given while that edit is in flight waits for it, and is sent
    // then -- not dropped, nor the field put back under the hand.
    auto *instances = tab.field<QSpinBox>("pelican.graphCanvasPass.instanceCount");
    instances->setValue(2);
    Q_EMIT instances->editingFinished();
    QCoreApplication::processEvents();
    CHECK(tab.reads("edit_graph_document") == 1);
    CHECK(instances->value() == 2);
    document = ux7RasterDocument(pass);
    tab.commit(document, "ux7-r-1");
    QCoreApplication::processEvents();
    CHECK(tab.field<QSpinBox>("pelican.graphCanvasPass.vertexCount")->value() == 393216);
    for (int wait = 0; wait < 100 && tab.reads("edit_graph_document") < 2; ++wait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        QCoreApplication::processEvents();
    }
    op = tab.sent(2);
    pass["draw"]["instance_count"] = 2;
    CHECK(op.at("parameters") == pass);
    document = ux7RasterDocument(pass);
    tab.commit(document, "ux7-r-2");
    QCoreApplication::processEvents();

    // Decision 3: the Raster form is a raster pass's too; no depth rows
    // without a depth image.
    REQUIRE(tab.field<QWidget>("pelican.graphCanvasPass.rasterState")->isVisible());
    CHECK_FALSE(tab.field<QComboBox>("pelican.graphCanvasPass.rasterState.depthTest")->isVisible());
    auto *depth = tab.field<QComboBox>("pelican.graphCanvasPass.depth");
    REQUIRE(depth->isVisible());
    CHECK(depth->currentData().toString().isEmpty());
    tab.pick(depth, QString::fromStdString(std::string{GraphCanvasModel::kNewRasterDepth}));
    const auto ops = ux7Ops(tab, 3);
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].at("op") == "add_node");
    const auto image = ops[0].at("node");
    CHECK(image.at("id") == "sea__depth");
    CHECK(image.at("parameters").at("format") == "D32_SFLOAT");
    CHECK(image.at("parameters").at("extent_scale") == 1.0);
    CHECK(image.at("parameters").at("usage") == Json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"}));
    CHECK(ops[1].at("op") == "replace_node");
    const auto sea = ops[1].at("node");
    CHECK(sea.at("parameters").at("ports").at("depth") ==
          Json{{"kind", "image"},
               {"type", image.at("parameters").at("type")},
               {"access", "write"},
               {"attachment", "depth"}});
    CHECK(sea.at("inputs").at("depth") == Json{{"node", "sea__depth"}, {"port", "resource"}});
    CHECK(sea.at("outputs") == Json::array({"color", "depth"}));
    CHECK(sea.at("parameters").at("raster_state") == Json{{"depth_test", true}, {"depth_write", true}});
    // The engine's graph edit takes it.
    Json deep;
    REQUIRE_NOTHROW(deep = Pelican::editGraphDocument(document, ops));
    tab.commit(deep, "ux7-r-3");
    QCoreApplication::processEvents();
    CHECK(tab.field<QComboBox>("pelican.graphCanvasPass.depth")->currentData().toString() ==
          QStringLiteral("sea__depth"));
    auto *compare = tab.field<QComboBox>("pelican.graphCanvasPass.rasterState.depthCompare");
    REQUIRE(compare->isVisible());
    tab.pick(compare, QStringLiteral("less_equal"));
    op = tab.sent(4);
    auto expected = sea.at("parameters");
    expected["raster_state"]["depth_compare"] = "less_equal";
    CHECK(op.at("parameters") == expected);
    deep = Pelican::editGraphDocument(deep, Json::array({op}));
    tab.commit(deep, "ux7-r-4");
    QCoreApplication::processEvents();
    tab.pick(tab.field<QComboBox>("pelican.graphCanvasPass.rasterState.cull"), QStringLiteral("back"));
    op = tab.sent(5);
    expected["raster_state"]["cull"] = "back";
    CHECK(op.at("parameters") == expected);
    deep = Pelican::editGraphDocument(deep, Json::array({op}));
    tab.commit(deep, "ux7-r-5");
    QCoreApplication::processEvents();

    // None again: the port, its line and output go, and so do the depth keys
    // (the engine refuses depth state without an attachment).
    tab.pick(tab.field<QComboBox>("pelican.graphCanvasPass.depth"), QString{});
    const auto back = ux7Ops(tab, 6);
    REQUIRE(back.size() == 1);
    CHECK(back[0].at("op") == "replace_node");
    CHECK_FALSE(back[0].at("node").at("parameters").at("ports").contains("depth"));
    CHECK_FALSE(back[0].at("node").at("inputs").contains("depth"));
    CHECK(back[0].at("node").at("outputs") == Json::array({"color"}));
    CHECK(back[0].at("node").at("parameters").at("raster_state") == Json{{"cull", "back"}});
    CHECK_NOTHROW(Pelican::editGraphDocument(deep, back));
}

// UX-7 follow-up (GB-3's ocean, step "two instances"): a count typed into a
// draw spin and not yet confirmed is sent when a redraw shows no node for a
// moment and the Pass fields let go of the pass -- the hidden spin's focus out
// used to find no pass -- and the notice says "Saving" from the moment a
// count is confirmed, so a wait for "saved" cannot pass on the last save.
TEST_CASE("UX-7 a draw count typed but not confirmed survives the Pass fields letting go, and "
          "the notice says Saving at once",
          "[devstudio][graph-canvas][scene][ux7]") {
    (void)application();
    Ux2Tab tab{true};
    auto pass = ux7RasterPass();
    auto document = ux7RasterDocument(pass);
    tab.read(document, "ux7-f-0");
    tab.answer("get_graph_catalog", ux7Catalog());
    tab.widget->scene().selectNode("sea");
    QCoreApplication::processEvents();
    REQUIRE(tab.field<QWidget>("pelican.graphCanvasPass")->isVisible());
    auto *notice = tab.field<QLabel>("pelican.graphCanvasNotice");

    // Grid: sent, and the line says Saving before any answer.
    ux7Click(tab.field<QPushButton>("pelican.graphCanvasPass.grid"));
    auto op = tab.sent(1);
    pass["draw"]["vertex_count"] = 6 * 256 * 256;
    CHECK(op.at("parameters") == pass);
    CHECK(notice->text().contains(QStringLiteral("Saving")));
    document = ux7RasterDocument(pass);
    tab.commit(document, "ux7-f-1");
    QCoreApplication::processEvents();
    CHECK_FALSE(notice->text().contains(QStringLiteral("Saving")));

    // 2 typed into Instances, no Return.
    auto *instances = tab.field<QSpinBox>("pelican.graphCanvasPass.instanceCount");
    tab.widget->activateWindow();
    instances->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    REQUIRE(instances->hasFocus());
    instances->selectAll();
    QKeyEvent press{QEvent::KeyPress, Qt::Key_2, Qt::NoModifier, QStringLiteral("2")};
    QCoreApplication::sendEvent(instances, &press);
    QKeyEvent release{QEvent::KeyRelease, Qt::Key_2, Qt::NoModifier, QStringLiteral("2")};
    QCoreApplication::sendEvent(instances, &release);
    CHECK(instances->cleanText() == QStringLiteral("2"));
    CHECK(tab.reads("edit_graph_document") == 1);
    // A redraw that picks nothing for a moment.
    tab.widget->scene().selectNode("not_drawn");
    QCoreApplication::processEvents();
    op = tab.sent(2);
    pass["draw"]["instance_count"] = 2;
    CHECK(op.at("parameters") == pass);
    CHECK(notice->text().contains(QStringLiteral("Saving")));
}

TEST_CASE("UX-7 decision 5: render.scalar, value.constant and value.input ask for what they "
          "cannot be placed without, and are placed with it",
          "[devstudio][graph-canvas][scene][ux7]") {
    (void)application();
    auto document = ux3Document();
    // CT-5: what a render.scalar writes is a runtime_f32 buffer as large as
    // its sinks (the one float its value edge brings).
    document["nodes"].push_back({{"id", "params"}, {"op", "render.buffer"},
                                 {"parameters", {{"element", "float"}, {"size", 4},
                                                 {"host_source", "runtime_f32"}}},
                                 {"outputs", Json::array({"resource"})}});
    GraphCanvasModel model;
    model.setConnected(true);
    answer(model, document, engineCatalog());
    REQUIRE(model.canEdit());

    // Nothing to ask for a node the engine takes as it is.
    CHECK_FALSE(PelicanStudio::GraphCanvasRequiredDialog(model, {}, "render.image").asks());

    {
        PelicanStudio::GraphCanvasRequiredDialog ask{model, {}, "render.scalar"};
        REQUIRE(ask.asks());
        auto *target = ask.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasRequired.target"));
        REQUIRE(target != nullptr);
        CHECK(target->currentText() == QStringLiteral("params"));
        CHECK(ask.parameters() == Json{{"target", "params"}});
        // A new name; an empty one cannot be OK'd.
        target->setEditText(QStringLiteral("glow"));
        CHECK(ask.parameters() == Json{{"target", "glow"}});
        auto *ok = ask.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasRequired.ok"));
        REQUIRE(ok != nullptr);
        CHECK(ok->isEnabled());
        target->setEditText(QString{});
        CHECK_FALSE(ok->isEnabled());
    }
    {
        PelicanStudio::GraphCanvasRequiredDialog ask{model, {}, "value.input"};
        auto *name = ask.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasRequired.name"));
        REQUIRE(name != nullptr);
        CHECK(ask.parameters() == Json{{"name", "scene_time"}});
        name->setCurrentIndex(name->findData(QStringLiteral("delta_time")));
        CHECK(ask.parameters() == Json{{"name", "delta_time"}});
    }
    PelicanStudio::GraphCanvasRequiredDialog constant{model, {}, "value.constant"};
    auto *value =
        constant.findChild<QDoubleSpinBox *>(QStringLiteral("pelican.graphCanvasRequired.value"));
    REQUIRE(value != nullptr);
    value->setValue(2.5);
    CHECK(constant.parameters() == Json{{"value", 2.5}});

    // Placed with them, in the engine's form.
    const auto registry = Pelican::makeBuiltinGraphModuleRegistry();
    for (const auto &[op, parameters] :
         std::vector<std::pair<std::string, Json>>{{"value.constant", {{"value", 2.5}}},
                                                   {"value.input", {{"name", "scene_time"}}},
                                                   {"render.scalar", {{"target", "params"}}}}) {
        INFO(op);
        const auto ops = model.addNodeOpsIn({}, op, {}, Json{{"parameters", parameters}});
        REQUIRE_FALSE(ops.empty());
        CHECK(ops.back().at("node").at("parameters") == parameters);
        auto placed = document;
        for (const auto &each : ops) placed["nodes"].push_back(each.at("node"));
        CHECK_NOTHROW(Pelican::partitionGraphDocument(placed, registry));
    }
}

TEST_CASE("UX-7 decision 6: a render.image keeps the previous frame by its check "
          "(history: true), one set_node_parameters each way",
          "[devstudio][graph-canvas][scene][ux7]") {
    (void)application();
    Ux2Tab tab{true};
    auto image = ux2Image();
    auto document = ux2Document(image);
    tab.read(document, "ux7-h-0");
    tab.answer("get_graph_catalog", ux2Catalog());
    tab.widget->scene().selectNode("shade__color");
    QCoreApplication::processEvents();
    auto *history = tab.field<QCheckBox>("pelican.graphCanvasImage.history");
    REQUIRE(history->isVisible());
    CHECK_FALSE(history->isChecked());
    ux7Click(history);
    auto op = tab.sent(1);
    image["history"] = true;
    CHECK(op.at("parameters") == image);
    document = ux2Document(image);
    tab.commit(document, "ux7-h-1");
    QCoreApplication::processEvents();
    history = tab.field<QCheckBox>("pelican.graphCanvasImage.history");
    CHECK(history->isChecked());
    ux7Click(history);
    op = tab.sent(2);
    image.erase("history");
    CHECK(op.at("parameters") == image);
}

TEST_CASE("UX-7 decision 7: a node the document gains is drawn while a pill's editor is open, "
          "the editor carried over the new drawing",
          "[devstudio][graph-canvas][scene][ux7]") {
    (void)application();
    Ux2Tab tab{false};
    auto document = ux3Document();
    tab.read(document, "ux7-p-0");
    tab.answer("get_graph_catalog", ux7Catalog());
    auto &scene = tab.widget->scene();
    REQUIRE(nodeItem(scene, QStringLiteral("shade")) != nullptr);
    // GB-3: a pick that lands on a pill leaves its editor open; the raster
    // pass the palette added after it reached the document and the plan,
    // but not the canvas, Refresh or not.
    REQUIRE(scene.beginValueEdit("shade", "shader"));
    REQUIRE(scene.editingInline());
    auto gained = document;
    const auto raster = ux7RasterDocument();
    gained["nodes"].push_back({{"id", "sea__color"}, {"op", "render.image"},
                               {"parameters", raster.at("nodes").at(0).at("parameters")},
                               {"outputs", Json::array({"resource"})}});
    auto sea = raster.at("nodes").at(1);
    sea["inputs"]["color"]["node"] = "sea__color";
    gained["nodes"].push_back(sea);
    REQUIRE_NOTHROW(Pelican::editGraphDocument(gained, Json::array()));
    tab.field<QPushButton>("pelican.graphCanvasRefresh")->click();
    QCoreApplication::processEvents();
    tab.read(gained, "ux7-p-1");
    QCoreApplication::processEvents();
    CHECK(nodeItem(scene, QStringLiteral("sea")) != nullptr);
    CHECK(nodeItem(scene, QStringLiteral("sea__color")) != nullptr);
    // The editor is still open, over the new drawing.
    CHECK(scene.editingInline());
}

// --- UX-8.

namespace {

Json ux8CurveDocument() {
    return {{"schema", "pelican.graph"},
            {"version", 1},
            {"outputs", Json::object()},
            {"definitions", Json::object()},
            {"nodes",
             Json::array({{{"id", "scene_time"}, {"op", "value.input"},
                           {"parameters", {{"name", "scene_time"}}},
                           {"outputs", Json::array({"value"})}},
                          {{"id", "curve"}, {"op", "value.curve"},
                           {"parameters",
                            {{"keys", Json::array({{{"time", 0.0}, {"value", 0.0}},
                                                   {{"time", 1.0}, {"value", 1.0}}})}}},
                           {"inputs", {{"time", {{"node", "scene_time"}, {"port", "value"}}}}},
                           {"outputs", Json::array({"value"})}}})}};
}

// The keys the `count`th edit wrote.
Json ux8Keys(const Ux2Tab &tab, int count) {
    const auto ops = ux7Ops(tab, count);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].at("op") == "set_node_parameters");
    return ops[0].at("parameters").at("keys");
}

void ux8Mouse(QWidget *widget, QEvent::Type type, const QPointF &at,
              Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event{type, at, widget->mapToGlobal(at), Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      modifiers};
    QCoreApplication::sendEvent(widget, &event);
}

} // namespace

TEST_CASE("UX-8 decision 4: a curve's Keys table, its small curve and the Timeline's marks each "
          "write one edit",
          "[devstudio][graph-canvas][scene][ux8]") {
    (void)application();
    Ux2Tab tab{false};
    auto document = ux8CurveDocument();
    tab.read(document, "ux8-k-0");
    tab.answer("get_graph_catalog", engineCatalog());
    tab.widget->scene().selectNode("curve");
    QCoreApplication::processEvents();

    auto *table = tab.field<QTableWidget>("pelican.graphCanvasCurve.keys");
    REQUIRE(table->isVisible());
    CHECK(table->rowCount() == 2);
    // One row's value, typed: one edit with the whole list.
    auto *value = tab.field<QDoubleSpinBox>("pelican.graphCanvasCurve.key1.value");
    value->setValue(0.9);
    Q_EMIT value->editingFinished();
    QCoreApplication::processEvents();
    CHECK(ux8Keys(tab, 1) == Json::array({{{"time", 0.0}, {"value", 0.0}},
                                          {{"time", 1.0}, {"value", 0.9}}}));
    document["nodes"][1]["parameters"]["keys"][1]["value"] = 0.9;
    tab.commit(document, "ux8-k-1");
    QCoreApplication::processEvents();

    // The small curve: key 0 dragged right with Shift moves its time only.
    tab.widget->scene().selectNode("curve");
    QCoreApplication::processEvents();
    // No Q_OBJECT of its own: found as the widget it is.
    auto *graph = static_cast<PelicanStudio::GraphCanvasCurveGraph *>(
        tab.field<QWidget>("pelican.graphCanvasCurve.graph"));
    REQUIRE(graph->keys().size() == 2);
    const auto start = graph->keyPoint(0);
    ux8Mouse(graph, QEvent::MouseButtonPress, start);
    ux8Mouse(graph, QEvent::MouseMove, start + QPointF{30.0, -20.0}, Qt::ShiftModifier);
    ux8Mouse(graph, QEvent::MouseButtonRelease, start + QPointF{30.0, -20.0}, Qt::ShiftModifier);
    QCoreApplication::processEvents();
    const auto dragged = ux8Keys(tab, 2);
    CHECK(dragged[0].at("time").get<double>() > 0.0);
    CHECK(dragged[0].at("value") == 0.0);
    CHECK(dragged[1] == Json{{"time", 1.0}, {"value", 0.9}});
    document["nodes"][1]["parameters"]["keys"] = dragged;
    tab.commit(document, "ux8-k-2");
    QCoreApplication::processEvents();

    // The Timeline: key 1's mark dragged left, to the middle of the strip.
    tab.widget->scene().selectNode("curve");
    tab.field<QPushButton>("pelican.graphTimelineToggle")->click();
    QCoreApplication::processEvents();
    auto *strip = tab.field<QWidget>("pelican.graphTimeline");
    REQUIRE(strip->isVisible());
    const qreal y = strip->height() - 10.0;
    const qreal right = strip->width() - 1.0;
    ux8Mouse(strip, QEvent::MouseButtonPress, QPointF{right, y});
    ux8Mouse(strip, QEvent::MouseMove, QPointF{right * 0.75, y});
    ux8Mouse(strip, QEvent::MouseButtonRelease, QPointF{right * 0.75, y});
    QCoreApplication::processEvents();
    const auto moved = ux8Keys(tab, 3);
    CHECK(std::abs(moved[1].at("time").get<double>() - 0.75) < 0.02);
    CHECK(moved[1].at("value") == 0.9);
    // A press elsewhere on the strip is a scrub, as before.
    CHECK(tab.reads("set_time") == 0);
    ux8Mouse(strip, QEvent::MouseButtonPress, QPointF{right * 0.5, 10.0});
    ux8Mouse(strip, QEvent::MouseButtonRelease, QPointF{right * 0.5, 10.0});
    CHECK(tab.reads("set_time") == 1);
    CHECK(tab.reads("edit_graph_document") == 3);
}

TEST_CASE("UX-8 decisions 7 and 8: audio.volume asks which voice it drives; what one addition "
          "brings beside it is placed clear of what is drawn",
          "[devstudio][graph-canvas][scene][ux8]") {
    (void)application();
    Ux2Tab tab{false};
    auto document = ux8CurveDocument();
    document["nodes"].push_back({{"id", "voice"}, {"op", "audio.playback"},
                                 {"parameters", {{"asset", "project://assets/audio/pulse.wav"}}},
                                 {"inputs", {{"time", {{"node", "scene_time"}, {"port", "value"}}}}},
                                 {"outputs", Json::array({"voice"})}});
    // The root draws its value and audio nodes too (Render only to begin with).
    tab.field<QCheckBox>("pelican.graphCanvasShow.value")->setChecked(true);
    tab.field<QCheckBox>("pelican.graphCanvasShow.audio")->setChecked(true);
    tab.read(document, "ux8-p-0");
    tab.answer("get_graph_catalog", engineCatalog());
    auto &model = tab.widget->model();

    PelicanStudio::GraphCanvasRequiredDialog ask{model, {}, "audio.volume"};
    REQUIRE(ask.asks());
    auto *voice = ask.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasRequired.voice"));
    REQUIRE(voice != nullptr);
    CHECK(voice->currentText() == QStringLiteral("voice"));
    CHECK(ask.seed() == Json{{"parameters", Json::object()},
                             {"inputs", {{"voice", {{"node", "voice"}, {"port", "voice"}}}}}});

    // Placed: the sink and the constant on its value, which lands left of it.
    auto &scene = tab.widget->scene();
    REQUIRE(scene.addFromPalette({}, "audio.volume", {}, ask.seed()));
    QCoreApplication::processEvents();
    const auto ops = ux7Ops(tab, 1);
    REQUIRE(ops.size() == 2);
    auto placed = document;
    for (const auto &op : ops) placed["nodes"].push_back(op.at("node"));
    tab.commit(placed, "ux8-p-1");
    QCoreApplication::processEvents();
    std::vector<std::pair<QString, QRectF>> boxes;
    for (const auto &node : placed.at("nodes")) {
        const auto id = QString::fromStdString(node.at("id").get<std::string>());
        auto *item = nodeItem(scene, id);
        REQUIRE(item != nullptr);
        boxes.emplace_back(id, item->sceneBoundingRect());
    }
    for (std::size_t a = 0; a < boxes.size(); ++a)
        for (std::size_t b = a + 1; b < boxes.size(); ++b) {
            INFO(boxes[a].first.toStdString() << " / " << boxes[b].first.toStdString());
            CHECK_FALSE(boxes[a].second.intersects(boxes[b].second));
        }
    const auto constant = QString::fromStdString(ops[0].at("node").at("id").get<std::string>());
    CHECK(nodeItem(scene, constant)->sceneBoundingRect().right() <
          nodeItem(scene, QStringLiteral("audio_volume"))->sceneBoundingRect().left());
}

TEST_CASE("UX-8 decisions 10 and 11: New definition opens the definition it made; a loop's body is "
          "drawn when its loop is gone",
          "[devstudio][graph-canvas][scene][ux8]") {
    (void)application();
    Ux2Tab tab{false};
    auto document = ux8CurveDocument();
    tab.read(document, "ux8-d-0");
    tab.answer("get_graph_catalog", engineCatalog());
    auto *action = tab.widget->findChild<QAction *>(QStringLiteral("pelican.graphCanvas.newDefinition"));
    REQUIRE(action != nullptr);
    action->trigger();
    QCoreApplication::processEvents();
    auto *name = tab.field<QLineEdit>("pelican.graphCanvasDefinitionName");
    CHECK(name->text() == QStringLiteral("definition"));
    name->setText(QStringLiteral("loop_body"));
    tab.field<QPushButton>("pelican.graphCanvasDefinitionApply")->click();
    QCoreApplication::processEvents();
    const auto ops = ux7Ops(tab, 1);
    CHECK(ops == Json::array({{{"op", "add_definition"}, {"name", "loop_body"},
                               {"definition", {{"nodes", Json::array()},
                                               {"outputs", Json::object()}}}}}));
    document["definitions"]["loop_body"] = {{"nodes", Json::array()}, {"outputs", Json::object()}};
    tab.commit(document, "ux8-d-1");
    QCoreApplication::processEvents();
    auto *scopes = tab.field<QComboBox>("pelican.graphCanvasScope");
    CHECK(scopes->currentData().toString() == QStringLiteral("loop_body"));
    auto &scene = tab.widget->scene();
    auto *view = tab.field<QGraphicsView>("pelican.graphCanvasView");
    // Decision 11 (GB-4): the empty body was fitted to nothing; the first
    // node it gains is brought into view rather than left outside it.
    view->scale(4.0, 4.0);
    view->centerOn(QPointF{-5000.0, -5000.0});
    document["definitions"]["loop_body"]["nodes"].push_back(
        {{"id", "gain"}, {"op", "value.constant"}, {"parameters", {{"value", 2.0}}},
         {"outputs", Json::array({"value"})}});
    tab.field<QPushButton>("pelican.graphCanvasRefresh")->click();
    tab.read(document, "ux8-d-1b");
    QCoreApplication::processEvents();
    REQUIRE(nodeItem(scene, QStringLiteral("gain")) != nullptr);
    CHECK(view->mapToScene(view->viewport()->rect())
              .boundingRect()
              .intersects(nodeItem(scene, QStringLiteral("gain"))->sceneBoundingRect()));

    // GB-4: a loop runs the body, the loop is deleted, and the body is
    // chosen in Scope again: its nodes are drawn and in view.
    scopes->setCurrentIndex(scopes->findData(QString{}));
    QCoreApplication::processEvents();
    auto with_loop = document;
    with_loop["nodes"].push_back({{"id", "loop"}, {"op", "graph.loop"},
                                  {"parameters", {{"over", {{"range", {{"count", 1}}}}},
                                                  {"branches", Json::array({{{"body", "loop_body"}}})}}},
                                  {"outputs", Json::array()}});
    tab.field<QPushButton>("pelican.graphCanvasRefresh")->click();
    tab.read(with_loop, "ux8-d-2");
    QCoreApplication::processEvents();
    scopes->setCurrentIndex(scopes->findData(QStringLiteral("loop_body")));
    QCoreApplication::processEvents();
    scopes->setCurrentIndex(scopes->findData(QString{}));
    QCoreApplication::processEvents();
    tab.field<QPushButton>("pelican.graphCanvasRefresh")->click();
    tab.read(document, "ux8-d-3");
    QCoreApplication::processEvents();
    scopes->setCurrentIndex(scopes->findData(QStringLiteral("loop_body")));
    QCoreApplication::processEvents();
    auto *gain = nodeItem(scene, QStringLiteral("gain"));
    REQUIRE(gain != nullptr);
    const auto visible = view->mapToScene(view->viewport()->rect()).boundingRect();
    CHECK(visible.intersects(gain->sceneBoundingRect()));
}

TEST_CASE("UX-8 decision 2: a pass's Buffers list its shader's buffer bindings, and + Buffer port "
          "offers the names its shader reads through the port header",
          "[devstudio][graph-canvas][scene][ux8]") {
    (void)application();
    const auto root = std::filesystem::temp_directory_path() / "pelican_ux8_buffers";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "shaders");
    {
        std::ofstream shader{root / "shaders" / "level.frag", std::ios::binary};
        shader << "#include \"pelican_resource_ports.glsl\"\n"
                  "#ifdef PELICAN_RESOURCE_PORT_level\n"
                  "float level = pelican_load_level(0u);\n#endif\n";
    }
    Ux2Tab tab{false};
    tab.widget->setProjectRoot(root);
    auto pass = ux2Pass();
    pass["shader"]["fragment"] = "project://shaders/level";
    auto document = ux2Document(ux2Image(), pass);
    document["nodes"].push_back({{"id", "level_buffer"}, {"op", "render.buffer"},
                                 {"parameters", {{"element", "float"}, {"size", 4}}},
                                 {"outputs", Json::array({"resource"})}});
    tab.read(document, "ux8-b-0");
    tab.answer("get_graph_catalog", ux7Catalog());
    tab.widget->scene().selectNode("shade");
    QCoreApplication::processEvents();
    auto *list = tab.field<QLabel>("pelican.graphCanvasBuffers.list");
    REQUIRE(list->isVisible());
    tab.field<QPushButton>("pelican.graphCanvasBuffers.add")->click();
    QCoreApplication::processEvents();
    auto *name = tab.field<QComboBox>("pelican.graphCanvasBuffers.name");
    CHECK(name->currentText() == QStringLiteral("level"));
    auto *source = tab.field<QComboBox>("pelican.graphCanvasBuffers.source");
    CHECK(source->currentData().toString() == QStringLiteral("level_buffer"));
    tab.field<QPushButton>("pelican.graphCanvasBuffers.apply")->click();
    QCoreApplication::processEvents();
    const auto ops = ux7Ops(tab, 1);
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].at("parameters").at("ports").at("level") ==
          Json{{"kind", "buffer"}, {"access", "read"}, {"element", "float"}});
    CHECK(ops[1].at("port") == "level");
    std::filesystem::remove_all(root);
}

// UX-9, decision 1. Picking a pass by its middle opened a pill's editor; the
// click on Add took the keyboard from it, and the editor committed the value
// it was opened on -- an edit of nothing, in flight just as the palette asked
// whether the model could edit, so the palette never opened. Now that value
// sends nothing, and a palette opened over a save in flight adds its pick
// once the save has landed.
TEST_CASE("UX-9 decision 1: an unchanged pill sends nothing as it loses the keyboard, and Add "
          "opens the palette over a save in flight, adding its pick once the save lands",
          "[devstudio][graph-canvas][scene][ux9]") {
    (void)application();
    Ux2Tab tab{false};
    auto document = readFixture("graph_node_visuals");
    tab.read(document, "ux9-a-0");
    // UX-12a, decision 2: what the dialog asks for is the table's (CT-3).
    auto catalog = visualCatalog();
    for (auto &module : catalog.at("modules"))
        for (auto &operation : module.at("operations"))
            if (operation.value("op", std::string{}) == "value.input")
                operation["parameters"] =
                    Json::array({{{"name", "name"}, {"kind", "string"}, {"required", true}}});
    tab.answer("get_graph_catalog", catalog);
    auto *show_values = tab.field<QCheckBox>("pelican.graphCanvasShow.value");
    show_values->setChecked(true);
    QCoreApplication::processEvents();
    auto &scene = tab.widget->scene();
    auto *add = tab.field<QPushButton>("pelican.graphCanvasAddNode");
    REQUIRE(add->isEnabled());

    // The editor left as it was opened commits on losing the keyboard: nothing.
    REQUIRE(scene.beginValueEdit("helmet_pulse", "gain"));
    CHECK_FALSE(scene.commitInlineEdit());
    QCoreApplication::processEvents();
    CHECK(tab.reads("edit_graph_document") == 0);
    CHECK(tab.widget->model().canEdit());

    // A value that did change goes as Add is pressed: the palette opens over
    // that save anyway.
    REQUIRE(scene.beginValueEdit("helmet_pulse", "gain"));
    scene.setInlineEditText(QStringLiteral("4.5"));
    add->click();
    REQUIRE(scene.commitInlineEdit());
    QDialog *palette = nullptr;
    for (int tick = 0; tick < 200 && palette == nullptr; ++tick) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        palette = paletteOnScreen();
    }
    REQUIRE(palette != nullptr);
    CHECK(tab.reads("edit_graph_document") == 1);
    CHECK_FALSE(tab.widget->model().canEdit());

    // Enter: the palette goes, and its pick waits for the save.
    auto *filter = palette->findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasPaletteFilter"));
    auto *list = palette->findChild<QListWidget *>(QStringLiteral("pelican.graphCanvasPaletteList"));
    REQUIRE(filter != nullptr);
    REQUIRE(list != nullptr);
    filter->setText(QStringLiteral("value.add"));
    REQUIRE(list->currentItem() != nullptr);
    const auto chosen = list->currentItem()->data(Qt::UserRole).toString().toStdString();
    sendKey(filter, Qt::Key_Return);
    QCoreApplication::processEvents();
    CHECK(paletteOnScreen() == nullptr);
    CHECK(tab.reads("edit_graph_document") == 1);
    CHECK(tab.field<QLabel>("pelican.graphCanvasNotice")->text() ==
          QStringLiteral("Saving the graph change..."));

    // The save lands; the pick is added.
    for (auto &node : document.at("nodes"))
        if (node.at("id") == "helmet_pulse") node["parameters"]["gain"] = 4.5;
    tab.commit(document, "ux9-a-1");
    QCoreApplication::processEvents();
    REQUIRE(tab.reads("edit_graph_document") == 2);
    const auto added = Json::parse(
        tab.transport.calls[static_cast<std::size_t>(
                                tab.transport.idOf(QStringLiteral("edit_graph_document")) - 1)]
            .body.toStdString());
    CHECK(std::ranges::any_of(added, [&](const Json &op) {
        return op.value("op", std::string{}) == "add_node" &&
               op.at("node").value("op", std::string{}) == chosen;
    }));

    // Decision 3: what the palette's pick asks for (a value.input's name)
    // comes in a dialog answered as it closes too, and is added after it.
    for (const auto &op : added)
        if (op.value("op", std::string{}) == "add_node") document["nodes"].push_back(op.at("node"));
    tab.commit(document, "ux9-a-2");
    QCoreApplication::processEvents();
    REQUIRE(tab.widget->model().canEdit());
    add->click();
    palette = nullptr;
    for (int tick = 0; tick < 200 && palette == nullptr; ++tick) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        palette = paletteOnScreen();
    }
    REQUIRE(palette != nullptr);
    palette->findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasPaletteFilter"))
        ->setText(QStringLiteral("value.input"));
    sendKey(palette->findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasPaletteFilter")),
            Qt::Key_Return);
    QDialog *ask = nullptr;
    for (int tick = 0; tick < 200 && ask == nullptr; ++tick) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->objectName() == QLatin1String{"pelican.graphCanvasRequired"} && top->isVisible())
                ask = qobject_cast<QDialog *>(top);
    }
    REQUIRE(ask != nullptr);
    auto *ok = ask->findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasRequired.ok"));
    REQUIRE(ok != nullptr);
    REQUIRE(ok->isEnabled());
    ok->click();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    REQUIRE(tab.reads("edit_graph_document") == 3);
    const auto asked = Json::parse(
        tab.transport.calls[static_cast<std::size_t>(
                                tab.transport.idOf(QStringLiteral("edit_graph_document")) - 1)]
            .body.toStdString());
    CHECK(std::ranges::any_of(asked, [](const Json &op) {
        return op.value("op", std::string{}) == "add_node" &&
               op.at("node").value("op", std::string{}) == "value.input" &&
               op.at("node").contains("parameters") && op.at("node").at("parameters").contains("name");
    }));
}

// UX-9, decision 2. LC-1s sent a draw count typed and not confirmed as the
// Pass fields let go of the pass; the other side fields do the same, and a
// value the model could not take (a save in flight) is held for its node
// across the letting go, saying Saving until it goes.
TEST_CASE("UX-9 decision 2: a value typed into a side field goes to its node as the fields let "
          "go of it, held for that node while a save is in flight",
          "[devstudio][graph-canvas][scene][ux9]") {
    (void)application();
    const auto typeInto = [](QAbstractSpinBox *spin, int key, const QString &text) {
        spin->setFocus(Qt::OtherFocusReason);
        QCoreApplication::processEvents();
        REQUIRE(spin->hasFocus());
        spin->selectAll();
        QKeyEvent press{QEvent::KeyPress, key, Qt::NoModifier, text};
        QCoreApplication::sendEvent(spin, &press);
        QKeyEvent release{QEvent::KeyRelease, key, Qt::NoModifier, text};
        QCoreApplication::sendEvent(spin, &release);
    };

    SECTION("an image's layers, typed, go to the image when another node is picked") {
        Ux2Tab tab{true};
        auto image = ux2Image();
        tab.read(ux2Document(image), "ux9-b-0");
        tab.answer("get_graph_catalog", ux2Catalog());
        tab.widget->scene().selectNode("shade__color");
        QCoreApplication::processEvents();
        REQUIRE(tab.field<QWidget>("pelican.graphCanvasImage")->isVisible());
        auto *layers = tab.field<QSpinBox>("pelican.graphCanvasImage.layers");
        tab.widget->activateWindow();
        typeInto(layers, Qt::Key_4, QStringLiteral("4"));
        CHECK(layers->cleanText() == QStringLiteral("4"));
        CHECK(tab.reads("edit_graph_document") == 0);
        tab.widget->scene().selectNode("shade");
        QCoreApplication::processEvents();
        const auto op = tab.sent(1);
        CHECK(op.at("node") == "shade__color");
        image["layers"] = 4;
        CHECK(op.at("parameters") == image);
        CHECK(tab.field<QLabel>("pelican.graphCanvasNotice")->text() ==
              QStringLiteral("Saving the graph change..."));
    }

    SECTION("a pass's count given over a save in flight goes to that pass once it lands") {
        Ux2Tab tab{true};
        auto pass = ux7RasterPass();
        auto document = ux7RasterDocument(pass);
        tab.read(document, "ux9-b-1");
        tab.answer("get_graph_catalog", ux7Catalog());
        tab.widget->scene().selectNode("sea");
        QCoreApplication::processEvents();
        REQUIRE(tab.field<QWidget>("pelican.graphCanvasPass")->isVisible());
        auto *notice = tab.field<QLabel>("pelican.graphCanvasNotice");
        // 2 typed into Instances, then Grid: its edit goes and the model is busy.
        auto *instances = tab.field<QSpinBox>("pelican.graphCanvasPass.instanceCount");
        tab.widget->activateWindow();
        typeInto(instances, Qt::Key_2, QStringLiteral("2"));
        ux7Click(tab.field<QPushButton>("pelican.graphCanvasPass.grid"));
        REQUIRE(tab.reads("edit_graph_document") >= 1);
        const int before = tab.reads("edit_graph_document");
        CHECK_FALSE(tab.widget->model().canEdit());
        // Another node picked while that save is in flight: nothing is sent
        // yet, and the line says Saving.
        tab.widget->scene().selectNode("not_drawn");
        QCoreApplication::processEvents();
        CHECK(tab.reads("edit_graph_document") == before);
        CHECK(notice->text() == QStringLiteral("Saving the graph change..."));
        // The save lands: the count goes to the pass it was typed for, on
        // what the document holds now.
        const auto first = ux7Ops(tab, before);
        REQUIRE(first.size() == 1);
        pass = first[0].at("parameters");
        document = ux7RasterDocument(pass);
        tab.commit(document, "ux9-b-2");
        QCoreApplication::processEvents();
        REQUIRE(tab.reads("edit_graph_document") == before + 1);
        const auto ops = ux7Ops(tab, before + 1);
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].at("node") == "sea");
        CHECK(ops[0].at("parameters").at("draw").at("instance_count") == 2);
        CHECK(ops[0].at("parameters").at("draw").at("vertex_count") ==
              pass.at("draw").at("vertex_count"));
    }
}

// UX-9, decision 4 (engine CT-1): get_status.audio.voices on the bands.
TEST_CASE("UX-9 decision 4: a voice's state reads as the band under its audio.playback -- "
          "playing and paused with where it is, idle only in a play session",
          "[devstudio][graph-canvas][scene][ux9]") {
    (void)application();
    using Bands = std::map<std::string, std::string, std::less<>>;
    const auto status = [](const char *play, Json voices) {
        return Json{{"play", {{"state", play}}},
                    {"audio", {{"available", true}, {"backend", "null"}, {"voices", std::move(voices)}}}};
    };
    const auto voice = [](const char *state, double at) {
        return Json::array({{{"node", "clock"}, {"state", state}, {"position_seconds", at}}});
    };
    const std::string dot = " \xC2\xB7 ";
    CHECK(PelicanStudio::graphCanvasVoiceBands(status("edit", voice("idle", 0.0))).empty());
    CHECK(PelicanStudio::graphCanvasVoiceBands(status("playing", voice("idle", 0.0))) ==
          Bands{{"clock", "idle"}});
    CHECK(PelicanStudio::graphCanvasVoiceBands(status("playing", voice("playing", 1.234))) ==
          Bands{{"clock", "playing" + dot + "1.2 s"}});
    CHECK(PelicanStudio::graphCanvasVoiceBands(status("paused", voice("paused", 0.04))) ==
          Bands{{"clock", "paused" + dot + "0.0 s"}});
    CHECK(PelicanStudio::graphCanvasVoiceBands(Json{{"frame", 3}}).empty());

    // On the canvas: the band under the node, in place.
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, readFixture("graph_node_visuals"), visualCatalog());
    scene.setModel(&model);
    const auto band = [&]() -> QString {
        for (QGraphicsItem *item : itemsOfKind(scene, PelicanStudio::GraphCanvasLayerBandItem))
            if (item->data(PelicanStudio::NodeGraphNameRole).toString() == QLatin1String{"clock"})
                return dynamic_cast<QGraphicsSimpleTextItem *>(item)->text();
        return QStringLiteral("<no band>");
    };
    CHECK(band() == QStringLiteral("<no band>"));
    const auto *node = nodeItem(scene, QStringLiteral("clock"));
    REQUIRE(node != nullptr);
    scene.setVoiceBands(PelicanStudio::graphCanvasVoiceBands(status("playing", voice("playing", 1.234))));
    CHECK(band() == QString::fromStdString("playing" + dot + "1.2 s"));
    CHECK(nodeItem(scene, QStringLiteral("clock")) == node);
    scene.setVoiceBands(PelicanStudio::graphCanvasVoiceBands(status("edit", voice("idle", 0.0))));
    CHECK(band().isEmpty());
}

// --- IA-1s.

TEST_CASE("IA-1s decision 1: Initial contents lists the project's images, is off on a per-frame "
          "image, and writes initial.asset with the size from the image; the check gives a fixed "
          "size back",
          "[devstudio][graph-canvas][scene][ia1s]") {
    (void)application();
    const auto root = std::filesystem::temp_directory_path() / "pelican_ia1s_initial";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "assets" / "luts");
    {
        // A KTX2's identifier, vkFormat, typeSize, then its width and height.
        std::string bytes{"\xABKTX 20\xBB\r\n\x1A\n", 12};
        for (const std::uint32_t word : {109u, 4u, 64u, 32u})
            for (int shift = 0; shift < 32; shift += 8)
                bytes.push_back(static_cast<char>((word >> shift) & 0xFF));
        bytes.resize(64, '\0');
        std::ofstream{root / "assets" / "luts" / "ltc.ktx2", std::ios::binary} << bytes;
        std::ofstream{root / "assets" / "luts" / "notes.txt", std::ios::binary} << "not an image";
    }
    Ux2Tab tab{false};
    tab.widget->setProjectRoot(root);
    auto image = ux2Image();
    tab.read(ux2Document(image), "ia1s-0");
    tab.answer("get_graph_catalog", ux2Catalog());
    tab.widget->scene().selectNode("shade__color");
    QCoreApplication::processEvents();
    auto *initial = tab.field<QComboBox>("pelican.graphCanvasImage.initial");
    REQUIRE(initial->isVisible());
    CHECK(ux2Items(*initial) == QStringList{"None", "assets/luts/ltc.ktx2"});
    // Per frame: off, and the tooltip says why.
    CHECK_FALSE(initial->isEnabled());
    CHECK(initial->toolTip() == QStringLiteral("Make it Persistent to start from an image"));
    CHECK_FALSE(tab.field<QPushButton>("pelican.graphCanvasImage.import")->isEnabled());
    CHECK_FALSE(tab.field<QCheckBox>("pelican.graphCanvasImage.fromImage")->isEnabled());

    tab.pick(tab.field<QComboBox>("pelican.graphCanvasImage.lifetime"), QStringLiteral("persistent"));
    image["lifetime"] = "persistent";
    CHECK(tab.sent(1).at("parameters") == image);
    tab.commit(ux2Document(image), "ia1s-1");
    initial = tab.field<QComboBox>("pelican.graphCanvasImage.initial");
    REQUIRE(initial->isEnabled());

    // The pick: initial.asset, and the screen's scale goes -- the size is
    // the image's (a screen-sized one would be refused).
    tab.pick(initial, QStringLiteral("assets/luts/ltc.ktx2"));
    image["initial"] = {{"asset", "assets/luts/ltc.ktx2"}};
    image.erase("extent_scale");
    CHECK(tab.sent(2).at("parameters") == image);
    tab.commit(ux2Document(image), "ia1s-2");
    auto *from_image = tab.field<QCheckBox>("pelican.graphCanvasImage.fromImage");
    CHECK(from_image->isChecked());
    CHECK(from_image->isEnabled());
    CHECK(from_image->text() == QString::fromUtf8("Size from the image (64 \xC3\x97 32)"));
    CHECK_FALSE(tab.field<QSpinBox>("pelican.graphCanvasImage.width")->isEnabled());
    CHECK_FALSE(tab.field<QRadioButton>("pelican.graphCanvasImage.sizeScale")->isEnabled());

    // Unchecked: a fixed size, the image's own.
    ux7Click(from_image);
    image["extent_scale"] = 1.0;
    image["width"] = 64;
    image["height"] = 32;
    CHECK(tab.sent(3).at("parameters") == image);
    tab.commit(ux2Document(image), "ia1s-3");
    CHECK_FALSE(tab.field<QCheckBox>("pelican.graphCanvasImage.fromImage")->isChecked());
    CHECK(tab.field<QSpinBox>("pelican.graphCanvasImage.width")->isEnabled());

    // None: the asset goes, the size stays.
    tab.pick(tab.field<QComboBox>("pelican.graphCanvasImage.initial"), QString{});
    image.erase("initial");
    CHECK(tab.sent(4).at("parameters") == image);
    std::filesystem::remove_all(root);
}

// --- UX-11: a material pass's settings on the Graph, in the Pass Editor's
// forms, and what it draws.

namespace {

// The Graph tab over a recorded transport that answers the frame plan and
// the authoring context too.
struct Ux11Tab {
    RecordedCapability transport;
    std::unique_ptr<PelicanStudio::GraphCanvasWidget> widget;

    Ux11Tab() {
        auto capability = transport.capability();
        capability.requestRenderAuthoringContext = [this](QString *) {
            transport.calls.push_back({QStringLiteral("get_render_authoring_context")});
            return transport.next++;
        };
        capability.requestFramePlan = [this](QString *) {
            transport.calls.push_back({QStringLiteral("get_frame_plan")});
            return transport.next++;
        };
        widget = std::make_unique<PelicanStudio::GraphCanvasWidget>(capability);
        widget->resize(1100, 900);
        widget->show();
        QCoreApplication::processEvents();
    }
    void answer(const char *method, const Json &reply) {
        const qint64 id = transport.idOf(QString::fromLatin1(method));
        REQUIRE(id != 0);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    }
    [[nodiscard]] int reads(const char *method) const {
        return transport.countOf(QString::fromLatin1(method));
    }
    void read(const Json &document, const std::string &digest) {
        auto reply = context(document);
        reply["source_digest"]["hex"] = digest;
        answer("get_graph_instances", reply);
    }
    void commit(const Json &document, const std::string &digest) {
        answer("edit_graph_document", Json{{"status", "accepted"}, {"ticket", "ux11"}});
        answer("get_edit_result", Json{{"status", "committed"}, {"committed", true},
                                       {"source_digest", {{"hex", digest}}}});
        read(document, digest);
    }
    [[nodiscard]] Json sent(int count) const {
        REQUIRE(reads("edit_graph_document") == count);
        const auto ops = Json::parse(transport.calls[static_cast<std::size_t>(
                                                         transport.idOf(QStringLiteral("edit_graph_document")) - 1)]
                                         .body.toStdString());
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].at("op") == "set_node_parameters");
        return ops[0];
    }
    template <typename T> T *field(const char *name) {
        auto *found = widget->findChild<T *>(QString::fromLatin1(name));
        REQUIRE(found != nullptr);
        return found;
    }
};

// The plan of ux5bDeferredDocument's material pass: one route to it, its
// filter as the engine resolved it, two draws.
Json ux11Plan() {
    auto pass = gl1bPass("mp", 0, "project", Json::array(),
                         Json::array({"mp__albedo", "mp__normal", "mp__material", "mp__worldpos",
                                      "mp__emissive", "mp__depth"}));
    pass["origin"] = {{"graph", "main_render"}, {"node", "mp"}};
    pass["material_filter"] = {{"include", {"hero"}}, {"exclude", Json::array()},
                               {"resolved_draw_count", 2}};
    return {{"schema", "pelican.frame_plan"},
            {"version", 1},
            {"graph", "main_render"},
            {"loops", Json::array()},
            {"material_routing",
             {{"policy", "hybrid_auto_v1"},
              {"routes", {{"deferred_geometry",
                           {{"pass", "mp"}, {"contract", "deferred_geometry_v1"},
                            {"phase", "opaque"}}}}}}},
            {"nodes", Json::array({pass})}};
}

} // namespace

TEST_CASE("UX-11 decisions 1 and 4: a material pass's Tags, outputs, bindings and attachment "
          "ops are the Pass Editor's forms on the Graph, each one set_node_parameters; "
          "Materials drawn lists the plan's routes and the materials its filter takes, and a "
          "row opens the Frame Plan's Materials",
          "[devstudio][graph-canvas][scene][ux11]") {
    (void)application();
    auto document = ux5bDeferredDocument();
    Ux11Tab tab;
    QString materials_for;
    tab.widget->setFramePlanMaterialsAction([&](const QString &pass) { materials_for = pass; });
    tab.read(document, "ux11-0");
    tab.answer("get_graph_catalog", ux5bCatalog());
    if (tab.reads("get_frame_plan") > 0) tab.answer("get_frame_plan", ux11Plan());
    tab.widget->scene().selectNode("mp");
    QCoreApplication::processEvents();
    tab.widget->poll();
    REQUIRE(tab.reads("get_render_authoring_context") >= 1);
    tab.answer("get_render_authoring_context", ux3Authoring());
    if (tab.reads("get_frame_plan") > 0) tab.answer("get_frame_plan", ux11Plan());
    tab.widget->poll();
    QCoreApplication::processEvents();
    REQUIRE(tab.widget->model().planPasses().size() == 1);

    // Decision 1: the forms are there, the Draw fields' Contract above them
    // and the form's own out of sight; no JSON row to type for them.
    CHECK(tab.field<QComboBox>("pelican.graphCanvasDraw.contract")->isVisibleTo(tab.widget.get()));
    CHECK_FALSE(tab.field<QComboBox>("pelican.graphCanvasMaterial.materialContract")
                    ->isVisibleTo(tab.widget.get()));
    auto *include = tab.field<QToolButton>("pelican.graphCanvasMaterial.materialFilter.include");
    REQUIRE(include->isVisibleTo(tab.widget.get()));
    CHECK(tab.field<QPushButton>("pelican.graphCanvasMaterial.materialOutputs.add")
              ->isVisibleTo(tab.widget.get()));
    CHECK(tab.field<QPushButton>("pelican.graphCanvasMaterial.materialResources.add")
              ->isVisibleTo(tab.widget.get()));
    // A colour row per G-buffer socket, and depth.
    CHECK(tab.field<QComboBox>("pelican.graphCanvasMaterial.outputLoad.4")->isVisibleTo(tab.widget.get()));
    CHECK(tab.widget->findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasMaterial.outputLoad.5")) ==
          nullptr);
    CHECK(tab.field<QComboBox>("pelican.graphCanvasMaterial.depthLoad")->isVisibleTo(tab.widget.get()));

    // Decision 2 on the Graph: a tag picked among the engine's.
    int edits = 0;
    tab.field<QAction>("pelican.graphCanvasMaterial.materialFilter.include.hero")->trigger();
    QCoreApplication::processEvents();
    auto op = tab.sent(++edits);
    CHECK(op.at("node") == "mp");
    auto expected = document["nodes"][6]["parameters"];
    REQUIRE(expected.at("material_contract") == "deferred_geometry_v1");
    expected["material_filter"] = {{"include", {"hero"}}};
    CHECK(op.at("parameters") == expected);
    document["nodes"][6]["parameters"] = op.at("parameters");
    tab.commit(document, "ux11-1");
    QCoreApplication::processEvents();

    // A colour row's Clear is the pass's color_load_op.
    auto *albedo_load = tab.field<QComboBox>("pelican.graphCanvasMaterial.outputLoad.0");
    albedo_load->setCurrentIndex(albedo_load->findData(QStringLiteral("clear")));
    QCoreApplication::processEvents();
    op = tab.sent(++edits);
    expected["color_load_op"] = "clear";
    CHECK(op.at("parameters") == expected);
    document["nodes"][6]["parameters"] = op.at("parameters");
    tab.commit(document, "ux11-2");
    QCoreApplication::processEvents();
    // Read back, every colour row says it.
    CHECK(tab.field<QComboBox>("pelican.graphCanvasMaterial.outputLoad.3")->currentData() ==
          QStringLiteral("clear"));

    // Depth's Load is depth_load_op.
    auto *depth_load = tab.field<QComboBox>("pelican.graphCanvasMaterial.depthLoad");
    depth_load->setCurrentIndex(depth_load->findData(QStringLiteral("clear")));
    QCoreApplication::processEvents();
    op = tab.sent(++edits);
    expected["depth_load_op"] = "clear";
    CHECK(op.at("parameters") == expected);
    document["nodes"][6]["parameters"] = op.at("parameters");
    tab.commit(document, "ux11-3");
    QCoreApplication::processEvents();

    // A binding: Add binding is a screen input, its name picked. UX-12c: it
    // goes once it is told what it reads (the engine refuses an empty one).
    tab.field<QPushButton>("pelican.graphCanvasMaterial.materialResources.add")->click();
    QCoreApplication::processEvents();
    CHECK(tab.reads("edit_graph_document") == edits);
    tab.field<QComboBox>("pelican.graphCanvasMaterial.materialResources.0.resource")
        ->setEditText(QStringLiteral("scene_color"));
    QCoreApplication::processEvents();
    op = tab.sent(++edits);
    expected["screen_inputs"] = {{"opaque_color", "scene_color"}};
    CHECK(op.at("parameters") == expected);

    // Decision 4: the route the plan names and the two materials tagged hero
    // (not the built-in one), with their tags.
    auto *drawn = tab.field<QTreeWidget>("pelican.graphCanvasMaterial.drawn");
    REQUIRE(drawn->isVisibleTo(tab.widget.get()));
    QStringList rows;
    for (int index = 0; index < drawn->topLevelItemCount(); ++index)
        rows << drawn->topLevelItem(index)->text(0) + QStringLiteral(" | ") +
                    drawn->topLevelItem(index)->text(1);
    CHECK(rows == QStringList({QStringLiteral("route deferred_geometry | deferred_geometry_v1"),
                               QStringLiteral("metal | hero"), QStringLiteral("glass | crowd, hero")}));
    CHECK(tab.field<QLabel>("pelican.graphCanvasMaterial.drawnHeading")->text().contains(
        QStringLiteral("2 draw")));
    Q_EMIT drawn->itemClicked(drawn->topLevelItem(1), 0);
    CHECK(materials_for == QStringLiteral("mp"));
}

// --- UX-12c (engine MP-1): Materials drawn from what the pass drew, and the
// binding names from the engine's list.

namespace {

// ux11Tab opened on the material pass with `plan` and `catalog`.
void ux12cOpen(Ux11Tab &tab, const Json &document, const Json &plan, const Json &catalog) {
    tab.read(document, "ux12c-0");
    tab.answer("get_graph_catalog", catalog);
    if (tab.reads("get_frame_plan") > 0) tab.answer("get_frame_plan", plan);
    tab.widget->scene().selectNode("mp");
    QCoreApplication::processEvents();
    tab.widget->poll();
    REQUIRE(tab.reads("get_render_authoring_context") >= 1);
    tab.answer("get_render_authoring_context", ux3Authoring());
    if (tab.reads("get_frame_plan") > 0) tab.answer("get_frame_plan", plan);
    tab.widget->poll();
    QCoreApplication::processEvents();
    REQUIRE(tab.widget->model().planPasses().size() == 1);
}

QStringList ux12cRows(QTreeWidget &tree) {
    QStringList rows;
    for (int index = 0; index < tree.topLevelItemCount(); ++index)
        rows << tree.topLevelItem(index)->text(0) + QStringLiteral(" | ") +
                    tree.topLevelItem(index)->text(1);
    return rows;
}

QStringList ux12cItems(const QComboBox &combo) {
    QStringList items;
    for (int index = 0; index < combo.count(); ++index) items << combo.itemText(index);
    return items;
}

} // namespace

TEST_CASE("UX-12c decision 1: Materials drawn lists the materials the plan says the pass drew "
          "in its last frame, with their draws, and falls back to the Tags' approximation, "
          "saying so, while the plan does not say",
          "[devstudio][graph-canvas][scene][ux12c]") {
    (void)application();
    const auto document = ux5bDeferredDocument();
    {
        // The engine has not said (no frame yet, or before MP-1).
        Ux11Tab tab;
        ux12cOpen(tab, document, ux11Plan(), ux5bCatalog());
        auto *drawn = tab.field<QTreeWidget>("pelican.graphCanvasMaterial.drawn");
        CHECK(ux12cRows(*drawn).contains(QStringLiteral("metal | hero")));
        const auto heading = tab.field<QLabel>("pelican.graphCanvasMaterial.drawnHeading")->text();
        CHECK(heading.contains(QStringLiteral("those its Tags and Variant take")));
        CHECK(heading.contains(QStringLiteral("the engine has not said what it drew")));
    }
    // What the pass drew: the effective materials in the order first drawn
    // (the engine's runtime ids), a built-in one marked, one the engine gave
    // no name by its id; not the materials its Tags take (glass is not drawn).
    auto plan = ux11Plan();
    plan["nodes"][0]["drawn_materials"] = Json::array(
        {{{"material", "metal"}, {"id", 3}, {"draws", 3}},
         {{"id", 7}, {"draws", 2}},
         {{"material", "pelican.builtin.transparent"}, {"id", 1}, {"draws", 1}}});
    plan["nodes"][0]["drawn_materials_frame"] = 42;
    plan["nodes"][0]["drawn_materials_truncated"] = true;
    Ux11Tab tab;
    QString materials_for;
    tab.widget->setFramePlanMaterialsAction([&](const QString &pass) { materials_for = pass; });
    ux12cOpen(tab, document, plan, ux5bCatalog());
    auto *drawn = tab.field<QTreeWidget>("pelican.graphCanvasMaterial.drawn");
    REQUIRE(drawn->isVisibleTo(tab.widget.get()));
    CHECK(ux12cRows(*drawn) ==
          QStringList({QStringLiteral("metal | 3"), QStringLiteral("material 7 | 2"),
                       QStringLiteral("pelican.builtin.transparent (built in) | 1")}));
    CHECK(drawn->headerItem()->text(1) == QStringLiteral("Draws"));
    const auto heading = tab.field<QLabel>("pelican.graphCanvasMaterial.drawnHeading")->text();
    CHECK(heading.contains(QStringLiteral("as of frame 42")));
    CHECK(heading.contains(QStringLiteral("6 draw")));
    CHECK(heading.contains(QStringLiteral("first 3 shown")));
    CHECK_FALSE(heading.contains(QStringLiteral("Tags")));
    // A row still opens the Frame Plan's Materials of the pass.
    Q_EMIT drawn->itemClicked(drawn->topLevelItem(0), 0);
    CHECK(materials_for == QStringLiteral("mp"));
}

TEST_CASE("UX-12c decision 3: a binding's names are the engine's list for its kind, and its "
          "Reads offers the targets of the type the name reads",
          "[devstudio][graph-canvas][scene][ux12c]") {
    (void)application();
    auto document = ux5bDeferredDocument();
    document["nodes"][6]["parameters"]["screen_inputs"] = {{"opaque_depth", "mp__depth"}};
    auto catalog = ux5bCatalog();
    const Json color = "pelican.render.color_signal@1";
    const Json depth = "pelican.render.depth@1";
    // lit_history is in no table of Studio's: the names are the engine's.
    catalog["material_pass_contracts"][0]["screen_inputs"] = Json::array(
        {{{"name", "opaque_color"}, {"source_type", color}},
         {{"name", "opaque_depth"}, {"source_type", depth}},
         {{"name", "lit_history"}, {"source_type", color}}});
    catalog["material_pass_contracts"][0]["surface_resources"] =
        Json::array({{{"name", "directional_shadow"}, {"source_type", depth}}});
    // The plan's targets: the graph's two images, two declared targets and
    // one whose type Studio cannot tell.
    auto plan = ux11Plan();
    plan["resources"] = Json::array();
    for (const char *name : {"mp__albedo", "mp__depth", "scene_color", "shadow_map", "mystery"})
        plan["resources"].push_back({{"kind", "render_target"}, {"name", name}, {"source", "project"}});
    Ux11Tab tab;
    ux12cOpen(tab, document, plan, catalog);
    CHECK(tab.widget->model().targetSemantic("mp__albedo") == color.get<std::string>());
    CHECK(tab.widget->model().targetSemantic("shadow_map") == depth.get<std::string>());
    CHECK(tab.widget->model().targetSemantic("mystery").empty());

    auto *name = tab.field<QComboBox>("pelican.graphCanvasMaterial.materialResources.0.name");
    auto *reads = tab.field<QComboBox>("pelican.graphCanvasMaterial.materialResources.0.resource");
    CHECK(ux12cItems(*name) == QStringList({"opaque_color", "opaque_depth", "lit_history"}));
    CHECK(name->currentText() == QStringLiteral("opaque_depth"));
    // A depth name reads a depth target (or one whose type is not known).
    CHECK(ux12cItems(*reads) == QStringList({"mp__depth", "mystery", "shadow_map"}));
    CHECK(reads->currentText() == QStringLiteral("mp__depth"));
    // A colour name, the colour ones; what the row reads stays.
    name->setCurrentIndex(name->findText(QStringLiteral("lit_history")));
    CHECK(ux12cItems(*reads) == QStringList({"mp__albedo", "mystery", "scene_color"}));
    CHECK(reads->currentText() == QStringLiteral("mp__depth"));
    // A surface resource: the engine's names for that kind.
    auto *kind = tab.field<QComboBox>("pelican.graphCanvasMaterial.materialResources.0.kind");
    kind->setCurrentIndex(kind->findData(QStringLiteral("surface_resources")));
    CHECK(ux12cItems(*name) == QStringList({"directional_shadow"}));
}

// UX-10, decision 5: no engine RPC lists a rig's clips, and the engine finds a
// clip by the name its glTF gives it, so the fields read the project's files.
TEST_CASE("UX-10 decision 5: a pose.clip_sample's rig is picked from the project's glTF files "
          "and its clip from the animations the rig's file holds; speed and loop are fields, "
          "each change one edit",
          "[devstudio][graph-canvas][scene][ux10][ux12a]") {
    (void)application();
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_ux10_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "assets");
    std::filesystem::create_directories(root / ".pelican");
    std::ofstream{root / "assets" / "hero.gltf"}
        << R"({"asset":{"version":"2.0"},"animations":[{"name":"Walk"},{"name":"Run"}]})";
    std::ofstream{root / ".pelican" / "cached.gltf"} << R"({"animations":[{"name":"No"}]})";
    {
        // A binary glTF: the header, then its JSON chunk (padded to 4 bytes).
        std::string json = R"({"asset":{"version":"2.0"},"animations":[{"name":"Idle"}]})";
        while (json.size() % 4 != 0) json.push_back(' ');
        const auto word = [](std::uint32_t value) {
            return std::string{reinterpret_cast<const char *>(&value), 4};
        };
        std::ofstream glb{root / "assets" / "crowd.glb", std::ios::binary};
        glb << "glTF" << word(2) << word(static_cast<std::uint32_t>(20 + json.size()))
            << word(static_cast<std::uint32_t>(json.size())) << word(0x4E4F534Au) << json;
    }
    CHECK(PelicanStudio::projectRigReferences(root) ==
          std::vector<std::string>{"project://assets/crowd.glb", "project://assets/hero.gltf"});
    CHECK(PelicanStudio::gltfAnimationNames(root / "assets" / "hero.gltf") ==
          std::vector<std::string>{"Walk", "Run"});
    CHECK(PelicanStudio::gltfAnimationNames(root / "assets" / "crowd.glb") ==
          std::vector<std::string>{"Idle"});
    {
        Ux2Tab tab{false};
        tab.widget->setProjectRoot(root);
        auto parameters = Json{{"rig", "project://assets/hero.gltf"}, {"clip", "Walk"}};
        const auto document = [&] {
            return Json{{"schema", "pelican.graph"},
                        {"version", 1},
                        {"definitions", Json::object()},
                        {"nodes", Json::array({{{"id", "walk"},
                                                {"op", "pose.clip_sample"},
                                                {"parameters", parameters},
                                                {"outputs", Json::array({"pose"})}}})}};
        };
        tab.read(document(), "ux10-0");
        // UX-12a, decision 5: the fields are the table's (engine CT-3).
        tab.answer("get_graph_catalog", engineCatalog());
        tab.widget->scene().selectNode("walk");
        QCoreApplication::processEvents();
        REQUIRE(tab.field<QWidget>("pelican.graphCanvasPose")->isVisible());
        auto *rig = tab.field<QComboBox>("pelican.graphCanvasPose.rig");
        CHECK(ux2Items(*rig) ==
              QStringList{"project://assets/crowd.glb", "project://assets/hero.gltf"});
        CHECK(rig->currentText() == QStringLiteral("project://assets/hero.gltf"));
        auto *clip = tab.field<QComboBox>("pelican.graphCanvasPose.clip");
        CHECK(ux2Items(*clip) == QStringList{"Walk", "Run"});
        CHECK(clip->currentText() == QStringLiteral("Walk"));
        // Unwritten: the engine's defaults.
        CHECK(tab.field<QDoubleSpinBox>("pelican.graphCanvasPose.speed")->value() == 1.0);
        CHECK(tab.field<QCheckBox>("pelican.graphCanvasPose.loop")->isChecked());
        // UX-12a, decision 5: and the rest the table lists.
        CHECK(tab.field<QDoubleSpinBox>("pelican.graphCanvasPose.start_time")->value() == 0.0);
        CHECK(tab.field<QComboBox>("pelican.graphCanvasPose.root_motion")->currentData() ==
              QStringLiteral("none"));
        CHECK(tab.field<QDoubleSpinBox>("pelican.graphCanvasPose.root_node")->decimals() == 0);
        // The generic fields leave the pose's to them.
        CHECK_FALSE(tab.field<QWidget>("pelican.graphCanvasParameters")->isVisible());

        tab.pick(clip, QStringLiteral("Run"));
        auto op = tab.sent(1);
        CHECK(op.at("node") == "walk");
        parameters["clip"] = "Run";
        CHECK(op.at("parameters") == parameters);
        tab.commit(document(), "ux10-1");
        // The node is picked again after each read.
        const auto pickWalk = [&] {
            tab.widget->scene().selectNode("walk");
            QCoreApplication::processEvents();
        };
        pickWalk();
        CHECK(tab.field<QDoubleSpinBox>("pelican.graphCanvasPose.speed")->isEnabled());

        tab.field<QDoubleSpinBox>("pelican.graphCanvasPose.speed")->setValue(2.0);
        QCoreApplication::processEvents();
        // A spin's steps are gathered over 150 ms (UX-12a).
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (tab.reads("edit_graph_document") < 2 && std::chrono::steady_clock::now() < until) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        op = tab.sent(2);
        parameters["speed"] = 2.0;
        CHECK(op.at("parameters") == parameters);
        tab.commit(document(), "ux10-2");
        pickWalk();

        tab.field<QCheckBox>("pelican.graphCanvasPose.loop")->setChecked(false);
        QCoreApplication::processEvents();
        op = tab.sent(3);
        parameters["loop"] = false;
        CHECK(op.at("parameters") == parameters);
        tab.commit(document(), "ux10-3");
        pickWalk();

        // Another rig lists its own clips; the clip it names stays shown, marked.
        tab.pick(rig, QStringLiteral("project://assets/crowd.glb"));
        op = tab.sent(4);
        CHECK(op.at("parameters").at("rig") == "project://assets/crowd.glb");
        CHECK(ux2Items(*clip) == QStringList{"Run", "Idle"});
        CHECK(tab.field<QLabel>("pelican.graphCanvasPose.note")
                  ->text()
                  .contains(QStringLiteral("Run is not an animation of")));
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

// --- UX-12a (engine CT-3): a node's fields from the catalog's table.

namespace {

// Lets the event loop run until `done` holds or `ms` pass (the 150 ms a spin's
// steps are gathered over).
bool ux12aWait(const std::function<bool()> &done, int ms = 2000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{ms};
    while (std::chrono::steady_clock::now() < until) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    QCoreApplication::processEvents();
    return done();
}

Json ux12aBufferDocument(const Json &parameters) {
    return Json{{"schema", "pelican.graph"},
                {"version", 1},
                {"definitions", Json::object()},
                {"nodes", Json::array({{{"id", "buf"},
                                        {"op", "render.buffer"},
                                        {"parameters", parameters},
                                        {"outputs", Json::array({"resource"})}}})}};
}

} // namespace

TEST_CASE("UX-12a decision 1: a render.buffer's parameters are fields made from the catalog's "
          "table -- an enum a combo, an integer a spin, a bool a check, an object folded, an array "
          "rows -- each change one set_node_parameters (a spin's steps gathered), Reset back to the "
          "engine's default",
          "[devstudio][graph-canvas][scene][ux12a]") {
    (void)application();
    Ux2Tab tab{false};
    auto parameters = Json{{"element", "vec4"}, {"size", 16}};
    tab.read(ux12aBufferDocument(parameters), "ux12a-0");
    tab.answer("get_graph_catalog", engineCatalog());
    const auto pick = [&] {
        tab.widget->scene().selectNode("buf");
        QCoreApplication::processEvents();
    };
    pick();
    REQUIRE(tab.field<QWidget>("pelican.graphCanvasParameters")->isVisible());
    auto *element = tab.field<QComboBox>("pelican.graphCanvasParameters.element");
    CHECK(element->currentData().toString() == QStringLiteral("vec4"));
    CHECK(element->findData(QStringLiteral("mat4")) >= 0);
    auto *size = tab.field<QDoubleSpinBox>("pelican.graphCanvasParameters.size");
    CHECK(size->value() == 16.0);
    CHECK(size->decimals() == 0);
    // Unwritten: the engine's default, marked.
    auto *lifetime = tab.field<QComboBox>("pelican.graphCanvasParameters.lifetime");
    CHECK(lifetime->currentData().toString() == QStringLiteral("persistent"));
    CHECK(lifetime->currentText().contains(QStringLiteral("default")));
    CHECK_FALSE(tab.field<QToolButton>("pelican.graphCanvasParameters.lifetime.reset")->isVisible());
    CHECK_FALSE(tab.field<QCheckBox>("pelican.graphCanvasParameters.index_buffer")->isChecked());
    // The required one is marked.
    CHECK(tab.field<QLabel>("pelican.graphCanvasParameters.element.label")->text().endsWith(
        QStringLiteral("*")));
    // An object's fields fold under it; an array is rows with +.
    auto *fold = tab.field<QToolButton>("pelican.graphCanvasParameters.size_from_extent.fold");
    CHECK_FALSE(tab.field<QWidget>("pelican.graphCanvasParameters.size_from_extent.tile_width")
                    ->isVisible());
    fold->click();
    CHECK(tab.field<QWidget>("pelican.graphCanvasParameters.size_from_extent.tile_width")->isVisible());
    CHECK(tab.field<QPushButton>("pelican.graphCanvasParameters.initial_f32.add")->isVisible());

    // A pick is one edit of the whole parameters.
    tab.pick(lifetime, QStringLiteral("transient"));
    auto op = tab.sent(1);
    CHECK(op.at("node") == "buf");
    parameters["lifetime"] = "transient";
    CHECK(op.at("parameters") == parameters);
    tab.commit(ux12aBufferDocument(parameters), "ux12a-1");
    pick();
    CHECK(tab.field<QToolButton>("pelican.graphCanvasParameters.lifetime.reset")->isVisible());

    // A spin's steps are gathered into one edit, an integer sent as one.
    CHECK(tab.field<QDoubleSpinBox>("pelican.graphCanvasParameters.size") == size);
    CHECK(tab.field<QWidget>("pelican.graphCanvasParameters")->isVisible());
    CHECK(size->isVisible());
    size->setValue(32.0);
    size->setValue(64.0);
    CHECK(tab.reads("edit_graph_document") == 1);
    REQUIRE(ux12aWait([&] { return tab.reads("edit_graph_document") == 2; }));
    op = tab.sent(2);
    parameters["size"] = 64;
    CHECK(op.at("parameters") == parameters);
    CHECK(op.at("parameters").at("size").is_number_integer());
    tab.commit(ux12aBufferDocument(parameters), "ux12a-2");
    pick();

    // Reset takes the key out: the engine's default applies.
    tab.field<QToolButton>("pelican.graphCanvasParameters.lifetime.reset")->click();
    QCoreApplication::processEvents();
    op = tab.sent(3);
    parameters.erase("lifetime");
    CHECK(op.at("parameters") == parameters);

    // An op with fields of its own lists only the rest, folded.
    CHECK(PelicanStudio::GraphCanvasParameterFields::hasOwnFields("render.image"));
    CHECK_FALSE(PelicanStudio::GraphCanvasParameterFields::hasOwnFields("render.buffer"));
    const auto rest = PelicanStudio::GraphCanvasParameterFields::ownFieldNames("value.playback");
    CHECK(std::ranges::find(rest, "sample_interval") == rest.end());
}

TEST_CASE("UX-12a decision 2: the palette's dialog is made from the table -- pose.clip_sample asks "
          "for its rig among the project's glTF files and its clip among the rig's animations, "
          "and is placed with them",
          "[devstudio][graph-canvas][scene][ux12a]") {
    (void)application();
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_ux12a_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "assets");
    std::ofstream{root / "assets" / "hero.gltf"}
        << R"({"asset":{"version":"2.0"},"animations":[{"name":"Walk"},{"name":"Run"}]})";
    {
        GraphCanvasModel model;
        model.setConnected(true);
        answer(model, ux3Document(), engineCatalog());
        REQUIRE(model.canEdit());
        // The table's required entries, less what the palette seeds itself.
        const auto names = [&](const char *op) {
            std::vector<std::string> result;
            for (const auto &each : model.requiredParametersFor(op))
                result.push_back(each.name + ":" + each.kind);
            return result;
        };
        CHECK(names("pose.clip_sample") == std::vector<std::string>{"rig:asset", "clip:string"});
        CHECK(names("value.constant") == std::vector<std::string>{"value:number"});
        CHECK(names("render.buffer").empty());
        const auto volume = model.requiredParametersFor("audio.volume");
        REQUIRE(volume.size() == 1);
        CHECK(volume[0].edge_alternatives == std::vector<std::string>{"voice"});

        PelicanStudio::GraphCanvasRequiredDialog ask{model, {}, "pose.clip_sample", nullptr, root};
        REQUIRE(ask.asks());
        auto *rig = ask.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasRequired.rig"));
        REQUIRE(rig != nullptr);
        CHECK(ux2Items(*rig) == QStringList{"(choose)", "project://assets/hero.gltf"});
        CHECK(ask.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasRequired.rig.import")) !=
              nullptr);
        auto *ok = ask.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasRequired.ok"));
        REQUIRE(ok != nullptr);
        CHECK_FALSE(ok->isEnabled());
        rig->setCurrentIndex(rig->findData(QStringLiteral("project://assets/hero.gltf")));
        auto *clip = ask.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasRequired.clip"));
        REQUIRE(clip != nullptr);
        CHECK(ux2Items(*clip) == QStringList{"(choose)", "Walk", "Run"});
        CHECK_FALSE(ok->isEnabled());
        clip->setCurrentIndex(clip->findData(QStringLiteral("Run")));
        CHECK(ok->isEnabled());
        const Json parameters{{"rig", "project://assets/hero.gltf"}, {"clip", "Run"}};
        CHECK(ask.parameters() == parameters);
        const auto ops = model.addNodeOpsIn({}, "pose.clip_sample", {}, ask.seed());
        REQUIRE_FALSE(ops.empty());
        CHECK(ops.back().at("node").at("parameters") == parameters);
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("UX-12a decision 3: a value pill's editor is chosen by the table's kind before the "
          "stored value's JSON type",
          "[devstudio][graph-canvas][scene][ux12a]") {
    (void)application();
    Ux2Tab tab{false};
    // A trigger clock's `paused` holds a string: the table says it is a number.
    const Json document{{"schema", "pelican.graph"},
                        {"version", 1},
                        {"definitions", Json::object()},
                        {"nodes", Json::array({{{"id", "clock"},
                                                {"op", "value.trigger_clock"},
                                                {"parameters", {{"paused", "x"}}},
                                                {"outputs", Json::array({"value"})}}})}};
    tab.read(document, "ux12a-v0");
    tab.answer("get_graph_catalog", engineCatalog());
    // The root draws its value nodes once asked to (GL-1).
    tab.field<QCheckBox>("pelican.graphCanvasShow.value")->setChecked(true);
    QCoreApplication::processEvents();
    REQUIRE(tab.widget->scene().beginValueEdit(std::string{}, "clock", "paused"));
    QDoubleSpinBox *spin = nullptr;
    for (QGraphicsItem *item : tab.widget->scene().items())
        if (auto *proxy = dynamic_cast<QGraphicsProxyWidget *>(item))
            if (auto *found = qobject_cast<QDoubleSpinBox *>(proxy->widget())) spin = found;
    REQUIRE(spin != nullptr);
    spin->setValue(1.0);
    REQUIRE(tab.widget->scene().commitInlineEdit());
    QCoreApplication::processEvents();
    const auto op = tab.sent(1);
    CHECK(op.at("parameters") == Json{{"paused", 1.0}});
}

TEST_CASE("UX-12a decision 4: graph.render_node_parameter_invalid@1 reads as one sentence and "
          "the field it names is red with it",
          "[devstudio][graph-canvas][scene][ux12a]") {
    (void)application();
    CHECK(PelicanStudio::graphLoopRefusalDescription(
              {{"code", "graph.render_node_parameter_invalid@1"},
               {"node", "buf"},
               {"op", "render.buffer"},
               {"parameter", "lifetime"},
               {"expected", "enum"},
               {"reason", "enum"},
               {"allowed", {"persistent", "transient"}}}) ==
          "buf.lifetime must be enum (one of: persistent, transient) "
          "(graph.render_node_parameter_invalid@1).");
    CHECK(PelicanStudio::graphLoopRefusalDescription(
              {{"code", "graph.render_node_parameter_invalid@1"},
               {"node", "clip"},
               {"op", "pose.clip_sample"},
               {"parameter", "speed"},
               {"expected", "number"},
               {"reason", "type"},
               {"document", "project://graphs/main.json"}}) ==
          "clip.speed must be number (wrong type) (in graphs/main.json, "
          "graph.render_node_parameter_invalid@1).");

    Ux2Tab tab{false};
    auto parameters = Json{{"element", "vec4"}, {"size", 16}};
    tab.read(ux12aBufferDocument(parameters), "ux12a-r0");
    tab.answer("get_graph_catalog", engineCatalog());
    tab.widget->scene().selectNode("buf");
    QCoreApplication::processEvents();
    auto *size = tab.field<QDoubleSpinBox>("pelican.graphCanvasParameters.size");
    // Not held to the range: the engine is the one that says what is wrong.
    size->setValue(-4.0);
    REQUIRE(ux12aWait([&] { return tab.reads("edit_graph_document") == 1; }));
    CHECK(tab.sent(1).at("parameters").at("size") == -4);
    tab.answer("edit_graph_document",
               Json{{"error",
                     {{"code", "graph.render_node_parameter_invalid@1"},
                      {"message", "graph.render_node_parameter_invalid@1"},
                      {"node", "buf"},
                      {"op", "render.buffer"},
                      {"parameter", "size"},
                      {"expected", "integer"},
                      {"reason", "range"},
                      {"min", 0}}}});
    if (tab.reads("get_graph_instances") > 0) tab.read(ux12aBufferDocument(parameters), "ux12a-r0");
    tab.widget->scene().selectNode("buf");
    QCoreApplication::processEvents();
    const auto sentence = QStringLiteral("buf.size must be integer (range: at least 0)");
    CHECK(tab.field<QLabel>("pelican.graphCanvasNotice")->text().contains(sentence));
    auto *problem = tab.field<QLabel>("pelican.graphCanvasParameters.size.problem");
    CHECK(problem->isVisible());
    CHECK(problem->text().contains(sentence));
    CHECK(tab.field<QLabel>("pelican.graphCanvasParameters.size.label")->styleSheet().contains(
        QStringLiteral("#d94c3d")));
    CHECK_FALSE(tab.field<QLabel>("pelican.graphCanvasParameters.element.problem")->isVisible());
}

// --- UX-12e (engine CT-4): the generic fields' remaining strings, assets,
// unions, usual ranges and placement.

namespace {

Json ux12eDocument(const std::string &name) {
    return Json{{"schema", "pelican.graph"},
                {"version", 1},
                {"definitions", Json::object()},
                {"nodes", Json::array({{{"id", "ref"},
                                        {"op", "render.target_ref"},
                                        {"parameters", {{"name", name}}},
                                        {"outputs", Json::array({"resource"})}},
                                       {{"id", "voice"},
                                        {"op", "audio.playback"},
                                        {"parameters", {{"asset", "project://assets/audio/a.wav"}}},
                                        {"outputs", Json::array({"voice"})}},
                                       {{"id", "loud"},
                                        {"op", "audio.volume"},
                                        {"parameters", {{"target", "voice"}}},
                                        {"outputs", Json::array()}}})}};
}

// The catalog's entry, as the engine's table gives it.
Json ux12eSchema(const GraphCanvasModel &model, const char *op, const char *name) {
    const auto schema = model.parameterSchema(op, name);
    REQUIRE(schema.has_value());
    return *schema;
}

} // namespace

TEST_CASE("UX-12e decision 1: a string the table says is chosen from somewhere is an editable "
          "combo of that list -- the authoring context read once per reading of the document, "
          "the scope's nodes of the named ops -- and a name typed off the list is kept, marked",
          "[devstudio][graph-canvas][scene][ux12e]") {
    (void)application();
    Ux2Tab tab{true};
    tab.read(ux12eDocument("scene_color"), "ux12e-0");
    tab.answer("get_graph_catalog", engineCatalog());
    const auto pick = [&](const char *id) {
        tab.widget->scene().selectNode(id);
        QCoreApplication::processEvents();
    };
    pick("ref");
    tab.widget->poll();
    REQUIRE(tab.reads("get_render_authoring_context") == 1);
    tab.answer("get_render_authoring_context",
               Json{{"render_targets", Json::array({{{"name", "scene_color"}}, {{"name", "scene_depth"}}})}});
    tab.widget->poll();
    QCoreApplication::processEvents();
    auto *name = tab.field<QComboBox>("pelican.graphCanvasParameters.name");
    CHECK(name->isEditable());
    CHECK(ux2Items(*name) == QStringList{"scene_color", "scene_depth"});
    CHECK(name->currentText() == QStringLiteral("scene_color"));
    CHECK_FALSE(tab.field<QLabel>("pelican.graphCanvasParameters.name.unlisted")->isVisible());
    // Not asked again for another field or the same one on this reading.
    pick("voice");
    pick("ref");
    tab.widget->poll();
    CHECK(tab.reads("get_render_authoring_context") == 1);

    // Picked from the list: one set_node_parameters.
    name = tab.field<QComboBox>("pelican.graphCanvasParameters.name");
    CHECK(ux2Items(*name) == QStringList{"scene_color", "scene_depth"});
    name->setCurrentIndex(name->findText(QStringLiteral("scene_depth")));
    QCoreApplication::processEvents();
    CHECK(tab.sent(1).at("parameters") == Json{{"name", "scene_depth"}});
    tab.commit(ux12eDocument("scene_depth"), "ux12e-1");
    pick("ref");
    tab.widget->poll();
    // Another reading of the document: asked once more.
    CHECK(tab.reads("get_render_authoring_context") == 2);

    // Typed, off the list: kept as typed, and marked.
    name = tab.field<QComboBox>("pelican.graphCanvasParameters.name");
    name->lineEdit()->setText(QStringLiteral("my_target"));
    Q_EMIT name->lineEdit()->editingFinished();
    QCoreApplication::processEvents();
    CHECK(tab.sent(2).at("parameters") == Json{{"name", "my_target"}});
    tab.commit(ux12eDocument("my_target"), "ux12e-2");
    pick("ref");
    CHECK(tab.field<QComboBox>("pelican.graphCanvasParameters.name")->currentText() ==
          QStringLiteral("my_target"));
    CHECK(tab.field<QLabel>("pelican.graphCanvasParameters.name.unlisted")->isVisible());

    // A sound sink's target: the scope's voices (graph_nodes audio.playback).
    pick("loud");
    auto *target = tab.field<QComboBox>("pelican.graphCanvasParameters.target");
    CHECK(target->isEditable());
    CHECK(ux2Items(*target) == QStringList{"voice"});
    CHECK(target->currentText() == QStringLiteral("voice"));
}

TEST_CASE("UX-12e decision 2: an asset parameter lists the project's files of the kinds its "
          "table entry names, with Import... beside it",
          "[devstudio][graph-canvas][scene][ux12e]") {
    (void)application();
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_ux12e_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "assets");
    for (const char *file : {"a.wav", "b.ogg", "hero.glb", "pic.png", "bands.ktx2", "notes.txt"})
        std::ofstream{root / "assets" / file} << "x";
    {
        GraphCanvasModel model;
        model.setConnected(true);
        answer(model, ux3Document(), engineCatalog());
        auto choices = std::make_shared<PelicanStudio::GraphParameterChoices>();
        choices->model = &model;
        choices->project_root = root;
        const auto listed = [&](const char *op, const char *name) {
            choices->op = op;
            PelicanStudio::GraphParameterEditor editor{ux12eSchema(model, op, name), choices,
                                                       QStringLiteral("asset"), {}};
            auto *combo = editor.findChild<QComboBox *>(QStringLiteral("asset"));
            REQUIRE(combo != nullptr);
            CHECK(editor.findChild<QPushButton *>(QStringLiteral("asset.import")) != nullptr);
            return ux2Items(*combo);
        };
        CHECK(PelicanStudio::assetParameterExtensions(ux12eSchema(model, "value.sample_table", "asset")) ==
              std::vector<std::string>{".ktx2"});
        CHECK(listed("value.sample_table", "asset") ==
              QStringList{"(choose)", "project://assets/bands.ktx2"});
        CHECK(listed("audio.playback", "asset") ==
              QStringList{"(choose)", "project://assets/a.wav", "project://assets/b.ogg"});
        CHECK(listed("pose.clip_sample", "rig") == QStringList{"(choose)", "project://assets/hero.glb"});
        // A field of an object: render.image's initial contents, images only.
        choices->op = "render.image";
        PelicanStudio::GraphParameterEditor initial{ux12eSchema(model, "render.image", "initial"), choices,
                                                    QStringLiteral("initial"), {}};
        auto *asset = initial.findChild<QComboBox *>(QStringLiteral("initial.asset"));
        REQUIRE(asset != nullptr);
        CHECK(ux2Items(*asset) ==
              QStringList{"(choose)", "project://assets/bands.ktx2", "project://assets/pic.png"});
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("UX-12e decision 3: a union is a combo of its shapes picked from the value's type -- "
          "ray_query Off / On / Custom, mip_levels Count / Full chain, a sample row Index / Name",
          "[devstudio][graph-canvas][scene][ux12e]") {
    (void)application();
    GraphCanvasModel model;
    model.setConnected(true);
    answer(model, ux3Document(), engineCatalog());
    auto choices = std::make_shared<PelicanStudio::GraphParameterChoices>();
    choices->model = &model;

    PelicanStudio::GraphParameterEditor ray{ux12eSchema(model, "render.fullscreen", "ray_query"), choices,
                                            QStringLiteral("rq"), {}};
    auto *shape = ray.findChild<QComboBox *>(QStringLiteral("rq"));
    REQUIRE(shape != nullptr);
    CHECK(ux2Items(*shape) == QStringList{"Off", "On", "Custom"});
    // Not written: the engine's default, false.
    CHECK(shape->currentText() == QStringLiteral("Off"));
    ray.setValue(Json{{"acceleration_structure", "tlas"}});
    CHECK(shape->currentText() == QStringLiteral("Custom"));
    CHECK_FALSE(ray.findChild<QWidget *>(QStringLiteral("rq.object.editor"))->isHidden());
    int edits = 0;
    ray.setEdited([&](bool) { ++edits; });
    shape->setCurrentIndex(shape->findText(QStringLiteral("On")));
    CHECK(edits == 1);
    CHECK(ray.value() == true);
    CHECK(ray.findChild<QWidget *>(QStringLiteral("rq.object.editor"))->isHidden());

    PelicanStudio::GraphParameterEditor mips{ux12eSchema(model, "render.image", "mip_levels"), choices,
                                             QStringLiteral("mips"), {}};
    auto *mip_shape = mips.findChild<QComboBox *>(QStringLiteral("mips"));
    REQUIRE(mip_shape != nullptr);
    CHECK(ux2Items(*mip_shape) == QStringList{"(none)", "Count", "Full chain"});
    mips.setValue(4);
    CHECK(mip_shape->currentText() == QStringLiteral("Count"));
    CHECK(mips.findChild<QDoubleSpinBox *>(QStringLiteral("mips.integer"))->value() == 4.0);
    mip_shape->setCurrentIndex(mip_shape->findText(QStringLiteral("Full chain")));
    CHECK(mips.value() == "full");
    mips.setValue("full");
    CHECK(mip_shape->currentText() == QStringLiteral("Full chain"));
    mip_shape->setCurrentIndex(mip_shape->findText(QStringLiteral("Count")));
    CHECK(mips.value() == 1);

    PelicanStudio::GraphParameterEditor row{ux12eSchema(model, "value.sample_table", "row"), choices,
                                            QStringLiteral("row"), {}};
    auto *row_shape = row.findChild<QComboBox *>(QStringLiteral("row"));
    REQUIRE(row_shape != nullptr);
    CHECK(ux2Items(*row_shape) == QStringList{"Index", "Name"});
    row.setValue("rms");
    CHECK(row_shape->currentText() == QStringLiteral("Name"));
    // UX-13b (engine CT-5): the name is chosen from the table's manifest.
    auto *row_name = row.findChild<QComboBox *>(QStringLiteral("row.string"));
    REQUIRE(row_name != nullptr);
    CHECK(row_name->currentText() == QStringLiteral("rms"));
}

TEST_CASE("UX-12e decision 4: a number's slider goes up to the table's soft_max, the spin past it, "
          "and a value above it is marked",
          "[devstudio][graph-canvas][scene][ux12e]") {
    (void)application();
    GraphCanvasModel model;
    model.setConnected(true);
    answer(model, ux3Document(), engineCatalog());
    auto choices = std::make_shared<PelicanStudio::GraphParameterChoices>();
    choices->model = &model;
    PelicanStudio::GraphParameterEditor width{ux12eSchema(model, "render.image", "width"), choices,
                                              QStringLiteral("w"), {}};
    auto *slider = width.findChild<QSlider *>(QStringLiteral("w.slider"));
    REQUIRE(slider != nullptr);
    auto *spin = width.findChild<QDoubleSpinBox *>(QStringLiteral("w"));
    REQUIRE(spin != nullptr);
    auto *unusual = width.findChild<QLabel *>(QStringLiteral("w.unusual"));
    REQUIRE(unusual != nullptr);
    width.setValue(8192);
    CHECK(slider->value() == slider->maximum());
    CHECK(unusual->isHidden());
    width.setValue(16384);
    CHECK(spin->value() == 16384.0);
    CHECK_FALSE(unusual->isHidden());
    CHECK(unusual->text() == QStringLiteral("above the usual range"));
    width.setValue(512);
    CHECK(unusual->isHidden());
}

TEST_CASE("UX-12e decision 5: in the root the palette greys an op that needs an owned instance, "
          "and its New definition... makes one, goes in and opens the palette there on that op",
          "[devstudio][graph-canvas][scene][ux12e]") {
    (void)application();
    CHECK(PelicanStudio::graphLoopRefusalDescription({{"code", "graph.op_requires_owned_instance@1"},
                                                      {"node", "clip"},
                                                      {"op", "pose.clip_sample"}}) ==
          "pose.clip_sample runs inside a definition attached to an object \xE2\x80\x94 make a "
          "definition and place it there (graph.op_requires_owned_instance@1).");

    Ux2Tab tab{false};
    auto document = ux12aBufferDocument(Json{{"element", "float"}, {"size", 4}});
    tab.read(document, "ux12e-p0");
    tab.answer("get_graph_catalog", engineCatalog());
    CHECK(tab.widget->model().operationPlacement("pose.clip_sample") == "owned_instance");
    CHECK(tab.widget->model().operationPlacement("value.add") == "anywhere");
    const auto openPalette = [&] {
        tab.field<QPushButton>("pelican.graphCanvasAddNode")->click();
        QDialog *palette = nullptr;
        for (int tick = 0; tick < 200 && palette == nullptr; ++tick) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            palette = paletteOnScreen();
        }
        return palette;
    };
    auto *palette = openPalette();
    REQUIRE(palette != nullptr);
    auto *filter = palette->findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasPaletteFilter"));
    auto *list = palette->findChild<QListWidget *>(QStringLiteral("pelican.graphCanvasPaletteList"));
    REQUIRE(filter != nullptr);
    REQUIRE(list != nullptr);
    filter->setText(QStringLiteral("pose.clip_sample"));
    REQUIRE(list->currentItem() != nullptr);
    CHECK(list->currentItem()->text().contains(QStringLiteral("needs a definition")));
    CHECK(list->currentItem()->toolTip().contains(QStringLiteral("New definition")));
    auto *buttons = palette->findChild<QDialogButtonBox *>();
    REQUIRE(buttons != nullptr);
    CHECK_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    auto *offer = palette->findChild<QToolButton *>(
        QStringLiteral("pelican.graphCanvasPaletteNeedsDefinition.pose.clip_sample"));
    REQUIRE(offer != nullptr);
    // An op that stands anywhere is not marked.
    filter->setText(QStringLiteral("value.add"));
    REQUIRE(list->currentItem() != nullptr);
    CHECK_FALSE(list->currentItem()->text().contains(QStringLiteral("needs a definition")));
    filter->setText(QStringLiteral("pose.clip_sample"));
    offer = palette->findChild<QToolButton *>(
        QStringLiteral("pelican.graphCanvasPaletteNeedsDefinition.pose.clip_sample"));
    REQUIRE(offer != nullptr);
    offer->click();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    CHECK(paletteOnScreen() == nullptr);

    // UX-8's New definition... row, on a name Studio proposes.
    auto *name = tab.field<QLineEdit>("pelican.graphCanvasDefinitionName");
    REQUIRE(name->isVisible());
    name->setText(QStringLiteral("walk"));
    sendKey(name, Qt::Key_Return);
    QCoreApplication::processEvents();
    REQUIRE(tab.reads("edit_graph_document") == 1);
    document["definitions"]["walk"] = Json{{"nodes", Json::array()}, {"outputs", Json::object()}};
    tab.commit(document, "ux12e-p1");
    palette = nullptr;
    for (int tick = 0; tick < 200 && palette == nullptr; ++tick) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        palette = paletteOnScreen();
    }
    REQUIRE(palette != nullptr);
    CHECK(tab.widget->model().currentScope() == "walk");
    filter = palette->findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasPaletteFilter"));
    list = palette->findChild<QListWidget *>(QStringLiteral("pelican.graphCanvasPaletteList"));
    CHECK(filter->text() == QStringLiteral("pose.clip_sample"));
    REQUIRE(list->currentItem() != nullptr);
    CHECK(list->currentItem()->data(Qt::UserRole).toString() == QStringLiteral("pose.clip_sample"));
    CHECK_FALSE(list->currentItem()->text().contains(QStringLiteral("needs a definition")));
    CHECK(palette->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
    palette->reject();
    QCoreApplication::processEvents();
}

// --- UX-13b (engine CT-5): where a value comes from, with conditions.

TEST_CASE("UX-13b decision 1: a list narrowed by where, one read from the catalog Studio holds, and "
          "one read from the import manifest beside the asset its sibling names, again when it changes",
          "[devstudio][graph-canvas][scene][ux13b]") {
    (void)application();
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_ux13b_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "assets" / "analysis");
    const auto manifest = [&](const char *name, const Json &features) {
        std::ofstream{root / "assets" / "analysis" / (std::string{name} + ".ktx2.import.json")}
            << Json{{"source", {{"metadata", {{"layout", {{"features", features}}}}}}}}.dump();
    };
    manifest("tone", Json{"rms", "peak"});
    manifest("kick", Json{"band:0", "band:1"});
    {
        auto document = ux3Document();
        const auto buffer = [&](const char *id, const Json &parameters) {
            document["nodes"].push_back({{"id", id}, {"op", "render.buffer"}, {"parameters", parameters},
                                         {"outputs", Json::array({"resource"})}});
        };
        buffer("ready", {{"element", "float"}, {"size", 4}, {"host_source", "runtime_f32"}});
        buffer("plain", {{"element", "vec4"}, {"size", 16}});
        buffer("counts", {{"element", "uint"}, {"size", 4}, {"host_source", "runtime_f32"}});
        GraphCanvasModel model;
        model.setConnected(true);
        answer(model, document, engineCatalog());
        auto choices = std::make_shared<PelicanStudio::GraphParameterChoices>();
        choices->model = &model;
        choices->project_root = root;

        // where: a runtime_f32 buffer of float elements (a host_source not
        // written is its table's default: none).
        choices->op = "render.scalar";
        PelicanStudio::GraphParameterEditor target{ux12eSchema(model, "render.scalar", "target"), choices,
                                                   QStringLiteral("target"), {}};
        auto *buffers = target.findChild<QComboBox *>(QStringLiteral("target"));
        REQUIRE(buffers != nullptr);
        CHECK(ux2Items(*buffers) == QStringList{"ready"});

        // get_graph_catalog: the image types of the catalog Studio holds.
        choices->op = "render.image";
        PelicanStudio::GraphParameterEditor type{ux12eSchema(model, "render.image", "type"), choices,
                                                 QStringLiteral("type"), {}};
        auto *semantic = type.findChild<QComboBox *>(QStringLiteral("type.semantic"));
        REQUIRE(semantic != nullptr);
        const auto types = ux2Items(*semantic);
        CHECK(types.contains(QStringLiteral("pelican.render.color_signal@1")));
        CHECK(types.contains(QStringLiteral("pelican.render.depth@1")));
        CHECK_FALSE(types.contains(QStringLiteral("pelican.value.scalar@1")));

        // asset_manifest: the rows the table beside the asset lists.
        choices->op = "value.sample_table";
        PelicanStudio::GraphParameterEditor row{ux12eSchema(model, "value.sample_table", "row"), choices,
                                                QStringLiteral("row"), {}};
        row.setValue("rms");
        row.refreshChoices(Json{{"asset", "project://assets/analysis/tone.ktx2"}, {"row", "rms"}});
        auto *name = row.findChild<QComboBox *>(QStringLiteral("row.string"));
        REQUIRE(name != nullptr);
        CHECK(ux2Items(*name) == QStringList{"rms", "peak"});
        CHECK(row.findChild<QLabel *>(QStringLiteral("row.string.unlisted"))->isHidden());
        // Another table: its rows; the row kept, marked.
        row.refreshChoices(Json{{"asset", "project://assets/analysis/kick.ktx2"}, {"row", "rms"}});
        CHECK(ux2Items(*name) == QStringList{"band:0", "band:1"});
        CHECK_FALSE(row.findChild<QLabel *>(QStringLiteral("row.string.unlisted"))->isHidden());
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("UX-13b decision 4: in a definition an instance with no owner also runs, the palette greys an "
          "op that needs an owned instance and says why; in one only owned instances run, it does not",
          "[devstudio][graph-canvas][scene][ux13b]") {
    (void)application();
    Ux2Tab tab{false};
    auto document = ux12aBufferDocument(Json{{"element", "float"}, {"size", 4}});
    document["definitions"]["walk"] = Json{{"nodes", Json::array()}, {"outputs", Json::object()}};
    document["nodes"].push_back({{"id", "walker"}, {"op", "graph.instance"}, {"definition", "walk"},
                                 {"parameters", Json::object()}, {"outputs", Json::array()}});
    const auto readWith = [&](bool owned_only, const char *digest) {
        auto reply = context(document);
        reply["source_digest"]["hex"] = digest;
        reply["definition_instances"] = {
            {"walk", {{"owned_instances_only", owned_only},
                      {"instances", Json::array({{{"id", "walker"}, {"op", "graph.instance"},
                                                  {"owned", owned_only}}})}}}};
        tab.answer("get_graph_instances", reply);
    };
    readWith(false, "ux13b-d0");
    tab.answer("get_graph_catalog", engineCatalog());
    CHECK_FALSE(tab.widget->model().definitionOwnedInstancesOnly("walk"));
    REQUIRE(tab.widget->model().selectScope("walk"));
    QCoreApplication::processEvents();
    const auto openPalette = [&] {
        tab.field<QPushButton>("pelican.graphCanvasAddNode")->click();
        QDialog *palette = nullptr;
        for (int tick = 0; tick < 200 && palette == nullptr; ++tick) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            palette = paletteOnScreen();
        }
        return palette;
    };
    auto *palette = openPalette();
    REQUIRE(palette != nullptr);
    auto *filter = palette->findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasPaletteFilter"));
    auto *list = palette->findChild<QListWidget *>(QStringLiteral("pelican.graphCanvasPaletteList"));
    REQUIRE(filter != nullptr);
    REQUIRE(list != nullptr);
    filter->setText(QStringLiteral("pose.clip_sample"));
    REQUIRE(list->currentItem() != nullptr);
    CHECK(list->currentItem()->text().contains(QStringLiteral("needs a definition")));
    CHECK(list->currentItem()->toolTip().contains(
        QStringLiteral("this definition is also used by an instance with no owner")));
    CHECK_FALSE(palette->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
    palette->reject();
    QCoreApplication::processEvents();

    // The instance given an owner: every use of walk is owned (read again).
    const auto read_before = tab.reads("get_graph_instances");
    tab.field<QPushButton>("pelican.graphCanvasRefresh")->click();
    QCoreApplication::processEvents();
    REQUIRE(tab.reads("get_graph_instances") > read_before);
    readWith(true, "ux13b-d1");
    CHECK(tab.widget->model().definitionOwnedInstancesOnly("walk"));
    palette = openPalette();
    REQUIRE(palette != nullptr);
    filter = palette->findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasPaletteFilter"));
    list = palette->findChild<QListWidget *>(QStringLiteral("pelican.graphCanvasPaletteList"));
    filter->setText(QStringLiteral("pose.clip_sample"));
    REQUIRE(list->currentItem() != nullptr);
    CHECK_FALSE(list->currentItem()->text().contains(QStringLiteral("needs a definition")));
    palette->reject();
    QCoreApplication::processEvents();
}

TEST_CASE("UX-13b decision 6: a format class with an argument is Explicit... and a combo of formats "
          "beside it, written explicit(<format>); a refusal inside a definition an unowned instance "
          "runs is one sentence",
          "[devstudio][graph-canvas][scene][ux13b]") {
    (void)application();
    CHECK(PelicanStudio::graphLoopRefusalDescription({{"code", "graph.op_requires_owned_instance@1"},
                                                      {"node", "right/weight"},
                                                      {"op", "value.morph_weight"},
                                                      {"document", "project://passes/main.json"},
                                                      {"definition", "gain_curve"}}) ==
          "value.morph_weight runs only in a definition attached to an object; this definition is also "
          "used by an instance with no owner (in definition gain_curve of passes/main.json, "
          "graph.op_requires_owned_instance@1).");

    Ux2Tab tab{true};
    const auto image = [](const Json &extra) {
        Json parameters{{"type", {{"semantic", "pelican.render.color_signal@1"},
                                  {"arguments", {{"reference", "scene"}, {"transfer", "linear"},
                                                 {"range", "extended"}}}}},
                        {"format", "R16G16B16A16_SFLOAT"},
                        {"extent_scale", 1.0},
                        {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
        for (const auto &[key, value] : extra.items()) parameters[key] = value;
        return Json{{"schema", "pelican.graph"},
                    {"version", 1},
                    {"definitions", Json::object()},
                    {"nodes", Json::array({{{"id", "img"}, {"op", "render.image"}, {"parameters", parameters},
                                            {"outputs", Json::array({"resource"})}}})}};
    };
    tab.read(image(Json::object()), "ux13b-f0");
    tab.answer("get_graph_catalog", engineCatalog());
    const auto pick = [&] {
        tab.widget->scene().selectNode("img");
        QCoreApplication::processEvents();
    };
    pick();
    tab.widget->poll();
    REQUIRE(tab.reads("get_render_authoring_context") == 1);
    const Json authoring{
        {"render_target_vocabulary",
         {{"formats", Json::array({"R8_UNORM", "R16G16B16A16_SFLOAT"})},
          {"format_classes", Json::array({"scene", "display", "data", "explicit"})},
          {"format_class_patterns",
           Json::array({{{"name", "explicit"},
                         {"form", "explicit(<format>)"},
                         {"argument_choices_from",
                          {{"rpc", "get_render_authoring_context"}, {"key", "render_target_vocabulary.formats"}}}}})}}}};
    tab.answer("get_render_authoring_context", authoring);
    tab.widget->poll();
    QCoreApplication::processEvents();
    auto *klass = tab.field<QComboBox>("pelican.graphCanvasParameters.format_class");
    CHECK(ux2Items(*klass) == QStringList{"scene", "display", "data", QStringLiteral("Explicit\u2026")});
    auto *argument = tab.field<QComboBox>("pelican.graphCanvasParameters.format_class.argument");
    CHECK(argument->isHidden());
    // Explicit...: the image's own format first, written with it.
    klass->setCurrentIndex(3);
    QCoreApplication::processEvents();
    CHECK_FALSE(argument->isHidden());
    CHECK(ux2Items(*argument) == QStringList{"R8_UNORM", "R16G16B16A16_SFLOAT"});
    CHECK(tab.sent(1).at("parameters").at("format_class") == "explicit(R16G16B16A16_SFLOAT)");
    tab.commit(image({{"format_class", "explicit(R16G16B16A16_SFLOAT)"}}), "ux13b-f1");
    pick();
    tab.widget->poll();
    tab.answer("get_render_authoring_context", authoring);
    tab.widget->poll();
    QCoreApplication::processEvents();
    // Read back: the class's entry, its argument beside it, not "not listed".
    klass = tab.field<QComboBox>("pelican.graphCanvasParameters.format_class");
    argument = tab.field<QComboBox>("pelican.graphCanvasParameters.format_class.argument");
    CHECK(klass->currentText() == QStringLiteral("Explicit\u2026"));
    CHECK(argument->currentText() == QStringLiteral("R16G16B16A16_SFLOAT"));
    CHECK(tab.field<QLabel>("pelican.graphCanvasParameters.format_class.unlisted")->isHidden());
    // Another format: written with it.
    argument->setCurrentIndex(argument->findText(QStringLiteral("R8_UNORM")));
    QCoreApplication::processEvents();
    CHECK(tab.sent(2).at("parameters").at("format_class") == "explicit(R8_UNORM)");
}

// --- AB-1s: a sound's analysis baked into a table (engine AB-1).

TEST_CASE("AB-1s decision 1: Bake analysis... asks for the features, bands, hop, smoothing, channels "
          "and table, sends bake_audio_analysis, says Baking while it runs and Baked when it is "
          "committed; a refusal is one sentence",
          "[devstudio][graph-canvas][scene][ab1s]") {
    const auto catalog = engineCatalog();
    if (!catalogHas(catalog, "audio.playback")) SKIP("this build registers no pelican.audio module");
    (void)application();
    const AudioProject project;
    RecordedCapability transport;
    auto capability = transport.capability();
    capability.requestBakeAudioAnalysis = [&transport](const QJsonObject &params, QString *) {
        transport.calls.push_back({QStringLiteral("bake_audio_analysis"), {}, 0,
                                   QJsonDocument{params}.toJson(QJsonDocument::Compact)});
        return transport.next++;
    };
    capability.requestDerivedAssets = [&transport](QString *) {
        transport.calls.push_back({QStringLiteral("list_derived_assets")});
        return transport.next++;
    };
    PelicanStudio::GraphCanvasWidget widget{capability};
    widget.setProjectRoot(project.root);
    widget.resize(1100, 800);
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
    REQUIRE(widget.model().canEdit());
    // The project opened and the document read: the tables are asked about.
    CHECK(transport.countOf(QStringLiteral("list_derived_assets")) >= 1);
    deliver(QStringLiteral("list_derived_assets"), Json::array());
    widget.scene().selectNode("voice");
    QCoreApplication::processEvents();
    auto *bake = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasAudioBake"));
    REQUIRE(bake != nullptr);
    REQUIRE(bake->isVisible());
    REQUIRE(bake->isEnabled());
    auto *line = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasBake.line"));
    auto *use_in_graph = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasBake.useInGraph"));
    REQUIRE(line != nullptr);
    REQUIRE(use_in_graph != nullptr);

    const auto openDialog = [&] {
        // The dialog closed before is gone first.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        bake->click();
        QCoreApplication::processEvents();
        auto *dialog = widget.findChild<QDialog *>(
            QStringLiteral("pelican.audioBakeDialog"));
        REQUIRE(dialog != nullptr);
        return dialog;
    };
    const auto bakeSent = [&] {
        const auto id = transport.idOf(QStringLiteral("bake_audio_analysis"));
        REQUIRE(id != 0);
        return Json::parse(transport.calls[static_cast<std::size_t>(id - 1)].body.toStdString());
    };

    auto *dialog = openDialog();
    // The table goes beside the others, named for the sound.
    auto *out = dialog->findChild<QLineEdit *>(QStringLiteral("pelican.audioBakeDialog.out"));
    REQUIRE(out != nullptr);
    CHECK(out->text() == QStringLiteral("assets/analysis/hit.ktx2"));
    dialog->findChild<QCheckBox *>(QStringLiteral("pelican.audioBakeDialog.peak"))->setChecked(false);
    dialog->findChild<QSpinBox *>(QStringLiteral("pelican.audioBakeDialog.bandCount"))->setValue(4);
    dialog->findChild<QDoubleSpinBox *>(QStringLiteral("pelican.audioBakeDialog.release"))->setValue(50.0);
    auto *channels = dialog->findChild<QComboBox *>(QStringLiteral("pelican.audioBakeDialog.channels"));
    channels->setCurrentIndex(channels->findData(QStringLiteral("left")));
    // No feature, or a table that is no .ktx2: nothing to bake.
    auto *rms = dialog->findChild<QCheckBox *>(QStringLiteral("pelican.audioBakeDialog.rms"));
    auto *bands = dialog->findChild<QCheckBox *>(QStringLiteral("pelican.audioBakeDialog.bands"));
    auto *ok = dialog->findChild<QPushButton *>(QStringLiteral("pelican.audioBakeDialog.bake"));
    rms->setChecked(false);
    bands->setChecked(false);
    CHECK_FALSE(ok->isEnabled());
    rms->setChecked(true);
    bands->setChecked(true);
    out->setText(QStringLiteral("assets/analysis/hit.png"));
    CHECK_FALSE(ok->isEnabled());
    out->setText(QStringLiteral("assets/analysis/hit.ktx2"));
    REQUIRE(ok->isEnabled());
    ok->click();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    REQUIRE(transport.countOf(QStringLiteral("bake_audio_analysis")) == 1);
    CHECK(bakeSent() == Json{{"asset", "project://sounds/hit.wav"},
                             {"out", "assets/analysis/hit.ktx2"},
                             {"settings",
                              {{"features", {"rms", "bands"}},
                               {"bands", 4},
                               {"hop_ms", 10.0},
                               {"smoothing", {{"attack_ms", 0.0}, {"release_ms", 50.0}}},
                               {"channels", "left"}}}});
    deliver(QStringLiteral("bake_audio_analysis"),
            Json{{"ticket", "render-config-edit-7"}, {"status", "accepted"}});
    CHECK(line->text() == QString::fromUtf8("Baking hit.wav\xE2\x80\xA6 (0 s)"));
    CHECK(widget.model().notice().message == "Baking hit.wav\xE2\x80\xA6 (0 s)");
    deliver(QStringLiteral("get_edit_result"), Json{{"ticket", "render-config-edit-7"}, {"status", "running"}});
    CHECK(line->text().startsWith(QStringLiteral("Baking hit.wav")));
    CHECK_FALSE(use_in_graph->isVisible());
    // The clock asks again.
    const auto polls = transport.countOf(QStringLiteral("get_edit_result"));
    for (int wait = 0; wait < 200 && transport.countOf(QStringLiteral("get_edit_result")) == polls; ++wait) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    REQUIRE(transport.countOf(QStringLiteral("get_edit_result")) == polls + 1);
    const auto derived_reads = transport.countOf(QStringLiteral("list_derived_assets"));
    deliver(QStringLiteral("get_edit_result"),
            Json{{"ticket", "render-config-edit-7"},
                 {"status", "committed"},
                 {"committed", true},
                 {"operation", "bake_audio_analysis"},
                 {"asset", "project://sounds/hit.wav"},
                 {"out", "assets/analysis/hit.ktx2"},
                 {"manifest", "assets/analysis/hit.ktx2.import.json"},
                 {"layout", {{"width", 4096}, {"rows_per_feature", 1}, {"sample_count", 101},
                             {"hop_seconds", 0.01},
                             {"features", {"rms", "band:0", "band:1", "band:2", "band:3"}}}}});
    CHECK(line->text() == QString::fromUtf8("Baked assets/analysis/hit.ktx2: rms, band:0..band:3 \xC3\x97 101 samples"));
    CHECK(widget.model().notice().kind == PelicanStudio::InspectorNoticeKind::Information);
    CHECK(use_in_graph->isVisible());
    CHECK(widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasBake.useInShader"))->isVisible());
    // A bake changes which tables are stale.
    CHECK(transport.countOf(QStringLiteral("list_derived_assets")) == derived_reads + 1);

    // Refused: one sentence from the refusal's fields.
    openDialog()->findChild<QPushButton *>(QStringLiteral("pelican.audioBakeDialog.bake"))->click();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    REQUIRE(transport.countOf(QStringLiteral("bake_audio_analysis")) == 2);
    deliver(QStringLiteral("bake_audio_analysis"),
            Json{{"status", "rejected"},
                 {"committed", false},
                 {"error", {{"code", "audio.analysis_settings_invalid@1"},
                            {"message", "hop_ms must be 1..1000"},
                            {"field", "hop_ms"},
                            {"reason", "range"},
                            {"value", 0}}}});
    CHECK(line->text() ==
          QStringLiteral("The analysis cannot take hop_ms = 0 (range) (audio.analysis_settings_invalid@1)."));
    CHECK(widget.model().notice().kind == PelicanStudio::InspectorNoticeKind::Error);
    // The other four read from their fields too.
    CHECK(PelicanStudio::audioAnalysisRefusalDescription(
              {{"code", "audio.analysis_busy@1"}, {"out", "assets/analysis/hit.ktx2"}, {"ticket", "t"}}) ==
          "assets/analysis/hit.ktx2 is being baked already; wait for it to finish (audio.analysis_busy@1).");
    CHECK(PelicanStudio::audioAnalysisRefusalDescription(
              {{"code", "audio.analysis_unavailable@1"}, {"reason", "built_without_audio"}}) ==
          "This engine cannot bake a sound's analysis (built_without_audio) (audio.analysis_unavailable@1).");
    CHECK(PelicanStudio::audioAnalysisRefusalDescription(
              {{"code", "audio.format_unsupported@1"}, {"asset", "project://sounds/hit.wav"}}) ==
          "sounds/hit.wav is not a sound the analysis reads: its extension and its contents must both "
          "say WAV, or both Ogg Vorbis (audio.format_unsupported@1).");
    CHECK(PelicanStudio::audioAnalysisRefusalDescription(
              {{"code", "audio.analysis_failed@1"}, {"asset", "sounds/hit.wav"},
               {"out", "assets/analysis/hit.ktx2"}, {"message", "cannot read"}}) ==
          "sounds/hit.wav could not be baked into assets/analysis/hit.ktx2: cannot read "
          "(audio.analysis_failed@1).");
    CHECK(PelicanStudio::audioAnalysisRefusalDescription({{"code", "graph.loop_nested@1"}}).empty());
}

TEST_CASE("UX-14 decision 5 (engine AB-2): a bake refused because the table is in use says to try "
          "Bake again, and Bake again sends the same bake once more",
          "[devstudio][graph-canvas][scene][ux14]") {
    const auto catalog = engineCatalog();
    if (!catalogHas(catalog, "audio.playback")) SKIP("this build registers no pelican.audio module");
    (void)application();
    const AudioProject project;
    RecordedCapability transport;
    auto capability = transport.capability();
    capability.requestBakeAudioAnalysis = [&transport](const QJsonObject &params, QString *) {
        transport.calls.push_back({QStringLiteral("bake_audio_analysis"), {}, 0,
                                   QJsonDocument{params}.toJson(QJsonDocument::Compact)});
        return transport.next++;
    };
    capability.requestDerivedAssets = [&transport](QString *) {
        transport.calls.push_back({QStringLiteral("list_derived_assets")});
        return transport.next++;
    };
    PelicanStudio::GraphCanvasWidget widget{capability};
    widget.setProjectRoot(project.root);
    widget.resize(1100, 800);
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
    REQUIRE(widget.model().canEdit());
    deliver(QStringLiteral("list_derived_assets"), Json::array());
    widget.scene().selectNode("voice");
    QCoreApplication::processEvents();
    auto *bake = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasAudioBake"));
    auto *line = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasBake.line"));
    auto *again = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasBake.again"));
    REQUIRE(bake != nullptr);
    REQUIRE(line != nullptr);
    REQUIRE(again != nullptr);
    bake->click();
    QCoreApplication::processEvents();
    auto *dialog = widget.findChild<QDialog *>(QStringLiteral("pelican.audioBakeDialog"));
    REQUIRE(dialog != nullptr);
    dialog->findChild<QPushButton *>(QStringLiteral("pelican.audioBakeDialog.bake"))->click();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    REQUIRE(transport.countOf(QStringLiteral("bake_audio_analysis")) == 1);
    const auto first = transport.calls[static_cast<std::size_t>(transport.idOf(QStringLiteral("bake_audio_analysis")) - 1)].body;
    deliver(QStringLiteral("bake_audio_analysis"), Json{{"ticket", "render-config-edit-9"}, {"status", "accepted"}});
    CHECK_FALSE(again->isVisible());
    // The engine replaced the table through its retries and still found it
    // held: the result names the reason and the file (engine AB-2).
    deliver(QStringLiteral("get_edit_result"),
            Json{{"status", "rejected"},
                 {"committed", false},
                 {"error",
                  {{"code", "audio.analysis_failed@1"},
                   {"message", "assets/analysis/hit.ktx2 is in use"},
                   {"reason", "output_locked"},
                   {"path", "assets/analysis/hit.ktx2"},
                   {"asset", "project://sounds/hit.wav"},
                   {"out", "assets/analysis/hit.ktx2"},
                   {"ticket", "render-config-edit-9"}}}});
    const auto locked = QString::fromUtf8("sounds/hit.wav could not be baked into assets/analysis/hit.ktx2: the "
                                          "table is in use \xE2\x80\x94 try Bake again (audio.analysis_failed@1).");
    CHECK(line->text() == locked);
    CHECK(widget.model().notice().kind == PelicanStudio::InspectorNoticeKind::Error);
    CHECK(widget.model().notice().message == locked.toStdString());
    REQUIRE(again->isVisible());
    // Bake again: the same request, and the row says Baking again.
    again->click();
    QCoreApplication::processEvents();
    REQUIRE(transport.countOf(QStringLiteral("bake_audio_analysis")) == 2);
    CHECK(transport.calls[static_cast<std::size_t>(transport.idOf(QStringLiteral("bake_audio_analysis")) - 1)].body ==
          first);
    CHECK_FALSE(again->isVisible());
    // Any other failure keeps its own sentence and offers no Bake again.
    deliver(QStringLiteral("bake_audio_analysis"),
            Json{{"status", "rejected"},
                 {"committed", false},
                 {"error", {{"code", "audio.analysis_failed@1"}, {"message", "cannot read"},
                            {"asset", "project://sounds/hit.wav"}, {"out", "assets/analysis/hit.ktx2"}}}});
    CHECK(line->text() == QStringLiteral("sounds/hit.wav could not be baked into assets/analysis/hit.ktx2: cannot "
                                         "read (audio.analysis_failed@1)."));
    CHECK_FALSE(again->isVisible());
}

TEST_CASE("UX-13a decision 3: a Re-bake sends no edit of its own; the table it rewrites is "
          "watched beside the images' initial assets, for the reload to pick up (IA-3)",
          "[devstudio][graph-canvas][scene][ux13a]") {
    const auto catalog = engineCatalog();
    if (!catalogHas(catalog, "audio.playback")) SKIP("this build registers no pelican.audio module");
    (void)application();
    const AudioProject project;
    std::filesystem::create_directories(project.root / "assets" / "analysis");
    std::ofstream{project.root / "assets" / "analysis" / "hit.ktx2.import.json"}
        << Json{{"schema", "pelican.import"},
                {"version", 1},
                {"tool", {{"name", "pelican.audio_analysis"}, {"version", "1"}}},
                {"source",
                 {{"file", "../../sounds/hit.wav"},
                  {"metadata",
                   {{"settings", {{"features", {"rms"}}, {"hop_ms", 10.0}, {"channels", "mix"}}},
                    {"layout", {{"width", 4096}, {"rows_per_feature", 1}, {"sample_count", 101},
                                {"hop_seconds", 0.01}, {"features", {"rms"}}}}}}}},
                {"outputs", Json::array({{{"file", "hit.ktx2"}}})}}
               .dump();
    RecordedCapability transport;
    auto capability = transport.capability();
    capability.requestBakeAudioAnalysis = [&transport](const QJsonObject &params, QString *) {
        transport.calls.push_back({QStringLiteral("bake_audio_analysis"), {}, 0,
                                   QJsonDocument{params}.toJson(QJsonDocument::Compact)});
        return transport.next++;
    };
    capability.requestDerivedAssets = [&transport](QString *) {
        transport.calls.push_back({QStringLiteral("list_derived_assets")});
        return transport.next++;
    };
    PelicanStudio::GraphCanvasWidget widget{capability};
    widget.setProjectRoot(project.root);
    widget.resize(1100, 800);
    widget.show();
    QCoreApplication::processEvents();
    const auto deliver = [&](const QString &method, const Json &reply) {
        const qint64 id = transport.idOf(method);
        REQUIRE(id != 0);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    };
    auto document = audioDocument();
    document["nodes"].push_back({{"id", "loudness"}, {"op", "value.sample_table"},
                                 {"parameters", {{"asset", "project://assets/analysis/hit.ktx2"}, {"row", "rms"}}},
                                 {"inputs", Json::object()},
                                 {"outputs", Json::array({"value"})}});
    deliver(QStringLiteral("get_graph_instances"), context(document));
    deliver(QStringLiteral("get_graph_catalog"), catalog);
    REQUIRE(widget.model().canEdit());
    // The table is watched as an image's initial asset is: a rewrite asks
    // reload_render_documents, and the engine reads it again (IA-3).
    CHECK(widget.model().initialAssets() == std::vector<std::string>{"assets/analysis/hit.ktx2"});
    deliver(QStringLiteral("list_derived_assets"),
            Json::array({{{"asset", "assets/analysis/hit.ktx2"},
                          {"manifest", "assets/analysis/hit.ktx2.import.json"},
                          {"tool", {{"name", "pelican.audio_analysis"}, {"version", "1"}}},
                          {"source", "sounds/hit.wav"},
                          {"stale", true},
                          {"stale_reasons", {"source_changed"}}}}));
    widget.scene().selectNode("loudness");
    QCoreApplication::processEvents();
    auto *rebake = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasStale.rebake"));
    REQUIRE(rebake != nullptr);
    REQUIRE(rebake->isVisible());
    REQUIRE(rebake->isEnabled());
    const auto edits = transport.countOf(QStringLiteral("edit_graph_document"));
    rebake->click();
    QCoreApplication::processEvents();
    REQUIRE(transport.countOf(QStringLiteral("bake_audio_analysis")) == 1);
    const auto sent = Json::parse(
        transport.calls[static_cast<std::size_t>(transport.idOf(QStringLiteral("bake_audio_analysis")) - 1)]
            .body.toStdString());
    CHECK(sent.at("asset") == "project://sounds/hit.wav");
    CHECK(sent.at("out") == "assets/analysis/hit.ktx2");
    deliver(QStringLiteral("bake_audio_analysis"),
            Json{{"ticket", "render-config-edit-9"}, {"status", "accepted"}});
    deliver(QStringLiteral("get_edit_result"),
            Json{{"ticket", "render-config-edit-9"},
                 {"status", "committed"},
                 {"committed", true},
                 {"operation", "bake_audio_analysis"},
                 {"asset", "project://sounds/hit.wav"},
                 {"out", "assets/analysis/hit.ktx2"},
                 {"manifest", "assets/analysis/hit.ktx2.import.json"},
                 {"layout", {{"width", 4096}, {"rows_per_feature", 1}, {"sample_count", 101},
                             {"hop_seconds", 0.01}, {"features", {"rms"}}}}});
    auto *line = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasBake.line"));
    REQUIRE(line != nullptr);
    CHECK(line->text().startsWith(QStringLiteral("Baked assets/analysis/hit.ktx2")));
    // Asked again which tables are stale, and nothing else: no edit of the
    // readers, so no Undo step either.
    deliver(QStringLiteral("list_derived_assets"), Json::array());
    for (int tick = 0; tick < 20; ++tick) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    CHECK(transport.countOf(QStringLiteral("edit_graph_document")) == edits);
    CHECK_FALSE(widget.model().canUndo());
}

TEST_CASE("AB-1s decision 2 / UX-13b decision 5: value.sample_table's dialog is its table's own "
          "fields -- the project's analysis tables, and a row the manifest beside the one picked lists, "
          "its first taken -- and is placed with them",
          "[devstudio][graph-canvas][scene][ab1s][ux13b]") {
    (void)application();
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_ab1s_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "assets" / "analysis");
    const auto table = [&](const char *name, const Json &features) {
        std::ofstream{root / "assets" / "analysis" / (std::string{name} + ".ktx2")} << "KTX";
        std::ofstream{root / "assets" / "analysis" / (std::string{name} + ".ktx2.import.json")}
            << Json{{"schema", "pelican.import"},
                    {"version", 1},
                    {"tool", {{"name", "pelican.audio_analysis"}, {"version", "1"}}},
                    {"source",
                     {{"file", std::string{"../audio/"} + name + ".wav"},
                      {"metadata",
                       {{"layout", {{"width", 4096}, {"rows_per_feature", 1}, {"sample_count", 11},
                                    {"hop_seconds", 0.01}, {"features", features}}}}}}},
                    {"outputs", Json::array({{{"file", std::string{name} + ".ktx2"}}})}}
                   .dump();
    };
    table("tone", Json{"rms", "peak", "band:0", "band:1"});
    table("kick", Json{"band:0", "band:1"});
    {
        GraphCanvasModel model;
        model.setConnected(true);
        answer(model, ux3Document(), engineCatalog());
        REQUIRE(model.canEdit());
        PelicanStudio::GraphCanvasRequiredDialog ask{model, {}, "value.sample_table", nullptr, root};
        REQUIRE(ask.asks());
        auto *asset = ask.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasRequired.asset"));
        auto *shape = ask.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasRequired.row"));
        auto *row = ask.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasRequired.row.string"));
        auto *ok = ask.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasRequired.ok"));
        REQUIRE(asset != nullptr);
        REQUIRE(shape != nullptr);
        REQUIRE(row != nullptr);
        REQUIRE(ok != nullptr);
        CHECK(ux2Items(*asset) == QStringList{"(choose)", "project://assets/analysis/kick.ktx2",
                                              "project://assets/analysis/tone.ktx2"});
        // No table picked: no rows, nothing to place.
        CHECK(row->count() == 0);
        CHECK_FALSE(ok->isEnabled());
        asset->setCurrentIndex(asset->findData(QStringLiteral("project://assets/analysis/tone.ktx2")));
        // Its rows by name, the first taken.
        CHECK(shape->currentText() == QStringLiteral("Name"));
        CHECK(ux2Items(*row) == QStringList{"rms", "peak", "band:0", "band:1"});
        CHECK(row->currentText() == QStringLiteral("rms"));
        CHECK(ok->isEnabled());
        row->setCurrentIndex(row->findText(QStringLiteral("band:1")));
        const Json parameters{{"asset", "project://assets/analysis/tone.ktx2"}, {"row", "band:1"}};
        CHECK(ask.parameters() == parameters);
        // Another table: its rows.
        asset->setCurrentIndex(asset->findData(QStringLiteral("project://assets/analysis/kick.ktx2")));
        CHECK(ux2Items(*row) == QStringList{"band:0", "band:1"});
        // Placed with them, at the scene time: no time edge.
        const auto ops = model.addNodeOpsIn({}, "value.sample_table", {}, ask.seed());
        REQUIRE(ops.size() == 1);
        CHECK(ops.back().at("node").at("parameters") ==
              Json{{"asset", "project://assets/analysis/kick.ktx2"}, {"row", "band:1"}});
        CHECK_FALSE(ops.back().at("node").contains("inputs"));

        // No table yet: nothing to pick, nothing to place.
        const auto empty = std::filesystem::temp_directory_path() / "pelican_ab1s_none";
        PelicanStudio::GraphCanvasRequiredDialog none{model, {}, "value.sample_table", nullptr, empty};
        CHECK(ux2Items(*none.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasRequired.asset"))) ==
              QStringList{"(choose)"});
        CHECK_FALSE(none.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasRequired.ok"))->isEnabled());
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

// --- CT-6s (engine CT-6): a ray tracing task built in the Graph --
// render.compute's ray_tracing and dispatch in the generic fields, a ray
// tracing set from New..., and the ray tracing refusals in one sentence.

namespace {

Json ct6sPorts() {
    return Json{{"color", {{"kind", "image"}, {"access", "write"}, {"binding", "storage"}}},
                {"depth", {{"kind", "image"}, {"access", "read"}, {"binding", "sampled"}}},
                {"counts", {{"kind", "buffer"}, {"access", "read"}}}};
}

Json ct6sDocument(const Json &parameters) {
    return Json{{"schema", "pelican.graph"},
                {"version", 1},
                {"definitions", Json::object()},
                {"nodes", Json::array({{{"id", "trace__color"},
                                        {"op", "render.image"},
                                        {"parameters", {{"format", "R16G16B16A16_SFLOAT"},
                                                        {"extent_scale", 1.0},
                                                        {"usage", {"STORAGE", "SAMPLED"}}}},
                                        {"outputs", Json::array({"resource"})}},
                                       {{"id", "trace"},
                                        {"op", "render.compute"},
                                        {"parameters", parameters},
                                        {"inputs", {{"color", {{"node", "trace__color"}, {"port", "resource"}}}}},
                                        {"outputs", Json::array({"color"})}}})}};
}

// A project whose shaders folder has a set (a), a second miss (b), a
// compute shader (c) and an any-hit shader (d).
std::filesystem::path ct6sProject() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_ct6s_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "shaders");
    for (const char *file : {"a.rgen", "a.rmiss", "a.rchit", "b.rmiss", "c.comp", "d.rahit"})
        std::ofstream{root / "shaders" / file} << "x";
    return root;
}

} // namespace

TEST_CASE("CT-6s decision 1: ray_tracing is the generic fields' -- each stage lists that stage's "
          "shaders, Max recursion a spin, a miss made a list keeps its shader -- and a task "
          "that runs a shader has none",
          "[devstudio][graph-canvas][scene][ct6s]") {
    (void)application();
    const auto root = ct6sProject();
    {
        Ux2Tab tab{false};
        tab.widget->setProjectRoot(root);
        const std::string set = "project://shaders/a";
        auto parameters = Json{{"ports", ct6sPorts()},
                               {"ray_tracing", {{"raygen", set}, {"miss", set}, {"closesthit", set}}},
                               {"dispatch", {{"rays_from", {{"port", "color"}}}}}};
        tab.read(ct6sDocument(parameters), "ct6s-0");
        tab.answer("get_graph_catalog", engineCatalog());
        const auto pick = [&] {
            tab.widget->scene().selectNode("trace");
            QCoreApplication::processEvents();
        };
        pick();
        const auto own = PelicanStudio::GraphCanvasParameterFields::ownFieldNames("render.compute");
        CHECK(std::ranges::find(own, "ray_tracing") == own.end());
        CHECK_FALSE(tab.field<QLabel>("pelican.graphCanvasParameters.ray_tracing.label")->isHidden());
        // Each stage: the project's files of that stage only.
        auto *raygen = tab.field<QComboBox>("pelican.graphCanvasParameters.ray_tracing.raygen");
        CHECK(ux2Items(*raygen) == QStringList{"project://shaders/a"});
        CHECK(raygen->currentData().toString() == QStringLiteral("project://shaders/a"));
        auto *miss = tab.field<QComboBox>("pelican.graphCanvasParameters.ray_tracing.miss");
        CHECK(ux2Items(*miss) == QStringList{"(none)", "Shader", "List"});
        CHECK(miss->currentText() == QStringLiteral("Shader"));
        CHECK(ux2Items(*tab.field<QComboBox>("pelican.graphCanvasParameters.ray_tracing.miss.shader")) ==
              QStringList{"(none)", "project://shaders/a", "project://shaders/b"});
        CHECK(ux2Items(*tab.field<QComboBox>("pelican.graphCanvasParameters.ray_tracing.closesthit.shader")) ==
              QStringList{"(none)", "project://shaders/a"});

        // Max recursion: a spin, one edit, the rest of ray_tracing kept.
        auto *recursion = tab.field<QDoubleSpinBox>("pelican.graphCanvasParameters.ray_tracing.max_recursion");
        CHECK(recursion->value() == 1.0);
        recursion->setValue(2.0);
        REQUIRE(ux12aWait([&] { return tab.reads("edit_graph_document") == 1; }));
        auto expected = parameters;
        expected["ray_tracing"]["max_recursion"] = 2;
        CHECK(tab.sent(1).at("parameters") == expected);
        tab.commit(ct6sDocument(expected), "ct6s-1");
        pick();

        // A miss made a list keeps the shader it had as its first row.
        miss = tab.field<QComboBox>("pelican.graphCanvasParameters.ray_tracing.miss");
        miss->setCurrentIndex(miss->findText(QStringLiteral("List")));
        QCoreApplication::processEvents();
        expected["ray_tracing"]["miss"] = Json::array({set});
        CHECK(tab.sent(2).at("parameters") == expected);
        CHECK(tab.field<QPushButton>("pelican.graphCanvasParameters.ray_tracing.miss.array.add") != nullptr);

        // A task that runs a compute shader shows no ray tracing.
        tab.commit(ct6sDocument(Json{{"ports", ct6sPorts()},
                                     {"shader", "project://shaders/c"},
                                     {"dispatch", {{"groups", {1, 1, 1}}}}}),
                   "ct6s-2");
        pick();
        CHECK(tab.field<QLabel>("pelican.graphCanvasParameters.ray_tracing.label")->isHidden());
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("CT-6s decision 2: dispatch is the catalog's fields -- groups a list or x / y / z, "
          "rays_from's port one of the task's image ports, indirect's buffer the graph's "
          "dispatch buffers",
          "[devstudio][graph-canvas][scene][ct6s]") {
    (void)application();
    Ux2Tab tab{false};
    const std::string set = "project://shaders/a";
    auto parameters = Json{{"ports", ct6sPorts()},
                           {"ray_tracing", {{"raygen", set}, {"miss", set}, {"closesthit", set}}},
                           {"dispatch", {{"rays_from", {{"port", "color"}}}}}};
    tab.read(ct6sDocument(parameters), "ct6s-0");
    tab.answer("get_graph_catalog", engineCatalog());
    tab.widget->scene().selectNode("trace");
    QCoreApplication::processEvents();
    CHECK(tab.field<QToolButton>("pelican.graphCanvasParameters.dispatch.fold") != nullptr);
    auto *groups = tab.field<QComboBox>("pelican.graphCanvasParameters.dispatch.groups");
    CHECK(ux2Items(*groups) == QStringList{"List", "Custom"});
    CHECK(groups->currentText() == QStringLiteral("List"));
    // The port is picked from the task's own image ports, not typed.
    auto *port = tab.field<QComboBox>("pelican.graphCanvasParameters.dispatch.rays_from.port");
    CHECK_FALSE(port->isHidden());
    CHECK(ux2Items(*port) == QStringList{"color", "depth"});
    CHECK(port->currentText() == QStringLiteral("color"));
    CHECK(ux2Items(*tab.field<QComboBox>("pelican.graphCanvasParameters.dispatch.groups_from.port")) ==
          QStringList{"(choose)", "color", "depth"});
    tab.pick(port, QStringLiteral("depth"));
    auto expected = parameters;
    expected["dispatch"]["rays_from"]["port"] = "depth";
    CHECK(tab.sent(1).at("parameters") == expected);
    // indirect's buffer: chosen from the graph's buffers, as CT-5's lists are.
    CHECK(tab.field<QComboBox>("pelican.graphCanvasParameters.dispatch.indirect.buffer")->isEditable());
}

TEST_CASE("CT-6s decision 3: New... beside a compute task's Shader writes a ray tracing set "
          "(raygen + miss + closest hit) under a free name it proposes, and the task traces "
          "with it; nothing already there is overwritten",
          "[devstudio][graph-canvas][scene][ct6s]") {
    (void)application();
    using PelicanStudio::RayTracingTemplateStage;
    // The engine's form: its includes, the scene at the ray query binding,
    // one storage image port, one payload the three stages share.
    const auto raygen = PelicanStudio::rayTracingShaderTemplate(RayTracingTemplateStage::raygen, "rt");
    for (const char *part : {"#include \"pelican_sets.glsl\"", "#include \"pelican_frame.glsl\"",
                             "binding = PELICAN_RAY_QUERY_TLAS_BINDING", "pelican_resource_color",
                             "rayPayloadEXT vec4 hit", "traceRayEXT", "imageStore"})
        CHECK(raygen.find(part) != std::string::npos);
    CHECK(PelicanStudio::rayTracingShaderTemplate(RayTracingTemplateStage::miss, "rt")
              .find("rayPayloadInEXT vec4 hit") != std::string::npos);
    CHECK(PelicanStudio::rayTracingShaderTemplate(RayTracingTemplateStage::closest_hit, "rt")
              .find("gl_HitTEXT") != std::string::npos);

    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_ct6s_new_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "shaders");
    {
        const Json document{{"schema", "pelican.graph"}, {"version", 1}, {"outputs", Json::object()},
                            {"definitions", Json::object()},
                            {"nodes", Json::array({Json{{"id", "task"}, {"op", "render.compute"},
                                                        {"parameters", {{"shader", "engine://some_compute"},
                                                                        {"dispatch", {{"groups", {1, 1, 1}}}},
                                                                        {"ports", Json::object()}}},
                                                        {"outputs", Json::array()}}})}};
        ZoneHarness h{document};
        h.catalog["modules"][1]["operations"].push_back(el5sComputeCatalogEntry());
        h.open();
        int submitted = 0;
        int changed = 0;
        PelicanStudio::GraphCanvasComputeFields fields{h.model, [&] { ++submitted; }};
        fields.setShadersChanged([&] { ++changed; });
        fields.setProjectRoot(root);
        const auto drawn = h.model.nodes({});
        REQUIRE(drawn.size() == 1);
        fields.showNode(&drawn[0], {});
        auto *new_shader = fields.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasCompute.newShader"));
        REQUIRE(new_shader != nullptr);
        CHECK(new_shader->isEnabled());
        new_shader->click();
        auto *kind = fields.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasCompute.shaderTemplateKind"));
        REQUIRE(kind != nullptr);
        CHECK(kind->currentText() == QStringLiteral("Ray tracing (raygen + miss + closest hit)"));
        // Proposed, so nothing has to be typed.
        auto *name = fields.findChild<QLineEdit *>(QStringLiteral("pelican.graphCanvasCompute.shaderTemplateName"));
        REQUIRE(name != nullptr);
        CHECK(name->text() == QStringLiteral("ray_tracing"));
        fields.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasCompute.createShader"))->click();
        for (const char *file : {"ray_tracing.rgen", "ray_tracing.rmiss", "ray_tracing.rchit"})
            CHECK(std::filesystem::exists(root / "shaders" / file));
        CHECK(changed == 1);
        CHECK(submitted == 1);
        // The task traces with it once the engine has read its ports.
        const auto asked = el5sAsked(h);
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].second.at("reference") == "project://shaders/ray_tracing");
        CHECK(fields.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasCompute.shaderTemplateNotice"))
                  ->text()
                  .contains(QStringLiteral("shaders/ray_tracing.rgen")));
        // The next proposal moves on; nothing there is written over, and a
        // set with one file already there is not written at all.
        CHECK(PelicanStudio::rayTracingTemplateName(root) == "ray_tracing_2");
        CHECK_FALSE(PelicanStudio::writeRayTracingTemplate(root, "ray_tracing").created);
        std::ofstream{root / "shaders" / "mine.rmiss"} << "keep";
        CHECK_FALSE(PelicanStudio::writeRayTracingTemplate(root, "mine").created);
        CHECK_FALSE(std::filesystem::exists(root / "shaders" / "mine.rgen"));
        CHECK(PelicanStudio::rayTracingTemplateName(root, "mine") == "mine_2");
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("CT-6s decision 5: a ray tracing task's refusal reads as one sentence -- the layout's "
          "form where the project has none and the other way round, a field the engine does "
          "not read, and the layout mode that changes only on a restart",
          "[devstudio][graph-canvas][scene][ct6s]") {
    using PelicanStudio::knownRefusalDescription;
    CHECK(knownRefusalDescription(
              "render_pipeline_preflight_failed",
              "render pipeline preflight failed: pelican.ray_tracing.legacy_syntax_with_layout@1: compute "
              "task 'trace' must use traced_ray_types/misses/hit_shaders") ==
          "This project has a ray group layout: Task trace names its shaders in traced_ray_types, misses "
          "and hit_shaders, not miss and closesthit (pelican.ray_tracing.legacy_syntax_with_layout@1).");
    CHECK(knownRefusalDescription({}, "pelican.ray_tracing.unknown_shader_stage@1: compute task 'trace' "
                                      "declares unknown ray tracing field 'traced_ray_types'") ==
          "This project has no ray group layout: Task trace names its shaders in miss and closesthit, not "
          "traced_ray_types (pelican.ray_tracing.unknown_shader_stage@1).");
    CHECK(knownRefusalDescription({}, "pelican.ray_tracing.unknown_shader_stage@1: compute task 'trace' "
                                      "declares unknown ray tracing field 'rgen'") ==
          "Task trace's ray_tracing has rgen, which the engine does not read "
          "(pelican.ray_tracing.unknown_shader_stage@1).");
    CHECK(knownRefusalDescription({}, "pelican.ray_tracing.invalid_recursion@1: compute task 'trace' "
                                      "max_recursion must be a positive uint32 integer") ==
          "Task trace's max_recursion must be a whole number from 1 (pelican.ray_tracing.invalid_recursion@1).");
    CHECK(knownRefusalDescription("pelican.ray_layout_mode_change_requires_restart@1",
                                  "ray layout semantic mode cannot change during a process lifetime") ==
          "Whether the project has a ray group layout changes only when the engine restarts: restart it to "
          "apply this (pelican.ray_layout_mode_change_requires_restart@1).");
    CHECK(PelicanStudio::rayTracingRefusalDescription({}, "graph.something_else@1: no").empty());
}

// --- RTL-2s b (engine RTL-1a / RTL-1b / RTL-1c): a ray tracing task of a
// project with a ray group layout, in the layout's terms.

namespace {

// Two ray types, two misses, a triangles group (the default) and a
// procedural one, the engine's record of normals.
Json rtl2sLayoutDocument() {
    return Json{{"schema", "pelican.ray_group_layout"},
                {"version", 3},
                {"ray_types", {"primary", "shadow"}},
                {"default_group", "mesh"},
                {"groups", {{{"name", "mesh"}, {"type", "triangles"}}, {{"name", "ball"}, {"type", "procedural"}}}},
                {"misses", {{{"name", "sky"}}, {{"name", "shadow"}}}},
                {"engine_record", {"material_index", "geometry_index"}},
                {"record", Json::array()}};
}

// get_render_authoring_context's ray_layout for it (engine RTL-1b).
Json rtl2sAuthoring(bool layout) {
    auto context = ux2Authoring();
    context["ray_layout"] =
        layout ? Json{{"mode", "layout"},
                      {"reference", "project://passes/ray_layout.json"},
                      {"ray_types", {"primary", "shadow"}},
                      {"groups", {{{"name", "mesh"}, {"type", "triangles"}}, {{"name", "ball"}, {"type", "procedural"}}}},
                      {"misses", {"sky", "shadow"}},
                      {"default_group", "mesh"}}
               : Json{{"mode", "none"},
                      {"reference", nullptr},
                      {"ray_types", Json::array()},
                      {"groups", Json::array()},
                      {"misses", Json::array()},
                      {"default_group", nullptr}};
    return context;
}

// get_ray_layout for it (engine RTL-1a / RTL-1c): the Graph's section reads
// the names from here (RTL-2s-c), as the Ray layout section does.
Json rtl2sRayLayout() {
    return Json{{"source_digest", {{"algorithm", "sha256"}, {"hex", std::string(64, 'c')}}},
                {"mode", "layout"},
                {"layout",
                 {{"reference", "project://passes/ray_layout.json"},
                  {"digest", std::string(64, 'd')},
                  {"document", rtl2sLayoutDocument()}}},
                {"bindings", nullptr},
                {"selectable_materials", Json::array()},
                {"selectable_procedural_assets", Json::array()},
                {"unresolved_bindings", Json::array()}};
}

Json rtl2sRayTracing() {
    const std::string a = "project://shaders/a";
    return Json{{"raygen", a},
                {"traced_ray_types", {"primary"}},
                {"misses", {{"sky", a}, {"shadow", a}}},
                {"hit_shaders", {{"primary", {{"mesh", {{"closesthit", a}}}, {"ball", {{"closesthit", a}}}}}}}};
}

std::filesystem::path rtl2sProject() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_rtl2s_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "shaders");
    std::filesystem::create_directories(root / "passes");
    for (const char *file : {"a.rgen", "a.rmiss", "a.rchit", "b.rchit", "s.rint", "c.rcall"})
        std::ofstream{root / "shaders" / file} << "x";
    std::ofstream{root / "passes" / "ray_layout.json"} << rtl2sLayoutDocument().dump(2);
    return root;
}

} // namespace

TEST_CASE("RTL-2s b decision 1: with a ray group layout a ray tracing task's fields are its raygen, a "
          "check per ray type, a row per miss and the hit shaders' grid, each change one edit of the "
          "layout's form; without one the generic fields pick a map's keys from the layout's names",
          "[devstudio][graph-canvas][scene][rtl2s]") {
    (void)application();
    const auto root = rtl2sProject();
    {
        Ux2Tab tab{true};
        tab.widget->setProjectRoot(root);
        auto parameters = Json{{"ports", ct6sPorts()},
                               {"ray_tracing", rtl2sRayTracing()},
                               {"dispatch", {{"rays_from", {{"port", "color"}}}}}};
        tab.read(ct6sDocument(parameters), "rtl2s-0");
        tab.answer("get_graph_catalog", engineCatalog());
        const auto pick = [&] {
            tab.widget->scene().selectNode("trace");
            QCoreApplication::processEvents();
        };
        pick();
        tab.widget->poll();
        REQUIRE(tab.reads("get_render_authoring_context") == 1);
        tab.answer("get_render_authoring_context", rtl2sAuthoring(true));
        tab.widget->poll();
        // RTL-2s-c: the names are get_ray_layout's, asked for the layout the
        // context names.
        REQUIRE(tab.reads("get_ray_layout") == 1);
        tab.answer("get_ray_layout", rtl2sRayLayout());
        pick();
        // The section, not the generic row.
        CHECK(tab.field<QLabel>("pelican.graphCanvasParameters.ray_tracing.label")->isHidden());
        CHECK(tab.field<QWidget>("pelican.graphCanvasRayTracing")->isVisible());
        auto *raygen = tab.field<QComboBox>("pelican.graphCanvasRayTracing.raygen");
        CHECK(raygen->currentData().toString() == QStringLiteral("project://shaders/a"));
        CHECK(ux2Items(*raygen) == QStringList{"project://shaders/a"});
        CHECK(tab.field<QCheckBox>("pelican.graphCanvasRayTracing.traced.primary")->isChecked());
        CHECK_FALSE(tab.field<QCheckBox>("pelican.graphCanvasRayTracing.traced.shadow")->isChecked());
        CHECK(tab.field<QComboBox>("pelican.graphCanvasRayTracing.miss.shadow")->currentData().toString() ==
              QStringLiteral("project://shaders/a"));
        // The grid: the traced row, a column per group; the procedural one has
        // an intersection, red while it has none.
        auto *closest = tab.field<QComboBox>("pelican.graphCanvasRayTracing.hit.primary.mesh.closesthit");
        CHECK(ux2Items(*closest) == QStringList{"project://shaders/a", "project://shaders/b"});
        auto *intersection = tab.field<QComboBox>("pelican.graphCanvasRayTracing.hit.primary.ball.intersection");
        CHECK(intersection->property("pelicanInvalid").toBool());
        CHECK(ux2Items(*intersection) == QStringList{"(choose intersection)", "project://shaders/s"});
        CHECK(tab.widget->findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasRayTracing.hit.primary.mesh."
                                                                "intersection")) == nullptr);
        CHECK(tab.widget->findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasRayTracing.hit.shadow.mesh."
                                                                "closesthit")) == nullptr);

        // A cell: one edit, the rest kept.
        tab.pick(intersection, QStringLiteral("project://shaders/s"));
        REQUIRE(ux12aWait([&] { return tab.reads("edit_graph_document") == 1; }));
        auto expected = parameters;
        expected["ray_tracing"]["hit_shaders"]["primary"]["ball"]["intersection"] = "project://shaders/s";
        CHECK(tab.sent(1).at("parameters") == expected);
        tab.commit(ct6sDocument(expected), "rtl2s-1");
        pick();

        // A ray type checked takes a whole row, so the task is whole --
        // RTL-3s, decision 5: in its own payload, the template's closest hits
        // of shadow written for it, the ball's intersection kept.
        tab.field<QCheckBox>("pelican.graphCanvasRayTracing.traced.shadow")->click();
        REQUIRE(ux12aWait([&] { return tab.reads("edit_graph_document") == 2; }));
        expected["ray_tracing"]["traced_ray_types"] = Json::array({"primary", "shadow"});
        expected["ray_tracing"]["hit_shaders"]["shadow"] = {
            {"mesh", {{"closesthit", "project://shaders/shadow_mesh"}}},
            {"ball", {{"closesthit", "project://shaders/shadow_ball"}, {"intersection", "project://shaders/s"}}}};
        CHECK(std::filesystem::exists(root / "shaders/shadow_mesh.rchit"));
        CHECK(tab.sent(2).at("parameters") == expected);
        tab.commit(ct6sDocument(expected), "rtl2s-2");
        pick();
        CHECK(tab.field<QComboBox>("pelican.graphCanvasRayTracing.hit.shadow.mesh.closesthit") != nullptr);

        // New... in a cell writes that stage's shader for that place and puts
        // it there.
        tab.field<QPushButton>("pelican.graphCanvasRayTracing.hit.shadow.mesh.closesthit.new")->click();
        REQUIRE(ux12aWait([&] { return tab.reads("edit_graph_document") == 3; }));
        CHECK(std::filesystem::exists(root / "shaders" / "trace_shadow_mesh.rchit"));
        expected["ray_tracing"]["hit_shaders"]["shadow"]["mesh"]["closesthit"] =
            "project://shaders/trace_shadow_mesh";
        CHECK(tab.sent(3).at("parameters") == expected);
        tab.commit(ct6sDocument(expected), "rtl2s-3");
        pick();
        tab.field<QPushButton>("pelican.graphCanvasRayTracing.miss.shadow.new")->click();
        REQUIRE(ux12aWait([&] { return tab.reads("edit_graph_document") == 4; }));
        std::ifstream miss_file{root / "shaders" / "trace_shadow.rmiss"};
        const std::string miss_text{std::istreambuf_iterator<char>{miss_file}, {}};
        CHECK(miss_text.find("occluded = 0.0;") != std::string::npos);
        expected["ray_tracing"]["misses"]["shadow"] = "project://shaders/trace_shadow";
        CHECK(tab.sent(4).at("parameters") == expected);
        tab.commit(ct6sDocument(expected), "rtl2s-4");
        pick();

        // A ray type unchecked loses its row.
        tab.field<QCheckBox>("pelican.graphCanvasRayTracing.traced.shadow")->click();
        REQUIRE(ux12aWait([&] { return tab.reads("edit_graph_document") == 5; }));
        auto unchecked = expected;
        unchecked["ray_tracing"]["traced_ray_types"] = Json::array({"primary"});
        unchecked["ray_tracing"]["hit_shaders"].erase("shadow");
        CHECK(tab.sent(5).at("parameters") == unchecked);
        tab.commit(ct6sDocument(expected), "rtl2s-5");
        pick();

        // A callable: + and New... add one, in index order.
        tab.field<QPushButton>("pelican.graphCanvasRayTracing.callable.new")->click();
        REQUIRE(ux12aWait([&] { return tab.reads("edit_graph_document") == 6; }));
        CHECK(std::filesystem::exists(root / "shaders" / "trace_callable.rcall"));
        CHECK(tab.sent(6).at("parameters").at("ray_tracing").at("callable") == "project://shaders/trace_callable");
        // RTL-2s-c: Callable records... waits for a callable_record schema
        // (and a way to the records' dialog).
        CHECK_FALSE(tab.field<QPushButton>("pelican.graphCanvasRayTracing.callableRecords")->isEnabled());
        // RTL-2s-c: the raygen's switches read the raygen file; "a" is not
        // one Studio wrote, so neither is offered.
        CHECK_FALSE(tab.field<QCheckBox>("pelican.graphCanvasRayTracing.traceSecond")->isEnabled());
        CHECK_FALSE(tab.field<QCheckBox>("pelican.graphCanvasRayTracing.callCallable")->isEnabled());
        // Max recursion: the catalog's editor, in the section.
        CHECK(tab.field<QDoubleSpinBox>("pelican.graphCanvasRayTracing.max_recursion")->value() == 1.0);
    }
    {
        // No layout: the generic fields, one line pointing at hit groups;
        // a map's keys are picked from the names the layout lists.
        Ux2Tab tab{true};
        tab.widget->setProjectRoot(root);
        int opened = 0;
        auto *compute = dynamic_cast<PelicanStudio::GraphCanvasComputeFields *>(
            tab.field<QWidget>("pelican.graphCanvasCompute"));
        REQUIRE(compute != nullptr);
        compute->setOpenRayLayout([&] { ++opened; });
        const std::string a = "project://shaders/a";
        auto parameters = Json{{"ports", ct6sPorts()},
                               {"ray_tracing", {{"raygen", a}, {"miss", a}, {"closesthit", a}}},
                               {"dispatch", {{"rays_from", {{"port", "color"}}}}}};
        tab.read(ct6sDocument(parameters), "rtl2s-n0");
        tab.answer("get_graph_catalog", engineCatalog());
        tab.widget->scene().selectNode("trace");
        QCoreApplication::processEvents();
        tab.widget->poll();
        // An engine that lists names without a layout to read them from
        // (they come with a layout): the keys' lists are the names.
        auto context = rtl2sAuthoring(false);
        context["ray_layout"]["misses"] = Json::array({"sky", "shadow"});
        tab.answer("get_render_authoring_context", context);
        tab.widget->scene().selectNode("trace");
        QCoreApplication::processEvents();
        CHECK_FALSE(tab.field<QLabel>("pelican.graphCanvasParameters.ray_tracing.label")->isHidden());
        CHECK(tab.field<QLabel>("pelican.graphCanvasRayTracing.noLayout")->isVisible());
        tab.field<QPushButton>("pelican.graphCanvasRayTracing.enableHitGroups")->click();
        CHECK(opened == 1);
        tab.field<QToolButton>("pelican.graphCanvasParameters.more")->click();
        tab.field<QToolButton>("pelican.graphCanvasParameters.ray_tracing.fold")->click();
        tab.field<QPushButton>("pelican.graphCanvasParameters.ray_tracing.misses.add")->click();
        REQUIRE(ux12aWait([&] { return tab.reads("edit_graph_document") == 1; }));
        // The first listed name no row has, not "key1".
        CHECK(tab.sent(1).at("parameters").at("ray_tracing").at("misses") == Json{{"sky", ""}});
        auto *key = tab.field<QComboBox>("pelican.graphCanvasParameters.ray_tracing.misses.0.key");
        CHECK(ux2Items(*key) == QStringList{"sky", "shadow"});
        CHECK(key->currentText() == QStringLiteral("sky"));
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("RTL-2s b decision 3: a ray tracing refusal reads as one sentence saying where, and the Graph's "
          "section has that place red; the Problems window's row goes to the task's node",
          "[devstudio][graph-canvas][scene][rtl2s]") {
    (void)application();
    using PelicanStudio::rayTracingRefusalDescription;
    // The fields beside the code (engine RTL-1b / RTL-1c).
    const Json missing{{"code", "pelican.ray_tracing.hit_shader_missing@1"},
                       {"task", "trace"},
                       {"node", "trace"},
                       {"ray_type", "shadow"},
                       {"group", "mesh"},
                       {"message", "pelican.ray_tracing.hit_shader_missing@1: compute task 'trace' group='mesh' "
                                   "ray_type='shadow' available=[]"}};
    CHECK(rayTracingRefusalDescription(missing) ==
          "Task trace needs a closest-hit shader for every ray type it traces and every hit group (ray type "
          "shadow, hit group mesh) (pelican.ray_tracing.hit_shader_missing@1).");
    // The message alone says where too.
    CHECK(PelicanStudio::knownRefusalDescription({}, missing.at("message").get<std::string>()) ==
          rayTracingRefusalDescription(missing));
    // The layout's and the assignments' parsers; a rolled-back generation by
    // its own code, not the one it quotes.
    CHECK(rayTracingRefusalDescription(
              Json{{"code", "pelican.procedural_group_requires_intersection@1"}, {"task", "trace"},
                   {"ray_type", "primary"}, {"group", "ball"}, {"message", "x"}}) ==
          "A procedural hit group needs an intersection shader and a closest-hit shader (ray type primary, hit "
          "group ball) (pelican.procedural_group_requires_intersection@1).");
    CHECK(PelicanStudio::knownRefusalDescription(
              "pelican.ray_tracing.generation_rolled_back@1",
              "pelican.ray_tracing.generation_rolled_back@1: staged authored render pipeline generation was not "
              "published: pelican.ray_binding_record_invalid@1: record member set does not match layout")
              .starts_with("The engine could not draw the first frame with the new ray tracing setup"));
    CHECK(PelicanStudio::knownRefusalDescription("pelican.ray_bindings_invalid@1", "no") ==
          "The hit group assignments are not ones the engine reads (pelican.ray_bindings_invalid@1).");
    CHECK(rayTracingRefusalDescription(Json{{"code", "pelican.ray_group_name_unknown@1"},
                                            {"name", "shadow"},
                                            {"kind", "miss"},
                                            {"message", "pelican.ray_group_name_unknown@1: miss 'shadow'"}}) ==
          "The ray group layout has no such name (miss shadow) (pelican.ray_group_name_unknown@1).");

    // The Problems window: a task of the root's graph is its node.
    const auto row = PelicanStudio::readEngineProblem(
        Json{{"id", "p1"}, {"severity", "error"}, {"source", "startup"},
             {"code", "pelican.ray_tracing.hit_shader_missing@1"}, {"message", missing.at("message")},
             {"fields", {{"task", "trace"}, {"ray_type", "shadow"}, {"group", "mesh"}}}});
    REQUIRE(row.has_value());
    CHECK(row->place == PelicanStudio::ProblemRow::Place::GraphNode);
    CHECK(row->node == "trace");
    CHECK(row->sentence.find("(ray type shadow, hit group mesh)") != std::string::npos);

    // The Graph: an edit refused at commit marks the node, and the section
    // has the cell red with the sentence.
    const auto root = rtl2sProject();
    {
        Ux2Tab tab{true};
        tab.widget->setProjectRoot(root);
        // Shadow traced, its row whole: the red is the refusal's.
        auto parameters = Json{{"ports", ct6sPorts()},
                               {"ray_tracing", rtl2sRayTracing()},
                               {"dispatch", {{"rays_from", {{"port", "color"}}}}}};
        parameters["ray_tracing"]["traced_ray_types"] = Json::array({"primary", "shadow"});
        parameters["ray_tracing"]["hit_shaders"]["shadow"] = parameters["ray_tracing"]["hit_shaders"]["primary"];
        tab.read(ct6sDocument(parameters), "rtl2s-r0");
        tab.answer("get_graph_catalog", engineCatalog());
        tab.widget->scene().selectNode("trace");
        QCoreApplication::processEvents();
        tab.widget->poll();
        tab.answer("get_render_authoring_context", rtl2sAuthoring(true));
        tab.widget->poll();
        tab.answer("get_ray_layout", rtl2sRayLayout());
        tab.widget->scene().selectNode("trace");
        QCoreApplication::processEvents();
        tab.pick(tab.field<QComboBox>("pelican.graphCanvasRayTracing.hit.shadow.mesh.closesthit"),
                 QStringLiteral("project://shaders/b"));
        REQUIRE(ux12aWait([&] { return tab.reads("edit_graph_document") == 1; }));
        auto refused = missing;
        refused["participant"] = "pelican.render_pipeline";
        refused["document"] = "passes/main_rendering_config.json";
        tab.answer("edit_graph_document", Json{{"status", "rejected"}, {"committed", false}, {"error", refused}});
        tab.widget->scene().selectNode("trace");
        QCoreApplication::processEvents();
        // The canvas's notice (elided to its width) is the sentence too.
        CHECK(tab.field<QLabel>("pelican.graphCanvasNotice")
                  ->text()
                  .startsWith(QStringLiteral("Task trace needs a closest-hit shader")));
        auto *problem = tab.field<QLabel>("pelican.graphCanvasRayTracing.problem");
        CHECK(problem->isVisible());
        CHECK(problem->text().contains(QStringLiteral("(ray type shadow, hit group mesh)")));
        auto *cell = tab.field<QComboBox>("pelican.graphCanvasRayTracing.hit.shadow.mesh.closesthit");
        CHECK(cell->property("pelicanInvalid").toBool());
        CHECK_FALSE(tab.field<QComboBox>("pelican.graphCanvasRayTracing.hit.primary.mesh.closesthit")
                        ->property("pelicanInvalid")
                        .toBool());
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("RTL-2s b decision 2: New... in a project with a ray group layout writes the layout's form -- "
          "the generated header, its ray type and miss by name, the payload of the ray type a place "
          "is for, the normal from the engine's tables, a sphere for an intersection",
          "[devstudio][graph-canvas][scene][rtl2s]") {
    using PelicanStudio::RayLayoutTemplatePlace;
    using PelicanStudio::RayLayoutTemplateStage;
    const auto layout = PelicanStudio::rayLayoutTemplateNamesFromDocument(rtl2sLayoutDocument());
    REQUIRE(layout.has_value());
    const auto source = [&](RayLayoutTemplateStage stage, std::string ray_type, std::string group, std::string miss) {
        return PelicanStudio::rayLayoutShaderTemplate(*layout, RayLayoutTemplatePlace{stage, ray_type, group, miss},
                                                      "t");
    };
    const auto raygen = source(RayLayoutTemplateStage::raygen, {}, {}, {});
    for (const char *part : {"#include \"__pelican_ray_group_layout.glsl\"", "PELICAN_RAY_TYPE_primary",
                             "pelican_ray_type_count", "PELICAN_MISS_sky", "#define TRACE_SECOND_RAY_TYPE 0",
                             "PELICAN_RAY_TYPE_shadow", "PELICAN_MISS_shadow", "#define CALL_CALLABLE_0 0",
                             "executeCallableEXT(0, 0)", "pelican_resource_color"})
        CHECK(raygen.find(part) != std::string::npos);
    // The layout's header before anything it might need to come first.
    CHECK(raygen.find("__pelican_ray_group_layout.glsl") < raygen.find("pelican_sets.glsl"));
    const auto closest = source(RayLayoutTemplateStage::closest_hit, "primary", "mesh", {});
    CHECK(closest.find("pelicanRayFetchNormal(record.geometry_index") != std::string::npos);
    CHECK(closest.find("PELICAN_RAY_ENGINE_RECORD_MEMBERS") != std::string::npos);
    CHECK(closest.find("rayPayloadInEXT vec4 hit") != std::string::npos);
    CHECK(closest.find("const vec3 GROUP_TINT = vec3(1.0);") != std::string::npos);
    // Another group reads apart; a later ray type's says it is occluded.
    CHECK(source(RayLayoutTemplateStage::closest_hit, "primary", "ball", {}).find("GROUP_TINT = vec3(1.0)") ==
          std::string::npos);
    const auto shadow = source(RayLayoutTemplateStage::closest_hit, "shadow", "mesh", {});
    CHECK(shadow.find("layout(location = 1) rayPayloadInEXT float occluded;") != std::string::npos);
    CHECK(shadow.find("occluded = 1.0;") != std::string::npos);
    CHECK(source(RayLayoutTemplateStage::miss, {}, {}, "sky").find("hit = vec4(0.0);") != std::string::npos);
    CHECK(source(RayLayoutTemplateStage::intersection, {}, "ball", {}).find("reportIntersectionEXT") !=
          std::string::npos);
    CHECK(source(RayLayoutTemplateStage::any_hit, "primary", "mesh", {}).find("ignoreIntersectionEXT") !=
          std::string::npos);
    CHECK(source(RayLayoutTemplateStage::callable, {}, {}, {}).find("callableDataInEXT vec4 callable_color") !=
          std::string::npos);
    // Without the engine's record: the position it hit, no shader record.
    auto plain = *layout;
    plain.engine_record.clear();
    const auto position = PelicanStudio::rayLayoutShaderTemplate(
        plain, RayLayoutTemplatePlace{RayLayoutTemplateStage::closest_hit, "primary", "mesh", {}}, "t");
    CHECK(position.find("pelicanRayFetchNormal") == std::string::npos);
    CHECK(position.find("shaderRecordEXT") == std::string::npos);

    // Written only when the file is not there; the set fills every cell.
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_rtl2s_t_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CHECK(PelicanStudio::rayLayoutTemplateName(root, RayLayoutTemplateStage::closest_hit, "x") == "x");
    CHECK(PelicanStudio::writeRayLayoutTemplate(root, "x", *layout,
                                                {RayLayoutTemplateStage::closest_hit, "primary", "mesh", {}})
              .created);
    CHECK_FALSE(PelicanStudio::writeRayLayoutTemplate(root, "x", *layout,
                                                      {RayLayoutTemplateStage::closest_hit, "primary", "mesh", {}})
                    .created);
    CHECK(PelicanStudio::rayLayoutTemplateName(root, RayLayoutTemplateStage::closest_hit, "x") == "x_2");
    CHECK(PelicanStudio::rayLayoutTemplateName(root, RayLayoutTemplateStage::miss, "x") == "x");
    const auto set = PelicanStudio::writeRayTracingTemplate(root, "set", *layout);
    CHECK(set.created);
    for (const char *file : {"set.rgen", "set.rmiss", "set.rchit", "set.rint"})
        CHECK(std::filesystem::exists(root / "shaders" / file));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

// --- GL-5e: what was left only on the Frame Plan, on the Graph.

TEST_CASE("GL-5e a rendering-section pass's View field, a click on its passthrough socket, Delete "
          "pass on a managed fragment, the ways out of a refused shader, a line on a folded node "
          "and the Layers section's right-click are the Frame Plan's",
          "[devstudio][graph-canvas][scene][gl5e]") {
    (void)application();
    bool reload_refused = false;
    Gl5bTab tab{{}, [&](Gl5bTab &self) {
                    self.inputs = {"shade__color", "other_color"};
                    self.plan_edit = [&reload_refused](Json &plan) {
                        // Decision 1: present runs in a family with an axis.
                        auto &present = plan["nodes"][1];
                        present["view_family"] = "$capture/cube";
                        present["view_axes"] = Json::array(
                            {{{"axis", "face"},
                              {"length", 6},
                              {"labels", Json::array({"+X", "-X", "+Y", "-Y", "+Z", "-Z"})}}});
                        // Decision 4: a reload refused binding 1 of present.
                        if (reload_refused)
                            present["pass_inputs"]["reload_refused"] = {
                                {"reason", "the new shader no longer declares binding 1"},
                                {"declarations",
                                 Json::array({{{"binding", 0},
                                               {"type", "sampled_image"},
                                               {"name", "in_a"},
                                               {"variadic", false}}})}};
                        // Decision 3: a pass the project authored (a managed
                        // fragment). Decision 6: a project feature's authored
                        // task, and an engine feature's pass.
                        auto tint = gl1bPass("tint", 2, "feature:authored_pass_abc", Json::array(),
                                             Json::array({"tint_color"}));
                        tint["provider_feature"] = "authored_pass_abc";
                        tint["provider_ref"] = "project://passes/authoring/pass-abc.json";
                        auto task = gl1bPass("blur_task", 3, "feature:blur",
                                             Json::array({"blur_in"}), Json::array({"blur_out"}));
                        task["kind"] = "compute";
                        task["authored_name"] = "blur_task";
                        task["provider_feature"] = "blur";
                        task["provider_ref"] = "project://features/blur.json";
                        auto bloom = gl1bPass("bloom_down", 4, "feature:bloom",
                                              Json::array({"bloom_in"}), Json::array({"bloom_out"}));
                        bloom["provider_feature"] = "bloom";
                        bloom["provider_ref"] = "engine://features/bloom.json";
                        plan["nodes"].push_back(std::move(tint));
                        plan["nodes"].push_back(std::move(task));
                        plan["nodes"].push_back(std::move(bloom));
                        // Decision 5: the backend candidates.
                        plan["physical_target_plan"] = {
                            {"backend_selection",
                             {{"selected_candidate", "vulkan_raster"},
                              {"candidates", Json::array({{{"candidate", "vulkan_raster"},
                                                           {"endpoint", "vulkan"},
                                                           {"feasible", true}}})}}}};
                    };
                    self.authoring_edit = [](Json &context) {
                        context["graphs"][0]["passes"].push_back(
                            {{"name", "tint"},
                             {"declaration_index", 1},
                             {"declaration", {{"type", "fullscreen"}}},
                             {"provenance",
                              {{"source", "feature"},
                               {"provider_feature", "authored_pass_abc"},
                               {"provider_reference", "project://passes/authoring/pass-abc.json"}}},
                             {"managed", true}});
                    };
                }};
    auto &widget = *tab.widget;
    auto &scene = widget.scene();
    widget.setProjectRoot(std::filesystem::temp_directory_path());
    std::vector<std::pair<QString, QString>> removed;
    std::vector<std::pair<PelicanStudio::GraphCanvasWidget::ShaderContract, bool>> contracts;
    std::vector<std::tuple<QString, QString, bool>> refusals;
    std::vector<std::pair<QString, QStringList>> tasks;
    std::vector<QString> pins;
    int freezes = 0;
    PelicanStudio::GraphCanvasWidget::FramePlanEdits edits;
    edits.remove_pass = [&](const QString &graph, const QString &pass) {
        removed.emplace_back(graph, pass);
    };
    edits.shader_contract = [&](const PelicanStudio::GraphCanvasWidget::ShaderContract &contract,
                                bool use_passthrough) { contracts.emplace_back(contract, use_passthrough); };
    edits.reload_refusal = [&](const QString &graph, const QString &pass, bool disconnect) {
        refusals.emplace_back(graph, pass, disconnect);
    };
    edits.pin = [&](const QString &candidate) { pins.push_back(candidate); };
    edits.unpin = [] {};
    edits.freeze = [&] { ++freezes; };
    edits.remove_frozen = [] {};
    edits.task_connection = [&](const QString &task, const QStringList &outputs) {
        tasks.emplace_back(task, outputs);
    };
    edits.preset_pass_connection = [](const QString &, const QString &, const QStringList &) {};
    widget.setFramePlanEdits(edits);
    const auto action = [&](const char *name) {
        auto *found = widget.findChild<QAction *>(QString::fromLatin1(name));
        REQUIRE(found != nullptr);
        return found;
    };
    const auto button = [&](const char *name) {
        auto *found = widget.findChild<QPushButton *>(
            QStringLiteral("pelican.graphCanvasNodeActions.") + QString::fromLatin1(name));
        REQUIRE(found != nullptr);
        return found;
    };
    const auto pick = [&](const char *node) {
        scene.selectNode(node);
        QCoreApplication::processEvents();
    };
    const auto fire = [&](const char *name) {
        REQUIRE(action(name)->isEnabled());
        action(name)->trigger();
        QCoreApplication::processEvents();
    };
    const auto commit = [&](const char *method, const char *ticket) {
        tab.answer(method, Json{{"status", "accepted"}, {"ticket", ticket}});
        tab.answer("get_edit_result", Json{{"status", "committed"},
                                           {"committed", true},
                                           {"source_digest", {{"hex", "gl1e-root"}}}});
        tab.reads();
    };
    auto *notice = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasNotice"));
    REQUIRE(notice != nullptr);
    const auto authoring = [&] {
        auto context = gl5bAuthoring(tab.extra, tab.inputs);
        tab.authoring_edit(context);
        return PelicanStudio::framePlanAuthoringFromContext(context);
    };

    // Decision 1: present's View field is LP-4b's, and a pick is the Frame
    // Plan's View menu's edit_preset_pass.
    pick("present");
    auto *face = widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasFoldedPass.view.face"));
    REQUIRE(face != nullptr);
    CHECK(face->isVisible());
    REQUIRE(face->count() == 7);
    CHECK(face->itemText(3) == QStringLiteral("2 (+Y)"));
    face->setCurrentIndex(3);
    QCoreApplication::processEvents();
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_preset_pass")) == 1);
    CHECK(tab.sent("edit_preset_pass") ==
          PelicanStudio::framePlanPassViewSelectRequest(authoring(), "main_render", "present", "face", 2)
              .params);
    CHECK(tab.sent("edit_preset_pass").at("view_select") == Json{{"face", 2}});
    commit("edit_preset_pass", "gl5e-view");

    // Decision 2: the input engine://passthrough shows is marked, and a
    // click on another connected socket of present is the Frame Plan's click.
    const auto sockets = scene.passInputSockets("present");
    REQUIRE(sockets);
    REQUIRE(sockets->sockets.size() >= 2);
    CHECK(sockets->sockets[0].label.ends_with("(shown)"));
    QGraphicsItem *second = nullptr;
    for (QGraphicsItem *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() ==
                QLatin1String{PelicanStudio::GraphCanvasDerivedSocketItem} &&
            item->data(PelicanStudio::NodeGraphNameRole).toString() == QStringLiteral("present") &&
            item->data(PelicanStudio::GraphCanvasSocketKeyRole).toString() == QStringLiteral("in@1"))
            second = item;
    REQUIRE(second != nullptr);
    click(scene, second->sceneBoundingRect().center());
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_preset_pass")) == 2);
    const auto shown = tab.sent("edit_preset_pass");
    CHECK(shown.at("shader").at("defines") == Json::array({"PELICAN_PASSTHROUGH_INPUT=1"}));
    CHECK(shown.at("input") == Json::array({"shade__color", "other_color"}));

    // Decision 4: refused because the shader does not read an input -- the
    // Frame Plan's two ways out, in the detail column and under the notice.
    tab.answer("edit_preset_pass",
               Json{{"status", "rejected"},
                    {"error",
                     {{"code", "render_pipeline_preflight_failed"},
                      {"message", "Fullscreen pass input texture/local-read contract does not match "
                                  "shader reflection: pass 'present' input 1 ('other_color') expects "
                                  "sampled_image, but fragment 'engine://passthrough' declares "
                                  "nothing at binding 1 (shader declares 1 pass inputs)"},
                      {"shader_input_mismatch",
                       {{"pass", "present"},
                        {"fragment", "engine://passthrough"},
                        {"kind", "texture"},
                        {"binding", 1},
                        {"resource", "other_color"},
                        {"expected", "sampled_image"},
                        {"pass_texture_inputs", 2},
                        {"pass_buffer_inputs", 0},
                        {"shader_pass_inputs",
                         Json::array({{{"binding", 0}, {"type", "sampled_image"}}})}}}}}});
    pick("present");
    CHECK(button("usePassthrough")->isVisible());
    CHECK(button("openContractShader")->isVisible());
    auto *under_notice = widget.findChild<QPushButton *>(
        QStringLiteral("pelican.graphCanvasNoticeActions.usePassthrough"));
    REQUIRE(under_notice != nullptr);
    CHECK(under_notice->isVisible());
    fire("pelican.graphCanvas.usePassthrough");
    REQUIRE(contracts.size() == 1);
    CHECK(contracts[0].second);
    CHECK(contracts[0].first.pass == "present");
    CHECK(contracts[0].first.fragment == "engine://passthrough");
    CHECK(contracts[0].first.inputs == std::vector<std::string>{"shade__color", "other_color"});
    CHECK_FALSE(button("usePassthrough")->isVisible());
    // And a reload that no longer declares binding 1: Disconnect.
    reload_refused = true;
    widget.shadersReloaded();
    tab.reads();
    pick("present");
    CHECK(button("disconnectRefused")->isVisible());
    fire("pelican.graphCanvas.disconnectRefused");
    REQUIRE(refusals.size() == 1);
    CHECK(refusals[0] == std::tuple{QStringLiteral("main_render"), QStringLiteral("present"), true});

    // Decision 3: Delete pass on the managed fragment's folded node -- its
    // button, its right-click and the Delete key -- not on a feature's.
    pick("feature:authored_pass_abc");
    CHECK(button("deletePass")->isVisible());
    fire("pelican.graphCanvas.deletePass");
    REQUIRE(removed.size() == 1);
    CHECK(removed[0] == std::pair{QStringLiteral("main_render"), QStringLiteral("tint")});
    {
        auto *item = nodeItem(scene, QStringLiteral("feature:authored_pass_abc"));
        REQUIRE(item != nullptr);
        CHECK(canvasMenuOver(scene, *item).contains(QStringLiteral("pelican.graphCanvas.deletePass")));
    }
    pick("feature:authored_pass_abc");
    pressKey(scene, Qt::Key_Delete);
    CHECK(removed.size() == 2);
    pick("feature:bloom");
    CHECK_FALSE(button("deletePass")->isVisible());

    // Decision 6: a line from shade onto the project feature's folded node is
    // the Frame Plan's Shift-drag on its authored task; onto the engine
    // feature's, nothing is sent and the notice says it is read only here.
    scene.clearPicks();
    QCoreApplication::processEvents();
    auto *from = socketItem(scene, QStringLiteral("shade"), QStringLiteral("color"), QStringLiteral("output"));
    REQUIRE(from != nullptr);
    auto *blur = nodeItem(scene, QStringLiteral("feature:blur"));
    REQUIRE(blur != nullptr);
    drag(scene, from->sceneBoundingRect().center(), blur->sceneBoundingRect().center());
    REQUIRE(tasks.size() == 1);
    CHECK(tasks[0] == std::pair{QStringLiteral("blur_task"), QStringList{QStringLiteral("shade__color")}});
    auto *bloom = nodeItem(scene, QStringLiteral("feature:bloom"));
    REQUIRE(bloom != nullptr);
    const int graph_edits = tab.transport.countOf(QStringLiteral("edit_graph_document"));
    drag(scene, from->sceneBoundingRect().center(), bloom->sceneBoundingRect().center());
    CHECK(tasks.size() == 1);
    CHECK(tab.transport.countOf(QStringLiteral("edit_graph_document")) == graph_edits);
    auto *folded_notice = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasFoldedNotice"));
    REQUIRE(folded_notice != nullptr);
    CHECK(folded_notice->isVisible());
    CHECK(folded_notice->text().contains(QStringLiteral("read only here")));

    // Decision 5: the whole graph's Layers -- a candidate's row pins it, and
    // Freeze this physical plan is the Frame Plan's.
    scene.clearPicks();
    QCoreApplication::processEvents();
    auto *layers = widget.findChild<QTreeWidget *>(QStringLiteral("pelican.graphCanvasLayers"));
    REQUIRE(layers != nullptr);
    QTreeWidgetItem *candidate = nullptr;
    for (QTreeWidgetItemIterator it{layers}; *it != nullptr; ++it)
        if ((*it)->text(0).startsWith(QStringLiteral("candidate vulkan_raster"))) candidate = *it;
    REQUIRE(candidate != nullptr);
    layers->setCurrentItem(candidate);
    QCoreApplication::processEvents();
    fire("pelican.graphCanvas.layers.pin");
    CHECK(pins == std::vector<QString>{QStringLiteral("vulkan_raster")});
    fire("pelican.graphCanvas.layers.freeze");
    fire("pelican.graphCanvas.layers.ejectAgain");
    CHECK(freezes == 2);
}

TEST_CASE("GL-5f Delete task on a compute task the project authored -- inside its feature's fold, "
          "or the rendering section's -- is the Frame Plan's Delete: its button, its right-click and "
          "the Delete key; not on an engine feature's task, nor on a feature's fold",
          "[devstudio][graph-canvas][scene][gl5f]") {
    (void)application();
    Gl5bTab tab{{}, [](Gl5bTab &self) {
                    self.plan_edit = [](Json &plan) {
                        const auto task = [](const std::string &name, std::size_t order,
                                             const std::string &source, const std::string &feature,
                                             const std::string &reference) {
                            auto pass = gl1bPass(name, order, source, Json::array(),
                                                 Json::array({name + "_out"}));
                            pass["kind"] = "compute";
                            pass["authored_name"] = name;
                            if (!feature.empty()) pass["provider_feature"] = feature;
                            if (!reference.empty()) pass["provider_ref"] = reference;
                            return pass;
                        };
                        // A project feature with two tasks, an engine feature's
                        // task, and one the rendering section declares.
                        plan["nodes"].push_back(
                            task("blur_task", 2, "feature:blur", "blur", "project://features/blur.json"));
                        plan["nodes"].push_back(
                            task("blur_mix", 3, "feature:blur", "blur", "project://features/blur.json"));
                        plan["nodes"].push_back(task("bloom_task", 4, "feature:bloom", "bloom",
                                                     "engine://features/bloom.json"));
                        plan["nodes"].push_back(task("root_task", 5, "project", "", ""));
                    };
                }};
    auto &widget = *tab.widget;
    auto &scene = widget.scene();
    std::vector<std::pair<QString, QString>> removed;
    PelicanStudio::GraphCanvasWidget::FramePlanEdits edits;
    edits.remove_pass = [&](const QString &graph, const QString &pass) {
        removed.emplace_back(graph, pass);
    };
    std::vector<QString> edited;
    edits.edit_task = [&](const QString &task) { edited.push_back(task); };
    widget.setFramePlanEdits(edits);
    auto *edit = widget.findChild<QAction *>(QStringLiteral("pelican.graphCanvas.editTask"));
    REQUIRE(edit != nullptr);
    auto *remove = widget.findChild<QAction *>(QStringLiteral("pelican.graphCanvas.deletePass"));
    REQUIRE(remove != nullptr);
    auto *button = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasNodeActions.deletePass"));
    REQUIRE(button != nullptr);
    const auto pick = [&](const char *node) {
        scene.selectNode(node);
        QCoreApplication::processEvents();
    };

    // The rendering section's task, at the root: Delete task.
    pick("root_task");
    CHECK(remove->isVisible());
    CHECK(remove->text() == QStringLiteral("Delete task"));
    CHECK(button->isVisible());
    CHECK(button->text() == QStringLiteral("Delete task"));
    REQUIRE(remove->isEnabled());
    remove->trigger();
    QCoreApplication::processEvents();
    REQUIRE(removed.size() == 1);
    CHECK(removed[0] == std::pair{QStringLiteral("main_render"), QStringLiteral("root_task")});
    // And Edit task…: the Frame Plan's Edit task (the Task Editor) on it.
    pick("root_task");
    REQUIRE(edit->isVisible());
    REQUIRE(edit->isEnabled());
    edit->trigger();
    QCoreApplication::processEvents();
    CHECK(edited == std::vector<QString>{QStringLiteral("root_task")});
    // A feature's fold is the feature, not one of its tasks.
    pick("feature:blur");
    CHECK_FALSE(remove->isVisible());
    {
        auto *item = nodeItem(scene, QStringLiteral("feature:blur"));
        REQUIRE(item != nullptr);
        CHECK_FALSE(canvasMenuOver(scene, *item).contains(QStringLiteral("pelican.graphCanvas.deletePass")));
    }

    SECTION("inside the project feature's fold: each task, by the right-click and the Delete key too") {
        REQUIRE(scene.enterFolded("feature:blur"));
        QCoreApplication::processEvents();
        pick("blur_mix");
        CHECK(remove->isVisible());
        CHECK(remove->text() == QStringLiteral("Delete task"));
        {
            auto *item = nodeItem(scene, QStringLiteral("blur_mix"));
            REQUIRE(item != nullptr);
            CHECK(canvasMenuOver(scene, *item).contains(QStringLiteral("pelican.graphCanvas.deletePass")));
        }
        pick("blur_task");
        pressKey(scene, Qt::Key_Delete);
        REQUIRE(removed.size() == 2);
        CHECK(removed[1] == std::pair{QStringLiteral("main_render"), QStringLiteral("blur_task")});
    }
    SECTION("inside an engine feature's fold: its task is read only here") {
        REQUIRE(scene.enterFolded("feature:bloom"));
        QCoreApplication::processEvents();
        pick("bloom_task");
        CHECK_FALSE(remove->isVisible());
        CHECK_FALSE(edit->isVisible());
        pressKey(scene, Qt::Key_Delete);
        CHECK(removed.size() == 1);
    }
}

TEST_CASE("GL-5f with no row in Layers, the heading stays with nothing picked and its right-click "
          "holds the whole graph's decisions: the Frame Plan's Unpin, Freeze, Eject again and Remove",
          "[devstudio][graph-canvas][scene][gl5f]") {
    (void)application();
    Gl5bTab tab;
    auto &widget = *tab.widget;
    int unpins = 0;
    int freezes = 0;
    int removes = 0;
    PelicanStudio::GraphCanvasWidget::FramePlanEdits edits;
    edits.unpin = [&] { ++unpins; };
    edits.freeze = [&] { ++freezes; };
    edits.remove_frozen = [&] { ++removes; };
    widget.setFramePlanEdits(edits);
    widget.scene().clearPicks();
    QCoreApplication::processEvents();
    auto *toggle = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasLayersToggle"));
    REQUIRE(toggle != nullptr);
    auto *layers = widget.findChild<QTreeWidget *>(QStringLiteral("pelican.graphCanvasLayers"));
    REQUIRE(layers != nullptr);
    // Blank's plan: no candidate, no decision -- no row.
    CHECK(layers->topLevelItemCount() == 0);
    CHECK_FALSE(layers->isVisible());
    CHECK(toggle->isVisible());
    CHECK(toggle->contextMenuPolicy() == Qt::CustomContextMenu);
    CHECK(toggle->toolTip().contains(QStringLiteral("whole graph")));
    for (const char *name : {"pelican.graphCanvas.layers.unpin", "pelican.graphCanvas.layers.freeze",
                             "pelican.graphCanvas.layers.ejectAgain",
                             "pelican.graphCanvas.layers.removeFrozen"}) {
        auto *action = widget.findChild<QAction *>(QString::fromLatin1(name));
        REQUIRE(action != nullptr);
        INFO(name);
        REQUIRE(action->isEnabled());
        action->trigger();
        QCoreApplication::processEvents();
    }
    CHECK(unpins == 1);
    CHECK(freezes == 2);
    CHECK(removes == 1);
}

// --- GL-5g: a raster pass of the rendering section, and the Decisions of a
// pass the detail column does not edit.

namespace {

// gl1e's root and plan with a raster pass `tri` the rendering section
// declares beside present, and shade's image as a render target of the plan
// (so No alias is offered for it).
Json gl5gTriangle() {
    return {{"type", "raster"},
            {"input", Json::array()},
            {"output", {{"color", Json::array({"display"})}, {"depth", nullptr}}},
            {"shader", {{"vertex", "project://shaders/tri"}, {"fragment", "project://shaders/tri"}}},
            {"draw", {{"operation", "direct"}, {"vertex_count", 3}}}};
}

Json gl5gPlan() {
    auto plan = gl1ePlan({});
    plan["nodes"].push_back(gl1bPass("tri", 2, "project", Json::array(), Json::array({"display"})));
    plan["resources"] = Json::array({{{"name", "shade__color"}, {"kind", "render_target"}},
                                     {{"name", "display"}, {"kind", "image"}}});
    return plan;
}

Json gl5gAuthoring() {
    auto context = gl1eAuthoring({});
    const auto tri = gl5gTriangle();
    context["graphs"][0]["passes"].push_back({{"name", "tri"},
                                              {"declaration_index", 1},
                                              {"editable_root_pass", true},
                                              {"declaration", tri},
                                              {"root_declaration", tri}});
    return context;
}

struct Gl5gTab {
    RecordedCapability transport;
    std::unique_ptr<PelicanStudio::GraphCanvasWidget> widget;

    Gl5gTab() {
        (void)application();
        auto capability = transport.capability();
        const auto record = [this](const char *method, const QJsonObject &params) {
            RecordedCapability::Call call{QString::fromLatin1(method)};
            call.body = QJsonDocument(params).toJson(QJsonDocument::Compact);
            transport.calls.push_back(call);
            return transport.next++;
        };
        capability.requestFramePlan = [this](QString *) {
            transport.calls.push_back({QStringLiteral("get_frame_plan")});
            return transport.next++;
        };
        capability.requestRenderAuthoringContext = [this](QString *) {
            transport.calls.push_back({QStringLiteral("get_render_authoring_context")});
            return transport.next++;
        };
        capability.requestEditPresetPass = [record](const QJsonObject &params, QString *) {
            return record("edit_preset_pass", params);
        };
        capability.requestEditTargetPlanning = [record](const QJsonObject &params, QString *) {
            return record("edit_target_planning", params);
        };
        capability.requestRootDocument = [this](const QString &reference, QString *) {
            transport.calls.push_back({QStringLiteral("get_render_documents"), reference});
            return transport.next++;
        };
        widget = std::make_unique<PelicanStudio::GraphCanvasWidget>(capability);
        widget->resize(1100, 800);
        widget->show();
        QCoreApplication::processEvents();
        answer("get_graph_catalog", visualCatalog());
        answer("get_graph_instances", gl1eRead());
        answer("get_frame_plan", gl5gPlan());
        answer("get_render_authoring_context", gl5gAuthoring());
    }

    void answer(const char *method, const Json &reply) {
        const qint64 id = transport.idOf(QString::fromLatin1(method));
        REQUIRE(id != 0);
        transport.deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
    }

    Json sent(const char *method) const {
        const auto id = transport.idOf(QString::fromLatin1(method));
        REQUIRE(id != 0);
        return Json::parse(transport.calls[static_cast<std::size_t>(id - 1)].body.toStdString());
    }

    // The root as get_render_documents gives it (no decisions yet).
    void answerRoot() {
        const Json content{{"schema", "pelican.graph"}, {"rendering", Json::object()}};
        answer("get_render_documents",
               Json{{"documents", Json::array({{{"reference", "fixture"}, {"content", content.dump()}}})}});
    }

    template <class T> T *child(const char *name) const {
        return widget->findChild<T *>(QString::fromLatin1(name));
    }
};

} // namespace

TEST_CASE("GL-5g a raster pass of the rendering section is edited in the detail column: its "
          "shaders, defines, raster_state and draw, each change one edit_preset_pass of what changed",
          "[devstudio][graph-canvas][scene][gl5g]") {
    Gl5gTab tab;
    const auto folded = tab.widget->model().foldedNodes();
    const auto tri = std::ranges::find(folded, std::string{"tri"},
                                       &PelicanStudio::GraphCanvasFoldedNode::id);
    REQUIRE(tri != folded.end());
    CHECK(tri->rendering_section);
    auto *fields = tab.child<QWidget>("pelican.graphCanvasFoldedPass");
    REQUIRE(fields != nullptr);
    tab.widget->scene().selectNode("tri");
    QCoreApplication::processEvents();
    // Before GL-5g a raster pass took the column away.
    REQUIRE(fields->isVisible());
    auto *vertex = tab.child<QComboBox>("pelican.graphCanvasFoldedPass.vertex");
    auto *fragment = tab.child<QComboBox>("pelican.graphCanvasFoldedPass.fragment");
    REQUIRE(vertex != nullptr);
    REQUIRE(fragment != nullptr);
    CHECK(vertex->isVisible());
    CHECK(vertex->currentData().toString() == QStringLiteral("project://shaders/tri"));
    CHECK(fragment->currentData().toString() == QStringLiteral("project://shaders/tri"));
    // The Pass Editor's raster parts: raster_state, with its draw.
    auto *raster = tab.child<QWidget>("pelican.graphCanvasFoldedPass.rasterState");
    auto *draw = tab.child<QWidget>("pelican.graphCanvasFoldedPass.draw");
    REQUIRE(raster != nullptr);
    REQUIRE(draw != nullptr);
    CHECK(raster->isVisible());
    CHECK(draw->isVisible());
    // A fullscreen pass's Frame data is not a raster pass's.
    auto *present_frame_data = tab.child<QWidget>("pelican.graphCanvasFoldedPass.frameData");
    if (present_frame_data != nullptr) CHECK_FALSE(present_frame_data->isVisible());

    // A fragment picked is one edit_preset_pass of tri, built as the Pass
    // Editor's Save builds it (presetPassEditParams): the declaration with
    // the shader replaced, its draw kept.
    const int index = fragment->findData(QStringLiteral("engine://fullscreen"));
    REQUIRE(index >= 0);
    fragment->setCurrentIndex(index);
    QCoreApplication::processEvents();
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_preset_pass")) == 1);
    const auto params = tab.sent("edit_preset_pass");
    CHECK(params.value("pass_name", std::string{}) == "tri");
    CHECK(params.value("graph", std::string{}) == "main_render");
    REQUIRE(params.contains("shader"));
    CHECK(params["shader"] ==
          Json{{"vertex", "project://shaders/tri"}, {"fragment", "engine://fullscreen"}});
    // Only what changed is sent for a pass that is not fullscreen
    // (presetPassEditParams): the draw and raster_state stay the root's.
    CHECK_FALSE(params.contains("draw"));
    CHECK_FALSE(params.contains("raster_state"));
    // A change of the draw is one edit of the draw.
    auto *count = tab.child<QSpinBox>("pelican.graphCanvasFoldedPass.draw.vertexCount");
    REQUIRE(count != nullptr);
    tab.answer("edit_preset_pass", Json{{"status", "accepted"}, {"ticket", "gl5g-tri"}});
    tab.answer("get_edit_result", Json{{"status", "committed"},
                                       {"committed", true},
                                       {"source_digest", {{"hex", "gl5g-root-2"}}}});
    count->setValue(6);
    // A value typed goes when the typing stops (half a second).
    QElapsedTimer waited;
    waited.start();
    while (tab.transport.countOf(QStringLiteral("edit_preset_pass")) < 2 && waited.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_preset_pass")) == 2);
    const auto drawn = tab.sent("edit_preset_pass");
    REQUIRE(drawn.contains("draw"));
    CHECK(drawn["draw"].value("vertex_count", 0) == 6);
    // Present, a fullscreen pass, still shows its own fields and no raster
    // part.
    tab.widget->scene().selectNode("present");
    QCoreApplication::processEvents();
    CHECK(fields->isVisible());
    CHECK_FALSE(raster->isVisible());
}

TEST_CASE("GL-5g a graph node's pass shows Serial, Isolate and No alias in the detail column; "
          "each is the Frame Plan's edit_target_planning of the plan's names",
          "[devstudio][graph-canvas][scene][gl5g]") {
    Gl5gTab tab;
    auto *fields = tab.child<QWidget>("pelican.graphCanvasFoldedPass");
    REQUIRE(fields != nullptr);
    CHECK_FALSE(fields->isVisible());
    // shade is the graph's render.fullscreen; the plan runs it as `shade`.
    tab.widget->scene().selectNode("shade");
    QCoreApplication::processEvents();
    REQUIRE(fields->isVisible());
    auto *heading = tab.child<QLabel>("pelican.graphCanvasFoldedPass.heading");
    REQUIRE(heading != nullptr);
    CHECK(heading->text() == QStringLiteral("shade: its decisions"));
    // The editing fields are not this column's to show for a graph node (its
    // Pass fields are).
    auto *vertex = tab.child<QComboBox>("pelican.graphCanvasFoldedPass.vertex");
    REQUIRE(vertex != nullptr);
    CHECK_FALSE(vertex->isVisible());
    tab.answerRoot();
    auto *serial = tab.child<QCheckBox>("pelican.graphCanvasFoldedPass.serial");
    auto *isolate = tab.child<QCheckBox>("pelican.graphCanvasFoldedPass.isolate");
    auto *no_alias = tab.child<QCheckBox>("pelican.graphCanvasFoldedPass.noAlias.shade__color");
    REQUIRE(serial != nullptr);
    REQUIRE(isolate != nullptr);
    REQUIRE(no_alias != nullptr);
    CHECK(serial->isVisible());
    CHECK(no_alias->isVisible());
    CHECK_FALSE(serial->isChecked());

    serial->click();
    QCoreApplication::processEvents();
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_target_planning")) == 1);
    CHECK(tab.sent("edit_target_planning") ==
          PelicanStudio::graphTargetPlanningParams("gl1e-root", "main_render", "node", "shade",
                                                   "serial", true));
    tab.answer("edit_target_planning", Json{{"status", "accepted"}, {"ticket", "gl5g-serial"}});
    tab.answer("get_edit_result", Json{{"status", "committed"},
                                       {"committed", true},
                                       {"source_digest", {{"hex", "gl5g-root-2"}}}});
    auto *notice = tab.child<QLabel>("pelican.graphCanvasNotice");
    REQUIRE(notice != nullptr);
    CHECK(notice->text() == QStringLiteral("Serial set on shade."));

    no_alias->click();
    QCoreApplication::processEvents();
    REQUIRE(tab.transport.countOf(QStringLiteral("edit_target_planning")) == 2);
    CHECK(tab.sent("edit_target_planning") ==
          PelicanStudio::graphTargetPlanningParams("gl1e-root", "main_render", "resource",
                                                   "shade__color", "no_alias", true));

    // present, the rendering section's pass, is edited here again: its
    // fields come back with its Decisions.
    tab.widget->scene().selectNode("present");
    QCoreApplication::processEvents();
    CHECK(fields->isVisible());
    CHECK(vertex->isVisible());
    CHECK(heading->text() != QStringLiteral("shade: its decisions"));
}


TEST_CASE("Graph drag stays attached across a crowded scene and rebuild",
          "[devstudio][graph-canvas][scene][drag][navigation-fix]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    model.setConnected(true);
    answer(model, connectedDocument());
    scene.setModel(&model);
    for (int generation = 0; generation < 2; ++generation) {
        auto *node = nodeItem(scene, QStringLiteral("left"));
        REQUIRE(node != nullptr);
        const auto edges = itemsOfKind(scene, PelicanStudio::GraphCanvasEdgeItem);
        REQUIRE(edges.size() == 1);
        auto *edge = dynamic_cast<QGraphicsPathItem *>(edges.front());
        REQUIRE(edge != nullptr);
        for (int i = 0; i < 1500; ++i)
            scene.addRect(i % 100, i / 100, 1, 1);
        const auto start = node->pos();
        QElapsedTimer timer;
        timer.start();
        for (int step = 1; step <= 80; ++step) {
            const auto previous = edge->path();
            node->setPos(start + QPointF{step * 2.0, step * 1.0});
            REQUIRE(edge->path() != previous);
        }
        if (qEnvironmentVariableIsSet("PELICAN_STUDIO_DRAG_BENCHMARK"))
            WARN("Graph: 80 drag updates " << timer.nsecsElapsed() / 1e6 << " ms");
        model.refresh();
        answer(model, connectedDocument());
        scene.repopulate();
        REQUIRE(nodeItem(scene, QStringLiteral("left"))->pos() == start + QPointF{160, 80});
    }
}

TEST_CASE("Sibling node selection reuses fields and edits only the newly selected node",
          "[devstudio][graph-canvas][scene][input-latency]") {
    (void)application();
    Ux2Tab tab{false};
    auto doc = ux12aBufferDocument(Json{{"element", "vec4"}, {"size", 16}});
    auto sibling = doc["nodes"][0];
    sibling["id"] = "other";
    sibling["parameters"]["size"] = 32;
    doc["nodes"].push_back(sibling);
    tab.read(doc, "reuse-fields-0");
    tab.answer("get_graph_catalog", engineCatalog());
    tab.widget->scene().selectNode("buf");
    QCoreApplication::processEvents();
    QPointer<QDoubleSpinBox> size = tab.field<QDoubleSpinBox>("pelican.graphCanvasParameters.size");
    REQUIRE(size);
    tab.widget->scene().selectNode("other");
    QCoreApplication::processEvents();
    REQUIRE(size);
    CHECK(tab.field<QDoubleSpinBox>("pelican.graphCanvasParameters.size") == size);
    CHECK(size->value() == 32);
    auto *lifetime = tab.field<QComboBox>("pelican.graphCanvasParameters.lifetime");
    tab.pick(lifetime, QStringLiteral("transient"));
    const auto op = tab.sent(1);
    CHECK(op.at("node") == "other");
    CHECK(op.at("parameters").at("size") == 32);
}

TEST_CASE("Mouse selection highlights immediately and loads the latest fields after the drag",
          "[devstudio][graph-canvas][scene][input-latency]") {
    (void)application();
    Ux2Tab tab{false};
    auto doc = ux12aBufferDocument(Json{{"element", "vec4"}, {"size", 16}});
    auto sibling = doc["nodes"][0];
    sibling["id"] = "other";
    sibling["parameters"]["size"] = 32;
    doc["nodes"].push_back(sibling);
    tab.read(doc, "mouse-fields-0");
    tab.answer("get_graph_catalog", engineCatalog());
    auto &scene = tab.widget->scene();
    scene.selectNode("buf");
    QCoreApplication::processEvents();
    auto *view = tab.field<QGraphicsView>("pelican.graphCanvasView");
    auto *other = nodeItem(scene, QStringLiteral("other"));
    REQUIRE(other != nullptr);
    view->centerOn(other);
    QCoreApplication::processEvents();
    const QPoint press = view->mapFromScene(other->mapToScene(QPointF{60, 15}));
    auto *port = view->viewport();
    REQUIRE(port->rect().contains(press));
    const auto send = [&](QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent event{type, QPointF{at}, QPointF{at}, port->mapToGlobal(QPointF{at}), button, held, Qt::NoModifier};
        QApplication::sendEvent(port, &event);
        QCoreApplication::processEvents();
    };
    const QPoint global_press = port->mapToGlobal(press);
    send(QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
    CHECK(scene.selectedNode() == std::optional<std::string>{"other"});
    REQUIRE(scene.dragging());
    send(QEvent::MouseMove, press + QPoint{20, 20}, Qt::NoButton, Qt::LeftButton);
    (void)ux12aWait([] { return false; }, 40);
    const QPoint grabbed = port->mapToGlobal(view->mapFromScene(
        other->mapToScene(QPointF{60, 15})));
    CHECK((grabbed - global_press - QPoint{20, 20}).manhattanLength() <= 2);
    // The old editors stay intact while the canvas owns the gesture.
    CHECK(tab.field<QDoubleSpinBox>("pelican.graphCanvasParameters.size")->value() == 16);
    send(QEvent::MouseButtonRelease, press + QPoint{20, 20}, Qt::LeftButton, Qt::NoButton);
    REQUIRE(ux12aWait([&] {
        return tab.field<QDoubleSpinBox>("pelican.graphCanvasParameters.size")->value() == 32;
    }));
}

TEST_CASE("Saving node positions does not repolish the graph notice on each move",
          "[devstudio][graph-canvas][scene][input-latency]") {
    (void)application();
    Ux2Tab tab{false};
    tab.read(ux12aBufferDocument(Json{{"element", "vec4"}, {"size", 16}}), "notice-0");
    auto &scene = tab.widget->scene();
    scene.setLayoutMember("frame_plan", Json{{"position", 0}});
    auto *notice = tab.field<QLabel>("pelican.graphCanvasNotice");
    REQUIRE_FALSE(notice->text().isEmpty());
    class StyleCounter : public QObject {
    public:
        int changes = 0;
        bool eventFilter(QObject *, QEvent *event) override {
            if (event->type() == QEvent::StyleChange) ++changes;
            return false;
        }
    } counter;
    notice->installEventFilter(&counter);
    for (int i = 1; i <= 10; ++i) scene.setLayoutMember("frame_plan", Json{{"position", i}});
    CHECK(counter.changes == 0);
}

TEST_CASE("Graph placement ignores empty serialization and preserves other panel members",
          "[devstudio][graph-canvas][scene][input-latency][layout-persistence]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    auto document = connectedDocument();
    document.erase("layout");
    model.setConnected(true);
    answer(model, document);
    scene.setModel(&model);
    // An automatic placement is not an authored layout. Its absent nodes,
    // reroutes and ties are equivalent to empty serialized collections.
    REQUIRE(scene.followDocumentPlacement());
    document["layout"] = {{"frame_plan", {{"nodes", {{"pass", {{"x", 50}, {"y", 75}}}}}}}};
    model.refresh();
    answer(model, document);
    REQUIRE(scene.followDocumentPlacement());
    CHECK(scene.layoutMember("frame_plan") == document["layout"]["frame_plan"]);
    document["layout"]["nodes"]["left"] = {{"x", 100}, {"y", 200}};
    model.refresh();
    answer(model, document);
    CHECK_FALSE(scene.followDocumentPlacement());
}

TEST_CASE("RT-D D1 the Graph tab refuses a later render entry from another panel",
          "[devstudio][graph-canvas][scene][rtd]") {
    RtdTab tab;
    tab.renderEdit();
    const auto own_serial = tab.history.undoStack().back().serial;
    const auto own_content = tab.document.dump();
    tab.renderEdit(false);
    REQUIRE(tab.history.undoStack().back().serial != own_serial);
    const auto before = tab.history_requests.size();
    tab.widget->scene().undoEdit();
    CHECK(tab.history_requests.size() == before);
    CHECK(tab.widget->model().notice().message ==
          "A later render edit from another panel is on top. Undo it there first, or refresh.");
    if (tab.history_requests.size() != before) return;
    REQUIRE(tab.history.undo());
    CHECK(tab.finishRestore()[0].at("content") == own_content);
    REQUIRE(tab.history.undoStack().back().serial == own_serial);
    tab.widget->scene().undoEdit();
    REQUIRE(tab.history.busy());
    const auto restored = tab.finishRestore();
    CHECK(Json::parse(restored[0].at("content").get<std::string>())
              .at("rendering").at("present").at("input").empty());
}

TEST_CASE("RT-D D2 a refused render Undo leaves the tab on the same entry",
          "[devstudio][graph-canvas][scene][rtd]") {
    RtdTab tab;
    tab.graphLabel();
    tab.renderEdit();
    const auto serial = tab.history.undoStack().back().serial;
    SECTION("asynchronous CAS refusal") {
        tab.widget->scene().undoEdit();
        REQUIRE(tab.history.busy());
        tab.finishRestore(true);
    }
    SECTION("synchronous refusal while a render edit is in flight") {
        tab.history.beforeRequest("edit_preset_pass", {{"pass_name", "pending"}});
        tab.history.requestSent(90001, "edit_preset_pass", {});
        tab.widget->scene().undoEdit();
        REQUIRE_FALSE(tab.history.busy());
        REQUIRE(tab.history.receiveResult(90001, {{"status", "rejected"},
                    {"error", {{"code", "gate_closed"}}}}));
    }
    REQUIRE(tab.history.undoStack().back().serial == serial);
    const auto graph_before = tab.transport.countOf(QStringLiteral("edit_graph_document"));
    const auto reads_before = tab.historyCount("get_render_documents");
    tab.widget->scene().undoEdit();
    CHECK(tab.transport.countOf(QStringLiteral("edit_graph_document")) == graph_before);
    CHECK(tab.historyCount("get_render_documents") == reads_before + 1);
    if (!tab.history.busy()) return;
    tab.finishRestore();
    CHECK(tab.history.redoStack().back().serial == serial);
}

TEST_CASE("RT-D D3 a detail-column Graph edit is undone before the tab render edit",
          "[devstudio][graph-canvas][scene][rtd]") {
    RtdTab tab;
    tab.renderEdit();
    const auto before_detail = tab.document;
    tab.widget->scene().selectNode("shade");
    QCoreApplication::processEvents();
    auto *fragment = tab.widget->findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasPass.shader.fragment"));
    REQUIRE(fragment != nullptr);
    REQUIRE(fragment->isVisible());
    const auto index = fragment->findData(QStringLiteral("engine://fullscreen"));
    REQUIRE(index >= 0);
    fragment->setCurrentIndex(index);
    QCoreApplication::processEvents();
    tab.graphCommit();
    const auto before = tab.transport.countOf(QStringLiteral("edit_graph_document"));
    const auto render_before = tab.history_requests.size();
    tab.widget->scene().undoEdit();
    CHECK(tab.transport.countOf(QStringLiteral("edit_graph_document")) == before + 1);
    CHECK(tab.history_requests.size() == render_before);
    if (tab.transport.countOf(QStringLiteral("edit_graph_document")) != before + 1) return;
    // Graph inverses restore the whole previous node (including its
    // absent fields), not a second forward set_node_parameters operation.
    CHECK(tab.tabEdit()[0].at("op") == "replace_node");
    CHECK(tab.tabEdit()[0].at("node").at("id") == "shade");
    CHECK(Pelican::editGraphDocument(tab.document, tab.tabEdit()) == before_detail);
    tab.graphCommit();
    CHECK(tab.document == before_detail);
    tab.widget->scene().undoEdit();
    REQUIRE(tab.history.busy());
    tab.finishRestore();
    CHECK(tab.document["rendering"]["present"]["input"].empty());
    tab.widget->scene().redoEdit();
    REQUIRE(tab.history.busy());
    tab.finishRestore();
    CHECK(tab.document == before_detail);
    tab.widget->scene().redoEdit();
    tab.graphCommit();
    CHECK(tab.document["nodes"][1]["parameters"]["shader"]["fragment"] == "engine://fullscreen");
    tab.widget->scene().undoEdit();
    tab.graphCommit();
    CHECK(tab.document == before_detail);
}

TEST_CASE("RT-D D4 disabled render Undo does not fall through to an older Graph edit",
          "[devstudio][graph-canvas][scene][rtd]") {
    RtdTab tab;
    tab.graphLabel();
    tab.renderEdit();
    REQUIRE(tab.widget->model().canUndo());
    tab.render_undo.setEnabled(false);
    const auto graph_before = tab.transport.countOf(QStringLiteral("edit_graph_document"));
    const auto render_before = tab.history_requests.size();
    CHECK_FALSE(tab.widget->scene().canUndoAny());
    tab.widget->scene().undoEdit();
    CHECK(tab.transport.countOf(QStringLiteral("edit_graph_document")) == graph_before);
    CHECK(tab.history_requests.size() == render_before);
    tab.render_undo.setEnabled(true);
    tab.widget->scene().undoEdit();
    REQUIRE(tab.history.busy());
    tab.finishRestore();
}

TEST_CASE("RT-D D5 reload and project change remove the tab render records",
          "[devstudio][graph-canvas][scene][rtd]") {
    RtdTab tab;
    tab.renderEdit();
    SECTION("external reload") {
        tab.history.cut("Reloaded from disk.");
        tab.widget->documentsReloadedFromDisk();
        CHECK_FALSE(tab.history.canUndo());
    }
    SECTION("project switch") {
        tab.history.clear();
        tab.widget->setProjectRoot(std::filesystem::temp_directory_path() / "rtd-other-project");
        CHECK_FALSE(tab.history.canUndo());
    }
    tab.readTab();
    tab.renderEdit(false);
    const auto before = tab.history_requests.size();
    tab.widget->scene().undoEdit();
    CHECK(tab.history_requests.size() == before);
    CHECK_FALSE(tab.widget->scene().canUndoAny());
}

TEST_CASE("RT-D Graph history invalidation after an attachment digest removes only Graph records",
          "[devstudio][graph-canvas][scene][rtd]") {
    RtdTab tab;
    tab.graphLabel();
    tab.renderEdit();
    // A newer Graph entry covers the render record in the tab.
    REQUIRE(tab.widget->scene().beginLabelEdit("shade"));
    tab.widget->scene().setInlineEditText(QStringLiteral("Newer Graph edit"));
    REQUIRE(tab.widget->scene().commitInlineEdit());
    tab.graphCommit();
    const auto render_serial = tab.history.undoStack().back().serial;
    auto hub = std::make_shared<PelicanStudio::RenderConfigDigest>();
    tab.widget->setRenderConfigDigest(hub);
    // Attachment's future hub connection reaches the same subscriber as
    // another panel: a different graph in the new root invalidates inverses.
    tab.document["nodes"].push_back({{"id", "attached"}, {"op", "value.constant"},
                                     {"parameters", {{"value", 3.0}}},
                                     {"outputs", Json::array({"value"})}});
    ++tab.generation;
    hub->publish(tab.digest());
    tab.readTab();
    REQUIRE_FALSE(tab.widget->model().canUndo());
    REQUIRE(tab.history.undoStack().back().serial == render_serial);
    const auto reads = tab.historyCount("get_render_documents");
    tab.widget->scene().undoEdit();
    CHECK(tab.historyCount("get_render_documents") == reads + 1);
    if (!tab.history.busy()) return;
    // The render entry remains guarded by its own CAS after invalidation.
    const auto docs = tab.finishRestore(true);
    CHECK(docs[0].at("base_digest") != tab.digest());
    CHECK(tab.history.undoStack().back().serial == render_serial);
}

TEST_CASE("RT-D engine restart retains the tab render record",
          "[devstudio][graph-canvas][scene][rtd]") {
    RtdTab tab;
    tab.renderEdit();
    const auto serial = tab.history.undoStack().back().serial;
    tab.history.abandonInFlight();
    tab.ready_changed(false, {});
    tab.ready_changed(true, {});
    tab.readTab();
    REQUIRE(tab.history.undoStack().back().serial == serial);
    tab.widget->scene().undoEdit();
    REQUIRE(tab.history.busy());
    tab.finishRestore();
    CHECK(tab.history.redoStack().back().serial == serial);
}

TEST_CASE("RT-D D4 disabled render Redo does not fall through to an older Graph redo",
          "[devstudio][graph-canvas][scene][rtd]") {
    RtdTab tab;
    tab.renderEdit();
    tab.graphLabel();
    tab.widget->scene().undoEdit();
    tab.graphCommit();
    REQUIRE(tab.widget->model().canRedo());
    tab.widget->scene().undoEdit();
    REQUIRE(tab.history.busy());
    tab.finishRestore();
    REQUIRE(tab.widget->model().canRedo());
    tab.render_redo.setEnabled(false);
    const auto graph_before = tab.transport.countOf(QStringLiteral("edit_graph_document"));
    const auto render_before = tab.history_requests.size();
    CHECK_FALSE(tab.widget->scene().canRedoAny());
    tab.widget->scene().redoEdit();
    CHECK(tab.transport.countOf(QStringLiteral("edit_graph_document")) == graph_before);
    CHECK(tab.history_requests.size() == render_before);
    tab.render_redo.setEnabled(true);
    tab.widget->scene().redoEdit();
    REQUIRE(tab.history.busy());
    tab.finishRestore();
}

TEST_CASE("RT-D a lost Graph submission drops Graph records and retains render records",
          "[devstudio][graph-canvas][scene][rtd]") {
    RtdTab tab;
    tab.renderEdit();
    tab.graphLabel();
    const auto serial = tab.history.undoStack().back().serial;
    REQUIRE(tab.widget->scene().beginLabelEdit("shade"));
    tab.widget->scene().setInlineEditText(QStringLiteral("Uncertain save"));
    REQUIRE(tab.widget->scene().commitInlineEdit());
    const auto id = tab.transport.idOf(QStringLiteral("edit_graph_document"));
    REQUIRE(id != 0);
    tab.failure(id, QStringLiteral("lost submission reply"));
    tab.readTab();
    REQUIRE_FALSE(tab.widget->model().canUndo());
    REQUIRE(tab.history.undoStack().back().serial == serial);
    const auto reads = tab.historyCount("get_render_documents");
    tab.widget->scene().undoEdit();
    CHECK(tab.historyCount("get_render_documents") == reads + 1);
    if (!tab.history.busy()) return;
    tab.finishRestore(true);
    CHECK(tab.history.undoStack().back().serial == serial);
}

TEST_CASE("RT-D Redo refuses another panel top and resumes after that panel restores it",
          "[devstudio][graph-canvas][scene][rtd]") {
    RtdTab tab;
    tab.renderEdit(false);
    const auto foreign = tab.history.undoStack().back().serial;
    tab.renderEdit();
    const auto own = tab.history.undoStack().back().serial;
    const auto content = tab.document;
    tab.widget->scene().undoEdit();
    REQUIRE(tab.history.busy());
    tab.finishRestore();
    REQUIRE(tab.history.undo());
    tab.finishRestore();
    REQUIRE(tab.history.redoStack().back().serial == foreign);
    const auto before = tab.history_requests.size();
    tab.widget->scene().redoEdit();
    CHECK(tab.history_requests.size() == before);
    CHECK(tab.widget->model().notice().message ==
          "A later render edit from another panel is on top. Redo it there first, or refresh.");
    REQUIRE(tab.history.redo());
    tab.finishRestore();
    REQUIRE(tab.history.redoStack().back().serial == own);
    tab.widget->scene().redoEdit();
    REQUIRE(tab.history.busy());
    tab.finishRestore();
    CHECK(tab.document == content);
    CHECK(tab.history.undoStack().back().serial == own);
}
