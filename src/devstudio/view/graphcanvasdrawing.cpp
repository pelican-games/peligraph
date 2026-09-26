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
            qtext(output.port),
            // LP-4c: a p[] is one value per element, and says so. (LP-4d1:
            // in the label -- the port is the name an edge from it names.)
            output.bundle ? QObject::tr("%1 (each)").arg(qtext(output.port)) : qtext(output.port),
            qtext(output.type),
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

void Builder::setContainer(QGraphicsItem *container) { container_ = container; }

void Builder::place(QGraphicsItem *item) {
    if (container_ != nullptr) item->setParentItem(container_);
    else scene_.addItem(item);
}

void Builder::tag(QGraphicsItem *item) const {
    item->setData(GraphCanvasScopeRole, scope_text_);
    // LP-4d1: an item of a zone's body says so, for its automation name.
    if (container_ != nullptr) item->setData(GraphCanvasZoneScopeRole, scope_text_);
}

void Builder::addNode(const Node &entry, int column, const QPointF &position,
                       Moved moved) {
    auto *item = addNodeItem(
        entry, column,
        roundedEntityPath(entry.width, entry.height, GraphCanvasNodeCornerRadius),
        position, std::move(moved));
    addNodeHeader(item, entry);
    addNodeRows(item, entry);
}

MovableNodeItem *Builder::addNodeItem(const Node &entry, int column,
                                      const QPainterPath &path,
                                      const QPointF &position, Moved moved) {
    const GraphCanvasNode &node = *entry.node;
    const QString node_name = qtext(node.id);
    const QString heading = qtext(node.heading());
    const bool folded = entry.folded;
    auto *item = new MovableNodeItem(path, std::move(moved));
    place(item);
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
    tag(item);
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
    return item;
}

