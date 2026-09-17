#include "graphcanvaswidget.hpp"

#include <QBrush>
#include <QByteArray>
#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
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
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

QString qtext(const std::string &value) {
    return QString::fromStdString(value);
}

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
    if (node.op == "graph.instance") return QColor{QStringLiteral("#7653a6")};
    if (node.op == "graph.input") return QColor{QStringLiteral("#2f6f7a")};
    if (node.domain == "rendering") return QColor{QStringLiteral("#3f5d86")};
    if (node.domain == "animation") return QColor{QStringLiteral("#7a5a34")};
    if (node.domain == "values") return QColor{QStringLiteral("#3d6b4d")};
    return QColor{QStringLiteral("#4a5560")};
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
           kind == QLatin1String{GraphCanvasSocketItem} ||
           kind == QLatin1String{GraphCanvasValuePillItem};
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
        findSocket(scene, scope, from_name,
                   edge.data(GraphCanvasFromPortRole).toString(),
                   QStringLiteral("output"));
    QGraphicsItem *to_socket =
        findSocket(scene, scope, to_name,
                   edge.data(GraphCanvasToPortRole).toString(),
                   QStringLiteral("input"));

    const CurveGeometry geometry =
        from_socket != nullptr && to_socket != nullptr
            ? curveGeometryBetween(from_socket->sceneBoundingRect().center(),
                                   to_socket->sceneBoundingRect().center())
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
        if (auto *edge = dynamic_cast<QGraphicsPathItem *>(item))
            routeGraphCanvasEdge(scene, *edge);
    }
}

} // namespace

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

GraphCanvasScene::GraphCanvasScene(QObject *parent) : QGraphicsScene{parent} {}
GraphCanvasScene::~GraphCanvasScene() = default;

void GraphCanvasScene::setModel(GraphCanvasModel *model) {
    model_ = model;
    session_node_positions_.clear();
    pruned_revision_.reset();
    selected_node_.reset();
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
}

