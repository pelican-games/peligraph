#include "graphcanvaswidget.hpp"
#include "graphcanvasdrawing.hpp"
#include "graphcanvasloopfields.hpp"
#include "graphcanvasrenderfields.hpp"
#include "fullscreenshadertemplate.hpp"

#include "inlineediteventfilter.hpp"
#include "jsonrpcrequest.hpp"
#include "panelhelp.hpp"
#include "renderconfigdigest.hpp"
#include "rendereditcompletion.hpp"

#include "nodelayout.hpp"
#include "studiotheme.hpp"

#include <QAbstractItemDelegate>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QAction>
#include <QBrush>
#include <QByteArray>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFormLayout>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QListWidget>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPolygonF>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

QString qtext(const std::string &value) {
    return QString::fromStdString(value);
}

using GraphCanvasDrawing::numberText;
using GraphCanvasDrawing::portSummary;

// Everything that belongs to a node's body: a press anywhere on it selects
// the node, the way a press on the node rectangle used to.
bool isNodeSurface(const QString &kind) {
    return kind == QLatin1String{GraphCanvasNodeItem} ||
           kind == QLatin1String{GraphCanvasNodeLabelItem} ||
           kind == QLatin1String{GraphCanvasNodeHeaderItem} ||
           kind == QLatin1String{GraphCanvasCollapseItem} ||
           kind == QLatin1String{GraphCanvasSocketItem} ||
           kind == QLatin1String{GraphCanvasValuePillItem} ||
           kind == QLatin1String{GraphCanvasValueBandItem} ||
           // LP-4d1: the inside of a zone, its marks and its body outputs'
           // inside ends belong to the loop.
           kind == QLatin1String{GraphCanvasZoneItem} ||
           kind == QLatin1String{GraphCanvasZoneLabelItem} ||
           kind == QLatin1String{GraphCanvasZoneSocketItem};
}

std::string itemScope(const QGraphicsItem &item) {
    return item.data(GraphCanvasScopeRole).toString().toStdString();
}

using InlineEditFilter = InlineEditEventFilter;

// What kind of editor one stored value asks for. The current value is the only
// evidence there is: no schema says what a node's parameter is allowed to be.
// UX-2, decision 4: except where the model has a list to pick from (a
// render.image's format and type, a render pass's fragment shader) -- that is
// a combo, `choice`, never a line to type in.
enum class ValueEditorKind { number, boolean, text, json, choice };

ValueEditorKind editorKindFor(const nlohmann::json &value) {
    if (value.is_boolean()) return ValueEditorKind::boolean;
    if (value.is_number()) return ValueEditorKind::number;
    if (value.is_string()) return ValueEditorKind::text;
    return ValueEditorKind::json;
}

QGraphicsItem *findSocket(QGraphicsScene &scene, const QString &scope,
                          const QString &node, const QString &port,
                          const QString &direction) {
    for (QGraphicsItem *item : scene.items()) {
        if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasSocketItem} ||
            item->data(GraphCanvasScopeRole).toString() != scope ||
            item->data(NodeGraphNameRole).toString() != node ||
            item->data(GraphCanvasSocketDirectionRole).toString() != direction)
            continue;
        // Q-19: a socket of the shader's declarations is named by its key;
        // the numbered port an edge names is the one bound there.
        if (item->data(GraphCanvasPortRole).toString() != port &&
            (port.isEmpty() || item->data(GraphCanvasBoundPortRole).toString() != port))
            continue;
        return item;
    }
    return nullptr;
}

// A folded node has no socket rows, so its edges gather on one point per side,
// drawn at the middle of the heading band. That anchor carries the empty port
// name: it stands for every port at once, which is exactly what folding says.
QGraphicsItem *findSocketOrAnchor(QGraphicsScene &scene, const QString &scope,
                                  const QString &node, const QString &port,
                                  const QString &direction) {
    if (QGraphicsItem *exact = findSocket(scene, scope, node, port, direction))
        return exact;
    return findSocket(scene, scope, node, QString{}, direction);
}

// An edge runs socket centre to socket centre. Only a port the canvas could
// not draw a socket for falls back to the node box, so a missing contract
// degrades the line rather than losing it.
void routeGraphCanvasEdge(QGraphicsScene &scene, QGraphicsPathItem &edge) {
    const QString scope = edge.data(GraphCanvasScopeRole).toString();
    const QString from_name = edge.data(NodeGraphFromNameRole).toString();
    const QString to_name = edge.data(NodeGraphToNameRole).toString();
    QGraphicsItem *from = findNodeItem(scene, graphCanvasKinds(), scope, from_name);
    QGraphicsItem *to = findNodeItem(scene, graphCanvasKinds(), scope, to_name);

    QGraphicsItem *from_socket =
        findSocketOrAnchor(scene, scope, from_name,
                           edge.data(GraphCanvasFromPortRole).toString(),
                           QStringLiteral("output"));
    QGraphicsItem *to_socket =
        findSocketOrAnchor(scene, scope, to_name,
                           edge.data(GraphCanvasToPortRole).toString(),
                           QStringLiteral("input"));
    // LP-4d1: a body's boundary input is drawn as a socket on its zone's
    // edge, not as a node; the socket is enough to draw the line from.
    if ((from == nullptr && from_socket == nullptr) || (to == nullptr && to_socket == nullptr))
        return;

    const QPointF start = from_socket != nullptr
                              ? from_socket->sceneBoundingRect().center()
                              : QPointF{from->sceneBoundingRect().right(),
                                        from->sceneBoundingRect().center().y()};
    const QPointF tip = to_socket != nullptr
                            ? to_socket->sceneBoundingRect().center()
                            : QPointF{to->sceneBoundingRect().left(),
                                      to->sceneBoundingRect().center().y()};
    // The bends are in the scope's own coordinates, which a zone's body does
    // not share with the scene; the line is computed in the scene and handed
    // back to the item in its own.
    QVector<QPointF> bends = nodeGraphBends(edge);
    if (const QGraphicsItem *parent = edge.parentItem())
        for (QPointF &bend : bends) bend = parent->mapToScene(bend);
    const CurveGeometry geometry =
        !bends.isEmpty() ? curveGeometryThrough(start, bends, tip)
        : (from_socket != nullptr && to_socket != nullptr) || from == nullptr || to == nullptr
            ? curveGeometryBetween(start, tip)
            : curveGeometry(from->sceneBoundingRect(), to->sceneBoundingRect());
    edge.setPath(edge.mapFromScene(geometry.path));
    if (auto *arrow = dynamic_cast<QGraphicsPolygonItem *>(findEndpointArtifact(
            scene, graphCanvasKinds().arrow, scope, from_name, to_name)))
        arrow->setPolygon(arrow->mapFromScene(geometry.arrow));
    // LP-4d2, decision 4: the element a dashed line reads sits over its
    // middle, and goes where the line goes.
    if (edge.data(GraphCanvasEdgeElementRole).toString().isEmpty()) return;
    const QString to_port = edge.data(GraphCanvasToPortRole).toString();
    for (QGraphicsItem *item : scene.items()) {
        if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasEdgeLabelItem} ||
            item->data(GraphCanvasScopeRole).toString() != scope ||
            item->data(NodeGraphNameRole).toString() != to_name ||
            item->data(GraphCanvasPortRole).toString() != to_port)
            continue;
        const QPointF middle = geometry.path.pointAtPercent(0.5);
        const QRectF box = item->boundingRect();
        const QPointF at{middle.x() - box.width() / 2.0, middle.y() - box.height() - 2.0};
        item->setPos(item->parentItem() != nullptr ? item->parentItem()->mapFromScene(at) : at);
        break;
    }
}

// LP-4d1: a body output's line, from the port that makes it to the inside
// end of that output on the zone's right edge.
void routeZoneLink(QGraphicsScene &scene, QGraphicsPathItem &link) {
    const QString scope = link.data(GraphCanvasScopeRole).toString();
    QGraphicsItem *from = findSocketOrAnchor(scene, scope,
                                             link.data(NodeGraphFromNameRole).toString(),
                                             link.data(GraphCanvasFromPortRole).toString(),
                                             QStringLiteral("output"));
    QGraphicsItem *to = nullptr;
    for (QGraphicsItem *item : scene.items())
        if (nodeGraphItemKind(*item) == QLatin1String{GraphCanvasZoneSocketItem} &&
            item->data(NodeGraphNameRole) == link.data(NodeGraphNameRole) &&
            item->data(GraphCanvasPortRole) == link.data(GraphCanvasPortRole)) {
            to = item;
            break;
        }
    if (from == nullptr || to == nullptr) return;
    const CurveGeometry geometry = curveGeometryBetween(from->sceneBoundingRect().center(),
                                                        to->sceneBoundingRect().center());
    link.setPath(link.mapFromScene(geometry.path));
}

// GL-1, decision 2: a line the frame plan implies, from the row of the
// resource it names on a folded node (or the heading of a graph node) to the
// row that reads it. Drawing only: nothing about it is kept or sent.
void routeDerivedEdge(QGraphicsScene &scene, QGraphicsPathItem &line) {
    const QString scope = line.data(GraphCanvasScopeRole).toString();
    const QString resource = line.data(GraphCanvasFromPortRole).toString();
    const auto end = [&](const QString &name, bool output) -> std::optional<QPointF> {
        QGraphicsItem *node = nullptr;
        for (QGraphicsItem *item : scene.items()) {
            if (item->data(NodeGraphNameRole).toString() != name ||
                item->data(GraphCanvasScopeRole).toString() != scope ||
                !item->data(GraphCanvasZoneScopeRole).toString().isEmpty())
                continue;
            const QString kind = nodeGraphItemKind(*item);
            if (kind == QLatin1String{GraphCanvasDerivedSocketItem} &&
                item->data(GraphCanvasPortRole).toString() == resource &&
                item->data(GraphCanvasSocketDirectionRole).toString() ==
                    (output ? QLatin1String{"output"} : QLatin1String{"input"}))
                return item->sceneBoundingRect().center();
            if (kind == QLatin1String{GraphCanvasNodeItem}) node = item;
        }
        if (node == nullptr) return std::nullopt;
        const QRectF box = node->sceneBoundingRect();
        return QPointF{output ? box.right() : box.left(), box.top() + GraphCanvasHeaderHeight / 2.0};
    };
    const auto start = end(line.data(NodeGraphFromNameRole).toString(), true);
    const auto tip = end(line.data(NodeGraphToNameRole).toString(), false);
    if (!start || !tip) return;
    line.setPath(line.mapFromScene(curveGeometryBetween(*start, *tip).path));
}

// GL-1e, decision 2: one item of a rendering-section pass's `input`, from the
// output it names -- a graph node's socket, or the row of a folded node -- to
// the pass's socket for that item.
void routePassInputEdge(QGraphicsScene &scene, QGraphicsPathItem &line) {
    const QString scope = line.data(GraphCanvasScopeRole).toString();
    const QString from = line.data(NodeGraphFromNameRole).toString();
    const QString from_port = line.data(GraphCanvasFromPortRole).toString();
    const QString pass = line.data(NodeGraphToNameRole).toString();
    const auto index = line.data(GraphCanvasPassInputRole).toLongLong();
    QGraphicsItem *start = nullptr;
    QGraphicsItem *from_node = nullptr;
    QGraphicsItem *end = nullptr;
    for (QGraphicsItem *item : scene.items()) {
        if (item->data(GraphCanvasScopeRole).toString() != scope ||
            !item->data(GraphCanvasZoneScopeRole).toString().isEmpty())
            continue;
        const QString kind = nodeGraphItemKind(*item);
        const QString name = item->data(NodeGraphNameRole).toString();
        const bool output = item->data(GraphCanvasSocketDirectionRole).toString() ==
                            QLatin1String{"output"};
        if (name == from) {
            if ((kind == QLatin1String{GraphCanvasSocketItem} ||
                 kind == QLatin1String{GraphCanvasDerivedSocketItem}) &&
                output && !from_port.isEmpty() &&
                item->data(GraphCanvasPortRole).toString() == from_port)
                start = item;
            else if (kind == QLatin1String{GraphCanvasNodeItem})
                from_node = item;
        }
        if (name == pass && kind == QLatin1String{GraphCanvasDerivedSocketItem} && !output &&
            item->data(GraphCanvasPassInputRole).isValid() &&
            item->data(GraphCanvasPassInputRole).toLongLong() == index)
            end = item;
    }
    if (end == nullptr || (start == nullptr && from_node == nullptr)) return;
    const QPointF tip = end->sceneBoundingRect().center();
    const QPointF origin = start != nullptr
                               ? start->sceneBoundingRect().center()
                               : QPointF{from_node->sceneBoundingRect().right(),
                                         from_node->sceneBoundingRect().top() +
                                             GraphCanvasHeaderHeight / 2.0};
    line.setPath(line.mapFromScene(curveGeometryBetween(origin, tip).path));
}

// GL-1b, decision 1: a folded node's key in the one placement. It cannot be a
// graph node's id, so the two never share a box.
std::string foldedLayoutKey(const std::string &id) { return std::string{"\x01" "folded|"} + id; }
bool isFoldedLayoutKey(const std::string &key) { return key.starts_with('\x01'); }

void rerouteDerivedEdges(QGraphicsScene &scene, const std::string &node) {
    const QString name = qtext(node);
    for (QGraphicsItem *item : scene.items()) {
        const QString kind = nodeGraphItemKind(*item);
        const bool pass_input = kind == QLatin1String{GraphCanvasPassInputEdgeItem};
        if ((kind != QLatin1String{GraphCanvasDerivedEdgeItem} && !pass_input) ||
            (item->data(NodeGraphFromNameRole).toString() != name &&
             item->data(NodeGraphToNameRole).toString() != name))
            continue;
        auto *line = dynamic_cast<QGraphicsPathItem *>(item);
        if (line == nullptr) continue;
        // GL-1e: a pass's own line is routed to its socket.
        if (pass_input) routePassInputEdge(scene, *line);
        else routeDerivedEdge(scene, *line);
    }
}

void rerouteGraphCanvasEdges(QGraphicsScene &scene, const std::string &scope,
                             const std::string &node) {
    // GL-1: the lines the plan implies follow a root node too.
    if (scope.empty()) rerouteDerivedEdges(scene, node);
    const QString expected_scope = qtext(scope);
    const QString expected_node = qtext(node);
    for (QGraphicsItem *item : scene.items()) {
        if (nodeGraphItemKind(*item) == QLatin1String{GraphCanvasZoneLinkItem} &&
            item->data(GraphCanvasScopeRole).toString() == expected_scope &&
            item->data(NodeGraphFromNameRole).toString() == expected_node) {
            if (auto *link = dynamic_cast<QGraphicsPathItem *>(item)) routeZoneLink(scene, *link);
            continue;
        }
        if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasEdgeItem} ||
            item->data(GraphCanvasScopeRole).toString() != expected_scope ||
            (item->data(NodeGraphFromNameRole).toString() != expected_node &&
             item->data(NodeGraphToNameRole).toString() != expected_node))
            continue;
        if (auto *edge = dynamic_cast<QGraphicsPathItem *>(item)) {
            // N-6: the bends are not dropped. A control point is an absolute
            // place the person put it, so moving one end of the edge is the
            // case it exists for -- the line still goes through it. (N-4 threw
            // them away here, because the lane belonged to an automatic
            // placement the drag had just invalidated; that is the mutation
            // control for "a reroute survives a node drag".)
            routeGraphCanvasEdge(scene, *edge);
        }
    }
}

} // namespace

namespace {
// One point of the placement, as the document spells it: a pair, because a
// control point is a place and not a named thing.
std::optional<QPointF> layoutPoint(const nlohmann::json &value) {
    if (!value.is_array() || value.size() != 2) return std::nullopt;
    if (!value.at(0).is_number() || !value.at(1).is_number()) return std::nullopt;
    return QPointF{value.at(0).get<double>(), value.at(1).get<double>()};
}

std::optional<GraphCanvasEdgeKey> layoutEdgeKey(const nlohmann::json &value) {
    if (!value.is_object()) return std::nullopt;
    const auto node = value.value("node", std::string{});
    const auto port = value.value("port", std::string{});
    if (node.empty() || port.empty()) return std::nullopt;
    return GraphCanvasEdgeKey{node, port};
}
} // namespace

GraphCanvasLayout graphCanvasLayoutFromJson(const nlohmann::json &layout) {
    GraphCanvasLayout result;
    if (!layout.is_object()) return result;
    if (const auto nodes = layout.find("nodes");
        nodes != layout.end() && nodes->is_object())
        for (const auto &[id, place] : nodes->items()) {
            if (!place.is_object()) continue;
            const auto x = place.find("x");
            const auto y = place.find("y");
            if (x == place.end() || y == place.end() || !x->is_number() ||
                !y->is_number())
                continue;
            result.nodes.emplace(id, QPointF{x->get<double>(), y->get<double>()});
        }
    if (const auto reroutes = layout.find("reroutes");
        reroutes != layout.end() && reroutes->is_array())
        for (const auto &entry : *reroutes) {
            if (!entry.is_object()) continue;
            const auto key = layoutEdgeKey(entry.value("edge", nlohmann::json{}));
            if (!key) continue;
            GraphCanvasReroute reroute{*key, {}};
            for (const auto &point : entry.value("points", nlohmann::json::array()))
                if (const auto at = layoutPoint(point)) reroute.points.push_back(*at);
            if (reroute.points.empty()) continue;
            // One entry per edge: the index in a name is an index into the
            // edge's own control points, so two entries for one edge would
            // make `graph_reroute#node/port/0` mean two things.
            const auto found = std::ranges::find(result.reroutes, reroute.edge,
                                                 &GraphCanvasReroute::edge);
            if (found == result.reroutes.end()) {
                result.reroutes.push_back(std::move(reroute));
            } else {
                found->points.insert(found->points.end(), reroute.points.begin(),
                                     reroute.points.end());
            }
        }
    if (const auto ties = layout.find("ties");
        ties != layout.end() && ties->is_array())
        for (const auto &entry : *ties) {
            if (!entry.is_object()) continue;
            const auto x = entry.find("x");
            const auto y = entry.find("y");
            if (x == entry.end() || y == entry.end() || !x->is_number() ||
                !y->is_number())
                continue;
            GraphCanvasTie tie{QPointF{x->get<double>(), y->get<double>()}, {}};
            for (const auto &edge : entry.value("edges", nlohmann::json::array()))
                if (const auto key = layoutEdgeKey(edge)) tie.edges.push_back(*key);
            // A bundle of one is not a bundle.
            if (tie.edges.size() < 2) continue;
            result.ties.push_back(std::move(tie));
        }
    return result;
}

nlohmann::json graphCanvasLayoutToJson(const GraphCanvasLayout &layout) {
    nlohmann::json result = nlohmann::json::object();
    auto nodes = nlohmann::json::object();
    for (const auto &[id, position] : layout.nodes)
        nodes[id] = {{"x", position.x()}, {"y", position.y()}};
    result["nodes"] = std::move(nodes);
    auto reroutes = nlohmann::json::array();
    for (const GraphCanvasReroute &reroute : layout.reroutes) {
        if (reroute.points.empty()) continue;
        auto points = nlohmann::json::array();
        for (const QPointF &point : reroute.points)
            points.push_back(nlohmann::json::array({point.x(), point.y()}));
        reroutes.push_back({{"edge", {{"node", reroute.edge.node},
                                      {"port", reroute.edge.port}}},
                            {"points", std::move(points)}});
    }
    result["reroutes"] = std::move(reroutes);
    auto ties = nlohmann::json::array();
    for (const GraphCanvasTie &tie : layout.ties) {
        if (tie.edges.size() < 2) continue;
        auto edges = nlohmann::json::array();
        for (const GraphCanvasEdgeKey &edge : tie.edges)
            edges.push_back({{"node", edge.node}, {"port", edge.port}});
        ties.push_back({{"x", tie.position.x()}, {"y", tie.position.y()},
                        {"edges", std::move(edges)}});
    }
    result["ties"] = std::move(ties);
    return result;
}

const NodeGraphKinds &graphCanvasKinds() {
    static const NodeGraphKinds kinds{
        QStringList{QString::fromLatin1(GraphCanvasNodeItem)},
        QString::fromLatin1(GraphCanvasEdgeItem),
        QString::fromLatin1(GraphCanvasEdgeArrowItem),
        // The edge labels are gone: the socket rows carry the port names, so
        // a box in the middle of every line said the same thing twice.
        QString{}};
    return kinds;
}

SocketTypeFacts graphCanvasSocketFacts(const GraphCanvasModel &model,
                                       const std::string &type) {
    SocketTypeFacts facts;
    facts.type = type;
    const auto *schema = type.empty() ? nullptr : model.typeSchema(type);
    if (schema == nullptr) return facts;
    const auto display = schema->find("display");
    if (display != schema->end() && display->is_object()) {
        facts.has_display = true;
        facts.display_shape = display->value("shape", std::string{});
        facts.display_color = display->value("color", std::string{});
        // A display object that names neither is not a declaration.
        if (facts.display_shape.empty() && facts.display_color.empty())
            facts.has_display = false;
    }
    const auto traits = schema->find("traits");
    if (traits != schema->end() && traits->is_array())
        for (const auto &trait : *traits)
            if (trait.is_string()) facts.traits.push_back(trait.get<std::string>());
    return facts;
}

GraphCanvasScene::GraphCanvasScene(QObject *parent) : QGraphicsScene{parent} {
    // Every item holds the colour it was built with, so following a theme is
    // drawing the scope again -- not walking the items and guessing which of
    // their brushes came from the table.
    theme_listener_ = addStudioThemeListener([this] { repopulate(); });
    // One drag is fifty mouse moves and one edit. The wait is what makes that
    // true; `flushLayout` is the same act without it, for a caller that has
    // already finished the gesture.
    layout_timer_ = new QTimer(this);
    layout_timer_->setSingleShot(true);
    layout_timer_->setInterval(GraphCanvasLayoutDebounceMs);
    QObject::connect(layout_timer_, &QTimer::timeout, this,
                     [this] { flushLayout(); });
}

void GraphCanvasScene::setModel(GraphCanvasModel *model) {
    model_ = model;
    session_node_positions_.clear();
    folded_places_.clear();
    session_collapsed_.clear();
    pruned_revision_.reset();
    selected_node_.reset();
    selected_edge_.reset();
    selected_edges_.clear();
    selection_scope_.clear();
    clearControlSelection();
    placements_.clear();
    selected_pass_input_.reset();
    pass_authoring_.reset();
    repopulate();
}

void GraphCanvasScene::pruneSessionPositions() {
    if (model_ == nullptr) return;
    // A refresh that left every node in place must not move anything, so the
    // pruning only runs when the document itself changed.
    if (pruned_revision_ && *pruned_revision_ == model_->contentRevision()) return;
    pruned_revision_ = model_->contentRevision();
    std::set<std::pair<std::string, std::string>> present;
    for (auto &key : model_->allNodeKeys()) present.insert(std::move(key));
    // GL-1b: a folded node's place is not a document node's; it stays.
    const auto folded_prefix = foldedPositionKey({}).first;
    std::erase_if(session_node_positions_, [&](const auto &entry) {
        return entry.first.first != folded_prefix && !present.contains(entry.first);
    });
    std::erase_if(session_collapsed_, [&](const auto &entry) {
        return !present.contains(entry.first);
    });
}

bool GraphCanvasScene::collapsed(const std::string &id) const {
    if (model_ == nullptr) return false;
    return collapsed(model_->currentScope(), id);
}

bool GraphCanvasScene::collapsed(const std::string &scope, const std::string &id) const {
    const auto found = session_collapsed_.find({scope, id});
    return found != session_collapsed_.end() && found->second;
}

void GraphCanvasScene::setCollapsed(const std::string &id, bool folded) {
    if (model_ == nullptr) return;
    setCollapsed(model_->currentScope(), id, folded);
}

void GraphCanvasScene::setCollapsed(const std::string &scope, const std::string &id,
                                    bool folded) {
    if (model_ == nullptr) return;
    session_collapsed_[{scope, id}] = folded;
    repopulate();
}

void GraphCanvasScene::toggleCollapsed(const std::string &id) {
    setCollapsed(id, !collapsed(id));
}

bool GraphCanvasScene::showLoopBranch(const std::string &loop, std::size_t branch) {
    if (model_ == nullptr || !model_->showLoopBranch(loop, branch)) return false;
    repopulate();
    return true;
}

void GraphCanvasScene::autoLayout() {
    if (model_ == nullptr) return;
    const std::string scope = model_->currentScope();
    // GL-1b: the folded nodes of the view on screen are placed again too.
    const auto folded_prefix = foldedPositionKey({});
    std::erase_if(session_node_positions_, [&](const auto &entry) {
        return entry.first.first == scope ||
               (entry.first.first == folded_prefix.first &&
                entry.first.second.starts_with(folded_prefix.second));
    });
    std::erase_if(folded_places_, [&](const auto &entry) {
        return entry.first.second.starts_with(folded_prefix.second);
    });
    std::erase_if(added_folded_places_, [&](const auto &entry) {
        return entry.first.second.starts_with(folded_prefix.second);
    });
    // Start from nothing: the placement is the whole answer, so a leftover
    // control point from the last one would be a bend nobody asked for.
    {
        auto &placed = placement(scope);
        placed.layout = GraphCanvasLayout{};
        placed.dirty = false;
    }
    clearControlSelection();
    // One draw to get the placement, which is where `placement_*` comes from.
    repopulate();
    // GL-1b, decision 1: the folded nodes' places in it, for the session.
    keepFoldedPlaces();

    GraphCanvasLayout placed;
    placed.nodes = placement_boxes_;
    // Every lane the placement kept, as (column, edge, y). A lane is two
    // points: the edge runs level across the column it flies over, and only
    // the gaps between columns slope.
    struct Lane {
        qreal left = 0.0;
        qreal right = 0.0;
        qreal y = 0.0;
        GraphCanvasEdgeKey edge;
    };
    std::vector<Lane> lanes;
    for (const auto &[edge, points] : placement_bends_)
        for (std::size_t index = 0; index + 1 < points.size(); index += 2)
            lanes.push_back(Lane{points[index].x(), points[index + 1].x(),
                                 points[index].y(), edge});
    std::ranges::stable_sort(lanes, [](const Lane &a, const Lane &b) {
        return a.left != b.left ? a.left < b.left : a.y < b.y;
    });
    // Lanes of the same column that run within a node's pitch of each other
    // are one bundle: that is what "these lines go the same way" looks like in
    // numbers. A lane with nobody beside it stays a control point of its own.
    for (std::size_t start = 0; start < lanes.size();) {
        std::size_t end = start + 1;
        while (end < lanes.size() && lanes[end].left == lanes[start].left &&
               lanes[end].y - lanes[end - 1].y <= NodeGraphVerticalGap)
            ++end;
        if (end - start >= 2) {
            GraphCanvasTie tie;
            tie.position = QPointF{(lanes[start].left + lanes[start].right) / 2.0,
                                   lanes[start].y};
            for (std::size_t index = start; index < end; ++index)
                tie.edges.push_back(lanes[index].edge);
            placed.ties.push_back(std::move(tie));
        } else {
            const Lane &lane = lanes[start];
            const auto found = std::ranges::find(placed.reroutes, lane.edge,
                                                 &GraphCanvasReroute::edge);
            GraphCanvasReroute &entry =
                found != placed.reroutes.end()
                    ? *found
                    : placed.reroutes.emplace_back(GraphCanvasReroute{lane.edge, {}});
            entry.points.push_back(QPointF{lane.left, lane.y});
            entry.points.push_back(QPointF{lane.right, lane.y});
        }
        start = end;
    }
    placement(scope).layout = std::move(placed);
    noteLayoutChange(scope);
    repopulate();
    // "Auto layout" is one deliberate act, not the tail of a drag: it is sent
    // at once rather than after the wait a drag needs.
    flushLayout();
}

// GL-1b: a folded node's place is session state, per view (GL-1).
std::optional<QRectF> GraphCanvasScene::fitFocus() {
    const auto box = [&](const std::string &scope, const std::string &id) -> std::optional<QRectF> {
        if (QGraphicsItem *item = findNodeItem(*this, graphCanvasKinds(), qtext(scope), qtext(id)))
            return item->sceneBoundingRect();
        return std::nullopt;
    };
    if (selected_node_)
        if (const auto picked = box(selection_scope_, *selected_node_)) return picked;
    if (last_added_)
        if (const auto added = box(last_added_->first, last_added_->second)) return added;
    return std::nullopt;
}

std::pair<std::string, std::string> GraphCanvasScene::foldedPositionKey(const std::string &id) const {
    return {std::string{"\x01" "derived"}, (model_ != nullptr ? model_->viewKey() : std::string{}) + "|" + id};
}

void GraphCanvasScene::holdRootPlaces(const std::string &root) {
    const QString scope = qtext(root);
    for (QGraphicsItem *item : items()) {
        if (item->parentItem() != nullptr ||
            nodeGraphItemKind(*item) != QLatin1String{GraphCanvasNodeItem} ||
            item->data(GraphCanvasDerivedRole).toBool() ||
            item->data(GraphCanvasScopeRole).toString() != scope)
            continue;
        session_node_positions_.try_emplace(
            {root, item->data(NodeGraphNameRole).toString().toStdString()}, item->pos());
    }
}

void GraphCanvasScene::keepFoldedPlaces() {
    for (QGraphicsItem *item : items()) {
        if (!item->data(GraphCanvasDerivedRole).toBool() ||
            nodeGraphItemKind(*item) != QLatin1String{GraphCanvasNodeItem})
            continue;
        folded_places_.insert_or_assign(
            foldedPositionKey(item->data(NodeGraphNameRole).toString().toStdString()), item->pos());
    }
}

void GraphCanvasScene::setMenuActions(QAction *auto_layout, QAction *fit_view,
                                      QAction *tie, QAction *untie) {
    auto_layout_action_ = auto_layout;
    fit_view_action_ = fit_view;
    tie_action_ = tie;
    untie_action_ = untie;
}

// --- N-6: the placement.

GraphCanvasScene::ScopePlacement &GraphCanvasScene::placement(const std::string &scope) {
    return placements_[scope];
}

const GraphCanvasLayout &GraphCanvasScene::layout() const noexcept {
    static const std::string root;
    return layout(model_ != nullptr ? model_->currentScope() : root);
}

const GraphCanvasLayout &GraphCanvasScene::layout(const std::string &scope) const noexcept {
    static const GraphCanvasLayout empty;
    const auto found = placements_.find(scope);
    return found == placements_.end() ? empty : found->second.layout;
}

bool GraphCanvasScene::layoutPending() const noexcept {
    return std::ranges::any_of(placements_,
                               [](const auto &entry) { return entry.second.dirty; });
}

bool GraphCanvasScene::placementUnsent() const noexcept {
    return std::ranges::any_of(placements_, [](const auto &entry) {
        return entry.second.dirty && entry.second.sent.is_null();
    });
}

void GraphCanvasScene::loadLayout(const std::string &scope) {
    if (model_ == nullptr) return;
    auto &placed = placement(scope);
    const auto document = model_->scopeLayout(scope);
    if (placed.dirty) {
        // The document is the one that is behind, not the canvas.
        if (document != placed.sent) return;
        placed.dirty = false;
    }
    placed.layout = graphCanvasLayoutFromJson(document);
    placed.loaded = true;
    // Everything in the document's `layout` this canvas has no opinion about.
    // It travels back out on the next write; dropping it would make a node drag
    // here erase the Frame Plan's placement over there.
    placed.foreign = nlohmann::json::object();
    if (document.is_object())
        for (const auto &[key, value] : document.items())
            if (key != "nodes" && key != "reroutes" && key != "ties")
                placed.foreign[key] = value;
}

nlohmann::json GraphCanvasScene::layoutMember(const std::string &key) const {
    if (model_ == nullptr) return nlohmann::json::object();
    const auto found_scope = placements_.find(model_->currentScope());
    if (found_scope == placements_.end()) return nlohmann::json::object();
    const auto &foreign = found_scope->second.foreign;
    const auto found = foreign.find(key);
    return found == foreign.end() ? nlohmann::json::object() : *found;
}

void GraphCanvasScene::setLayoutMember(const std::string &key,
                                       nlohmann::json value) {
    if (model_ == nullptr) return;
    const std::string scope = model_->currentScope();
    auto &foreign = placement(scope).foreign;
    const bool drop = value.is_null() ||
                      (value.is_object() && value.empty());
    if (drop) {
        if (foreign.find(key) == foreign.end()) return;
        foreign.erase(key);
    } else {
        const auto found = foreign.find(key);
        if (found != foreign.end() && *found == value) return;
        foreign[key] = std::move(value);
    }
    noteLayoutChange(scope);
}

void GraphCanvasScene::pruneLayout(const std::string &scope) {
    if (model_ == nullptr) return;
    auto &layout = placement(scope).layout;
    const auto before = graphCanvasLayoutToJson(layout);
    std::set<std::string, std::less<>> present;
    for (const auto &node : model_->nodes(scope)) present.insert(node.id);
    std::set<GraphCanvasEdgeKey> drawn;
    for (const auto &edge : model_->edges(scope))
        drawn.insert(GraphCanvasEdgeKey{edge.to_node, edge.to_port});
    std::erase_if(layout.nodes, [&](const auto &entry) {
        return !present.contains(entry.first);
    });
    std::erase_if(layout.reroutes, [&](const GraphCanvasReroute &entry) {
        return entry.points.empty() || !drawn.contains(entry.edge);
    });
    for (GraphCanvasTie &tie : layout.ties)
        std::erase_if(tie.edges, [&](const GraphCanvasEdgeKey &key) {
            return !drawn.contains(key);
        });
    // A bundle that lost all but one line is not a bundle any more; the line
    // that is left keeps its place as a control point of its own.
    for (GraphCanvasTie &tie : layout.ties) {
        if (tie.edges.size() != 1) continue;
        GraphCanvasReroute &entry = rerouteFor(scope, tie.edges.front());
        entry.points.push_back(tie.position);
        std::ranges::stable_sort(entry.points, {}, &QPointF::x);
        tie.edges.clear();
    }
    std::erase_if(layout.ties, [](const GraphCanvasTie &tie) {
        return tie.edges.empty();
    });
    // A placement that named something the document no longer holds would be
    // refused the next time it is written, so the tidying is itself a change.
    if (graphCanvasLayoutToJson(layout) != before) noteLayoutChange(scope);
}

void GraphCanvasScene::noteLayoutChange(const std::string &scope) {
    // GL-1: a folded view is Studio's drawing; where its nodes sit is not
    // kept anywhere.
    if (model_ != nullptr && model_->foldedView()) return;
    auto &placed = placement(scope);
    placed.dirty = true;
    // The canvas is now ahead of anything that has gone out. Forgetting what
    // was last sent is what keeps the next rebuild from deciding the document
    // has caught up and putting the older placement back over this change.
    placed.sent = nlohmann::json{};
    if (layout_timer_ != nullptr)
        layout_timer_->start(GraphCanvasLayoutDebounceMs);
    if (placementChanged) placementChanged();
}

nlohmann::json GraphCanvasScene::placementPayload(const std::string &scope) const {
    const auto found = placements_.find(scope);
    if (found == placements_.end()) return graphCanvasLayoutToJson(GraphCanvasLayout{});
    auto payload = graphCanvasLayoutToJson(found->second.layout);
    // The other panel's members ride along: one `set_layout` replaces the whole
    // object, so leaving them out here is the same as deleting them.
    for (const auto &[key, value] : found->second.foreign.items())
        payload[key] = value;
    return payload;
}

bool GraphCanvasScene::followDocumentPlacement() {
    if (model_ == nullptr) return false;
    const std::string scope = model_->currentScope();
    if (drawn_scopes_.empty() || drawn_scopes_.front() != scope) return false;
    // A placement the canvas sent for a scope it no longer draws has caught
    // up once the document says what was sent.
    for (auto &[placed_scope, placed] : placements_)
        if (placed.dirty && !placed.sent.is_null() &&
            std::ranges::find(drawn_scopes_, placed_scope) == drawn_scopes_.end() &&
            model_->scopeLayout(placed_scope) == placed.sent)
            placed.dirty = false;
    bool follows = true;
    for (const std::string &drawn : drawn_scopes_) {
        const auto found = placements_.find(drawn);
        if (found == placements_.end()) return false;
        auto &placed = found->second;
        const auto document = model_->scopeLayout(drawn);
        if (placed.dirty) {
            // The canvas is ahead of the document, and what it draws is its
            // own placement whichever way this read went -- the same rule
            // loadLayout applies. When the document now says exactly what was
            // sent, the canvas has caught up and has nothing left in the air.
            if (document == placed.sent) placed.dirty = false;
            continue;
        }
        // Nothing of this canvas's is in the air, so the document is the one
        // to follow; it only needs no redraw when it already says what is
        // drawn.
        if (document != placementPayload(drawn)) follows = false;
    }
    return follows;
}

void GraphCanvasScene::flushLayout() {
    if (model_ == nullptr) return;
    // A scope the canvas has left goes first: it is the gesture that was
    // finished before the canvas moved on.
    std::vector<std::pair<const std::string, ScopePlacement> *> order;
    for (auto &entry : placements_)
        if (std::ranges::find(drawn_scopes_, entry.first) == drawn_scopes_.end())
            order.push_back(&entry);
    for (auto &entry : placements_)
        if (std::ranges::find(drawn_scopes_, entry.first) != drawn_scopes_.end())
            order.push_back(&entry);
    for (auto *entry : order) {
        const std::string &scope = entry->first;
        ScopePlacement &placed = entry->second;
        if (!placed.dirty) continue;
        // A definition the document no longer has has nothing to be placed.
        if (!scope.empty() &&
            std::ranges::none_of(model_->scopes(), [&](const GraphCanvasScope &known) {
                return known.id == scope;
            })) {
            placed.dirty = false;
            continue;
        }
        auto payload = placementPayload(scope);
        // Already on its way: nothing changed since it was sent.
        if (!placed.sent.is_null() && placed.sent == payload) continue;
        // Busy with the last edit, or no digest to hold the document still
        // yet. The change is kept rather than dropped: losing it would lose
        // the drag.
        if (!model_->canEdit() || !model_->setLayout(scope, payload)) {
            if (layout_timer_ != nullptr)
                layout_timer_->start(GraphCanvasLayoutDebounceMs);
            return;
        }
        placed.sent = std::move(payload);
        // One edit at a time: a second scope's placement goes when this one
        // has landed.
        if (layout_timer_ != nullptr &&
            std::ranges::any_of(placements_, [&](const auto &entry) {
                return entry.second.dirty && entry.second.sent.is_null();
            }))
            layout_timer_->start(GraphCanvasLayoutDebounceMs);
        // Sending from inside a rebuild would post a request while the scene
        // is half-built; the tab's own tick drains the outbox a moment later
        // anyway.
        if (!rebuilding_) afterEdit(true);
        return;
    }
}

