#include "graphcanvasdrawing.hpp"

#include "graphcanvaswidget.hpp"
#include "studiotheme.hpp"

#include <QBrush>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QPainterPathStroker>
#include <QPen>

#include <algorithm>
#include <utility>

namespace PelicanStudio::GraphCanvasDrawing {
namespace {

QString qtext(const std::string &value) {
    return QString::fromStdString(value);
}

QFont drawingFont(qreal size_offset, bool bold = false) {
    QFont font;
    font.setPointSizeF(font.pointSizeF() + size_offset);
    if (bold) font.setBold(true);
    return font;
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
    // LP-4c: a loop is structure the way an instance is.
    if (node.op == "graph.instance" || node.loop)
        return theme.domainTint(QStringLiteral("instance"));
    if (node.op == "graph.input")
        return theme.domainTint(QStringLiteral("input"));
    if (node.domain.empty())
        return theme.domainTint(QStringLiteral("structural"));
    return theme.domainTint(QString::fromStdString(node.domain));
}

Value rowValue(const GraphCanvasPort &port) {
    if (port.connected) return {};
    // A published generation outranks the authored number: it is what the
    // port was actually fed, overrides included.
    if (port.has_value)
        return Value{numberText(port.value), qtext(port.value_source)};
    if (port.has_parameter)
        return Value{qtext(port.parameter.dump()), QStringLiteral("stored")};
    return {};
}

QString pillText(const Value &value) {
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

} // namespace

QString portSummary(const GraphCanvasPort &port) {
    QString text = qtext(port.port);
    if (port.connected)
        text += QStringLiteral(" <- %1.%2")
                    .arg(qtext(port.source_node), qtext(port.source_port));
    // LP-4c: an edge that reads one element of a loop's p[] says which.
    if (port.connected && !port.source_element.empty())
        text += QStringLiteral(" [element %1]").arg(qtext(port.source_element));
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

Builder::Builder(QGraphicsScene &scene, QString scope)
    : scene_{scene}, scope_text_{std::move(scope)}, theme_{currentStudioTheme()},
      label_font_{drawingFont(-0.5)}, detail_font_{drawingFont(-1.0)},
      heading_font_{drawingFont(-0.5, true)},
      heading_metrics_{heading_font_}, row_metrics_{detail_font_} {}

Node Builder::prepareNode(const GraphCanvasModel &model, const GraphCanvasNode &node,
                          bool folded) const {
    Node entry;
    entry.node = &node;
    for (const auto &output : node.outputs)
        entry.outputs.push_back(Row{
            // LP-4c: a p[] is one value per element, and says so.
            output.bundle ? QObject::tr("%1 (each)").arg(qtext(output.port)) : qtext(output.port),
            qtext(output.port), qtext(output.type),
            socketStyleFor(graphCanvasSocketFacts(model, output.type)),
            // No source word on an output: there is only one place an
            // output value can come from, and naming it every row would
            // be a column of the same word.
            output.has_value ? Value{numberText(output.value), {}}
                             : Value{},
            false, false});
    for (const auto &port : node.inputs)
        entry.inputs.push_back(Row{
            qtext(port.port), qtext(port.port), qtext(port.type),
            socketStyleFor(graphCanvasSocketFacts(model, port.type)),
            rowValue(port), port.connected, port.declared});

    qreal width = GraphCanvasNodeMinimumWidth;
    width = std::max(width,
                     18.0 + heading_metrics_.horizontalAdvance(
                                qtext(node.heading())) +
                         12.0 +
                         row_metrics_.horizontalAdvance(
                             qtext(node.badge.empty() ? node.op : node.badge)) +
                         14.0);
    for (const auto &row : entry.outputs) {
        qreal needed = row_metrics_.horizontalAdvance(row.label) + 34.0;
        if (row.value.shown())
            needed += row_metrics_.horizontalAdvance(pillText(row.value)) + 24.0;
        width = std::max(width, needed);
    }
    for (const auto &row : entry.inputs) {
        qreal needed = row_metrics_.horizontalAdvance(row.label) + 34.0;
        if (row.value.shown())
            needed += row_metrics_.horizontalAdvance(pillText(row.value)) + 24.0;
        width = std::max(width, needed);
    }
    entry.width = std::min(width, GraphCanvasNodeMaximumWidth);

    // A folded node is its heading band and nothing else: the rows are not
    // drawn small, they are not drawn.
    entry.folded = folded;
    const auto row_count =
        static_cast<qreal>(entry.outputs.size() + entry.inputs.size());
    entry.height =
        entry.folded
            ? GraphCanvasHeaderHeight
            : GraphCanvasHeaderHeight + GraphCanvasSectionPadding +
                  row_count * GraphCanvasSocketRowHeight +
                  GraphCanvasSectionPadding;
    return entry;
}

NodeGraphLayout placeNodes(const std::vector<Node> &nodes,
                          const std::vector<GraphCanvasEdge> &edges, qreal top) {
    std::vector<LayoutNode> layout_nodes;
    layout_nodes.reserve(nodes.size());
    for (const Node &entry : nodes)
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
    return layoutNodeGraph(layout_nodes, layout_edges, layout_options);
}

void Builder::addNode(const Node &entry, int column, const QPointF &position,
                       Moved moved) {
    const GraphCanvasNode &node = *entry.node;
    const QString node_name = qtext(node.id);
    const QString heading = qtext(node.heading());
    const bool folded = entry.folded;
    auto *item = new MovableNodeItem(
        roundedEntityPath(entry.width, entry.height, GraphCanvasNodeCornerRadius),
        std::move(moved));
    scene_.addItem(item);
    item->setPos(position);
    item->setZValue(2.0);
    // The body is neutral; the heading band carries the domain colour, so
    // a long port list does not turn into a wall of one hue.
    item->setBrush(theme_.canvas.node_body);
    // LP-4c, decision 9: the node a refused edit pointed at has a red rim,
    // and says why when hovered.
    item->setPen(node.refused ? QPen{QColor{QStringLiteral("#c0392b")}, 2.6}
                              : QPen{theme_.canvas.node_border, 1.2});
    item->setData(GraphCanvasRefusalRole, node.refused ? qtext(node.refusal) : QString{});
    if (node.refused) item->setToolTip(qtext(node.refusal));
    item->setFlag(QGraphicsItem::ItemIsMovable, true);
    item->setFlag(QGraphicsItem::ItemIsSelectable, true);
    item->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
    item->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasNodeItem));
    item->setData(GraphCanvasScopeRole, scope_text_);
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
    addNodeHeader(item, entry);
    addNodeRows(item, entry);
}

void Builder::addNodeHeader(QGraphicsPathItem *item, const Node &entry) {
    const GraphCanvasNode &node = *entry.node;
    const QString node_name = qtext(node.id);
    const QString heading = qtext(node.heading());
    // LP-4c: a loop shows its mark and what it became ("expand x3") where
    // another node shows its operation.
    const QString type_name = qtext(node.badge.empty() ? node.op : node.badge);
    const qreal width = entry.width;
    const bool folded = entry.folded;
    const auto tag = [&](QGraphicsItem *child, const char *kind) {
        child->setData(NodeGraphItemKindRole, QString::fromLatin1(kind));
        child->setData(GraphCanvasScopeRole, scope_text_);
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
    collapse->setBrush(theme_.canvas.heading_text);
    collapse->setPen(QPen{Qt::NoPen});
    tag(collapse, GraphCanvasCollapseItem);
    collapse->setData(GraphCanvasHeadingRole, true);
    collapse->setData(GraphCanvasCollapsedRole, folded);
    collapse->setToolTip(folded ? QObject::tr("Show the sockets")
                               : QObject::tr("Fold the sockets away"));

    auto *title = new QGraphicsSimpleTextItem(heading, band);
    title->setFont(heading_font_);
    title->setBrush(theme_.canvas.heading_text);
    title->setPos(18.0, (GraphCanvasHeaderHeight -
                         heading_metrics_.height()) / 2.0);
    tag(title, GraphCanvasNodeLabelItem);
    title->setData(GraphCanvasLabelRole, heading);
    title->setData(GraphCanvasHeadingRole, true);
    title->setToolTip(QObject::tr("Double-click to rename"));

    // The operation type stays visible next to the chosen name: renaming a
    // node must never hide what it actually is.
    auto *kind = new QGraphicsSimpleTextItem(type_name, band);
    kind->setFont(detail_font_);
    kind->setBrush(theme_.canvas.dim_text);
    kind->setOpacity(0.78);
    kind->setPos(width - 10.0 - row_metrics_.horizontalAdvance(type_name),
                 (GraphCanvasHeaderHeight - row_metrics_.height()) / 2.0);
    tag(kind, GraphCanvasNodeLabelItem);
    kind->setData(GraphCanvasOpRole, qtext(node.op));
    kind->setData(GraphCanvasHeadingRole, true);
}

void Builder::addNodeRows(QGraphicsPathItem *item, const Node &entry) {
    const auto &outputs = entry.outputs;
    const auto &inputs = entry.inputs;
    const qreal width = entry.width;
    const bool folded = entry.folded;
    const qreal rows_top = GraphCanvasHeaderHeight + GraphCanvasSectionPadding;
    const QString node_name = qtext(entry.node->id);
    const QString heading = qtext(entry.node->heading());
    const auto tag = [&](QGraphicsItem *child, const char *kind) {
        child->setData(NodeGraphItemKindRole, QString::fromLatin1(kind));
        child->setData(GraphCanvasScopeRole, scope_text_);
        child->setData(NodeGraphNameRole, node_name);
    };

    const auto socket = [&](const Row &drawn, const QPointF &centre,
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
            dot->setBrush(theme_.canvas.dim_text);
            dot->setPen(QPen{theme_.canvas.node_border, 1.1});
            tag(dot, GraphCanvasSocketItem);
            dot->setData(GraphCanvasPortRole, QString{});
            dot->setData(GraphCanvasSocketDirectionRole,
                         QString::fromLatin1(direction));
            dot->setData(GraphCanvasCollapsedRole, true);
            dot->setToolTip(QObject::tr("%1 (folded)").arg(heading));
        };
        if (!inputs.empty()) anchor(QPointF{0.0, centre_y}, "input");
        if (!outputs.empty()) anchor(QPointF{width, centre_y}, "output");
        return;
    }

    qreal cursor_y = rows_top;
    for (const auto &drawn : outputs) {
        const qreal centre_y = cursor_y + GraphCanvasSocketRowHeight / 2.0;
        auto *name = new QGraphicsSimpleTextItem(drawn.label, item);
        name->setFont(detail_font_);
        name->setBrush(theme_.canvas.dim_text);
        name->setPos(width - 14.0 - row_metrics_.horizontalAdvance(drawn.label),
                     centre_y - row_metrics_.height() / 2.0);
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
                row_metrics_.horizontalAdvance(text) + 14.0;
            auto *band = new QGraphicsPathItem(
                roundedEntityPath(band_width, band_height,
                                  band_height / 2.0),
                item);
            band->setPos(10.0, centre_y - band_height / 2.0);
            band->setBrush(theme_.canvas.pill_bg);
            band->setPen(QPen{theme_.canvas.node_border, 0.8});
            band->setToolTip(
                QObject::tr("%1 = %2 in the published generation (rendering)")
                    .arg(drawn.label, text));
            tag(band, GraphCanvasValueBandItem);
            band->setData(GraphCanvasPortRole, drawn.port);
            band->setData(GraphCanvasValueTextRole, text);
            auto *value = new QGraphicsSimpleTextItem(text, band);
            value->setFont(detail_font_);
            value->setBrush(theme_.canvas.heading_text);
            value->setPos(7.0, (band_height - row_metrics_.height()) / 2.0);
            tag(value, GraphCanvasValueBandItem);
            value->setData(GraphCanvasPortRole, drawn.port);
            value->setData(GraphCanvasValueTextRole, text);
        }
        cursor_y += GraphCanvasSocketRowHeight;
    }
    for (const auto &drawn : inputs) {
        const qreal centre_y = cursor_y + GraphCanvasSocketRowHeight / 2.0;
        auto *name = new QGraphicsSimpleTextItem(drawn.label, item);
        name->setFont(detail_font_);
        // A port the catalog never declared is dimmer than one it did,
        // and "dimmer" has to survive both themes: it is the theme's own
        // dim colour, faded, rather than a second fixed grey.
        QColor undeclared = theme_.canvas.dim_text;
        undeclared.setAlphaF(0.72F);
        name->setBrush(drawn.declared ? theme_.canvas.dim_text : undeclared);
        name->setPos(14.0, centre_y - row_metrics_.height() / 2.0);
        tag(name, GraphCanvasNodeLabelItem);
        name->setData(GraphCanvasPortRole, drawn.port);
        socket(drawn, QPointF{0.0, centre_y}, "input");

        if (drawn.value.shown()) {
            const QString text = pillText(drawn.value);
            const qreal pill_height = GraphCanvasSocketRowHeight - 6.0;
            const qreal pill_width =
                row_metrics_.horizontalAdvance(text) + 14.0;
            auto *pill = new QGraphicsPathItem(
                roundedEntityPath(pill_width, pill_height,
                                  pill_height / 2.0),
                item);
            pill->setPos(width - 10.0 - pill_width,
                         centre_y - pill_height / 2.0);
            pill->setBrush(theme_.canvas.pill_bg);
            pill->setPen(QPen{theme_.canvas.node_border, 0.8});
            tag(pill, GraphCanvasValuePillItem);
            pill->setData(GraphCanvasPortRole, drawn.port);
            pill->setData(GraphCanvasValueTextRole, drawn.value.text);
            pill->setData(GraphCanvasValueSourceRole, drawn.value.source);
            auto *value = new QGraphicsSimpleTextItem(text, pill);
            value->setFont(detail_font_);
            value->setBrush(theme_.canvas.heading_text);
            value->setPos(7.0, (pill_height - row_metrics_.height()) / 2.0);
            tag(value, GraphCanvasValuePillItem);
            value->setData(GraphCanvasPortRole, drawn.port);
        }
        cursor_y += GraphCanvasSocketRowHeight;
    }
}

