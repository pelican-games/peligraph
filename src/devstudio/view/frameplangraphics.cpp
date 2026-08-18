#include "frameplangraphics.hpp"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QLineF>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

constexpr qreal NodeWidth = 240.0;
constexpr qreal NodeHeight = 64.0;
constexpr qreal HorizontalGap = 88.0;
constexpr qreal VerticalGap = 42.0;
constexpr qreal EdgeLabelHeight = 24.0;
constexpr qreal EdgeLabelMinimumWidth = 120.0;
constexpr qreal PhysicalPanelGap = 74.0;
constexpr qreal PhysicalPanelMinimumWidth = 760.0;
constexpr qreal PhysicalHeaderHeight = 76.0;
constexpr qreal PhysicalResourceHeight = 92.0;
constexpr qreal PhysicalOutcomeHeight = 34.0;
constexpr qreal LogicalUnavailableWidth = 640.0;
constexpr qreal LogicalUnavailableHeight = 140.0;

using EntityPair = std::pair<std::string, std::string>;

struct DependencyRecord {
    std::string from;
    std::string to;
    std::string reason;
    std::string resource;
};

struct PhysicalOverlaySummary {
    std::size_t resource_count = 0;
    std::size_t adopted_alias_count = 0;
    std::size_t not_adopted_alias_count = 0;
};

class MovableNodeItem final : public QGraphicsPathItem {
  public:
    using Moved = std::function<void(const QPointF &)>;

    MovableNodeItem(QPainterPath path, Moved moved)
        : QGraphicsPathItem{std::move(path)}, moved_{std::move(moved)} {}

  protected:
    QVariant itemChange(GraphicsItemChange change,
                        const QVariant &value) override {
        QVariant result = QGraphicsPathItem::itemChange(change, value);
        if (change == QGraphicsItem::ItemPositionHasChanged && moved_) {
            moved_(pos());
        }
        return result;
    }

  private:
    Moved moved_;
};

QString qtext(const std::string &value) {
    return QString::fromStdString(value);
}

QString itemKind(const QGraphicsItem &item) {
    return item.data(FramePlanItemKindRole).toString();
}

bool isAnchor(const FramePlanNode &node) {
    return node.kind == "anchor" && node.name.starts_with("__anchor_") &&
           node.reads.empty() && node.history_reads.empty() &&
           node.writes.empty();
}

QColor sourceColor(const std::string &source) {
    if (source == "project") {
        return QColor{QStringLiteral("#3978a8")};
    }
    if (source.starts_with("feature:")) {
        return QColor{QStringLiteral("#c47b25")};
    }
    if (source == "engine") {
        return QColor{QStringLiteral("#66717e")};
    }
    return QColor{QStringLiteral("#806f68")};
}

std::string recordIdentity(const DependencyRecord &dependency) {
    constexpr char separator = '\x1f';
    return dependency.from + separator + dependency.to + separator +
           dependency.reason + separator + dependency.resource;
}

QStringList qlist(const std::vector<std::string> &values) {
    QStringList result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const auto &value : values) {
        result.push_back(qtext(value));
    }
    return result;
}

void annotateIdentity(QGraphicsItem &item, const std::string &kind,
                      const std::string &graph, const std::string &name) {
    item.setData(FramePlanItemKindRole, qtext(kind));
    item.setData(FramePlanGraphRole, qtext(graph));
    item.setData(FramePlanNameRole, qtext(name));
}

void annotateEndpoints(QGraphicsItem &item, const EntityPair &endpoints) {
    item.setData(FramePlanFromNameRole, qtext(endpoints.first));
    item.setData(FramePlanToNameRole, qtext(endpoints.second));
}

bool containsResource(const std::vector<std::string> &resources,
                      const std::set<std::string, std::less<>> &frontier) {
    return std::any_of(resources.begin(), resources.end(),
                       [&](const std::string &resource) {
                           return frontier.contains(resource);
                       });
}

std::set<std::string, std::less<>> subtreeNodeNames(
    const FramePlanModel &model, const std::string &target, int depth) {
    std::set<std::string, std::less<>> visible;
    if (depth <= 0) {
        return visible;
    }

    std::set<std::string, std::less<>> visited_resources{target};
    std::set<std::string, std::less<>> resource_frontier{target};
    for (int step = 0; step < depth && !resource_frontier.empty(); ++step) {
        std::vector<const FramePlanNode *> node_frontier;
        for (const auto &node : model.nodes) {
            if ((containsResource(node.reads, resource_frontier) ||
                 containsResource(node.writes, resource_frontier)) &&
                !visible.contains(node.name)) {
                node_frontier.push_back(&node);
            }
        }

        std::set<std::string, std::less<>> next_resources;
        for (const FramePlanNode *node : node_frontier) {
            visible.insert(node->name);
            next_resources.insert(node->reads.begin(), node->reads.end());
            next_resources.insert(node->writes.begin(), node->writes.end());
        }
        for (const auto &resource : visited_resources) {
            next_resources.erase(resource);
        }
        visited_resources.insert(next_resources.begin(), next_resources.end());
        resource_frontier = std::move(next_resources);
    }
    return visible;
}