QVector<QPointF> GraphCanvasScene::bendsFor(const std::string &scope,
                                            const GraphCanvasEdge &edge) const {
    const GraphCanvasEdgeKey key{edge.to_node, edge.to_port};
    const GraphCanvasLayout &placed = layout(scope);
    std::vector<QPointF> points;
    for (const GraphCanvasReroute &reroute : placed.reroutes) {
        if (!(reroute.edge == key)) continue;
        points.insert(points.end(), reroute.points.begin(), reroute.points.end());
    }
    for (const GraphCanvasTie &tie : placed.ties) {
        const auto slot = std::ranges::find(tie.edges, key);
        if (slot == tie.edges.end()) continue;
        const qreal y =
            tie.position.y() +
            static_cast<qreal>(std::distance(tie.edges.begin(), slot)) *
                GraphCanvasTieSpacing;
        // Two points, so the line runs level across the band: that is what
        // makes a bundle read as parallel rather than as a shared corner.
        points.push_back(QPointF{tie.position.x() - GraphCanvasTieReach, y});
        points.push_back(QPointF{tie.position.x() + GraphCanvasTieReach, y});
    }
    std::ranges::stable_sort(points, {}, &QPointF::x);
    return QVector<QPointF>(points.begin(), points.end());
}

void GraphCanvasScene::applyEdgeBends() {
    for (QGraphicsItem *item : items()) {
        if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasEdgeItem}) continue;
        auto *line = dynamic_cast<QGraphicsPathItem *>(item);
        if (line == nullptr) continue;
        const GraphCanvasEdge edge{
            item->data(NodeGraphFromNameRole).toString().toStdString(),
            item->data(GraphCanvasFromPortRole).toString().toStdString(),
            item->data(NodeGraphToNameRole).toString().toStdString(),
            item->data(GraphCanvasToPortRole).toString().toStdString()};
        setNodeGraphBends(*line, bendsFor(itemScope(*item), edge));
        routeGraphCanvasEdge(*this, *line);
    }
}

GraphCanvasReroute &GraphCanvasScene::rerouteFor(const std::string &scope,
                                                 const GraphCanvasEdgeKey &edge) {
    auto &reroutes = placement(scope).layout.reroutes;
    const auto found = std::ranges::find(reroutes, edge, &GraphCanvasReroute::edge);
    if (found != reroutes.end()) return *found;
    reroutes.push_back(GraphCanvasReroute{edge, {}});
    return reroutes.back();
}

void GraphCanvasScene::detachFromTies(const std::string &scope,
                                      const GraphCanvasEdgeKey &edge) {
    auto &ties = placement(scope).layout.ties;
    for (GraphCanvasTie &tie : ties)
        std::erase(tie.edges, edge);
    std::erase_if(ties, [](const GraphCanvasTie &tie) {
        return tie.edges.size() < 2;
    });
}

void GraphCanvasScene::moveReroute(const std::string &scope, const GraphCanvasEdgeKey &edge,
                                   std::size_t index, const QPointF &at) {
    auto &reroutes = placement(scope).layout.reroutes;
    const auto found = std::ranges::find(reroutes, edge, &GraphCanvasReroute::edge);
    if (found == reroutes.end() || index >= found->points.size()) return;
    found->points[index] = at;
    noteLayoutChange(scope);
    applyEdgeBends();
    scheduleSceneRectUpdate();
}

void GraphCanvasScene::moveTie(const std::string &scope, std::size_t index,
                               const QPointF &at) {
    auto &ties = placement(scope).layout.ties;
    if (index >= ties.size()) return;
    ties[index].position =
        at + QPointF{GraphCanvasTieWidth / 2.0, GraphCanvasTiePadding};
    noteLayoutChange(scope);
    applyEdgeBends();
    scheduleSceneRectUpdate();
}

void GraphCanvasScene::clearControlSelection() {
    selected_reroutes_.clear();
    selected_ties_.clear();
}

void GraphCanvasScene::enterSelectionScope(const std::string &scope) {
    if (scope == selection_scope_) return;
    selected_node_.reset();
    selected_edge_.reset();
    selected_edges_.clear();
    clearControlSelection();
    selection_scope_ = scope;
}

void GraphCanvasScene::selectReroute(const GraphCanvasEdgeKey &edge,
                                     std::size_t index, bool additive) {
    selectReroute(model_ != nullptr ? model_->currentScope() : std::string{}, edge,
                  index, additive);
}

void GraphCanvasScene::selectReroute(const std::string &scope, const GraphCanvasEdgeKey &edge,
                                     std::size_t index, bool additive) {
    selected_carry_.reset();
    if (scope != selection_scope_) additive = false;
    enterSelectionScope(scope);
    selected_ties_.clear();
    if (!additive) {
        selected_reroutes_.clear();
        selected_edges_.clear();
        selected_edge_.reset();
        selected_node_.reset();
    }
    const auto entry = std::pair{edge, index};
    const auto found = std::ranges::find(selected_reroutes_, entry);
    if (found != selected_reroutes_.end()) {
        // Holding the modifier on something already picked lets it go.
        if (additive) selected_reroutes_.erase(found);
    } else {
        selected_reroutes_.push_back(entry);
    }
    applyEdgeEmphasis();
    if (selectionChanged) selectionChanged();
}

void GraphCanvasScene::selectTie(std::size_t index, bool additive) {
    selectTie(model_ != nullptr ? model_->currentScope() : std::string{}, index, additive);
}

void GraphCanvasScene::selectTie(const std::string &scope, std::size_t index, bool additive) {
    selected_carry_.reset();
    if (scope != selection_scope_) additive = false;
    enterSelectionScope(scope);
    selected_reroutes_.clear();
    if (!additive) {
        selected_ties_.clear();
        selected_edges_.clear();
        selected_edge_.reset();
        selected_node_.reset();
    }
    const auto found = std::ranges::find(selected_ties_, index);
    if (found != selected_ties_.end()) {
        if (additive) selected_ties_.erase(found);
    } else {
        selected_ties_.push_back(index);
    }
    applyEdgeEmphasis();
    if (selectionChanged) selectionChanged();
}

bool GraphCanvasScene::insertReroute(const GraphCanvasEdge &edge,
                                     const QPointF &at) {
    if (model_ == nullptr) return false;
    return insertReroute(model_->currentScope(), edge, at);
}

bool GraphCanvasScene::insertReroute(const std::string &scope, const GraphCanvasEdge &edge,
                                     const QPointF &at) {
    if (model_ == nullptr) return false;
    const GraphCanvasEdgeKey key{edge.to_node, edge.to_port};
    const auto drawn = model_->edges(scope);
    if (std::ranges::none_of(drawn, [&](const GraphCanvasEdge &candidate) {
            return candidate.to_node == key.node && candidate.to_port == key.port;
        }))
        return false;
    GraphCanvasReroute &entry = rerouteFor(scope, key);
    entry.points.push_back(toScopePoint(scope, at));
    // The order along the line is the order the drawing meets them, and on a
    // canvas that runs left to right that is the order of x.
    std::ranges::stable_sort(entry.points, {}, &QPointF::x);
    noteLayoutChange(scope);
    repopulate();
    return true;
}

bool GraphCanvasScene::tieSelection() {
    if (model_ == nullptr) return false;
    const std::string scope = selection_scope_;
    const QString scope_text = qtext(scope);
    // Where each picked line runs now. A reroute says so exactly; an edge
    // picked without one is taken at the middle of the curve it is drawn as
    // -- in the item's own coordinates, which are the scope's.
    const auto midpoint = [this, &scope_text](const GraphCanvasEdge &edge) {
        for (QGraphicsItem *item : items()) {
            if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasEdgeItem} ||
                item->data(GraphCanvasScopeRole).toString() != scope_text ||
                item->data(NodeGraphFromNameRole).toString() != qtext(edge.from_node) ||
                item->data(GraphCanvasFromPortRole).toString() != qtext(edge.from_port) ||
                item->data(NodeGraphToNameRole).toString() != qtext(edge.to_node) ||
                item->data(GraphCanvasToPortRole).toString() != qtext(edge.to_port))
                continue;
            if (auto *line = dynamic_cast<QGraphicsPathItem *>(item))
                return std::optional<QPointF>{line->path().pointAtPercent(0.5)};
        }
        return std::optional<QPointF>{};
    };
    struct Pick {
        GraphCanvasEdgeKey key;
        QPointF at;
        bool from_reroute = false;
        std::size_t index = 0;
    };
    auto &layout = placement(scope).layout;
    std::vector<Pick> picks;
    const auto has = [&](const GraphCanvasEdgeKey &key) {
        return std::ranges::any_of(
            picks, [&](const Pick &pick) { return pick.key == key; });
    };
    for (const auto &[key, index] : selected_reroutes_) {
        if (has(key)) continue;
        const auto found =
            std::ranges::find(layout.reroutes, key, &GraphCanvasReroute::edge);
        if (found == layout.reroutes.end() || index >= found->points.size()) continue;
        picks.push_back(Pick{key, found->points[index], true, index});
    }
    for (const GraphCanvasEdge &edge : selected_edges_) {
        const GraphCanvasEdgeKey key{edge.to_node, edge.to_port};
        if (has(key)) continue;
        const auto at = midpoint(edge);
        if (!at) continue;
        picks.push_back(Pick{key, *at, false, 0});
    }
    // A bundle of one is not a bundle.
    if (picks.size() < 2) return false;
    std::ranges::stable_sort(
        picks, [](const Pick &a, const Pick &b) { return a.at.y() < b.at.y(); });
    qreal x = 0.0;
    for (const Pick &pick : picks) x += pick.at.x();
    x /= static_cast<qreal>(picks.size());

    // The picked control points are the bundle's now. Highest index first, so
    // the earlier ones keep the place the later erase would have moved.
    std::vector<std::pair<GraphCanvasEdgeKey, std::size_t>> taken;
    for (const Pick &pick : picks)
        if (pick.from_reroute) taken.emplace_back(pick.key, pick.index);
    std::ranges::sort(taken, [](const auto &a, const auto &b) {
        return a.second > b.second;
    });
    for (const auto &[key, index] : taken) {
        const auto found =
            std::ranges::find(layout.reroutes, key, &GraphCanvasReroute::edge);
        if (found == layout.reroutes.end() || index >= found->points.size()) continue;
        found->points.erase(found->points.begin() +
                            static_cast<std::ptrdiff_t>(index));
    }
    std::erase_if(layout.reroutes, [](const GraphCanvasReroute &entry) {
        return entry.points.empty();
    });

    GraphCanvasTie tie;
    tie.position = QPointF{x, picks.front().at.y()};
    for (const Pick &pick : picks) {
        // One line belongs to at most one bundle.
        detachFromTies(scope, pick.key);
        tie.edges.push_back(pick.key);
    }
    layout.ties.push_back(std::move(tie));
    clearControlSelection();
    selected_edges_.clear();
    selected_edge_.reset();
    noteLayoutChange(scope);
    repopulate();
    return true;
}

bool GraphCanvasScene::untieSelection() {
    if (selected_ties_.empty()) return false;
    const std::string scope = selection_scope_;
    auto &ties = placement(scope).layout.ties;
    auto indices = selected_ties_;
    std::ranges::sort(indices, std::greater<>{});
    bool changed = false;
    for (const std::size_t index : indices) {
        if (index >= ties.size()) continue;
        const GraphCanvasTie tie = ties[index];
        ties.erase(ties.begin() + static_cast<std::ptrdiff_t>(index));
        for (std::size_t slot = 0; slot < tie.edges.size(); ++slot) {
            GraphCanvasReroute &entry = rerouteFor(scope, tie.edges[slot]);
            entry.points.push_back(
                QPointF{tie.position.x(),
                        tie.position.y() +
                            static_cast<qreal>(slot) * GraphCanvasTieSpacing});
            std::ranges::stable_sort(entry.points, {}, &QPointF::x);
        }
        changed = true;
    }
    if (!changed) return false;
    clearControlSelection();
    noteLayoutChange(scope);
    repopulate();
    return true;
}

bool GraphCanvasScene::deleteControlSelection() {
    const std::string scope = selection_scope_;
    auto &layout = placement(scope).layout;
    bool changed = false;
    if (!selected_ties_.empty()) {
        auto indices = selected_ties_;
        std::ranges::sort(indices, std::greater<>{});
        for (const std::size_t index : indices) {
            if (index >= layout.ties.size()) continue;
            layout.ties.erase(layout.ties.begin() +
                              static_cast<std::ptrdiff_t>(index));
            changed = true;
        }
    }
    if (!selected_reroutes_.empty()) {
        auto picks = selected_reroutes_;
        std::ranges::sort(picks, [](const auto &a, const auto &b) {
            return a.second > b.second;
        });
        for (const auto &[key, index] : picks) {
            const auto found =
                std::ranges::find(layout.reroutes, key, &GraphCanvasReroute::edge);
            if (found == layout.reroutes.end() || index >= found->points.size())
                continue;
            found->points.erase(found->points.begin() +
                                static_cast<std::ptrdiff_t>(index));
            changed = true;
        }
        std::erase_if(layout.reroutes, [](const GraphCanvasReroute &entry) {
            return entry.points.empty();
        });
    }
    if (!changed) return false;
    clearControlSelection();
    noteLayoutChange(scope);
    repopulate();
    return true;
}

namespace {

// LP-4d1: what a zone's tab says a branch tests.
QString zoneCondition(const GraphCanvasLoopBranch &branch) {
    const auto integers = GraphCanvasLoopFields::integersText(branch.values);
    if (branch.when == "element") return QObject::tr("element = %1").arg(integers);
    if (branch.when == "element_in") return QObject::tr("element in %1").arg(integers);
    if (branch.when == "element_not_in") return QObject::tr("element not in %1").arg(integers);
    if (branch.when == "draw") {
        QStringList parts;
        const auto &when = branch.draw_when;
        if (when.material) parts.push_back(qtext(*when.material));
        if (when.tags) {
            QStringList tags;
            for (const auto &tag : when.tags->include) tags.push_back(qtext(tag));
            for (const auto &tag : when.tags->require_all) tags.push_back(qtext(tag));
            for (const auto &tag : when.tags->exclude) tags.push_back(QStringLiteral("!") + qtext(tag));
            parts.push_back(QObject::tr("tags %1").arg(tags.join(QStringLiteral(" "))));
        }
        if (when.contract) parts.push_back(qtext(*when.contract));
        if (when.skinned) parts.push_back(*when.skinned ? QObject::tr("skinned")
                                                        : QObject::tr("not skinned"));
        if (when.has_variant)
            parts.push_back(*when.has_variant ? QObject::tr("has variant")
                                              : QObject::tr("no variant"));
        return parts.join(QStringLiteral(" & "));
    }
    return QObject::tr("expression");
}

} // namespace

void GraphCanvasScene::repopulate() {
    ++rebuild_generation_;
    // An open editor belongs to items that are about to be deleted -- so what
    // it is and what it holds is taken down first, and opened again over the
    // items this draw produces. A refresh, a theme switch or an observation
    // reply arriving mid-edit is not a reason to throw away what somebody is
    // in the middle of typing.
    const auto carried = rebuilding_ ? std::nullopt : carryInlineEdit();
    rebuilding_ = true;
    closeEditor();
    clearPendingConnection();
    // A refresh is not a deselection: the node the user is working on stays
    // selected as long as the document still holds it (N-1's recorded hole).
    const auto previous_node = selected_node_;
    const auto previous_edge = selected_edge_;
    clear();
    zone_content_.clear();
    zone_content_rect_.clear();
    zone_geometry_.clear();
    zones_.clear();
    drawn_scopes_.clear();
    selected_node_.reset();
    selected_edge_.reset();
    setProperty("pelicanGraphScope", QVariant{});
    setProperty("pelicanGraphBreadcrumb", QVariant{});
    setProperty("pelicanGraphNodeCount", QVariant{});
    setProperty("pelicanGraphEdgeCount", QVariant{});
    setProperty("pelicanGraphNotice", QVariant{});
    setProperty("pelicanGraphZoneCount", QVariant{});
    if (model_ == nullptr) {
        rebuilding_ = false;
        return;
    }
    pruneSessionPositions();
    // A placement sent for a scope this draw may not hold has caught up once
    // the document says what was sent.
    for (auto &[placed_scope, placed] : placements_)
        if (placed.dirty && !placed.sent.is_null() &&
            model_->scopeLayout(placed_scope) == placed.sent)
            placed.dirty = false;

    const std::string scope = model_->currentScope();
    // N-6: what the document says about where this scope was drawn. It is read
    // before the nodes so the placement can answer for them; the session table
    // under it only speaks for a node the document has never placed.
    drawn_scopes_.push_back(scope);
    loadLayout(scope);
    pruneLayout(scope);
    const QString scope_text = qtext(scope);
    auto nodes = model_->nodes(scope);
    auto edges = model_->edges(scope);
    // GL-1, decision 5: the root draws the domains the filter holds; a line
    // to a node it hides is cut at the shown end (the stubs, below).
    if (scope.empty()) {
        std::set<std::string, std::less<>> hidden;
        for (const auto &node : nodes)
            if (!model_->nodeShown(scope, node)) hidden.insert(node.id);
        std::erase_if(nodes, [&](const GraphCanvasNode &node) { return hidden.contains(node.id); });
        std::erase_if(edges, [&](const GraphCanvasEdge &edge) {
            return hidden.contains(edge.from_node) || hidden.contains(edge.to_node);
        });
    }
    // Decision 1: and, left of them, what no graph node made, folded.
    auto folded_nodes = model_->foldedNodes();
    const auto derived_edges = model_->derivedEdges();
    // GL-1e, decisions 1 and 2: a pass of the rendering section takes lines.
    // Its sockets are its `input` (and `+ input` when its shader takes more),
    // and each item of it is a solid line from what it names -- in place of
    // the dashed one the plan implies for that resource.
    struct PassLine {
        std::string from;
        std::string from_port;
        std::string pass;
        std::string resource;
        std::size_t input_index = 0;
    };
    std::vector<PassLine> pass_lines;
    std::set<std::pair<std::string, std::string>> pass_line_resources;
    if (scope.empty() && !model_->foldedView()) {
        // The root node a resource is named by: its output hands it on.
        std::map<std::string, std::string, std::less<>> resource_nodes;
        for (const auto &node : nodes) {
            if (node.op == "render.image" || node.op == "render.buffer" ||
                node.op == "render.attribute")
                resource_nodes.emplace(node.id, node.id);
            else if (node.op == "render.target_ref")
                if (const auto name = node.parameters.find("name");
                    name != node.parameters.end() && name->is_string())
                    resource_nodes.emplace(name->get<std::string>(), node.id);
        }
        // The graph node and output that hand on the image a pass reads: of
        // the plan's passes that write it before that pass, the last whose
        // node has an output resolving to it.
        const auto writerOutput = [&](const std::string &reader, const std::string &resource)
            -> std::optional<std::pair<std::string, std::string>> {
            std::optional<std::pair<std::string, std::string>> found;
            for (const auto &pass : model_->planPasses()) {
                if (pass.name == reader) break;
                if (std::ranges::find(pass.writes, resource) == pass.writes.end()) continue;
                const auto owner = model_->canvasNodeOfPass(pass.name);
                const auto drawn = std::ranges::find(nodes, owner, &GraphCanvasNode::id);
                if (owner.empty() || drawn == nodes.end()) continue;
                for (const auto &output : drawn->outputs)
                    if (model_->outputResource(owner, output.port) == resource) {
                        found = std::pair{owner, output.port};
                        break;
                    }
            }
            return found;
        };
        // UX-5c: the root loop and output a pass's input names, by the
        // plan's loop_outputs (RS-13).
        const auto loopOutputOf = [&](const std::string &reader, const std::string &resource)
            -> std::optional<std::pair<std::string, std::string>> {
            const auto pass = std::ranges::find(model_->planPasses(), reader,
                                                &GraphCanvasPlanPass::name);
            if (pass == model_->planPasses().end()) return std::nullopt;
            for (const auto &output : pass->loop_outputs) {
                if (resource != output.reference && resource != output.resource) continue;
                const auto dot = output.reference.find('.');
                if (dot == std::string::npos) continue;
                const auto loop = output.reference.substr(0, dot);
                const auto port = output.reference.substr(dot + 1);
                const auto drawn = std::ranges::find(nodes, loop, &GraphCanvasNode::id);
                if (drawn == nodes.end() ||
                    std::ranges::none_of(drawn->outputs, [&](const auto &socket) {
                        return socket.port == port;
                    }))
                    continue;
                return std::pair{loop, port};
            }
            return std::nullopt;
        };
        for (auto &folded : folded_nodes) {
            if (folded.feature_group) continue;
            const auto sockets = passInputSockets(folded.id);
            if (!sockets) continue;
            folded.takes_lines = true;
            for (const auto &socket : sockets->sockets) {
                GraphCanvasFoldedNode::InputSocket row;
                row.key = socket.key();
                row.binding = socket.binding;
                row.resource = socket.resource;
                row.label = socket.label;
                if (socket.kind == FramePlanInputSocket::Kind::variadic_drop) {
                    row.port = "in:+";
                } else if (socket.connected()) {
                    row.port = socket.resource;
                    row.input_index = socket.input_index;
                } else {
                    row.port = row.key;
                }
                if (row.port.empty()) continue;
                folded.input_sockets.push_back(row);
                if (!row.input_index) continue;
                PassLine line{{}, {}, folded.id, socket.resource, *row.input_index};
                // UX-5c: a loop's output the pass names (`loop.color`, or
                // what the engine resolved it to) is a line from that output.
                // GL-1f, decision 3: else from the graph pass's output that
                // hands the image on (a material pass's `color`), when one
                // does -- the last such pass before this one in the plan.
                if (const auto loop_output = loopOutputOf(folded.id, socket.resource)) {
                    line.from = loop_output->first;
                    line.from_port = loop_output->second;
                } else if (const auto writer = writerOutput(folded.id, socket.resource)) {
                    line.from = writer->first;
                    line.from_port = writer->second;
                } else if (const auto named = resource_nodes.find(socket.resource);
                           named != resource_nodes.end()) {
                    line.from = named->second;
                    line.from_port = "resource";
                } else {
                    // What the plan says wrote it: a folded node's row, or a
                    // graph node's heading.
                    for (const auto &edge : derived_edges)
                        if (edge.to == folded.id &&
                            std::ranges::find(edge.resources, socket.resource) !=
                                edge.resources.end()) {
                            line.from = edge.from;
                            if (std::ranges::any_of(folded_nodes, [&](const auto &other) {
                                    return other.id == edge.from;
                                }))
                                line.from_port = socket.resource;
                            break;
                        }
                }
                if (line.from.empty()) continue;
                pass_line_resources.emplace(folded.id, socket.resource);
                pass_lines.push_back(std::move(line));
            }
        }
    }

    // Decision 3: the breadcrumb names the folded steps too, and a feature's
    // fold has its decomposed version's button under it.
    const auto display_trail = model_->displayTrail();
    const bool decomposable = model_->canShowDecomposed();
    const qreal crumb_top = display_trail.size() > 1 ? NodeGraphBreadcrumbHeight + 18.0 : 0.0;
    const qreal top = crumb_top + (decomposable ? NodeGraphBreadcrumbHeight : 0.0);
    GraphCanvasDrawing::Builder drawing{*this, scope_text};

    // LP-4d1: every root loop is drawn as a zone around the body its tab
    // shows. The zones are planned before the root is placed, because a
    // frame's size is its body's extent. A definition two loops run is drawn
    // once, in the first loop's zone; a folded zone is the loop's node.
    struct ZonePlan {
        std::string body;
        std::vector<GraphCanvasNode> nodes;
        std::vector<std::string> boundary;
        std::vector<GraphCanvasEdge> edges;
        std::vector<GraphCanvasDrawing::Node> drawn;
        std::map<std::string, QPointF> positions;
        std::vector<GraphCanvasBodyOutput> outputs;
        QRectF content;
    };
    std::map<std::string, ZonePlan> plans;
    std::set<std::string> folded_zones;
    if (scope.empty()) {
        std::set<std::string> claimed;
        for (const auto &node : nodes) {
            if (!node.loop) continue;
            const auto body = model_->loopZoneBody(node.id);
            if (body.empty() || claimed.contains(body)) continue;
            if (collapsed(scope, node.id)) {
                folded_zones.insert(node.id);
                continue;
            }
            claimed.insert(body);
            ZonePlan &plan = plans[node.id];
            plan.body = body;
            drawn_scopes_.push_back(body);
            loadLayout(body);
            pruneLayout(body);
            for (auto &inside : model_->nodes(body)) {
                // A boundary input is the zone's left edge, not a node.
                if (inside.op == "graph.input") plan.boundary.push_back(inside.id);
                else plan.nodes.push_back(std::move(inside));
            }
            plan.edges = model_->edges(body);
            plan.outputs = model_->bodyOutputs(body);
            plan.drawn.reserve(plan.nodes.size());
            for (const auto &inside : plan.nodes)
                plan.drawn.push_back(drawing.prepareNode(*model_, inside, collapsed(body, inside.id)));
            // Decision 6: a body node the document never placed is placed
            // inside the frame, as the root places its own.
            std::vector<GraphCanvasEdge> inner;
            for (const auto &edge : plan.edges)
                if (std::ranges::find(plan.boundary, edge.from_node) == plan.boundary.end())
                    inner.push_back(edge);
            const NodeGraphLayout automatic =
                GraphCanvasDrawing::placeNodes(plan.drawn, inner, 0.0);
            const GraphCanvasLayout &placed = this->layout(body);
            for (const auto &entry : plan.drawn) {
                const auto &id = entry.node->id;
                QPointF at;
                if (const auto found = placed.nodes.find(id); found != placed.nodes.end())
                    at = found->second;
                else if (const auto kept = session_node_positions_.find({body, id});
                         kept != session_node_positions_.end())
                    at = kept->second;
                else if (const auto box = automatic.boxes.find(id); box != automatic.boxes.end())
                    at = QPointF{box->second.x, box->second.y};
                plan.positions[id] = at;
                const QRectF box{at, QSizeF{entry.width, entry.height}};
                plan.content = plan.content.isValid() ? plan.content.united(box) : box;
            }
        }
    }

    // A gesture left in the air belongs to the scope it happened in: a scope
    // this draw no longer holds sends it now, before the canvas moves on.
    if (std::ranges::any_of(placements_, [&](const auto &entry) {
            return entry.second.dirty && entry.second.sent.is_null() &&
                   std::ranges::find(drawn_scopes_, entry.first) == drawn_scopes_.end();
        }))
        flushLayout();

    // Placement needs the final size of each node, including its socket rows
    // -- and of each zone, including its body.
    std::vector<GraphCanvasDrawing::Node> drawn;
    drawn.reserve(nodes.size());
    std::map<std::string, GraphCanvasDrawing::Zone> zones;
    for (const auto &node : nodes) {
        const bool zone_folded = folded_zones.contains(node.id);
        auto entry = drawing.prepareNode(*model_, node,
                                         !zone_folded && collapsed(scope, node.id));
        entry.zone_folded = zone_folded;
        if (const auto plan = plans.find(node.id); plan != plans.end()) {
            const auto loop = model_->loop(node.id);
            GraphCanvasDrawing::Zone zone;
            zone.node = entry;
            zone.definition = qtext(plan->second.body);
            if (loop && loop->reported) {
                zone.stage = loop->reported_tier == "draw_loop"
                                 ? QObject::tr("draw_loop %1 materials")
                                       .arg(static_cast<qulonglong>(
                                           loop->reported_total > 0
                                               ? loop->reported_total
                                               : static_cast<std::size_t>(std::max<std::int64_t>(
                                                     loop->reported_count, 0))))
                                 : QStringLiteral("%1 ×%2")
                                       .arg(qtext(loop->reported_tier))
                                       .arg(loop->reported_count);
            } else {
                zone.stage = QObject::tr("loop");
            }
            // LP-4f1, decision 3: a loop over a view axis says how many views
            // and along which axis -- `×6 (face)` -- and, with one branch,
            // names the views where the tabs would be.
            if (loop && !loop->view_family.empty()) {
                const auto count = loop->reported ? std::optional<std::int64_t>{loop->reported_count}
                                                  : loop->known_count;
                zone.stage = count ? QObject::tr("×%1 (%2)").arg(*count).arg(qtext(loop->view_axis))
                                   : QObject::tr("loop (%1)").arg(qtext(loop->view_axis));
                if (loop->branches.size() <= 1 && loop->known_count && *loop->known_count > 0 &&
                    *loop->known_count <= 32)
                    for (std::int64_t element = 0; element < *loop->known_count; ++element) {
                        const auto &labels = loop->view_axis_info ? loop->view_axis_info->labels
                                                                  : std::vector<std::string>{};
                        zone.axis_labels.push_back(
                            static_cast<std::size_t>(element) < labels.size()
                                ? qtext(labels[static_cast<std::size_t>(element)])
                                : QString::number(element));
                    }
            }
            const auto uses = model_->definitionUseCount(plan->second.body);
            if (uses > 1)
                zone.shared = QObject::tr("shared (%1 users)").arg(static_cast<qulonglong>(uses));
            const bool draw_loop_report =
                loop && loop->reported && loop->reported_tier == "draw_loop";
            if (loop && loop->branches.size() > 1) {
                const std::size_t shown = model_->loopBranch(node.id);
                const auto defined = model_->scopes();
                for (std::size_t index = 0; index < loop->branches.size(); ++index) {
                    const auto &branch = loop->branches[index];
                    const bool last = index + 1 == loop->branches.size();
                    GraphCanvasDrawing::ZoneTab tab;
                    tab.text = last ? QObject::tr("else")
                                    : (index == 0 ? QObject::tr("if %1") : QObject::tr("elif %1"))
                                          .arg(zoneCondition(branch));
                    // LP-4d2, decision 5: a draw_loop's tab says how many of
                    // the materials it drew.
                    if (draw_loop_report)
                        tab.text += QStringLiteral(" (%1)").arg(
                            static_cast<qulonglong>(branch.matched_materials.size()));
                    tab.branch = index;
                    tab.runs = !branch.nothing && !branch.body.empty();
                    tab.current = tab.runs && index == shown;
                    // UX-1, decision 4: a body another branch runs too is
                    // said on the tab (the else's first), and a body the
                    // document does not define is why a press does nothing.
                    if (tab.runs) {
                        for (std::size_t other = loop->branches.size(); other-- > 0;) {
                            const auto &sibling = loop->branches[other];
                            if (other == index || sibling.nothing || sibling.body != branch.body)
                                continue;
                            tab.shared_with = other + 1 == loop->branches.size()
                                                  ? QObject::tr("else")
                                                  : QObject::tr("branch %1").arg(
                                                        static_cast<qulonglong>(other));
                            break;
                        }
                        if (std::ranges::none_of(defined, [&](const GraphCanvasScope &known) {
                                return !known.root && known.id == branch.body;
                            }))
                            tab.reason = QObject::tr("This branch runs %1, which the document "
                                                     "does not define")
                                             .arg(qtext(branch.body));
                    }
                    zone.tabs.push_back(std::move(tab));
                }
            }
            // The left edge: the loop's inputs and the body's boundary, by
            // name; the right edge: the body's outputs and the p / p[] the
            // loop declares from them.
            const auto outer_input = [&](const std::string &port) {
                return std::ranges::find(entry.inputs, qtext(port), &GraphCanvasDrawing::Row::port);
            };
            std::vector<std::string> input_names;
            for (const auto &port : node.inputs) input_names.push_back(port.port);
            for (const auto &id : plan->second.boundary)
                if (std::ranges::find(input_names, id) == input_names.end()) input_names.push_back(id);
            for (const auto &name : input_names) {
                GraphCanvasDrawing::ZoneRow row;
                row.label = qtext(name);
                if (const auto outer = outer_input(name); outer != entry.inputs.end()) {
                    row.outer = qtext(name);
                    row.outer_row = *outer;
                }
                if (std::ranges::find(plan->second.boundary, name) != plan->second.boundary.end())
                    row.inner = qtext(name);
                zone.inputs.push_back(std::move(row));
            }
            const auto outer_output = [&](const std::string &port) {
                return std::ranges::find(entry.outputs, qtext(port), &GraphCanvasDrawing::Row::port);
            };
            std::set<std::string> placed_outputs;
            for (const auto &output : plan->second.outputs) {
                GraphCanvasDrawing::ZoneRow row;
                row.label = qtext(output.name);
                row.inner = qtext(output.name);
                if (const auto outer = outer_output(output.name); outer != entry.outputs.end()) {
                    row.outer = qtext(output.name);
                    row.outer_row = *outer;
                    placed_outputs.insert(output.name);
                }
                zone.outputs.push_back(std::move(row));
                const auto each = output.name + "[]";
                if (const auto outer = outer_output(each); outer != entry.outputs.end()) {
                    GraphCanvasDrawing::ZoneRow bundle;
                    bundle.label = outer->label;
                    bundle.outer = qtext(each);
                    bundle.outer_row = *outer;
                    placed_outputs.insert(each);
                    zone.outputs.push_back(std::move(bundle));
                }
            }
            for (const auto &output : node.outputs) {
                if (placed_outputs.contains(output.port)) continue;
                const auto outer = outer_output(output.port);
                if (outer == entry.outputs.end()) continue;
                GraphCanvasDrawing::ZoneRow row;
                row.label = outer->label;
                row.outer = qtext(output.port);
                row.outer_row = *outer;
                zone.outputs.push_back(std::move(row));
            }
            // LP-4d2. The edges' empty slots, for a document that can be
            // edited (decisions 1, 2); the carry's arrows (decision 3); and
            // what the loop became, along the bottom (decision 5).
            zone.edge_slots = !model_->digest().empty() && !model_->foldedView();
            if (loop) {
                for (const auto &carry : loop->carry) {
                    GraphCanvasDrawing::ZoneCarry drawn_carry;
                    drawn_carry.from = qtext(carry.from);
                    drawn_carry.to = qtext(carry.to);
                    drawn_carry.unbound = carry.to_unbound;
                    // The Loop fields' warning, word for word.
                    if (carry.to_unbound)
                        drawn_carry.warning =
                            GraphCanvasLoopFields::tr(
                                "%1 has no loop input: connect one for the first element to "
                                "start from.")
                                .arg(qtext(carry.to));
                    drawn_carry.selected =
                        selected_carry_ && selected_carry_->loop == node.id &&
                        selected_carry_->from == carry.from && selected_carry_->to == carry.to;
                    zone.carries.push_back(std::move(drawn_carry));
                }
                if (draw_loop_report && model_->foldedView() && loop->reported_total == 0 &&
                    loop->reported_count <= 0) {
                    // GL-1, decision 3: a material pass's inside, with no
                    // count the plan resolved -- the stage, and no number.
                    zone.stage = QObject::tr("draw_loop");
                } else if (draw_loop_report) {
                    const auto materials = loop->reported_total > 0
                                               ? loop->reported_total
                                               : static_cast<std::size_t>(std::max<std::int64_t>(
                                                     loop->reported_count, 0));
                    zone.expansion = materials == 1
                                         ? QObject::tr("1 material")
                                         : QObject::tr("%1 materials")
                                               .arg(static_cast<qulonglong>(materials));
                } else if (loop->reported) {
                    zone.expansion = QObject::tr("×%1 copies").arg(loop->reported_count);
                    zone.frame_plan_button = true;
                }
            }
            zone.content = plan->second.content;
            GraphCanvasDrawing::layoutZone(zone, drawing.headingMetrics(), drawing.rowMetrics());
            entry.width = zone.node.width;
            entry.height = zone.node.height;
            zones.emplace(node.id, std::move(zone));
        }
        drawn.push_back(std::move(entry));
    }
    // GL-1b, decision 1: one placement for the whole root, the Frame Plan's
    // (layoutNodeGraph, columns by the longest path) -- the folded nodes and
    // the lines the plan implies go into it with the graph's own nodes and
    // edges, so the plan's order reads left to right as it does on the Frame
    // Plan's Logical graph.
    std::set<std::string, std::less<>> graph_ids;
    for (const auto &entry : drawn) graph_ids.insert(entry.node->id);
    std::set<std::string, std::less<>> folded_ids;
    std::vector<LayoutNode> folded_layout_nodes;
    for (const auto &folded : folded_nodes) {
        folded_ids.insert(folded.id);
        const QSizeF size = drawing.derivedNodeSize(folded);
        folded_layout_nodes.push_back(
            LayoutNode{foldedLayoutKey(folded.id), size.width(), size.height()});
    }
    std::vector<LayoutEdge> derived_layout_edges;
    std::vector<LayoutEdge> folded_only_edges;
    for (const auto &edge : derived_edges) {
        const auto end = [&](const std::string &id) {
            return folded_ids.contains(id) ? foldedLayoutKey(id)
                   : graph_ids.contains(id) ? id
                                            : std::string{};
        };
        LayoutEdge line{end(edge.from), end(edge.to)};
        if (line.from.empty() || line.to.empty()) continue;
        if (folded_ids.contains(edge.from) && folded_ids.contains(edge.to))
            folded_only_edges.push_back(line);
        derived_layout_edges.push_back(std::move(line));
    }
    // UX-1, decision 6: the frame's ends stand in the last column, however
    // short the path to them -- a sink, and what writes to the screen (a
    // folded pass such as the Blank's present, or the graph node whose pass
    // does).
    const auto writesDisplay = [](const std::vector<std::string> &writes) {
        return std::ranges::any_of(writes, [](const std::string &resource) {
            return resource == "display" || resource == "swapchain";
        });
    };
    std::set<std::string, std::less<>> last_column;
    for (const auto &entry : drawn)
        if (entry.node->sink) last_column.insert(entry.node->id);
    for (const auto &folded : folded_nodes)
        if (writesDisplay(folded.writes)) last_column.insert(foldedLayoutKey(folded.id));
    for (const auto &pass : model_->planPasses()) {
        if (!writesDisplay(pass.writes)) continue;
        if (const auto owner = model_->canvasNodeOfPass(pass.name); graph_ids.contains(owner))
            last_column.insert(owner);
    }
    const NodeGraphLayout layout = GraphCanvasDrawing::placeNodes(
        drawn, edges, top, folded_layout_nodes, derived_layout_edges, last_column);
    // A zone's frame can reach left of or above the item's position (a body
    // node dragged there); the placement is of the frame, so the item goes
    // that much further in.
    const auto frameOffset = [&](const std::string &id) {
        const auto zone = zones.find(id);
        return zone == zones.end() ? QPointF{} : zone->second.frame.topLeft();
    };
    // What it chose, in the canvas's own terms. "Auto layout" turns this into
    // control points; nothing else reads it, and it is recomputed every draw.
    placement_boxes_.clear();
    for (const auto &[id, box] : layout.boxes)
        if (!isFoldedLayoutKey(id)) placement_boxes_.emplace(id, QPointF{box.x, box.y} - frameOffset(id));
    placement_bends_.clear();
    for (std::size_t index = 0; index < edges.size(); ++index) {
        if (index >= layout.bends.size() || layout.bends[index].empty()) continue;
        std::vector<QPointF> points;
        points.reserve(layout.bends[index].size());
        for (const LayoutPoint &point : layout.bends[index])
            points.push_back(QPointF{point.x, point.y});
        placement_bends_.emplace_back(
            GraphCanvasEdgeKey{edges[index].to_node, edges[index].to_port},
            std::move(points));
    }

    const GraphCanvasLayout &root_placement = this->layout(scope);
    // GL-1b: whether the document places any of the graph's nodes. Until it
    // does, the one placement is the whole canvas.
    const bool anchored = std::ranges::any_of(drawn, [&](const auto &entry) {
        return root_placement.nodes.contains(entry.node->id);
    });
    std::set<std::string, std::less<>> drawn_nodes;
    std::vector<std::pair<std::string, QGraphicsItem *>> zone_bodies;
    for (const auto &entry : drawn) {
        const GraphCanvasNode &node = *entry.node;
        const auto column_entry = layout.columns.find(node.id);
        const int column =
            column_entry == layout.columns.end() ? 0 : column_entry->second;
        const auto box = layout.boxes.find(node.id);
        const QPointF laid_out =
            box == layout.boxes.end() ? QPointF{0.0, top}
                                      : QPointF{box->second.x, box->second.y} - frameOffset(node.id);
        const auto position_key = std::pair{scope, node.id};
        QPointF position = laid_out;
        // N-6: the document is where a place lives now. The session table is
        // the fallback for a node the document has never placed -- one just
        // added from the palette, before its first drag went out.
        if (const auto placed = root_placement.nodes.find(node.id);
            placed != root_placement.nodes.end()) {
            position = placed->second;
        } else if (const auto retained = session_node_positions_.find(position_key);
                   retained != session_node_positions_.end()) {
            position = retained->second;
        }

        auto moved = [this, position_key, scope, anchored](const QPointF &at) {
            if (rebuilding_) return;
            // GL-1b: the first place the document is given ends the one
            // placement; the folded nodes stay where it put them.
            if (!anchored) keepFoldedPlaces();
            session_node_positions_[position_key] = at;
            // And the document, once the drag has settled. The control
            // points do not move with it: they are absolute, which is the
            // whole point of decision 1. (A zone's body is drawn inside its
            // frame, so it comes along without being written.)
            placement(scope).layout.nodes[position_key.second] = at;
            noteLayoutChange(scope);
            rerouteGraphCanvasEdges(*this, scope, position_key.second);
            // Growing the scene rect from inside itemChange re-enters this
            // handler through Qt's view update. Defer it to the event loop.
            scheduleSceneRectUpdate();
        };
        if (const auto zone = zones.find(node.id); zone != zones.end()) {
            QGraphicsItem *content = drawing.addZone(zone->second, column, position, std::move(moved));
            const auto &body = plans.at(node.id).body;
            zone_content_[body] = content;
            zone_content_rect_[body] = plans.at(node.id).content;
            const auto &metrics = zone->second.metrics;
            zone_geometry_[body] = ZoneGeometry{metrics.origin, metrics.min_width,
                                                metrics.min_height, metrics.footer,
                                                zone->second.frame};
            // LP-4d2: the carry's arrows, now that both their ends are drawn.
            for (QGraphicsItem *child : content->parentItem()->childItems())
                if (nodeGraphItemKind(*child) == QLatin1String{GraphCanvasZoneCarryItem})
                    if (auto *carry = dynamic_cast<QGraphicsPathItem *>(child))
                        GraphCanvasDrawing::routeZoneCarry(*carry);
            zones_[node.id] = body;
            zone_bodies.emplace_back(node.id, content);
        } else {
            drawing.addNode(entry, column, position, std::move(moved));
        }
        drawn_nodes.insert(node.id);
    }
    // GL-1c, decision 1: everything the graph has drawn on the root so far,
    // which no folded node lies on -- each node with what it holds (its
    // sockets, a value's text running past its box) and each zone's whole
    // frame (heading, edge rows, the carries' arrows, the bottom row).
    std::optional<QRectF> graph_bounds;
    std::vector<QRectF> graph_boxes;
    for (QGraphicsItem *item : items()) {
        if (item->parentItem() != nullptr ||
            nodeGraphItemKind(*item) != QLatin1String{GraphCanvasNodeItem})
            continue;
        const QRectF drawn_box =
            item->sceneBoundingRect().united(item->mapRectToScene(item->childrenBoundingRect()));
        graph_bounds = graph_bounds ? graph_bounds->united(drawn_box) : drawn_box;
        graph_boxes.push_back(drawn_box);
    }

    // GL-1b, decision 1: a folded node sits where the one placement put it --
    // a zone is one box of its frame's size there, so none lies on a frame
    // (GL-1c, decision 2). Once the document places the graph's nodes (a
    // drag, Auto layout), that placement no longer describes them: the folded
    // nodes keep the places they had this session while every one of them
    // stays clear of what the graph draws. When one would not (a zone grew
    // under it, a node was put on it) or one has no place yet, they are
    // placed again among themselves -- the same layout -- left of all of it
    // (GL-1c, decision 1), and keep that. A folded node the user dragged
    // stays where it was put. Where they sit is never written to the
    // document.
    std::optional<NodeGraphLayout> folded_alone;
    QPointF folded_shift;
    bool folded_kept = false;
    if (anchored && !folded_nodes.empty()) {
        folded_alone = GraphCanvasDrawing::placeNodes({}, {}, top, folded_layout_nodes,
                                                      folded_only_edges, last_column);
        std::optional<qreal> right;
        for (const auto &[id, box] : folded_alone->boxes)
            right = std::max(right.value_or(box.right()), box.right());
        const QRectF bounds = graph_bounds.value_or(QRectF{});
        folded_shift = QPointF{bounds.left() - NodeGraphHorizontalGap - right.value_or(0.0), 0.0};
        folded_kept = std::ranges::all_of(folded_nodes, [&](const GraphCanvasFoldedNode &folded) {
            const auto key = foldedPositionKey(folded.id);
            if (session_node_positions_.contains(key)) return true;
            const auto kept = folded_places_.find(key);
            return kept != folded_places_.end() &&
                   !QRectF{kept->second, drawing.derivedNodeSize(folded)}.intersects(bounds);
        });
    }
    const NodeGraphLayout &folded_layout = folded_alone ? *folded_alone : layout;
    std::set<std::string, std::less<>> derived_drawn;
    for (const auto &folded : folded_nodes) {
        const auto key = foldedPositionKey(folded.id);
        const auto layout_key = foldedLayoutKey(folded.id);
        QPointF position{0.0, top};
        if (const auto box = folded_layout.boxes.find(layout_key); box != folded_layout.boxes.end())
            position = QPointF{box->second.x, box->second.y} + folded_shift;
        if (folded_alone) {
            if (!folded_kept) folded_places_.insert_or_assign(key, position);
            else if (const auto kept = folded_places_.find(key); kept != folded_places_.end())
                position = kept->second;
        } else if (const auto kept = added_folded_places_.find(key);
                   kept != added_folded_places_.end() &&
                   std::ranges::none_of(graph_boxes, [&](const QRectF &box) {
                       return QRectF{kept->second, drawing.derivedNodeSize(folded)}.intersects(box);
                   })) {
            // UX-5a: a node was added before the document placed any: the
            // folded nodes stay where they were then, clear of it (placeNewNode).
            position = kept->second;
        }
        if (const auto kept = session_node_positions_.find(key); kept != session_node_positions_.end())
            position = kept->second;
        const auto column = folded_layout.columns.find(layout_key);
        const std::string id = folded.id;
        QGraphicsItem *item = drawing.addDerivedNode(folded, position, [this, key, id](const QPointF &at) {
            if (rebuilding_) return;
            session_node_positions_[key] = at;
            rerouteDerivedEdges(*this, id);
            scheduleSceneRectUpdate();
        });
        item->setData(GraphCanvasColumnRole, column == folded_layout.columns.end() ? 0 : column->second);
        derived_drawn.insert(folded.id);
    }

    for (std::size_t index = 0; index < edges.size(); ++index) {
        const auto &edge = edges[index];
        if (!drawn_nodes.contains(edge.from_node) || !drawn_nodes.contains(edge.to_node)) continue;
        // An edge that flies over a column follows the lane the layout kept
        // for it, so it passes between the nodes it flies over rather than
        // through them. An end that has been dragged loses the lane again.
        QVector<QPointF> bends = bendsFor(scope, edge);
        if (bends.isEmpty() && root_placement.empty() && index < layout.bends.size() &&
            !layout.bends[index].empty() &&
            !session_node_positions_.contains({scope, edge.from_node}) &&
            !session_node_positions_.contains({scope, edge.to_node})) {
            // No placement has been kept for this scope yet, so the lane the
            // automatic layout chose is still the best thing to draw through.
            // It is not a control point: "Auto layout" is what makes one.
            for (const LayoutPoint &point : layout.bends[index])
                bends.push_back(QPointF{point.x, point.y});
        }
        auto *line = drawing.addEdge(edge, bends);
        // No edge label: the socket rows already name both ends, and a box in
        // the middle of every line repeated them.
        routeGraphCanvasEdge(*this, *line);
    }

    // GL-1, decision 2: the lines the frame plan implies, dashed, read only.
    const auto drawn_end = [&](const std::string &id) {
        return derived_drawn.contains(id) || drawn_nodes.contains(id);
    };
    for (auto edge : derived_edges) {
        if (!drawn_end(edge.from) || !drawn_end(edge.to)) continue;
        // GL-1e: a resource a pass's `input` names is its own solid line.
        std::erase_if(edge.resources, [&](const std::string &resource) {
            return pass_line_resources.contains({edge.to, resource});
        });
        if (edge.resources.empty()) continue;
        routeDerivedEdge(*this, *drawing.addDerivedEdge(edge));
    }
    for (const auto &line : pass_lines) {
        if (!drawn_end(line.from) || !derived_drawn.contains(line.pass)) continue;
        auto *item = drawing.addPassInputEdge(qtext(line.from), qtext(line.from_port),
                                              qtext(line.pass), qtext(line.resource),
                                              line.input_index);
        routePassInputEdge(*this, *item);
        if (selected_pass_input_ && selected_pass_input_->pass == line.pass &&
            selected_pass_input_->input_index == line.input_index)
            item->setPen(QPen{currentStudioTheme().canvas.edge_selected, 2.4});
    }
    // Decision 5: where a line to a hidden node is cut, the socket it ends on
    // says where it goes.
    for (const auto &stub : model_->domainStubs()) {
        QGraphicsItem *socket =
            findSocketOrAnchor(*this, scope_text, qtext(stub.node), qtext(stub.port),
                               stub.input ? QStringLiteral("input") : QStringLiteral("output"));
        if (socket != nullptr) drawing.addDomainStub(stub, *socket);
    }
    // GL-2b, decision 2: under each root node, what its lower layers hold
    // (only what is not the default), and a node the composition switched
    // off drawn faint.
    if (scope.empty() && !model_->foldedView()) {
        std::vector<QGraphicsItem *> roots;
        for (QGraphicsItem *item : items())
            if (item->parentItem() == nullptr &&
                nodeGraphItemKind(*item) == QLatin1String{GraphCanvasNodeItem} &&
                item->data(GraphCanvasZoneScopeRole).toString().isEmpty())
                roots.push_back(item);
        for (QGraphicsItem *item : roots) {
            const auto id = item->data(NodeGraphNameRole).toString().toStdString();
            if (model_->deactivatedNode(id) != nullptr) item->setOpacity(0.45);
            if (const auto band = model_->layerBand(id); !band.empty())
                drawing.addLayerBand(*item, qtext(id), qtext(band));
        }
    }

    // The control points of one scope, drawn by one builder. They go over
    // everything they may come to lie on. A bundle's place is the middle of
    // the lines it carries, and the middle of a line is often over a node:
    // drawn under one, the handle cannot be taken hold of, and the press that
    // reaches for it drags the node instead. (The scenario found exactly that
    // on the real window.) They are small -- a 9-pixel dot and a 10-pixel
    // band -- so being on top costs the drawing almost nothing and is what
    // makes them reachable.
    const auto drawControls = [this](GraphCanvasDrawing::Builder &builder,
                                     const std::string &controls_scope,
                                     const std::vector<GraphCanvasEdge> &scope_edges,
                                     const std::set<std::string, std::less<>> &ends) {
        const auto drawnEdge = [&](const GraphCanvasEdgeKey &key) {
            return std::ranges::any_of(scope_edges, [&](const GraphCanvasEdge &candidate) {
                return candidate.to_node == key.node && candidate.to_port == key.port &&
                       ends.contains(candidate.from_node) && ends.contains(candidate.to_node);
            });
        };
        const GraphCanvasLayout &placed = this->layout(controls_scope);
        for (const GraphCanvasReroute &reroute : placed.reroutes) {
            if (!drawnEdge(reroute.edge)) continue;
            for (std::size_t index = 0; index < reroute.points.size(); ++index) {
                const GraphCanvasEdgeKey key = reroute.edge;
                builder.addReroute(key, index, reroute.points[index],
                    [this, controls_scope, key, index](const QPointF &moved) {
                        if (rebuilding_) return;
                        moveReroute(controls_scope, key, index, moved);
                    });
            }
        }
        for (std::size_t index = 0; index < placed.ties.size(); ++index)
            builder.addTie(placed.ties[index], index,
                           [this, controls_scope, index](const QPointF &moved) {
                               if (rebuilding_) return;
                               moveTie(controls_scope, index, moved);
                           });
    };
    drawControls(drawing, scope, edges, drawn_nodes);

    // LP-4d1: each zone's body, in the frame's item and the definition's own
    // coordinates, edited in the definition.
    for (const auto &[loop, content] : zone_bodies) {
        const ZonePlan &plan = plans.at(loop);
        const std::string &body = plan.body;
        GraphCanvasDrawing::Builder inside{*this, qtext(body)};
        inside.setContainer(content);
        std::set<std::string, std::less<>> ends;
        for (const auto &entry : plan.drawn) {
            const auto key = std::pair{body, entry.node->id};
            inside.addNode(entry, 0, plan.positions.at(entry.node->id),
                [this, key, scope, anchored](const QPointF &moved) {
                    if (rebuilding_) return;
                    // UX-1, decision 3: the zone stays where it is drawn. A
                    // root the document places nothing of is placed again
                    // on every draw, and a frame grown left or up by this
                    // drag would be placed that much further in -- the
                    // zone, and the node with it, back where the hand took
                    // it from. The root's nodes keep the places they have
                    // for the session; the root is not written.
                    if (!anchored) holdRootPlaces(scope);
                    session_node_positions_[key] = moved;
                    placement(key.first).layout.nodes[key.second] = moved;
                    noteLayoutChange(key.first);
                    rerouteGraphCanvasEdges(*this, key.first, key.second);
                    // LP-4d2, decision 7: the frame keeps round its body
                    // while the node is dragged, and is drawn around it
                    // again once the hand lets go.
                    refitZoneLive(key.first);
                    scheduleZoneRefit();
                    scheduleSceneRectUpdate();
                });
            ends.insert(entry.node->id);
        }
        for (const auto &id : plan.boundary) ends.insert(id);
        for (const auto &edge : plan.edges) {
            if (!ends.contains(edge.from_node) || !ends.contains(edge.to_node)) continue;
            auto *line = inside.addEdge(edge, bendsFor(body, edge));
            routeGraphCanvasEdge(*this, *line);
        }
        for (const auto &output : plan.outputs) {
            if (!ends.contains(output.node)) continue;
            auto *link = inside.addZoneLink(qtext(loop), qtext(output.name), qtext(output.node),
                                            qtext(output.port));
            routeZoneLink(*this, *link);
        }
        drawControls(inside, body, plan.edges, ends);
    }

    // The selection survives the rebuild when what it named is still here, in
    // the scope it was picked in.
    const auto scope_drawn = std::ranges::find(drawn_scopes_, selection_scope_) != drawn_scopes_.end();
    if (!scope_drawn) {
        selected_edges_.clear();
        clearControlSelection();
        selection_scope_ = scope;
    }
    const auto scopeNodesDrawn = [&](const std::string &id) {
        if (selection_scope_ == scope)
            return derived_drawn.contains(id) ||
                   std::ranges::any_of(nodes, [&](const GraphCanvasNode &node) { return node.id == id; });
        for (const auto &[loop, plan] : plans) {
            if (plan.body != selection_scope_) continue;
            return std::ranges::any_of(plan.nodes, [&](const GraphCanvasNode &node) { return node.id == id; }) ||
                   std::ranges::find(plan.boundary, id) != plan.boundary.end();
        }
        return false;
    };
    const auto selection_edges = [&]() -> std::vector<GraphCanvasEdge> {
        if (selection_scope_ == scope) return edges;
        for (const auto &[loop, plan] : plans)
            if (plan.body == selection_scope_) return plan.edges;
        return {};
    }();
    if (scope_drawn && previous_node && scopeNodesDrawn(*previous_node))
        selected_node_ = previous_node;
    if (scope_drawn && previous_edge &&
        std::ranges::any_of(selection_edges, [&](const GraphCanvasEdge &edge) {
            return edge.from_node == previous_edge->from_node &&
                   edge.from_port == previous_edge->from_port &&
                   edge.to_node == previous_edge->to_node &&
                   edge.to_port == previous_edge->to_port;
        }))
        selected_edge_ = previous_edge;
    // N-6: the multi-pick a bundle is made from survives a rebuild the same
    // way -- what is still drawn stays picked, and what is gone lets go.
    std::erase_if(selected_edges_, [&](const GraphCanvasEdge &picked) {
        return std::ranges::none_of(selection_edges, [&](const GraphCanvasEdge &edge) {
            return edge.from_node == picked.from_node &&
                   edge.from_port == picked.from_port &&
                   edge.to_node == picked.to_node &&
                   edge.to_port == picked.to_port;
        });
    });
    const GraphCanvasLayout &selection_placement = this->layout(selection_scope_);
    std::erase_if(selected_reroutes_, [&](const auto &picked) {
        const auto found = std::ranges::find(selection_placement.reroutes, picked.first,
                                             &GraphCanvasReroute::edge);
        return found == selection_placement.reroutes.end() ||
               picked.second >= found->points.size();
    });
    std::erase_if(selected_ties_, [&](const std::size_t index) {
        return index >= selection_placement.ties.size();
    });
    setProperty("pelicanGraphSelectedNode",
                selected_node_ ? qtext(*selected_node_) : QString{});
    // LP-4d2: a picked carry stays picked while its arrow is still drawn.
    if (selected_carry_ &&
        std::ranges::none_of(items(), [&](const QGraphicsItem *item) {
            return nodeGraphItemKind(*item) == QLatin1String{GraphCanvasZoneCarryItem} &&
                   item->data(NodeGraphNameRole).toString() == qtext(selected_carry_->loop) &&
                   item->data(GraphCanvasFromPortRole).toString() == qtext(selected_carry_->from) &&
                   item->data(GraphCanvasToPortRole).toString() == qtext(selected_carry_->to);
        }))
        selected_carry_.reset();
    applyEdgeEmphasis();

    QStringList trail;
    for (const auto &step : display_trail) trail.push_back(qtext(step));
    if (trail.size() > 1)
        drawing.addBreadcrumb(trail, crumb_top, [this] {
            // Leave after this item's mouse event finishes dispatching.
            QMetaObject::invokeMethod(this, [this] { leaveCurrentScope(); },
                                      Qt::QueuedConnection);
        });
    if (decomposable)
        drawing.addFoldedButton(QStringLiteral("show_decomposed"),
                                QObject::tr("Show decomposed version"),
                                QPointF{0.0, crumb_top - 12.0});

    setProperty("pelicanGraphScope",
                scope.empty() ? QStringLiteral("root") : scope_text);
    setProperty("pelicanGraphBreadcrumb", trail.join(QStringLiteral("/")));
    setProperty("pelicanGraphNodeCount", static_cast<qulonglong>(nodes.size()));
    setProperty("pelicanGraphEdgeCount", static_cast<qulonglong>(edges.size()));
    setProperty("pelicanGraphZoneCount", static_cast<qulonglong>(zones_.size()));
    setProperty("pelicanGraphFoldedCount", static_cast<qulonglong>(derived_drawn.size()));
    setProperty("pelicanGraphFoldedView", model_->foldedView());
    setProperty("pelicanGraphNotice", qtext(model_->notice().message));
    // What the automatic placement achieved, so `layout_stats` can ask a live
    // window how tangled it is rather than a reader having to look at it.
    setProperty("pelicanLayoutCrossings",
                static_cast<qulonglong>(layout.crossings));
    setProperty("pelicanLayoutOverlaps",
                static_cast<qulonglong>(layoutOverlaps(layout.boxes)));
    setProperty("pelicanLayoutNodes",
                static_cast<qulonglong>(layout.boxes.size()));
    setProperty("pelicanLayoutReversedEdges",
                static_cast<qulonglong>(layout.reversed_edges.size()));
    rebuilding_ = false;
    applySceneRectNow();
    // The editor goes back over the thing it was editing. A node or port the
    // draw no longer holds simply has nothing to open over, and the edit ends
    // there -- the one case where a rebuild may take it away.
    if (carried) restoreInlineEdit(*carried);
    if (repopulated) repopulated();
}

