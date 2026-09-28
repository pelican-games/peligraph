#include "graphcanvasdrawing.hpp"

#include "graphcanvaswidget.hpp"
#include "studiotheme.hpp"

#include <QBrush>
#include <QCursor>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QPainterPathStroker>
#include <QPen>

#include <algorithm>
#include <optional>
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
    // Q-19: the sockets the pass's shader declares come first, in binding
    // order, the way the Frame Plan draws them; the node's other rows follow.
    for (const auto &socket : node.input_sockets) {
        using Kind = GraphCanvasInputSocket::Kind;
        Row row{qtext(socket.key()), qtext(socket.label), {},
                socketStyleFor(graphCanvasSocketFacts(model, {})), Value{},
                socket.connected(), socket.kind != Kind::undeclared};
        row.socket_key = qtext(socket.key());
        row.bound_port = qtext(socket.port);
        row.band = socket.kind == Kind::variadic || socket.kind == Kind::variadic_drop;
        row.drop = socket.kind == Kind::variadic_drop;
        if (socket.buffer) row.type = QStringLiteral("buffer");
        entry.inputs.push_back(std::move(row));
    }
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
                          const std::vector<GraphCanvasEdge> &edges, qreal top,
                          const std::vector<LayoutNode> &extra_nodes,
                          const std::vector<LayoutEdge> &extra_edges) {
    std::vector<LayoutNode> layout_nodes;
    layout_nodes.reserve(nodes.size() + extra_nodes.size());
    for (const Node &entry : nodes)
        layout_nodes.push_back(
            LayoutNode{entry.node->id, entry.width, entry.height});
    layout_nodes.insert(layout_nodes.end(), extra_nodes.begin(), extra_nodes.end());
    std::vector<LayoutEdge> layout_edges;
    layout_edges.reserve(edges.size() + extra_edges.size());
    for (const auto &edge : edges)
        layout_edges.push_back(LayoutEdge{edge.from_node, edge.to_node});
    layout_edges.insert(layout_edges.end(), extra_edges.begin(), extra_edges.end());
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
    // Q-19: and each socket the shader declares, by its key and label.
    for (const auto &socket : node.input_sockets)
        input_ports.push_back(QStringLiteral("%1: %2").arg(qtext(socket.key()), qtext(socket.label)));
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
        // Q-19: a socket of the shader's declarations -- its key, and the
        // numbered port its edge names.
        if (!drawn.socket_key.isEmpty()) {
            dot->setData(GraphCanvasSocketKeyRole, drawn.socket_key);
            dot->setData(GraphCanvasBoundPortRole, drawn.bound_port);
            dot->setToolTip(drawn.drop ? QObject::tr("Drop a line here: the next input of the "
                                                     "variadic group")
                                       : drawn.label);
            // The empty end of the band, and an empty declaration, are open.
            if (!drawn.connected) dot->setBrush(theme_.canvas.node_body);
        }
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
    // Q-19: the variadic group's rows, drawn over one long band on the edge.
    std::optional<qreal> band_top;
    qreal band_bottom = 0.0;
    for (const auto &drawn : inputs) {
        const qreal centre_y = cursor_y + GraphCanvasSocketRowHeight / 2.0;
        if (drawn.band) {
            if (!band_top) band_top = cursor_y + 2.0;
            band_bottom = cursor_y + GraphCanvasSocketRowHeight - 2.0;
        }
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
            QString text = pillText(drawn.value);
            // LP-4d2, decision 7: a render node's parameters (a shader, its
            // ports, an image's type) read as long JSON; the pill keeps to
            // the node and the whole of it is the tooltip. Inside a zone at
            // first, and on the root too since the root's long pills ran
            // far past the node's left edge.
            QString full;
            const qreal room = width - 10.0 - 14.0 - 14.0 - 8.0 -
                               row_metrics_.horizontalAdvance(drawn.label);
            if (room > 24.0 && row_metrics_.horizontalAdvance(text) > room) {
                full = text;
                text = row_metrics_.elidedText(text, Qt::ElideRight, room);
            }
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
            if (!full.isEmpty()) pill->setToolTip(full);
            auto *value = new QGraphicsSimpleTextItem(text, pill);
            value->setFont(detail_font_);
            value->setBrush(theme_.canvas.heading_text);
            value->setPos(7.0, (pill_height - row_metrics_.height()) / 2.0);
            tag(value, GraphCanvasValuePillItem);
            value->setData(GraphCanvasPortRole, drawn.port);
            if (!full.isEmpty()) value->setToolTip(full);
        }
        cursor_y += GraphCanvasSocketRowHeight;
    }
    if (band_top) {
        const qreal band_width = GraphCanvasSocketSize + 4.0;
        auto *band = new QGraphicsPathItem(
            roundedEntityPath(band_width, band_bottom - *band_top, band_width / 2.0), item);
        band->setPos(-band_width / 2.0, *band_top);
        band->setZValue(0.5);
        QColor fill = theme_.canvas.dim_text;
        fill.setAlphaF(0.25F);
        band->setBrush(fill);
        band->setPen(QPen{theme_.canvas.node_border, 0.8});
        band->setToolTip(QObject::tr("The shader's variadic inputs, in binding order"));
        tag(band, GraphCanvasNodeLabelItem);
        band->setData(GraphCanvasPortRole, QStringLiteral("in:band"));
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

    // LP-4d2, decision 4: a line that reads one element of a `p[]` each
    // time round is dashed, and says which element on a label over it.
    if (!edge.element.empty()) {
        const QString element = qtext(edge.element);
        line->setData(GraphCanvasEdgeElementRole, element);
        QPen pen = line->pen();
        pen.setStyle(Qt::DashLine);
        line->setPen(pen);
        const QString text = QStringLiteral("[%1]").arg(element);
        auto *label = new QGraphicsSimpleTextItem(text);
        place(label);
        label->setFont(detail_font_);
        label->setBrush(theme_.canvas.heading_text);
        label->setZValue(1.2);
        label->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasEdgeLabelItem));
        tag(label);
        label->setData(NodeGraphNameRole, qtext(edge.to_node));
        label->setData(GraphCanvasPortRole, qtext(edge.to_port));
        label->setData(NodeGraphFromNameRole, qtext(edge.from_node));
        label->setData(NodeGraphToNameRole, qtext(edge.to_node));
        label->setData(GraphCanvasEdgeElementRole, element);
        label->setToolTip(QObject::tr("Each element reads element %1 of %2.%3")
                              .arg(element, qtext(edge.from_node), qtext(edge.from_port)));
    }
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