QPainterPath nodePath(bool anchor) {
    QPainterPath path;
    if (anchor) {
        const QPointF center{18.0, NodeHeight / 2.0};
        QPolygonF diamond;
        diamond << QPointF{center.x(), center.y() - 16.0}
                << QPointF{center.x() + 16.0, center.y()}
                << QPointF{center.x(), center.y() + 16.0}
                << QPointF{center.x() - 16.0, center.y()};
        path.addPolygon(diamond);
        path.closeSubpath();
    } else {
        path.addRoundedRect(QRectF{0.0, 0.0, NodeWidth, NodeHeight}, 9.0,
                            9.0);
    }
    return path;
}

void addNodeLabel(QGraphicsPathItem &item, const FramePlanNode &node,
                  const std::string &graph) {
    auto *label = new QGraphicsSimpleTextItem(qtext(node.name), &item);
    label->setBrush(QColor{QStringLiteral("#f7f9fb")});
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    const QRectF bounds = label->boundingRect();
    if (isAnchor(node)) {
        label->setPos(43.0, (NodeHeight - bounds.height()) / 2.0);
    } else {
        label->setPos((NodeWidth - bounds.width()) / 2.0,
                      (NodeHeight - bounds.height()) / 2.0);
    }
    annotateIdentity(*label, FramePlanNodeLabelItem, graph, node.name);
}

QString dependencyLabel(const std::vector<DependencyRecord> &records) {
    std::set<std::string, std::less<>> resources;
    std::set<std::string, std::less<>> reasons;
    for (const auto &record : records) {
        if (!record.resource.empty()) {
            resources.insert(record.resource);
        }
        reasons.insert(record.reason);
    }
    QStringList parts;
    for (const auto &resource : resources) {
        parts.push_back(qtext(resource));
    }
    if (parts.isEmpty()) {
        for (const auto &reason : reasons) {
            const auto marker = reason.rfind('.');
            const auto version = reason.rfind('@');
            parts.push_back(qtext(reason.substr(
                marker == std::string::npos ? 0 : marker + 1,
                version == std::string::npos
                    ? std::string::npos
                    : version - (marker == std::string::npos ? 0
                                                              : marker + 1))));
        }
    }
    QString label = parts.join(QStringLiteral(", "));
    if (records.size() > 1) {
        label += QStringLiteral("  x%1").arg(records.size());
    }
    return label;
}

struct CurveGeometry {
    QPainterPath path;
    QPolygonF arrow;
    QPointF label_center;
};

CurveGeometry curveGeometry(const QRectF &from, const QRectF &to) {
    QPointF start;
    QPointF tip;
    QPointF control_one;
    QPointF control_two;

    const qreal horizontal_distance =
        std::abs(from.center().x() - to.center().x());
    if (horizontal_distance < 20.0) {
        const qreal direction = to.center().y() >= from.center().y() ? 1.0
                                                                     : -1.0;
        start = direction > 0.0
                    ? QPointF{from.center().x(), from.bottom()}
                    : QPointF{from.center().x(), from.top()};
        tip = direction > 0.0 ? QPointF{to.center().x(), to.top()}
                              : QPointF{to.center().x(), to.bottom()};
        const qreal control_distance =
            std::max<qreal>(52.0, std::abs(tip.y() - start.y()) * 0.45);
        control_one = start + QPointF{0.0, direction * control_distance};
        control_two = tip - QPointF{0.0, direction * control_distance};
    } else {
        const qreal direction = to.center().x() >= from.center().x() ? 1.0
                                                                     : -1.0;
        start = direction > 0.0 ? QPointF{from.right(), from.center().y()}
                                : QPointF{from.left(), from.center().y()};
        tip = direction > 0.0 ? QPointF{to.left(), to.center().y()}
                              : QPointF{to.right(), to.center().y()};
        const qreal control_distance =
            std::max<qreal>(58.0, std::abs(tip.x() - start.x()) * 0.45);
        control_one = start + QPointF{direction * control_distance, 0.0};
        control_two = tip - QPointF{direction * control_distance, 0.0};
    }

    QPainterPath path{start};
    path.cubicTo(control_one, control_two, tip);

    QLineF tangent{control_two, tip};
    if (tangent.length() < 0.001) {
        tangent = QLineF{start, tip};
    }
    if (tangent.length() < 0.001) {
        tangent = QLineF{QPointF{0.0, 0.0}, QPointF{1.0, 0.0}};
    }
    const QPointF unit{tangent.dx() / tangent.length(),
                       tangent.dy() / tangent.length()};
    const QPointF perpendicular{-unit.y(), unit.x()};
    const QPointF base = tip - unit * 11.0;
    QPolygonF arrow;
    arrow << tip << base + perpendicular * 5.0
          << base - perpendicular * 5.0;

    const QPointF label_center = path.pointAtPercent(0.5);
    return CurveGeometry{std::move(path), std::move(arrow), label_center};
}

QGraphicsItem *findNodeItem(QGraphicsScene &scene, const QString &graph,
                            const QString &name) {
    for (QGraphicsItem *item : scene.items()) {
        if (itemKind(*item) == QLatin1String{FramePlanNodeItem} &&
            item->data(FramePlanGraphRole).toString() == graph &&
            item->data(FramePlanNameRole).toString() == name) {
            return item;
        }
    }
    return nullptr;
}

QGraphicsItem *findEndpointArtifact(QGraphicsScene &scene,
                                    const QString &kind_value,
                                    const QString &graph,
                                    const QString &from,
                                    const QString &to) {
    for (QGraphicsItem *item : scene.items()) {
        if (itemKind(*item) == kind_value &&
            item->data(FramePlanGraphRole).toString() == graph &&
            item->data(FramePlanFromNameRole).toString() == from &&
            item->data(FramePlanToNameRole).toString() == to) {
            return item;
        }
    }
    return nullptr;
}