void GraphCanvasScene::scheduleZoneRefit() {
    if (zone_refit_queued_) return;
    zone_refit_queued_ = true;
    QTimer::singleShot(0, this, [this] {
        zone_refit_queued_ = false;
        // Not under the hand: redrawing deletes the item being dragged.
        if (dragging()) {
            zone_refit_queued_ = true;
            QTimer::singleShot(60, this, [this] {
                zone_refit_queued_ = false;
                scheduleZoneRefit();
            });
            return;
        }
        // Only when a body now reaches past what its frame was drawn around.
        bool changed = false;
        for (const auto &[body, content] : zone_content_) {
            QRectF extent;
            for (QGraphicsItem *child : content->childItems()) {
                if (nodeGraphItemKind(*child) != QLatin1String{GraphCanvasNodeItem}) continue;
                const auto *node = dynamic_cast<const QGraphicsPathItem *>(child);
                if (node == nullptr) continue;
                // The drawn box, not the pen around it: the same extent the
                // frame was drawn around.
                const QRectF box = child->mapRectToParent(node->path().boundingRect());
                extent = extent.isValid() ? extent.united(box) : box;
            }
            const auto drawn = zone_content_rect_.find(body);
            const QRectF before = drawn == zone_content_rect_.end() ? QRectF{} : drawn->second;
            if (std::abs(extent.left() - before.left()) > 0.5 ||
                std::abs(extent.top() - before.top()) > 0.5 ||
                std::abs(extent.right() - before.right()) > 0.5 ||
                std::abs(extent.bottom() - before.bottom()) > 0.5)
                changed = true;
        }
        if (changed) repopulate();
    });
}

void GraphCanvasScene::refitZoneLive(const std::string &body) {
    const auto content = zone_content_.find(body);
    const auto geometry = zone_geometry_.find(body);
    if (content == zone_content_.end() || geometry == zone_geometry_.end()) return;
    auto *frame = dynamic_cast<QGraphicsPathItem *>(content->second->parentItem());
    if (frame == nullptr) return;
    // The drawn boxes of the body's nodes, as the frame was drawn around
    // them (scheduleZoneRefit reads them the same way).
    QRectF extent;
    for (QGraphicsItem *child : content->second->childItems()) {
        if (nodeGraphItemKind(*child) != QLatin1String{GraphCanvasNodeItem}) continue;
        const auto *node = dynamic_cast<const QGraphicsPathItem *>(child);
        if (node == nullptr) continue;
        const QRectF box = child->mapRectToParent(node->path().boundingRect());
        extent = extent.isValid() ? extent.united(box) : box;
    }
    GraphCanvasDrawing::ZoneFrameMetrics metrics;
    metrics.origin = geometry->second.origin;
    metrics.min_width = geometry->second.min_width;
    metrics.min_height = geometry->second.min_height;
    metrics.footer = geometry->second.footer;
    const QRectF to = GraphCanvasDrawing::zoneFrame(metrics, extent);
    const QRectF from = geometry->second.frame;
    if (to == from) return;
    GraphCanvasDrawing::refitZoneFrame(*frame, from, to);
    geometry->second.frame = to;
    // What hangs off the frame's edges goes with them: the root's lines into
    // the loop and out of it, the body outputs' lines, the carry's arrows.
    const std::string loop = zoneLoopOf(body);
    const std::string root = model_ != nullptr ? model_->currentScope() : std::string{};
    if (!loop.empty()) rerouteGraphCanvasEdges(*this, root, loop);
    for (QGraphicsItem *item : items()) {
        const QString kind = nodeGraphItemKind(*item);
        if (kind == QLatin1String{GraphCanvasZoneLinkItem} &&
            item->data(GraphCanvasScopeRole).toString() == qtext(body)) {
            if (auto *link = dynamic_cast<QGraphicsPathItem *>(item)) routeZoneLink(*this, *link);
        } else if (kind == QLatin1String{GraphCanvasZoneCarryItem} && item->parentItem() == frame) {
            if (auto *carry = dynamic_cast<QGraphicsPathItem *>(item))
                GraphCanvasDrawing::routeZoneCarry(*carry);
        }
    }
}

std::string GraphCanvasScene::zoneLoopOf(const std::string &body) const {
    for (const auto &[loop, drawn] : zones_)
        if (drawn == body) return loop;
    return {};
}

void GraphCanvasScene::selectCarry(const std::string &loop, const std::string &from,
                                   const std::string &to) {
    const std::string root = model_ != nullptr ? model_->currentScope() : std::string{};
    enterSelectionScope(root);
    selected_node_.reset();
    selected_edge_.reset();
    selected_edges_.clear();
    clearControlSelection();
    selected_carry_ = CarryPick{loop, from, to};
    setProperty("pelicanGraphSelectedNode", QString{});
    for (QGraphicsItem *item : items()) {
        if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasZoneCarryItem}) continue;
        auto *arrow = dynamic_cast<QGraphicsPathItem *>(item);
        if (arrow == nullptr) continue;
        const bool picked = item->data(NodeGraphNameRole).toString() == qtext(loop) &&
                            item->data(GraphCanvasFromPortRole).toString() == qtext(from) &&
                            item->data(GraphCanvasToPortRole).toString() == qtext(to);
        QPen pen = arrow->pen();
        pen.setWidthF(picked ? 3.0 : 1.8);
        arrow->setPen(pen);
    }
    applyEdgeEmphasis();
    if (selectionChanged) selectionChanged();
}

void GraphCanvasScene::beginCarry(const QGraphicsItem &socket, const std::string &loop,
                                  const std::string &output) {
    beginConnection(socket);
    if (!pending_connection_) return;
    pending_connection_->carry_loop = loop;
    pending_connection_->node = output;
    pending_connection_->from_output = true;
    pending_connection_->scope = zones_.contains(loop) ? zones_.at(loop) : std::string{};
    // It is a carry now: what it can land on is a boundary input.
    for (const LitSocket &lit : lit_sockets_) {
        lit.item->setOpacity(lit.opacity);
        lit.item->setScale(lit.scale);
    }
    lit_sockets_.clear();
    lightDropTargets(&socket);
}

QPointF GraphCanvasScene::toScopePoint(const std::string &scope, const QPointF &at) const {
    const auto content = zone_content_.find(scope);
    if (content == zone_content_.end()) return at;
    return content->second->mapFromScene(at);
}

std::string GraphCanvasScene::scopeAt(const QPointF &at) const {
    const std::string current = model_ != nullptr ? model_->currentScope() : std::string{};
    for (QGraphicsItem *item : items(at)) {
        const QString kind = nodeGraphItemKind(*item);
        if (kind.isEmpty()) continue;
        const QString zone = item->data(GraphCanvasZoneScopeRole).toString();
        if (!zone.isEmpty()) return zone.toStdString();
        if (kind == QLatin1String{GraphCanvasZoneItem})
            return item->data(GraphCanvasDefinitionRole).toString().toStdString();
        return current;
    }
    return current;
}

std::string GraphCanvasScene::addScope() const {
    if (menu_scope_) return *menu_scope_;
    if (palette_scope_) return *palette_scope_;
    const std::string current = model_ != nullptr ? model_->currentScope() : std::string{};
    // Something picked inside a zone: its body.
    if (selection_scope_ != current && zone_content_.contains(selection_scope_))
        return selection_scope_;
    // A picked zone: its body.
    if (selected_node_ && selection_scope_ == current)
        if (const auto zone = zones_.find(*selected_node_); zone != zones_.end())
            return zone->second;
    return current;
}

void GraphCanvasScene::applyEdgeEmphasis() {
    // Which edges touch the selected node is the one thing the canvas says
    // about a selection, so it is applied in one pass over every edge rather
    // than remembered per item. Only the edges of the scope the pick is in:
    // a body node's id can be a root node's too.
    const QString selected =
        selected_node_ ? qtext(*selected_node_) : QString{};
    const QString selected_scope = qtext(selection_scope_);
    for (QGraphicsItem *item : items()) {
        const QString kind = nodeGraphItemKind(*item);
        const bool edge = kind == QLatin1String{GraphCanvasEdgeItem};
        const bool arrow = kind == QLatin1String{GraphCanvasEdgeArrowItem};
        if (!edge && !arrow) continue;
        const bool in_scope = item->data(GraphCanvasScopeRole).toString() == selected_scope;
        const bool touches =
            in_scope && !selected.isEmpty() &&
            (item->data(NodeGraphFromNameRole).toString() == selected ||
             item->data(NodeGraphToNameRole).toString() == selected);
        // The picked edge is the one Delete would cut, so it is louder than an
        // edge that merely touches the picked node.
        // Every picked edge is loud, not only the last one: a bundle is made
        // out of several, and the drawing has to say which ones they are.
        const bool picked =
            in_scope &&
            std::ranges::any_of(selected_edges_, [&](const GraphCanvasEdge &drawn) {
                return item->data(NodeGraphFromNameRole).toString() ==
                           qtext(drawn.from_node) &&
                       item->data(NodeGraphToNameRole).toString() ==
                           qtext(drawn.to_node) &&
                       (!edge ||
                        (item->data(GraphCanvasFromPortRole).toString() ==
                             qtext(drawn.from_port) &&
                         item->data(GraphCanvasToPortRole).toString() ==
                             qtext(drawn.to_port)));
            });
        const StudioTheme &theme = currentStudioTheme();
        const QColor colour = picked    ? theme.canvas.edge_selected
                              : touches ? theme.canvas.heading_text
                                        : theme.canvas.edge;
        if (edge) {
            if (auto *line = dynamic_cast<QGraphicsPathItem *>(item)) {
                QPen pen{colour, picked ? 3.0 : touches ? 2.2 : 1.6};
                // LP-4d2, decision 4: a line reading one element stays dashed.
                if (!item->data(GraphCanvasEdgeElementRole).toString().isEmpty())
                    pen.setStyle(Qt::DashLine);
                line->setPen(pen);
            }
        } else if (auto *head = dynamic_cast<QGraphicsPolygonItem *>(item)) {
            head->setBrush(colour);
        }
    }
    // What the two bundle entries are allowed to do, from what is picked.
    if (tie_action_ != nullptr) {
        std::set<GraphCanvasEdgeKey> distinct;
        for (const GraphCanvasEdge &drawn : selected_edges_)
            distinct.insert(GraphCanvasEdgeKey{drawn.to_node, drawn.to_port});
        for (const auto &[key, index] : selected_reroutes_) {
            (void)index;
            distinct.insert(key);
        }
        tie_action_->setEnabled(distinct.size() >= 2);
    }
    if (untie_action_ != nullptr)
        untie_action_->setEnabled(!selected_ties_.empty());
}

void GraphCanvasScene::scheduleSceneRectUpdate() {
    if (scene_rect_update_queued_) return;
    scene_rect_update_queued_ = true;
    // A gesture (a node dragged, a reroute or a tie moved) only grows the
    // rect. Recomputing it from the items made it shrink on the side a node
    // left -- dragging the leftmost node (a folded feature) moved the
    // scene's edge under the view and the whole canvas slid.
    QMetaObject::invokeMethod(
        this, [this] { applySceneRectNow(/*grow_only=*/true); }, Qt::QueuedConnection);
}

void GraphCanvasScene::applySceneRectNow(bool grow_only) {
    scene_rect_update_queued_ = false;
    QRectF rect = itemsBoundingRect().adjusted(-60.0, -60.0, 60.0, 60.0);
    if (grow_only) {
        rect = rect.united(sceneRect());
        if (rect == sceneRect()) return;
    }
    // A redraw sets the rect anew and may shrink it; each view keeps the
    // scene point it had in its middle, so what is on screen stays put.
    //
    // UX-1, decision 2: keeping the middle is not enough on its own. A view
    // centres a scene rect that is narrower (or shorter) than its viewport --
    // which a fitted graph always is along one side -- so a rect that grew
    // under a drag, or was set anew by the redraw after it, moved everything
    // on screen by half the change: the zone the hand had just put down was
    // drawn back where it came from, and during the drag Qt replayed the
    // move against the shifted view and the zone lagged behind the hand.
    // Each view is given the rect together with what it shows now -- before
    // the scene's own changes, so no view ever follows a rect it would
    // centre -- and keeps its scroll as it is: what it shows lies inside
    // the new rect, so nothing on screen moves. (Centring on the old middle
    // again was off by the half pixel QRect::center() rounds away, and the
    // drag's every step added another.)
    std::vector<std::pair<QGraphicsView *, QPointF>> centres;
    for (QGraphicsView *view : views()) {
        const QRectF visible = view->mapToScene(view->viewport()->rect()).boundingRect();
        if (visible.isValid() && !visible.isEmpty())
            view->setSceneRect(rect.united(visible));
        else
            centres.emplace_back(view, view->mapToScene(view->viewport()->rect().center()));
    }
    setSceneRect(rect);
    for (const auto &[view, centre] : centres) view->centerOn(centre);
}

void GraphCanvasScene::selectNode(const std::string &id) {
    selectNode(model_ != nullptr ? model_->currentScope() : std::string{}, id);
}

void GraphCanvasScene::selectNode(const std::string &scope, const std::string &id) {
    selected_carry_.reset();
    enterSelectionScope(scope);
    selected_node_ = id;
    selected_edge_.reset();
    selected_edges_.clear();
    clearControlSelection();
    setProperty("pelicanGraphSelectedNode", qtext(id));
    applyEdgeEmphasis();
    if (selectionChanged) selectionChanged();
}

void GraphCanvasScene::selectEdge(const GraphCanvasEdge &edge, bool additive) {
    selectEdge(model_ != nullptr ? model_->currentScope() : std::string{}, edge, additive);
}

void GraphCanvasScene::selectEdge(const std::string &scope, const GraphCanvasEdge &edge,
                                  bool additive) {
    selected_carry_.reset();
    const auto same = [&](const GraphCanvasEdge &other) {
        return other.from_node == edge.from_node &&
               other.from_port == edge.from_port &&
               other.to_node == edge.to_node && other.to_port == edge.to_port;
    };
    if (scope != selection_scope_) additive = false;
    enterSelectionScope(scope);
    if (!additive) {
        selected_edges_.clear();
        clearControlSelection();
    }
    const auto found = std::ranges::find_if(selected_edges_, same);
    if (found != selected_edges_.end()) {
        // Holding the modifier on something already picked lets it go: that is
        // how a bundle is corrected without starting the picking again.
        if (additive) selected_edges_.erase(found);
    } else {
        selected_edges_.push_back(edge);
    }
    selected_edge_ = selected_edges_.empty()
                         ? std::optional<GraphCanvasEdge>{}
                         : std::optional<GraphCanvasEdge>{selected_edges_.back()};
    selected_node_.reset();
    setProperty("pelicanGraphSelectedNode", QString{});
    applyEdgeEmphasis();
    if (selectionChanged) selectionChanged();
}

void GraphCanvasScene::afterEdit(bool submitted) {
    if (!submitted) return;
    // GL-1e: the tab's last edit is the graph's now.
    render_edits_ = 0;
    render_undone_ = 0;
    // The request is in the model's outbox; only the owner can post it.
    if (editSubmitted) editSubmitted();
}

void GraphCanvasScene::clearPendingConnection() {
    pending_connection_.reset();
    // UX-1: the sockets go back to how they were drawn. Called before every
    // redraw deletes them, so the pointers are still the scene's.
    for (const LitSocket &lit : lit_sockets_) {
        lit.item->setOpacity(lit.opacity);
        lit.item->setScale(lit.scale);
    }
    lit_sockets_.clear();
    snap_target_ = nullptr;
    if (pending_edge_ != nullptr) {
        removeItem(pending_edge_);
        delete pending_edge_;
        pending_edge_ = nullptr;
    }
}