namespace {

// LP-4d2: the Show in Frame Plan button's text, and its padding.
QString framePlanButtonText() { return QObject::tr("Show in Frame Plan"); }

// How many rows each edge draws: its rows, and the empty slot at its end.
std::size_t zoneRowCount(const Zone &zone) {
    const std::size_t slot = zone.edge_slots ? 1 : 0;
    return std::max(zone.inputs.size() + slot, zone.outputs.size() + slot);
}

qreal zoneExpansionHeight(const Zone &zone) {
    return zone.expansion.isEmpty() ? 0.0 : GraphCanvasZoneFooterHeight;
}

} // namespace

void layoutZone(Zone &zone, const QFontMetricsF &heading_metrics,
                const QFontMetricsF &row_metrics) {
    const qreal header =
        GraphCanvasHeaderHeight +
        (zone.tabs.empty() && zone.axis_labels.empty() ? 0.0 : GraphCanvasZoneTabHeight);
    const auto rows = static_cast<qreal>(zoneRowCount(zone));
    const qreal rows_height =
        2.0 * GraphCanvasSectionPadding + rows * GraphCanvasSocketRowHeight;
    zone.origin = QPointF{GraphCanvasZoneContentLeft, header + GraphCanvasZonePadding};
    const GraphCanvasNode &node = *zone.node.node;
    const qreal heading = 18.0 + heading_metrics.horizontalAdvance(qtext(node.heading())) + 24.0 +
                          row_metrics.horizontalAdvance(zone.stage) + 18.0 +
                          (zone.shared.isEmpty()
                               ? 0.0
                               : row_metrics.horizontalAdvance(zone.shared) + 18.0);
    qreal tabs = 12.0;
    for (const ZoneTab &tab : zone.tabs)
        tabs += row_metrics.horizontalAdvance(tab.text) + 22.0;
    for (const QString &label : zone.axis_labels)
        tabs += row_metrics.horizontalAdvance(label) + 10.0;
    // LP-4d2: the bottom line, and the carry arrows' lanes above it.
    qreal footer_width = 0.0;
    if (!zone.expansion.isEmpty())
        footer_width = 24.0 + row_metrics.horizontalAdvance(zone.expansion) +
                       (zone.frame_plan_button
                            ? 18.0 + row_metrics.horizontalAdvance(framePlanButtonText()) + 18.0
                            : 0.0);
    zone.metrics.origin = zone.origin;
    zone.metrics.footer = zoneExpansionHeight(zone) +
                          static_cast<qreal>(zone.carries.size()) * GraphCanvasZoneCarryLane;
    zone.metrics.min_width =
        std::max({GraphCanvasZoneMinimumWidth, heading, tabs, footer_width});
    zone.metrics.min_height =
        std::max(header + rows_height,
                 header + 2.0 * GraphCanvasZonePadding + GraphCanvasZoneMinimumContentHeight) +
        zone.metrics.footer;
    zone.frame = zoneFrame(zone.metrics, zone.content);
    zone.node.width = zone.frame.width();
    zone.node.height = zone.frame.height();
}

QRectF zoneFrame(const ZoneFrameMetrics &metrics, const QRectF &extent) {
    const QRectF content = extent.isValid() ? extent : QRectF{};
    // The body can reach left of or above its origin (a node dragged there):
    // the frame grows that way rather than moving what is inside it.
    const qreal left = std::min<qreal>(0.0, content.left());
    const qreal top = std::min<qreal>(0.0, content.top());
    const qreal right = std::max(metrics.origin.x() + content.right() + GraphCanvasZoneContentLeft,
                                 left + metrics.min_width);
    const qreal bottom = std::max(metrics.origin.y() + content.bottom() + GraphCanvasZonePadding +
                                      metrics.footer,
                                  top + metrics.min_height);
    return QRectF{QPointF{left, top}, QPointF{right, bottom}};
}

QPainterPath zoneCarryPath(const QPointF &from, const QPointF &to, qreal lane_y) {
    // Out of the output's socket to the right, down to the lane, back along
    // it under the body, and up into the input's socket from the left.
    const qreal reach = 14.0;
    QPainterPath path{from};
    path.lineTo(from.x() + reach, from.y());
    path.lineTo(from.x() + reach, lane_y);
    path.lineTo(to.x() - reach, lane_y);
    path.lineTo(to.x() - reach, to.y());
    path.lineTo(to.x() - GraphCanvasSocketSize / 2.0, to.y());
    return path;
}