void routeLogicalBundle(QGraphicsScene &scene, QGraphicsPathItem &edge) {
    const QString graph = edge.data(FramePlanGraphRole).toString();
    const QString from_name = edge.data(FramePlanFromNameRole).toString();
    const QString to_name = edge.data(FramePlanToNameRole).toString();
    QGraphicsItem *from = findNodeItem(scene, graph, from_name);
    QGraphicsItem *to = findNodeItem(scene, graph, to_name);
    if (from == nullptr || to == nullptr) {
        return;
    }

    const CurveGeometry geometry =
        curveGeometry(from->sceneBoundingRect(), to->sceneBoundingRect());
    edge.setPath(geometry.path);
    if (auto *arrow = dynamic_cast<QGraphicsPolygonItem *>(
            findEndpointArtifact(scene,
                                 QLatin1String{FramePlanEdgeArrowItem}, graph,
                                 from_name, to_name))) {
        arrow->setPolygon(geometry.arrow);
    }
    if (QGraphicsItem *label = findEndpointArtifact(
            scene, QLatin1String{FramePlanEdgeLabelItem}, graph, from_name,
            to_name)) {
        const QRectF bounds = label->boundingRect();
        label->setPos(geometry.label_center - bounds.center());
    }
}

void rerouteConnectedBundles(QGraphicsScene &scene, const std::string &graph,
                             const std::string &node) {
    const QString expected_graph = qtext(graph);
    const QString expected_node = qtext(node);
    for (QGraphicsItem *item : scene.items()) {
        if (itemKind(*item) != QLatin1String{FramePlanEdgeItem} ||
            item->data(FramePlanGraphRole).toString() != expected_graph ||
            (item->data(FramePlanFromNameRole).toString() != expected_node &&
             item->data(FramePlanToNameRole).toString() != expected_node)) {
            continue;
        }
        if (auto *edge = dynamic_cast<QGraphicsPathItem *>(item)) {
            routeLogicalBundle(scene, *edge);
        }
    }
}

QString lifetimeText(const FramePlanLifetime &lifetime) {
    if (!lifetime.used) {
        return QStringLiteral("unused");
    }
    return QStringLiteral("[%1, %2]")
        .arg(static_cast<qulonglong>(*lifetime.first_use))
        .arg(static_cast<qulonglong>(*lifetime.last_use));
}

void annotatePhysicalContext(QGraphicsItem &item,
                             const FramePlanModel &model) {
    item.setData(FramePlanProfileRole,
                 qtext(model.physical_plan.planning_profile));
    item.setData(FramePlanEndpointRole,
                 qtext(model.physical_plan.planning_endpoint));
}

void annotatePhysicalResource(QGraphicsItem &item,
                              const FramePlanModel &model,
                              const FramePlanResource &resource) {
    annotateIdentity(item, FramePlanResourceLifetimeItem, model.graph,
                     resource.name);
    annotatePhysicalContext(item, model);
    item.setData(FramePlanReasonRole, qtext(resource.reason));
    item.setData(FramePlanWidestReadRole, qtext(resource.widest_read));
    item.setData(FramePlanAliasableRole, resource.aliasable);
    item.setData(FramePlanRepresentationRole,
                 qtext(resource.representation));
    item.setData(FramePlanLifetimeUsedRole, resource.lifetime.used);
    if (resource.lifetime.first_use) {
        item.setData(FramePlanLifetimeFirstRole,
                     static_cast<qulonglong>(*resource.lifetime.first_use));
    }
    if (resource.lifetime.last_use) {
        item.setData(FramePlanLifetimeLastRole,
                     static_cast<qulonglong>(*resource.lifetime.last_use));
    }
    item.setData(
        FramePlanPhysicalStateRole,
        resource.alias_group.empty()
            ? QStringLiteral("separate")
            : QStringLiteral("reused:%1").arg(qtext(resource.alias_group)));
}

QString physicalResourceToolTip(const FramePlanResource &resource) {
    return QStringLiteral(
               "%1\nrepresentation: %2\nwidest read: %3\naliasable: "
               "%4\nlifetime: %5\nreason: %6")
        .arg(qtext(resource.name), qtext(resource.representation),
             qtext(resource.widest_read),
             resource.aliasable ? QStringLiteral("yes")
                                : QStringLiteral("no"),
             lifetimeText(resource.lifetime), qtext(resource.reason));
}

bool containsMember(const std::vector<std::string> &members,
                    const std::string &target) {
    return std::find(members.begin(), members.end(), target) != members.end();
}