void GraphCanvasScene::beginConnection(const QGraphicsItem &socket) {
    const auto direction = socket.data(GraphCanvasSocketDirectionRole).toString();
    const auto port = socket.data(GraphCanvasPortRole).toString();
    // A folded node's anchor stands for every port at once: there is no one
    // port a drag from it could mean, so it does not start one.
    if (port.isEmpty()) return;
    clearPendingConnection();
    PendingConnection pending;
    pending.scope = itemScope(socket);
    pending.node = socket.data(NodeGraphNameRole).toString().toStdString();
    pending.port = port.toStdString();
    pending.from_output = direction == QLatin1String{"output"};
    pending.socket_key = socket.data(GraphCanvasSocketKeyRole).toString().toStdString();
    pending.origin = socket.sceneBoundingRect().center();
    pending.from_folded = nodeGraphItemKind(socket) == QLatin1String{GraphCanvasDerivedSocketItem};
    pending_connection_ = std::move(pending);
    pending_edge_ = new QGraphicsPathItem;
    addItem(pending_edge_);
    pending_edge_->setZValue(4.0);
    pending_edge_->setPen(QPen{currentStudioTheme().canvas.edge_selected, 1.8,
                               Qt::DashLine});
    pending_edge_->setData(NodeGraphItemKindRole,
                           QString::fromLatin1(GraphCanvasPendingEdgeItem));
    pending_edge_->setData(NodeGraphNameRole,
                           qtext(pending_connection_->node));
    pending_edge_->setData(GraphCanvasPortRole, port);
    pending_edge_->setPath(
        curveGeometryBetween(pending_connection_->origin,
                             pending_connection_->origin).path);
    lightDropTargets(&socket);
}

void GraphCanvasScene::lightDropTargets(const QGraphicsItem *origin) {
    if (!pending_connection_) return;
    const auto pending = *pending_connection_;
    for (QGraphicsItem *item : items()) {
        if (item == origin || !item->isVisible()) continue;
        const QString kind = nodeGraphItemKind(*item);
        const bool socket = kind == QLatin1String{GraphCanvasSocketItem} ||
                            kind == QLatin1String{GraphCanvasZoneSocketItem} ||
                            (kind == QLatin1String{GraphCanvasDerivedSocketItem} &&
                             item->data(GraphCanvasPassInputRole).isValid());
        if (!socket) continue;
        // The `+` drawn on an empty slot is the slot's; the slot answers.
        if (item->parentItem() != nullptr && nodeGraphItemKind(*item->parentItem()) == kind)
            continue;
        LitSocket lit{item, item->opacity(), item->scale(), item->sceneBoundingRect().center(),
                      static_cast<bool>(dropAction(pending, *item))};
        // Lit a size up; the rest drawn faint, so what is left to aim at
        // is what the line can land on.
        if (lit.target) item->setScale(lit.scale * 1.35);
        else item->setOpacity(lit.opacity * 0.3);
        lit_sockets_.push_back(lit);
    }
    drop_nodes_.clear();
}

void GraphCanvasScene::followCursor(const QPointF &at) {
    if (!pending_connection_ || pending_edge_ == nullptr) return;
    // The reach is pixels on the screen: the scene units it covers follow
    // the zoom.
    qreal zoom = 1.0;
    if (!views().isEmpty()) zoom = views().front()->transform().m11();
    const qreal reach = GraphCanvasSnapRadius / (zoom > 0.0 ? zoom : 1.0);
    const LitSocket *nearest = nullptr;
    qreal best = reach;
    for (const LitSocket &lit : lit_sockets_) {
        if (!lit.target) continue;
        const qreal distance = QLineF{lit.centre, at}.length();
        if (distance <= best) {
            best = distance;
            nearest = &lit;
        }
    }
    QGraphicsItem *snapped = nearest != nullptr ? nearest->item : nullptr;
    if (snapped != snap_target_) {
        for (const LitSocket &lit : lit_sockets_) {
            if (lit.item == snap_target_) lit.item->setScale(lit.scale * 1.35);
            if (lit.item == snapped) lit.item->setScale(lit.scale * 1.7);
        }
        snap_target_ = snapped;
        pending_edge_->setData(NodeGraphToNameRole, snapped != nullptr
                                                        ? snapped->data(NodeGraphNameRole)
                                                        : QVariant{});
        pending_edge_->setData(GraphCanvasToPortRole, snapped != nullptr
                                                          ? snapped->data(GraphCanvasPortRole)
                                                          : QVariant{});
    }
    const QPointF end = nearest != nullptr ? nearest->centre : at;
    const bool from_output = pending_connection_->from_output;
    pending_edge_->setPath(curveGeometryBetween(from_output ? pending_connection_->origin : end,
                                                from_output ? end : pending_connection_->origin)
                               .path);
}

std::function<void()> GraphCanvasScene::dropAction(const PendingConnection &pending,
                                                   QGraphicsItem &item) {
    if (model_ == nullptr) return {};
    const QString kind = nodeGraphItemKind(item);
    const std::string current = model_->currentScope();
    // LP-4d2, decision 3: from a body output's inside end to a boundary
    // input's (the left edge's inside socket): the loop carries it.
    if (!pending.carry_loop.empty()) {
        if (kind != QLatin1String{GraphCanvasSocketItem} || itemScope(item) != pending.scope ||
            item.data(GraphCanvasZoneScopeRole).toString().isEmpty() ||
            item.data(GraphCanvasSocketDirectionRole).toString() != QLatin1String{"output"} ||
            item.data(GraphCanvasPortRole).toString() != QLatin1String{"value"} ||
            item.parentItem() == nullptr ||
            nodeGraphItemKind(*item.parentItem()) != QLatin1String{GraphCanvasNodeItem} ||
            item.parentItem()->data(NodeGraphNameRole).toString() != qtext(pending.carry_loop))
            return {};
        const auto to = item.data(NodeGraphNameRole).toString().toStdString();
        return [this, loop = pending.carry_loop, from = pending.node, to] {
            afterEdit(model_->addLoopCarry(loop, from, to));
        };
    }
    // GL-1e, decision 2: onto an input socket of a rendering-section pass
    // (or its `+ input`): that pass's `input`, edited as the Frame Plan's
    // socket drop edits it.
    if (kind == QLatin1String{GraphCanvasDerivedSocketItem} &&
        item.data(GraphCanvasPassInputRole).isValid()) {
        if (!pending.from_output || !pending.scope.empty() || !current.empty()) return {};
        const auto pass = item.data(NodeGraphNameRole).toString().toStdString();
        QGraphicsItem *socket = &item;
        // UX-5c: a folded node's row is the resource it names (a feature's
        // image, the ray-traced mask).
        if (pending.from_folded) {
            if (pending.node == pass) return {};
            return [this, pass, socket, resource = pending.port] {
                dropResourceOnPassInput(pass, *socket, resource);
            };
        }
        return [this, pass, socket, node = pending.node, port = pending.port] {
            dropOnPassInput(pass, *socket, node, port);
        };
    }
    // Nothing else takes a folded node's row: it is no graph node's output.
    if (pending.from_folded) return {};
    // LP-4d2, decisions 1 and 2: the empty slot at the end of a zone's edge.
    // Left, a root output there is a new loop input; right, a body output
    // there is a new output of every body.
    if (kind == QLatin1String{GraphCanvasZoneSocketItem}) {
        const auto slot = item.data(GraphCanvasPortRole).toString();
        const auto loop = item.data(NodeGraphNameRole).toString().toStdString();
        if (!pending.from_output) return {};
        if (slot == QLatin1String{"in:+"} && current.empty() && pending.scope.empty())
            return [this, loop, node = pending.node, port = pending.port] {
                afterEdit(model_->addZoneInput(loop, node, port));
            };
        if (slot == QLatin1String{"out:+"} && zones_.contains(loop) &&
            zones_.at(loop) == pending.scope)
            return [this, loop, scope = pending.scope, node = pending.node, port = pending.port] {
                const bool submitted = model_->addZoneOutput(loop, scope, node, port);
                afterEdit(submitted);
                if (!submitted && noticeChanged) noticeChanged();
            };
        return {};
    }
    if (kind != QLatin1String{GraphCanvasSocketItem}) return {};
    const QString wanted = pending.from_output ? QStringLiteral("input") : QStringLiteral("output");
    if (item.data(GraphCanvasSocketDirectionRole).toString() != wanted) return {};
    // LP-4d1: an edge stays in one scope. A zone's body and the root around
    // it are two documents' worth of nodes. LP-4d2, decision 1: a root output
    // dropped on a body node's input inside a zone is the one exception -- a
    // new loop input, its graph.input in every body, and the input reading
    // it, in one list.
    if (itemScope(item) != pending.scope) {
        const auto body = itemScope(item);
        const auto loop = zoneLoopOf(body);
        if (!pending.from_output || !pending.scope.empty() || !current.empty() || loop.empty() ||
            item.data(GraphCanvasZoneScopeRole).toString().isEmpty())
            return {};
        const auto target = item.data(NodeGraphNameRole).toString().toStdString();
        // Q-19, decision 3: onto a socket of a body pass's shader, the new
        // graph.input is read through a numbered port.
        const auto key = item.data(GraphCanvasSocketKeyRole).toString().toStdString();
        if (!key.empty())
            return [this, loop, body, target, key, node = pending.node, port = pending.port] {
                const bool submitted =
                    model_->addZoneInputToSocket(loop, node, port, body, target, key);
                afterEdit(submitted);
                if (!submitted && noticeChanged) noticeChanged();
            };
        const auto target_port = item.data(GraphCanvasPortRole).toString().toStdString();
        if (target_port.empty()) return {};
        return [this, loop, body, target, target_port, node = pending.node, port = pending.port] {
            afterEdit(model_->addZoneInput(loop, node, port, body, target, target_port));
        };
    }
    const auto port = item.data(GraphCanvasPortRole).toString().toStdString();
    if (port.empty()) return {}; // A folded node names no single port.
    const auto node = item.data(NodeGraphNameRole).toString().toStdString();
    // Q-19, decision 2: a socket of a render node's shader declarations takes
    // the line as a numbered port at its binding -- or refuses it in one line
    // (a skipped binding, a full band).
    const auto key = item.data(GraphCanvasSocketKeyRole).toString().toStdString();
    if (!key.empty() || !pending.socket_key.empty())
        return [this, pending, node, port, key] {
            const bool submitted =
                pending.from_output
                    ? model_->connectSocket(pending.scope, node, key, pending.node, pending.port)
                    : model_->connectSocket(pending.scope, pending.node, pending.socket_key, node,
                                            port);
            afterEdit(submitted);
            if (!submitted && noticeChanged) noticeChanged();
        };
    const auto from_node = pending.from_output ? pending.node : node;
    const auto from_port = pending.from_output ? pending.port : port;
    const auto to_node = pending.from_output ? node : pending.node;
    const auto to_port = pending.from_output ? port : pending.port;
    // A line that does not type-check does not land: the socket is simply not
    // one this edge can end on.
    auto drawn = drop_nodes_.find(pending.scope);
    if (drawn == drop_nodes_.end())
        drawn = drop_nodes_.emplace(pending.scope, model_->nodes(pending.scope)).first;
    if (!GraphCanvasModel::canConnectAmong(drawn->second, from_node, from_port, to_node, to_port))
        return {};
    return [this, scope = pending.scope, from_node, from_port, to_node, to_port] {
        afterEdit(model_->connectInput(scope, to_node, to_port, from_node, from_port));
    };
}

void GraphCanvasScene::finishConnection(const QPointF &position) {
    if (!pending_connection_ || model_ == nullptr) {
        clearPendingConnection();
        return;
    }
    const auto pending = *pending_connection_;
    QGraphicsItem *snapped = snap_target_;
    clearPendingConnection();
    drop_nodes_.clear();
    // UX-1, decision 1: the socket the line was drawn onto, else what is
    // under the hand -- judged by the one function that lit them.
    std::function<void()> drop;
    if (snapped != nullptr) drop = dropAction(pending, *snapped);
    if (!drop)
        for (QGraphicsItem *item : items(position))
            if ((drop = dropAction(pending, *item))) break;
    drop_nodes_.clear();
    if (drop) {
        drop();
        return;
    }
    // Nothing takes it. A carry, or a line from an output, lands nowhere.
    if (!pending.carry_loop.empty() || pending.from_output) return;
    // From an input, let go over a socket it could have been meant for (an
    // output of its scope of another type, a pass's input, a zone's slot):
    // that is a drop that does not land, not a cut.
    for (QGraphicsItem *item : items(position)) {
        const QString kind = nodeGraphItemKind(*item);
        if ((kind == QLatin1String{GraphCanvasDerivedSocketItem} &&
             item->data(GraphCanvasPassInputRole).isValid()) ||
            kind == QLatin1String{GraphCanvasZoneSocketItem} ||
            (kind == QLatin1String{GraphCanvasSocketItem} &&
             item->data(GraphCanvasSocketDirectionRole).toString() == QLatin1String{"output"} &&
             itemScope(*item) == pending.scope))
            return;
    }
    // Dropped on nothing. From an input socket that is standing on an edge,
    // that is how the edge is cut.
    const auto drawn = model_->nodes(pending.scope);
    const auto owner = std::ranges::find(drawn, pending.node, &GraphCanvasNode::id);
    if (owner == drawn.end()) return;
    // Q-19: a connected socket of the shader's declarations dragged off is
    // its numbered port taken out (and the ones after it renumbered).
    if (!pending.socket_key.empty()) {
        const auto socket = std::ranges::find_if(owner->input_sockets, [&](const auto &each) {
            return each.key() == pending.socket_key;
        });
        if (socket == owner->input_sockets.end() || !socket->connected()) return;
        afterEdit(model_->disconnectInput(pending.scope, pending.node, socket->port));
        return;
    }
    const auto port = std::ranges::find(owner->inputs, pending.port,
                                        &GraphCanvasPort::port);
    if (port == owner->inputs.end() || !port->connected) return;
    afterEdit(model_->disconnectInput(pending.scope, pending.node, pending.port));
}

void GraphCanvasScene::mousePressEvent(QGraphicsSceneMouseEvent *event) {
    // A control point is picked here and then handed to the base class, which
    // is what starts its drag. The node hunt at the bottom must not then take
    // the pick away again.
    bool picked_control = false;
    if (event->button() == Qt::LeftButton) {
        // GL-1e: a press anywhere else lets go of a picked pass line.
        if (selected_pass_input_) {
            selected_pass_input_.reset();
            for (QGraphicsItem *line : items())
                if (nodeGraphItemKind(*line) == QLatin1String{GraphCanvasPassInputEdgeItem})
                    if (auto *path = dynamic_cast<QGraphicsPathItem *>(line))
                        path->setPen(QPen{currentStudioTheme().canvas.edge, 1.8});
        }
        for (QGraphicsItem *item : items(event->scenePos())) {
            const QString kind = nodeGraphItemKind(*item);
            if (kind == QLatin1String{GraphCanvasEditorItem}) break;
            // GL-1e: a line into a rendering-section pass is picked; Delete
            // takes it out of the pass's `input`.
            if (kind == QLatin1String{GraphCanvasPassInputEdgeItem}) {
                event->accept();
                selectPassInput(item->data(NodeGraphToNameRole).toString().toStdString(),
                                static_cast<std::size_t>(
                                    item->data(GraphCanvasPassInputRole).toLongLong()));
                return;
            }
            // LP-4d1: every pick is made in the scope the item belongs to --
            // a zone's body is its definition, whatever the canvas shows.
            const std::string scope = itemScope(*item);
            if (kind == QLatin1String{GraphCanvasZoneTabItem}) {
                event->accept();
                const auto loop = item->data(NodeGraphNameRole).toString().toStdString();
                const auto branch =
                    static_cast<std::size_t>(item->data(GraphCanvasBranchRole).toULongLong());
                // A redraw deletes the tab being pressed: from the event loop.
                QMetaObject::invokeMethod(
                    this, [this, loop, branch] { showLoopBranch(loop, branch); },
                    Qt::QueuedConnection);
                return;
            }
            // LP-4d2, decision 3: a line from a body output's inside end on
            // the right edge is a carry on its way to a boundary input.
            if (kind == QLatin1String{GraphCanvasZoneSocketItem}) {
                const auto port = item->data(GraphCanvasPortRole).toString();
                if (port.startsWith(QLatin1String{"out:"}) && port != QLatin1String{"out:+"}) {
                    event->accept();
                    const auto loop = item->data(NodeGraphNameRole).toString().toStdString();
                    selectNode(scope, loop);
                    beginCarry(*item, loop, port.mid(4).toStdString());
                    return;
                }
            }
            // Decision 5: Show in Frame Plan, from the event loop -- the
            // owner switches tabs.
            if (kind == QLatin1String{GraphCanvasZoneButtonItem}) {
                event->accept();
                const auto loop = item->data(NodeGraphNameRole).toString().toStdString();
                QMetaObject::invokeMethod(
                    this,
                    [this, loop] {
                        if (showInFramePlanRequested) showInFramePlanRequested(loop);
                    },
                    Qt::QueuedConnection);
                return;
            }
            if (kind == QLatin1String{GraphCanvasZoneCarryItem} ||
                kind == QLatin1String{GraphCanvasZoneCarryMarkItem}) {
                // Q-19: the arrow ends on the boundary input's socket and is
                // drawn over it; a press on the socket is a line from it.
                const auto under = items(event->scenePos());
                if (std::ranges::any_of(under, [](const QGraphicsItem *other) {
                        return nodeGraphItemKind(*other) ==
                               QLatin1String{GraphCanvasSocketItem};
                    }))
                    continue;
                event->accept();
                selectCarry(item->data(NodeGraphNameRole).toString().toStdString(),
                            item->data(GraphCanvasFromPortRole).toString().toStdString(),
                            item->data(GraphCanvasToPortRole).toString().toStdString());
                return;
            }
            // GL-1, decision 5: a cut end shows the domain it leads to.
            if (kind == QLatin1String{GraphCanvasDomainStubItem}) {
                event->accept();
                const auto domain = item->data(GraphCanvasStubDomainRole).toString().toStdString();
                QMetaObject::invokeMethod(
                    this,
                    [this, domain] {
                        if (model_ == nullptr || !model_->setDomainShown(domain, true)) return;
                        repopulate();
                        if (domainFilterChanged) domainFilterChanged();
                    },
                    Qt::QueuedConnection);
                return;
            }
            // Decision 3: a feature's decomposed version, read only.
            if (kind == QLatin1String{GraphCanvasFoldedButtonItem}) {
                event->accept();
                QMetaObject::invokeMethod(
                    this, [this] { showDecomposed(); }, Qt::QueuedConnection);
                return;
            }
            if (kind == QLatin1String{GraphCanvasCollapseItem}) {
                event->accept();
                // Folding rebuilds the scene, which deletes the item this
                // press is standing on. Same reason as the breadcrumb: do it
                // from the event loop, not from inside the dispatch.
                const auto id = item->data(NodeGraphNameRole).toString().toStdString();
                QMetaObject::invokeMethod(
                    this, [this, scope, id] { setCollapsed(scope, id, !collapsed(scope, id)); },
                    Qt::QueuedConnection);
                return;
            }
            if (kind == QLatin1String{GraphCanvasSocketItem}) {
                event->accept();
                selectNode(scope, item->data(NodeGraphNameRole).toString().toStdString());
                beginConnection(*item);
                return;
            }
            // UX-5c: a folded node's output row (what a feature, the preset
            // or the rendering section writes, by name) starts a line that a
            // rendering-section pass's input takes -- the ray-traced mask onto
            // present's + input.
            if (kind == QLatin1String{GraphCanvasDerivedSocketItem} && scope.empty() &&
                model_ != nullptr && model_->currentScope().empty() &&
                !item->data(GraphCanvasPassInputRole).isValid() &&
                item->data(GraphCanvasSocketDirectionRole).toString() ==
                    QLatin1String{"output"} &&
                !item->data(GraphCanvasPortRole).toString().isEmpty()) {
                event->accept();
                beginConnection(*item);
                return;
            }
            if (kind == QLatin1String{GraphCanvasValuePillItem}) {
                event->accept();
                const auto node = item->data(NodeGraphNameRole).toString().toStdString();
                selectNode(scope, node);
                // UX-2, decision 4: a list opens at once, under the pill.
                if (beginValueEdit(scope, node,
                                   item->data(GraphCanvasPortRole).toString().toStdString()))
                    popUpInlineChoice();
                return;
            }
            // A control point is picked *and* dragged by the same press, so
            // the event is not accepted here: the base class still has to turn
            // it into a grab. Picking is the same work a node's press does,
            // and it is done in the same place.
            if (kind == QLatin1String{GraphCanvasRerouteItem}) {
                selectReroute(
                    scope,
                    GraphCanvasEdgeKey{
                        item->data(NodeGraphNameRole).toString().toStdString(),
                        item->data(GraphCanvasToPortRole).toString().toStdString()},
                    static_cast<std::size_t>(
                        item->data(GraphCanvasControlIndexRole).toULongLong()),
                    (event->modifiers() & Qt::ControlModifier) != 0);
                picked_control = true;
                break;
            }
            if (kind == QLatin1String{GraphCanvasTieItem}) {
                selectTie(scope, static_cast<std::size_t>(
                              item->data(GraphCanvasControlIndexRole).toULongLong()),
                          (event->modifiers() & Qt::ControlModifier) != 0);
                picked_control = true;
                break;
            }
            if (kind == QLatin1String{GraphCanvasEdgeItem} ||
                kind == QLatin1String{GraphCanvasEdgeArrowItem}) {
                event->accept();
                selectEdge(scope, GraphCanvasEdge{
                    item->data(NodeGraphFromNameRole).toString().toStdString(),
                    item->data(GraphCanvasFromPortRole).toString().toStdString(),
                    item->data(NodeGraphToNameRole).toString().toStdString(),
                    item->data(GraphCanvasToPortRole).toString().toStdString()},
                    (event->modifiers() & Qt::ControlModifier) != 0);
                return;
            }
            if (isNodeSurface(kind)) break;
        }
    }
    const auto generation = rebuild_generation_;
    QGraphicsScene::mousePressEvent(event);
    if (event->button() != Qt::LeftButton) return;
    // The press went to the breadcrumb and the scene is a different one now.
    if (generation != rebuild_generation_ || picked_control) return;
    for (QGraphicsItem *item : items(event->scenePos())) {
        if (!isNodeSurface(nodeGraphItemKind(*item))) continue;
        selectNode(itemScope(*item), item->data(NodeGraphNameRole).toString().toStdString());
        return;
    }
}

void GraphCanvasScene::mouseMoveEvent(QGraphicsSceneMouseEvent *event) {
    if (pending_connection_ && pending_edge_ != nullptr) {
        event->accept();
        followCursor(event->scenePos());
        return;
    }
    QGraphicsScene::mouseMoveEvent(event);
}

void GraphCanvasScene::mouseReleaseEvent(QGraphicsSceneMouseEvent *event) {
    if (pending_connection_) {
        event->accept();
        finishConnection(event->scenePos());
        return;
    }
    QGraphicsScene::mouseReleaseEvent(event);
}

void GraphCanvasScene::contextMenuEvent(QGraphicsSceneContextMenuEvent *event) {
    context_position_ = event->scenePos();
    // GL-1e, decision 2: a line into a rendering-section pass offers Delete.
    for (QGraphicsItem *item : items(event->scenePos())) {
        if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasPassInputEdgeItem}) continue;
        selectPassInput(item->data(NodeGraphToNameRole).toString().toStdString(),
                        static_cast<std::size_t>(
                            item->data(GraphCanvasPassInputRole).toLongLong()));
        QMenu menu;
        QAction *remove = menu.addAction(QObject::tr("Delete"));
        remove->setObjectName(QStringLiteral("pelican.graphCanvas.deletePassInput"));
        QObject::connect(remove, &QAction::triggered, this, [this] {
            QMetaObject::invokeMethod(
                this, [this] { deletePassInput(); }, Qt::QueuedConnection);
        });
        event->accept();
        menu.exec(event->screenPos());
        return;
    }
    // LP-4d1: opened inside a zone, the menu adds to the zone's body.
    const std::string scope = scopeAt(context_position_);
    const bool root = scope.empty();
    QMenu menu;
    // UX-3, decision 3: over a render node's input socket, how that port
    // reads -- what `ports` held as JSON -- comes first.
    if (model_ != nullptr)
        for (QGraphicsItem *item : items(event->scenePos())) {
            if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasSocketItem} ||
                item->data(GraphCanvasSocketDirectionRole).toString() != QLatin1String{"input"})
                continue;
            auto port = item->data(GraphCanvasBoundPortRole).toString().toStdString();
            if (port.empty()) port = item->data(GraphCanvasPortRole).toString().toStdString();
            const auto node = item->data(NodeGraphNameRole).toString().toStdString();
            addGraphCanvasSocketEntries(menu, *model_, itemScope(*item), node, port,
                                        [this] { afterEdit(true); });
            break;
        }
    // Both handlers run from the event loop rather than from inside the menu's
    // own: the palette is a modal dialog, and opening one while the menu is
    // still dispatching nests two modal loops.
    if (addNodeRequested) {
        QAction *add = menu.addAction(QObject::tr("Add node..."));
        QObject::connect(add, &QAction::triggered, this, [this, scope] {
            const QPointF at = context_position_;
            palette_scope_ = scope;
            QMetaObject::invokeMethod(
                this,
                [this, at] {
                    if (addNodeRequested) addNodeRequested(at);
                },
                Qt::QueuedConnection);
        });
    }
    // LP-4c: the named additions, where they apply -- a loop in the root, a
    // fullscreen pass anywhere the catalog has one.
    if (add_loop_action_ != nullptr && add_loop_action_->isEnabled() && root)
        menu.addAction(add_loop_action_);
    if (add_fullscreen_action_ != nullptr && model_ != nullptr && model_->canEdit() &&
        !model_->paletteOperationsIn(scope, "render.fullscreen").empty()) {
        add_fullscreen_action_->setEnabled(true);
        menu.addAction(add_fullscreen_action_);
    }
    // Decision 6: an input of a root loop can read one element of another
    // loop's p[].
    if (model_ != nullptr && perIterationRequested && model_->currentScope().empty()) {
        for (QGraphicsItem *item : items(event->scenePos())) {
            if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasSocketItem} &&
                nodeGraphItemKind(*item) != QLatin1String{GraphCanvasNodeLabelItem})
                continue;
            if (!itemScope(*item).empty()) continue;
            const auto port = item->data(GraphCanvasPortRole).toString().toStdString();
            const auto node = item->data(NodeGraphNameRole).toString().toStdString();
            if (port.empty()) continue;
            const auto loop = model_->loop(node);
            if (!loop || std::ranges::find(loop->body_inputs, port) == loop->body_inputs.end())
                continue;
            if (model_->perIterationSources(node).empty()) break;
            QAction *each = menu.addAction(QObject::tr("Per iteration…"));
            QObject::connect(each, &QAction::triggered, this, [this, node, port] {
                QMetaObject::invokeMethod(
                    this,
                    [this, node, port] {
                        if (perIterationRequested) perIterationRequested(node, port);
                    },
                    Qt::QueuedConnection);
            });
            break;
        }
    }
    // The two entries the owner named are the same objects the automation
    // surface fires: a scenario must not have to open a popup to reach them.
    if (auto_layout_action_ != nullptr) {
        menu.addAction(auto_layout_action_);
    } else {
        QAction *automatic = menu.addAction(QObject::tr("Auto layout"));
        QObject::connect(automatic, &QAction::triggered, this, [this] {
            QMetaObject::invokeMethod(
                this, [this] { autoLayout(); }, Qt::QueuedConnection);
        });
    }
    if (tie_action_ != nullptr || untie_action_ != nullptr) menu.addSeparator();
    if (tie_action_ != nullptr) menu.addAction(tie_action_);
    if (untie_action_ != nullptr) menu.addAction(untie_action_);
    if (fit_view_action_ != nullptr) {
        menu.addAction(fit_view_action_);
    } else if (fitViewRequested) {
        QAction *fit = menu.addAction(QObject::tr("Fit view"));
        fit->setShortcut(QKeySequence{Qt::Key_F});
        QObject::connect(fit, &QAction::triggered, this, [this] {
            QMetaObject::invokeMethod(
                this,
                [this] {
                    if (fitViewRequested) fitViewRequested();
                },
                Qt::QueuedConnection);
        });
    }
    // Q-19, decision 4: over the breadcrumb, the definition on screen; over a
    // zone's heading, the definition its tab shows.
    std::string definition;
    if (model_ != nullptr && rename_definition_action_ != nullptr &&
        delete_definition_action_ != nullptr)
        for (QGraphicsItem *item : items(event->scenePos())) {
            const QString kind = nodeGraphItemKind(*item);
            if (kind == QLatin1String{GraphCanvasBreadcrumbItem}) {
                definition = model_->currentScope();
                break;
            }
            if (!item->data(GraphCanvasHeadingRole).toBool() || !itemScope(*item).empty() ||
                !item->data(GraphCanvasZoneScopeRole).toString().isEmpty())
                continue;
            const auto found = zones_.find(item->data(NodeGraphNameRole).toString().toStdString());
            if (found != zones_.end()) definition = found->second;
            break;
        }
    if (!definition.empty()) {
        menu.addSeparator();
        if (definition_target_) definition_target_(definition);
        rename_definition_action_->setEnabled(model_->canEdit());
        delete_definition_action_->setEnabled(model_->canEdit());
        menu.addAction(rename_definition_action_);
        menu.addAction(delete_definition_action_);
    }
    if (menu.actions().isEmpty()) {
        QGraphicsScene::contextMenuEvent(event);
        return;
    }
    event->accept();
    menu_scope_ = scope;
    menu.exec(event->screenPos());
    menu_scope_.reset();
    if (!definition.empty() && definition_target_) definition_target_(std::nullopt);
}

// One editor at a time, over the thing it edits. It owns nothing the scene
// owns: the proxy is an item like any other, and clearing the scene ends it.
struct GraphCanvasScene::InlineEditor {
    enum class Kind { label, value };

    Kind kind = Kind::label;
    std::string scope;
    std::string node;
    std::string port;
    ValueEditorKind value_kind = ValueEditorKind::number;
    // UX-2: what a `choice` editor's items stand for (an item's data is its
    // index here; -1 is the document's own value when no choice is it).
    std::vector<GraphCanvasValueChoice> choices;
    QGraphicsProxyWidget *proxy = nullptr;
    QWidget *widget = nullptr;
    std::unique_ptr<InlineEditFilter> filter;
};

GraphCanvasScene::~GraphCanvasScene() {
    removeStudioThemeListener(theme_listener_);
    // The filter outlives nothing: an editor still open when the scene goes
    // must not be left watching a widget the base class is about to delete.
    if (editor_ && editor_->widget != nullptr && editor_->filter)
        editor_->widget->removeEventFilter(editor_->filter.get());
}

namespace {
QString editorText(const QWidget *widget);
} // namespace

std::optional<GraphCanvasScene::CarriedInlineEdit>
GraphCanvasScene::carryInlineEdit() const {
    if (!editor_) return std::nullopt;
    return CarriedInlineEdit{editor_->kind == InlineEditor::Kind::label,
                             editor_->scope, editor_->node, editor_->port,
                             editorText(editor_->widget)};
}

void GraphCanvasScene::restoreInlineEdit(const CarriedInlineEdit &carried) {
    const bool opened = carried.label
                            ? beginLabelEdit(carried.scope, carried.node)
                            : beginValueEdit(carried.scope, carried.node, carried.port);
    if (!opened) return;
    setInlineEditText(carried.text);
    if (editor_ && editor_->widget != nullptr)
        editor_->widget->setFocus(Qt::OtherFocusReason);
}

void GraphCanvasScene::closeEditor() {
    if (!editor_) return;
    auto editor = std::move(editor_);
    if (editor->widget != nullptr && editor->filter)
        editor->widget->removeEventFilter(editor->filter.get());
    if (editor->proxy != nullptr) {
        removeItem(editor->proxy);
        // The editor is often closed from inside its own event handler, so it
        // outlives this call by one trip through the event loop.
        editor->proxy->deleteLater();
    }
}

namespace {
// The item the editor has to cover, found by kind and by what it names.
QGraphicsItem *findHeadingLabel(QGraphicsScene &scene, const QString &scope,
                                const QString &node) {
    for (QGraphicsItem *item : scene.items()) {
        if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasNodeLabelItem}) continue;
        if (item->data(GraphCanvasScopeRole).toString() != scope) continue;
        if (item->data(NodeGraphNameRole).toString() != node) continue;
        if (!item->data(GraphCanvasHeadingRole).toBool()) continue;
        if (item->data(GraphCanvasLabelRole).toString().isEmpty()) continue;
        return item;
    }
    return nullptr;
}

QGraphicsItem *findPill(QGraphicsScene &scene, const QString &scope, const QString &node,
                        const QString &port) {
    for (QGraphicsItem *item : scene.items()) {
        if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasValuePillItem}) continue;
        if (item->data(GraphCanvasScopeRole).toString() != scope) continue;
        if (item->data(NodeGraphNameRole).toString() != node) continue;
        if (item->data(GraphCanvasPortRole).toString() != port) continue;
        if (item->data(GraphCanvasValueTextRole).toString().isEmpty()) continue;
        return item;
    }
    return nullptr;
}

QString editorText(const QWidget *widget) {
    if (const auto *line = qobject_cast<const QLineEdit *>(widget)) return line->text();
    if (const auto *combo = qobject_cast<const QComboBox *>(widget)) return combo->currentText();
    if (const auto *spin = qobject_cast<const QDoubleSpinBox *>(widget))
        return QString::number(spin->value(), 'g', 10);
    if (const auto *check = qobject_cast<const QCheckBox *>(widget))
        return check->isChecked() ? QStringLiteral("true") : QStringLiteral("false");
    return {};
}

void setEditorText(QWidget *widget, const QString &text) {
    if (auto *line = qobject_cast<QLineEdit *>(widget)) {
        line->setText(text);
        return;
    }
    if (auto *combo = qobject_cast<QComboBox *>(widget)) {
        if (const int index = combo->findText(text); index >= 0) combo->setCurrentIndex(index);
        return;
    }
    if (auto *spin = qobject_cast<QDoubleSpinBox *>(widget)) {
        spin->setValue(text.toDouble());
        return;
    }
    if (auto *check = qobject_cast<QCheckBox *>(widget))
        check->setChecked(text.compare(QLatin1String{"true"},
                                       Qt::CaseInsensitive) == 0 ||
                          text == QLatin1String{"1"});
}
} // namespace

bool GraphCanvasScene::beginLabelEdit(const std::string &id) {
    if (model_ == nullptr) return false;
    return beginLabelEdit(model_->currentScope(), id);
}

bool GraphCanvasScene::beginLabelEdit(const std::string &scope, const std::string &id) {
    if (model_ == nullptr || !model_->canEdit()) return false;
    const auto drawn = model_->nodes(scope);
    const auto found = std::ranges::find(drawn, id, &GraphCanvasNode::id);
    if (found == drawn.end()) return false;
    closeEditor();
    QGraphicsItem *anchor = findHeadingLabel(*this, qtext(scope), qtext(id));
    if (anchor == nullptr) return false;

    auto editor = std::make_unique<InlineEditor>();
    editor->kind = InlineEditor::Kind::label;
    editor->scope = scope;
    editor->node = id;
    auto *line = new QLineEdit;
    // The current label, not the heading: an unlabelled node starts empty, so
    // pressing Enter on it leaves it unlabelled rather than freezing its id in.
    line->setText(qtext(found->label));
    line->setPlaceholderText(qtext(found->id));
    line->selectAll();
    editor->widget = line;

    auto *proxy = addWidget(line);
    proxy->setZValue(6.0);
    const QRectF area = anchor->sceneBoundingRect();
    line->resize(std::max<int>(140, static_cast<int>(area.width() + 80)),
                 static_cast<int>(GraphCanvasHeaderHeight - 8.0));
    proxy->setPos(area.left() - 2.0, area.center().y() - line->height() / 2.0);
    proxy->setData(NodeGraphItemKindRole,
                   QString::fromLatin1(GraphCanvasEditorItem));
    proxy->setData(NodeGraphNameRole, qtext(id));
    // UX-5a: named as the heading it stands over is -- a body node drawn in a
    // zone has its definition in front of its id.
    proxy->setData(GraphCanvasZoneScopeRole, anchor->data(GraphCanvasZoneScopeRole));
    editor->proxy = proxy;
    editor->filter = std::make_unique<InlineEditFilter>([this](bool commit) {
        // The widget is still dispatching the event that ended the edit.
        QMetaObject::invokeMethod(
            this, [this, commit] {
                if (commit) commitInlineEdit();
                else cancelInlineEdit();
            },
            Qt::QueuedConnection);
    });
    line->installEventFilter(editor->filter.get());
    editor_ = std::move(editor);
    line->setFocus();
    return true;
}

bool GraphCanvasScene::beginValueEdit(const std::string &id, const std::string &port) {
    if (model_ == nullptr) return false;
    return beginValueEdit(model_->currentScope(), id, port);
}