void refitZoneFrame(QGraphicsPathItem &frame, const QRectF &from, const QRectF &to) {
    if (from == to) return;
    QPainterPath outline =
        roundedEntityPath(to.width(), to.height(), GraphCanvasNodeCornerRadius);
    outline.translate(to.topLeft());
    frame.setPath(outline);
    const qreal left = to.left() - from.left();
    const qreal top = to.top() - from.top();
    const qreal right = to.right() - from.right();
    const qreal bottom = to.bottom() - from.bottom();
    for (QGraphicsItem *child : frame.childItems()) {
        const auto anchor = static_cast<GraphCanvasZoneAnchor>(
            child->data(GraphCanvasZoneAnchorRole).toInt());
        const QString kind = child->data(NodeGraphItemKindRole).toString();
        if (kind == QLatin1String{GraphCanvasZoneItem}) {
            if (auto *area = dynamic_cast<QGraphicsRectItem *>(child)) {
                const QRectF was = area->rect();
                area->setRect(QRectF{to.left(), was.top() + top, to.width(),
                                     was.height() + bottom - top});
            }
            continue;
        }
        if (kind == QLatin1String{GraphCanvasNodeHeaderItem} &&
            anchor == GraphCanvasZoneAnchor::left) {
            if (auto *band = dynamic_cast<QGraphicsPathItem *>(child)) {
                band->setPath(headerBandPath(to.width(), GraphCanvasHeaderHeight,
                                             GraphCanvasNodeCornerRadius));
                band->setPos(to.topLeft());
                for (QGraphicsItem *mark : band->childItems())
                    if (static_cast<GraphCanvasZoneAnchor>(
                            mark->data(GraphCanvasZoneAnchorRole).toInt()) ==
                        GraphCanvasZoneAnchor::band_right)
                        mark->moveBy(right - left, 0.0);
            }
            continue;
        }
        switch (anchor) {
        case GraphCanvasZoneAnchor::left: child->moveBy(left, top); break;
        case GraphCanvasZoneAnchor::right: child->moveBy(right, top); break;
        case GraphCanvasZoneAnchor::bottom: child->moveBy(left, bottom); break;
        default: break;
        }
    }
    // A click meant for the loop still lands on its heading.
    frame.setData(NodeGraphHotspotRole, QPointF{to.center().x(), to.top() + 6.0});
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
        GraphCanvasHeaderHeight +
        (zone.tabs.empty() && zone.axis_labels.empty() ? 0.0 : GraphCanvasZoneTabHeight);

    const auto tagged = [&](QGraphicsItem *child, const char *kind) {
        child->setData(NodeGraphItemKindRole, QString::fromLatin1(kind));
        tag(child);
        child->setData(NodeGraphNameRole, loop);
    };
    // LP-4d2, decision 7: which edge each thing on the frame keeps to, so the
    // frame can be drawn larger around a body node being dragged.
    const auto anchored = [](QGraphicsItem *child, GraphCanvasZoneAnchor anchor) {
        child->setData(GraphCanvasZoneAnchorRole, static_cast<int>(anchor));
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
    anchored(band, GraphCanvasZoneAnchor::left);

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
    anchored(stage, GraphCanvasZoneAnchor::band_right);
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
        anchored(shared, GraphCanvasZoneAnchor::band_right);
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
        anchored(chip, GraphCanvasZoneAnchor::left);
        auto *text = new QGraphicsSimpleTextItem(tab.text, chip);
        text->setFont(detail_font_);
        text->setBrush(theme_.canvas.heading_text);
        text->setPos(9.0, (height - row_metrics_.height()) / 2.0);
        tagged(text, GraphCanvasZoneTabItem);
        text->setData(GraphCanvasPortRole, QString::number(static_cast<qulonglong>(tab.branch)));
        text->setData(GraphCanvasBranchRole, static_cast<qulonglong>(tab.branch));
        tab_x += width + 4.0;
    }
    // LP-4f1: the views of a loop over a view axis, read only and small.
    for (std::size_t index = 0; index < zone.axis_labels.size(); ++index) {
        const QString &label = zone.axis_labels[index];
        auto *text = new QGraphicsSimpleTextItem(label, item);
        text->setFont(detail_font_);
        text->setBrush(theme_.canvas.heading_text);
        text->setOpacity(0.75);
        text->setPos(tab_x, frame.top() + GraphCanvasHeaderHeight +
                                (GraphCanvasZoneTabHeight - row_metrics_.height()) / 2.0);
        tagged(text, GraphCanvasZoneLabelItem);
        text->setData(GraphCanvasPortRole,
                      QStringLiteral("axis%1").arg(static_cast<qulonglong>(index)));
        text->setToolTip(QObject::tr("Element %1 runs for this view")
                             .arg(static_cast<qulonglong>(index)));
        anchored(text, GraphCanvasZoneAnchor::left);
        tab_x += row_metrics_.horizontalAdvance(label) + 10.0;
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
    // LP-4d2, decisions 1 and 2: the empty slot at the end of an edge. A line
    // dropped on it makes a new loop input (left) or body output (right).
    const auto slot = [&](const QPointF &centre, const QString &port, const QString &tooltip,
                          GraphCanvasZoneAnchor anchor) {
        auto *dot = new QGraphicsPathItem(socketPath(SocketShape::circle, GraphCanvasSocketSize + 3.0),
                                          item);
        dot->setPos(centre);
        dot->setZValue(2.0);
        dot->setBrush(Qt::NoBrush);
        QPen pen{tint, 1.3};
        pen.setStyle(Qt::DashLine);
        dot->setPen(pen);
        dot->setToolTip(tooltip);
        tagged(dot, GraphCanvasZoneSocketItem);
        dot->setData(GraphCanvasPortRole, port);
        dot->setData(GraphCanvasSocketDirectionRole, QStringLiteral("input"));
        anchored(dot, anchor);
        auto *plus = new QGraphicsSimpleTextItem(QStringLiteral("+"), dot);
        plus->setFont(detail_font_);
        plus->setBrush(tint);
        plus->setPos(-row_metrics_.horizontalAdvance(QStringLiteral("+")) / 2.0,
                     -row_metrics_.height() / 2.0);
        tagged(plus, GraphCanvasZoneSocketItem);
        plus->setData(GraphCanvasPortRole, port);
        plus->setData(GraphCanvasSocketDirectionRole, QStringLiteral("input"));
        return dot;
    };
    const qreal rows_top = frame.top() + header + GraphCanvasSectionPadding;
    for (std::size_t index = 0; index < zone.inputs.size(); ++index) {
        const ZoneRow &row = zone.inputs[index];
        const qreal y = rows_top + (static_cast<qreal>(index) + 0.5) * GraphCanvasSocketRowHeight;
        if (!row.outer.isEmpty())
            anchored(socket(row.outer_row, QPointF{frame.left(), y}, row.outer, "input"),
                     GraphCanvasZoneAnchor::left);
        anchored(label(row.label, QPointF{frame.left() + 12.0, y},
                       row.outer.isEmpty() ? row.inner : row.outer),
                 GraphCanvasZoneAnchor::left);
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
            anchored(inner, GraphCanvasZoneAnchor::left);
        }
    }
    if (zone.edge_slots) {
        const qreal y = rows_top + (static_cast<qreal>(zone.inputs.size()) + 0.5) *
                                       GraphCanvasSocketRowHeight;
        slot(QPointF{frame.left(), y}, QStringLiteral("in:+"),
             QObject::tr("Drop a line from outside here for a new loop input"),
             GraphCanvasZoneAnchor::left);
    }
    for (std::size_t index = 0; index < zone.outputs.size(); ++index) {
        const ZoneRow &row = zone.outputs[index];
        const qreal y = rows_top + (static_cast<qreal>(index) + 0.5) * GraphCanvasSocketRowHeight;
        if (!row.outer.isEmpty())
            anchored(socket(row.outer_row, QPointF{frame.right(), y}, row.outer, "output"),
                     GraphCanvasZoneAnchor::right);
        anchored(label(row.label, QPointF{frame.right() - GraphCanvasZoneColumn + 12.0, y},
                       row.outer.isEmpty() ? row.inner : row.outer),
                 GraphCanvasZoneAnchor::right);
        if (!row.inner.isEmpty()) {
            auto *inner = new QGraphicsPathItem(socketPath(SocketShape::circle, GraphCanvasSocketSize),
                                                item);
            inner->setPos(frame.right() - GraphCanvasZoneColumn, y);
            inner->setZValue(2.0);
            inner->setBrush(theme_.canvas.pill_bg);
            inner->setPen(QPen{tint, 1.4});
            // LP-4d2, decision 3: dragged back to a boundary input, a carry.
            inner->setToolTip(
                QObject::tr("The body's output %1 (drag to a boundary input to carry it)")
                    .arg(row.inner));
            tagged(inner, GraphCanvasZoneSocketItem);
            inner->setData(GraphCanvasPortRole, QStringLiteral("out:") + row.inner);
            inner->setData(GraphCanvasSocketDirectionRole, QStringLiteral("input"));
            anchored(inner, GraphCanvasZoneAnchor::right);
        }
    }
    if (zone.edge_slots) {
        const qreal y = rows_top + (static_cast<qreal>(zone.outputs.size()) + 0.5) *
                                       GraphCanvasSocketRowHeight;
        slot(QPointF{frame.right() - GraphCanvasZoneColumn, y}, QStringLiteral("out:+"),
             QObject::tr("Drop a body node's output here for a new loop output"),
             GraphCanvasZoneAnchor::right);
    }

    // Decision 3: the carry, each pair an arrow round the bottom of the frame
    // from the output back to the input. Routed by the scene, which knows
    // where both sockets are once the frame is drawn.
    for (std::size_t index = 0; index < zone.carries.size(); ++index) {
        const ZoneCarry &carry = zone.carries[index];
        auto *arrow_item = new GraphEdgeItem;
        arrow_item->setParentItem(item);
        arrow_item->setZValue(2.2);
        const QColor ink = carry.unbound ? QColor{QStringLiteral("#e67e22")} : tint;
        arrow_item->setPen(QPen{ink, carry.selected ? 3.0 : 1.8});
        arrow_item->setBrush(Qt::NoBrush);
        tagged(arrow_item, GraphCanvasZoneCarryItem);
        arrow_item->setData(GraphCanvasPortRole, carry.from + QStringLiteral("/") + carry.to);
        arrow_item->setData(GraphCanvasFromPortRole, carry.from);
        arrow_item->setData(GraphCanvasToPortRole, carry.to);
        arrow_item->setData(GraphCanvasControlIndexRole, static_cast<qulonglong>(index));
        arrow_item->setData(GraphCanvasZoneFooterRole, zoneExpansionHeight(zone));
        arrow_item->setData(GraphCanvasRefusalRole, carry.warning);
        arrow_item->setToolTip(
            carry.warning.isEmpty()
                ? QObject::tr("%1 carries into %2 (select and press Delete to remove)")
                      .arg(carry.from, carry.to)
                : carry.warning);
        if (carry.unbound) {
            auto *mark = new QGraphicsSimpleTextItem(QStringLiteral("!"), arrow_item);
            mark->setFont(heading_font_);
            mark->setBrush(ink);
            tagged(mark, GraphCanvasZoneCarryMarkItem);
            mark->setData(GraphCanvasPortRole, carry.from + QStringLiteral("/") + carry.to);
            mark->setData(GraphCanvasFromPortRole, carry.from);
            mark->setData(GraphCanvasToPortRole, carry.to);
            mark->setToolTip(carry.warning);
        }
    }

    // Decision 5: what the loop became, along the bottom.
    if (!zone.expansion.isEmpty()) {
        const qreal y = frame.bottom() - GraphCanvasZoneFooterHeight / 2.0;
        auto *line = new QGraphicsSimpleTextItem(zone.expansion, item);
        line->setFont(detail_font_);
        line->setBrush(theme_.canvas.heading_text);
        line->setPos(frame.left() + 12.0, y - row_metrics_.height() / 2.0);
        line->setZValue(2.0);
        tagged(line, GraphCanvasZoneLabelItem);
        line->setData(GraphCanvasPortRole, QStringLiteral("expansion"));
        anchored(line, GraphCanvasZoneAnchor::bottom);
        if (zone.frame_plan_button) {
            const QString text = framePlanButtonText();
            const qreal width = row_metrics_.horizontalAdvance(text) + 16.0;
            const qreal height = GraphCanvasZoneFooterHeight - 8.0;
            auto *button = new QGraphicsPathItem(roundedEntityPath(width, height, 5.0), item);
            button->setPos(frame.left() + 12.0 + row_metrics_.horizontalAdvance(zone.expansion) + 18.0,
                           y - height / 2.0);
            button->setZValue(2.0);
            button->setBrush(theme_.canvas.pill_bg);
            button->setPen(QPen{tint, 1.0});
            button->setToolTip(QObject::tr("Open the Frame Plan with this loop's copies picked"));
            tagged(button, GraphCanvasZoneButtonItem);
            button->setData(GraphCanvasPortRole, QStringLiteral("frame_plan"));
            anchored(button, GraphCanvasZoneAnchor::bottom);
            auto *caption = new QGraphicsSimpleTextItem(text, button);
            caption->setFont(detail_font_);
            caption->setBrush(theme_.canvas.heading_text);
            caption->setPos(8.0, (height - row_metrics_.height()) / 2.0);
            tagged(caption, GraphCanvasZoneButtonItem);
            caption->setData(GraphCanvasPortRole, QStringLiteral("frame_plan"));
        }
    }

    // The body is drawn in here, in its own coordinates.
    auto *content = new QGraphicsRectItem(QRectF{}, item);
    content->setFlag(QGraphicsItem::ItemHasNoContents, true);
    content->setPos(zone.origin);
    content->setZValue(1.0);
    return content;
}