PhysicalOverlaySummary addPhysicalOverlay(
    QGraphicsScene &scene, const FramePlanModel &model, qreal logical_bottom,
    const std::optional<FramePlanNodeKey> &selected_target) {
    PhysicalOverlaySummary summary;

    const FramePlanResource *resource = nullptr;
    if (selected_target) {
        const auto found = std::find_if(
            model.resources.begin(), model.resources.end(),
            [&](const FramePlanResource &candidate) {
                return candidate.name == selected_target->name;
            });
        if (found != model.resources.end() && !found->representation.empty()) {
            resource = &*found;
        }
    }

    std::vector<const FramePlanAliasGroup *> adopted;
    std::vector<const FramePlanOpportunityPair *> not_adopted;
    if (resource != nullptr) {
        for (const auto &group : model.physical_plan.alias_groups) {
            if (containsMember(group.resources, resource->name)) {
                adopted.push_back(&group);
            }
        }
        for (const auto &candidate : model.physical_plan.alias_candidates) {
            if (!candidate.adopted &&
                (candidate.first == resource->name ||
                 candidate.second == resource->name)) {
                not_adopted.push_back(&candidate);
            }
        }
        std::ranges::sort(adopted, {},
                          [](const FramePlanAliasGroup *group) {
                              return group->id;
                          });
        std::ranges::sort(
            not_adopted, {}, [](const FramePlanOpportunityPair *candidate) {
                return std::pair{candidate->first, candidate->second};
            });
    }
    summary.adopted_alias_count = adopted.size();
    summary.not_adopted_alias_count = not_adopted.size();

    const QRectF logical_bounds = scene.itemsBoundingRect();
    const qreal panel_left = logical_bounds.isValid()
                                 ? std::min<qreal>(0.0, logical_bounds.left())
                                 : 0.0;
    const qreal panel_width =
        std::max(PhysicalPanelMinimumWidth,
                 logical_bounds.isValid() ? logical_bounds.right() - panel_left
                                          : 0.0);
    const qreal panel_top = logical_bottom + PhysicalPanelGap;
    const std::size_t outcome_count = adopted.size() + not_adopted.size();
    const qreal panel_height =
        !model.physical_plan.available() || resource == nullptr
            ? 118.0
            : PhysicalHeaderHeight + PhysicalResourceHeight + 18.0 +
                  static_cast<qreal>(outcome_count) * PhysicalOutcomeHeight;

    auto *context = scene.addRect(
        QRectF{panel_left, panel_top, panel_width, panel_height},
        QPen{QColor{QStringLiteral("#6e7b88")}, 1.4},
        QBrush{QColor{QStringLiteral("#182129")}});
    context->setZValue(0.6);
    annotateIdentity(*context, FramePlanPhysicalContextItem, model.graph,
                     "physical_planning_context");
    annotatePhysicalContext(*context, model);

    auto *title = new QGraphicsSimpleTextItem(context);
    QFont title_font = title->font();
    title_font.setBold(true);
    title_font.setPointSizeF(10.5);
    title->setFont(title_font);
    title->setBrush(QColor{QStringLiteral("#f1f5f8")});
    title->setPos(panel_left + 12.0, panel_top + 8.0);

    if (!model.physical_plan.available()) {
        context->setData(FramePlanPhysicalStateRole,
                         QStringLiteral("unavailable"));
        title->setText(QStringLiteral("Physical planning unavailable"));
        auto *reason = new QGraphicsSimpleTextItem(
            qtext(model.physical_plan.unavailable_reason), context);
        reason->setBrush(QColor{QStringLiteral("#efb366")});
        reason->setPos(panel_left + 12.0, panel_top + 43.0);
        context->setToolTip(qtext(model.physical_plan.unavailable_reason));
        return summary;
    }

    if (!selected_target) {
        context->setData(FramePlanPhysicalStateRole,
                         QStringLiteral("no_target"));
        title->setText(QStringLiteral("Select a target to inspect physical planning."));
        return summary;
    }
    if (resource == nullptr) {
        context->setData(FramePlanPhysicalStateRole,
                         QStringLiteral("target_missing"));
        title->setText(
            QStringLiteral("No physical facts were published for %1")
                .arg(qtext(selected_target->name)));
        return summary;
    }

    context->setData(FramePlanPhysicalStateRole,
                     QStringLiteral("available"));
    title->setText(
        QStringLiteral("Target: %1  |  profile: %2  |  endpoint: %3")
            .arg(qtext(resource->name),
                 qtext(model.physical_plan.planning_profile),
                 qtext(model.physical_plan.planning_endpoint)));
    context->setToolTip(
        QStringLiteral("Physical decisions for only the selected target."));

    const qreal resource_top = panel_top + PhysicalHeaderHeight;
    auto *row = scene.addRect(
        QRectF{panel_left + 8.0, resource_top, panel_width - 16.0,
               PhysicalResourceHeight - 4.0},
        QPen{resource->alias_group.empty()
                 ? QColor{QStringLiteral("#71889a")}
                 : QColor{QStringLiteral("#4fbc78")},
             resource->alias_group.empty() ? 1.0 : 1.8},
        QBrush{QColor{QStringLiteral("#232c34")}});
    row->setZValue(0.8);
    annotatePhysicalResource(*row, model, *resource);
    row->setToolTip(physicalResourceToolTip(*resource));
    ++summary.resource_count;

    auto *facts = new QGraphicsSimpleTextItem(
        QStringLiteral(
            "%1 | representation: %2 | widest read: %3 | aliasable: %4 | lifetime: %5")
            .arg(qtext(resource->name), qtext(resource->representation),
                 qtext(resource->widest_read),
                 resource->aliasable ? QStringLiteral("yes")
                                     : QStringLiteral("no"),
                 lifetimeText(resource->lifetime)),
        row);
    QFont facts_font = facts->font();
    facts_font.setBold(true);
    facts->setFont(facts_font);
    facts->setBrush(QColor{QStringLiteral("#edf3f7")});
    facts->setPos(panel_left + 18.0, resource_top + 10.0);

    auto *reason = new QGraphicsSimpleTextItem(
        QStringLiteral("reason: %1").arg(qtext(resource->reason)), row);
    reason->setBrush(QColor{QStringLiteral("#b9c6d1")});
    reason->setPos(panel_left + 18.0, resource_top + 45.0);

    auto *selection = new QGraphicsSimpleTextItem(
        QStringLiteral("Selected target physical facts"), row);
    selection->setVisible(false);
    annotateIdentity(*selection, FramePlanPhysicalSelectionItem, model.graph,
                     resource->name);
    annotatePhysicalContext(*selection, model);

    qreal outcome_top = resource_top + PhysicalResourceHeight + 8.0;
    const auto add_outcome = [&](const std::string &name,
                                 const std::vector<std::string> &members,
                                 const QString &state, const QString &label,
                                 const QColor &color) {
        QPainterPath path;
        path.addRoundedRect(
            QRectF{panel_left + 8.0, outcome_top, panel_width - 16.0,
                   PhysicalOutcomeHeight - 4.0},
            6.0, 6.0);
        auto *item = scene.addPath(path);
        item->setPen(QPen{color, 1.6,
                          state == QStringLiteral("adopted")
                              ? Qt::SolidLine
                              : Qt::DashLine});
        item->setBrush(QColor{QStringLiteral("#1e272f")});
        item->setZValue(0.9);
        annotateIdentity(*item, FramePlanAliasOverlayItem, model.graph, name);
        annotatePhysicalContext(*item, model);
        item->setData(FramePlanMembersRole, qlist(members));
        item->setData(FramePlanOpportunityKindRole,
                      QStringLiteral("alias"));
        item->setData(FramePlanPhysicalStateRole, state);
        item->setToolTip(label);
        auto *text = new QGraphicsSimpleTextItem(label, item);
        text->setBrush(color);
        text->setPos(panel_left + 16.0, outcome_top + 5.0);
        outcome_top += PhysicalOutcomeHeight;
    };

    for (const FramePlanAliasGroup *group : adopted) {
        add_outcome(
            group->id, group->resources, QStringLiteral("adopted"),
            QStringLiteral("Allocation reuse adopted: %1")
                .arg(qlist(group->resources).join(QStringLiteral(" + "))),
            QColor{QStringLiteral("#65d48b")});
    }
    for (const FramePlanOpportunityPair *candidate : not_adopted) {
        const std::vector<std::string> members{candidate->first,
                                                candidate->second};
        add_outcome(
            "not_adopted:" + candidate->first + "+" + candidate->second,
            members, QStringLiteral("not_adopted"),
            QStringLiteral("Legal allocation reuse not adopted: %1 + %2")
                .arg(qtext(candidate->first), qtext(candidate->second)),
            QColor{QStringLiteral("#e3a84c")});
    }
    return summary;
}

} // namespace

