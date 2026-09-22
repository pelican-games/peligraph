#include "graphcanvaswidget.hpp"

#include "inlineediteventfilter.hpp"
#include "panelhelp.hpp"

#include "nodelayout.hpp"
#include "studiotheme.hpp"

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
#include <QFontMetricsF>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsRectItem>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPolygonF>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPen>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
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

// An edge is ink 1.6 units wide, and a person aims at it with a mouse. What can
// be picked is deliberately wider than what is drawn -- the same reason a link
// in a page has padding -- so that selecting a line, and double-clicking one to
// give it a control point, is a gesture rather than a test of aim.
class GraphEdgeItem final : public QGraphicsPathItem {
  public:
    static constexpr qreal PickWidth = 12.0;

    [[nodiscard]] QPainterPath shape() const override {
        QPainterPathStroker stroker;
        stroker.setWidth(PickWidth);
        return stroker.createStroke(path());
    }
};

class BreadcrumbItem final : public QGraphicsRectItem {
  public:
    using Clicked = std::function<void()>;

    BreadcrumbItem(const QRectF &rect, Clicked clicked)
        : QGraphicsRectItem{rect}, clicked_{std::move(clicked)} {}

  protected:
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && clicked_) {
            event->accept();
            clicked_();
            return;
        }
        QGraphicsRectItem::mousePressEvent(event);
    }

  private:
    Clicked clicked_;
};

// The domain is what the operation catalog says the node executes in. The
// two structural operations are coloured apart from it on purpose: a boundary
// port and an instance are not "some values node".
QColor nodeColor(const GraphCanvasNode &node) {
    const StudioTheme &theme = currentStudioTheme();
    if (node.op == "graph.instance")
        return theme.domainTint(QStringLiteral("instance"));
    if (node.op == "graph.input")
        return theme.domainTint(QStringLiteral("input"));
    if (node.domain.empty())
        return theme.domainTint(QStringLiteral("structural"));
    return theme.domainTint(QString::fromStdString(node.domain));
}

QString portSummary(const GraphCanvasPort &port) {
    QString text = qtext(port.port);
    if (port.connected)
        text += QStringLiteral(" <- %1.%2")
                    .arg(qtext(port.source_node), qtext(port.source_port));
    else if (port.has_parameter)
        text += QStringLiteral(" = %1").arg(qtext(port.parameter.dump()));
    if (port.has_value)
        text += QStringLiteral(" [%1 %2]")
                    .arg(port.value)
                    .arg(qtext(port.value_source));
    else if (!port.value_source.empty())
        text += QStringLiteral(" [%1]").arg(qtext(port.value_source));
    return text;
}

QString numberText(double value) {
    return QString::number(value, 'g', 6);
}

// What a Blender-style row shows on its right: the value the port is standing
// on while nothing is connected to it. A connected port has no pill, because
// the edge is the answer.
struct RowValue {
    QString text;
    QString source;
    [[nodiscard]] bool shown() const { return !text.isEmpty(); }
};

RowValue rowValue(const GraphCanvasPort &port) {
    if (port.connected) return {};
    // A published generation outranks the authored number: it is what the
    // port was actually fed, overrides included.
    if (port.has_value)
        return RowValue{numberText(port.value), qtext(port.value_source)};
    if (port.has_parameter)
        return RowValue{qtext(port.parameter.dump()),
                        QStringLiteral("stored")};
    return {};
}

QString pillText(const RowValue &value) {
    if (value.source.isEmpty()) return value.text;
    return QStringLiteral("%1 · %2").arg(value.text, value.source);
}

QPainterPath headerBandPath(qreal width, qreal height, qreal radius) {
    // The band has to share the node's rounded top corners and square off
    // where the socket sections begin.
    QPainterPath path;
    path.moveTo(0.0, height);
    path.lineTo(0.0, radius);
    path.quadTo(0.0, 0.0, radius, 0.0);
    path.lineTo(width - radius, 0.0);
    path.quadTo(width, 0.0, width, radius);
    path.lineTo(width, height);
    path.closeSubpath();
    return path;
}

// Everything that belongs to a node's body: a press anywhere on it selects
// the node, the way a press on the node rectangle used to.
bool isNodeSurface(const QString &kind) {
    return kind == QLatin1String{GraphCanvasNodeItem} ||
           kind == QLatin1String{GraphCanvasNodeLabelItem} ||
           kind == QLatin1String{GraphCanvasNodeHeaderItem} ||
           kind == QLatin1String{GraphCanvasCollapseItem} ||
           kind == QLatin1String{GraphCanvasSocketItem} ||
           kind == QLatin1String{GraphCanvasValuePillItem} ||
           kind == QLatin1String{GraphCanvasValueBandItem};
}

using InlineEditFilter = InlineEditEventFilter;

// What kind of editor one stored value asks for. The current value is the only
// evidence there is: no schema says what a node's parameter is allowed to be.
enum class ValueEditorKind { number, boolean, text, json };

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
            item->data(GraphCanvasPortRole).toString() != port ||
            item->data(GraphCanvasSocketDirectionRole).toString() != direction)
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
    if (from == nullptr || to == nullptr) return;

    QGraphicsItem *from_socket =
        findSocketOrAnchor(scene, scope, from_name,
                           edge.data(GraphCanvasFromPortRole).toString(),
                           QStringLiteral("output"));
    QGraphicsItem *to_socket =
        findSocketOrAnchor(scene, scope, to_name,
                           edge.data(GraphCanvasToPortRole).toString(),
                           QStringLiteral("input"));

    const QPointF start = from_socket != nullptr
                              ? from_socket->sceneBoundingRect().center()
                              : QPointF{from->sceneBoundingRect().right(),
                                        from->sceneBoundingRect().center().y()};
    const QPointF tip = to_socket != nullptr
                            ? to_socket->sceneBoundingRect().center()
                            : QPointF{to->sceneBoundingRect().left(),
                                      to->sceneBoundingRect().center().y()};
    const QVector<QPointF> bends = nodeGraphBends(edge);
    const CurveGeometry geometry =
        !bends.isEmpty() ? curveGeometryThrough(start, bends, tip)
        : from_socket != nullptr && to_socket != nullptr
            ? curveGeometryBetween(start, tip)
            : curveGeometry(from->sceneBoundingRect(), to->sceneBoundingRect());
    edge.setPath(geometry.path);
    if (auto *arrow = dynamic_cast<QGraphicsPolygonItem *>(findEndpointArtifact(
            scene, graphCanvasKinds().arrow, scope, from_name, to_name)))
        arrow->setPolygon(geometry.arrow);
}