void routeZoneCarry(QGraphicsPathItem &carry) {
    QGraphicsItem *frame = carry.parentItem();
    if (frame == nullptr) return;
    const QString from = QStringLiteral("out:") + carry.data(GraphCanvasFromPortRole).toString();
    const QString to = carry.data(GraphCanvasToPortRole).toString();
    const QGraphicsItem *out = nullptr;
    const QGraphicsItem *in = nullptr;
    for (QGraphicsItem *child : frame->childItems()) {
        const QString kind = child->data(NodeGraphItemKindRole).toString();
        if (kind == QLatin1String{GraphCanvasZoneSocketItem} &&
            child->data(GraphCanvasPortRole).toString() == from)
            out = child;
        else if (kind == QLatin1String{GraphCanvasSocketItem} &&
                 !child->data(GraphCanvasZoneScopeRole).toString().isEmpty() &&
                 child->data(NodeGraphNameRole).toString() == to)
            in = child;
    }
    if (out == nullptr || in == nullptr) {
        carry.setPath(QPainterPath{});
        return;
    }
    const auto *outline = dynamic_cast<const QGraphicsPathItem *>(frame);
    const qreal bottom = outline != nullptr ? outline->path().boundingRect().bottom()
                                            : frame->boundingRect().bottom();
    const auto lane = static_cast<qreal>(carry.data(GraphCanvasControlIndexRole).toULongLong());
    const qreal lane_y = bottom - carry.data(GraphCanvasZoneFooterRole).toReal() -
                         (lane + 0.5) * GraphCanvasZoneCarryLane;
    QPainterPath path = zoneCarryPath(out->pos(), in->pos(), lane_y);
    // The head, pointing into the input.
    const QPointF tip = in->pos() - QPointF{GraphCanvasSocketSize / 2.0, 0.0};
    path.moveTo(tip + QPointF{-7.0, -4.0});
    path.lineTo(tip);
    path.lineTo(tip + QPointF{-7.0, 4.0});
    carry.setPath(path);
    for (QGraphicsItem *mark : carry.childItems())
        mark->setPos((out->pos().x() + in->pos().x()) / 2.0, lane_y - 18.0);
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
    // GL-1: named (`graph_breadcrumb#trail`), so a scenario can come back
    // out of a folded view the way a person does.
    crumb->setData(NodeGraphNameRole, QStringLiteral("trail"));
    auto *crumb_label = new QGraphicsSimpleTextItem(
        QStringLiteral("%1   (click to leave)").arg(text), crumb);
    crumb_label->setFont(label_font_);
    crumb_label->setBrush(theme_.canvas.heading_text);
    crumb_label->setPos(12.0, 8.0);
    crumb_label->setData(NodeGraphItemKindRole,
                         QString::fromLatin1(GraphCanvasBreadcrumbItem));
}