FramePlanGraphicsScene::FramePlanGraphicsScene(QObject *parent)
    : QGraphicsScene{parent} {
    setItemIndexMethod(QGraphicsScene::NoIndex);
    connect(this, &QGraphicsScene::selectionChanged, this,
            [this] { recordSelection(); });
    resetGraph();
}

void FramePlanGraphicsScene::resetGraph() {
    rebuilding_ = true;
    clear();
    selected_node_.reset();
    selected_resource_.reset();
    setSceneRect({});
    rebuilding_ = false;
    setProperty("pelicanGraph", QString{});
    setProperty("pelicanTarget", QString{});
    setProperty("pelicanSubtreeDepth", 1);
    setProperty("pelicanTargetCount", 0);
    setProperty("pelicanNodeRecordCount", 0);
    setProperty("pelicanDependencyRecordCount", 0);
    setProperty("pelicanVisibleDependencyRecordCount", 0);
    setProperty("pelicanVisibleItemCount", 0);
    setProperty("pelicanEdgeBundleCount", 0);
    setProperty("pelicanCurveEdgeCount", 0);
    setProperty("pelicanResourceOverlayCount", 0);
    setProperty("pelicanPhysicalResourceCount", 0);
    setProperty("pelicanAdoptedAliasCount", 0);
    setProperty("pelicanNotAdoptedAliasCount", 0);
    setProperty("pelicanFusionCandidateCount", 0);
    setProperty("pelicanParallelCandidateCount", 0);
    setProperty("pelicanPhysicalExplicitEmptyCount", 0);
    setProperty("pelicanPlanningProfile", QString{});
    setProperty("pelicanPlanningEndpoint", QString{});
    setProperty("pelicanCollapsedGroups", QStringList{});
    setProperty("pelicanExecutionPlanState", QString{});
    setProperty("pelicanExecutionPlanReasonCode", QString{});
    setProperty("pelicanExecutionPlanReason", QString{});
    publishStateProperties();
}