void rerouteGraphCanvasEdges(QGraphicsScene &scene, const std::string &scope,
                             const std::string &node) {
    const QString expected_scope = qtext(scope);
    const QString expected_node = qtext(node);
    for (QGraphicsItem *item : scene.items()) {
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
    session_collapsed_.clear();
    pruned_revision_.reset();
    selected_node_.reset();
    selected_edge_.reset();
    selected_edges_.clear();
    clearControlSelection();
    layout_ = GraphCanvasLayout{};
    layout_scope_.clear();
    layout_sent_ = nlohmann::json{};
    layout_dirty_ = false;
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
    std::erase_if(session_node_positions_, [&](const auto &entry) {
        return !present.contains(entry.first);
    });
    std::erase_if(session_collapsed_, [&](const auto &entry) {
        return !present.contains(entry.first);
    });
}

bool GraphCanvasScene::collapsed(const std::string &id) const {
    if (model_ == nullptr) return false;
    const auto found = session_collapsed_.find({model_->currentScope(), id});
    return found != session_collapsed_.end() && found->second;
}

void GraphCanvasScene::setCollapsed(const std::string &id, bool folded) {
    if (model_ == nullptr) return;
    session_collapsed_[{model_->currentScope(), id}] = folded;
    repopulate();
}

void GraphCanvasScene::toggleCollapsed(const std::string &id) {
    setCollapsed(id, !collapsed(id));
}

void GraphCanvasScene::autoLayout() {
    if (model_ == nullptr) return;
    const std::string scope = model_->currentScope();
    std::erase_if(session_node_positions_, [&](const auto &entry) {
        return entry.first.first == scope;
    });
    // Start from nothing: the placement is the whole answer, so a leftover
    // control point from the last one would be a bend nobody asked for.
    layout_ = GraphCanvasLayout{};
    layout_scope_ = scope;
    layout_dirty_ = false;
    clearControlSelection();
    // One draw to get the placement, which is where `placement_*` comes from.
    repopulate();

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
    layout_ = std::move(placed);
    noteLayoutChange();
    repopulate();
    // "Auto layout" is one deliberate act, not the tail of a drag: it is sent
    // at once rather than after the wait a drag needs.
    flushLayout();
}

void GraphCanvasScene::setMenuActions(QAction *auto_layout, QAction *fit_view,
                                      QAction *tie, QAction *untie) {
    auto_layout_action_ = auto_layout;
    fit_view_action_ = fit_view;
    tie_action_ = tie;
    untie_action_ = untie;
}

// --- N-6: the placement.

void GraphCanvasScene::loadLayout(const std::string &scope) {
    if (model_ == nullptr) return;
    // A gesture left in the air belongs to the scope it happened in. Send it
    // before the canvas moves, and do not carry it across: a placement written
    // against the wrong scope names nodes that scope does not hold.
    if (layout_dirty_ && layout_scope_ != scope) {
        flushLayout();
        layout_dirty_ = false;
        layout_sent_ = nlohmann::json{};
    }
    const auto document = model_->scopeLayout(scope);
    if (layout_scope_ == scope && layout_dirty_) {
        // The document is the one that is behind, not the canvas.
        if (document != layout_sent_) return;
        layout_dirty_ = false;
    }
    layout_scope_ = scope;
    layout_ = graphCanvasLayoutFromJson(document);
    // Everything in the document's `layout` this canvas has no opinion about.
    // It travels back out on the next write; dropping it would make a node drag
    // here erase the Frame Plan's placement over there.
    layout_foreign_ = nlohmann::json::object();
    if (document.is_object())
        for (const auto &[key, value] : document.items())
            if (key != "nodes" && key != "reroutes" && key != "ties")
                layout_foreign_[key] = value;
}

nlohmann::json GraphCanvasScene::layoutMember(const std::string &key) const {
    const auto found = layout_foreign_.find(key);
    return found == layout_foreign_.end() ? nlohmann::json::object() : *found;
}

void GraphCanvasScene::setLayoutMember(const std::string &key,
                                       nlohmann::json value) {
    const bool drop = value.is_null() ||
                      (value.is_object() && value.empty());
    if (drop) {
        if (layout_foreign_.find(key) == layout_foreign_.end()) return;
        layout_foreign_.erase(key);
    } else {
        const auto found = layout_foreign_.find(key);
        if (found != layout_foreign_.end() && *found == value) return;
        layout_foreign_[key] = std::move(value);
    }
    noteLayoutChange();
}

void GraphCanvasScene::pruneLayout() {
    if (model_ == nullptr) return;
    const auto before = graphCanvasLayoutToJson(layout_);
    std::set<std::string, std::less<>> present;
    for (const auto &node : model_->nodes()) present.insert(node.id);
    std::set<GraphCanvasEdgeKey> drawn;
    for (const auto &edge : model_->edges())
        drawn.insert(GraphCanvasEdgeKey{edge.to_node, edge.to_port});
    std::erase_if(layout_.nodes, [&](const auto &entry) {
        return !present.contains(entry.first);
    });
    std::erase_if(layout_.reroutes, [&](const GraphCanvasReroute &entry) {
        return entry.points.empty() || !drawn.contains(entry.edge);
    });
    for (GraphCanvasTie &tie : layout_.ties)
        std::erase_if(tie.edges, [&](const GraphCanvasEdgeKey &key) {
            return !drawn.contains(key);
        });
    // A bundle that lost all but one line is not a bundle any more; the line
    // that is left keeps its place as a control point of its own.
    for (GraphCanvasTie &tie : layout_.ties) {
        if (tie.edges.size() != 1) continue;
        GraphCanvasReroute &entry = rerouteFor(tie.edges.front());
        entry.points.push_back(tie.position);
        std::ranges::stable_sort(entry.points, {}, &QPointF::x);
        tie.edges.clear();
    }
    std::erase_if(layout_.ties, [](const GraphCanvasTie &tie) {
        return tie.edges.empty();
    });
    // A placement that named something the document no longer holds would be
    // refused the next time it is written, so the tidying is itself a change.
    if (graphCanvasLayoutToJson(layout_) != before) noteLayoutChange();
}

void GraphCanvasScene::noteLayoutChange() {
    layout_dirty_ = true;
    // The canvas is now ahead of anything that has gone out. Forgetting what
    // was last sent is what keeps the next rebuild from deciding the document
    // has caught up and putting the older placement back over this change.
    layout_sent_ = nlohmann::json{};
    if (layout_timer_ != nullptr)
        layout_timer_->start(GraphCanvasLayoutDebounceMs);
}

void GraphCanvasScene::flushLayout() {
    if (!layout_dirty_ || model_ == nullptr) return;
    auto payload = graphCanvasLayoutToJson(layout_);
    // The other panel's members ride along: one `set_layout` replaces the whole
    // object, so leaving them out here is the same as deleting them.
    for (const auto &[key, value] : layout_foreign_.items())
        payload[key] = value;
    // Busy with the last edit, or no digest to hold the document still yet.
    // The change is kept rather than dropped: losing it would lose the drag.
    if (!model_->canEdit() || !model_->setLayout(layout_scope_, payload)) {
        if (layout_timer_ != nullptr)
            layout_timer_->start(GraphCanvasLayoutDebounceMs);
        return;
    }
    layout_sent_ = std::move(payload);
    // Sending from inside a rebuild would post a request while the scene is
    // half-built; the tab's own tick drains the outbox a moment later anyway.
    if (!rebuilding_) afterEdit(true);
}

QVector<QPointF> GraphCanvasScene::bendsFor(const GraphCanvasEdge &edge) const {
    const GraphCanvasEdgeKey key{edge.to_node, edge.to_port};
    std::vector<QPointF> points;
    for (const GraphCanvasReroute &reroute : layout_.reroutes) {
        if (!(reroute.edge == key)) continue;
        points.insert(points.end(), reroute.points.begin(), reroute.points.end());
    }
    for (const GraphCanvasTie &tie : layout_.ties) {
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
        setNodeGraphBends(*line, bendsFor(edge));
        routeGraphCanvasEdge(*this, *line);
    }
}

GraphCanvasReroute &GraphCanvasScene::rerouteFor(const GraphCanvasEdgeKey &edge) {
    const auto found =
        std::ranges::find(layout_.reroutes, edge, &GraphCanvasReroute::edge);
    if (found != layout_.reroutes.end()) return *found;
    layout_.reroutes.push_back(GraphCanvasReroute{edge, {}});
    return layout_.reroutes.back();
}

void GraphCanvasScene::detachFromTies(const GraphCanvasEdgeKey &edge) {
    for (GraphCanvasTie &tie : layout_.ties)
        std::erase(tie.edges, edge);
    std::erase_if(layout_.ties, [](const GraphCanvasTie &tie) {
        return tie.edges.size() < 2;
    });
}

void GraphCanvasScene::moveReroute(const GraphCanvasEdgeKey &edge,
                                   std::size_t index, const QPointF &at) {
    const auto found =
        std::ranges::find(layout_.reroutes, edge, &GraphCanvasReroute::edge);
    if (found == layout_.reroutes.end() || index >= found->points.size()) return;
    found->points[index] = at;
    noteLayoutChange();
    applyEdgeBends();
    scheduleSceneRectUpdate();
}

void GraphCanvasScene::moveTie(std::size_t index, const QPointF &at) {
    if (index >= layout_.ties.size()) return;
    layout_.ties[index].position =
        at + QPointF{GraphCanvasTieWidth / 2.0, GraphCanvasTiePadding};
    noteLayoutChange();
    applyEdgeBends();
    scheduleSceneRectUpdate();
}

void GraphCanvasScene::clearControlSelection() {
    selected_reroutes_.clear();
    selected_ties_.clear();
}

void GraphCanvasScene::selectReroute(const GraphCanvasEdgeKey &edge,
                                     std::size_t index, bool additive) {
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
    const GraphCanvasEdgeKey key{edge.to_node, edge.to_port};
    const auto drawn = model_->edges();
    if (std::ranges::none_of(drawn, [&](const GraphCanvasEdge &candidate) {
            return candidate.to_node == key.node && candidate.to_port == key.port;
        }))
        return false;
    GraphCanvasReroute &entry = rerouteFor(key);
    entry.points.push_back(at);
    // The order along the line is the order the drawing meets them, and on a
    // canvas that runs left to right that is the order of x.
    std::ranges::stable_sort(entry.points, {}, &QPointF::x);
    noteLayoutChange();
    repopulate();
    return true;
}

bool GraphCanvasScene::tieSelection() {
    if (model_ == nullptr) return false;
    // Where each picked line runs now. A reroute says so exactly; an edge
    // picked without one is taken at the middle of the curve it is drawn as.
    const auto midpoint = [this](const GraphCanvasEdge &edge) {
        for (QGraphicsItem *item : items()) {
            if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasEdgeItem} ||
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
    std::vector<Pick> picks;
    const auto has = [&](const GraphCanvasEdgeKey &key) {
        return std::ranges::any_of(
            picks, [&](const Pick &pick) { return pick.key == key; });
    };
    for (const auto &[key, index] : selected_reroutes_) {
        if (has(key)) continue;
        const auto found =
            std::ranges::find(layout_.reroutes, key, &GraphCanvasReroute::edge);
        if (found == layout_.reroutes.end() || index >= found->points.size()) continue;
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
            std::ranges::find(layout_.reroutes, key, &GraphCanvasReroute::edge);
        if (found == layout_.reroutes.end() || index >= found->points.size()) continue;
        found->points.erase(found->points.begin() +
                            static_cast<std::ptrdiff_t>(index));
    }
    std::erase_if(layout_.reroutes, [](const GraphCanvasReroute &entry) {
        return entry.points.empty();
    });

    GraphCanvasTie tie;
    tie.position = QPointF{x, picks.front().at.y()};
    for (const Pick &pick : picks) {
        // One line belongs to at most one bundle.
        detachFromTies(pick.key);
        tie.edges.push_back(pick.key);
    }
    layout_.ties.push_back(std::move(tie));
    clearControlSelection();
    selected_edges_.clear();
    selected_edge_.reset();
    noteLayoutChange();
    repopulate();
    return true;
}

bool GraphCanvasScene::untieSelection() {
    if (selected_ties_.empty()) return false;
    auto indices = selected_ties_;
    std::ranges::sort(indices, std::greater<>{});
    bool changed = false;
    for (const std::size_t index : indices) {
        if (index >= layout_.ties.size()) continue;
        const GraphCanvasTie tie = layout_.ties[index];
        layout_.ties.erase(layout_.ties.begin() +
                           static_cast<std::ptrdiff_t>(index));
        for (std::size_t slot = 0; slot < tie.edges.size(); ++slot) {
            GraphCanvasReroute &entry = rerouteFor(tie.edges[slot]);
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
    noteLayoutChange();
    repopulate();
    return true;
}

bool GraphCanvasScene::deleteControlSelection() {
    bool changed = false;
    if (!selected_ties_.empty()) {
        auto indices = selected_ties_;
        std::ranges::sort(indices, std::greater<>{});
        for (const std::size_t index : indices) {
            if (index >= layout_.ties.size()) continue;
            layout_.ties.erase(layout_.ties.begin() +
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
                std::ranges::find(layout_.reroutes, key, &GraphCanvasReroute::edge);
            if (found == layout_.reroutes.end() || index >= found->points.size())
                continue;
            found->points.erase(found->points.begin() +
                                static_cast<std::ptrdiff_t>(index));
            changed = true;
        }
        std::erase_if(layout_.reroutes, [](const GraphCanvasReroute &entry) {
            return entry.points.empty();
        });
    }
    if (!changed) return false;
    clearControlSelection();
    noteLayoutChange();
    repopulate();
    return true;
}

void GraphCanvasScene::repopulate() {
    const StudioTheme &theme = currentStudioTheme();
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
    selected_node_.reset();
    selected_edge_.reset();
    setProperty("pelicanGraphScope", QVariant{});
    setProperty("pelicanGraphBreadcrumb", QVariant{});
    setProperty("pelicanGraphNodeCount", QVariant{});
    setProperty("pelicanGraphEdgeCount", QVariant{});
    setProperty("pelicanGraphNotice", QVariant{});
    if (model_ == nullptr) {
        rebuilding_ = false;
        return;
    }
    pruneSessionPositions();

    const std::string scope = model_->currentScope();
    // N-6: what the document says about where this scope was drawn. It is read
    // before the nodes so the placement can answer for them; the session table
    // under it only speaks for a node the document has never placed.
    loadLayout(scope);
    pruneLayout();
    const QString scope_text = qtext(scope);
    const auto nodes = model_->nodes();
    const auto edges = model_->edges();

    QFont label_font;
    label_font.setPointSizeF(label_font.pointSizeF() - 0.5);
    QFont detail_font = label_font;
    detail_font.setPointSizeF(detail_font.pointSizeF() - 0.5);
    QFont heading_font = label_font;
    heading_font.setBold(true);
    const QFontMetricsF heading_metrics{heading_font};
    const QFontMetricsF row_metrics{detail_font};

    const qreal top = model_->breadcrumb().size() > 1
                          ? NodeGraphBreadcrumbHeight + 18.0
                          : 0.0;
    // The rows of every node first. A node is as wide as its widest row and as
    // tall as its socket list, and the automatic placement cannot be asked
    // where a thing goes before it is told how big the thing is -- which is
    // why the drawing loop below is a second pass rather than the only one.
    struct DrawnRow {
        QString port;
        QString label;
        QString type;
        SocketStyle style;
        RowValue value;
        bool connected = false;
        bool declared = false;
    };
    struct DrawnNode {
        const GraphCanvasNode *node = nullptr;
        std::vector<DrawnRow> outputs;
        std::vector<DrawnRow> inputs;
        qreal width = 0.0;
        qreal height = 0.0;
        bool folded = false;
    };
    std::vector<DrawnNode> drawn;
    drawn.reserve(nodes.size());
    for (const auto &node : nodes) {
        DrawnNode entry;
        entry.node = &node;
        for (const auto &output : node.outputs)
            entry.outputs.push_back(DrawnRow{
                qtext(output.port), qtext(output.port), qtext(output.type),
                socketStyleFor(graphCanvasSocketFacts(*model_, output.type)),
                // No source word on an output: there is only one place an
                // output value can come from, and naming it every row would
                // be a column of the same word.
                output.has_value ? RowValue{numberText(output.value), {}}
                                 : RowValue{},
                false, false});
        for (const auto &port : node.inputs)
            entry.inputs.push_back(DrawnRow{
                qtext(port.port), qtext(port.port), qtext(port.type),
                socketStyleFor(graphCanvasSocketFacts(*model_, port.type)),
                rowValue(port), port.connected, port.declared});

        qreal width = GraphCanvasNodeMinimumWidth;
        width = std::max(width,
                         18.0 + heading_metrics.horizontalAdvance(
                                    qtext(node.heading())) +
                             12.0 +
                             row_metrics.horizontalAdvance(qtext(node.op)) +
                             14.0);
        for (const auto &row : entry.outputs) {
            qreal needed = row_metrics.horizontalAdvance(row.label) + 34.0;
            if (row.value.shown())
                needed += row_metrics.horizontalAdvance(pillText(row.value)) + 24.0;
            width = std::max(width, needed);
        }
        for (const auto &row : entry.inputs) {
            qreal needed = row_metrics.horizontalAdvance(row.label) + 34.0;
            if (row.value.shown())
                needed += row_metrics.horizontalAdvance(pillText(row.value)) + 24.0;
            width = std::max(width, needed);
        }
        entry.width = std::min(width, GraphCanvasNodeMaximumWidth);

        // A folded node is its heading band and nothing else: the rows are not
        // drawn small, they are not drawn.
        entry.folded = collapsed(node.id);
        const auto row_count =
            static_cast<qreal>(entry.outputs.size() + entry.inputs.size());
        entry.height =
            entry.folded
                ? GraphCanvasHeaderHeight
                : GraphCanvasHeaderHeight + GraphCanvasSectionPadding +
                      row_count * GraphCanvasSocketRowHeight +
                      GraphCanvasSectionPadding;
        drawn.push_back(std::move(entry));
    }

    // N-4: one shared, Qt-free placement for both node canvases. It is what
    // decides the columns as well, so the column a node publishes and the
    // column its position came from cannot drift apart.
    std::vector<LayoutNode> layout_nodes;
    layout_nodes.reserve(drawn.size());
    for (const DrawnNode &entry : drawn)
        layout_nodes.push_back(
            LayoutNode{entry.node->id, entry.width, entry.height});
    std::vector<LayoutEdge> layout_edges;
    layout_edges.reserve(edges.size());
    for (const auto &edge : edges)
        layout_edges.push_back(LayoutEdge{edge.from_node, edge.to_node});
    LayoutOptions layout_options;
    layout_options.horizontal_gap = NodeGraphHorizontalGap;
    layout_options.vertical_gap = NodeGraphVerticalGap;
    layout_options.origin_y = top;
    const NodeGraphLayout layout =
        layoutNodeGraph(layout_nodes, layout_edges, layout_options);
    // What it chose, in the canvas's own terms. "Auto layout" turns this into
    // control points; nothing else reads it, and it is recomputed every draw.
    placement_boxes_.clear();
    for (const auto &[id, box] : layout.boxes)
        placement_boxes_.emplace(id, QPointF{box.x, box.y});
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

    std::map<std::string, QGraphicsPathItem *, std::less<>> items;
    for (const DrawnNode &entry : drawn) {
        const GraphCanvasNode &node = *entry.node;
        const std::vector<DrawnRow> &outputs = entry.outputs;
        const std::vector<DrawnRow> &inputs = entry.inputs;
        const qreal width = entry.width;
        const qreal height = entry.height;
        const bool folded = entry.folded;
        const qreal rows_top = GraphCanvasHeaderHeight + GraphCanvasSectionPadding;
        const QString node_name = qtext(node.id);
        const QString heading = qtext(node.heading());
        const QString type_name = qtext(node.op);

        const auto column_entry = layout.columns.find(node.id);
        const int column =
            column_entry == layout.columns.end() ? 0 : column_entry->second;
        const auto box = layout.boxes.find(node.id);
        const QPointF laid_out =
            box == layout.boxes.end() ? QPointF{0.0, top}
                                      : QPointF{box->second.x, box->second.y};
        const auto position_key = std::pair{scope, node.id};
        QPointF position = laid_out;
        // N-6: the document is where a place lives now. The session table is
        // the fallback for a node the document has never placed -- one just
        // added from the palette, before its first drag went out.
        if (const auto placed = layout_.nodes.find(node.id);
            placed != layout_.nodes.end()) {
            position = placed->second;
        } else if (const auto retained = session_node_positions_.find(position_key);
                   retained != session_node_positions_.end()) {
            position = retained->second;
        }

        auto *item = new MovableNodeItem(
            roundedEntityPath(width, height, GraphCanvasNodeCornerRadius),
            [this, position_key, scope](const QPointF &moved) {
                if (rebuilding_) return;
                session_node_positions_[position_key] = moved;
                // And the document, once the drag has settled. The control
                // points do not move with it: they are absolute, which is the
                // whole point of decision 1.
                layout_.nodes[position_key.second] = moved;
                noteLayoutChange();
                rerouteGraphCanvasEdges(*this, scope, position_key.second);
                // Growing the scene rect from inside itemChange re-enters this
                // handler through Qt's view update. Defer it to the event loop.
                scheduleSceneRectUpdate();
            });
        addItem(item);
        item->setPos(position);
        item->setZValue(2.0);
        // The body is neutral; the heading band carries the domain colour, so
        // a long port list does not turn into a wall of one hue.
        item->setBrush(theme.canvas.node_body);
        item->setPen(QPen{theme.canvas.node_border, 1.2});
        item->setFlag(QGraphicsItem::ItemIsMovable, true);
        item->setFlag(QGraphicsItem::ItemIsSelectable, true);
        item->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
        item->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasNodeItem));
        item->setData(GraphCanvasScopeRole, scope_text);
        item->setData(NodeGraphNameRole, node_name);
        item->setData(GraphCanvasLabelRole, heading);
        item->setData(GraphCanvasOpRole, qtext(node.op));
        item->setData(GraphCanvasDomainRole, qtext(node.domain));
        item->setData(GraphCanvasEvaluationRole, qtext(node.evaluation));
        item->setData(GraphCanvasSinkRole, node.sink);
        item->setData(GraphCanvasColumnRole, column);
        item->setData(GraphCanvasDefinitionRole, qtext(node.definition));
        item->setData(GraphCanvasCollapsedRole, folded);
        QStringList input_ports;
        QStringList declared_ports;
        for (const auto &port : node.inputs) {
            input_ports.push_back(portSummary(port));
            if (port.declared) declared_ports.push_back(qtext(port.port));
        }
        QStringList output_ports;
        for (const auto &output : node.outputs)
            output_ports.push_back(qtext(output.port));
        item->setData(GraphCanvasInputPortsRole, input_ports);
        item->setData(GraphCanvasDeclaredPortsRole, declared_ports);
        item->setData(GraphCanvasOutputPortsRole, output_ports);
        items.emplace(node.id, item);

        const auto tag = [&](QGraphicsItem *child, const char *kind) {
            child->setData(NodeGraphItemKindRole, QString::fromLatin1(kind));
            child->setData(GraphCanvasScopeRole, scope_text);
            child->setData(NodeGraphNameRole, node_name);
        };

        auto *band = new QGraphicsPathItem(
            headerBandPath(width, GraphCanvasHeaderHeight,
                           GraphCanvasNodeCornerRadius),
            item);
        band->setBrush(nodeColor(node));
        band->setPen(Qt::NoPen);
        tag(band, GraphCanvasNodeHeaderItem);
        band->setData(GraphCanvasHeadingRole, true);

        // The triangle folds the socket sections away. It points down while
        // the node is open and right while it is folded, which is the one
        // place the state is visible without counting rows.
        QPainterPath arrow;
        if (folded) {
            arrow.moveTo(-2.0, -4.5);
            arrow.lineTo(3.5, 0.0);
            arrow.lineTo(-2.0, 4.5);
        } else {
            arrow.moveTo(-4.5, -2.0);
            arrow.lineTo(4.5, -2.0);
            arrow.lineTo(0.0, 3.5);
        }
        arrow.closeSubpath();
        auto *collapse = new QGraphicsPathItem(arrow, band);
        collapse->setPos(9.5, GraphCanvasHeaderHeight / 2.0);
        collapse->setBrush(theme.canvas.heading_text);
        collapse->setPen(QPen{Qt::NoPen});
        tag(collapse, GraphCanvasCollapseItem);
        collapse->setData(GraphCanvasHeadingRole, true);
        collapse->setData(GraphCanvasCollapsedRole, folded);
        collapse->setToolTip(folded ? QObject::tr("Show the sockets")
                                    : QObject::tr("Fold the sockets away"));

        auto *title = new QGraphicsSimpleTextItem(heading, band);
        title->setFont(heading_font);
        title->setBrush(theme.canvas.heading_text);
        title->setPos(18.0, (GraphCanvasHeaderHeight -
                             heading_metrics.height()) / 2.0);
        tag(title, GraphCanvasNodeLabelItem);
        title->setData(GraphCanvasLabelRole, heading);
        title->setData(GraphCanvasHeadingRole, true);
        title->setToolTip(QObject::tr("Double-click to rename"));

        // The operation type stays visible next to the chosen name: renaming a
        // node must never hide what it actually is.
        auto *kind = new QGraphicsSimpleTextItem(type_name, band);
        kind->setFont(detail_font);
        kind->setBrush(theme.canvas.dim_text);
        kind->setOpacity(0.78);
        kind->setPos(width - 10.0 - row_metrics.horizontalAdvance(type_name),
                     (GraphCanvasHeaderHeight - row_metrics.height()) / 2.0);
        tag(kind, GraphCanvasNodeLabelItem);
        kind->setData(GraphCanvasOpRole, type_name);
        kind->setData(GraphCanvasHeadingRole, true);

        const auto socket = [&](const DrawnRow &drawn, const QPointF &centre,
                                const char *direction) {
            auto *dot = new QGraphicsPathItem(
                socketPath(drawn.style.shape, drawn.style.size), item);
            dot->setPos(centre);
            dot->setZValue(1.0);
            dot->setBrush(drawn.style.fill);
            dot->setPen(QPen{drawn.style.outline, 1.1});
            dot->setToolTip(drawn.type.isEmpty()
                                ? QObject::tr("%1 (type not declared)").arg(drawn.label)
                                : QStringLiteral("%1 : %2").arg(drawn.label, drawn.type));
            tag(dot, GraphCanvasSocketItem);
            dot->setData(GraphCanvasPortRole, drawn.port);
            dot->setData(GraphCanvasPortTypeRole, drawn.type);
            dot->setData(GraphCanvasSocketDirectionRole,
                         QString::fromLatin1(direction));
            dot->setData(GraphCanvasSocketShapeRole,
                         static_cast<int>(drawn.style.shape));
        };

        if (folded) {
            // One point per side, at the middle of the band: every edge that
            // entered a row now arrives here, and leaves from the other one.
            const qreal centre_y = GraphCanvasHeaderHeight / 2.0;
            const auto anchor = [&](const QPointF &centre, const char *direction) {
                auto *dot = new QGraphicsPathItem(
                    socketPath(SocketShape::circle, GraphCanvasSocketSize), item);
                dot->setPos(centre);
                dot->setZValue(1.0);
                dot->setBrush(theme.canvas.dim_text);
                dot->setPen(QPen{theme.canvas.node_border, 1.1});
                tag(dot, GraphCanvasSocketItem);
                dot->setData(GraphCanvasPortRole, QString{});
                dot->setData(GraphCanvasSocketDirectionRole,
                             QString::fromLatin1(direction));
                dot->setData(GraphCanvasCollapsedRole, true);
                dot->setToolTip(QObject::tr("%1 (folded)").arg(heading));
            };
            if (!inputs.empty()) anchor(QPointF{0.0, centre_y}, "input");
            if (!outputs.empty()) anchor(QPointF{width, centre_y}, "output");
            continue;
        }

        qreal cursor_y = rows_top;
        for (const auto &drawn : outputs) {
            const qreal centre_y = cursor_y + GraphCanvasSocketRowHeight / 2.0;
            auto *name = new QGraphicsSimpleTextItem(drawn.label, item);
            name->setFont(detail_font);
            name->setBrush(theme.canvas.dim_text);
            name->setPos(width - 14.0 - row_metrics.horizontalAdvance(drawn.label),
                         centre_y - row_metrics.height() / 2.0);
            tag(name, GraphCanvasNodeLabelItem);
            name->setData(GraphCanvasPortRole, drawn.port);
            // Half outside the right edge, which is where the edge meets it.
            socket(drawn, QPointF{width, centre_y}, "output");

            // AG-3b: the published generation's value, on the left of the row
            // because the port's own name is on the right. It is drawn for
            // every scope: get_graph_node_values is the one report that
            // answers for a node inside a definition body.
            if (drawn.value.shown()) {
                const QString text = drawn.value.text;
                const qreal band_height = GraphCanvasSocketRowHeight - 6.0;
                const qreal band_width =
                    row_metrics.horizontalAdvance(text) + 14.0;
                auto *band = new QGraphicsPathItem(
                    roundedEntityPath(band_width, band_height,
                                      band_height / 2.0),
                    item);
                band->setPos(10.0, centre_y - band_height / 2.0);
                band->setBrush(theme.canvas.pill_bg);
                band->setPen(QPen{theme.canvas.node_border, 0.8});
                band->setToolTip(
                    QObject::tr("%1 = %2 in the published generation (rendering)")
                        .arg(drawn.label, text));
                tag(band, GraphCanvasValueBandItem);
                band->setData(GraphCanvasPortRole, drawn.port);
                band->setData(GraphCanvasValueTextRole, text);
                auto *value = new QGraphicsSimpleTextItem(text, band);
                value->setFont(detail_font);
                value->setBrush(theme.canvas.heading_text);
                value->setPos(7.0, (band_height - row_metrics.height()) / 2.0);
                tag(value, GraphCanvasValueBandItem);
                value->setData(GraphCanvasPortRole, drawn.port);
                value->setData(GraphCanvasValueTextRole, text);
            }
            cursor_y += GraphCanvasSocketRowHeight;
        }
        for (const auto &drawn : inputs) {
            const qreal centre_y = cursor_y + GraphCanvasSocketRowHeight / 2.0;
            auto *name = new QGraphicsSimpleTextItem(drawn.label, item);
            name->setFont(detail_font);
            // A port the catalog never declared is dimmer than one it did,
            // and "dimmer" has to survive both themes: it is the theme's own
            // dim colour, faded, rather than a second fixed grey.
            QColor undeclared = theme.canvas.dim_text;
            undeclared.setAlphaF(0.72F);
            name->setBrush(drawn.declared ? theme.canvas.dim_text : undeclared);
            name->setPos(14.0, centre_y - row_metrics.height() / 2.0);
            tag(name, GraphCanvasNodeLabelItem);
            name->setData(GraphCanvasPortRole, drawn.port);
            socket(drawn, QPointF{0.0, centre_y}, "input");

            if (drawn.value.shown()) {
                const QString text = pillText(drawn.value);
                const qreal pill_height = GraphCanvasSocketRowHeight - 6.0;
                const qreal pill_width =
                    row_metrics.horizontalAdvance(text) + 14.0;
                auto *pill = new QGraphicsPathItem(
                    roundedEntityPath(pill_width, pill_height,
                                      pill_height / 2.0),
                    item);
                pill->setPos(width - 10.0 - pill_width,
                             centre_y - pill_height / 2.0);
                pill->setBrush(theme.canvas.pill_bg);
                pill->setPen(QPen{theme.canvas.node_border, 0.8});
                tag(pill, GraphCanvasValuePillItem);
                pill->setData(GraphCanvasPortRole, drawn.port);
                pill->setData(GraphCanvasValueTextRole, drawn.value.text);
                pill->setData(GraphCanvasValueSourceRole, drawn.value.source);
                auto *value = new QGraphicsSimpleTextItem(text, pill);
                value->setFont(detail_font);
                value->setBrush(theme.canvas.heading_text);
                value->setPos(7.0, (pill_height - row_metrics.height()) / 2.0);
                tag(value, GraphCanvasValuePillItem);
                value->setData(GraphCanvasPortRole, drawn.port);
            }
            cursor_y += GraphCanvasSocketRowHeight;
        }
    }

    const QFontMetricsF metrics{detail_font};
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const auto &edge = edges[index];
        if (!items.contains(edge.from_node) || !items.contains(edge.to_node)) continue;
        auto *line = new GraphEdgeItem;
        addItem(line);
        // An edge that flies over a column follows the lane the layout kept
        // for it, so it passes between the nodes it flies over rather than
        // through them. An end that has been dragged loses the lane again.
        const QVector<QPointF> placed = bendsFor(edge);
        if (!placed.isEmpty()) {
            setNodeGraphBends(*line, placed);
        } else if (layout_.empty() && index < layout.bends.size() &&
                   !layout.bends[index].empty() &&
                   !session_node_positions_.contains({scope, edge.from_node}) &&
                   !session_node_positions_.contains({scope, edge.to_node})) {
            // No placement has been kept for this scope yet, so the lane the
            // automatic layout chose is still the best thing to draw through.
            // It is not a control point: "Auto layout" is what makes one.
            QVector<QPointF> bends;
            for (const LayoutPoint &point : layout.bends[index])
                bends.push_back(QPointF{point.x, point.y});
            setNodeGraphBends(*line, bends);
        }
        line->setZValue(0.0);
        line->setPen(QPen{theme.canvas.edge, 1.6});
        line->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasEdgeItem));
        line->setData(GraphCanvasScopeRole, scope_text);
        line->setData(NodeGraphFromNameRole, qtext(edge.from_node));
        line->setData(NodeGraphToNameRole, qtext(edge.to_node));
        line->setData(GraphCanvasFromPortRole, qtext(edge.from_port));
        line->setData(GraphCanvasToPortRole, qtext(edge.to_port));

        auto *arrow = new QGraphicsPolygonItem;
        addItem(arrow);
        arrow->setZValue(1.0);
        arrow->setBrush(theme.canvas.edge);
        arrow->setPen(Qt::NoPen);
        arrow->setData(NodeGraphItemKindRole,
                       QString::fromLatin1(GraphCanvasEdgeArrowItem));
        arrow->setData(GraphCanvasScopeRole, scope_text);
        arrow->setData(NodeGraphFromNameRole, qtext(edge.from_node));
        arrow->setData(NodeGraphToNameRole, qtext(edge.to_node));
        // N-6: an edge is named by the input end it lands on, and a press on
        // the head means the same edge as a press on the line. Without the two
        // port names the head would name an edge with no input end at all.
        arrow->setData(GraphCanvasFromPortRole, qtext(edge.from_port));
        arrow->setData(GraphCanvasToPortRole, qtext(edge.to_port));

        line->setToolTip(
            QObject::tr("%1.%2 -> %3.%4  (select and press Delete to cut)")
                .arg(qtext(edge.from_node), qtext(edge.from_port),
                     qtext(edge.to_node), qtext(edge.to_port)));

        // No edge label: the socket rows already name both ends, and a box in
        // the middle of every line repeated them.
        routeGraphCanvasEdge(*this, *line);
    }

    // N-6: the control points, over everything they may come to lie on. A
    // bundle's place is the middle of the lines it carries, and the middle of
    // a line is often over a node: drawn under one, the handle cannot be taken
    // hold of, and the press that reaches for it drags the node instead. (The
    // scenario found exactly that on the real window.) They are small -- a
    // 9-pixel dot and a 10-pixel band -- so being on top costs the drawing
    // almost nothing and is what makes them reachable.
    const auto drawnEdge = [&](const GraphCanvasEdgeKey &key) {
        return std::ranges::any_of(edges, [&](const GraphCanvasEdge &candidate) {
            return candidate.to_node == key.node && candidate.to_port == key.port &&
                   items.contains(candidate.from_node) &&
                   items.contains(candidate.to_node);
        });
    };
    for (const GraphCanvasReroute &reroute : layout_.reroutes) {
        if (!drawnEdge(reroute.edge)) continue;
        for (std::size_t index = 0; index < reroute.points.size(); ++index) {
            QPainterPath circle;
            circle.addEllipse(QPointF{0.0, 0.0}, GraphCanvasRerouteSize / 2.0,
                              GraphCanvasRerouteSize / 2.0);
            const GraphCanvasEdgeKey key = reroute.edge;
            auto *dot = new MovableNodeItem(
                circle, [this, key, index](const QPointF &moved) {
                    if (rebuilding_) return;
                    moveReroute(key, index, moved);
                });
            addItem(dot);
            dot->setPos(reroute.points[index]);
            dot->setZValue(2.6);
            dot->setBrush(theme.canvas.pill_bg);
            dot->setPen(QPen{theme.canvas.edge_selected, 1.4});
            dot->setFlag(QGraphicsItem::ItemIsMovable, true);
            dot->setFlag(QGraphicsItem::ItemIsSelectable, true);
            dot->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
            dot->setData(NodeGraphItemKindRole,
                         QString::fromLatin1(GraphCanvasRerouteItem));
            dot->setData(GraphCanvasScopeRole, scope_text);
            dot->setData(NodeGraphNameRole, qtext(key.node));
            // The automation name is built from the node and the port, so the
            // index rides on the port half: `graph_reroute#left/gain/0`.
            dot->setData(GraphCanvasPortRole,
                         qtext(key.port) + QStringLiteral("/") +
                             QString::number(static_cast<qulonglong>(index)));
            dot->setData(GraphCanvasToPortRole, qtext(key.port));
            dot->setData(GraphCanvasControlIndexRole,
                         static_cast<qulonglong>(index));
            dot->setToolTip(
                QObject::tr("Control point on %1.%2 (drag to bend, Delete to remove)")
                    .arg(qtext(key.node), qtext(key.port)));
        }
    }
    for (std::size_t index = 0; index < layout_.ties.size(); ++index) {
        const GraphCanvasTie &tie = layout_.ties[index];
        // `slots` is a Qt keyword, so the count of them cannot be called that.
        const auto carried = static_cast<qreal>(tie.edges.size() - 1);
        const qreal height =
            carried * GraphCanvasTieSpacing + 2.0 * GraphCanvasTiePadding;
        auto *bar = new MovableNodeItem(
            roundedEntityPath(GraphCanvasTieWidth, height,
                              GraphCanvasTieWidth / 2.0),
            [this, index](const QPointF &moved) {
                if (rebuilding_) return;
                moveTie(index, moved);
            });
        addItem(bar);
        bar->setPos(tie.position - QPointF{GraphCanvasTieWidth / 2.0,
                                           GraphCanvasTiePadding});
        bar->setZValue(2.5);
        bar->setBrush(theme.canvas.edge_selected);
        bar->setPen(QPen{theme.canvas.node_border, 1.0});
        bar->setFlag(QGraphicsItem::ItemIsMovable, true);
        bar->setFlag(QGraphicsItem::ItemIsSelectable, true);
        bar->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
        bar->setData(NodeGraphItemKindRole,
                     QString::fromLatin1(GraphCanvasTieItem));
        bar->setData(GraphCanvasScopeRole, scope_text);
        bar->setData(NodeGraphNameRole,
                     QString::number(static_cast<qulonglong>(index)));
        bar->setData(GraphCanvasControlIndexRole,
                     static_cast<qulonglong>(index));
        bar->setData(GraphCanvasTieSlotRole,
                     static_cast<qulonglong>(tie.edges.size()));
        bar->setToolTip(QObject::tr(
            "%1 lines carried together (drag to move, double-click to reverse)")
                            .arg(tie.edges.size()));
    }

    // The selection survives the rebuild when what it named is still here.
    if (previous_node &&
        std::ranges::any_of(nodes, [&](const GraphCanvasNode &drawn) {
            return drawn.id == *previous_node;
        }))
        selected_node_ = previous_node;
    if (previous_edge &&
        std::ranges::any_of(edges, [&](const GraphCanvasEdge &drawn) {
            return drawn.from_node == previous_edge->from_node &&
                   drawn.from_port == previous_edge->from_port &&
                   drawn.to_node == previous_edge->to_node &&
                   drawn.to_port == previous_edge->to_port;
        }))
        selected_edge_ = previous_edge;
    // N-6: the multi-pick a bundle is made from survives a rebuild the same
    // way -- what is still drawn stays picked, and what is gone lets go.
    std::erase_if(selected_edges_, [&](const GraphCanvasEdge &picked) {
        return std::ranges::none_of(edges, [&](const GraphCanvasEdge &drawn) {
            return drawn.from_node == picked.from_node &&
                   drawn.from_port == picked.from_port &&
                   drawn.to_node == picked.to_node &&
                   drawn.to_port == picked.to_port;
        });
    });
    std::erase_if(selected_reroutes_, [&](const auto &picked) {
        const auto found = std::ranges::find(layout_.reroutes, picked.first,
                                             &GraphCanvasReroute::edge);
        return found == layout_.reroutes.end() ||
               picked.second >= found->points.size();
    });
    std::erase_if(selected_ties_, [&](const std::size_t index) {
        return index >= layout_.ties.size();
    });
    setProperty("pelicanGraphSelectedNode",
                selected_node_ ? qtext(*selected_node_) : QString{});
    applyEdgeEmphasis();

    if (model_->breadcrumb().size() > 1) {
        QStringList trail;
        for (const auto &step : model_->breadcrumb())
            trail.push_back(step.empty() ? QStringLiteral("root") : qtext(step));
        const QString text = trail.join(QStringLiteral(" / "));
        const qreal width =
            std::max<qreal>(220.0, metrics.horizontalAdvance(text) + 96.0);
        auto *crumb = new BreadcrumbItem{
            QRectF{0.0, 0.0, width, NodeGraphBreadcrumbHeight}, [this] {
                // Rebuilding here would delete the item that is handling this
                // press while Qt is still inside its event dispatch. Leave the
                // scope from the event loop instead.
                QMetaObject::invokeMethod(
                    this, [this] { leaveCurrentScope(); },
                    Qt::QueuedConnection);
            }};
        addItem(crumb);
        crumb->setPos(0.0, -NodeGraphBreadcrumbHeight - 18.0 + top);
        crumb->setZValue(3.0);
        crumb->setBrush(theme.canvas.pill_bg);
        crumb->setPen(QPen{theme.canvas.edge, 1.0});
        crumb->setAcceptedMouseButtons(Qt::LeftButton);
        crumb->setData(NodeGraphItemKindRole,
                       QString::fromLatin1(GraphCanvasBreadcrumbItem));
        crumb->setData(GraphCanvasScopeRole, scope_text);
        auto *crumb_label = new QGraphicsSimpleTextItem(
            QStringLiteral("%1   (click to leave)").arg(text), crumb);
        crumb_label->setFont(label_font);
        crumb_label->setBrush(theme.canvas.heading_text);
        crumb_label->setPos(12.0, 8.0);
        crumb_label->setData(NodeGraphItemKindRole,
                             QString::fromLatin1(GraphCanvasBreadcrumbItem));
    }

    setProperty("pelicanGraphScope",
                scope.empty() ? QStringLiteral("root") : scope_text);
    QStringList trail;
    for (const auto &step : model_->breadcrumb())
        trail.push_back(step.empty() ? QStringLiteral("root") : qtext(step));
    setProperty("pelicanGraphBreadcrumb", trail.join(QStringLiteral("/")));
    setProperty("pelicanGraphNodeCount", static_cast<qulonglong>(nodes.size()));
    setProperty("pelicanGraphEdgeCount", static_cast<qulonglong>(edges.size()));
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

void GraphCanvasScene::applyEdgeEmphasis() {
    // Which edges touch the selected node is the one thing the canvas says
    // about a selection, so it is applied in one pass over every edge rather
    // than remembered per item.
    const QString selected =
        selected_node_ ? qtext(*selected_node_) : QString{};
    for (QGraphicsItem *item : items()) {
        const QString kind = nodeGraphItemKind(*item);
        const bool edge = kind == QLatin1String{GraphCanvasEdgeItem};
        const bool arrow = kind == QLatin1String{GraphCanvasEdgeArrowItem};
        if (!edge && !arrow) continue;
        const bool touches =
            !selected.isEmpty() &&
            (item->data(NodeGraphFromNameRole).toString() == selected ||
             item->data(NodeGraphToNameRole).toString() == selected);
        // The picked edge is the one Delete would cut, so it is louder than an
        // edge that merely touches the picked node.
        // Every picked edge is loud, not only the last one: a bundle is made
        // out of several, and the drawing has to say which ones they are.
        const bool picked =
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
            if (auto *line = dynamic_cast<QGraphicsPathItem *>(item))
                line->setPen(QPen{colour, picked ? 3.0 : touches ? 2.2 : 1.6});
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
    QMetaObject::invokeMethod(
        this, [this] { applySceneRectNow(); }, Qt::QueuedConnection);
}

void GraphCanvasScene::applySceneRectNow() {
    scene_rect_update_queued_ = false;
    const QRectF bounds = itemsBoundingRect();
    setSceneRect(bounds.adjusted(-60.0, -60.0, 60.0, 60.0));
}

void GraphCanvasScene::selectNode(const std::string &id) {
    selected_node_ = id;
    selected_edge_.reset();
    selected_edges_.clear();
    clearControlSelection();
    setProperty("pelicanGraphSelectedNode", qtext(id));
    applyEdgeEmphasis();
    if (selectionChanged) selectionChanged();
}

void GraphCanvasScene::selectEdge(const GraphCanvasEdge &edge, bool additive) {
    const auto same = [&](const GraphCanvasEdge &other) {
        return other.from_node == edge.from_node &&
               other.from_port == edge.from_port &&
               other.to_node == edge.to_node && other.to_port == edge.to_port;
    };
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
    // The request is in the model's outbox; only the owner can post it.
    if (editSubmitted) editSubmitted();
}

void GraphCanvasScene::clearPendingConnection() {
    pending_connection_.reset();
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
    pending.node = socket.data(NodeGraphNameRole).toString().toStdString();
    pending.port = port.toStdString();
    pending.from_output = direction == QLatin1String{"output"};
    pending.origin = socket.sceneBoundingRect().center();
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
}

void GraphCanvasScene::finishConnection(const QPointF &position) {
    if (!pending_connection_ || model_ == nullptr) {
        clearPendingConnection();
        return;
    }
    const auto pending = *pending_connection_;
    clearPendingConnection();
    const QString wanted = pending.from_output ? QStringLiteral("input")
                                               : QStringLiteral("output");
    for (QGraphicsItem *item : items(position)) {
        if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasSocketItem}) continue;
        if (item->data(GraphCanvasSocketDirectionRole).toString() != wanted) continue;
        const auto port = item->data(GraphCanvasPortRole).toString();
        if (port.isEmpty()) return; // A folded node names no single port.
        const auto node = item->data(NodeGraphNameRole).toString().toStdString();
        const auto from_node = pending.from_output ? pending.node : node;
        const auto from_port = pending.from_output ? pending.port : port.toStdString();
        const auto to_node = pending.from_output ? node : pending.node;
        const auto to_port = pending.from_output ? port.toStdString() : pending.port;
        // A drag that does not type-check does not land: the socket is simply
        // not one this edge can end on.
        afterEdit(model_->connectInput(to_node, to_port, from_node, from_port));
        return;
    }
    // Dropped on nothing. From an input socket that is standing on an edge,
    // that is how the edge is cut; from an output socket it means nothing.
    if (pending.from_output) return;
    const auto drawn = model_->nodes();
    const auto owner = std::ranges::find(drawn, pending.node, &GraphCanvasNode::id);
    if (owner == drawn.end()) return;
    const auto port = std::ranges::find(owner->inputs, pending.port,
                                        &GraphCanvasPort::port);
    if (port == owner->inputs.end() || !port->connected) return;
    afterEdit(model_->disconnectInput(pending.node, pending.port));
}