bool GraphCanvasScene::beginValueEdit(const std::string &scope, const std::string &id,
                                      const std::string &port) {
    if (model_ == nullptr || !model_->canEdit() || port.empty()) return false;
    const auto drawn = model_->nodes(scope);
    const auto found = std::ranges::find(drawn, id, &GraphCanvasNode::id);
    if (found == drawn.end()) return false;
    const auto entry = std::ranges::find(found->inputs, port, &GraphCanvasPort::port);
    if (entry == found->inputs.end() || entry->connected) return false;
    closeEditor();
    QGraphicsItem *anchor = findPill(*this, qtext(scope), qtext(id), qtext(port));
    if (anchor == nullptr) return false;

    // What is being edited is the stored value, even when the pill is showing
    // the number a published generation fed the port: the document is the only
    // thing an edit can change.
    const nlohmann::json current =
        entry->has_parameter ? entry->parameter
        : entry->has_value   ? nlohmann::json(entry->value)
                             : nlohmann::json(0.0);
    auto editor = std::make_unique<InlineEditor>();
    editor->kind = InlineEditor::Kind::value;
    editor->scope = scope;
    editor->node = id;
    editor->port = port;
    editor->value_kind = editorKindFor(current);
    // UX-2, decision 4: a value with a list is picked from it.
    editor->choices = model_->valueChoices(scope, id, port);
    if (!editor->choices.empty()) editor->value_kind = ValueEditorKind::choice;
    switch (editor->value_kind) {
    case ValueEditorKind::choice: {
        auto *combo = new QComboBox;
        const auto parameters = model_->nodeParameters(scope, id);
        int selected = -1;
        for (std::size_t index = 0; index < editor->choices.size(); ++index) {
            combo->addItem(qtext(editor->choices[index].label), static_cast<int>(index));
            if (selected < 0 && editor->choices[index].parameters == parameters)
                selected = static_cast<int>(index);
        }
        if (selected < 0) {
            // The document's value is none of the choices: it is shown
            // first, as itself, and picking it changes nothing.
            combo->insertItem(0, current.is_string() ? qtext(current.get<std::string>())
                                                     : qtext(current.dump()),
                              -1);
            selected = 0;
        }
        combo->setCurrentIndex(selected);
        combo->setObjectName(QStringLiteral("pelican.graphCanvasValueChoice"));
        // A pick is the whole gesture.
        QObject::connect(combo, &QComboBox::activated, this, [this] {
            QMetaObject::invokeMethod(
                this, [this] { commitInlineEdit(); }, Qt::QueuedConnection);
        });
        editor->widget = combo;
        break;
    }
    case ValueEditorKind::boolean: {
        auto *check = new QCheckBox;
        check->setChecked(current.get<bool>());
        editor->widget = check;
        break;
    }
    case ValueEditorKind::number: {
        auto *spin = new QDoubleSpinBox;
        spin->setDecimals(6);
        spin->setRange(-1.0e9, 1.0e9);
        spin->setValue(current.get<double>());
        spin->setKeyboardTracking(false);
        editor->widget = spin;
        break;
    }
    case ValueEditorKind::text: {
        auto *line = new QLineEdit;
        line->setText(qtext(current.get<std::string>()));
        line->selectAll();
        editor->widget = line;
        break;
    }
    case ValueEditorKind::json: {
        auto *line = new QLineEdit;
        line->setText(qtext(current.dump()));
        line->selectAll();
        editor->widget = line;
        break;
    }
    }

    auto *proxy = addWidget(editor->widget);
    proxy->setZValue(6.0);
    const QRectF area = anchor->sceneBoundingRect();
    editor->widget->resize(
        std::max<int>(96, static_cast<int>(area.width() + 40)),
        std::max<int>(18, static_cast<int>(area.height() + 6.0)));
    proxy->setPos(area.right() - editor->widget->width(),
                  area.center().y() - editor->widget->height() / 2.0);
    proxy->setData(NodeGraphItemKindRole,
                   QString::fromLatin1(GraphCanvasEditorItem));
    proxy->setData(NodeGraphNameRole, qtext(id));
    proxy->setData(GraphCanvasPortRole, qtext(port));
    // UX-5a: named as the pill it opened from is -- a body node drawn in a
    // zone has its definition in front (`loop_body/render_fullscreen/shader`).
    proxy->setData(GraphCanvasZoneScopeRole, anchor->data(GraphCanvasZoneScopeRole));
    editor->proxy = proxy;
    editor->filter = std::make_unique<InlineEditFilter>([this](bool commit) {
        QMetaObject::invokeMethod(
            this, [this, commit] {
                if (commit) commitInlineEdit();
                else cancelInlineEdit();
            },
            Qt::QueuedConnection);
    });
    editor->widget->installEventFilter(editor->filter.get());
    editor_ = std::move(editor);
    editor_->widget->setFocus();
    return true;
}

void GraphCanvasScene::popUpInlineChoice() {
    if (!editor_) return;
    auto *combo = qobject_cast<QComboBox *>(editor_->widget);
    if (combo == nullptr) return;
    // From the event loop: the press that opened it is still being handled.
    QMetaObject::invokeMethod(
        combo, [combo] { combo->showPopup(); }, Qt::QueuedConnection);
}

QString GraphCanvasScene::inlineEditText() const {
    return editor_ ? editorText(editor_->widget) : QString{};
}

void GraphCanvasScene::setInlineEditText(const QString &text) {
    if (editor_) setEditorText(editor_->widget, text);
}

void GraphCanvasScene::cancelInlineEdit() { closeEditor(); }

bool GraphCanvasScene::commitInlineEdit() {
    if (!editor_ || model_ == nullptr) return false;
    const auto kind = editor_->kind;
    const auto scope = editor_->scope;
    const auto node = editor_->node;
    const auto port = editor_->port;
    const auto value_kind = editor_->value_kind;
    // A spin box with keyboard tracking off holds the value it was given until
    // something asks it to read what was typed -- normally the Return key or
    // losing focus. The editor's filter consumes Return before the spin box
    // sees it, so ask here: otherwise Enter reports "saved" and stores the
    // number the user just replaced.
    if (auto *spin = qobject_cast<QAbstractSpinBox *>(editor_->widget))
        spin->interpretText();
    const auto text = editorText(editor_->widget);
    if (value_kind == ValueEditorKind::choice) {
        // UX-2, decision 4: the pick's whole parameters, one
        // set_node_parameters. The document's own value sends nothing.
        const auto *combo = qobject_cast<const QComboBox *>(editor_->widget);
        const int index = combo != nullptr ? combo->currentData().toInt() : -1;
        std::optional<nlohmann::json> picked;
        if (index >= 0 && static_cast<std::size_t>(index) < editor_->choices.size())
            picked = editor_->choices[static_cast<std::size_t>(index)].parameters;
        closeEditor();
        if (!picked || *picked == model_->nodeParameters(scope, node)) return false;
        const bool submitted = model_->setNodeParameters(scope, node, *picked);
        afterEdit(submitted);
        return submitted;
    }
    closeEditor();
    if (kind == InlineEditor::Kind::label) {
        const bool submitted = model_->setNodeLabel(scope, node, text.toStdString());
        afterEdit(submitted);
        return submitted;
    }
    nlohmann::json value;
    switch (value_kind) {
    case ValueEditorKind::boolean:
        value = text == QLatin1String{"true"};
        break;
    case ValueEditorKind::number: {
        bool numeric = false;
        const double parsed = text.toDouble(&numeric);
        if (!numeric) return false;
        value = parsed;
        break;
    }
    case ValueEditorKind::text:
        value = text.toStdString();
        break;
    case ValueEditorKind::json: {
        const auto bytes = text.toStdString();
        if (!nlohmann::json::accept(bytes)) return false;
        value = nlohmann::json::parse(bytes);
        break;
    }
    case ValueEditorKind::choice:
        return false;
    }
    const bool submitted = model_->setNodeParameter(scope, node, port, value);
    afterEdit(submitted);
    return submitted;
}

void GraphCanvasScene::placeNewNode(const std::string &scope, const std::string &id,
                                    const std::string &op) {
    if (model_ == nullptr || id.empty()) return;
    last_added_ = std::pair{scope, id};
    const std::string current = model_->currentScope();
    // A box drawn on the canvas, in the scope's own coordinates (a zone's
    // body is drawn inside an item).
    const auto scopeRect = [&](const QGraphicsItem &item) {
        const QRectF drawn = item.sceneBoundingRect();
        return QRectF{toScopePoint(scope, drawn.topLeft()), toScopePoint(scope, drawn.bottomRight())};
    };
    // Beside the selected node when there is one, otherwise where the canvas
    // was right-clicked. Either way it is session state, like every position.
    QPointF position = toScopePoint(scope, context_position_);
    const bool from_menu = menu_scope_.has_value() || palette_scope_.has_value();
    if (selected_node_ && selection_scope_ == scope) {
        // LP-4f1, decision 5: past the selected node's real right edge -- a
        // zone's frame is as wide as its body, not a node's 240.
        if (QGraphicsItem *item = findNodeItem(*this, graphCanvasKinds(), qtext(scope),
                                               qtext(*selected_node_))) {
            const QRectF box = scopeRect(*item);
            position = QPointF{box.right() + NodeGraphHorizontalGap, box.top()};
        } else if (const auto found = session_node_positions_.find({scope, *selected_node_});
                   found != session_node_positions_.end()) {
            position = found->second +
                       QPointF{NodeGraphNodeWidth + NodeGraphHorizontalGap, 0.0};
        }
    } else if (scope != current && !from_menu) {
        // LP-4d1, decision 6: added to a zone's body from no particular
        // place, it is placed inside the frame with the rest of the body.
        return;
    }
    // The boxes already drawn in the scope: the new one is moved down past
    // any it would land on. Its own size is not known until it is drawn, so
    // it is taken as a node of a few rows, or a zone's least frame.
    // Those the automatic placement put where they are stay there: a new
    // node takes a column of its own in that placement and would push the
    // rest aside, into the place it was given.
    std::vector<QRectF> boxes;
    const GraphCanvasLayout &placed = layout(scope);
    // UX-5a: everything drawn in the scope, the read-only folded nodes too,
    // each with what it holds (GL-1c's bounds).
    std::optional<QRectF> bounds;
    bool anchored = false;
    std::vector<std::pair<std::string, QPointF>> folded;
    for (QGraphicsItem *item : items()) {
        if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasNodeItem} ||
            item->data(GraphCanvasScopeRole).toString() != qtext(scope))
            continue;
        const auto name = item->data(NodeGraphNameRole).toString().toStdString();
        if (name == id) continue;
        boxes.push_back(scopeRect(*item));
        const QRectF held =
            item->sceneBoundingRect().united(item->mapRectToScene(item->childrenBoundingRect()));
        const QRectF drawn{toScopePoint(scope, held.topLeft()), toScopePoint(scope, held.bottomRight())};
        bounds = bounds ? bounds->united(drawn) : drawn;
        if (item->data(GraphCanvasDerivedRole).toBool()) {
            folded.emplace_back(name, item->pos());
            continue;
        }
        if (placed.nodes.contains(name)) anchored = true;
        else session_node_positions_.try_emplace({scope, name}, item->pos());
    }
    // UX-5a: until the document places a node, the folded nodes follow the
    // one placement, which gives the new node a column of its own and moves
    // them onto the place it was given. They stay where they stand, while
    // they stay clear of the graph (GL-1c); Auto layout places them again.
    if (scope.empty() && !anchored)
        for (const auto &[name, at] : folded)
            added_folded_places_.insert_or_assign(foldedPositionKey(name), at);
    // UX-5a: a new loop from no particular node goes below all of it, the
    // folded nodes included: a zone grows with its body, right and down,
    // into nothing.
    if (op == "graph.loop" && !(selected_node_ && selection_scope_ == scope) && bounds)
        position = QPointF{bounds->left(), bounds->bottom() + NodeGraphVerticalGap};
    const QSizeF size =
        op == "graph.loop"
            ? QSizeF{GraphCanvasZoneMinimumWidth,
                     GraphCanvasHeaderHeight + 2.0 * GraphCanvasZonePadding +
                         GraphCanvasZoneMinimumContentHeight}
            : QSizeF{GraphCanvasNodeMinimumWidth, GraphCanvasHeaderHeight +
                                                      2.0 * GraphCanvasSectionPadding +
                                                      4.0 * GraphCanvasSocketRowHeight};
    QRectF box{position, size};
    const qreal margin = GraphCanvasSectionPadding;
    for (std::size_t guard = 0; guard <= boxes.size(); ++guard) {
        const auto hit = std::ranges::find_if(boxes, [&](const QRectF &other) {
            return box.adjusted(-margin, -margin, margin, margin).intersects(other);
        });
        if (hit == boxes.end()) break;
        box.moveTop(hit->bottom() + NodeGraphVerticalGap / 2.0);
    }
    session_node_positions_[{scope, id}] = box.topLeft();
}

bool GraphCanvasScene::addFromPalette(const std::string &op,
                                      const std::string &definition) {
    return addFromPalette(addScope(), op, definition);
}

bool GraphCanvasScene::addFromPalette(const std::string &scope, const std::string &op,
                                      const std::string &definition, const nlohmann::json &seed) {
    if (model_ == nullptr || !model_->canEdit()) return false;
    // The id the model is about to choose is the one the position is filed
    // under: asking twice would answer differently once the node exists.
    const auto ops = model_->addNodeOpsIn(scope, op, definition, seed);
    if (ops.empty()) return false;
    // The node the gesture is about is the last one added; LP-4c's list can
    // end with the loop that runs the scope being given a place.
    std::string added;
    std::string added_scope = scope;
    for (const auto &entry : ops)
        if (entry.value("op", std::string{}) == "add_node") {
            added = entry.at("node").value("id", std::string{});
            const auto token = entry.value("scope", std::string{"root"});
            added_scope = token == "root" ? std::string{} : token;
        }
    if (!model_->addNodeIn(scope, op, definition, seed)) return false;
    placeNewNode(added_scope, added, added_scope == scope ? op : std::string{});
    afterEdit(true);
    return true;
}

bool GraphCanvasScene::addLoop(const std::string &body, const std::string &new_definition) {
    if (model_ == nullptr || !model_->canEdit()) return false;
    const auto ops = model_->addLoopOps(body, new_definition);
    if (ops.empty()) return false;
    const auto &added = ops.back();
    if (!model_->addLoop(body, new_definition)) return false;
    placeNewNode({}, added.at("node").value("id", std::string{}), "graph.loop");
    afterEdit(true);
    return true;
}

void GraphCanvasScene::setAddActions(QAction *add_loop, QAction *add_fullscreen) {
    add_loop_action_ = add_loop;
    add_fullscreen_action_ = add_fullscreen;
}

void GraphCanvasScene::setDefinitionActions(
    QAction *rename, QAction *remove, std::function<void(std::optional<std::string>)> target) {
    rename_definition_action_ = rename;
    delete_definition_action_ = remove;
    definition_target_ = std::move(target);
}

bool GraphCanvasScene::deleteSelection() {
    if (model_ == nullptr) return false;
    // LP-4d2, decision 3: a picked carry arrow leaves the loop's carry.
    if (selected_carry_)
        return model_->removeLoopCarry(selected_carry_->loop, selected_carry_->from,
                                       selected_carry_->to);
    if (selected_edge_)
        return model_->disconnectInput(selection_scope_, selected_edge_->to_node,
                                       selected_edge_->to_port);
    if (selected_node_) return model_->removeNode(selection_scope_, *selected_node_);
    return false;
}

void GraphCanvasScene::keyPressEvent(QKeyEvent *event) {
    // Text widgets keep their own Undo stack. Only a canvas gesture belongs
    // to graph history, and a drag must finish saving before it can be undone.
    if (!editingInline() && event->matches(QKeySequence::Undo)) {
        undoEdit();
        event->accept();
        return;
    }
    if (!editingInline() && event->matches(QKeySequence::Redo)) {
        redoEdit();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        // A control point is placement, not document: removing one sends a
        // `set_layout` when the wait runs out, and no graph edit at all.
        // GL-1e: a picked line into a rendering-section pass.
        if (selected_pass_input_ && deletePassInput()) {
            event->accept();
            return;
        }
        if (deleteControlSelection()) {
            event->accept();
            return;
        }
        if (deleteSelection()) {
            event->accept();
            afterEdit(true);
            return;
        }
    }
    QGraphicsScene::keyPressEvent(event);
}

void GraphCanvasScene::undoEdit() {
    if (!model_ || editingInline() || dragging() || layoutPending()) return;
    // GL-1e: a pass's input edit this tab sent went to the render edit
    // history (RU-1), and the tab's Undo takes it back there while it is
    // the tab's last edit. Any other render edit stays the Frame Plan's and
    // the Pass Editor's to undo: here Ctrl+Z is the graph's (RU-1's keys).
    if (render_edits_ > 0 && renderUndo && renderUndoAvailable && renderUndoAvailable()) {
        if (renderUndo()) {
            --render_edits_;
            ++render_undone_;
        }
        return;
    }
    afterEdit(model_->undo());
}

void GraphCanvasScene::redoEdit() {
    if (!model_ || editingInline() || dragging() || layoutPending()) return;
    if (render_undone_ > 0 && renderRedo && renderRedoAvailable && renderRedoAvailable()) {
        if (renderRedo()) {
            --render_undone_;
            ++render_edits_;
        }
        return;
    }
    afterEdit(model_->redo());
}

void GraphCanvasScene::noteRenderEdit() {
    ++render_edits_;
    render_undone_ = 0;
}

bool GraphCanvasScene::canUndoAny() const {
    return model_ != nullptr &&
           (model_->canUndo() ||
            (render_edits_ > 0 && renderUndoAvailable && renderUndoAvailable()));
}

bool GraphCanvasScene::canRedoAny() const {
    return model_ != nullptr &&
           (model_->canRedo() ||
            (render_undone_ > 0 && renderRedoAvailable && renderRedoAvailable()));
}

// --- GL-1e: a pass of the rendering section takes lines.

FramePlanAuthoring GraphCanvasScene::passAuthoring() const {
    if (model_ == nullptr) return {};
    const auto revision = model_->authoringRevision();
    if (!pass_authoring_ || pass_authoring_->first != revision)
        pass_authoring_ = std::pair{
            revision, framePlanAuthoringFromContext(model_->renderAuthoringContext())};
    return pass_authoring_->second;
}

std::optional<FramePlanInputSockets> GraphCanvasScene::passInputSockets(
    const std::string &pass) const {
    if (model_ == nullptr || !model_->currentScope().empty() || model_->foldedView())
        return std::nullopt;
    // A pass of the rendering section: the project's, which no graph node
    // made and no feature brought, drawn as a folded node of its own.
    const auto &passes = model_->planPasses();
    const auto found = std::ranges::find(passes, pass, &GraphCanvasPlanPass::name);
    if (found == passes.end() || found->source != "project" || !found->feature.empty() ||
        model_->canvasNodeOfPass(pass) != pass)
        return std::nullopt;
    // Decision 1: one the root declares (F-6), fullscreen. Preset and
    // feature passes need a fork first and take no lines here.
    const auto authoring = passAuthoring();
    const auto &graph = model_->planGraph();
    const auto *row = authoring.find(graph, pass);
    if (row == nullptr || !row->root ||
        row->editable_declaration.value("type", std::string{}) != "fullscreen")
        return std::nullopt;
    const auto plan = model_->passInputPlan();
    const auto node = std::ranges::find(plan.nodes, pass, &FramePlanNode::name);
    if (node == plan.nodes.end()) return std::nullopt;
    const auto inputs = framePlanPassInputList(authoring, graph, pass);
    auto sockets = framePlanInputSockets(plan, *node, inputs);
    if (sockets.declared) return sockets;
    // A shader whose declarations the plan does not publish: the pass's
    // `input` in order, with nothing to add after it.
    FramePlanInputSockets listed;
    if (inputs)
        for (std::size_t index = 0; index < inputs->size(); ++index) {
            FramePlanInputSocket socket;
            socket.kind = FramePlanInputSocket::Kind::undeclared;
            const auto &input = inputs->at(index);
            const auto at = input.find('@');
            socket.resource = input.substr(0, at);
            socket.history = at != std::string::npos;
            socket.input_index = index;
            socket.label = input;
            listed.sockets.push_back(std::move(socket));
        }
    return listed;
}

FramePlanEditRequest GraphCanvasScene::passInputConnectRequest(
    const std::string &pass, const std::string &resource,
    const FramePlanSocketDrop &drop) const {
    if (model_ == nullptr) return {};
    return framePlanSocketConnectRequest(passAuthoring(), model_->passInputPlan(),
                                         model_->planGraph(), pass, resource, drop);
}

FramePlanEditRequest GraphCanvasScene::passInputDisconnectRequest(
    const std::string &pass, std::size_t input_index) const {
    if (model_ == nullptr) return {};
    return framePlanSocketDisconnectRequest(passAuthoring(), model_->passInputPlan(),
                                            model_->planGraph(), pass, input_index);
}

void GraphCanvasScene::selectPassInput(const std::string &pass, std::size_t input_index) {
    selected_pass_input_ = PassInputPick{pass, input_index};
    selected_node_.reset();
    selected_edge_.reset();
    selected_edges_.clear();
    setProperty("pelicanGraphSelectedNode", QString{});
    applyEdgeEmphasis();
    for (QGraphicsItem *item : items()) {
        if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasPassInputEdgeItem}) continue;
        auto *line = dynamic_cast<QGraphicsPathItem *>(item);
        if (line == nullptr) continue;
        const bool picked =
            item->data(NodeGraphToNameRole).toString() == qtext(pass) &&
            item->data(GraphCanvasPassInputRole).toLongLong() ==
                static_cast<qlonglong>(input_index);
        line->setPen(picked ? QPen{currentStudioTheme().canvas.edge_selected, 2.4}
                            : QPen{currentStudioTheme().canvas.edge, 1.8});
    }
    if (selectionChanged) selectionChanged();
}

bool GraphCanvasScene::deletePassInput() {
    if (!selected_pass_input_ || model_ == nullptr) return false;
    const auto pick = *std::exchange(selected_pass_input_, std::nullopt);
    handPassInputEdit(passInputDisconnectRequest(pick.pass, pick.input_index),
                      [this, pick] {
                          return passInputDisconnectRequest(pick.pass, pick.input_index);
                      });
    return true;
}

void GraphCanvasScene::dropOnPassInput(const std::string &pass, const QGraphicsItem &socket,
                                       const std::string &from_node,
                                       const std::string &from_port) {
    const auto resource = model_->outputResource(from_node, from_port);
    if (resource.empty()) {
        // UX-5c: a loop's `p[]` is one value per element; a pass reads its
        // `p`, the last element's.
        const bool per_element =
            from_port.ends_with("[]") && model_->loop(from_node).has_value();
        model_->setEditNotice(
            {InspectorNoticeKind::Error,
             per_element
                 ? from_node + "." + from_port + " holds one image per element, which " + pass +
                       " cannot read: a pass reads a loop's single output (" + from_node + "." +
                       from_port.substr(0, from_port.size() - 2) +
                       ", the last element's)."
                 : from_node + "." + from_port + " is not an image or a buffer " + pass +
                       " can read."});
        if (noticeChanged) noticeChanged();
        return;
    }
    dropResourceOnPassInput(pass, socket, resource);
}

void GraphCanvasScene::dropResourceOnPassInput(const std::string &pass,
                                               const QGraphicsItem &socket,
                                               const std::string &resource) {
    // Where it landed, in the Frame Plan's terms: the `+ input` slot, a
    // socket by its binding (in place of what is connected there), or the
    // pass as a whole (a shader whose declarations are not published).
    FramePlanSocketDrop drop;
    const auto key = socket.data(GraphCanvasSocketKeyRole).toString();
    if (key == QLatin1String{"in:variadic"}) {
        drop.kind = FramePlanSocketDrop::Kind::variadic;
    } else if (key.startsWith(QLatin1String{"in@"})) {
        bool number = false;
        const auto binding = key.mid(3).toULongLong(&number);
        if (number) {
            drop.kind = FramePlanSocketDrop::Kind::socket;
            drop.binding = static_cast<std::size_t>(binding);
        }
    }
    handPassInputEdit(passInputConnectRequest(pass, resource, drop),
                      [this, pass, resource, drop] {
                          return passInputConnectRequest(pass, resource, drop);
                      });
}

void GraphCanvasScene::handPassInputEdit(const FramePlanEditRequest &request,
                                         std::function<FramePlanEditRequest()> rebuild) {
    if (!request.sendable()) {
        const auto line = !request.line.empty()      ? request.line
                          : !request.refusal.empty() ? request.refusal
                                                     : std::string{"Nothing to change."};
        model_->setEditNotice({InspectorNoticeKind::Error, line});
        if (noticeChanged) noticeChanged();
        return;
    }
    if (passInputEditRequested) passInputEditRequested(request, std::move(rebuild));
}

void GraphCanvasScene::leaveCurrentScope() {
    if (model_ == nullptr || !model_->leaveScope()) return;
    repopulate();
    if (scopeChanged) scopeChanged();
}

void GraphCanvasScene::enterInstanceScope(const std::string &id) {
    if (model_ == nullptr || !model_->enterInstance(id)) return;
    repopulate();
    if (scopeChanged) scopeChanged();
}

bool GraphCanvasScene::enterFolded(const std::string &id) {
    if (model_ == nullptr) return false;
    // A drag of the real root still in the air goes before the view does.
    flushLayout();
    if (!model_->enterFolded(id)) return false;
    repopulate();
    if (scopeChanged) scopeChanged();
    return true;
}

bool GraphCanvasScene::showDecomposed() {
    if (model_ == nullptr || !model_->showDecomposed()) return false;
    repopulate();
    if (scopeChanged) scopeChanged();
    return true;
}

void GraphCanvasScene::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) {
    if (model_ != nullptr && event->button() == Qt::LeftButton) {
        // GL-1, decision 3: a folded node is looked inside, read only. Its
        // heading renames nothing, and a line the plan implies takes no
        // control point.
        for (QGraphicsItem *item : items(event->scenePos())) {
            const QString kind = nodeGraphItemKind(*item);
            if (kind == QLatin1String{GraphCanvasEditorItem}) break;
            if (!item->data(GraphCanvasDerivedRole).toBool()) {
                if (isNodeSurface(kind)) break;
                continue;
            }
            event->accept();
            if (kind == QLatin1String{GraphCanvasDerivedEdgeItem}) return;
            const auto id = item->data(NodeGraphNameRole).toString().toStdString();
            QMetaObject::invokeMethod(
                this, [this, id] { enterFolded(id); }, Qt::QueuedConnection);
            return;
        }
        // N-6 first: a control point and a bundle are drawn over the lines and
        // under the nodes, so they are the topmost thing a double-click on one
        // of them can mean.
        for (QGraphicsItem *item : items(event->scenePos())) {
            const QString kind = nodeGraphItemKind(*item);
            if (kind == QLatin1String{GraphCanvasTieItem}) {
                event->accept();
                const auto index =
                    static_cast<std::size_t>(
                        item->data(GraphCanvasControlIndexRole).toULongLong());
                const std::string scope = itemScope(*item);
                QMetaObject::invokeMethod(
                    this,
                    [this, scope, index] {
                        auto &ties = placement(scope).layout.ties;
                        if (index >= ties.size()) return;
                        std::ranges::reverse(ties[index].edges);
                        noteLayoutChange(scope);
                        repopulate();
                    },
                    Qt::QueuedConnection);
                return;
            }
            if (kind == QLatin1String{GraphCanvasRerouteItem}) {
                event->accept();
                return;
            }
            if (kind == QLatin1String{GraphCanvasEdgeItem} ||
                kind == QLatin1String{GraphCanvasEdgeArrowItem}) {
                event->accept();
                const GraphCanvasEdge edge{
                    item->data(NodeGraphFromNameRole).toString().toStdString(),
                    item->data(GraphCanvasFromPortRole).toString().toStdString(),
                    item->data(NodeGraphToNameRole).toString().toStdString(),
                    item->data(GraphCanvasToPortRole).toString().toStdString()};
                const QPointF at = event->scenePos();
                const std::string scope = itemScope(*item);
                // Same reason as the breadcrumb: the item under the cursor
                // cannot be deleted while Qt is still dispatching this event.
                QMetaObject::invokeMethod(
                    this, [this, scope, edge, at] { insertReroute(scope, edge, at); },
                    Qt::QueuedConnection);
                return;
            }
            if (isNodeSurface(kind)) break;
        }
        // The heading is the node's name, so double-clicking it renames the
        // node. Anywhere else on the node still means "show me what is in it".
        for (QGraphicsItem *item : items(event->scenePos())) {
            if (nodeGraphItemKind(*item) == QLatin1String{GraphCanvasEditorItem}) break;
            if (!item->data(GraphCanvasHeadingRole).toBool()) continue;
            event->accept();
            beginLabelEdit(itemScope(*item), item->data(NodeGraphNameRole).toString().toStdString());
            return;
        }
        for (QGraphicsItem *item : items(event->scenePos())) {
            if (!isNodeSurface(nodeGraphItemKind(*item))) continue;
            const auto id = item->data(NodeGraphNameRole).toString().toStdString();
            // Only the scope on screen descends: a zone's body holds neither
            // an instance nor a loop.
            if (itemScope(*item) != model_->currentScope() || !model_->canEnterInstance(id)) break;
            event->accept();
            // Same reason as the breadcrumb: the items under the cursor cannot
            // be deleted while Qt is still dispatching this event.
            QMetaObject::invokeMethod(
                this, [this, id] { enterInstanceScope(id); },
                Qt::QueuedConnection);
            return;
        }
    }
    QGraphicsScene::mouseDoubleClickEvent(event);
}

namespace {

// AG-3b, 決定 3: the read-only time strip under the canvas.
//
// Everything it draws is client-side. AG-3a publishes a snapshot of the
// current generation and no history at all, so the only honest strip over time
// is what this session watched go by -- which is why the buffer lives in the
// model and this widget only draws it. The one thing it writes is the scrub:
// a press on the axis is a time, and a time is set_time plus one drawn frame.
class GraphTimelineStrip final : public QWidget {
  public:
    using Scrub = std::function<void(double time)>;

    explicit GraphTimelineStrip(const GraphCanvasModel &model, Scrub scrub,
                                QWidget *parent = nullptr)
        : QWidget{parent}, model_{model}, scrub_{std::move(scrub)} {
        setObjectName(QStringLiteral("pelican.graphTimeline"));
        setMinimumHeight(96);
        setScrubBlocked({});
    }

    // PM-3a: while the game plays, the strip is read-only and says why.
    void setScrubBlocked(const QString &reason) {
        setToolTip(reason.isEmpty()
                       ? tr("Scene time, left to right. Drag to go to a time.")
                       : reason);
        if (reason.isEmpty())
            unsetCursor();
        else
            setCursor(Qt::ForbiddenCursor);
    }

    // The window the strip draws, in scene seconds. Empty buffer: nothing has
    // been watched, so there is no span and no scrub.
    [[nodiscard]] bool span(double *from, double *to) const {
        const auto &samples = model_.timelineSamples();
        if (samples.empty()) return false;
        *from = samples.front().time;
        *to = samples.back().time;
        // A run that has not moved yet is one point, and a point has no
        // inside: give it a second of width so the strip is drivable rather
        // than dividing by zero.
        if (!(*to > *from)) *to = *from + 1.0;
        return true;
    }

    // What one x in this widget means. Public so a test can ask the strip what
    // it would do rather than deducing it from the pixels.
    [[nodiscard]] std::optional<double> timeAt(int x) const {
        double from = 0;
        double to = 0;
        if (!span(&from, &to) || width() <= 1) return std::nullopt;
        const double ratio =
            std::clamp(static_cast<double>(x) / static_cast<double>(width() - 1),
                       0.0, 1.0);
        return from + ratio * (to - from);
    }

  protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        emitScrub(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if ((event->buttons() & Qt::LeftButton) == 0) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        emitScrub(event);
    }

    void paintEvent(QPaintEvent *) override {
        const StudioTheme &theme = currentStudioTheme();
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), theme.canvas.node_body);
        painter.setPen(QPen{theme.canvas.node_border, 1.0});
        painter.drawRect(rect().adjusted(0, 0, -1, -1));

        const auto &samples = model_.timelineSamples();
        const auto &ports = model_.timelinePorts();
        if (samples.size() < 2 || ports.empty()) {
            painter.setPen(QPen{theme.canvas.dim_text});
            painter.drawText(rect(), Qt::AlignCenter,
                             model_.live()
                                 ? tr("Waiting for readings...")
                                 : tr("Turn Live on to follow a node over time."));
            return;
        }
        double from = 0;
        double to = 0;
        if (!span(&from, &to)) return;

        // The animation band sits along the bottom; the traces take the rest.
        const qreal band_height = 14.0;
        const QRectF plot{4.0, 4.0, width() - 8.0,
                          height() - 8.0 - band_height - 4.0};
        const QRectF band{4.0, plot.bottom() + 4.0, width() - 8.0, band_height};

        double lowest = samples.front().values.front();
        double highest = lowest;
        for (const auto &sample : samples)
            for (const double value : sample.values) {
                lowest = std::min(lowest, value);
                highest = std::max(highest, value);
            }
        // A flat trace is a fact about the graph, not a reason to divide by
        // zero: it is drawn down the middle of the strip.
        const double range = highest > lowest ? highest - lowest : 1.0;
        const double base = highest > lowest ? lowest : lowest - 0.5;

        const auto xOf = [&](double time) {
            return plot.left() + plot.width() * (time - from) / (to - from);
        };
        for (std::size_t trace = 0; trace < ports.size(); ++trace) {
            QPolygonF line;
            line.reserve(static_cast<int>(samples.size()));
            for (const auto &sample : samples) {
                if (trace >= sample.values.size()) continue;
                const double normalized = (sample.values[trace] - base) / range;
                line.push_back(QPointF{
                    xOf(sample.time),
                    plot.bottom() - plot.height() * normalized});
            }
            painter.setPen(QPen{theme.domainTint(traceName(trace)), 1.6});
            painter.drawPolyline(line);
        }

        // The sink band: the clip's phase, and the transition's progress when
        // one is running. Both are already 0..1, so the band is their own
        // scale rather than the traces'.
        painter.setPen(Qt::NoPen);
        for (const auto &sample : samples) {
            if (!sample.has_phase && !sample.has_progress) continue;
            const qreal x = xOf(sample.time);
            const double height_ratio =
                sample.has_progress ? sample.progress : sample.phase;
            QColor tint = theme.domainTint(
                sample.has_progress ? QStringLiteral("instance")
                                    : QStringLiteral("animation"));
            tint.setAlphaF(0.85F);
            painter.fillRect(
                QRectF{x, band.bottom() - band.height() * height_ratio, 2.0,
                       band.height() * height_ratio},
                tint);
        }
        painter.setPen(QPen{theme.canvas.node_border, 1.0});
        painter.drawLine(band.topLeft(), band.topRight());
    }

  private:
    // Four fixed hues, taken from the theme's own domain table so the strip
    // belongs to the same palette as the nodes.
    static QString traceName(std::size_t index) {
        switch (index % 4) {
        case 0: return QStringLiteral("values");
        case 1: return QStringLiteral("rendering");
        case 2: return QStringLiteral("animation");
        default: return QStringLiteral("instance");
        }
    }

    void emitScrub(QMouseEvent *event) {
        const auto time = timeAt(static_cast<int>(event->position().x()));
        if (time && scrub_) scrub_(*time);
        event->accept();
    }

    const GraphCanvasModel &model_;
    Scrub scrub_;
};

// GL-1d, decision 5: the palette's filter keeps the keyboard while the list
// is walked. Up, Down and the page keys go to the list; everything else is
// the filter's (Enter is OK and Esc is Cancel, as the dialog has them).
class PaletteFilterKeys final : public QObject {
public:
    PaletteFilterKeys(QListWidget *list, QObject *parent) : QObject(parent), list_(list) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::KeyPress) return QObject::eventFilter(watched, event);
        switch (static_cast<QKeyEvent *>(event)->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            QCoreApplication::sendEvent(list_, event);
            return true;
        default:
            return QObject::eventFilter(watched, event);
        }
    }

private:
    QListWidget *list_;
};

} // namespace

// GL-1e, decision 3: what an engine before RS-3 refusing a pass's input
// edit is told besides its own sentence (it placed the graph's pass after
// the rendering section's pass that reads it).
constexpr std::string_view passRunsAfterHint =
    " \xE2\x80\x94 the graph pass that draws it runs after this pass";

struct GraphCanvasWidget::Impl {
    GraphCanvasWidget &view;
    GraphCanvasReadCapability graph;
    GraphCanvasModel model;
    // Q-13: the digest the panels that edit the document share, and whether
    // another panel moved the document while this tab was busy.
    std::shared_ptr<RenderConfigDigest> digest_hub;
    bool document_reread = false;
    GraphCanvasScene *scene = nullptr;
    NodeCanvasView *canvas = nullptr;
    QComboBox *scopes = nullptr;
    QComboBox *value_instances = nullptr;
    QPushButton *refresh = nullptr;
    QCheckBox *live = nullptr;
    QPushButton *timeline_toggle = nullptr;
    QLabel *timeline_summary = nullptr;
    GraphTimelineStrip *timeline = nullptr;
    QWidget *timeline_panel = nullptr;
    QLabel *breadcrumb = nullptr;
    QLabel *notice = nullptr;
    QTreeWidget *details = nullptr;
    // UX-2, decision 5: the parameter row of the details whose value is
    // being typed as JSON, and the node it belongs to. A redraw waits for it,
    // as it waits for the canvas's own editor.
    QTreeWidgetItem *editing_parameter = nullptr;
    std::string editing_parameter_scope;
    std::string editing_parameter_node;
    // LP-4c: the Loop fields for a selected root loop (decisions 3, 4), and,
    // inside a loop's body, the branch it shows and whether it is shared.
    GraphCanvasLoopFields *loop_fields = nullptr;
    // LP-4e: a material pass's contract, a draw step's variant or override.
    GraphCanvasDrawFields *draw_fields = nullptr;
    // LP-4f1: a render node's View field.
    GraphCanvasViewFields *view_fields = nullptr;
    // UX-2: a render.image's values (decision 1) and a render pass's
    // shaders (decision 2), picked rather than typed.
    GraphCanvasImageFields *image_fields = nullptr;
    GraphCanvasPassFields *pass_fields = nullptr;
    // UX-3, decision 2: where a render node lands.
    GraphCanvasPlaceFields *place_fields = nullptr;
    QComboBox *loop_branch = nullptr;
    QLabel *shared_mark = nullptr;
    // GL-1, decision 5: Show: one box per domain the root canvas can draw.
    std::vector<std::pair<std::string, QCheckBox *>> domain_boxes;
    // Decision 4: the line an edit inside a folded view gets, with its
    // "Expand to edit…" link (the named action), and the owner's two flows.
    QLabel *folded_notice = nullptr;
    QAction *expand_action = nullptr;
    std::function<void(const QString &, const QString &)> decompose_action;
    std::function<void(const QString &)> replace_action;
    // Decision 1: what a picked folded node is, in one line.
    QLabel *folded_summary = nullptr;
    // GL-2b, decision 3: the Layers section over the details (it folds),
    // and who selects a plan node in the Frame Plan when an L1 row is
    // clicked. The plan's diagnostics, one line under the notice that
    // unfolds into the list.
    QPushButton *layers_toggle = nullptr;
    QTreeWidget *layers = nullptr;
    std::function<void(const QString &)> frame_plan_node_action;
    QPushButton *diagnostics_toggle = nullptr;
    QLabel *diagnostics_list = nullptr;
    QAction *add_loop_action = nullptr;
    QAction *add_fullscreen_action = nullptr;
    // Q-19, decision 4: "Rename definition…" and "Delete definition", and
    // the row they open under the scope controls -- the new name, or the
    // question -- with the one line that says why a name cannot be sent.
    QAction *rename_definition_action = nullptr;
    QAction *delete_definition_action = nullptr;
    QWidget *definition_row = nullptr;
    QLabel *definition_caption = nullptr;
    QLineEdit *definition_name = nullptr;
    QPushButton *definition_apply = nullptr;
    QPushButton *definition_cancel = nullptr;
    QLabel *definition_line = nullptr;
    // What the row is about, and whether it deletes rather than renames.
    std::string definition_subject;
    bool definition_deleting = false;
    // The definition an open context menu is about.
    std::optional<std::string> definition_menu_target;
    // LP-4d2, decision 5: what Show in Frame Plan does (the owner's).
    std::function<bool(const QString &)> frame_plan_loop_action;
    // Q-18: who is told that an edit of this tab committed and the plan may
    // have changed with it, and the last commit they were told of.
    std::function<void()> plan_commit_action;
    std::uint64_t told_plan_commit = 0;
    // --- GL-1e: a rendering-section pass's input edit, sent from here as
    // the Frame Plan sends it (edit_preset_pass, then get_edit_result).
    qint64 pass_edit_request = 0;
    qint64 pass_ticket_request = 0;
    QString pass_ticket;
    std::string pass_edit_notice;
    // A refusal for a moved document builds the edit once more, from the
    // context read after it (the revision it waits past).
    std::function<FramePlanEditRequest()> pass_edit_rebuild;
    bool pass_edit_retried = false;
    std::optional<std::uint64_t> pass_retry_after;
    // RU-1's two actions, which the tab's Undo and Redo also reach.
    QAction *render_undo = nullptr;
    QAction *render_redo = nullptr;
    // GL-1b, decision 2: who restarts the engine for a commit that asks.
    std::function<void(const QString &)> engine_restart_action;
    // Whether this tab asked for the restart now pending (a decline is only
    // this tab's to report then).
    bool restart_asked = false;
    QLineEdit *label_edit = nullptr;
    QPushButton *apply_label = nullptr;
    QPushButton *add_node = nullptr;
    // Q-8: the UI node a selected ui.value / ui.visible sink writes, chosen
    // from the project's UI document (and still typeable).
    QWidget *ui_node_row = nullptr;
    QComboBox *ui_node = nullptr;
    // Q-10: the three parameters of a selected audio.playback node that are
    // not ports -- the sound (the project's .wav / .ogg files, still
    // typeable), the bus and the wrap (the engine's own values, chosen).
    QWidget *audio_row = nullptr;
    QComboBox *audio_asset = nullptr;
    QComboBox *audio_bus = nullptr;
    QComboBox *audio_wrap = nullptr;
    std::string audio_assets_error;
    // The node the audio fields were last filled for. A redraw of the same
    // node (Live reads four times a second) neither walks the project again
    // nor overwrites a sound being typed.
    std::string audio_row_node;
    std::filesystem::path project_root;
    std::string ui_nodes_error;
    QAction *auto_layout_action = nullptr;
    QAction *fit_view_action = nullptr;
    QAction *tie_action = nullptr;
    QAction *untie_action = nullptr;
    QAction *undo_action = nullptr;
    QAction *redo_action = nullptr;
    QTimer *poll_timer = nullptr;
    QHash<qint64, quint64> transport_requests;
    std::uint64_t rendered_content = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t rendered_report = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t rendered_placement = std::numeric_limits<std::uint64_t>::max();