void FramePlanGraphicsScene::populate(
    const FramePlanModel &model,
    const std::optional<FramePlanNodeKey> &selected_target, int depth) {
    const int normalized_depth = std::max(0, depth);
    const auto retained_node = selected_node_;

    const bool target_is_valid =
        model.execution_plan.available() && selected_target &&
        selected_target->graph == model.graph &&
        std::any_of(model.resources.begin(), model.resources.end(),
                    [&](const FramePlanResource &resource) {
                        return resource.name == selected_target->name;
                    });
    selected_resource_ = target_is_valid ? selected_target : std::nullopt;
    const std::set<std::string, std::less<>> visible_names =
        selected_resource_
            ? subtreeNodeNames(model, selected_resource_->name,
                               normalized_depth)
            : std::set<std::string, std::less<>>{};
    const bool node_selection_survives =
        retained_node && retained_node->graph == model.graph &&
        visible_names.contains(retained_node->name);

    rebuilding_ = true;
    clear();
    selected_node_ = node_selection_survives ? retained_node : std::nullopt;

    if (!model.execution_plan.available()) {
        selected_resource_.reset();
        auto *panel = addRect(
            QRectF{0.0, 0.0, LogicalUnavailableWidth,
                   LogicalUnavailableHeight},
            QPen{QColor{QStringLiteral("#d68a32")}, 1.8},
            QBrush{QColor{QStringLiteral("#2b2118")}});
        panel->setZValue(1.0);
        annotateIdentity(*panel, FramePlanLogicalUnavailableItem, model.graph,
                         model.execution_plan.unavailable_reason_code);
        panel->setData(FramePlanLogicalStateRole,
                       QStringLiteral("unavailable"));
        panel->setData(FramePlanReasonCodeRole,
                       qtext(model.execution_plan.unavailable_reason_code));
        panel->setData(FramePlanReasonRole,
                       qtext(model.execution_plan.unavailable_reason));
        panel->setToolTip(qtext(model.execution_plan.unavailable_reason));

        auto *title = new QGraphicsSimpleTextItem(
            QStringLiteral("Logical graph unavailable"), panel);
        QFont title_font = title->font();
        title_font.setBold(true);
        title_font.setPointSizeF(12.0);
        title->setFont(title_font);
        title->setBrush(QColor{QStringLiteral("#f6d09a")});
        title->setPos(18.0, 18.0);
        auto *reason = new QGraphicsSimpleTextItem(
            qtext(model.execution_plan.unavailable_reason), panel);
        reason->setBrush(QColor{QStringLiteral("#efb366")});
        reason->setPos(18.0, 66.0);

        setSceneRect(panel->sceneBoundingRect().adjusted(-30.0, -30.0, 30.0,
                                                         30.0));
        rebuilding_ = false;
        setProperty("pelicanGraph", qtext(model.graph));
        setProperty("pelicanTarget", QString{});
        setProperty("pelicanSubtreeDepth", normalized_depth);
        setProperty("pelicanTargetCount", 0);
        setProperty("pelicanNodeRecordCount", 0);
        setProperty("pelicanDependencyRecordCount", 0);
        setProperty("pelicanVisibleDependencyRecordCount", 0);
        setProperty("pelicanVisibleItemCount", 0);
        setProperty("pelicanEdgeBundleCount", 0);
        setProperty("pelicanCurveEdgeCount", 0);
        setProperty("pelicanResourceOverlayCount", 0);
        setProperty("pelicanPhysicalResourceCount", 0);
        setProperty("pelicanAdoptedAliasCount", 0);
        setProperty("pelicanNotAdoptedAliasCount", 0);
        setProperty("pelicanPlanningProfile", QString{});
        setProperty("pelicanPlanningEndpoint", QString{});
        setProperty("pelicanExecutionPlanState",
                    QStringLiteral("unavailable"));
        setProperty("pelicanExecutionPlanReasonCode",
                    qtext(model.execution_plan.unavailable_reason_code));
        setProperty("pelicanExecutionPlanReason",
                    qtext(model.execution_plan.unavailable_reason));
        publishStateProperties();
        return;
    }

    std::map<std::size_t, std::vector<const FramePlanNode *>> levels;
    for (const auto &node : model.nodes) {
        if (visible_names.contains(node.name)) {
            levels[node.level].push_back(&node);
        }
    }
    for (auto &[level, nodes] : levels) {
        (void)level;
        std::ranges::sort(nodes, {}, [](const FramePlanNode *node) {
            return std::pair{node->order, node->name};
        });
    }

    std::map<std::string, QGraphicsPathItem *, std::less<>> node_items;
    std::size_t column = 0;
    for (const auto &[level, nodes] : levels) {
        (void)level;
        for (std::size_t row = 0; row < nodes.size(); ++row) {
            const FramePlanNode &node = *nodes[row];
            const FramePlanNodeKey key{model.graph, node.name};
            QPointF position{
                static_cast<qreal>(column) * (NodeWidth + HorizontalGap),
                static_cast<qreal>(row) * (NodeHeight + VerticalGap)};
            if (const auto retained = session_node_positions_.find(key);
                retained != session_node_positions_.end()) {
                position = retained->second;
            }

            auto *item = new MovableNodeItem(
                nodePath(isAnchor(node)),
                [this, key](const QPointF &moved_position) {
                    if (rebuilding_) {
                        return;
                    }
                    session_node_positions_[key] = moved_position;
                    rerouteConnectedBundles(*this, key.graph, key.name);
                    setSceneRect(itemsBoundingRect().adjusted(
                        -30.0, -30.0, 30.0, 30.0));
                });
            addItem(item);
            item->setPos(position);
            item->setZValue(2.0);
            const QColor fill = sourceColor(node.source);
            item->setBrush(fill);
            QPen outline{isAnchor(node)
                             ? QColor{QStringLiteral("#d8e3ec")}
                             : QColor{QStringLiteral("#edf2f6")}};
            outline.setWidthF(1.3);
            if (isAnchor(node)) {
                outline.setStyle(Qt::DashLine);
            }
            item->setPen(outline);
            item->setFlag(QGraphicsItem::ItemIsSelectable, true);
            item->setFlag(QGraphicsItem::ItemIsMovable, true);
            item->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
            annotateIdentity(*item, FramePlanNodeItem, model.graph, node.name);
            item->setData(FramePlanSourceRole, qtext(node.source));
            item->setData(FramePlanColorRole, fill.name(QColor::HexRgb));
            item->setData(FramePlanAnchorRole, isAnchor(node));
            item->setData(FramePlanMembersRole, qlist({node.name}));
            item->setData(FramePlanInternalEdgeRecordsRole, QStringList{});
            item->setData(FramePlanSubtreeDepthRole, normalized_depth);
            item->setToolTip(QStringLiteral("%1 / %2")
                                 .arg(qtext(model.graph), qtext(node.name)));
            addNodeLabel(*item, node, model.graph);
            node_items.emplace(node.name, item);
        }
        ++column;
    }

    std::vector<DependencyRecord> sorted_dependencies;
    for (const auto &dependency : model.dependencies) {
        if (visible_names.contains(dependency.from) &&
            visible_names.contains(dependency.to)) {
            sorted_dependencies.push_back(DependencyRecord{
                dependency.from, dependency.to, dependency.reason,
                dependency.resource});
        }
    }
    std::ranges::sort(sorted_dependencies, {},
                      [](const DependencyRecord &record) {
                          return std::tie(record.from, record.to,
                                          record.reason, record.resource);
                      });

    std::map<EntityPair, std::vector<DependencyRecord>> bundles;
    for (const auto &dependency : sorted_dependencies) {
        if (dependency.from != dependency.to) {
            bundles[{dependency.from, dependency.to}].push_back(dependency);
        }
    }

    std::size_t bundle_order = 0;
    std::size_t resource_overlay_count = 0;
    for (const auto &[endpoints, records] : bundles) {
        auto *edge = addPath(QPainterPath{});
        edge->setZValue(0.0);
        QPen edge_pen{QColor{QStringLiteral("#748394")}};
        edge_pen.setWidthF(1.6);
        edge->setPen(edge_pen);
        edge->setBrush(Qt::NoBrush);
        annotateIdentity(*edge, FramePlanEdgeItem, model.graph,
                         endpoints.first + "->" + endpoints.second);
        annotateEndpoints(*edge, endpoints);
        edge->setData(FramePlanBundleOrderRole,
                      static_cast<qulonglong>(bundle_order));
        edge->setData(FramePlanCurveRole, true);
        edge->setData(FramePlanSubtreeDepthRole, normalized_depth);

        std::vector<std::string> identities;
        std::set<std::string, std::less<>> resources;
        for (const auto &record : records) {
            identities.push_back(recordIdentity(record));
            if (!record.resource.empty()) {
                resources.insert(record.resource);
            }
        }
        edge->setData(FramePlanEdgeRecordsRole, qlist(identities));
        edge->setData(
            FramePlanResourcesRole,
            qlist(std::vector<std::string>{resources.begin(), resources.end()}));
        if (!resources.empty()) {
            ++resource_overlay_count;
        }

        auto *arrow = addPolygon(
            QPolygonF{}, QPen{QColor{QStringLiteral("#748394")}},
            QBrush{QColor{QStringLiteral("#748394")}});
        arrow->setZValue(0.5);
        annotateIdentity(*arrow, FramePlanEdgeArrowItem, model.graph,
                         endpoints.first + "->" + endpoints.second);
        annotateEndpoints(*arrow, endpoints);
        arrow->setData(FramePlanBundleOrderRole,
                       static_cast<qulonglong>(bundle_order));

        const QString label_text = dependencyLabel(records);
        QFont label_font;
        label_font.setPointSizeF(8.5);
        const qreal label_width = std::max(
            EdgeLabelMinimumWidth,
            QFontMetricsF{label_font}.horizontalAdvance(label_text) + 14.0);
        auto *label_box = addRect(
            QRectF{0.0, 0.0, label_width, EdgeLabelHeight},
            QPen{QColor{QStringLiteral("#9aa7b4")}},
            QBrush{QColor{QStringLiteral("#f5f7f9")}});
        label_box->setZValue(1.0);
        annotateIdentity(*label_box, FramePlanEdgeLabelItem, model.graph,
                         endpoints.first + "->" + endpoints.second);
        annotateEndpoints(*label_box, endpoints);
        label_box->setData(FramePlanEdgeRecordsRole, qlist(identities));
        label_box->setData(
            FramePlanResourcesRole,
            qlist(std::vector<std::string>{resources.begin(), resources.end()}));
        label_box->setData(FramePlanBundleOrderRole,
                           static_cast<qulonglong>(bundle_order));
        auto *label = new QGraphicsSimpleTextItem(label_text, label_box);
        label->setFont(label_font);
        label->setBrush(QColor{QStringLiteral("#263441")});
        const QRectF text_bounds = label->boundingRect();
        label->setPos((label_width - text_bounds.width()) / 2.0,
                      (EdgeLabelHeight - text_bounds.height()) / 2.0);

        routeLogicalBundle(*this, *edge);
        ++bundle_order;
    }

    const qreal logical_bottom = items().empty() ? 0.0
                                                 : itemsBoundingRect().bottom();
    const PhysicalOverlaySummary physical_overlay = addPhysicalOverlay(
        *this, model, logical_bottom, selected_resource_);

    if (selected_node_) {
        if (const auto found = node_items.find(selected_node_->name);
            found != node_items.end()) {
            found->second->setSelected(true);
        }
    }

    setSceneRect(itemsBoundingRect().adjusted(-30.0, -30.0, 30.0, 30.0));
    rebuilding_ = false;

    setProperty("pelicanGraph", qtext(model.graph));
    setProperty("pelicanTarget",
                selected_resource_ ? qtext(selected_resource_->name)
                                   : QString{});
    setProperty("pelicanSubtreeDepth", normalized_depth);
    setProperty("pelicanTargetCount",
                static_cast<qulonglong>(model.resources.size()));
    setProperty("pelicanNodeRecordCount",
                static_cast<qulonglong>(model.nodes.size()));
    setProperty("pelicanDependencyRecordCount",
                static_cast<qulonglong>(model.dependencies.size()));
    setProperty("pelicanVisibleDependencyRecordCount",
                static_cast<qulonglong>(sorted_dependencies.size()));
    setProperty("pelicanVisibleItemCount",
                static_cast<qulonglong>(visible_names.size()));
    setProperty("pelicanEdgeBundleCount",
                static_cast<qulonglong>(bundles.size()));
    setProperty("pelicanCurveEdgeCount",
                static_cast<qulonglong>(bundles.size()));
    setProperty("pelicanResourceOverlayCount",
                static_cast<qulonglong>(resource_overlay_count));
    setProperty("pelicanPhysicalResourceCount",
                static_cast<qulonglong>(physical_overlay.resource_count));
    setProperty("pelicanAdoptedAliasCount",
                static_cast<qulonglong>(
                    physical_overlay.adopted_alias_count));
    setProperty("pelicanNotAdoptedAliasCount",
                static_cast<qulonglong>(
                    physical_overlay.not_adopted_alias_count));
    setProperty("pelicanFusionCandidateCount", 0);
    setProperty("pelicanParallelCandidateCount", 0);
    setProperty("pelicanPhysicalExplicitEmptyCount", 0);
    setProperty("pelicanPlanningProfile",
                qtext(model.physical_plan.planning_profile));
    setProperty("pelicanPlanningEndpoint",
                qtext(model.physical_plan.planning_endpoint));
    setProperty("pelicanCollapsedGroups", QStringList{});
    setProperty("pelicanExecutionPlanState", QStringLiteral("available"));
    setProperty("pelicanExecutionPlanReasonCode", QString{});
    setProperty("pelicanExecutionPlanReason", QString{});
    publishStateProperties();
}

