#include "graphcanvaswidget.hpp"

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
#include <QEvent>
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
           kind == QLatin1String{GraphCanvasValuePillItem};
}

// Enter confirms, Escape abandons, and losing the focus confirms the way a
// spreadsheet cell does. The decision is reported back rather than acted on
// here: the widget being watched is still dispatching the event that produced
// it, so it cannot be deleted from inside this call.
class InlineEditFilter final : public QObject {
  public:
    using Done = std::function<void(bool commit)>;

    explicit InlineEditFilter(Done done) : done_{std::move(done)} {}

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Escape) {
                finish(false);
                return true;
            }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                finish(true);
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            finish(true);
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    void finish(bool commit) {
        if (!done_) return;
        auto done = std::exchange(done_, {});
        done(commit);
    }
    Done done_;
};

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
            // An end has just been placed by hand: the lane the layout kept
            // for this edge is about a drawing that no longer exists.
            edge->setData(NodeGraphBendsRole, QVariant{});
            routeGraphCanvasEdge(scene, *edge);
        }
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

GraphCanvasScene::GraphCanvasScene(QObject *parent) : QGraphicsScene{parent} {
    // Every item holds the colour it was built with, so following a theme is
    // drawing the scope again -- not walking the items and guessing which of
    // their brushes came from the table.
    theme_listener_ = addStudioThemeListener([this] { repopulate(); });
}

void GraphCanvasScene::setModel(GraphCanvasModel *model) {
    model_ = model;
    session_node_positions_.clear();
    session_collapsed_.clear();
    pruned_revision_.reset();
    selected_node_.reset();
    selected_edge_.reset();
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
    repopulate();
}

void GraphCanvasScene::setMenuActions(QAction *auto_layout, QAction *fit_view) {
    auto_layout_action_ = auto_layout;
    fit_view_action_ = fit_view;
}

void GraphCanvasScene::repopulate() {
    const StudioTheme &theme = currentStudioTheme();
    ++rebuild_generation_;
    rebuilding_ = true;
    // An open editor belongs to items that are about to be deleted.
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
                {}, false, false});
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
        for (const auto &row : entry.outputs)
            width = std::max(width,
                             row_metrics.horizontalAdvance(row.label) + 34.0);
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
        auto *line = new QGraphicsPathItem;
        addItem(line);
        // An edge that flies over a column follows the lane the layout kept
        // for it, so it passes between the nodes it flies over rather than
        // through them. An end that has been dragged loses the lane again.
        if (index < layout.bends.size() && !layout.bends[index].empty() &&
            !session_node_positions_.contains({scope, edge.from_node}) &&
            !session_node_positions_.contains({scope, edge.to_node})) {
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

        line->setToolTip(
            QObject::tr("%1.%2 -> %3.%4  (select and press Delete to cut)")
                .arg(qtext(edge.from_node), qtext(edge.from_port),
                     qtext(edge.to_node), qtext(edge.to_port)));

        // No edge label: the socket rows already name both ends, and a box in
        // the middle of every line repeated them.
        routeGraphCanvasEdge(*this, *line);
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
        const bool picked =
            selected_edge_ &&
            item->data(NodeGraphFromNameRole).toString() ==
                qtext(selected_edge_->from_node) &&
            item->data(NodeGraphToNameRole).toString() ==
                qtext(selected_edge_->to_node) &&
            (!edge ||
             (item->data(GraphCanvasFromPortRole).toString() ==
                  qtext(selected_edge_->from_port) &&
              item->data(GraphCanvasToPortRole).toString() ==
                  qtext(selected_edge_->to_port)));
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
    setProperty("pelicanGraphSelectedNode", qtext(id));
    applyEdgeEmphasis();
    if (selectionChanged) selectionChanged();
}

void GraphCanvasScene::selectEdge(const GraphCanvasEdge &edge) {
    selected_edge_ = edge;
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
            if (kind == QLatin1String{GraphCanvasEdgeItem} ||
                kind == QLatin1String{GraphCanvasEdgeArrowItem}) {
                event->accept();
                selectEdge(GraphCanvasEdge{
                    item->data(NodeGraphFromNameRole).toString().toStdString(),
                    item->data(GraphCanvasFromPortRole).toString().toStdString(),
                    item->data(NodeGraphToNameRole).toString().toStdString(),
                    item->data(GraphCanvasToPortRole).toString().toStdString()});
                return;
            }
            if (isNodeSurface(kind)) break;
        }
    }
    const auto generation = rebuild_generation_;
    QGraphicsScene::mousePressEvent(event);
    if (event->button() != Qt::LeftButton) return;
    // The press went to the breadcrumb and the scene is a different one now.
    if (generation != rebuild_generation_) return;
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
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (deleteSelection()) {
            event->accept();
            afterEdit(true);
            return;
        }
    }
    QGraphicsScene::keyPressEvent(event);
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

struct GraphCanvasWidget::Impl {
    GraphCanvasWidget &view;
    GraphCanvasReadCapability graph;
    GraphCanvasModel model;
    GraphCanvasScene *scene = nullptr;
    NodeCanvasView *canvas = nullptr;
    QComboBox *scopes = nullptr;
    QComboBox *value_instances = nullptr;
    QPushButton *refresh = nullptr;
    QLabel *breadcrumb = nullptr;
    QLabel *notice = nullptr;
    QTreeWidget *details = nullptr;
    QLineEdit *label_edit = nullptr;
    QPushButton *apply_label = nullptr;
    QPushButton *add_node = nullptr;
    QAction *auto_layout_action = nullptr;
    QAction *fit_view_action = nullptr;
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
        canvas = new NodeCanvasView(scene, splitter);
        canvas->setObjectName(QStringLiteral("pelican.graphCanvasView"));
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
        scene->setMenuActions(auto_layout_action, fit_view_action);
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
        label_row->addWidget(new QLabel(QObject::tr("Label:"), side));
        label_edit = new QLineEdit(side);
        label_edit->setObjectName(QStringLiteral("pelican.graphCanvasLabel"));
        label_edit->setPlaceholderText(QObject::tr("(no label: the id is the heading)"));
        label_row->addWidget(label_edit, 1);
        apply_label = new QPushButton(QObject::tr("Rename"), side);
        apply_label->setObjectName(QStringLiteral("pelican.graphCanvasRename"));
        label_row->addWidget(apply_label);
        add_node = new QPushButton(QObject::tr("Add node..."), side);
        add_node->setObjectName(QStringLiteral("pelican.graphCanvasAddNode"));
        label_row->addWidget(add_node);
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

        scene->setModel(&model);
        scene->scopeChanged = [this] {
            syncScopeControls();
            syncValueControls();
            showDetails();
        };
        scene->selectionChanged = [this] { showDetails(); };
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
        // The heading first, in the order the canvas shows it: the chosen name,
        // then what the node actually is. The field above the table is where it
        // is edited; this row is what the canvas is currently showing.
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