    explicit Impl(GraphCanvasWidget &owner, GraphCanvasReadCapability capability)
        : view{owner}, graph{std::move(capability)} {
        // Q-2 looked at stopping this clock while there is nothing to poll
        // for, and put it back: it is not only a poll. Descending into a
        // definition, leaving a scope, choosing one from the combo and
        // revealing a node all queue reads on the model and none of them
        // flushes the queue itself -- the tick does, through dispatch(). The
        // clock costs no RPC while idle (the model's own poll() asks for
        // nothing then); making it stoppable means first giving every one of
        // those paths a flush of its own, which is a change of its own.
        poll_timer = new QTimer(&view);
        poll_timer->setObjectName(
            QStringLiteral("pelican.graphCanvasPollTimer"));
        poll_timer->setInterval(250);
        QObject::connect(poll_timer, &QTimer::timeout, &view,
                         [this] { view.poll(); });
        poll_timer->start();
        view.setFont(studioPanelFont());
        auto *layout = new QVBoxLayout(&view);
        layout->setContentsMargins(6, 6, 6, 6);

        auto *controls = new QHBoxLayout;
        controls->addWidget(new QLabel(QObject::tr("Scope:"), &view));
        scopes = new QComboBox(&view);
        scopes->setObjectName(QStringLiteral("pelican.graphCanvasScope"));
        controls->addWidget(scopes);
        breadcrumb = new QLabel(&view);
        breadcrumb->setObjectName(QStringLiteral("pelican.graphCanvasBreadcrumb"));
        controls->addWidget(breadcrumb);
        // LP-4c, decision 5: inside a loop's body, which branch's body it is.
        loop_branch = new QComboBox(&view);
        loop_branch->setObjectName(QStringLiteral("pelican.graphCanvasLoopBranch"));
        loop_branch->setToolTip(QObject::tr("The branch of the loop whose body is shown"));
        loop_branch->hide();
        controls->addWidget(loop_branch);
        shared_mark = new QLabel(&view);
        shared_mark->setObjectName(QStringLiteral("pelican.graphCanvasShared"));
        shared_mark->setStyleSheet(QStringLiteral("color: #b9770e;"));
        shared_mark->hide();
        controls->addWidget(shared_mark);
        // GL-1, decision 5: which domains the root canvas draws. Render only
        // to begin with; session state, never written to the document.
        for (const auto &domain : GraphCanvasModel::domainNames())
            if (domain != "render") model.setDomainShown(domain, false);
        controls->addWidget(new QLabel(QObject::tr("Show:"), &view));
        for (const auto &domain : GraphCanvasModel::domainNames()) {
            const QString label = domain == "ui" ? QStringLiteral("UI")
                                                 : qtext(domain).left(1).toUpper() + qtext(domain).mid(1);
            auto *box = new QCheckBox(label, &view);
            box->setObjectName(QStringLiteral("pelican.graphCanvasShow.") + qtext(domain));
            box->setToolTip(QObject::tr("Draw the root's %1 nodes").arg(qtext(domain)));
            box->setChecked(model.domainShown(domain));
            controls->addWidget(box);
            domain_boxes.emplace_back(domain, box);
            QObject::connect(box, &QCheckBox::toggled, &view, [this, domain](bool on) {
                if (!model.setDomainShown(domain, on)) return;
                dispatch();
            });
        }
        controls->addStretch(1);
        controls->addWidget(new QLabel(QObject::tr("Values from:"), &view));
        value_instances = new QComboBox(&view);
        value_instances->setObjectName(QStringLiteral("pelican.graphCanvasValues"));
        controls->addWidget(value_instances);
        // Live is the switch between "the overlay follows the document" and
        // "the overlay follows the running engine". It reads on the report
        // slots, so a number that is moving cannot delay or cancel an edit.
        live = new QCheckBox(QObject::tr("Live"), &view);
        live->setObjectName(QStringLiteral("pelican.graphCanvasLive"));
        live->setToolTip(QObject::tr(
            "Re-read the node values four times a second while the engine runs."));
        controls->addWidget(live);
        refresh = new QPushButton(QObject::tr("Refresh"), &view);
        refresh->setObjectName(QStringLiteral("pelican.graphCanvasRefresh"));
        controls->addWidget(refresh);
        layout->addLayout(controls);

        // Q-19, decision 4: the definition row, hidden until Rename
        // definition… or Delete definition opens it.
        definition_row = new QWidget(&view);
        definition_row->setObjectName(QStringLiteral("pelican.graphCanvasDefinitionRow"));
        auto *definition_layout = new QHBoxLayout(definition_row);
        definition_layout->setContentsMargins(0, 0, 0, 0);
        definition_caption = new QLabel(definition_row);
        definition_caption->setObjectName(
            QStringLiteral("pelican.graphCanvasDefinitionCaption"));
        definition_layout->addWidget(definition_caption);
        definition_name = new QLineEdit(definition_row);
        definition_name->setObjectName(QStringLiteral("pelican.graphCanvasDefinitionName"));
        definition_name->setToolTip(QObject::tr(
            "The definition's new name: no slash, not \"root\", and no other definition's."));
        definition_layout->addWidget(definition_name, 1);
        definition_apply = new QPushButton(definition_row);
        definition_apply->setObjectName(QStringLiteral("pelican.graphCanvasDefinitionApply"));
        definition_layout->addWidget(definition_apply);
        definition_cancel = new QPushButton(QObject::tr("Cancel"), definition_row);
        definition_cancel->setObjectName(QStringLiteral("pelican.graphCanvasDefinitionCancel"));
        definition_layout->addWidget(definition_cancel);
        definition_line = new QLabel(definition_row);
        definition_line->setObjectName(QStringLiteral("pelican.graphCanvasDefinitionLine"));
        definition_line->setStyleSheet(QStringLiteral("color: #c0392b;"));
        definition_line->hide();
        definition_layout->addWidget(definition_line);
        layout->addWidget(definition_row);
        definition_row->hide();
        QObject::connect(definition_name, &QLineEdit::textChanged, &view,
                         [this] { validateDefinitionRow(); });
        QObject::connect(definition_name, &QLineEdit::returnPressed, &view,
                         [this] { applyDefinitionRow(); });
        QObject::connect(definition_apply, &QPushButton::clicked, &view,
                         [this] { applyDefinitionRow(); });
        QObject::connect(definition_cancel, &QPushButton::clicked, &view,
                         [this] { closeDefinitionRow(); });
        auto *definition_escape = new QAction(definition_name);
        definition_escape->setShortcut(QKeySequence{Qt::Key_Escape});
        definition_escape->setShortcutContext(Qt::WidgetShortcut);
        definition_name->addAction(definition_escape);
        QObject::connect(definition_escape, &QAction::triggered, &view,
                         [this] { closeDefinitionRow(); });
        // The Scope combo and the breadcrumb offer them for the definition
        // they show, on a right-click.
        scopes->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(scopes, &QWidget::customContextMenuRequested, &view,
                         [this](const QPoint &at) {
                             definitionMenu(scopes->currentData().toString().toStdString(),
                                            scopes->mapToGlobal(at));
                         });
        breadcrumb->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(breadcrumb, &QWidget::customContextMenuRequested, &view,
                         [this](const QPoint &at) {
                             definitionMenu(model.currentScope(), breadcrumb->mapToGlobal(at));
                         });

        notice = new QLabel(&view);
        notice->setObjectName(QStringLiteral("pelican.graphCanvasNotice"));
        applyPanelNoticeStyle(notice);
        notice->hide();
        layout->addWidget(notice);
        // GL-1, decision 4.
        folded_notice = new QLabel(&view);
        folded_notice->setObjectName(QStringLiteral("pelican.graphCanvasFoldedNotice"));
        folded_notice->setTextFormat(Qt::RichText);
        folded_notice->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        folded_notice->setStyleSheet(QStringLiteral("color: #b9770e;"));
        folded_notice->hide();
        layout->addWidget(folded_notice);
        // GL-2b, decision 3: the plan's diagnostics, folded to one line.
        diagnostics_toggle = new QPushButton(&view);
        diagnostics_toggle->setObjectName(QStringLiteral("pelican.graphCanvasDiagnostics"));
        diagnostics_toggle->setCheckable(true);
        diagnostics_toggle->setFlat(true);
        diagnostics_toggle->setStyleSheet(QStringLiteral("text-align: left;"));
        diagnostics_toggle->setToolTip(
            QObject::tr("What the engine decided while it composed the frame plan"));
        diagnostics_toggle->hide();
        layout->addWidget(diagnostics_toggle);
        diagnostics_list = new QLabel(&view);
        diagnostics_list->setObjectName(QStringLiteral("pelican.graphCanvasDiagnosticsList"));
        diagnostics_list->setWordWrap(true);
        diagnostics_list->setTextInteractionFlags(Qt::TextSelectableByMouse);
        diagnostics_list->hide();
        layout->addWidget(diagnostics_list);
        QObject::connect(diagnostics_toggle, &QPushButton::toggled, &view,
                         [this](bool) { syncDiagnostics(); });
        expand_action = new QAction(QObject::tr("Expand to edit…"), &view);
        expand_action->setObjectName(QStringLiteral("pelican.graphCanvas.expandToEdit"));
        expand_action->setEnabled(false);
        view.addAction(expand_action);
        QObject::connect(expand_action, &QAction::triggered, &view, [this] { expandToEdit(); });
        QObject::connect(folded_notice, &QLabel::linkActivated, &view,
                         [this](const QString &) { expand_action->trigger(); });

        auto *splitter = new QSplitter(Qt::Horizontal, &view);
        scene = new GraphCanvasScene(&view);
        canvas = new NodeCanvasView(scene, splitter);
        canvas->setObjectName(QStringLiteral("pelican.graphCanvasView"));
        const auto historyAction = [&](const QString &label, const QString &name,
                                       QKeySequence::StandardKey key, auto invoke) {
            auto *action = new QAction(label, &view);
            action->setObjectName(name);
            action->setShortcut(QKeySequence{key});
            // The scene handles canvas keys; inline text editors must retain
            // Qt's text Undo. The action remains available to automation.
            action->setShortcutContext(Qt::WidgetShortcut);
            view.addAction(action);
            auto *button = new QPushButton(label, &view);
            button->setObjectName(name + QStringLiteral("Button"));
            controls->insertWidget(controls->count() - 1, button);
            QObject::connect(button, &QPushButton::clicked, action, &QAction::trigger);
            QObject::connect(action, &QAction::changed, button,
                             [button, action] { button->setEnabled(action->isEnabled()); });
            QObject::connect(action, &QAction::triggered, &view, invoke);
            action->setEnabled(false);
            return action;
        };
        undo_action = historyAction(QObject::tr("Undo"),
            QStringLiteral("pelican.graphCanvas.undo"), QKeySequence::Undo,
            [this] { scene->undoEdit(); });
        redo_action = historyAction(QObject::tr("Redo"),
            QStringLiteral("pelican.graphCanvas.redo"), QKeySequence::Redo,
            [this] { scene->redoEdit(); });
        // The canvas menu's two entries live for as long as the tab does, and
        // are named: `action` fires a QAction by name without opening a popup,
        // which a command that owes a reply on the wire cannot do.
        auto_layout_action = new QAction(QObject::tr("Auto layout"), &view);
        auto_layout_action->setObjectName(
            QStringLiteral("pelican.graphCanvas.autoLayout"));
        view.addAction(auto_layout_action);
        QObject::connect(auto_layout_action, &QAction::triggered, &view,
                         [this] { scene->autoLayout(); });
        fit_view_action = new QAction(QObject::tr("Fit view"), &view);
        fit_view_action->setObjectName(
            QStringLiteral("pelican.graphCanvas.fitView"));
        fit_view_action->setShortcut(QKeySequence{Qt::Key_F});
        // The key itself belongs to the view, which knows that "F" typed into
        // an in-place editor is a letter. The entry carries the binding so the
        // menu can show it; the context is the panel itself, which never holds
        // the keyboard, so the shortcut cannot take a keystroke off an editor.
        fit_view_action->setShortcutContext(Qt::WidgetShortcut);
        view.addAction(fit_view_action);
        QObject::connect(fit_view_action, &QAction::triggered, &view,
                         [this] { canvas->fitView(); });
        // UX-1, decision 5: both stand in the row too, after Undo and Redo --
        // the right-click menu was the only way to them. The buttons fire
        // the same actions.
        for (QAction *action : {auto_layout_action, fit_view_action}) {
            auto *button = new QPushButton(action->text(), &view);
            button->setObjectName(action->objectName() + QStringLiteral("Button"));
            button->setToolTip(action == auto_layout_action
                                   ? QObject::tr("Place this scope's nodes again")
                                   : QObject::tr("Show the whole graph (F)"));
            controls->insertWidget(controls->count() - 1, button);
            QObject::connect(button, &QPushButton::clicked, action, &QAction::trigger);
            QObject::connect(action, &QAction::changed, button,
                             [button, action] { button->setEnabled(action->isEnabled()); });
            button->setEnabled(action->isEnabled());
        }
        tie_action = new QAction(QObject::tr("Tie together"), &view);
        tie_action->setObjectName(
            QStringLiteral("pelican.graphCanvas.tieTogether"));
        tie_action->setEnabled(false);
        view.addAction(tie_action);
        QObject::connect(tie_action, &QAction::triggered, &view,
                         [this] { scene->tieSelection(); });
        untie_action = new QAction(QObject::tr("Untie"), &view);
        untie_action->setObjectName(QStringLiteral("pelican.graphCanvas.untie"));
        untie_action->setEnabled(false);
        view.addAction(untie_action);
        QObject::connect(untie_action, &QAction::triggered, &view,
                         [this] { scene->untieSelection(); });
        scene->setMenuActions(auto_layout_action, fit_view_action, tie_action,
                              untie_action);
        // LP-4c. "Add loop" puts a loop in the root over a new, empty body
        // named by a proposal -- no dialog, no name to type -- and "Add
        // fullscreen pass" the engine's passthrough into an image of its own,
        // in the scope on screen. Both are named, so the canvas menu and the
        // automation surface fire the same object; the palette offers both
        // too, with the body to choose.
        add_loop_action = new QAction(QObject::tr("Add loop"), &view);
        add_loop_action->setObjectName(QStringLiteral("pelican.graphCanvas.addLoop"));
        view.addAction(add_loop_action);
        QObject::connect(add_loop_action, &QAction::triggered, &view, [this] {
            scene->addLoop({}, model.suggestDefinitionName());
        });
        add_fullscreen_action = new QAction(QObject::tr("Add fullscreen pass"), &view);
        add_fullscreen_action->setObjectName(
            QStringLiteral("pelican.graphCanvas.addFullscreenPass"));
        view.addAction(add_fullscreen_action);
        QObject::connect(add_fullscreen_action, &QAction::triggered, &view, [this] {
            scene->addFromPalette("render.fullscreen");
        });
        scene->setAddActions(add_loop_action, add_fullscreen_action);
        // Q-19, decision 4. Named, like the additions: fired by name they
        // are about the definition on screen, else the one the picked zone
        // shows; from a menu, about the definition the menu was opened over.
        rename_definition_action = new QAction(QObject::tr("Rename definition…"), &view);
        rename_definition_action->setObjectName(
            QStringLiteral("pelican.graphCanvas.renameDefinition"));
        rename_definition_action->setEnabled(false);
        view.addAction(rename_definition_action);
        QObject::connect(rename_definition_action, &QAction::triggered, &view,
                         [this] { openDefinitionRow(false); });
        delete_definition_action = new QAction(QObject::tr("Delete definition"), &view);
        delete_definition_action->setObjectName(
            QStringLiteral("pelican.graphCanvas.deleteDefinition"));
        delete_definition_action->setEnabled(false);
        view.addAction(delete_definition_action);
        QObject::connect(delete_definition_action, &QAction::triggered, &view,
                         [this] { openDefinitionRow(true); });
        scene->setDefinitionActions(
            rename_definition_action, delete_definition_action,
            [this](std::optional<std::string> target) {
                definition_menu_target = std::move(target);
            });
        configureGraphCanvasView(*canvas);
        canvas->setFitFloor(GraphCanvasFitFloor, [this] { return scene->fitFocus(); });
        canvas->setRenderHint(QPainter::Antialiasing, true);
        // Delete has to reach the scene, and a right-click on the canvas has to
        // reach the scene's own handler rather than the view's context menu.
        canvas->setFocusPolicy(Qt::StrongFocus);
        canvas->setContextMenuPolicy(Qt::DefaultContextMenu);
        splitter->addWidget(canvas);
        // UX-5d: the column beside the canvas scrolls rather than widens. A
        // node's fields appear the moment it is pressed -- also as the press
        // that starts a line from its socket -- and a column that grew to fit
        // them moved the splitter, so the canvas, and the socket the line was
        // being drawn to, slid away under the pointer mid-drag.
        auto *side_scroll = new QScrollArea(splitter);
        side_scroll->setObjectName(QStringLiteral("pelican.graphCanvasSide"));
        side_scroll->setWidgetResizable(true);
        side_scroll->setFrameShape(QFrame::NoFrame);
        side_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        auto *side = new QWidget(side_scroll);
        auto *side_layout = new QVBoxLayout(side);
        side_layout->setContentsMargins(0, 0, 0, 0);
        folded_summary = new QLabel(side);
        folded_summary->setObjectName(QStringLiteral("pelican.graphCanvasFoldedSummary"));
        folded_summary->setWordWrap(true);
        folded_summary->hide();
        side_layout->addWidget(folded_summary);
        // GL-2b, decision 3: what the picked root node became below L0 --
        // expanded (L1), physically (L2), on the backend (L3) -- or, with
        // nothing picked, what concerns the whole graph. It folds.
        layers_toggle = new QPushButton(QObject::tr("Layers"), side);
        layers_toggle->setObjectName(QStringLiteral("pelican.graphCanvasLayersToggle"));
        layers_toggle->setCheckable(true);
        layers_toggle->setChecked(true);
        layers_toggle->setFlat(true);
        layers_toggle->setStyleSheet(QStringLiteral("text-align: left; font-weight: bold;"));
        layers_toggle->hide();
        side_layout->addWidget(layers_toggle);
        layers = new QTreeWidget(side);
        layers->setObjectName(QStringLiteral("pelican.graphCanvasLayers"));
        layers->setColumnCount(1);
        layers->setHeaderHidden(true);
        layers->setRootIsDecorated(true);
        layers->setToolTip(QObject::tr("Read only. Click a pass to pick it in the Frame Plan."));
        layers->hide();
        side_layout->addWidget(layers, 1);
        QObject::connect(layers_toggle, &QPushButton::toggled, &view,
                         [this](bool) { syncLayers(); });
        QObject::connect(layers, &QTreeWidget::itemClicked, &view,
                         [this](QTreeWidgetItem *item, int) {
                             if (item == nullptr || !frame_plan_node_action) return;
                             const auto node = item->data(0, Qt::UserRole).toString();
                             if (!node.isEmpty()) frame_plan_node_action(node);
                         });
        auto *label_row = new QHBoxLayout;
        auto *label_caption = new QLabel(QObject::tr("Label"), side);
        label_caption->setFont(studioSmallFont());
        label_row->addWidget(label_caption);
        label_edit = new QLineEdit(side);
        label_edit->setObjectName(QStringLiteral("pelican.graphCanvasLabel"));
        label_edit->setPlaceholderText(QObject::tr("(the id is the heading)"));
        label_edit->setToolTip(
            QObject::tr("A node with no label is drawn under its id."));
        label_row->addWidget(label_edit, 1);
        apply_label = new QPushButton(QObject::tr("Rename"), side);
        apply_label->setObjectName(QStringLiteral("pelican.graphCanvasRename"));
        label_row->addWidget(apply_label);
        add_node = new QPushButton(QObject::tr("Add"), side);
        add_node->setObjectName(QStringLiteral("pelican.graphCanvasAddNode"));
        add_node->setToolTip(QObject::tr("Add a node from the catalog."));
        label_row->addWidget(add_node);
        label_row->addWidget(makePanelHelpButton(
            side, "pelican.graphCanvas.help", ManualGraphCanvasAnchor,
            QObject::tr("Shift-drag a socket to connect; double-click an "
                        "instance to descend into its definition.")));
        side_layout->addLayout(label_row);
        ui_node_row = new QWidget(side);
        ui_node_row->setObjectName(QStringLiteral("pelican.graphCanvasUiNodeRow"));
        auto *ui_node_layout = new QHBoxLayout(ui_node_row);
        ui_node_layout->setContentsMargins(0, 0, 0, 0);
        auto *ui_node_caption = new QLabel(QObject::tr("UI node"), ui_node_row);
        ui_node_caption->setFont(studioSmallFont());
        ui_node_layout->addWidget(ui_node_caption);
        ui_node = new QComboBox(ui_node_row);
        ui_node->setObjectName(QStringLiteral("pelican.graphCanvasUiNode"));
        ui_node->setEditable(true);
        // A path typed that the document does not have is sent as typed and
        // refused by the engine by name; it is never added to the list.
        ui_node->setInsertPolicy(QComboBox::NoInsert);
        ui_node->setToolTip(QObject::tr(
            "The path of the UI node this sink writes: the ids from the root "
            "of the project's UI document, joined by \"/\"."));
        ui_node_layout->addWidget(ui_node, 1);
        side_layout->addWidget(ui_node_row);
        ui_node_row->hide();
        audio_row = new QWidget(side);
        audio_row->setObjectName(QStringLiteral("pelican.graphCanvasAudioRow"));
        auto *audio_layout = new QFormLayout(audio_row);
        audio_layout->setContentsMargins(0, 0, 0, 0);
        const auto audioCombo = [this](const char *name, bool editable) {
            auto *combo = new QComboBox(audio_row);
            combo->setObjectName(QString::fromLatin1(name));
            combo->setEditable(editable);
            if (editable) combo->setInsertPolicy(QComboBox::NoInsert);
            return combo;
        };
        audio_asset = audioCombo("pelican.graphCanvasAudioAsset", true);
        audio_asset->setToolTip(QObject::tr(
            "The sound this voice plays: a .wav or .ogg file of the project. "
            "Its extension and its contents have to agree."));
        audio_bus = audioCombo("pelican.graphCanvasAudioBus", false);
        audio_bus->setToolTip(QObject::tr("The bus the voice plays on."));
        audio_wrap = audioCombo("pelican.graphCanvasAudioWrap", false);
        const auto caption = [this](const QString &text) {
            auto *label = new QLabel(text, audio_row);
            label->setFont(studioSmallFont());
            return label;
        };
        audio_layout->addRow(caption(QObject::tr("Sound")), audio_asset);
        audio_layout->addRow(caption(QObject::tr("Bus")), audio_bus);
        audio_layout->addRow(caption(QObject::tr("Wrap")), audio_wrap);
        side_layout->addWidget(audio_row);
        audio_row->hide();
        loop_fields = new GraphCanvasLoopFields(
            model, [this] { dispatch(); },
            [this](const std::string &loop) {
                // From the event loop: the scope change rebuilds the fields
                // the press came from.
                QMetaObject::invokeMethod(
                    &view, [this, loop] {
                        scene->enterNode(loop);
                        dispatch();
                    },
                    Qt::QueuedConnection);
            },
            side);
        side_layout->addWidget(loop_fields);
        draw_fields = new GraphCanvasDrawFields(model, [this] { dispatch(); }, side);
        side_layout->addWidget(draw_fields);
        view_fields = new GraphCanvasViewFields(model, [this] { dispatch(); }, side);
        side_layout->addWidget(view_fields);
        image_fields = new GraphCanvasImageFields(model, [this] { dispatch(); }, side);
        side_layout->addWidget(image_fields);
        pass_fields = new GraphCanvasPassFields(model, [this] { dispatch(); }, side);
        // UX-5b: New... wrote a shader; the lists hold it from now on.
        pass_fields->setShadersChanged([this] { refreshShaderChoices(); });
        side_layout->addWidget(pass_fields);
        place_fields = new GraphCanvasPlaceFields(model, [this] { dispatch(); }, side);
        side_layout->addWidget(place_fields);
        details = new QTreeWidget(side);
        details->setObjectName(QStringLiteral("pelican.graphCanvasDetails"));
        details->setColumnCount(2);
        details->setHeaderLabels({QObject::tr("Field"), QObject::tr("Value")});
        details->setRootIsDecorated(true);
        // UX-2, decision 5: a parameter's value is still typed as JSON here,
        // the one place the raw document is written; the fields above are
        // what is picked. Only a parameter's value opens, on a double-click.
        details->setEditTriggers(QAbstractItemView::NoEditTriggers);
        QObject::connect(details, &QTreeWidget::itemDoubleClicked, &view,
                         [this](QTreeWidgetItem *item, int) { beginParameterEdit(item); });
        QObject::connect(details, &QTreeWidget::itemChanged, &view,
                         [this](QTreeWidgetItem *item, int column) {
                             if (item == nullptr || item != editing_parameter || column != 1) return;
                             editing_parameter = nullptr;
                             const auto key = item->data(1, Qt::UserRole).toString().toStdString();
                             const auto text = item->text(1);
                             // Not from inside the tree's own signal: the
                             // edit redraws the details, item included.
                             QMetaObject::invokeMethod(
                                 &view, [this, key, text] { commitParameterText(key, text); },
                                 Qt::QueuedConnection);
                         });
        QObject::connect(details->itemDelegate(), &QAbstractItemDelegate::closeEditor, &view,
                         [this] { editing_parameter = nullptr; });
        side_layout->addWidget(details, 1);
        side_scroll->setWidget(side);
        splitter->addWidget(side_scroll);
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 2);
        layout->addWidget(splitter, 1);

        // The Timeline, folded away until it is asked for: it is a second
        // reading of the same numbers, and a panel that is always there would
        // take height from the canvas for a strip most sessions never open.
        auto *timeline_row = new QHBoxLayout;
        timeline_toggle = new QPushButton(QObject::tr("Timeline"), &view);
        timeline_toggle->setObjectName(
            QStringLiteral("pelican.graphTimelineToggle"));
        timeline_toggle->setCheckable(true);
        timeline_row->addWidget(timeline_toggle);
        timeline_summary = new QLabel(&view);
        timeline_summary->setObjectName(
            QStringLiteral("pelican.graphTimelineSummary"));
        timeline_row->addWidget(timeline_summary, 1);
        layout->addLayout(timeline_row);
        timeline_panel = new QWidget(&view);
        auto *timeline_layout = new QVBoxLayout(timeline_panel);
        timeline_layout->setContentsMargins(0, 0, 0, 0);
        timeline = new GraphTimelineStrip(model, [this](double time) {
            if (model.scrub(time)) dispatch();
        }, timeline_panel);
        timeline_layout->addWidget(timeline);
        timeline_panel->hide();
        layout->addWidget(timeline_panel);
        QObject::connect(timeline_toggle, &QPushButton::toggled, &view,
                         [this](bool open) {
                             timeline_panel->setVisible(open);
                             synchronizeTimeline();
                         });
        QObject::connect(live, &QCheckBox::toggled, &view, [this](bool on) {
            model.setLive(on);
            dispatch();
        });

        scene->setModel(&model);
        scene->scopeChanged = [this] {
            syncScopeControls();
            syncValueControls();
            showDetails();
        };
        scene->selectionChanged = [this] {
            // The Timeline follows the selection: the node a person is looking
            // at is the node whose outputs they mean. (LP-4d1: not a node of a
            // zone's body -- no reading answers for one.)
            const auto &selected = scene->selectedNode();
            model.setTimelineNode(selected && scene->selectedScope() == model.currentScope()
                                      ? *selected
                                      : std::string{});
            showDetails();
            synchronizeTimeline();
            // Q-19: a picked zone is what the definition actions are about.
            syncDefinitionActions();
        };
        // Every canvas gesture is applied the moment it finishes, so the only
        // thing left to do here is post what the model already built.
        // UX-5d: and the notice says "Saving" the moment the gesture is
        // made. An edit can wait to go out (behind get_engine_shaders, which
        // the engine takes seconds over), and until then the line still said
        // the last edit was saved.
        scene->editSubmitted = [this] {
            dispatch();
            showNotice();
        };
        // Q-5: the notice says "Saving" the moment a placement changes, not
        // when the wait before sending it runs out. Only the label: this runs
        // from inside a drag and from inside a rebuild.
        scene->placementChanged = [this] { showNotice(); };
        // LP-4d2: a zone gesture refused before it was sent says why.
        scene->noticeChanged = [this] { showNotice(); };
        // GL-1e: a line into a rendering-section pass is sent from here.
        scene->passInputEditRequested =
            [this](const FramePlanEditRequest &request,
                   std::function<FramePlanEditRequest()> rebuild) {
                sendPassInputEdit(request, std::move(rebuild));
            };
        // Decision 5: the owner brings the Frame Plan forward.
        scene->showInFramePlanRequested = [this](const std::string &loop) {
            if (!frame_plan_loop_action || !frame_plan_loop_action(qtext(loop))) {
                notice->setText(QObject::tr("The Frame Plan cannot show loop %1.").arg(qtext(loop)));
                return;
            }
        };
        scene->addNodeRequested = [this](const QPointF &) { openPalette(); };
        scene->perIterationRequested = [this](const std::string &node,
                                              const std::string &port) {
            openPerIteration(node, port);
        };
        QObject::connect(loop_branch, &QComboBox::currentIndexChanged, &view,
                         [this](int index) {
                             if (index < 0) return;
                             const auto branch = static_cast<std::size_t>(
                                 loop_branch->itemData(index).toULongLong());
                             if (!model.selectLoopBranch(branch)) return;
                             scene->repopulate();
                             syncScopeControls();
                             syncValueControls();
                             showDetails();
                             dispatch();
                         });
        scene->fitViewRequested = [this] { canvas->fitView(); };
        // A scope shown for the first time -- which includes the first draw
        // after a document arrives -- is fitted; one being returned to keeps
        // the zoom and scroll it was left at.
        scene->repopulated = [this] {
            // GL-1: a folded view is a view of its own, fitted the first time.
            canvas->showScope(qtext(model.viewKey()));
        };
        scene->domainFilterChanged = [this] {
            syncDomainFilter();
            dispatch();
        };

        QObject::connect(refresh, &QPushButton::clicked, &view, [this] {
            model.refresh();
            dispatch();
        });
        QObject::connect(
            scopes, &QComboBox::currentIndexChanged, &view, [this](int index) {
                if (index < 0) return;
                const auto scope =
                    scopes->itemData(index).toString().toStdString();
                // GL-1: "root" from inside a folded view is the real one.
                if (scope == model.currentScope() && !model.foldedView()) return;
                model.selectScope(scope);
                scene->repopulate();
                syncScopeControls();
                syncValueControls();
                showDetails();
            });
        QObject::connect(
            value_instances, &QComboBox::currentIndexChanged, &view,
            [this](int index) {
                if (index < 0) return;
                model.watchInstance(
                    value_instances->itemData(index).toString().toStdString());
                dispatch();
            });
        // The same rename the heading's double-click performs, for a node that
        // is easier to reach in the list than on the canvas.
        const auto rename = [this] {
            const auto &selected = scene->selectedNode();
            if (!selected) return;
            if (model.setNodeLabel(scene->selectedScope(), *selected,
                                   label_edit->text().toStdString()))
                dispatch();
        };
        QObject::connect(apply_label, &QPushButton::clicked, &view, rename);
        QObject::connect(label_edit, &QLineEdit::returnPressed, &view, rename);
        // GL-1d: the palette is a modal dialog; opened from the event loop,
        // as the menu's Add node... opens it, the click that asked for it
        // has finished (and a scenario's click has its reply).
        QObject::connect(add_node, &QPushButton::clicked, &view, [this] {
            QMetaObject::invokeMethod(&view, [this] { openPalette(); },
                                      Qt::QueuedConnection);
        });
        // Picking from the list and pressing Enter on a typed path are the
        // two ways the field is finished; each sends one set_node_parameters.
        QObject::connect(ui_node, &QComboBox::activated, &view,
                         [this](int index) { commitUiNode(ui_node->itemText(index)); });
        QObject::connect(ui_node->lineEdit(), &QLineEdit::returnPressed, &view,
                         [this] { commitUiNode(ui_node->currentText()); });
        // Q-10: the same two ways for the sound; bus and wrap are only picked.
        QObject::connect(audio_asset, &QComboBox::activated, &view, [this](int index) {
            commitAudio("asset", audio_asset->itemText(index));
        });
        QObject::connect(audio_asset->lineEdit(), &QLineEdit::returnPressed, &view,
                         [this] { commitAudio("asset", audio_asset->currentText()); });
        QObject::connect(audio_bus, &QComboBox::activated, &view, [this](int index) {
            commitAudio("bus", audio_bus->itemText(index));
        });
        QObject::connect(audio_wrap, &QComboBox::activated, &view, [this](int index) {
            commitAudio("wrap", audio_wrap->itemText(index));
        });

        // ready() reports the current state synchronously, which happens while
        // the widget's pointer to this Impl is still null. Every handler works
        // on the Impl directly, never back through the half-built widget.
        if (graph.result)
            graph.result(&view, [this](qint64 id, const QByteArray &json) {
                deliverResult(id, json);
            });
        if (graph.failure)
            graph.failure(&view, [this](qint64 id, const QString &message) {
                deliverFailure(id, message);
            });
        if (graph.ready)
            graph.ready(&view, [this](bool available, const QString &) {
                if (!available) restart_asked = false;
                model.setConnected(available);
                dispatch();
            });