void GraphCanvasScene::repopulate() {
    ++rebuild_generation_;
    rebuilding_ = true;
    clear();
    selected_node_.reset();
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
    const QString scope_text = qtext(scope);
    const auto nodes = model_->nodes();
    const auto edges = model_->edges();

    std::set<std::string, std::less<>> visible;
    for (const auto &node : nodes) visible.insert(node.id);
    std::vector<NodeGraphEdgeKey> relation;
    relation.reserve(edges.size());
    for (const auto &edge : edges)
        relation.push_back(NodeGraphEdgeKey{edge.from_node, edge.to_node});
    const auto columns = longestPathColumns(relation, visible);

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
    std::map<std::string, QGraphicsPathItem *, std::less<>> items;
    // A node's height now follows its port count, so a fixed row pitch would
    // overlap a tall node with the one under it. Each column advances by what
    // it has actually placed.
    std::map<int, qreal> column_cursor;
    for (const auto &node : nodes) {
        const QString node_name = qtext(node.id);
        const QString heading = qtext(node.heading());
        const QString type_name = qtext(node.op);

        // The rows first: the node is as wide as its widest row and as tall as
        // the number of ports, the way a socket list has to be.
        struct DrawnRow {
            QString port;
            QString label;
            QString type;
            SocketStyle style;
            RowValue value;
            bool connected = false;
            bool declared = false;
        };
        std::vector<DrawnRow> outputs;
        std::vector<DrawnRow> inputs;
        for (const auto &output : node.outputs)
            outputs.push_back(DrawnRow{
                qtext(output.port), qtext(output.port), qtext(output.type),
                socketStyleFor(graphCanvasSocketFacts(*model_, output.type)),
                {}, false, false});
        for (const auto &port : node.inputs)
            inputs.push_back(DrawnRow{
                qtext(port.port), qtext(port.port), qtext(port.type),
                socketStyleFor(graphCanvasSocketFacts(*model_, port.type)),
                rowValue(port), port.connected, port.declared});

        qreal width = GraphCanvasNodeMinimumWidth;
        width = std::max(width, 18.0 + heading_metrics.horizontalAdvance(heading) +
                                    12.0 + row_metrics.horizontalAdvance(type_name) +
                                    14.0);
        for (const auto &row : outputs)
            width = std::max(width,
                             row_metrics.horizontalAdvance(row.label) + 34.0);
        for (const auto &row : inputs) {
            qreal needed = row_metrics.horizontalAdvance(row.label) + 34.0;
            if (row.value.shown())
                needed += row_metrics.horizontalAdvance(pillText(row.value)) + 24.0;
            width = std::max(width, needed);
        }
        width = std::min(width, GraphCanvasNodeMaximumWidth);

        const qreal rows_top = GraphCanvasHeaderHeight + GraphCanvasSectionPadding;
        const auto row_count = static_cast<qreal>(outputs.size() + inputs.size());
        const qreal height =
            rows_top + row_count * GraphCanvasSocketRowHeight +
            GraphCanvasSectionPadding;

        const auto column_entry = columns.find(node.id);
        const int column = column_entry == columns.end() ? 0 : column_entry->second;
        const auto placed = column_cursor.find(column);
        if (placed == column_cursor.end())
            column_cursor.emplace(
                column,
                top + (column % 2 == 1 ? NodeGraphStaggerOffset * 0.25 : 0.0));
        auto &cursor = column_cursor.at(column);
        const QPointF laid_out{
            column * (NodeGraphNodeWidth + NodeGraphHorizontalGap), cursor};
        cursor += height + NodeGraphVerticalGap;
        const auto position_key = std::pair{scope, node.id};
        QPointF position = laid_out;
        if (const auto retained = session_node_positions_.find(position_key);
            retained != session_node_positions_.end()) {
            position = retained->second;
        }

        auto *item = new MovableNodeItem(
            roundedEntityPath(width, height, GraphCanvasNodeCornerRadius),
            [this, position_key, scope](const QPointF &moved) {
                if (rebuilding_) return;
                session_node_positions_[position_key] = moved;
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
        item->setBrush(QColor{QStringLiteral("#272e36")});
        item->setPen(QPen{QColor{QStringLiteral("#0f1418")}, 1.2});
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

        // The collapse arrow is drawn but inert in this version: the section
        // it would fold is what N-2 makes editable.
        QPainterPath arrow;
        arrow.moveTo(0.0, -4.0);
        arrow.lineTo(5.0, 0.0);
        arrow.lineTo(0.0, 4.0);
        arrow.closeSubpath();
        auto *collapse = new QGraphicsPathItem(arrow, band);
        collapse->setPos(9.0, GraphCanvasHeaderHeight / 2.0);
        collapse->setBrush(QColor{QStringLiteral("#e8eef4")});
        collapse->setPen(Qt::NoPen);
        tag(collapse, GraphCanvasNodeHeaderItem);

        auto *title = new QGraphicsSimpleTextItem(heading, band);
        title->setFont(heading_font);
        title->setBrush(QColor{QStringLiteral("#f6f9fc")});
        title->setPos(18.0, (GraphCanvasHeaderHeight -
                             heading_metrics.height()) / 2.0);
        tag(title, GraphCanvasNodeLabelItem);
        title->setData(GraphCanvasLabelRole, heading);

        // The operation type stays visible next to the chosen name: renaming a
        // node must never hide what it actually is.
        auto *kind = new QGraphicsSimpleTextItem(type_name, band);
        kind->setFont(detail_font);
        kind->setBrush(QColor{QStringLiteral("#cfdae4")});
        kind->setOpacity(0.78);
        kind->setPos(width - 10.0 - row_metrics.horizontalAdvance(type_name),
                     (GraphCanvasHeaderHeight - row_metrics.height()) / 2.0);
        tag(kind, GraphCanvasNodeLabelItem);
        kind->setData(GraphCanvasOpRole, type_name);

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

        qreal cursor_y = rows_top;
        for (const auto &drawn : outputs) {
            const qreal centre_y = cursor_y + GraphCanvasSocketRowHeight / 2.0;
            auto *name = new QGraphicsSimpleTextItem(drawn.label, item);
            name->setFont(detail_font);
            name->setBrush(QColor{QStringLiteral("#dbe4ec")});
            name->setPos(width - 14.0 - row_metrics.horizontalAdvance(drawn.label),
                         centre_y - row_metrics.height() / 2.0);
            tag(name, GraphCanvasNodeLabelItem);
            name->setData(GraphCanvasPortRole, drawn.port);
            // Half outside the right edge, which is where the edge meets it.
            socket(drawn, QPointF{width, centre_y}, "output");
            cursor_y += GraphCanvasSocketRowHeight;
        }
        for (const auto &drawn : inputs) {
            const qreal centre_y = cursor_y + GraphCanvasSocketRowHeight / 2.0;
            auto *name = new QGraphicsSimpleTextItem(drawn.label, item);
            name->setFont(detail_font);
            name->setBrush(QColor{drawn.declared ? QStringLiteral("#dbe4ec")
                                                 : QStringLiteral("#b3c0cb")});
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
                pill->setBrush(QColor{QStringLiteral("#3a444f")});
                pill->setPen(QPen{QColor{QStringLiteral("#596776")}, 0.8});
                tag(pill, GraphCanvasValuePillItem);
                pill->setData(GraphCanvasPortRole, drawn.port);
                pill->setData(GraphCanvasValueTextRole, drawn.value.text);
                pill->setData(GraphCanvasValueSourceRole, drawn.value.source);
                auto *value = new QGraphicsSimpleTextItem(text, pill);
                value->setFont(detail_font);
                value->setBrush(QColor{QStringLiteral("#e6edf4")});
                value->setPos(7.0, (pill_height - row_metrics.height()) / 2.0);
                tag(value, GraphCanvasValuePillItem);
                value->setData(GraphCanvasPortRole, drawn.port);
            }
            cursor_y += GraphCanvasSocketRowHeight;
        }
    }

    const QFontMetricsF metrics{detail_font};
    for (const auto &edge : edges) {
        if (!items.contains(edge.from_node) || !items.contains(edge.to_node)) continue;
        auto *line = new QGraphicsPathItem;
        addItem(line);
        line->setZValue(0.0);
        line->setPen(QPen{QColor{QStringLiteral("#8fa3b4")}, 1.6});
        line->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasEdgeItem));
        line->setData(GraphCanvasScopeRole, scope_text);
        line->setData(NodeGraphFromNameRole, qtext(edge.from_node));
        line->setData(NodeGraphToNameRole, qtext(edge.to_node));
        line->setData(GraphCanvasFromPortRole, qtext(edge.from_port));
        line->setData(GraphCanvasToPortRole, qtext(edge.to_port));

        auto *arrow = new QGraphicsPolygonItem;
        addItem(arrow);
        arrow->setZValue(1.0);
        arrow->setBrush(QColor{QStringLiteral("#8fa3b4")});
        arrow->setPen(Qt::NoPen);
        arrow->setData(NodeGraphItemKindRole,
                       QString::fromLatin1(GraphCanvasEdgeArrowItem));
        arrow->setData(GraphCanvasScopeRole, scope_text);
        arrow->setData(NodeGraphFromNameRole, qtext(edge.from_node));
        arrow->setData(NodeGraphToNameRole, qtext(edge.to_node));

        // No edge label: the socket rows already name both ends, and a box in
        // the middle of every line repeated them.
        routeGraphCanvasEdge(*this, *line);
    }
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
        crumb->setBrush(QColor{QStringLiteral("#2a3440")});
        crumb->setPen(QPen{QColor{QStringLiteral("#7f8f9d")}, 1.0});
        crumb->setAcceptedMouseButtons(Qt::LeftButton);
        crumb->setData(NodeGraphItemKindRole,
                       QString::fromLatin1(GraphCanvasBreadcrumbItem));
        crumb->setData(GraphCanvasScopeRole, scope_text);
        auto *crumb_label = new QGraphicsSimpleTextItem(
            QStringLiteral("%1   (click to leave)").arg(text), crumb);
        crumb_label->setFont(label_font);
        crumb_label->setBrush(QColor{QStringLiteral("#e6eef5")});
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
    rebuilding_ = false;
    applySceneRectNow();
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
        const QColor colour{touches ? QStringLiteral("#e4eef8")
                                    : QStringLiteral("#7b8d9d")};
        if (edge) {
            if (auto *line = dynamic_cast<QGraphicsPathItem *>(item))
                line->setPen(QPen{colour, touches ? 2.2 : 1.6});
        } else if (auto *head = dynamic_cast<QGraphicsPolygonItem *>(item)) {
            head->setBrush(colour);
        }
    }
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