// --- GL-1.

namespace {

// The band under a folded node's heading: a feature says what it folds.
QString derivedBand(const GraphCanvasFoldedNode &node) {
    if (!node.feature_group) return {};
    return node.passes.size() == 1
               ? QObject::tr("1 pass")
               : QObject::tr("%1 passes").arg(static_cast<qulonglong>(node.passes.size()));
}

// "feature" for "feature:bloom": the Frame Plan's source tint.
QString derivedSourceKind(const std::string &source) {
    const auto colon = source.find(':');
    return qtext(colon == std::string::npos ? source : source.substr(0, colon));
}

} // namespace

QSizeF Builder::derivedNodeSize(const GraphCanvasFoldedNode &node) const {
    qreal width = GraphCanvasNodeMinimumWidth;
    width = std::max(width, 18.0 + heading_metrics_.horizontalAdvance(qtext(node.label)) + 12.0 +
                                row_metrics_.horizontalAdvance(qtext(node.type)) + 14.0);
    for (const auto *list : {&node.reads, &node.writes})
        for (const auto &resource : *list)
            width = std::max(width, row_metrics_.horizontalAdvance(qtext(resource)) + 34.0);
    for (const auto &socket : node.input_sockets)
        width = std::max(width, row_metrics_.horizontalAdvance(qtext(socket.label)) + 34.0);
    width = std::min(width, GraphCanvasNodeMaximumWidth);
    // GL-1e: a pass that takes lines shows its input sockets for its reads.
    const std::size_t inputs = node.takes_lines ? node.input_sockets.size() : node.reads.size();
    const auto rows = static_cast<qreal>(inputs + node.writes.size() +
                                         (derivedBand(node).isEmpty() ? 0 : 1));
    return QSizeF{width, GraphCanvasHeaderHeight + GraphCanvasSectionPadding +
                             rows * GraphCanvasSocketRowHeight + GraphCanvasSectionPadding};
}