        synchronize();
    }

    // --- GL-1e.

    void sendPassInputEdit(const FramePlanEditRequest &request,
                           std::function<FramePlanEditRequest()> rebuild, bool retry = false) {
        const auto say = [this](InspectorNoticeKind kind, std::string line) {
            model.setEditNotice({kind, std::move(line)});
            showNotice();
        };
        if (pass_edit_request != 0 || !pass_ticket.isEmpty()) {
            say(InspectorNoticeKind::Error, "Wait for the current change to finish.");
            return;
        }
        if (!graph.requestEditPresetPass) {
            say(InspectorNoticeKind::Error,
                "This engine connection cannot edit a pass from the Graph.");
            return;
        }
        QJsonObject params;
        try {
            params = exactJsonRpcObject(request.params);
        } catch (const std::exception &error) {
            say(InspectorNoticeKind::Error, error.what());
            return;
        }
        QString error;
        pass_edit_request = graph.requestEditPresetPass(params, &error);
        if (pass_edit_request == 0) {
            say(InspectorNoticeKind::Error, error.toStdString());
            return;
        }
        pass_edit_notice = request.notice;
        pass_edit_rebuild = std::move(rebuild);
        pass_edit_retried = retry;
        say(InspectorNoticeKind::Information,
            request.params.value("pass_name", std::string{}) + ": saving its inputs...");
    }

    void requestPassTicket() {
        if (pass_ticket.isEmpty() || pass_ticket_request != 0) return;
        QString error;
        pass_ticket_request =
            graph.requestEditResult ? graph.requestEditResult(pass_ticket, &error) : 0;
        if (pass_ticket_request != 0) return;
        pass_ticket.clear();
        model.setEditNotice({InspectorNoticeKind::Error, error.toStdString()});
        showNotice();
    }

    // Decision 3: the engine's own sentence, and -- for a pass that reads an
    // image drawn later in the frame -- why (an engine before RS-3 puts the
    // graph's pass after the rendering section's).
    std::string passEditRefusal(const nlohmann::json &response) const {
        const auto error = response.find("error");
        const auto &reason = error != response.end() && error->is_object() ? *error : response;
        const auto code = reason.value("code", std::string{});
        const auto message = reason.value("message", std::string{});
        // UX-5a: the line is the code and the message's sentence; the whole
        // message follows it, on the lines the tooltip shows.
        std::string line = engineRefusalLine(reason);
        if (line.empty()) return response.dump();
        // UX-5c: a refusal Studio has a sentence for (a loop output the pass
        // names, RS-13; a render node's missing parameters, RS-9) reads as
        // that sentence, after the code of the refusal that carried it.
        if (auto known = knownRefusalDescription(code, message); !known.empty())
            line = code.empty() || known.find(code) != std::string::npos ? known
                                                                         : code + ": " + known;
        // UX-5d, decision 3: only when a graph node's pass writes the image
        // (the frame plan says who does): not for a feature's task or a pass
        // of the rendering section, which the phrase would misname.
        if (message.find("Pass input target is not produced as an earlier output") !=
                std::string::npos &&
            model.graphWritesResourceNamedIn(message))
            line += passRunsAfterHint;
        const auto whole = code.empty() ? message : code + ": " + message;
        if (!message.empty() && line.find(whole) == std::string::npos) line += "\n" + whole;
        return line;
    }

    void refusePassEdit(const nlohmann::json &response) {
        const auto error = response.find("error");
        const auto code = error != response.end() && error->is_object()
                              ? error->value("code", std::string{})
                              : response.value("code", std::string{});
        // The document moved since the context was read: read it again, and
        // send the gesture once more from what arrives.
        if (code == "external_modification" && pass_edit_rebuild && !pass_edit_retried) {
            pass_retry_after = model.authoringRevision();
            model.setEditNotice({InspectorNoticeKind::Information,
                                 "The document changed since the Graph read it; reading it "
                                 "again..."});
            showNotice();
            model.refresh();
            dispatch();
            return;
        }
        pass_edit_rebuild = {};
        model.setEditNotice({InspectorNoticeKind::Error, passEditRefusal(response)});
        showNotice();
    }

    void resumePassRetry() {
        if (!pass_retry_after || model.authoringRevision() == *pass_retry_after) return;
        pass_retry_after.reset();
        auto rebuild = std::exchange(pass_edit_rebuild, {});
        if (!rebuild) return;
        const auto request = rebuild();
        if (!request.sendable()) {
            model.setEditNotice({InspectorNoticeKind::Error,
                                 !request.line.empty() ? request.line : request.refusal});
            showNotice();
            return;
        }
        sendPassInputEdit(request, rebuild, true);
    }

    void passEditCommitted(const nlohmann::json &response) {
        pass_edit_rebuild = {};
        // Q-13: the other panels read the revision this commit made, and
        // this tab reads the document, the plan and the context again.
        const auto digest = response.value("source_digest", nlohmann::json::object())
                                .value("hex", std::string{});
        if (digest_hub && !digest.empty()) digest_hub->publish(digest, &view);
        // RSX, decision 1: a pass edit the engine can run only after a
        // restart restarts it, as the tab's own graph edits do (GL-1b).
        if (const auto completion = readRenderEditCompletion(response);
            completion.restart_required) {
            model.setEditNotice({InspectorNoticeKind::Information, completion.restart_line});
            scene->noteRenderEdit();
            showNotice();
            if (engine_restart_action) {
                restart_asked = true;
                engine_restart_action(qtext(completion.restart_line));
            }
            return;
        }
        model.setEditNotice({InspectorNoticeKind::Information,
                             pass_edit_notice.empty() ? std::string{"Pass inputs saved."}
                                                      : pass_edit_notice});
        // RU-1: the tab's Undo takes this back first.
        scene->noteRenderEdit();
        if (plan_commit_action) plan_commit_action();
        model.refresh();
        dispatch();
    }

    // The two replies of a pass's input edit; false for any other.
    bool deliverPassEdit(qint64 request_id, const QByteArray &bytes) {
        if (request_id == 0 ||
            (request_id != pass_edit_request && request_id != pass_ticket_request))
            return false;
        const bool submission = request_id == pass_edit_request;
        (submission ? pass_edit_request : pass_ticket_request) = 0;
        try {
            const auto response = nlohmann::json::parse(
                bytes.constData(), bytes.constData() + bytes.size());
            const auto status = response.value("status", std::string{});
            if (submission) {
                const auto ticket = response.find("ticket");
                if (status != "accepted" || ticket == response.end() || !ticket->is_string()) {
                    refusePassEdit(response);
                    return true;
                }
                pass_ticket = qtext(ticket->get<std::string>());
                requestPassTicket();
                return true;
            }
            if (status == "pending") {
                QTimer::singleShot(25, &view, [this] { requestPassTicket(); });
                return true;
            }
            pass_ticket.clear();
            if (status != "committed" || !response.value("committed", false)) {
                refusePassEdit(response);
                return true;
            }
            passEditCommitted(response);
        } catch (const std::exception &error) {
            pass_ticket.clear();
            model.setEditNotice({InspectorNoticeKind::Error, error.what()});
            showNotice();
        }
        return true;
    }

    void deliverResult(qint64 request_id, const QByteArray &result_json) {
        if (deliverPassEdit(request_id, result_json)) return;
        const auto found = transport_requests.find(request_id);
        if (found == transport_requests.end()) return;
        const auto model_id = found.value();
        transport_requests.erase(found);
        model.receiveRpcResult(
            model_id,
            std::string_view(result_json.constData(),
                             static_cast<std::size_t>(result_json.size())));
        followDocument();
        dispatch();
        tellPlanCommit();
        revealPending();
        // GL-1e: an edit refused for a moved document, once the context
        // read after it has arrived.
        resumePassRetry();
        restartEngineIfAsked();
    }

    // GL-1b, decision 2: the commit is written; the engine starts again to
    // run it, through the same restart the Frame Plan asks for. Last, since
    // it can stop the engine this tab's requests go to.
    void restartEngineIfAsked() {
        const auto line = model.takeEngineRestart();
        if (!line) return;
        if (!engine_restart_action) return;
        restart_asked = true;
        engine_restart_action(qtext(*line));
    }

    // Q-18. The digest the panels share moves on a placement too -- the
    // Frame Plan's own node positions included -- and says nothing of what
    // changed, so the plan is not read again on it: this names the commits
    // that can change the plan.
    void tellPlanCommit() {
        if (model.planCommitRevision() == told_plan_commit) return;
        told_plan_commit = model.planCommitRevision();
        if (plan_commit_action) plan_commit_action();
    }

    // LP-4e: a root node another panel made, shown once it is here.
    std::string pending_reveal;
    bool revealPending() {
        if (pending_reveal.empty()) return false;
        const auto nodes = model.nodes({});
        if (std::ranges::none_of(nodes, [&](const GraphCanvasNode &drawn) {
                return drawn.id == pending_reveal;
            }))
            return false;
        const auto id = std::exchange(pending_reveal, {});
        model.selectScope({});
        showDomainOf({}, id);
        scene->repopulate();
        scene->selectNode(id);
        syncScopeControls();
        syncValueControls();
        showDetails();
        dispatch();
        for (QGraphicsItem *item : scene->items()) {
            if (item->data(NodeGraphItemKindRole).toString() !=
                    QLatin1String{GraphCanvasNodeItem} ||
                !item->data(GraphCanvasScopeRole).toString().isEmpty() ||
                item->data(NodeGraphNameRole).toString().toStdString() != id)
                continue;
            canvas->centerOn(item);
            break;
        }
        return true;
    }

    // Q-13. Whatever revision the tab now holds, the other panels hear of;
    // a move it could not follow while busy is followed now.
    void followDocument() {
        if (digest_hub) digest_hub->publish(model.digest(), &view);
        if (document_reread && model.canRefresh()) {
            document_reread = false;
            model.refresh();
        }
    }

    void documentMoved(const std::string &digest) {
        if (!model.contextReceived() || model.digest() == digest) return;
        // Busy with a read that may predate the move, or with an edit that
        // reads the document again when it ends: either way, once more after.
        if (!model.canRefresh()) {
            document_reread = true;
            return;
        }
        model.refresh();
        dispatch();
    }

    void deliverFailure(qint64 request_id, const QString &message) {
        if (request_id != 0 &&
            (request_id == pass_edit_request || request_id == pass_ticket_request)) {
            pass_edit_request = request_id == pass_edit_request ? 0 : pass_edit_request;
            pass_ticket_request = request_id == pass_ticket_request ? 0 : pass_ticket_request;
            pass_ticket.clear();
            pass_edit_rebuild = {};
            model.setEditNotice({InspectorNoticeKind::Error, message.toStdString()});
            showNotice();
            return;
        }
        const auto found = transport_requests.find(request_id);
        if (found == transport_requests.end()) return;
        const auto model_id = found.value();
        transport_requests.erase(found);
        model.receiveRpcFailure(model_id, message.toStdString());
        followDocument();
        dispatch();
    }

    void dispatch() {
        for (const auto &request : model.takeRpcRequests()) {
            QString error;
            qint64 transport = 0;
            if (request.method == "get_graph_instances" && graph.requestGraphInstances)
                transport = graph.requestGraphInstances(&error);
            else if (request.method == "get_graph_catalog" && graph.requestGraphCatalog)
                transport = graph.requestGraphCatalog(&error);
            else if (request.method == "get_graph_values" && graph.requestGraphValues)
                transport = graph.requestGraphValues(
                    qtext(request.params.value("instance", std::string{})), &error);
            else if (request.method == "get_graph_node_values" &&
                     graph.requestGraphNodeValues)
                transport = graph.requestGraphNodeValues(
                    qtext(request.params.value("instance", std::string{})), &error);
            else if (request.method == "get_status" && graph.requestStatus)
                transport = graph.requestStatus(&error);
            else if (request.method == "get_frame_plan" && request.params.contains("loop_elements")) {
                // LP-4e: a page of a draw_loop's materials.
                if (graph.requestLoopElements) {
                    const auto &page = request.params.at("loop_elements");
                    transport = graph.requestLoopElements(
                        qtext(page.value("node", std::string{})),
                        page.value("offset", qint64{0}), page.value("limit", qint64{256}), &error);
                }
            } else if (request.method == "get_frame_plan" && graph.requestFramePlan)
                transport = graph.requestFramePlan(&error);
            else if (request.method == "get_graph_templates" && graph.requestGraphTemplates)
                transport = graph.requestGraphTemplates(&error);
            else if (request.method == "get_engine_shaders" && graph.requestEngineShaders)
                transport = graph.requestEngineShaders(&error);
            else if (request.method == "get_render_authoring_context" &&
                     graph.requestRenderAuthoringContext)
                transport = graph.requestRenderAuthoringContext(&error);
            else if (request.method == "set_time" && graph.requestSetTime)
                transport = graph.requestSetTime(
                    request.params.value("t", 0.0), &error);
            else if (request.method == "render_frame" && graph.requestRenderFrame)
                transport = graph.requestRenderFrame(&error);
            else if (request.method == "edit_graph_document" &&
                     graph.requestEditGraphDocument)
                transport = graph.requestEditGraphDocument(
                    qtext(request.params.value("base_source_digest", std::string{})),
                    QByteArray::fromStdString(
                        request.params.value("ops", nlohmann::json::array()).dump()),
                    qtext(request.params.value("material_route_default", std::string{})),
                    &error);
            else if (request.method == "get_edit_result" && graph.requestEditResult)
                transport = graph.requestEditResult(
                    qtext(request.params.value("ticket", std::string{})), &error);
            else
                error = QObject::tr("The graph canvas cannot issue %1.")
                            .arg(qtext(request.method));
            if (transport != 0)
                transport_requests.insert(transport, request.request_id);
            else
                model.receiveRpcFailure(request.request_id, error.toStdString());
        }
        synchronize();
    }

    // --- Q-19, decision 4: renaming and deleting a definition.

    // What the two definition actions are about: the definition an open
    // menu names; else the one on screen; else the one the picked zone
    // shows (or whose node is picked). Empty when none.
    [[nodiscard]] std::string definitionTarget() const {
        if (definition_menu_target) return *definition_menu_target;
        if (!model.currentScope().empty()) return model.currentScope();
        const auto &selected = scene->selectedNode();
        if (!selected) return {};
        if (!scene->selectedScope().empty()) return scene->selectedScope();
        const auto &zones = scene->zones();
        const auto found = zones.find(*selected);
        return found == zones.end() ? std::string{} : found->second;
    }

    void syncDefinitionActions() {
        const bool available = model.canEdit() && !definitionTarget().empty();
        rename_definition_action->setEnabled(available);
        delete_definition_action->setEnabled(available);
    }

    void definitionMenu(const std::string &definition, const QPoint &global) {
        if (definition.empty()) return;
        QMenu menu;
        definition_menu_target = definition;
        rename_definition_action->setEnabled(model.canEdit());
        delete_definition_action->setEnabled(model.canEdit());
        menu.addAction(rename_definition_action);
        menu.addAction(delete_definition_action);
        menu.exec(global);
        definition_menu_target.reset();
        synchronize();
    }

    void openDefinitionRow(bool deleting) {
        const auto target = definitionTarget();
        if (target.empty() || !model.canEdit()) return;
        closeDefinitionRow();
        // Something still runs it: the notice says what, and nothing is sent
        // or asked.
        if (deleting && !model.definitionRemoveRefusal(target).empty()) {
            model.removeDefinition(target);
            showNotice();
            return;
        }
        definition_subject = target;
        definition_deleting = deleting;
        definition_caption->setText(
            deleting ? QObject::tr("Delete definition %1? Nothing runs it.").arg(qtext(target))
                     : QObject::tr("Rename definition %1 to").arg(qtext(target)));
        definition_name->setVisible(!deleting);
        definition_apply->setText(deleting ? QObject::tr("Delete") : QObject::tr("Rename"));
        definition_row->show();
        if (!deleting) {
            // UX-3, decision 4: a name Studio proposes -- after the node that
            // runs it -- chosen: typing replaces it, Enter takes it.
            definition_name->setText(qtext(model.proposeDefinitionName(target)));
            definition_name->selectAll();
            definition_name->setFocus(Qt::OtherFocusReason);
        }
        validateDefinitionRow();
    }

    void validateDefinitionRow() {
        if (definition_subject.empty()) return;
        std::string refusal;
        bool changed = true;
        if (!definition_deleting) {
            const auto name = definition_name->text().toStdString();
            refusal = model.definitionNameRefusal(definition_subject, name);
            changed = name != definition_subject;
        }
        definition_line->setText(qtext(refusal));
        definition_line->setVisible(!refusal.empty());
        definition_apply->setEnabled(model.canEdit() && refusal.empty() && changed);
    }

    void applyDefinitionRow() {
        if (definition_subject.empty()) return;
        if (!definition_deleting) {
            const auto name = definition_name->text().toStdString();
            // Checked before anything is sent: the line says why, and the
            // row stays open for another name.
            if (const auto refusal = model.definitionNameRefusal(definition_subject, name);
                !refusal.empty()) {
                validateDefinitionRow();
                return;
            }
            if (name != definition_subject) model.renameDefinition(definition_subject, name);
        } else {
            model.removeDefinition(definition_subject);
        }
        closeDefinitionRow();
        dispatch();
        showNotice();
    }

    void closeDefinitionRow() {
        definition_subject.clear();
        definition_line->clear();
        definition_line->hide();
        definition_row->hide();
    }

    void syncScopeControls() {
        const QSignalBlocker block{scopes};
        const auto current = qtext(model.currentScope());
        scopes->clear();
        for (const auto &scope : model.scopes())
            scopes->addItem(qtext(scope.label), qtext(scope.id));
        const int index = scopes->findData(current);
        scopes->setCurrentIndex(index < 0 ? 0 : index);
        QStringList trail;
        for (const auto &step : model.displayTrail()) trail.push_back(qtext(step));
        breadcrumb->setText(trail.join(QStringLiteral(" / ")));
        syncLoopBranch();
        syncDomainFilter();
    }

    // GL-1, decision 5: a root node another panel reveals is drawn, whatever
    // the filter held -- its domain is added to it.
    void showDomainOf(const std::string &scope, const std::string &id) {
        if (!scope.empty()) return;
        const auto nodes = model.nodes({});
        const auto found = std::ranges::find(nodes, id, &GraphCanvasNode::id);
        if (found != nodes.end() && !model.nodeShown({}, *found))
            model.setDomainShown(GraphCanvasModel::opDomain(found->op), true);
        syncDomainFilter();
    }

    // GL-1, decision 5: the boxes say what the model shows (a stub adds a
    // domain from the canvas).
    void syncDomainFilter() {
        for (const auto &[domain, box] : domain_boxes) {
            const QSignalBlocker block{box};
            box->setChecked(model.domainShown(domain));
        }
    }

    // Decision 4: the Frame Plan's Decompose into loop, or the Features
    // tab's Replace with loop version -- each with its own confirmation.
    void expandToEdit() {
        const auto expand = model.foldedExpand();
        if (expand.kind == "decompose" && decompose_action)
            decompose_action(qtext(expand.graph), qtext(expand.pass));
        else if (expand.kind == "replace" && replace_action)
            replace_action(qtext(expand.feature));
    }

    // GL-2b, decision 3: the Layers section for the root node picked, or for
    // the whole graph with nothing picked. Rebuilt only when what it says
    // changed, so a click on a row is not lost to a poll.
    std::string layers_shown_key;
    void syncLayers() {
        if (layers == nullptr || layers_toggle == nullptr || scene == nullptr) return;
        const auto &selected = scene->selectedNode();
        std::vector<GraphCanvasLayerRow> rows;
        QString heading;
        if (!selected) {
            rows = model.graphLayerRows();
            heading = QObject::tr("Layers: the whole graph");
        } else if (scene->selectedScope().empty() && model.currentScope().empty()) {
            rows = model.layerRows(*selected);
            heading = QObject::tr("Layers: %1").arg(qtext(*selected));
        }
        const bool open = layers_toggle->isChecked();
        layers_toggle->setText((open ? QStringLiteral("▾ ") : QStringLiteral("▸ ")) +
                               heading);
        layers_toggle->setVisible(!rows.empty());
        layers->setVisible(!rows.empty() && open);
        std::string key = heading.toStdString();
        for (const auto &row : rows) key += "\n" + row.layer + row.text + "|" + row.plan_node;
        if (key == layers_shown_key) return;
        layers_shown_key = std::move(key);
        layers->clear();
        const std::pair<const char *, QString> groups[] = {
            {"L1", QObject::tr("L1 expanded")},
            {"L2", QObject::tr("L2 physical")},
            {"L3", QObject::tr("L3 backend")},
        };
        for (const auto &[layer, title] : groups) {
            QTreeWidgetItem *group = nullptr;
            for (const auto &row : rows) {
                if (row.layer != layer) continue;
                if (group == nullptr) {
                    group = new QTreeWidgetItem(layers);
                    group->setText(0, title);
                    QFont font = group->font(0);
                    font.setBold(true);
                    group->setFont(0, font);
                }
                auto *item = new QTreeWidgetItem(group);
                item->setText(0, qtext(row.text));
                item->setToolTip(0, qtext(row.text));
                if (!row.plan_node.empty()) {
                    item->setData(0, Qt::UserRole, qtext(row.plan_node));
                    item->setToolTip(0, QObject::tr("%1\nClick to pick %2 in the Frame Plan.")
                                            .arg(qtext(row.text), qtext(row.plan_node)));
                }
            }
        }
        layers->expandAll();
    }

    // GL-2b, decision 3: the plan's diagnostics, one line that unfolds.
    void syncDiagnostics() {
        if (diagnostics_toggle == nullptr || diagnostics_list == nullptr) return;
        const auto &diagnostics = model.planDiagnostics();
        if (diagnostics.empty()) {
            diagnostics_toggle->hide();
            diagnostics_list->hide();
            return;
        }
        const bool open = diagnostics_toggle->isChecked();
        diagnostics_toggle->setText(
            (open ? QStringLiteral("▾ ") : QStringLiteral("▸ ")) +
            (diagnostics.size() == 1
                 ? QObject::tr("1 diagnostic")
                 : QObject::tr("%1 diagnostics").arg(static_cast<qulonglong>(diagnostics.size()))));
        diagnostics_toggle->show();
        QStringList lines;
        for (const auto &diagnostic : diagnostics)
            lines.push_back(QStringLiteral("%1 · %2: %3")
                                .arg(qtext(diagnostic.kind), qtext(diagnostic.subject),
                                     qtext(diagnostic.detail)));
        diagnostics_list->setText(lines.join(QLatin1Char('\n')));
        diagnostics_list->setVisible(open);
    }

    // Decision 1: a picked folded node, in one line: what it is, where it
    // comes from, what it reads and hands on.
    void syncFoldedSummary() {
        const auto &selected = scene->selectedNode();
        std::optional<GraphCanvasFoldedNode> found;
        if (selected && scene->selectedScope() == model.currentScope())
            for (auto &node : model.foldedNodes())
                if (node.id == *selected) found = std::move(node);
        if (!found) {
            folded_summary->clear();
            folded_summary->hide();
            return;
        }
        const auto join = [](const std::vector<std::string> &list) {
            QStringList names;
            for (const auto &name : list) names.push_back(qtext(name));
            return names.isEmpty() ? QObject::tr("nothing") : names.join(QStringLiteral(", "));
        };
        QString text = QObject::tr("%1 (%2): ").arg(qtext(found->label), qtext(found->type));
        if (found->feature_group)
            text += QObject::tr("%1 passes of %2. ")
                        .arg(static_cast<qulonglong>(found->passes.size()))
                        .arg(qtext(found->feature));
        text += QObject::tr("Reads %1; writes %2. ").arg(join(found->reads), join(found->writes));
        text += qtext(found->tooltip());
        if (found->enterable()) text += QObject::tr(" Double-click to look inside.");
        folded_summary->setText(text);
        folded_summary->show();
    }

    // LP-4c, decision 5: inside a loop's body, the branches that have one to
    // switch to, and the "shared" mark when another node runs it too.
    void syncLoopBranch() {
        const QSignalBlocker block{loop_branch};
        loop_branch->clear();
        const auto &entered = model.enteredLoop();
        const auto loop = entered.empty() ? std::nullopt : model.loop(entered);
        if (loop) {
            for (std::size_t index = 0; index < loop->branches.size(); ++index) {
                const auto &branch = loop->branches[index];
                if (branch.nothing || branch.body.empty()) continue;
                const bool last = index + 1 == loop->branches.size();
                QString condition = last ? QObject::tr("else")
                                         : branch.when == "element"
                                               ? QObject::tr("element = %1")
                                                     .arg(GraphCanvasLoopFields::integersText(branch.values))
                                         : branch.when == "element_in"
                                               ? QObject::tr("element in %1")
                                                     .arg(GraphCanvasLoopFields::integersText(branch.values))
                                         : branch.when == "element_not_in"
                                               ? QObject::tr("element not in %1")
                                                     .arg(GraphCanvasLoopFields::integersText(branch.values))
                                               : QObject::tr("expression");
                loop_branch->addItem(QObject::tr("%1: %2 → %3")
                                         .arg(qtext(entered), condition, qtext(branch.body)),
                                     static_cast<qulonglong>(index));
            }
            const int current = loop_branch->findData(
                static_cast<qulonglong>(model.loopBranch(entered)));
            loop_branch->setCurrentIndex(current < 0 ? 0 : current);
        }
        loop_branch->setVisible(loop.has_value() && loop_branch->count() > 0);
        const auto &scope = model.currentScope();
        const auto uses = scope.empty() ? std::size_t{0} : model.definitionUseCount(scope);
        shared_mark->setText(QObject::tr("shared (%1 users)").arg(uses));
        shared_mark->setToolTip(QObject::tr(
            "Other instances or loops run this definition too: an edit here changes them all."));
        shared_mark->setVisible(uses > 1);
    }

    // Decision 6: which other loop's p[] this input reads one element of, and
    // the element: `element` (the receiving loop's own) unless rewritten.
    void openPerIteration(const std::string &node, const std::string &port) {
        if (!model.canEdit()) return;
        const auto sources = model.perIterationSources(node);
        if (sources.empty()) return;
        QDialog dialog{&view};
        dialog.setObjectName(QStringLiteral("pelican.graphCanvasPerIteration"));
        dialog.setWindowTitle(QObject::tr("Per iteration: %1.%2").arg(qtext(node), qtext(port)));
        auto *layout = new QFormLayout(&dialog);
        auto *source = new QComboBox(&dialog);
        source->setObjectName(QStringLiteral("pelican.graphCanvasPerIteration.source"));
        for (const auto &[loop, output] : sources)
            source->addItem(QStringLiteral("%1.%2").arg(qtext(loop), qtext(output)));
        layout->addRow(QObject::tr("Reads"), source);
        // LP-4f1, decision 4: element, element - 1, element + 1, or Custom...
        auto *element = new GraphCanvasElementChoice(
            QStringLiteral("pelican.graphCanvasPerIteration.element"), &dialog);
        layout->addRow(QObject::tr("Element"), element);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                             &dialog);
        layout->addRow(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return;
        const auto index = static_cast<std::size_t>(source->currentIndex());
        if (index >= sources.size()) return;
        const auto expression = element->expression().toStdString();
        if (model.connectPerIteration(node, port, sources[index].first, sources[index].second,
                                      expression))
            dispatch();
    }

    void syncValueControls() {
        const QSignalBlocker block{value_instances};
        // The control reaches every scope now (AG-3b): get_graph_values still
        // answers only for a root instance, but get_graph_node_values answers
        // for that instance's definition body, which is what a person inside
        // one is looking at. The instance names whose generation to read.
        value_instances->setEnabled(true);
        const auto current = qtext(model.watchedInstance());
        value_instances->clear();
        value_instances->addItem(QObject::tr("(none)"), QString{});
        for (const auto &instance : model.valueInstances())
            value_instances->addItem(qtext(instance), qtext(instance));
        const int index = value_instances->findData(current);
        value_instances->setCurrentIndex(index < 0 ? 0 : index);
    }

    // Q-8. Read the project's UI document again. It is small, and reading it
    // when it is about to be offered means an edit made to it since the
    // project was opened is what the list shows.
    void reloadUiNodes() {
        if (project_root.empty()) {
            model.setUiNodes({});
            ui_nodes_error = "no project is open";
            return;
        }
        auto read = readProjectUiNodes(project_root);
        model.setUiNodes(std::move(read.nodes));
        ui_nodes_error = std::move(read.error);
    }

    void syncUiNodeRow(const GraphCanvasNode *node) {
        const auto key = node != nullptr
                             ? GraphCanvasModel::uiNodeParameter(node->op)
                             : std::string_view{};
        if (key.empty()) {
            ui_node_row->hide();
            return;
        }
        reloadUiNodes();
        const QSignalBlocker block{ui_node};
        ui_node->clear();
        for (const auto &path : model.uiNodeChoices(node->op))
            ui_node->addItem(qtext(path));
        const auto stored = node->parameters.find(std::string{key});
        const QString current =
            stored != node->parameters.end() && stored->is_string()
                ? qtext(stored->get<std::string>())
                : QString{};
        ui_node->setCurrentIndex(ui_node->findText(current));
        ui_node->setEditText(current);
        ui_node->lineEdit()->setPlaceholderText(
            ui_nodes_error.empty() ? QObject::tr("a UI node path")
                                   : qtext(ui_nodes_error));
        ui_node->setEnabled(model.canEdit());
        ui_node_row->show();
    }

    void commitUiNode(const QString &text) {
        const auto &selected = scene->selectedNode();
        if (!selected || text.isEmpty()) return;
        const auto scope = scene->selectedScope();
        const auto nodes = model.nodes(scope);
        const auto found = std::ranges::find(nodes, *selected, &GraphCanvasNode::id);
        if (found == nodes.end()) return;
        const auto key = std::string{GraphCanvasModel::uiNodeParameter(found->op)};
        if (key.empty()) return;
        const auto path = text.toStdString();
        const auto stored = found->parameters.find(key);
        if (stored != found->parameters.end() && *stored == path) return;
        if (model.setNodeParameter(scope, *selected, key, path)) dispatch();
    }

    // UX-2, decision 5: a parameter row of the details, opened for its value
    // as JSON.
    void beginParameterEdit(QTreeWidgetItem *item) {
        if (item == nullptr || !model.canEdit()) return;
        if (item->data(1, Qt::UserRole).toString().isEmpty()) return;
        const auto &selected = scene->selectedNode();
        if (!selected) return;
        editing_parameter_scope = scene->selectedScope();
        editing_parameter_node = *selected;
        editing_parameter = item;
        details->editItem(item, 1);
    }

    // One key of the node, from the text written in its row: JSON, or the
    // row says what the document holds again.
    void commitParameterText(const std::string &key, const QString &text) {
        const auto bytes = text.trimmed().toStdString();
        const auto parameters =
            model.nodeParameters(editing_parameter_scope, editing_parameter_node);
        if (!nlohmann::json::accept(bytes)) {
            model.setEditNotice({InspectorNoticeKind::Error,
                                 key + " was not changed: its value is not JSON."});
            showDetails();
            showNotice();
            return;
        }
        const auto value = nlohmann::json::parse(bytes);
        if (parameters.contains(key) && parameters.at(key) == value) {
            showDetails();
            return;
        }
        if (model.setNodeParameter(editing_parameter_scope, editing_parameter_node, key, value))
            dispatch();
        else
            showDetails();
    }

    // UX-5b, decision 1: the engine's shader list, once it arrived, is every
    // shader field's (the Pass Editor's too): handed to the shared list, and
    // this canvas's choices read again.
    std::uint64_t published_engine_shaders = 0;
    void publishEngineShaders() {
        if (published_engine_shaders == model.engineShadersRevision()) return;
        published_engine_shaders = model.engineShadersRevision();
        std::vector<EngineShaderListEntry> entries;
        for (const auto &shader : model.engineShaders()) {
            if (shader.stage != "vertex" && shader.stage != "fragment") continue;
            entries.push_back({shader.reference,
                               shader.stage == "vertex" ? FullscreenShaderStage::vertex
                                                        : FullscreenShaderStage::fragment,
                               shader.ports_by_hand});
        }
        setEngineShaderList(std::move(entries));
        refreshShaderChoices();
    }

    // UX-2, decision 2: the shaders a render pass picks from -- the Pass
    // Editor's list, read again whenever it is about to be offered, as the
    // project's sounds are.
    void refreshShaderChoices() {
        const auto root = project_root.empty()
                              ? std::optional<std::filesystem::path>{}
                              : std::optional<std::filesystem::path>{project_root};
        model.setShaderChoices(fullscreenShaderChoices(FullscreenShaderStage::vertex, root),
                               fullscreenShaderChoices(FullscreenShaderStage::fragment, root));
    }

    // UX-3, decision 1: the project's compute shaders (shaders/<name>.comp,
    // as project://shaders/<name>), which a new render.compute starts from,
    // in name order.
    void refreshComputeShaderChoices() {
        std::vector<std::string> names;
        if (!project_root.empty()) {
            std::error_code error;
            for (std::filesystem::directory_iterator entry{std::filesystem::path{project_root} /
                                                               "shaders",
                                                           error},
                 end;
                 !error && entry != end; entry.increment(error)) {
                std::error_code type_error;
                if (!entry->is_regular_file(type_error) ||
                    entry->path().extension().string() != ".comp")
                    continue;
                const auto stem = entry->path().stem().string();
                if (isValidShaderTemplateName(stem)) names.push_back(stem);
            }
        }
        std::ranges::sort(names);
        std::vector<std::string> references;
        for (const auto &name : names) references.push_back(shaderTemplateReference(name));
        model.setComputeShaderChoices(std::move(references));
    }

    // Q-10. The project's sounds, read again whenever they are about to be
    // offered, as the UI document is.
    void reloadAudioAssets() {
        auto read = readProjectAudioAssets(project_root);
        model.setAudioAssets(std::move(read.assets));
        audio_assets_error = std::move(read.error);
    }

    // One of bus / wrap: the engine's values, and the stored one kept visible
    // even when it is not among them (the engine refuses it by name). An
    // absent key shows the engine's default, which is the list's first.
    static void fillChoice(QComboBox &combo, const std::vector<std::string> &choices,
                           const nlohmann::json &parameters, const char *key) {
        combo.clear();
        for (const auto &choice : choices) combo.addItem(qtext(choice));
        const auto stored = parameters.find(key);
        QString current = qtext(choices.front());
        if (stored != parameters.end() && stored->is_string()) {
            current = qtext(stored->get<std::string>());
            if (combo.findText(current) < 0) combo.addItem(current);
        }
        combo.setCurrentIndex(combo.findText(current));
    }

    void syncAudioRow(const GraphCanvasNode *node) {
        if (node == nullptr || !GraphCanvasModel::isAudioPlayback(node->op)) {
            audio_row->hide();
            audio_row_node.clear();
            return;
        }
        const bool same_node = audio_row_node == node->id;
        if (!same_node) reloadAudioAssets();
        audio_row_node = node->id;
        const QSignalBlocker block_asset{audio_asset};
        const QSignalBlocker block_bus{audio_bus};
        const QSignalBlocker block_wrap{audio_wrap};
        if (!(same_node && audio_asset->lineEdit()->hasFocus())) {
            audio_asset->clear();
            for (const auto &asset : model.audioAssets()) audio_asset->addItem(qtext(asset));
            const auto stored = node->parameters.find("asset");
            const QString asset = stored != node->parameters.end() && stored->is_string()
                                      ? qtext(stored->get<std::string>())
                                      : QString{};
            audio_asset->setCurrentIndex(audio_asset->findText(asset));
            audio_asset->setEditText(asset);
        }
        audio_asset->lineEdit()->setPlaceholderText(
            audio_assets_error.empty() ? QObject::tr("project://....wav or .ogg")
                                       : qtext(audio_assets_error));
        fillChoice(*audio_bus, GraphCanvasModel::audioBusChoices(), node->parameters, "bus");
        fillChoice(*audio_wrap, GraphCanvasModel::audioWrapChoices(), node->parameters, "wrap");
        // A voice that follows `position` has no time map of its own: a wrap
        // there is audio.playback_mapping_conflict@1.
        const bool follows_position = std::ranges::any_of(node->inputs, [](const auto &port) {
            return port.port == "position" && port.connected;
        });
        const bool editable = model.canEdit();
        audio_asset->setEnabled(editable);
        audio_bus->setEnabled(editable);
        audio_wrap->setEnabled(editable && !follows_position);
        audio_wrap->setToolTip(
            follows_position
                ? QObject::tr("This voice follows its position input, so it has no wrap.")
                : QObject::tr("How the sound wraps when its events play it past the end."));
        audio_row->show();
    }

    void commitAudio(const char *key, const QString &text) {
        const auto &selected = scene->selectedNode();
        if (!selected || text.isEmpty()) return;
        const auto scope = scene->selectedScope();
        const auto nodes = model.nodes(scope);
        const auto found = std::ranges::find(nodes, *selected, &GraphCanvasNode::id);
        if (found == nodes.end() || !GraphCanvasModel::isAudioPlayback(found->op)) return;
        const auto value = text.toStdString();
        const auto stored = found->parameters.find(key);
        if (stored != found->parameters.end()) {
            if (*stored == value) return;
        } else {
            // Picking the default already in effect changes nothing, and
            // writing it would put a wrap on a node that may not take one.
            const std::string_view name{key};
            const auto *defaults = name == "bus"    ? &GraphCanvasModel::audioBusChoices()
                                   : name == "wrap" ? &GraphCanvasModel::audioWrapChoices()
                                                    : nullptr;
            if (defaults != nullptr && defaults->front() == value) return;
        }
        if (model.setNodeParameter(scope, *selected, key, value)) dispatch();
    }

    // The "Add node..." palette: the catalog, narrowed by domain and by what
    // the user types, and an instance also needs the definition it stands on.
    void openPalette() {
        // LP-4d1: the scope the addition goes into -- a zone's body when the
        // menu was opened inside it, or when the zone is picked.
        const std::string scope = scene->addScope();
        // The menu's scope is this palette's only, however it closes.
        struct PaletteScope {
            GraphCanvasScene *scene;
            ~PaletteScope() { scene->clearPaletteScope(); }
        } palette_scope{scene};
        if (!model.canEdit()) return;
        // A new ui.* sink is seeded with a node the UI document has.
        reloadUiNodes();
        // A new audio.playback, with a sound the project has.
        reloadAudioAssets();
        // UX-3, decision 1: a raster pass starts from the shaders the Pass
        // Editor lists, a compute task from one of the project's; a target
        // reference, a copy's pass and the materials come with the authoring
        // context, asked for now if it has not arrived.
        refreshShaderChoices();
        refreshComputeShaderChoices();
        model.askRenderTargetVocabulary();
        dispatch();
        QDialog dialog{&view};
        dialog.setObjectName(QStringLiteral("pelican.graphCanvasPalette"));
        dialog.setWindowTitle(QObject::tr("Add node"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *filters = new QHBoxLayout;
        auto *domains = new QComboBox(&dialog);
        domains->setObjectName(QStringLiteral("pelican.graphCanvasPaletteDomain"));
        domains->addItem(QObject::tr("all domains"), QString{});
        for (const auto &domain : model.paletteDomainsIn(scope))
            domains->addItem(qtext(domain), qtext(domain));
        filters->addWidget(domains);
        auto *search = new QLineEdit(&dialog);
        search->setObjectName(QStringLiteral("pelican.graphCanvasPaletteFilter"));
        search->setPlaceholderText(QObject::tr("filter by name"));
        filters->addWidget(search, 1);
        layout->addLayout(filters);
        auto *list = new QListWidget(&dialog);
        list->setObjectName(QStringLiteral("pelican.graphCanvasPaletteList"));
        layout->addWidget(list, 1);
        auto *definitions = new QComboBox(&dialog);
        definitions->setObjectName(
            QStringLiteral("pelican.graphCanvasPaletteDefinition"));
        for (const auto &scope : model.scopes())
            if (!scope.root) definitions->addItem(qtext(scope.label), qtext(scope.id));
        definitions->hide();
        layout->addWidget(definitions);
        // LP-4c: a loop's body can be a new definition, whose name is
        // proposed and can be changed before it exists.
        auto *new_name = new QLineEdit(qtext(model.suggestDefinitionName()), &dialog);
        new_name->setObjectName(QStringLiteral("pelican.graphCanvasPaletteNewDefinition"));
        new_name->setToolTip(QObject::tr("The name of the new, empty body definition"));
        new_name->hide();
        layout->addWidget(new_name);
        // LP-4e: a material pass arrives with the contract picked here, which
        // decides its sockets.
        auto *contracts = new QComboBox(&dialog);
        contracts->setObjectName(QStringLiteral("pelican.graphCanvasPaletteContract"));
        for (const auto &contract : GraphCanvasModel::materialPassContracts())
            contracts->addItem(qtext(contract), qtext(contract));
        contracts->setToolTip(QObject::tr("The material contract of the new pass"));
        contracts->hide();
        layout->addWidget(contracts);
        // UX-3, decision 1: what the other render nodes arrive with, picked
        // here -- a reference's target, a copy's pass, a buffer's element,
        // an attribute's domain and either one's count.
        auto *targets = new QComboBox(&dialog);
        targets->setObjectName(QStringLiteral("pelican.graphCanvasPaletteTarget"));
        targets->setToolTip(QObject::tr("The declared render target the reference reads; its "
                                        "image type follows the declaration"));
        targets->hide();
        layout->addWidget(targets);
        auto *snapshot_after = new QComboBox(&dialog);
        snapshot_after->setObjectName(QStringLiteral("pelican.graphCanvasPaletteSnapshotAfter"));
        snapshot_after->setToolTip(QObject::tr("The pass the copy is taken after; it copies "
                                               "what that pass wrote"));
        snapshot_after->hide();
        layout->addWidget(snapshot_after);
        // UX-5d, decision 2: what a compute task runs -- a compute shader of
        // the project's, or one of the engine's ray tracing pipelines.
        auto *compute_shader = new QComboBox(&dialog);
        compute_shader->setObjectName(QStringLiteral("pelican.graphCanvasPaletteComputeShader"));
        compute_shader->setToolTip(QObject::tr("The project's compute shaders, and the engine's "
                                               "ray tracing pipelines (raygen, miss and "
                                               "closesthit), whose ports the task arrives with"));
        compute_shader->hide();
        layout->addWidget(compute_shader);
        auto *buffer_row = new QWidget(&dialog);
        auto *buffer_layout = new QHBoxLayout(buffer_row);
        buffer_layout->setContentsMargins(0, 0, 0, 0);
        auto *element = new QComboBox(buffer_row);
        element->setObjectName(QStringLiteral("pelican.graphCanvasPaletteElement"));
        element->setToolTip(QObject::tr("What one element holds"));
        for (const auto &name : GraphCanvasModel::bufferElements())
            element->addItem(qtext(name), qtext(name));
        auto *attribute_domain = new QComboBox(buffer_row);
        attribute_domain->setObjectName(
            QStringLiteral("pelican.graphCanvasPaletteAttributeDomain"));
        attribute_domain->setToolTip(QObject::tr("One element per vertex, per instance, or an "
                                                 "index (uint)"));
        for (const auto &name : GraphCanvasModel::attributeDomains())
            attribute_domain->addItem(qtext(name), qtext(name));
        auto *count = new QSpinBox(buffer_row);
        count->setObjectName(QStringLiteral("pelican.graphCanvasPaletteCount"));
        count->setRange(1, 1 << 24);
        count->setValue(1);
        count->setPrefix(QObject::tr("× "));
        count->setToolTip(QObject::tr("How many elements"));
        buffer_layout->addWidget(element, 1);
        buffer_layout->addWidget(attribute_domain, 1);
        buffer_layout->addWidget(count, 1);
        buffer_row->hide();
        layout->addWidget(buffer_row);
        auto *seed_note = new QLabel(&dialog);
        seed_note->setObjectName(QStringLiteral("pelican.graphCanvasPaletteNote"));
        seed_note->setWordWrap(true);
        seed_note->setFont(studioSmallFont());
        seed_note->hide();
        layout->addWidget(seed_note);
        const QString new_definition_data = QStringLiteral("\x1f" "new");
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                                 QDialogButtonBox::Cancel,
                                             &dialog);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        const auto reload = [&] {
            list->clear();
            for (const auto &entry : model.paletteOperationsIn(
                     scope, search->text().toStdString(),
                     domains->currentData().toString().toStdString())) {
                auto *row = new QListWidgetItem(
                    entry.structural
                        ? QStringLiteral("%1   (structure)").arg(qtext(entry.op))
                        : QStringLiteral("%1   (%2)").arg(qtext(entry.op),
                                                          qtext(entry.domain)),
                    list);
                row->setData(Qt::UserRole, qtext(entry.op));
            }
            if (list->count() > 0) list->setCurrentRow(0);
        };
        const auto syncDefinition = [&] {
            auto *row = list->currentItem();
            const auto op = row != nullptr ? row->data(Qt::UserRole).toString() : QString{};
            const bool loop = op == QLatin1String{"graph.loop"};
            // Only a loop can bring a body of its own.
            const int offered = definitions->findData(new_definition_data);
            if (loop && offered < 0)
                definitions->insertItem(0, QObject::tr("New definition…"), new_definition_data);
            else if (!loop && offered >= 0)
                definitions->removeItem(offered);
            if (loop && offered < 0) definitions->setCurrentIndex(0);
            definitions->setVisible(loop || op == QLatin1String{"graph.instance"});
            new_name->setVisible(loop && definitions->currentData().toString() ==
                                             new_definition_data);
            contracts->setVisible(op == QLatin1String{"render.material_pass"});
            // UX-3, decision 1. The lists are read again as the row is
            // picked: the context may have arrived since the palette opened.
            const bool target_ref = op == QLatin1String{"render.target_ref"};
            const bool snapshot = op == QLatin1String{"render.snapshot_copy"};
            const bool buffer = op == QLatin1String{"render.buffer"};
            const bool attribute = op == QLatin1String{"render.attribute"};
            const bool compute = op == QLatin1String{"render.compute"};
            QString note;
            bool ready = row != nullptr;
            if (compute) {
                // UX-5d, decision 2: the engine's ray tracing pipelines are in
                // its shader list, asked for when a compute task is picked --
                // not as the palette opens: the engine takes seconds over it
                // and answers nothing else meanwhile, so a quick pick of
                // another node would time out behind it.
                if (!model.engineShadersKnown()) {
                    model.askEngineShaders();
                    dispatch();
                }
                const auto kept = compute_shader->currentData().toString();
                const QSignalBlocker block{compute_shader};
                compute_shader->clear();
                for (const auto &shader : model.computeShaderChoices())
                    compute_shader->addItem(qtext(shader), QStringLiteral("shader|") + qtext(shader));
                for (const auto &pipeline : model.rayTracingPipelines())
                    compute_shader->addItem(
                        QObject::tr("%1  (ray tracing)").arg(qtext(pipeline.reference)),
                        QStringLiteral("ray_tracing|") + qtext(pipeline.reference));
                compute_shader->setCurrentIndex(std::max(0, compute_shader->findData(kept)));
                if (compute_shader->count() == 0) {
                    ready = false;
                    note = model.engineShadersKnown()
                               ? QObject::tr("Neither the project (shaders/<name>.comp) nor the "
                                             "engine has a shader a compute task can run.")
                               : QObject::tr("Reading the engine's ray tracing pipelines...");
                }
            }
            if (target_ref) {
                const auto kept = targets->currentData().toString();
                const QSignalBlocker block{targets};
                targets->clear();
                for (const auto &target : model.declaredRenderTargets()) {
                    targets->addItem(qtext(target.name), qtext(target.name));
                    targets->setItemData(targets->count() - 1,
                                         QObject::tr("declared by: %1").arg(qtext(target.source)),
                                         Qt::ToolTipRole);
                }
                targets->setCurrentIndex(std::max(0, targets->findData(kept)));
                if (targets->count() == 0) {
                    ready = false;
                    note = QObject::tr("No render target is declared yet: declare one (Frame "
                                       "Plan > New render target...) or use render.image.");
                }
            }
            if (snapshot) {
                const auto kept = snapshot_after->currentData().toString();
                const QSignalBlocker block{snapshot_after};
                snapshot_after->clear();
                for (const auto &pass : model.placePasses())
                    snapshot_after->addItem(qtext(pass), qtext(pass));
                const int index = snapshot_after->findData(kept);
                // The last pass of the frame by default: a copy of the frame
                // as it ends.
                snapshot_after->setCurrentIndex(index >= 0 ? index : snapshot_after->count() - 1);
                if (snapshot_after->count() == 0) {
                    ready = false;
                    note = QObject::tr("The frame plan lists no pass to copy after yet.");
                }
            }
            targets->setVisible(target_ref);
            snapshot_after->setVisible(snapshot);
            compute_shader->setVisible(compute);
            buffer_row->setVisible(buffer || attribute);
            attribute_domain->setVisible(attribute);
            element->setEnabled(!(attribute && attribute_domain->currentData().toString() ==
                                                   QLatin1String{"index"}));
            seed_note->setText(note);
            seed_note->setVisible(!note.isEmpty());
            if (auto *ok = buttons->button(QDialogButtonBox::Ok)) ok->setEnabled(ready);
        };
        QObject::connect(attribute_domain, &QComboBox::currentIndexChanged, &dialog,
                         [&] { syncDefinition(); });
        QObject::connect(definitions, &QComboBox::currentIndexChanged, &dialog,
                         [&] { syncDefinition(); });
        QObject::connect(search, &QLineEdit::textChanged, &dialog, [&] {
            reload();
            syncDefinition();
        });
        QObject::connect(domains, &QComboBox::currentIndexChanged, &dialog, [&] {
            reload();
            syncDefinition();
        });
        QObject::connect(list, &QListWidget::currentRowChanged, &dialog,
                         [&] { syncDefinition(); });
        QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog,
                         &QDialog::accept);
        // GL-1d, decision 5: what is typed as the palette opens is the
        // filter's; the arrows walk the list; Enter is OK and Esc Cancel.
        search->installEventFilter(new PaletteFilterKeys(list, search));
        QObject::connect(search, &QLineEdit::returnPressed, &dialog, &QDialog::accept);
        reload();
        syncDefinition();
        // UX-5d, decision 2: the engine's list arrives while the palette is
        // open; the rows and the compute list are read again then, the row
        // in hand kept.
        QTimer engine_shaders_arrive{&dialog};
        engine_shaders_arrive.setInterval(200);
        auto shaders_seen = model.engineShadersRevision();
        QObject::connect(&engine_shaders_arrive, &QTimer::timeout, &dialog, [&] {
            if (model.engineShadersRevision() == shaders_seen) return;
            shaders_seen = model.engineShadersRevision();
            auto *current = list->currentItem();
            const auto kept = current != nullptr ? current->data(Qt::UserRole).toString() : QString{};
            reload();
            for (int index = 0; index < list->count(); ++index)
                if (list->item(index)->data(Qt::UserRole).toString() == kept) list->setCurrentRow(index);
            syncDefinition();
        });
        engine_shaders_arrive.start();
        search->setFocus(Qt::PopupFocusReason);
        QTimer::singleShot(0, search, [search] { search->setFocus(Qt::PopupFocusReason); });
        if (dialog.exec() != QDialog::Accepted) return;
        auto *row = list->currentItem();
        if (row == nullptr) return;
        const auto op = row->data(Qt::UserRole).toString().toStdString();
        if (op == "graph.loop" && definitions->currentData().toString() == new_definition_data) {
            const auto name = new_name->text().trimmed().toStdString();
            scene->addLoop({}, name.empty() ? model.suggestDefinitionName() : name);
            return;
        }
        // LP-4e: a material pass's second argument is its contract.
        if (op == "render.material_pass") {
            scene->addFromPalette(scope, op, contracts->currentData().toString().toStdString());
            return;
        }
        // UX-3, decision 1: what was picked beside the op.
        nlohmann::json seed = nlohmann::json::object();
        if (op == "render.target_ref")
            seed["name"] = targets->currentData().toString().toStdString();
        else if (op == "render.snapshot_copy")
            seed["snapshot_after"] = snapshot_after->currentData().toString().toStdString();
        else if (op == "render.compute") {
            // UX-5d, decision 2: "shader|<reference>" or "ray_tracing|<reference>".
            const auto picked = compute_shader->currentData().toString();
            const auto bar = picked.indexOf(QLatin1Char{'|'});
            if (bar > 0) seed[picked.left(bar).toStdString()] = picked.mid(bar + 1).toStdString();
        }
        else if (op == "render.buffer" || op == "render.attribute") {
            seed["element"] = element->currentData().toString().toStdString();
            seed["count"] = count->value();
            if (op == "render.attribute")
                seed["domain"] = attribute_domain->currentData().toString().toStdString();
        }
        if (!seed.empty()) {
            scene->addFromPalette(scope, op, {}, seed);
            return;
        }
        scene->addFromPalette(scope, op, definitions->currentData().toString().toStdString());
    }

    void showDetails() {
        syncFoldedSummary();
        syncLayers();
        // UX-2: a row being written goes with the rows.
        editing_parameter = nullptr;
        details->clear();
        const auto &selected = scene->selectedNode();
        label_edit->setEnabled(selected.has_value() && model.canEdit());
        apply_label->setEnabled(selected.has_value() && model.canEdit());
        add_node->setEnabled(model.canEdit());
        if (!selected) {
            label_edit->clear();
            syncUiNodeRow(nullptr);
            syncAudioRow(nullptr);
            loop_fields->showLoop({});
            draw_fields->showNode(nullptr);
            view_fields->showNode(nullptr, {});
            image_fields->showNode(nullptr, {});
            pass_fields->showNode(nullptr, {});
            place_fields->showNode(nullptr, {});
            return;
        }
        // LP-4d1: the node is looked for in the scope it was picked in -- a
        // zone's body is its definition, whatever the canvas shows.
        const auto scope = scene->selectedScope();
        const auto nodes = model.nodes(scope);
        const auto found = std::ranges::find(nodes, *selected, &GraphCanvasNode::id);
        if (found == nodes.end()) {
            label_edit->clear();
            syncUiNodeRow(nullptr);
            syncAudioRow(nullptr);
            loop_fields->showLoop({});
            draw_fields->showNode(nullptr);
            view_fields->showNode(nullptr, {});
            image_fields->showNode(nullptr, {});
            pass_fields->showNode(nullptr, {});
            place_fields->showNode(nullptr, {});
            return;
        }
        label_edit->setText(qtext(found->label));
        syncUiNodeRow(&*found);
        syncAudioRow(&*found);
        // LP-4c: a loop's parameters are its fields, not rows of JSON.
        loop_fields->showLoop(found->loop ? found->id : std::string{});
        // LP-4e: and so are a material pass's and a draw step's.
        draw_fields->showNode(&*found, scope);
        // LP-4f1: and a render node's views.
        view_fields->showNode(&*found, scope);
        // UX-2: and a render.image's values, a render pass's shaders.
        if (found->op == "render.fullscreen" || found->op == "render.raster") refreshShaderChoices();
        image_fields->showNode(&*found, scope);
        pass_fields->showNode(&*found, scope);
        place_fields->showNode(&*found, scope);
        const auto row = [this](const QString &field, const QString &value) {
            auto *item = new QTreeWidgetItem(details);
            item->setText(0, field);
            item->setText(1, value);
            return item;
        };
        // N-7 rule 4: the heading, the operation, the domain and the
        // evaluation were four rows carrying one sentence. They are one row
        // -- label, op and domain on one line -- and the evaluation, which no
        // canvas gesture depends on, is that row's tooltip.
        const QString unknown = QObject::tr("(catalog not fetched)");
        const QString heading =
            found->label.empty() ? qtext(found->id) : qtext(found->label);
        const QString domain =
            found->contract_known ? qtext(found->domain) : unknown;
        const QString evaluation =
            found->contract_known ? qtext(found->evaluation) : unknown;
        auto *node_row = row(QObject::tr("node"),
                             QStringLiteral("%1 \u00b7 %2 \u00b7 %3")
                                 .arg(heading, qtext(found->op), domain));
        node_row->setToolTip(
            1, QObject::tr("label: %1\ntype: %2\ndomain: %3\n"
                           "evaluation: %4\nid: %5")
                   .arg(heading, qtext(found->op), domain, evaluation,
                        qtext(found->id)));
        row(QObject::tr("id"), qtext(found->id));
        row(QObject::tr("sink"),
            found->sink ? QObject::tr("yes") : QObject::tr("no"));
        if (!found->definition.empty())
            row(QObject::tr(found->loop ? "body" : "definition"), qtext(found->definition));
        if (found->refused)
            row(QObject::tr("refused"), qtext(found->refusal))->setToolTip(1, qtext(found->refusal));
        if (!found->parameters.empty() && !found->loop) {
            auto *parameters = row(QObject::tr("parameters"), {});
            for (const auto &[name, value] : found->parameters.items()) {
                auto *entry = new QTreeWidgetItem(parameters);
                entry->setText(0, qtext(name));
                entry->setText(1, qtext(value.dump()));
                // UX-2, decision 5: the key, so a double-click edits it.
                entry->setData(1, Qt::UserRole, qtext(name));
                entry->setFlags(entry->flags() | Qt::ItemIsEditable);
                entry->setToolTip(1, QObject::tr("Double-click to write the value as JSON."));
            }
            parameters->setExpanded(true);
        }
        // The port table: name, type, where the supply comes from, and what it
        // is. The type column is the same fact the socket's shape encodes.
        const auto type_text = [](const std::string &type) {
            return type.empty() ? QObject::tr("(not declared)") : qtext(type);
        };
        if (!found->inputs.empty()) {
            auto *inputs = row(QObject::tr("inputs"), {});
            for (const auto &port : found->inputs) {
                auto *entry = new QTreeWidgetItem(inputs);
                entry->setText(0, qtext(port.port));
                entry->setText(1, QStringLiteral("%1  ·  %2")
                                      .arg(type_text(port.type),
                                           portSummary(port)));
                entry->setToolTip(0, port.declared
                                         ? QObject::tr("declared by the catalog")
                                         : QObject::tr("read from the document"));
            }
            inputs->setExpanded(true);
        }
        if (!found->outputs.empty()) {
            auto *outputs = row(QObject::tr("outputs"), {});
            for (const auto &port : found->outputs) {
                auto *entry = new QTreeWidgetItem(outputs);
                entry->setText(0, qtext(port.port));
                const QString type = port.bundle
                                         ? QObject::tr("one per element: %1").arg(type_text(port.type))
                                         : type_text(port.type);
                entry->setText(1,
                    port.has_value
                        ? QStringLiteral("%1  ·  %2").arg(type, numberText(port.value))
                        : type);
            }
            outputs->setExpanded(true);
        }
    }

    // One line that says what the strip is showing, so a reader -- and a
    // scenario -- can ask the panel what it holds instead of judging pixels.
    void synchronizeTimeline() {
        const QSignalBlocker block{live};
        live->setChecked(model.live());
        const auto &samples = model.timelineSamples();
        const auto &ports = model.timelinePorts();
        QStringList traces;
        for (const auto &port : ports)
            traces.push_back(QStringLiteral("%1.%2")
                                 .arg(qtext(model.timelineNode()), qtext(port)));
        QString summary;
        if (model.timelineNode().empty())
            summary = QObject::tr("no node selected");
        else if (samples.empty())
            summary = QObject::tr("%1: 0 samples")
                          .arg(qtext(model.timelineNode()));
        else
            summary = QObject::tr("%1: %2 samples · %3 · t %4 to %5")
                          .arg(qtext(model.timelineNode()))
                          .arg(samples.size())
                          .arg(traces.join(QStringLiteral(", ")))
                          .arg(numberText(samples.front().time),
                               numberText(samples.back().time));
        const auto sinks = model.animationSinks();
        if (!sinks.empty()) {
            const auto &sink = sinks.front();
            summary += QObject::tr(" · sink %1 (%2)")
                           .arg(qtext(sink.node), qtext(sink.source));
        }
        timeline_summary->setText(summary);
        if (timeline_panel->isVisible()) timeline->update();
    }

    void synchronize() {
        const bool history_available = !scene->editingInline() &&
                                       !scene->dragging() && !scene->layoutPending();
        // GL-1e: the render edit history is reached through these too.
        undo_action->setEnabled(history_available && scene->canUndoAny());
        redo_action->setEnabled(history_available && scene->canRedoAny());
        add_loop_action->setEnabled(model.canEdit() && model.currentScope().empty());
        add_fullscreen_action->setEnabled(
            model.canEdit() &&
            !model.paletteOperationsIn(scene->addScope(), "render.fullscreen").empty());
        // Q-19: the definition actions follow what they would be about.
        syncDefinitionActions();
        if (!definition_subject.empty()) {
            const auto known = model.scopes();
            if (std::ranges::none_of(known, [&](const GraphCanvasScope &scope) {
                    return !scope.root && scope.id == definition_subject;
                }))
                closeDefinitionRow();
            else
                validateDefinitionRow();
        }
        const bool content_changed = rendered_content != model.contentRevision();
        const bool report_changed = rendered_report != model.reportRevision();
        // A redraw deletes the in-place editor, so it waits for the editor to
        // close. Live reads four times a second (AG-3b); without this, typing
        // a value while the overlay is following the engine would be
        // impossible, and the reason would be invisible. Nothing is lost: the
        // rendered marks are left behind, so the next tick draws it. N-6 adds
        // the other half of the same rule: a drag in progress is a hand on an
        // item, and rebuilding would delete what it is holding.
        const bool placement_changed =
            rendered_placement != model.placementRevision();
        if ((content_changed || report_changed || placement_changed) &&
            !scene->editingInline() && !scene->dragging() && editing_parameter == nullptr) {
            rendered_content = model.contentRevision();
            rendered_report = model.reportRevision();
            rendered_placement = model.placementRevision();
            publishEngineShaders();
            // Q-5: a read that only moved the placement -- the reply to the
            // drag that was just made -- keeps the scene when the scene is
            // already drawing it. Rebuilding there threw every item away and
            // drew it again a moment after the person had gone on to use it.
            const bool redraw = content_changed || report_changed ||
                                !scene->followDocumentPlacement();
            if (redraw) {
                scene->repopulate();
                syncScopeControls();
                syncValueControls();
                showDetails();
            }
        }
        // Q-17: an edit or a read in flight changes no revision, so the Loop
        // fields are not rebuilt for it; they follow it here instead.
        if (loop_fields != nullptr) loop_fields->followEditable();
        // Q-18: and so do the draw.* fields and a render node's View field.
        if (draw_fields != nullptr) draw_fields->followEditable();
        if (view_fields != nullptr) view_fields->followEditable();
        // UX-2: and so do a render.image's and a render pass's.
        if (image_fields != nullptr) image_fields->followEditable();
        if (pass_fields != nullptr) pass_fields->followEditable();
        if (place_fields != nullptr) place_fields->followEditable();
        showNotice();
        refresh->setEnabled(model.canRefresh());
        synchronizeTimeline();
    }

    void showNotice() {
        auto message = model.notice();
        // Q-5: a placement change nobody has sent yet is not saved, whatever
        // the last save said. Only an error outranks it: that is the one thing
        // the user must still read.
        if (scene->placementUnsent() && message.kind != InspectorNoticeKind::Error)
            message = {InspectorNoticeKind::Information,
                       "Saving the graph change..."};
        if (message.kind == InspectorNoticeKind::None) {
            notice->clear();
            notice->hide();
        } else {
            const QString whole = qtext(message.message);
            notice->setToolTip(whole);
            // GL-1b, decision 3: the engine's sentence is long; what would
            // have applied the change is kept on the line whatever is cut.
            // GL-1e, decision 3: so is why a pass's input edit was refused.
            // UX-5a: a refusal's first line is its sentence; what follows it
            // is the engine's whole message, for the tooltip.
            const QString first = whole.section(QLatin1Char('\n'), 0, 0);
            QString hint = qtext(std::string{GraphCanvasModel::restartNotOfferedHint});
            if (!first.endsWith(hint)) hint = qtext(std::string{passRunsAfterHint});
            notice->setText(first.endsWith(hint) ? oneLine(first.chopped(hint.size())) + hint
                                                 : oneLine(first));
            notice->setStyleSheet(message.kind == InspectorNoticeKind::Error
                                      ? QStringLiteral("color: #c0392b;")
                                      : QStringLiteral("color: #7f8c8d;"));
            notice->show();
        }
        // GL-2b: the plan the bands and the Layers section read may have
        // just arrived.
        syncDiagnostics();
        syncLayers();
        // GL-1, decision 4: nothing was sent; the link is the way to edit.
        const auto expand = model.foldedExpand();
        expand_action->setEnabled(model.foldedView() && !expand.kind.empty());
        if (model.foldedView() && model.foldedEditRefused()) {
            QString line = qtext(std::string{GraphCanvasModel::foldedEditNotice}).toHtmlEscaped();
            if (!expand.kind.empty())
                line += QStringLiteral(" <a href=\"expand\">%1</a>")
                            .arg(QObject::tr("Expand to edit…").toHtmlEscaped());
            folded_notice->setText(line);
            folded_notice->setToolTip(expand.kind == "decompose"
                                          ? QObject::tr("Decompose into loop… (the Frame Plan's)")
                                      : expand.kind == "replace"
                                          ? QObject::tr("Replace with loop version… (the Features tab's)")
                                          : QString{});
            folded_notice->show();
        } else {
            folded_notice->clear();
            folded_notice->hide();
        }
    }
};