void GraphCanvasScene::mousePressEvent(QGraphicsSceneMouseEvent *event) {
    // A control point is picked here and then handed to the base class, which
    // is what starts its drag. The node hunt at the bottom must not then take
    // the pick away again.
    bool picked_control = false;
    if (event->button() == Qt::LeftButton) {
        for (QGraphicsItem *item : items(event->scenePos())) {
            const QString kind = nodeGraphItemKind(*item);
            if (kind == QLatin1String{GraphCanvasEditorItem}) break;
            if (kind == QLatin1String{GraphCanvasCollapseItem}) {
                event->accept();
                // Folding rebuilds the scene, which deletes the item this
                // press is standing on. Same reason as the breadcrumb: do it
                // from the event loop, not from inside the dispatch.
                const auto id = item->data(NodeGraphNameRole).toString().toStdString();
                QMetaObject::invokeMethod(
                    this, [this, id] { toggleCollapsed(id); },
                    Qt::QueuedConnection);
                return;
            }
            if (kind == QLatin1String{GraphCanvasSocketItem}) {
                event->accept();
                selectNode(item->data(NodeGraphNameRole).toString().toStdString());
                beginConnection(*item);
                return;
            }
            if (kind == QLatin1String{GraphCanvasValuePillItem}) {
                event->accept();
                const auto node = item->data(NodeGraphNameRole).toString().toStdString();
                selectNode(node);
                beginValueEdit(node, item->data(GraphCanvasPortRole).toString().toStdString());
                return;
            }
            // A control point is picked *and* dragged by the same press, so
            // the event is not accepted here: the base class still has to turn
            // it into a grab. Picking is the same work a node's press does,
            // and it is done in the same place.
            if (kind == QLatin1String{GraphCanvasRerouteItem}) {
                selectReroute(
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
                selectTie(static_cast<std::size_t>(
                              item->data(GraphCanvasControlIndexRole).toULongLong()),
                          (event->modifiers() & Qt::ControlModifier) != 0);
                picked_control = true;
                break;
            }
            if (kind == QLatin1String{GraphCanvasEdgeItem} ||
                kind == QLatin1String{GraphCanvasEdgeArrowItem}) {
                event->accept();
                selectEdge(GraphCanvasEdge{
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
        selectNode(item->data(NodeGraphNameRole).toString().toStdString());
        return;
    }
}

void GraphCanvasScene::mouseMoveEvent(QGraphicsSceneMouseEvent *event) {
    if (pending_connection_ && pending_edge_ != nullptr) {
        event->accept();
        pending_edge_->setPath(
            curveGeometryBetween(pending_connection_->from_output
                                     ? pending_connection_->origin
                                     : event->scenePos(),
                                 pending_connection_->from_output
                                     ? event->scenePos()
                                     : pending_connection_->origin)
                .path);
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
    QMenu menu;
    // Both handlers run from the event loop rather than from inside the menu's
    // own: the palette is a modal dialog, and opening one while the menu is
    // still dispatching nests two modal loops.
    if (addNodeRequested) {
        QAction *add = menu.addAction(QObject::tr("Add node..."));
        QObject::connect(add, &QAction::triggered, this, [this] {
            const QPointF at = context_position_;
            QMetaObject::invokeMethod(
                this,
                [this, at] {
                    if (addNodeRequested) addNodeRequested(at);
                },
                Qt::QueuedConnection);
        });
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
    if (menu.actions().isEmpty()) {
        QGraphicsScene::contextMenuEvent(event);
        return;
    }
    event->accept();
    menu.exec(event->screenPos());
}

// One editor at a time, over the thing it edits. It owns nothing the scene
// owns: the proxy is an item like any other, and clearing the scene ends it.
struct GraphCanvasScene::InlineEditor {
    enum class Kind { label, value };

    Kind kind = Kind::label;
    std::string node;
    std::string port;
    ValueEditorKind value_kind = ValueEditorKind::number;
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
                             editor_->node, editor_->port,
                             editorText(editor_->widget)};
}

void GraphCanvasScene::restoreInlineEdit(const CarriedInlineEdit &carried) {
    const bool opened = carried.label
                            ? beginLabelEdit(carried.node)
                            : beginValueEdit(carried.node, carried.port);
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
QGraphicsItem *findHeadingLabel(QGraphicsScene &scene, const QString &node) {
    for (QGraphicsItem *item : scene.items()) {
        if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasNodeLabelItem}) continue;
        if (item->data(NodeGraphNameRole).toString() != node) continue;
        if (!item->data(GraphCanvasHeadingRole).toBool()) continue;
        if (item->data(GraphCanvasLabelRole).toString().isEmpty()) continue;
        return item;
    }
    return nullptr;
}

QGraphicsItem *findPill(QGraphicsScene &scene, const QString &node,
                        const QString &port) {
    for (QGraphicsItem *item : scene.items()) {
        if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasValuePillItem}) continue;
        if (item->data(NodeGraphNameRole).toString() != node) continue;
        if (item->data(GraphCanvasPortRole).toString() != port) continue;
        if (item->data(GraphCanvasValueTextRole).toString().isEmpty()) continue;
        return item;
    }
    return nullptr;
}

QString editorText(const QWidget *widget) {
    if (const auto *line = qobject_cast<const QLineEdit *>(widget)) return line->text();
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
    if (model_ == nullptr || !model_->canEdit()) return false;
    const auto drawn = model_->nodes();
    const auto found = std::ranges::find(drawn, id, &GraphCanvasNode::id);
    if (found == drawn.end()) return false;
    closeEditor();
    QGraphicsItem *anchor = findHeadingLabel(*this, qtext(id));
    if (anchor == nullptr) return false;

    auto editor = std::make_unique<InlineEditor>();
    editor->kind = InlineEditor::Kind::label;
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
    if (model_ == nullptr || !model_->canEdit() || port.empty()) return false;
    const auto drawn = model_->nodes();
    const auto found = std::ranges::find(drawn, id, &GraphCanvasNode::id);
    if (found == drawn.end()) return false;
    const auto entry = std::ranges::find(found->inputs, port, &GraphCanvasPort::port);
    if (entry == found->inputs.end() || entry->connected) return false;
    closeEditor();
    QGraphicsItem *anchor = findPill(*this, qtext(id), qtext(port));
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
    editor->node = id;
    editor->port = port;
    editor->value_kind = editorKindFor(current);
    switch (editor->value_kind) {
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
    closeEditor();
    if (kind == InlineEditor::Kind::label) {
        const bool submitted = model_->setNodeLabel(node, text.toStdString());
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
    }
    const bool submitted = model_->setNodeParameter(node, port, value);
    afterEdit(submitted);
    return submitted;
}

void GraphCanvasScene::placeNewNode(const std::string &id) {
    if (model_ == nullptr || id.empty()) return;
    // Beside the selected node when there is one, otherwise where the canvas
    // was right-clicked. Either way it is session state, like every position.
    QPointF position = context_position_;
    if (selected_node_) {
        if (const auto found = session_node_positions_.find(
                {model_->currentScope(), *selected_node_});
            found != session_node_positions_.end()) {
            position = found->second +
                       QPointF{NodeGraphNodeWidth + NodeGraphHorizontalGap, 0.0};
        } else if (QGraphicsItem *item = findNodeItem(
                       *this, graphCanvasKinds(), qtext(model_->currentScope()),
                       qtext(*selected_node_))) {
            position = item->pos() +
                       QPointF{NodeGraphNodeWidth + NodeGraphHorizontalGap, 0.0};
        }
    }
    session_node_positions_[{model_->currentScope(), id}] = position;
}

bool GraphCanvasScene::addFromPalette(const std::string &op,
                                      const std::string &definition) {
    if (model_ == nullptr || !model_->canEdit()) return false;
    // The id the model is about to choose is the one the position is filed
    // under: asking twice would answer differently once the node exists.
    const auto ops = model_->addNodeOps(op, definition);
    if (ops.empty()) return false;
    const auto &added = ops.back();
    if (!model_->addNode(op, definition)) return false;
    placeNewNode(added.at("node").value("id", std::string{}));
    afterEdit(true);
    return true;
}

bool GraphCanvasScene::deleteSelection() {
    if (model_ == nullptr) return false;
    if (selected_edge_)
        return model_->disconnectInput(selected_edge_->to_node, selected_edge_->to_port);
    if (selected_node_) return model_->removeNode(*selected_node_);
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
    if (model_ && !editingInline() && !dragging() && !layoutPending())
        afterEdit(model_->undo());
}

void GraphCanvasScene::redoEdit() {
    if (model_ && !editingInline() && !dragging() && !layoutPending())
        afterEdit(model_->redo());
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

void GraphCanvasScene::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) {
    if (model_ != nullptr && event->button() == Qt::LeftButton) {
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
                QMetaObject::invokeMethod(
                    this,
                    [this, index] {
                        if (index >= layout_.ties.size()) return;
                        std::ranges::reverse(layout_.ties[index].edges);
                        noteLayoutChange();
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
                // Same reason as the breadcrumb: the item under the cursor
                // cannot be deleted while Qt is still dispatching this event.
                QMetaObject::invokeMethod(
                    this, [this, edge, at] { insertReroute(edge, at); },
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
            beginLabelEdit(item->data(NodeGraphNameRole).toString().toStdString());
            return;
        }
        for (QGraphicsItem *item : items(event->scenePos())) {
            if (!isNodeSurface(nodeGraphItemKind(*item))) continue;
            const auto id = item->data(NodeGraphNameRole).toString().toStdString();
            if (!model_->canEnterInstance(id)) break;
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
        setToolTip(tr("Scene time, left to right. Drag to go to a time."));
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

} // namespace

struct GraphCanvasWidget::Impl {
    GraphCanvasWidget &view;
    GraphCanvasReadCapability graph;
    GraphCanvasModel model;
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
    QLineEdit *label_edit = nullptr;
    QPushButton *apply_label = nullptr;
    QPushButton *add_node = nullptr;
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

        notice = new QLabel(&view);
        notice->setObjectName(QStringLiteral("pelican.graphCanvasNotice"));
        applyPanelNoticeStyle(notice);
        notice->hide();
        layout->addWidget(notice);

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
        canvas->setDragMode(QGraphicsView::RubberBandDrag);
        canvas->setRenderHint(QPainter::Antialiasing, true);
        // Delete has to reach the scene, and a right-click on the canvas has to
        // reach the scene's own handler rather than the view's context menu.
        canvas->setFocusPolicy(Qt::StrongFocus);
        canvas->setContextMenuPolicy(Qt::DefaultContextMenu);
        splitter->addWidget(canvas);
        auto *side = new QWidget(splitter);
        auto *side_layout = new QVBoxLayout(side);
        side_layout->setContentsMargins(0, 0, 0, 0);
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
        details = new QTreeWidget(side);
        details->setObjectName(QStringLiteral("pelican.graphCanvasDetails"));
        details->setColumnCount(2);
        details->setHeaderLabels({QObject::tr("Field"), QObject::tr("Value")});
        details->setRootIsDecorated(true);
        side_layout->addWidget(details, 1);
        splitter->addWidget(side);
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
            // at is the node whose outputs they mean.
            const auto &selected = scene->selectedNode();
            model.setTimelineNode(selected ? *selected : std::string{});
            showDetails();
            synchronizeTimeline();
        };
        // Every canvas gesture is applied the moment it finishes, so the only
        // thing left to do here is post what the model already built.
        scene->editSubmitted = [this] { dispatch(); };
        scene->addNodeRequested = [this](const QPointF &) { openPalette(); };
        scene->fitViewRequested = [this] { canvas->fitView(); };
        // A scope shown for the first time -- which includes the first draw
        // after a document arrives -- is fitted; one being returned to keeps
        // the zoom and scroll it was left at.
        scene->repopulated = [this] {
            canvas->showScope(qtext(model.currentScope()));
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
                if (scope == model.currentScope()) return;
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
            if (model.setNodeLabel(*selected, label_edit->text().toStdString()))
                dispatch();
        };
        QObject::connect(apply_label, &QPushButton::clicked, &view, rename);
        QObject::connect(label_edit, &QLineEdit::returnPressed, &view, rename);
        QObject::connect(add_node, &QPushButton::clicked, &view,
                         [this] { openPalette(); });

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
                model.setConnected(available);
                dispatch();
            });

        synchronize();
    }

    void deliverResult(qint64 request_id, const QByteArray &result_json) {
        const auto found = transport_requests.find(request_id);
        if (found == transport_requests.end()) return;
        const auto model_id = found.value();
        transport_requests.erase(found);
        model.receiveRpcResult(
            model_id,
            std::string_view(result_json.constData(),
                             static_cast<std::size_t>(result_json.size())));
        dispatch();
    }

    void deliverFailure(qint64 request_id, const QString &message) {
        const auto found = transport_requests.find(request_id);
        if (found == transport_requests.end()) return;
        const auto model_id = found.value();
        transport_requests.erase(found);
        model.receiveRpcFailure(model_id, message.toStdString());
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

    void syncScopeControls() {
        const QSignalBlocker block{scopes};
        const auto current = qtext(model.currentScope());
        scopes->clear();
        for (const auto &scope : model.scopes())
            scopes->addItem(qtext(scope.label), qtext(scope.id));
        const int index = scopes->findData(current);
        scopes->setCurrentIndex(index < 0 ? 0 : index);
        QStringList trail;
        for (const auto &step : model.breadcrumb())
            trail.push_back(step.empty() ? QStringLiteral("root") : qtext(step));
        breadcrumb->setText(trail.join(QStringLiteral(" / ")));
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

    // The "Add node..." palette: the catalog, narrowed by domain and by what
    // the user types, and an instance also needs the definition it stands on.
    void openPalette() {
        if (!model.canEdit()) return;
        QDialog dialog{&view};
        dialog.setObjectName(QStringLiteral("pelican.graphCanvasPalette"));
        dialog.setWindowTitle(QObject::tr("Add node"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *filters = new QHBoxLayout;
        auto *domains = new QComboBox(&dialog);
        domains->setObjectName(QStringLiteral("pelican.graphCanvasPaletteDomain"));
        domains->addItem(QObject::tr("all domains"), QString{});
        for (const auto &domain : model.paletteDomains())
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
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                                 QDialogButtonBox::Cancel,
                                             &dialog);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        const auto reload = [&] {
            list->clear();
            for (const auto &entry : model.paletteOperations(
                     search->text().toStdString(),
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
            definitions->setVisible(
                row != nullptr &&
                row->data(Qt::UserRole).toString() == QLatin1String{"graph.instance"});
        };
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
        reload();
        syncDefinition();
        if (dialog.exec() != QDialog::Accepted) return;
        auto *row = list->currentItem();
        if (row == nullptr) return;
        scene->addFromPalette(row->data(Qt::UserRole).toString().toStdString(),
                              definitions->currentData().toString().toStdString());
    }

    void showDetails() {
        details->clear();
        const auto &selected = scene->selectedNode();
        label_edit->setEnabled(selected.has_value() && model.canEdit());
        apply_label->setEnabled(selected.has_value() && model.canEdit());
        add_node->setEnabled(model.canEdit());
        if (!selected) {
            label_edit->clear();
            return;
        }
        const auto nodes = model.nodes();
        const auto found = std::ranges::find(nodes, *selected, &GraphCanvasNode::id);
        if (found == nodes.end()) {
            label_edit->clear();
            return;
        }
        label_edit->setText(qtext(found->label));
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
            row(QObject::tr("definition"), qtext(found->definition));
        if (!found->parameters.empty()) {
            auto *parameters = row(QObject::tr("parameters"), {});
            for (const auto &[name, value] : found->parameters.items()) {
                auto *entry = new QTreeWidgetItem(parameters);
                entry->setText(0, qtext(name));
                entry->setText(1, qtext(value.dump()));
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
                entry->setText(1,
                    port.has_value
                        ? QStringLiteral("%1  ·  %2")
                              .arg(type_text(port.type), numberText(port.value))
                        : type_text(port.type));
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
        undo_action->setEnabled(history_available && model.canUndo());
        redo_action->setEnabled(history_available && model.canRedo());
        const bool content_changed = rendered_content != model.contentRevision();
        const bool report_changed = rendered_report != model.reportRevision();
        // A redraw deletes the in-place editor, so it waits for the editor to
        // close. Live reads four times a second (AG-3b); without this, typing
        // a value while the overlay is following the engine would be
        // impossible, and the reason would be invisible. Nothing is lost: the
        // rendered marks are left behind, so the next tick draws it. N-6 adds
        // the other half of the same rule: a drag in progress is a hand on an
        // item, and rebuilding would delete what it is holding.
        if ((content_changed || report_changed) && !scene->editingInline() &&
            !scene->dragging()) {
            rendered_content = model.contentRevision();
            rendered_report = model.reportRevision();
            scene->repopulate();
            syncScopeControls();
            syncValueControls();
            showDetails();
        }
        const auto message = model.notice();
        if (message.kind == InspectorNoticeKind::None) {
            notice->clear();
            notice->hide();
        } else {
            const QString whole = qtext(message.message);
            notice->setToolTip(whole);
            notice->setText(oneLine(whole));
            notice->setStyleSheet(message.kind == InspectorNoticeKind::Error
                                      ? QStringLiteral("color: #c0392b;")
                                      : QStringLiteral("color: #7f8c8d;"));
            notice->show();
        }
        refresh->setEnabled(model.canRefresh());
        synchronizeTimeline();
    }
};

GraphCanvasWidget::GraphCanvasWidget(GraphCanvasReadCapability graph,
                                     QWidget *parent)
    : QWidget{parent}, impl_{std::make_unique<Impl>(*this, std::move(graph))} {
    setObjectName(QStringLiteral("pelican.graphCanvas"));
}

GraphCanvasWidget::~GraphCanvasWidget() = default;

void GraphCanvasWidget::refresh() {
    impl_->model.refresh();
    impl_->dispatch();
}

void GraphCanvasWidget::poll() {
    impl_->model.poll();
    impl_->dispatch();
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
    impl_->scene->repopulate();
    impl_->scene->selectNode(id);
    impl_->syncScopeControls();
    impl_->syncValueControls();
    impl_->showDetails();
    return true;
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

GraphCanvasModel &GraphCanvasWidget::model() { return impl_->model; }
GraphCanvasScene &GraphCanvasWidget::scene() { return *impl_->scene; }

} // namespace PelicanStudio