QGraphicsItem *Builder::addDerivedNode(const GraphCanvasFoldedNode &node, const QPointF &position,
                                       Moved moved) {
    const QSizeF size = derivedNodeSize(node);
    const qreal width = size.width();
    const QString name = qtext(node.id);
    const QString tip = qtext(node.tooltip()) +
                        (node.enterable() ? QObject::tr("\nDouble-click to look inside.")
                                          : QString{});
    auto *item = new MovableNodeItem(
        roundedEntityPath(width, size.height(), GraphCanvasNodeCornerRadius), std::move(moved));
    place(item);
    item->setPos(position);
    item->setZValue(2.0);
    item->setBrush(theme_.canvas.derived_body);
    QPen frame{theme_.canvas.derived_border, 1.3};
    frame.setStyle(Qt::DashLine);
    item->setPen(frame);
    item->setFlag(QGraphicsItem::ItemIsMovable, true);
    item->setFlag(QGraphicsItem::ItemIsSelectable, true);
    item->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
    item->setToolTip(tip);
    const auto tag = [&](QGraphicsItem *child, const char *kind) {
        child->setData(NodeGraphItemKindRole, QString::fromLatin1(kind));
        this->tag(child);
        child->setData(NodeGraphNameRole, name);
        child->setData(GraphCanvasDerivedRole, true);
    };
    tag(item, GraphCanvasNodeItem);
    item->setData(GraphCanvasLabelRole, qtext(node.label));
    item->setData(GraphCanvasOpRole, qtext(node.type));
    item->setData(GraphCanvasDomainRole, QStringLiteral("render"));
    item->setData(GraphCanvasColumnRole, -1);

    // The heading band carries the plan's source tint, a step down.
    auto *band = new QGraphicsPathItem(
        headerBandPath(width, GraphCanvasHeaderHeight, GraphCanvasNodeCornerRadius), item);
    QColor tint = theme_.sourceTint(derivedSourceKind(node.source));
    tint.setAlphaF(0.55F);
    band->setBrush(tint);
    band->setPen(Qt::NoPen);
    band->setToolTip(tip);
    tag(band, GraphCanvasNodeHeaderItem);
    auto *title = new QGraphicsSimpleTextItem(qtext(node.label), band);
    title->setFont(heading_font_);
    title->setBrush(theme_.canvas.heading_text);
    title->setPos(12.0, (GraphCanvasHeaderHeight - heading_metrics_.height()) / 2.0);
    title->setToolTip(tip);
    tag(title, GraphCanvasNodeLabelItem);
    title->setData(GraphCanvasLabelRole, qtext(node.label));
    auto *kind = new QGraphicsSimpleTextItem(qtext(node.type), band);
    kind->setFont(detail_font_);
    kind->setBrush(theme_.canvas.dim_text);
    kind->setOpacity(0.78);
    kind->setPos(width - 10.0 - row_metrics_.horizontalAdvance(qtext(node.type)),
                 (GraphCanvasHeaderHeight - row_metrics_.height()) / 2.0);
    tag(kind, GraphCanvasNodeLabelItem);
    kind->setData(GraphCanvasOpRole, qtext(node.type));

    qreal cursor_y = GraphCanvasHeaderHeight + GraphCanvasSectionPadding;
    if (const QString text = derivedBand(node); !text.isEmpty()) {
        auto *line = new QGraphicsSimpleTextItem(text, item);
        line->setFont(detail_font_);
        line->setBrush(theme_.canvas.dim_text);
        line->setPos(14.0, cursor_y + (GraphCanvasSocketRowHeight - row_metrics_.height()) / 2.0);
        tag(line, GraphCanvasNodeLabelItem);
        cursor_y += GraphCanvasSocketRowHeight;
    }
    const auto row = [&](const std::string &resource, bool output) {
        const qreal centre_y = cursor_y + GraphCanvasSocketRowHeight / 2.0;
        const QString label = qtext(resource);
        auto *text = new QGraphicsSimpleTextItem(label, item);
        text->setFont(detail_font_);
        text->setBrush(theme_.canvas.dim_text);
        text->setPos(output ? width - 14.0 - row_metrics_.horizontalAdvance(label) : 14.0,
                     centre_y - row_metrics_.height() / 2.0);
        tag(text, GraphCanvasNodeLabelItem);
        text->setData(GraphCanvasPortRole, label);
        auto *dot = new QGraphicsPathItem(socketPath(SocketShape::circle, GraphCanvasSocketSize - 2.0),
                                          item);
        dot->setPos(output ? width : 0.0, centre_y);
        dot->setZValue(1.0);
        dot->setBrush(theme_.canvas.derived_body);
        dot->setPen(QPen{theme_.canvas.derived_border, 1.1});
        dot->setToolTip(output ? QObject::tr("%1: written here (read only)").arg(label)
                               : QObject::tr("%1: read here (read only)").arg(label));
        tag(dot, GraphCanvasDerivedSocketItem);
        dot->setData(GraphCanvasPortRole, label);
        dot->setData(GraphCanvasSocketDirectionRole,
                     output ? QStringLiteral("output") : QStringLiteral("input"));
        cursor_y += GraphCanvasSocketRowHeight;
    };
    for (const auto &resource : node.writes) row(resource, true);
    if (!node.takes_lines) {
        for (const auto &resource : node.reads) row(resource, false);
        return item;
    }
    // GL-1e, decision 1: the pass's `input`, one socket each, and the empty
    // `+ input` when its shader takes more. These take a line dropped from a
    // graph node's output; they are the document's, not the plan's.
    for (const auto &socket : node.input_sockets) {
        const qreal centre_y = cursor_y + GraphCanvasSocketRowHeight / 2.0;
        const QString label = qtext(socket.label);
        const bool slot = !socket.input_index.has_value();
        auto *text = new QGraphicsSimpleTextItem(label, item);
        text->setFont(detail_font_);
        text->setBrush(slot ? theme_.canvas.dim_text : theme_.canvas.heading_text);
        text->setPos(14.0, centre_y - row_metrics_.height() / 2.0);
        tag(text, GraphCanvasNodeLabelItem);
        text->setData(GraphCanvasPortRole, qtext(socket.port));
        auto *dot = new QGraphicsPathItem(socketPath(SocketShape::circle, GraphCanvasSocketSize),
                                          item);
        dot->setPos(0.0, centre_y);
        dot->setZValue(1.0);
        dot->setBrush(slot ? theme_.canvas.derived_body : theme_.canvas.derived_border);
        dot->setPen(QPen{theme_.canvas.heading_text, 1.1});
        dot->setToolTip(slot ? QObject::tr("Drop an image here: %1 reads one more input")
                                   .arg(name)
                             : QObject::tr("%1: input %2 of %3; drop another image here to "
                                           "read that instead")
                                   .arg(qtext(socket.resource))
                                   .arg(static_cast<qulonglong>(*socket.input_index))
                                   .arg(name));
        tag(dot, GraphCanvasDerivedSocketItem);
        dot->setData(GraphCanvasPortRole, qtext(socket.port));
        dot->setData(GraphCanvasSocketDirectionRole, QStringLiteral("input"));
        dot->setData(GraphCanvasSocketKeyRole, qtext(socket.key));
        dot->setData(GraphCanvasPassInputRole,
                     slot ? qlonglong{-1} : static_cast<qlonglong>(*socket.input_index));
        cursor_y += GraphCanvasSocketRowHeight;
    }
    return item;
}