QGraphicsPathItem *Builder::addEdge(const GraphCanvasEdge &edge,
                                  const QVector<QPointF> &bends) {
    auto *line = new GraphEdgeItem;
    scene_.addItem(line);
    if (!bends.isEmpty()) setNodeGraphBends(*line, bends);
    line->setZValue(0.0);
    line->setPen(QPen{theme_.canvas.edge, 1.6});
    line->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasEdgeItem));
    line->setData(GraphCanvasScopeRole, scope_text_);
    line->setData(NodeGraphFromNameRole, qtext(edge.from_node));
    line->setData(NodeGraphToNameRole, qtext(edge.to_node));
    line->setData(GraphCanvasFromPortRole, qtext(edge.from_port));
    line->setData(GraphCanvasToPortRole, qtext(edge.to_port));

    auto *arrow = new QGraphicsPolygonItem;
    scene_.addItem(arrow);
    arrow->setZValue(1.0);
    arrow->setBrush(theme_.canvas.edge);
    arrow->setPen(Qt::NoPen);
    arrow->setData(NodeGraphItemKindRole,
                   QString::fromLatin1(GraphCanvasEdgeArrowItem));
    arrow->setData(GraphCanvasScopeRole, scope_text_);
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
    return line;
}

void Builder::addReroute(const GraphCanvasEdgeKey &key, std::size_t index,
                        const QPointF &position, Moved moved) {
    QPainterPath circle;
    circle.addEllipse(QPointF{0.0, 0.0}, GraphCanvasRerouteSize / 2.0,
                      GraphCanvasRerouteSize / 2.0);
    auto *dot = new MovableNodeItem(circle, std::move(moved));
    scene_.addItem(dot);
    dot->setPos(position);
    dot->setZValue(2.6);
    dot->setBrush(theme_.canvas.pill_bg);
    dot->setPen(QPen{theme_.canvas.edge_selected, 1.4});
    dot->setFlag(QGraphicsItem::ItemIsMovable, true);
    dot->setFlag(QGraphicsItem::ItemIsSelectable, true);
    dot->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
    dot->setData(NodeGraphItemKindRole,
                 QString::fromLatin1(GraphCanvasRerouteItem));
    dot->setData(GraphCanvasScopeRole, scope_text_);
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

void Builder::addTie(const GraphCanvasTie &tie, std::size_t index, Moved moved) {
    // `slots` is a Qt keyword, so the count of them cannot be called that.
    const auto carried = static_cast<qreal>(tie.edges.size() - 1);
    const qreal height =
        carried * GraphCanvasTieSpacing + 2.0 * GraphCanvasTiePadding;
    auto *bar = new MovableNodeItem(
        roundedEntityPath(GraphCanvasTieWidth, height,
                          GraphCanvasTieWidth / 2.0),
        std::move(moved));
    scene_.addItem(bar);
    bar->setPos(tie.position - QPointF{GraphCanvasTieWidth / 2.0,
                                   GraphCanvasTiePadding});
    bar->setZValue(2.5);
    bar->setBrush(theme_.canvas.edge_selected);
    bar->setPen(QPen{theme_.canvas.node_border, 1.0});
    bar->setFlag(QGraphicsItem::ItemIsMovable, true);
    bar->setFlag(QGraphicsItem::ItemIsSelectable, true);
    bar->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
    bar->setData(NodeGraphItemKindRole,
                 QString::fromLatin1(GraphCanvasTieItem));
    bar->setData(GraphCanvasScopeRole, scope_text_);
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

void Builder::addBreadcrumb(const QStringList &trail, qreal top,
                           std::function<void()> leave_scope) {
    const QString text = trail.join(QStringLiteral(" / "));
    const qreal width =
        std::max<qreal>(220.0, row_metrics_.horizontalAdvance(text) + 96.0);
    auto *crumb = new BreadcrumbItem{
        QRectF{0.0, 0.0, width, NodeGraphBreadcrumbHeight}, std::move(leave_scope)};
    scene_.addItem(crumb);
    crumb->setPos(0.0, -NodeGraphBreadcrumbHeight - 18.0 + top);
    crumb->setZValue(3.0);
    crumb->setBrush(theme_.canvas.pill_bg);
    crumb->setPen(QPen{theme_.canvas.edge, 1.0});
    crumb->setAcceptedMouseButtons(Qt::LeftButton);
    crumb->setData(NodeGraphItemKindRole,
                   QString::fromLatin1(GraphCanvasBreadcrumbItem));
    crumb->setData(GraphCanvasScopeRole, scope_text_);
    auto *crumb_label = new QGraphicsSimpleTextItem(
        QStringLiteral("%1   (click to leave)").arg(text), crumb);
    crumb_label->setFont(label_font_);
    crumb_label->setBrush(theme_.canvas.heading_text);
    crumb_label->setPos(12.0, 8.0);
    crumb_label->setData(NodeGraphItemKindRole,
                         QString::fromLatin1(GraphCanvasBreadcrumbItem));
}

} // namespace PelicanStudio::GraphCanvasDrawing