void Builder::addNodeHeader(QGraphicsPathItem *item, const Node &entry) {
    const GraphCanvasNode &node = *entry.node;
    const QString node_name = qtext(node.id);
    const QString heading = qtext(node.heading());
    // LP-4c: a loop shows its mark and what it became ("expand x3") where
    // another node shows its operation.
    const QString type_name = qtext(node.badge.empty() ? node.op : node.badge);
    const qreal width = entry.width;
    // LP-4d1: a folded zone's triangle points the way a folded node's does.
    const bool folded = entry.folded || entry.zone_folded;
    const auto tag = [&](QGraphicsItem *child, const char *kind) {
        child->setData(NodeGraphItemKindRole, QString::fromLatin1(kind));
        this->tag(child);
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
    collapse->setToolTip(entry.zone_folded ? QObject::tr("Open the loop's zone")
                         : folded          ? QObject::tr("Show the sockets")
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
        this->tag(child);
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
    place(line);
    if (!bends.isEmpty()) setNodeGraphBends(*line, bends);
    line->setZValue(0.0);
    line->setPen(QPen{theme_.canvas.edge, 1.6});
    line->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasEdgeItem));
    tag(line);
    line->setData(NodeGraphFromNameRole, qtext(edge.from_node));
    line->setData(NodeGraphToNameRole, qtext(edge.to_node));
    line->setData(GraphCanvasFromPortRole, qtext(edge.from_port));
    line->setData(GraphCanvasToPortRole, qtext(edge.to_port));

    auto *arrow = new QGraphicsPolygonItem;
    place(arrow);
    arrow->setZValue(1.0);
    arrow->setBrush(theme_.canvas.edge);
    arrow->setPen(Qt::NoPen);
    arrow->setData(NodeGraphItemKindRole,
                   QString::fromLatin1(GraphCanvasEdgeArrowItem));
    tag(arrow);
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
    place(dot);
    dot->setPos(position);
    dot->setZValue(2.6);
    dot->setBrush(theme_.canvas.pill_bg);
    dot->setPen(QPen{theme_.canvas.edge_selected, 1.4});
    dot->setFlag(QGraphicsItem::ItemIsMovable, true);
    dot->setFlag(QGraphicsItem::ItemIsSelectable, true);
    dot->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
    dot->setData(NodeGraphItemKindRole,
                 QString::fromLatin1(GraphCanvasRerouteItem));
    tag(dot);
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
    place(bar);
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
    tag(bar);
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

void layoutZone(Zone &zone, const QFontMetricsF &heading_metrics,
                const QFontMetricsF &row_metrics) {
    const qreal header =
        GraphCanvasHeaderHeight + (zone.tabs.empty() ? 0.0 : GraphCanvasZoneTabHeight);
    const auto rows = static_cast<qreal>(std::max(zone.inputs.size(), zone.outputs.size()));
    const qreal rows_height =
        2.0 * GraphCanvasSectionPadding + rows * GraphCanvasSocketRowHeight;
    const QRectF content = zone.content.isValid() ? zone.content : QRectF{};
    zone.origin = QPointF{GraphCanvasZoneContentLeft, header + GraphCanvasZonePadding};
    // The body can reach left of or above its origin (a node dragged there):
    // the frame grows that way rather than moving what is inside it.
    const qreal left = std::min<qreal>(0.0, content.left());
    const qreal top = std::min<qreal>(0.0, content.top());
    const GraphCanvasNode &node = *zone.node.node;
    const qreal heading = 18.0 + heading_metrics.horizontalAdvance(qtext(node.heading())) + 24.0 +
                          row_metrics.horizontalAdvance(zone.stage) + 18.0 +
                          (zone.shared.isEmpty()
                               ? 0.0
                               : row_metrics.horizontalAdvance(zone.shared) + 18.0);
    qreal tabs = 12.0;
    for (const ZoneTab &tab : zone.tabs)
        tabs += row_metrics.horizontalAdvance(tab.text) + 22.0;
    const qreal right = std::max({zone.origin.x() + content.right() + GraphCanvasZoneContentLeft,
                                  left + GraphCanvasZoneMinimumWidth, left + heading,
                                  left + tabs});
    const qreal bottom =
        std::max({zone.origin.y() + content.bottom() + GraphCanvasZonePadding,
                  top + header + rows_height,
                  top + header + 2.0 * GraphCanvasZonePadding +
                      GraphCanvasZoneMinimumContentHeight});
    zone.frame = QRectF{QPointF{left, top}, QPointF{right, bottom}};
    zone.node.width = zone.frame.width();
    zone.node.height = zone.frame.height();
}

QGraphicsItem *Builder::addZone(const Zone &zone, int column, const QPointF &position,
                                Moved moved) {
    const GraphCanvasNode &node = *zone.node.node;
    const QString loop = qtext(node.id);
    const QRectF frame = zone.frame;
    QPainterPath outline = roundedEntityPath(frame.width(), frame.height(),
                                             GraphCanvasNodeCornerRadius);
    outline.translate(frame.topLeft());
    auto *item = addNodeItem(zone.node, column, outline, position, std::move(moved));
    // Under the root's nodes: a frame is large, and a node dragged over it
    // is still the thing in the hand.
    item->setZValue(1.5);
    const QColor tint = nodeColor(node);
    QColor fill = tint;
    fill.setAlphaF(0.14F);
    item->setBrush(fill);
    if (!node.refused) item->setPen(QPen{tint, 1.6});
    // A click meant for the loop lands on its heading, not in its body.
    item->setData(NodeGraphHotspotRole,
                  QPointF{frame.center().x(), frame.top() + 6.0});
    const qreal header =
        GraphCanvasHeaderHeight + (zone.tabs.empty() ? 0.0 : GraphCanvasZoneTabHeight);

    const auto tagged = [&](QGraphicsItem *child, const char *kind) {
        child->setData(NodeGraphItemKindRole, QString::fromLatin1(kind));
        tag(child);
        child->setData(NodeGraphNameRole, loop);
    };

    // The inside of the frame, where the body is drawn: what `graph_zone`
    // names, and what a press between the body's nodes lands on.
    auto *area = new QGraphicsRectItem(
        QRectF{frame.left(), frame.top() + header, frame.width(), frame.height() - header}, item);
    area->setPen(Qt::NoPen);
    area->setBrush(Qt::NoBrush);
    tagged(area, GraphCanvasZoneItem);
    area->setData(GraphCanvasDefinitionRole, zone.definition);

    auto *band = new QGraphicsPathItem(
        headerBandPath(frame.width(), GraphCanvasHeaderHeight, GraphCanvasNodeCornerRadius),
        item);
    band->setPos(frame.topLeft());
    band->setBrush(tint);
    band->setPen(Qt::NoPen);
    tagged(band, GraphCanvasNodeHeaderItem);
    band->setData(GraphCanvasHeadingRole, true);

    // The same triangle a node folds with: here it folds the zone into the
    // loop's node, the way LP-4c drew it.
    QPainterPath arrow;
    arrow.moveTo(-4.5, -2.0);
    arrow.lineTo(4.5, -2.0);
    arrow.lineTo(0.0, 3.5);
    arrow.closeSubpath();
    auto *collapse = new QGraphicsPathItem(arrow, band);
    collapse->setPos(9.5, GraphCanvasHeaderHeight / 2.0);
    collapse->setBrush(theme_.canvas.heading_text);
    collapse->setPen(QPen{Qt::NoPen});
    tagged(collapse, GraphCanvasCollapseItem);
    collapse->setData(GraphCanvasHeadingRole, true);
    collapse->setData(GraphCanvasCollapsedRole, false);
    collapse->setToolTip(QObject::tr("Fold the zone into the loop's node"));

    const QString heading = qtext(node.heading());
    auto *title = new QGraphicsSimpleTextItem(heading, band);
    title->setFont(heading_font_);
    title->setBrush(theme_.canvas.heading_text);
    title->setPos(18.0, (GraphCanvasHeaderHeight - heading_metrics_.height()) / 2.0);
    tagged(title, GraphCanvasNodeLabelItem);
    title->setData(GraphCanvasLabelRole, heading);
    title->setData(GraphCanvasHeadingRole, true);
    title->setToolTip(QObject::tr("Double-click to rename"));

    // The stage (expand x3, draw_loop 2 materials, loop), where a node shows
    // its operation.
    auto *stage = new QGraphicsSimpleTextItem(zone.stage, band);
    stage->setFont(detail_font_);
    stage->setBrush(theme_.canvas.heading_text);
    const qreal stage_x = frame.width() - 10.0 - row_metrics_.horizontalAdvance(zone.stage);
    stage->setPos(stage_x, (GraphCanvasHeaderHeight - row_metrics_.height()) / 2.0);
    tagged(stage, GraphCanvasZoneLabelItem);
    stage->setData(GraphCanvasPortRole, QStringLiteral("stage"));
    stage->setData(GraphCanvasOpRole, qtext(node.op));
    stage->setData(GraphCanvasHeadingRole, true);
    if (!zone.shared.isEmpty()) {
        auto *shared = new QGraphicsSimpleTextItem(zone.shared, band);
        shared->setFont(detail_font_);
        shared->setBrush(QColor{QStringLiteral("#f5b041")});
        shared->setPos(stage_x - 14.0 - row_metrics_.horizontalAdvance(zone.shared),
                       (GraphCanvasHeaderHeight - row_metrics_.height()) / 2.0);
        tagged(shared, GraphCanvasZoneLabelItem);
        shared->setData(GraphCanvasPortRole, QStringLiteral("shared"));
        shared->setData(GraphCanvasHeadingRole, true);
        shared->setToolTip(QObject::tr(
            "Other instances or loops run %1 too: an edit here changes them all.")
                               .arg(zone.definition));
    }

    // The branches' tabs, under the heading: if / elif / else.
    qreal tab_x = frame.left() + 12.0;
    for (const ZoneTab &tab : zone.tabs) {
        const qreal width = row_metrics_.horizontalAdvance(tab.text) + 18.0;
        const qreal height = GraphCanvasZoneTabHeight - 5.0;
        auto *chip = new QGraphicsPathItem(roundedEntityPath(width, height, 5.0), item);
        chip->setPos(tab_x, frame.top() + GraphCanvasHeaderHeight + 2.0);
        chip->setZValue(2.0);
        chip->setBrush(tab.current ? tint : theme_.canvas.pill_bg);
        chip->setPen(QPen{tab.current ? theme_.canvas.heading_text : theme_.canvas.node_border,
                          tab.current ? 1.2 : 0.8});
        if (!tab.runs) chip->setOpacity(0.5);
        tagged(chip, GraphCanvasZoneTabItem);
        chip->setData(GraphCanvasPortRole, QString::number(static_cast<qulonglong>(tab.branch)));
        chip->setData(GraphCanvasBranchRole, static_cast<qulonglong>(tab.branch));
        chip->setData(GraphCanvasLabelRole, tab.text);
        chip->setToolTip(tab.runs ? QObject::tr("Show this branch's body")
                                  : QObject::tr("This branch runs nothing"));
        auto *text = new QGraphicsSimpleTextItem(tab.text, chip);
        text->setFont(detail_font_);
        text->setBrush(theme_.canvas.heading_text);
        text->setPos(9.0, (height - row_metrics_.height()) / 2.0);
        tagged(text, GraphCanvasZoneTabItem);
        text->setData(GraphCanvasPortRole, QString::number(static_cast<qulonglong>(tab.branch)));
        text->setData(GraphCanvasBranchRole, static_cast<qulonglong>(tab.branch));
        tab_x += width + 4.0;
    }

    // The edges: the loop's own sockets outside, the body's inside, one row
    // each, beside the name they share.
    const auto socket = [&](const Row &drawn, const QPointF &centre, const QString &port,
                            const char *direction) {
        auto *dot = new QGraphicsPathItem(socketPath(drawn.style.shape, drawn.style.size), item);
        dot->setPos(centre);
        // Over the body: an edge inside the zone starts at a socket here, and
        // a press on the socket must not pick the edge instead.
        dot->setZValue(2.0);
        dot->setBrush(drawn.style.fill);
        dot->setPen(QPen{drawn.style.outline, 1.1});
        dot->setToolTip(drawn.type.isEmpty()
                            ? QObject::tr("%1 (type not declared)").arg(drawn.label)
                            : QStringLiteral("%1 : %2").arg(drawn.label, drawn.type));
        tagged(dot, GraphCanvasSocketItem);
        dot->setData(GraphCanvasPortRole, port);
        dot->setData(GraphCanvasPortTypeRole, drawn.type);
        dot->setData(GraphCanvasSocketDirectionRole, QString::fromLatin1(direction));
        dot->setData(GraphCanvasSocketShapeRole, static_cast<int>(drawn.style.shape));
        return dot;
    };
    const auto label = [&](const QString &text, const QPointF &at, const QString &port) {
        auto *name = new QGraphicsSimpleTextItem(text, item);
        name->setFont(detail_font_);
        name->setBrush(theme_.canvas.dim_text);
        name->setPos(at.x(), at.y() - row_metrics_.height() / 2.0);
        tagged(name, GraphCanvasNodeLabelItem);
        name->setData(GraphCanvasPortRole, port);
        return name;
    };
    const qreal rows_top = frame.top() + header + GraphCanvasSectionPadding;
    for (std::size_t index = 0; index < zone.inputs.size(); ++index) {
        const ZoneRow &row = zone.inputs[index];
        const qreal y = rows_top + (static_cast<qreal>(index) + 0.5) * GraphCanvasSocketRowHeight;
        if (!row.outer.isEmpty())
            socket(row.outer_row, QPointF{frame.left(), y}, row.outer, "input");
        label(row.label, QPointF{frame.left() + 12.0, y}, row.outer.isEmpty() ? row.inner : row.outer);
        if (!row.inner.isEmpty()) {
            // The body's boundary input: its graph.input's `value`, which is
            // what the body's edges from it name.
            auto *inner = new QGraphicsPathItem(socketPath(SocketShape::circle, GraphCanvasSocketSize),
                                                item);
            inner->setPos(frame.left() + GraphCanvasZoneColumn, y);
            inner->setZValue(2.0);
            inner->setBrush(theme_.canvas.pill_bg);
            inner->setPen(QPen{tint, 1.4});
            inner->setToolTip(QObject::tr("%1 inside %2 (drag to a body input)")
                                  .arg(row.inner, zone.definition));
            inner->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasSocketItem));
            inner->setData(GraphCanvasScopeRole, zone.definition);
            inner->setData(GraphCanvasZoneScopeRole, zone.definition);
            inner->setData(NodeGraphNameRole, row.inner);
            inner->setData(GraphCanvasPortRole, QStringLiteral("value"));
            inner->setData(GraphCanvasSocketDirectionRole, QStringLiteral("output"));
            inner->setData(GraphCanvasSocketShapeRole, static_cast<int>(SocketShape::circle));
        }
    }
    for (std::size_t index = 0; index < zone.outputs.size(); ++index) {
        const ZoneRow &row = zone.outputs[index];
        const qreal y = rows_top + (static_cast<qreal>(index) + 0.5) * GraphCanvasSocketRowHeight;
        if (!row.outer.isEmpty())
            socket(row.outer_row, QPointF{frame.right(), y}, row.outer, "output");
        label(row.label, QPointF{frame.right() - GraphCanvasZoneColumn + 12.0, y},
              row.outer.isEmpty() ? row.inner : row.outer);
        if (!row.inner.isEmpty()) {
            auto *inner = new QGraphicsPathItem(socketPath(SocketShape::circle, GraphCanvasSocketSize),
                                                item);
            inner->setPos(frame.right() - GraphCanvasZoneColumn, y);
            inner->setZValue(2.0);
            inner->setBrush(theme_.canvas.pill_bg);
            inner->setPen(QPen{tint, 1.4});
            inner->setToolTip(QObject::tr("The body's output %1").arg(row.inner));
            tagged(inner, GraphCanvasZoneSocketItem);
            inner->setData(GraphCanvasPortRole, QStringLiteral("out:") + row.inner);
            inner->setData(GraphCanvasSocketDirectionRole, QStringLiteral("input"));
        }
    }

    // The body is drawn in here, in its own coordinates.
    auto *content = new QGraphicsRectItem(QRectF{}, item);
    content->setFlag(QGraphicsItem::ItemHasNoContents, true);
    content->setPos(zone.origin);
    content->setZValue(1.0);
    return content;
}

QGraphicsPathItem *Builder::addZoneLink(const QString &loop, const QString &output,
                                        const QString &from_node, const QString &from_port) {
    auto *line = new QGraphicsPathItem;
    place(line);
    line->setZValue(0.0);
    QPen pen{theme_.canvas.edge, 1.4};
    pen.setStyle(Qt::DashLine);
    line->setPen(pen);
    line->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasZoneLinkItem));
    tag(line);
    line->setData(NodeGraphNameRole, loop);
    line->setData(GraphCanvasPortRole, QStringLiteral("out:") + output);
    line->setData(NodeGraphFromNameRole, from_node);
    line->setData(GraphCanvasFromPortRole, from_port);
    line->setToolTip(QObject::tr("%1.%2 is the body's output %3").arg(from_node, from_port, output));
    return line;
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