GraphCanvasWidget::GraphCanvasWidget(GraphCanvasReadCapability graph,
                                     QWidget *parent)
    : QWidget{parent}, impl_{std::make_unique<Impl>(*this, std::move(graph))} {
    setObjectName(QStringLiteral("pelican.graphCanvas"));
    // UX-5b (EL-4b): a shader list opened anywhere (the Pass Editor's too)
    // asks the engine through this canvas's connection.
    setEngineShaderListRequester([impl = impl_.get()] {
        impl->model.askEngineShaders();
        impl->dispatch();
    });
}

GraphCanvasWidget::~GraphCanvasWidget() { setEngineShaderListRequester({}); }

void GraphCanvasWidget::refresh() {
    impl_->model.refresh();
    impl_->dispatch();
}

void GraphCanvasWidget::poll() {
    impl_->model.poll();
    impl_->dispatch();
}

bool GraphCanvasWidget::revealPlanPass(const QString &pass) {
    auto &model = impl_->model;
    // GL-2b, decision 4: where the plan's origin says the node is -- a loop's
    // copy in its zone on the element's tab, a node inside an instance
    // through that instance, else a root node or a folded one.
    const auto target = model.planNodeTarget(pass.toStdString());
    if (target.kind.empty()) return false;
    if (target.kind == "loop")
        return revealLoopOrigin(qtext(target.loop), static_cast<qint64>(target.element),
                                qtext(target.node));
    if (target.kind == "instance")
        return revealGraphNode(qtext(target.scope), qtext(target.node),
                               qtext(target.instance_path));
    const auto &id = target.node;
    // The real root, with the domain the node is in shown.
    model.selectScope({});
    const auto nodes = model.nodes({});
    if (const auto graph = std::ranges::find(nodes, id, &GraphCanvasNode::id); graph != nodes.end()) {
        if (!model.nodeShown({}, *graph))
            model.setDomainShown(GraphCanvasModel::opDomain(graph->op), true);
    } else {
        model.setDomainShown("render", true);
    }
    impl_->scene->repopulate();
    impl_->scene->selectNode(id);
    impl_->syncScopeControls();
    impl_->syncValueControls();
    impl_->showDetails();
    impl_->dispatch();
    for (QGraphicsItem *item : impl_->scene->items()) {
        if (item->data(NodeGraphItemKindRole).toString() != QLatin1String{GraphCanvasNodeItem} ||
            !item->data(GraphCanvasScopeRole).toString().isEmpty() ||
            !item->data(GraphCanvasZoneScopeRole).toString().isEmpty() ||
            item->data(NodeGraphNameRole).toString().toStdString() != id)
            continue;
        impl_->canvas->centerOn(item);
        break;
    }
    return true;
}

void GraphCanvasWidget::setFramePlanNodeAction(std::function<void(const QString &node)> action) {
    impl_->frame_plan_node_action = std::move(action);
}

void GraphCanvasWidget::setFoldedExpandActions(
    std::function<void(const QString &graph, const QString &pass)> decompose,
    std::function<void(const QString &feature)> replace) {
    impl_->decompose_action = std::move(decompose);
    impl_->replace_action = std::move(replace);
}

bool GraphCanvasWidget::revealNode(const QString &node) {
    const auto id = node.toStdString();
    // The Inspector names a producer in the root document, so the trail comes
    // back to the root before the node is looked for.
    impl_->model.selectScope({});
    const auto nodes = impl_->model.nodes();
    if (std::ranges::none_of(nodes, [&](const GraphCanvasNode &drawn) {
            return drawn.id == id;
        }))
        return false;
    impl_->showDomainOf({}, id);
    impl_->scene->repopulate();
    impl_->scene->selectNode(id);
    impl_->syncScopeControls();
    impl_->syncValueControls();
    impl_->showDetails();
    return true;
}

namespace {

// The engine spells the root document "root" (the token its edit ops take);
// the canvas files it under the empty id.
std::string canvasScopeId(const QString &scope) {
    auto token = scope.toStdString();
    return token == "root" ? std::string{} : token;
}

} // namespace

bool GraphCanvasWidget::canRevealGraphNode(const QString &scope,
                                           const QString &node) const {
    // GL-1: the question is about the real document; a folded view on
    // screen gives way to it (the reveal that follows draws the scene).
    impl_->model.leaveFolded();
    const auto &model = impl_->model;
    const auto target = canvasScopeId(scope);
    const auto id = node.toStdString();
    if (!target.empty() &&
        std::ranges::none_of(model.scopes(), [&](const GraphCanvasScope &known) {
            return known.id == target;
        }))
        return false;
    return std::ranges::any_of(model.nodes(target), [&](const GraphCanvasNode &drawn) {
        return drawn.id == id;
    });
}

bool GraphCanvasWidget::revealGraphNode(const QString &scope,
                                        const QString &node,
                                        const QString &instance) {
    if (!canRevealGraphNode(scope, node)) return false;
    auto &model = impl_->model;
    const auto target = canvasScopeId(scope);
    const auto id = node.toStdString();

    // The instance's context: descend the path from the root, the way a
    // double-click on each instance does, so the breadcrumb and the watched
    // instance are the ones that placed the node. A path that does not lead
    // to the scope opens the scope directly instead.
    bool through_instance = false;
    const auto path = instance.toStdString();
    if (!path.empty() && !target.empty()) {
        model.selectScope({});
        through_instance = true;
        std::string_view rest{path};
        while (through_instance && !rest.empty()) {
            const auto slash = rest.find('/');
            const auto step = rest.substr(0, slash);
            rest = slash == std::string_view::npos ? std::string_view{}
                                                   : rest.substr(slash + 1);
            through_instance = model.enterInstance(step);
        }
        through_instance = through_instance && model.currentScope() == target;
    }
    if (!through_instance) {
        model.selectScope(target);
        // Still the instance whose generation fed this body, when the Values
        // from control offers it.
        const auto root_instance = path.substr(0, path.find('/'));
        const auto offered = model.valueInstances();
        if (!target.empty() && !root_instance.empty() &&
            std::ranges::find(offered, root_instance) != offered.end())
            model.watchInstance(root_instance);
    }
    impl_->showDomainOf(model.currentScope(), id);
    impl_->scene->repopulate();
    impl_->scene->selectNode(id);
    impl_->syncScopeControls();
    impl_->syncValueControls();
    impl_->showDetails();
    impl_->dispatch();
    for (QGraphicsItem *item : impl_->scene->items()) {
        if (item->data(NodeGraphItemKindRole).toString() !=
                QLatin1String{GraphCanvasNodeItem} ||
            item->data(GraphCanvasScopeRole).toString() !=
                QString::fromStdString(impl_->model.currentScope()) ||
            item->data(NodeGraphNameRole).toString() != node)
            continue;
        impl_->canvas->centerOn(item);
        break;
    }
    return true;
}

bool GraphCanvasWidget::revealLoopOrigin(const QString &loop, qint64 element,
                                         const QString &node) {
    auto &model = impl_->model;
    auto &scene = *impl_->scene;
    const auto id = node.toStdString();
    const auto loop_id = loop.toStdString();
    // LP-4d2, decision 6: on the root canvas, in the zone, with the tab on
    // the branch that element ran.
    if (const auto body = model.showLoopOrigin(loop_id, element, id); !body.empty()) {
        if (scene.collapsed(std::string{}, loop_id))
            scene.setCollapsed(std::string{}, loop_id, false);
        scene.repopulate();
        const auto zone = scene.zones().find(loop_id);
        if (zone != scene.zones().end() && zone->second == body) {
            scene.selectNode(body, id);
            impl_->syncScopeControls();
            impl_->syncValueControls();
            impl_->showDetails();
            impl_->dispatch();
            for (QGraphicsItem *item : scene.items()) {
                if (item->data(NodeGraphItemKindRole).toString() !=
                        QLatin1String{GraphCanvasNodeItem} ||
                    item->data(GraphCanvasScopeRole).toString() != QString::fromStdString(body) ||
                    item->data(NodeGraphNameRole).toString() != node)
                    continue;
                impl_->canvas->centerOn(item);
                break;
            }
            return true;
        }
    }
    // A body no zone on the root canvas shows (another loop's zone draws a
    // definition they share): entered through the loop, as LP-4c did.
    if (!model.enterLoopOrigin(loop_id, element, id)) return false;
    impl_->scene->repopulate();
    impl_->scene->selectNode(id);
    impl_->syncScopeControls();
    impl_->syncValueControls();
    impl_->showDetails();
    impl_->dispatch();
    for (QGraphicsItem *item : impl_->scene->items()) {
        if (item->data(NodeGraphItemKindRole).toString() !=
                QLatin1String{GraphCanvasNodeItem} ||
            item->data(GraphCanvasScopeRole).toString() !=
                QString::fromStdString(impl_->model.currentScope()) ||
            item->data(NodeGraphNameRole).toString() != node)
            continue;
        impl_->canvas->centerOn(item);
        break;
    }
    return true;
}

void GraphCanvasWidget::setFramePlanLoopAction(std::function<bool(const QString &loop)> action) {
    impl_->frame_plan_loop_action = std::move(action);
}

void GraphCanvasWidget::setPlanCommitAction(std::function<void()> action) {
    impl_->plan_commit_action = std::move(action);
}

void GraphCanvasWidget::setRenderHistoryActions(QAction *undo, QAction *redo) {
    impl_->render_undo = undo;
    impl_->render_redo = redo;
    auto &scene = *impl_->scene;
    const auto fire = [](QAction *action) {
        if (action == nullptr || !action->isEnabled()) return false;
        action->trigger();
        return true;
    };
    scene.renderUndo = [undo, fire] { return fire(undo); };
    scene.renderRedo = [redo, fire] { return fire(redo); };
    scene.renderUndoAvailable = [undo] { return undo != nullptr && undo->isEnabled(); };
    scene.renderRedoAvailable = [redo] { return redo != nullptr && redo->isEnabled(); };
    // The tab's buttons follow what the history can do as well.
    for (QAction *action : {undo, redo})
        if (action != nullptr)
            connect(action, &QAction::changed, this, [impl = impl_.get()] { impl->synchronize(); });
    impl_->synchronize();
}

void GraphCanvasWidget::setEngineRestartAction(std::function<void(const QString &reason)> action) {
    impl_->engine_restart_action = std::move(action);
}

void GraphCanvasWidget::engineRestartDeclined(const QString &message) {
    if (!std::exchange(impl_->restart_asked, false)) return;
    impl_->model.engineRestartDeclined(message.toStdString());
    impl_->dispatch();
}

void configureGraphCanvasView(NodeCanvasView &view) {
    view.setDragMode(QGraphicsView::RubberBandDrag);
    view.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    view.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
}

bool openGraphNodeInTabs(QTabWidget &tabs, GraphCanvasWidget &graph,
                         const QString &scope, const QString &node,
                         const QString &instance) {
    if (!graph.canRevealGraphNode(scope, node)) return false;
    // The tab first, so the view the node is centred in is the one on screen.
    tabs.setCurrentWidget(&graph);
    return graph.revealGraphNode(scope, node, instance);
}

void GraphCanvasWidget::revealNodeWhenPresent(const QString &node) {
    impl_->pending_reveal = node.toStdString();
    if (impl_->revealPending()) return;
    // The document in hand predates the edit: read it again, now or when
    // what is in flight lands.
    if (impl_->model.canRefresh()) {
        impl_->model.refresh();
        impl_->dispatch();
    } else {
        impl_->document_reread = true;
    }
}

void GraphCanvasWidget::setConnected(bool connected) {
    impl_->model.setConnected(connected);
    impl_->dispatch();
}

void GraphCanvasWidget::receiveResult(qint64 request_id,
                                      const QByteArray &result_json) {
    impl_->deliverResult(request_id, result_json);
}

void GraphCanvasWidget::receiveFailure(qint64 request_id,
                                       const QString &message) {
    impl_->deliverFailure(request_id, message);
}

void GraphCanvasWidget::setProjectRoot(const std::filesystem::path &project_root) {
    // OT-1: the graph history survives an engine restart, not a project
    // switch.
    if (impl_->project_root != project_root) impl_->model.forgetHistory();
    impl_->project_root = project_root;
    impl_->pass_fields->setProjectRoot(project_root);
    impl_->reloadUiNodes();
    impl_->reloadAudioAssets();
    impl_->audio_row_node.clear();
    impl_->showDetails();
}

void GraphCanvasWidget::setRenderConfigDigest(
    std::shared_ptr<RenderConfigDigest> digest) {
    impl_->digest_hub = std::move(digest);
    if (impl_->digest_hub)
        impl_->digest_hub->subscribe(this, [impl = impl_.get()](const std::string &moved) {
            impl->documentMoved(moved);
        });
}

void GraphCanvasWidget::setTimelineScrubBlocked(const QString &reason) {
    impl_->model.setScrubBlockedReason(reason.toStdString());
    impl_->timeline->setScrubBlocked(reason);
}

GraphCanvasModel &GraphCanvasWidget::model() { return impl_->model; }
GraphCanvasScene &GraphCanvasWidget::scene() { return *impl_->scene; }

} // namespace PelicanStudio