void FramePlanGraphicsScene::recordSelection() {
    if (rebuilding_) {
        return;
    }
    selected_node_.reset();
    for (QGraphicsItem *item : selectedItems()) {
        if (itemKind(*item) != QLatin1String{FramePlanNodeItem}) {
            continue;
        }
        selected_node_ = FramePlanNodeKey{
            item->data(FramePlanGraphRole).toString().toStdString(),
            item->data(FramePlanNameRole).toString().toStdString(),
        };
        break;
    }
    publishStateProperties();
}

void FramePlanGraphicsScene::publishStateProperties() {
    setProperty("pelicanSelectedGraph",
                selected_node_ ? qtext(selected_node_->graph) : QString{});
    setProperty("pelicanSelectedNode",
                selected_node_ ? qtext(selected_node_->name) : QString{});
    setProperty("pelicanSelectedTarget",
                selected_resource_ ? qtext(selected_resource_->name)
                                   : QString{});
    setProperty("pelicanSelectedResource",
                selected_resource_ ? qtext(selected_resource_->name)
                                   : QString{});

    QGraphicsItem *resource_item = nullptr;
    if (selected_resource_) {
        for (QGraphicsItem *item : items()) {
            if (itemKind(*item) ==
                    QLatin1String{FramePlanResourceLifetimeItem} &&
                item->data(FramePlanNameRole).toString() ==
                    qtext(selected_resource_->name)) {
                resource_item = item;
                break;
            }
        }
    }
    setProperty("pelicanSelectedResourceReason",
                resource_item
                    ? resource_item->data(FramePlanReasonRole).toString()
                    : QString{});
    setProperty("pelicanSelectedResourceWidestRead",
                resource_item
                    ? resource_item->data(FramePlanWidestReadRole).toString()
                    : QString{});
    setProperty("pelicanSelectedResourceAliasable",
                resource_item ? resource_item->data(FramePlanAliasableRole)
                              : QVariant{});
    setProperty("pelicanSelectedResourceRepresentation",
                resource_item
                    ? resource_item->data(FramePlanRepresentationRole)
                          .toString()
                    : QString{});
    QString lifetime;
    if (resource_item) {
        lifetime = resource_item->data(FramePlanLifetimeUsedRole).toBool()
                       ? QStringLiteral("[%1, %2]")
                             .arg(resource_item
                                      ->data(FramePlanLifetimeFirstRole)
                                      .toULongLong())
                             .arg(resource_item
                                      ->data(FramePlanLifetimeLastRole)
                                      .toULongLong())
                       : QStringLiteral("unused");
    }
    setProperty("pelicanSelectedResourceLifetime", lifetime);
}

} // namespace PelicanStudio