QGraphicsPathItem *Builder::addPassInputEdge(const QString &from, const QString &from_port,
                                             const QString &pass, const QString &resource,
                                             std::size_t input_index) {
    auto *line = new GraphEdgeItem;
    place(line);
    line->setZValue(0.5);
    line->setPen(QPen{theme_.canvas.edge, 1.8});
    line->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasPassInputEdgeItem));
    tag(line);
    line->setData(NodeGraphFromNameRole, from);
    line->setData(NodeGraphToNameRole, pass);
    line->setData(GraphCanvasFromPortRole, from_port);
    line->setData(GraphCanvasToPortRole, resource);
    line->setData(GraphCanvasPassInputRole, static_cast<qlonglong>(input_index));
    line->setToolTip(QObject::tr("%1 reads %2 (input %3). Delete takes it out.")
                         .arg(pass, resource)
                         .arg(static_cast<qulonglong>(input_index)));
    return line;
}

QGraphicsPathItem *Builder::addDerivedEdge(const GraphCanvasDerivedEdge &edge) {
    auto *line = new GraphEdgeItem;
    place(line);
    line->setZValue(0.0);
    QPen pen{theme_.canvas.derived_border, 1.4};
    pen.setStyle(Qt::DashLine);
    line->setPen(pen);
    line->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasDerivedEdgeItem));
    tag(line);
    line->setData(GraphCanvasDerivedRole, true);
    line->setData(NodeGraphFromNameRole, qtext(edge.from));
    line->setData(NodeGraphToNameRole, qtext(edge.to));
    QStringList resources;
    for (const auto &resource : edge.resources) resources.push_back(qtext(resource));
    // The first resource names the rows the line runs between.
    line->setData(GraphCanvasFromPortRole, resources.value(0));
    line->setData(GraphCanvasToPortRole, resources.value(0));
    line->setToolTip(QObject::tr("%1: from the frame plan (read only)")
                         .arg(resources.join(QStringLiteral(", "))));
    return line;
}

