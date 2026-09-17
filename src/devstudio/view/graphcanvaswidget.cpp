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

} // namespace

const NodeGraphKinds &graphCanvasKinds() {
    static const NodeGraphKinds kinds{
        QStringList{QString::fromLatin1(GraphCanvasNodeItem)},
        QString::fromLatin1(GraphCanvasEdgeItem),
        QString::fromLatin1(GraphCanvasEdgeArrowItem),
        QString::fromLatin1(GraphCanvasEdgeLabelItem)};
    return kinds;
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

    const qreal top = model_->breadcrumb().size() > 1
                          ? NodeGraphBreadcrumbHeight + 18.0
                          : 0.0;
    std::map<int, int> rows_in_column;
    std::map<std::string, QGraphicsPathItem *, std::less<>> items;
    for (const auto &node : nodes) {
        const auto column_entry = columns.find(node.id);
        const int column = column_entry == columns.end() ? 0 : column_entry->second;
        const int row = rows_in_column[column]++;
        const QPointF laid_out{
            column * (NodeGraphNodeWidth + NodeGraphHorizontalGap),
            top + row * (NodeGraphNodeHeight + NodeGraphVerticalGap) +
                (column % 2 == 1 ? NodeGraphStaggerOffset * 0.25 : 0.0)};
        const auto position_key = std::pair{scope, node.id};
        QPointF position = laid_out;
        if (const auto retained = session_node_positions_.find(position_key);
            retained != session_node_positions_.end()) {
            position = retained->second;
        }

        auto *item = new MovableNodeItem(
            nodePath(false), [this, position_key, scope](const QPointF &moved) {
                if (rebuilding_) return;
                session_node_positions_[position_key] = moved;
                rerouteConnectedBundles(*this, graphCanvasKinds(), scope,
                                        position_key.second);
                // Growing the scene rect from inside itemChange re-enters this
                // handler through Qt's view update. Defer it to the event loop.
                scheduleSceneRectUpdate();
            });
        addItem(item);
        item->setPos(position);
        item->setZValue(2.0);
        item->setBrush(nodeColor(node));
        item->setPen(QPen{QColor{QStringLiteral("#0f1418")}, 1.2});
        item->setFlag(QGraphicsItem::ItemIsMovable, true);
        item->setFlag(QGraphicsItem::ItemIsSelectable, true);
        item->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
        item->setData(NodeGraphItemKindRole, QString::fromLatin1(GraphCanvasNodeItem));
        item->setData(GraphCanvasScopeRole, scope_text);
        item->setData(NodeGraphNameRole, qtext(node.id));
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
        for (const auto &output : node.outputs) output_ports.push_back(qtext(output));
        item->setData(GraphCanvasInputPortsRole, input_ports);
        item->setData(GraphCanvasDeclaredPortsRole, declared_ports);
        item->setData(GraphCanvasOutputPortsRole, output_ports);
        items.emplace(node.id, item);

        auto *title = new QGraphicsSimpleTextItem(qtext(node.id), item);
        title->setFont(label_font);
        title->setBrush(QColor{QStringLiteral("#f2f6fa")});
        title->setPos(12.0, 9.0);
        title->setData(NodeGraphItemKindRole,
                       QString::fromLatin1(GraphCanvasNodeLabelItem));
        title->setData(GraphCanvasScopeRole, scope_text);
        title->setData(NodeGraphNameRole, qtext(node.id));

        QString subtitle = qtext(node.op);
        if (!node.definition.empty())
            subtitle += QStringLiteral(" · %1").arg(qtext(node.definition));
        if (!node.domain.empty())
            subtitle += QStringLiteral(" · %1").arg(qtext(node.domain));
        if (node.sink) subtitle += QStringLiteral(" · sink");
        auto *detail = new QGraphicsSimpleTextItem(subtitle, item);
        detail->setFont(detail_font);
        detail->setBrush(QColor{QStringLiteral("#c8d4de")});
        detail->setPos(12.0, 30.0);
        detail->setData(NodeGraphItemKindRole,
                        QString::fromLatin1(GraphCanvasNodeLabelItem));
        detail->setData(GraphCanvasScopeRole, scope_text);
        detail->setData(NodeGraphNameRole, qtext(node.id));

        auto *ports = new QGraphicsSimpleTextItem(
            QStringLiteral("in %1 · out %2")
                .arg(input_ports.size())
                .arg(output_ports.size()),
            item);
        ports->setFont(detail_font);
        ports->setBrush(QColor{QStringLiteral("#9fb0bd")});
        ports->setPos(12.0, 45.0);
        ports->setData(NodeGraphItemKindRole,
                       QString::fromLatin1(GraphCanvasNodeLabelItem));
        ports->setData(GraphCanvasScopeRole, scope_text);
        ports->setData(NodeGraphNameRole, qtext(node.id));
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

        const QString label_text = QStringLiteral("%1 -> %2")
                                       .arg(qtext(edge.from_port),
                                            qtext(edge.to_port));
        const qreal label_width =
            std::max<qreal>(NodeGraphEdgeLabelMinimumWidth,
                            metrics.horizontalAdvance(label_text) + 18.0);
        auto *label_box = new QGraphicsPathItem{
            roundedEntityPath(label_width, NodeGraphEdgeLabelHeight, 7.0)};
        addItem(label_box);
        label_box->setZValue(1.0);
        label_box->setBrush(QColor{QStringLiteral("#dbe5ee")});
        label_box->setPen(QPen{QColor{QStringLiteral("#9fb0bd")}, 0.8});
        label_box->setData(NodeGraphItemKindRole,
                           QString::fromLatin1(GraphCanvasEdgeLabelItem));
        label_box->setData(GraphCanvasScopeRole, scope_text);
        label_box->setData(NodeGraphFromNameRole, qtext(edge.from_node));
        label_box->setData(NodeGraphToNameRole, qtext(edge.to_node));
        label_box->setData(GraphCanvasFromPortRole, qtext(edge.from_port));
        label_box->setData(GraphCanvasToPortRole, qtext(edge.to_port));
        auto *label = new QGraphicsSimpleTextItem(label_text, label_box);
        label->setFont(detail_font);
        label->setBrush(QColor{QStringLiteral("#263441")});
        const QRectF bounds = label->boundingRect();
        label->setPos((label_width - bounds.width()) / 2.0,
                      (NodeGraphEdgeLabelHeight - bounds.height()) / 2.0);

        routeLogicalBundle(*this, *line, graphCanvasKinds());
    }

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
        if (nodeGraphItemKind(*item) != QLatin1String{GraphCanvasNodeItem} &&
            nodeGraphItemKind(*item) != QLatin1String{GraphCanvasNodeLabelItem})
            continue;
        selected_node_ = item->data(NodeGraphNameRole).toString().toStdString();
        setProperty("pelicanGraphSelectedNode", item->data(NodeGraphNameRole));
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
            const QString kind = nodeGraphItemKind(*item);
            if (kind != QLatin1String{GraphCanvasNodeItem} &&
                kind != QLatin1String{GraphCanvasNodeLabelItem})
                continue;
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
        row(QObject::tr("id"), qtext(found->id));
        row(QObject::tr("op"), qtext(found->op));
        row(QObject::tr("domain"),
            found->contract_known ? qtext(found->domain)
                                  : QObject::tr("(catalog not fetched)"));
        row(QObject::tr("evaluation"),
            found->contract_known ? qtext(found->evaluation)
                                  : QObject::tr("(catalog not fetched)"));
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
        if (!found->inputs.empty()) {
            auto *inputs = row(QObject::tr("inputs"), {});
            for (const auto &port : found->inputs) {
                auto *entry = new QTreeWidgetItem(inputs);
                entry->setText(0, qtext(port.port));
                entry->setText(1, portSummary(port));
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
                entry->setText(0, qtext(port));
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