void GraphCanvasScene::mousePressEvent(QGraphicsSceneMouseEvent *event) {
    const auto generation = rebuild_generation_;
    QGraphicsScene::mousePressEvent(event);
    if (event->button() != Qt::LeftButton) return;
    // The press went to the breadcrumb and the scene is a different one now.
    if (generation != rebuild_generation_) return;
    for (QGraphicsItem *item : items(event->scenePos())) {
        if (!isNodeSurface(nodeGraphItemKind(*item))) continue;
        selected_node_ = item->data(NodeGraphNameRole).toString().toStdString();
        setProperty("pelicanGraphSelectedNode", item->data(NodeGraphNameRole));
        applyEdgeEmphasis();
        if (selectionChanged) selectionChanged();
        return;
    }
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

struct GraphCanvasWidget::Impl {
    GraphCanvasWidget &view;
    GraphCanvasReadCapability graph;
    GraphCanvasModel model;
    GraphCanvasScene *scene = nullptr;
    QGraphicsView *canvas = nullptr;
    QComboBox *scopes = nullptr;
    QComboBox *value_instances = nullptr;
    QPushButton *refresh = nullptr;
    QLabel *breadcrumb = nullptr;
    QLabel *notice = nullptr;
    QTreeWidget *details = nullptr;
    QHash<qint64, quint64> transport_requests;
    std::uint64_t rendered_content = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t rendered_report = std::numeric_limits<std::uint64_t>::max();

    explicit Impl(GraphCanvasWidget &owner, GraphCanvasReadCapability capability)
        : view{owner}, graph{std::move(capability)} {
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
        refresh = new QPushButton(QObject::tr("Refresh"), &view);
        refresh->setObjectName(QStringLiteral("pelican.graphCanvasRefresh"));
        controls->addWidget(refresh);
        layout->addLayout(controls);

        notice = new QLabel(&view);
        notice->setObjectName(QStringLiteral("pelican.graphCanvasNotice"));
        notice->setWordWrap(true);
        notice->hide();
        layout->addWidget(notice);

        auto *splitter = new QSplitter(Qt::Horizontal, &view);
        scene = new GraphCanvasScene(&view);
        canvas = new QGraphicsView(scene, splitter);
        canvas->setObjectName(QStringLiteral("pelican.graphCanvasView"));
        canvas->setDragMode(QGraphicsView::RubberBandDrag);
        canvas->setRenderHint(QPainter::Antialiasing, true);
        splitter->addWidget(canvas);
        details = new QTreeWidget(splitter);
        details->setObjectName(QStringLiteral("pelican.graphCanvasDetails"));
        details->setColumnCount(2);
        details->setHeaderLabels({QObject::tr("Field"), QObject::tr("Value")});
        details->setRootIsDecorated(true);
        splitter->addWidget(details);
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 2);
        layout->addWidget(splitter, 1);

        scene->setModel(&model);
        scene->scopeChanged = [this] {
            syncScopeControls();
            syncValueControls();
            showDetails();
        };
        scene->selectionChanged = [this] { showDetails(); };

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

        auto *poll = new QTimer(&view);
        poll->setInterval(250);
        QObject::connect(poll, &QTimer::timeout, &view, [this] {
            model.poll();
            dispatch();
        });
        poll->start();
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
        // get_graph_values answers for a root instance. Inside a definition
        // body there is nothing it could report, so the control says so.
        value_instances->setEnabled(model.currentScope().empty());
        const auto current = qtext(model.watchedInstance());
        value_instances->clear();
        value_instances->addItem(QObject::tr("(none)"), QString{});
        for (const auto &instance : model.valueInstances())
            value_instances->addItem(qtext(instance), qtext(instance));
        const int index = value_instances->findData(current);
        value_instances->setCurrentIndex(index < 0 ? 0 : index);
    }

    void showDetails() {
        details->clear();
        const auto &selected = scene->selectedNode();
        if (!selected) return;
        const auto nodes = model.nodes();
        const auto found = std::ranges::find(nodes, *selected, &GraphCanvasNode::id);
        if (found == nodes.end()) return;
        const auto row = [this](const QString &field, const QString &value) {
            auto *item = new QTreeWidgetItem(details);
            item->setText(0, field);
            item->setText(1, value);
            return item;
        };
        // The heading first, in the order the canvas shows it: the chosen name,
        // then what the node actually is. Editing the label is N-2.
        row(QObject::tr("label"),
            found->label.empty()
                ? QObject::tr("%1  (no label: the id is the heading)")
                      .arg(qtext(found->id))
                : qtext(found->label));
        row(QObject::tr("type"), qtext(found->op));
        row(QObject::tr("domain"),
            found->contract_known ? qtext(found->domain)
                                  : QObject::tr("(catalog not fetched)"));
        row(QObject::tr("evaluation"),
            found->contract_known ? qtext(found->evaluation)
                                  : QObject::tr("(catalog not fetched)"));
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
                entry->setText(1, type_text(port.type));
            }
            outputs->setExpanded(true);
        }
    }

    void synchronize() {
        const bool content_changed = rendered_content != model.contentRevision();
        const bool report_changed = rendered_report != model.reportRevision();
        if (content_changed || report_changed) {
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
            notice->setText(qtext(message.message));
            notice->setStyleSheet(message.kind == InspectorNoticeKind::Error
                                      ? QStringLiteral("color: #c0392b;")
                                      : QStringLiteral("color: #7f8c8d;"));
            notice->show();
        }
        refresh->setEnabled(model.canRefresh());
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