void Builder::addDomainStub(const GraphCanvasDomainStub &stub, QGraphicsItem &socket) {
    const QString text = qtext(stub.text());
    const qreal height = GraphCanvasSocketRowHeight - 4.0;
    const qreal width = row_metrics_.horizontalAdvance(text) + 14.0;
    auto *pill = new QGraphicsPathItem(roundedEntityPath(width, height, height / 2.0));
    QGraphicsItem *owner = socket.parentItem();
    if (owner != nullptr) pill->setParentItem(owner);
    else place(pill);
    const QPointF at = socket.pos();
    pill->setZValue(2.5);
    pill->setPos(stub.input ? at.x() - 10.0 - width : at.x() + 10.0, at.y() - height / 2.0);
    pill->setBrush(theme_.canvas.pill_bg);
    QPen pen{theme_.canvas.derived_border, 0.9};
    pen.setStyle(Qt::DashLine);
    pill->setPen(pen);
    pill->setCursor(Qt::PointingHandCursor);
    const QString tip = QObject::tr("%1 %2 node(s) the filter hides: click to show %2")
                            .arg(static_cast<qulonglong>(stub.count))
                            .arg(qtext(stub.domain));
    pill->setToolTip(tip);
    const auto mark = [&](QGraphicsItem *item) {
        item->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasDomainStubItem));
        tag(item);
        item->setData(NodeGraphNameRole, qtext(stub.node));
        item->setData(GraphCanvasPortRole, qtext(stub.port));
        item->setData(GraphCanvasStubDomainRole, qtext(stub.domain));
    };
    mark(pill);
    auto *label = new QGraphicsSimpleTextItem(text, pill);
    label->setFont(detail_font_);
    label->setBrush(theme_.canvas.heading_text);
    label->setPos(7.0, (height - row_metrics_.height()) / 2.0);
    label->setToolTip(tip);
    mark(label);
}

void Builder::addFoldedButton(const QString &name, const QString &text, const QPointF &at) {
    const qreal height = NodeGraphBreadcrumbHeight - 6.0;
    const qreal width = row_metrics_.horizontalAdvance(text) + 28.0;
    auto *button = new QGraphicsPathItem(roundedEntityPath(width, height, 6.0));
    scene_.addItem(button);
    button->setPos(at);
    button->setZValue(3.0);
    button->setBrush(theme_.canvas.pill_bg);
    button->setPen(QPen{theme_.canvas.edge, 1.0});
    button->setCursor(Qt::PointingHandCursor);
    button->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasFoldedButtonItem));
    button->setData(NodeGraphNameRole, name);
    button->setData(GraphCanvasScopeRole, scope_text_);
    button->setToolTip(QObject::tr("Show the feature's shipped decomposed version, read only"));
    auto *label = new QGraphicsSimpleTextItem(text, button);
    label->setFont(label_font_);
    label->setBrush(theme_.canvas.heading_text);
    label->setPos(14.0, (height - row_metrics_.height()) / 2.0 - 1.0);
    label->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasFoldedButtonItem));
    label->setData(NodeGraphNameRole, name);
}

void Builder::addLayerBand(QGraphicsItem &node, const QString &name, const QString &text) {
    auto *line = new QGraphicsSimpleTextItem(text, &node);
    line->setFont(detail_font_);
    line->setBrush(theme_.canvas.dim_text);
    const QRectF frame = node.boundingRect();
    line->setPos(frame.left() + 8.0, frame.bottom() + 3.0);
    line->setZValue(1.0);
    line->setAcceptedMouseButtons(Qt::NoButton);
    line->setToolTip(QObject::tr("%1 below the graph: select the node for its Layers").arg(text));
    line->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasLayerBandItem));
    tag(line);
    line->setData(NodeGraphNameRole, name);
}

} // namespace PelicanStudio::GraphCanvasDrawing
