#include "frameplangraphics.hpp"

#include <QAction>
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QKeyEvent>
#include <QLineF>
#include <QMenu>
#include <QMetaObject>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

constexpr qreal NodeWidth = 240.0;
constexpr qreal NodeHeight = 64.0;
constexpr qreal GroupWidth = 260.0;
constexpr qreal GroupHeight = 96.0;
constexpr qreal BoundaryStubWidth = 210.0;
constexpr qreal BoundaryStubHeight = 54.0;
constexpr qreal HorizontalGap = 88.0;
constexpr qreal VerticalGap = 74.0;
constexpr qreal StaggerOffset = 132.0;
constexpr qreal EdgeLabelHeight = 24.0;
constexpr qreal EdgeLabelMinimumWidth = 120.0;
constexpr qreal GroupWarningWidth = 680.0;
constexpr qreal GroupWarningHeight = 54.0;
constexpr qreal BreadcrumbHeight = 34.0;
constexpr qreal PhysicalPanelGap = 74.0;
constexpr qreal PhysicalPanelMinimumWidth = 760.0;
constexpr qreal PhysicalHeaderHeight = 76.0;
constexpr qreal PhysicalResourceHeight = 92.0;
constexpr qreal PhysicalOutcomeHeight = 34.0;
constexpr qreal LogicalUnavailableWidth = 960.0;
constexpr qreal LogicalUnavailableHeight = 176.0;

using EntityPair = std::pair<std::string, std::string>;

struct GroupingUnit {
    std::string key;
    std::string entity_base;
    QString label;
    QString subject;
};

struct GroupingDecision {
    std::optional<GroupingUnit> unit;
    std::string diagnostic_key;
    QString reason;
};

enum class LoweringNodeLookupState {
    found,
    node_missing,
    unavailable,
};

struct LoweringNodeLookup {
    LoweringNodeLookupState state = LoweringNodeLookupState::unavailable;
    const FramePlanLoweringNode *node = nullptr;
};

struct GroupDefinition {
    std::string key;
    std::pair<std::string, std::string> state_key;
    std::string entity_name;
    QString label;
    std::vector<std::string> members;
    bool collapsible = true;
    bool warning_only = false;
    QString reason;
};

struct VisibleEntity {
    std::string key;
    QString label;
    std::string source;
    std::vector<std::string> members;
    std::vector<std::string> internal_records;
    std::size_t internal_barrier_count = 0;
    std::size_t internal_fused_barrier_count = 0;
    std::string group_id;
    std::string stable_group_key;
    std::string boundary_target;
    QString boundary_direction;
    std::size_t order = 0;
    int source_column = 0;
    bool anchor = false;
    bool group = false;
    bool boundary_stub = false;
    QGraphicsPathItem *item = nullptr;
};

struct DependencyRecord {
    std::string from;
    std::string to;
    std::string reason;
    std::string resource;
    std::optional<std::size_t> barrier_index;
    std::string barrier_kind;
    bool same_pixel_attachment = false;
    bool fused_scope_absorbed = false;
};

struct DependencySummary {
    std::set<std::string, std::less<>> barrier_kinds;
    std::set<std::string, std::less<>> order_reasons;
    std::set<std::string, std::less<>> resources;
    std::size_t barrier_count = 0;
    std::size_t order_only_count = 0;
    std::size_t same_pixel_attachment_count = 0;
    std::size_t fused_barrier_count = 0;
    QString label;
};

struct BarrierCoverage {
    std::size_t total = 0;
    std::size_t on_edges = 0;
    std::size_t inside_collapsed_groups = 0;
    std::size_t outside_window = 0;
    std::size_t unmatched = 0;
    bool execution_available = false;
    bool target_selected = false;
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

std::string dependencyReasonKind(std::string_view reason) {
    const std::size_t marker = reason.rfind('.');
    const std::size_t start =
        marker == std::string_view::npos ? 0 : marker + 1;
    const std::size_t version = reason.rfind('@');
    const std::size_t end =
        version == std::string_view::npos || version < start
            ? reason.size()
            : version;
    return std::string{reason.substr(start, end - start)};
}

QStringList qlist(const std::vector<std::string> &values) {
    QStringList result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const auto &value : values) {
        result.push_back(qtext(value));
    }
    return result;
}

bool isSamePixelAttachmentBarrier(const FramePlanModel &model,
                                  const FramePlanBarrier &barrier) {
    const auto destination = std::ranges::find(
        model.nodes, barrier.to, &FramePlanNode::name);
    return destination != model.nodes.end() &&
           std::ranges::any_of(
               destination->resource_uses,
               [&](const FramePlanResourceUse &use) {
                   return use.resource == barrier.resource &&
                          use.footprint == "same_pixel" &&
                          use.intent == "attachment";
               });
}

bool isFusedScopeBarrier(const FramePlanModel &model,
                         const FramePlanBarrier &barrier) {
    return std::ranges::any_of(
        model.physical_plan.scopes,
        [&](const FramePlanPhysicalScope &scope) {
            return std::ranges::find(scope.nodes, barrier.from) !=
                       scope.nodes.end() &&
                   std::ranges::find(scope.nodes, barrier.to) !=
                       scope.nodes.end() &&
                   std::ranges::find(scope.local_reads, barrier.resource) !=
                       scope.local_reads.end();
        });
}

std::vector<DependencyRecord> dependencyRecords(
    const FramePlanModel &model) {
    std::vector<bool> claimed_barriers(model.barriers.size(), false);
    std::vector<DependencyRecord> records;
    records.reserve(model.dependencies.size());
    for (const auto &dependency : model.dependencies) {
        DependencyRecord record{
            .from = dependency.from,
            .to = dependency.to,
            .reason = dependency.reason,
            .resource = dependency.resource,
        };
        const std::string reason_kind =
            dependencyReasonKind(dependency.reason);
        for (std::size_t index = 0; index < model.barriers.size(); ++index) {
            const FramePlanBarrier &barrier = model.barriers[index];
            if (claimed_barriers[index] || barrier.from != dependency.from ||
                barrier.to != dependency.to ||
                barrier.resource != dependency.resource ||
                barrier.kind != reason_kind) {
                continue;
            }
            claimed_barriers[index] = true;
            record.barrier_index = index;
            record.barrier_kind = barrier.kind;
            record.same_pixel_attachment =
                isSamePixelAttachmentBarrier(model, barrier);
            record.fused_scope_absorbed =
                isFusedScopeBarrier(model, barrier);
            break;
        }
        records.push_back(std::move(record));
    }
    std::ranges::sort(records, {}, [](const DependencyRecord &record) {
        return std::tie(record.from, record.to, record.reason,
                        record.resource);
    });
    return records;
}

constexpr std::string_view LegacyRegionPrefix = "legacy.";

bool isLegacyRegion(std::string_view region) {
    return region.starts_with(LegacyRegionPrefix);
}

QString legacyRegionPrefixText() {
    return QString::fromUtf8(
        LegacyRegionPrefix.data(),
        static_cast<qsizetype>(LegacyRegionPrefix.size()));
}

// This exact-name join is the sole bridge from frame-plan nodes to the
// physical lowering graph. The producer guarantees the two name sets match;
// keeping the lookup here makes the grouping policy independent of all other
// physical-plan machinery. An unavailable graph, a valid graph with a broken
// name join, and a successful join are deliberately distinct outcomes.
LoweringNodeLookup loweringNode(
    const FramePlanModel &model, const FramePlanNode &node) {
    if (!model.physical_plan.loweringGraphAvailable()) {
        return {.state = LoweringNodeLookupState::unavailable};
    }
    const auto found = std::ranges::find(
        model.physical_plan.lowering_nodes, node.name,
        &FramePlanLoweringNode::name);
    if (found == model.physical_plan.lowering_nodes.end()) {
        return {.state = LoweringNodeLookupState::node_missing};
    }
    return {
        .state = LoweringNodeLookupState::found,
        .node = &*found,
    };
}

QString regionGroupingUnavailableReason(const FramePlanModel &model) {
    return QStringLiteral(
               "region_grouping_unavailable: authored-region grouping "
               "requires an available physical lowering graph (%1).")
        .arg(qtext(
            model.physical_plan.lowering_graph_unavailable_reason));
}

// This remains the only policy function that decides grouping membership.
// One authored region takes precedence over provider provenance. A feature is
// the compatibility fallback only when there is no authored region. Multiple
// authored regions deliberately suppress both choices because choosing either
// would silently invent overlap/nesting semantics that the view does not have.
GroupingDecision groupingUnit(const FramePlanModel &model,
                              const FramePlanNode &node) {
    const LoweringNodeLookup lookup = loweringNode(model, node);
    if (lookup.state == LoweringNodeLookupState::unavailable) {
        return GroupingDecision{
            .diagnostic_key = "region-grouping-unavailable",
            .reason = regionGroupingUnavailableReason(model),
        };
    }
    if (lookup.state == LoweringNodeLookupState::node_missing) {
        return GroupingDecision{
            .diagnostic_key = "lowering-node-missing:" + node.name,
            .reason =
                QStringLiteral(
                    "region_grouping_node_missing: node \"%1\" has no "
                    "matching lowering node; provider-feature fallback is "
                    "disabled because authored-region membership is "
                    "unknown.")
                    .arg(qtext(node.name)),
        };
    }

    std::vector<std::string> authored_regions;
    for (const auto &region : lookup.node->regions) {
        if (!isLegacyRegion(region)) {
            authored_regions.push_back(region);
        }
    }
    std::ranges::sort(authored_regions);

    if (authored_regions.size() > 1) {
        return GroupingDecision{
            .diagnostic_key = "multiple-regions:" + node.name,
            .reason =
                QStringLiteral(
                    "Node \"%1\" belongs to multiple authored regions "
                    "(\"%2\") and is not grouped: nested or overlapping "
                    "groups are not supported.")
                    .arg(qtext(node.name),
                         qlist(authored_regions)
                             .join(QStringLiteral("\", \""))),
        };
    }
    if (authored_regions.size() == 1) {
        const std::string &region = authored_regions.front();
        return GroupingDecision{
            .unit = GroupingUnit{
                .key = "region:" + region,
                .entity_base = "__pelican_group__:region:" + region,
                .label =
                    QStringLiteral("Region: %1").arg(qtext(region)),
                .subject =
                    QStringLiteral("Region \"%1\"").arg(qtext(region)),
            },
        };
    }
    if (node.provider_feature.empty()) {
        return {};
    }
    return GroupingDecision{
        .unit = GroupingUnit{
            .key = "feature:" + node.provider_feature,
            .entity_base = "__pelican_group__:" + node.provider_feature,
            .label = QStringLiteral("Feature: %1")
                         .arg(qtext(node.provider_feature)),
            .subject = QStringLiteral("Feature \"%1\"")
                           .arg(qtext(node.provider_feature)),
        },
    };
}

QString ignoredLegacyRegionNotice(const FramePlanModel &model,
                                  const FramePlanNode &node) {
    const LoweringNodeLookup lookup = loweringNode(model, node);
    if (lookup.state != LoweringNodeLookupState::found ||
        std::ranges::none_of(lookup.node->regions, isLegacyRegion)) {
        return {};
    }
    return QStringLiteral(
               "Region tags beginning with \"%1\" are compatibility tags "
               "and are ignored for grouping, including tags authored with "
               "that prefix.")
        .arg(legacyRegionPrefixText());
}

std::pair<std::string, std::string> groupStateKey(
    const FramePlanModel &model, const std::string &group_key) {
    return {model.graph, group_key};
}

// Group state is structural in memory. Qt properties and data roles need a
// string representation, so encode both UTF-8 byte strings with explicit
// lengths. Unlike a delimiter join, this remains injective when either input
// contains control characters or delimiter-looking text.
std::string groupStateId(
    const std::pair<std::string, std::string> &state_key) {
    const auto field = [](const std::string &value) {
        return std::to_string(value.size()) + ":" + value;
    };
    return "group-state-v1:" + field(state_key.first) +
           field(state_key.second);
}

QString nonConvexReason(
    const FramePlanModel &model, const QString &subject,
    const std::set<std::string, std::less<>> &members) {
    std::map<std::string, std::vector<std::string>, std::less<>> adjacency;
    for (const auto &dependency : model.dependencies) {
        adjacency[dependency.from].push_back(dependency.to);
    }
    for (auto &[from, destinations] : adjacency) {
        (void)from;
        std::ranges::sort(destinations);
        destinations.erase(
            std::unique(destinations.begin(), destinations.end()),
            destinations.end());
    }

    std::queue<std::string> frontier;
    std::set<std::string, std::less<>> visited;
    std::map<std::string, std::string, std::less<>> exit_member;
    for (const auto &member : members) {
        const auto outgoing = adjacency.find(member);
        if (outgoing == adjacency.end()) {
            continue;
        }
        for (const auto &destination : outgoing->second) {
            if (!members.contains(destination) &&
                visited.insert(destination).second) {
                frontier.push(destination);
                exit_member.emplace(destination, member);
            }
        }
    }

    while (!frontier.empty()) {
        std::string outside = std::move(frontier.front());
        frontier.pop();
        const auto outgoing = adjacency.find(outside);
        if (outgoing == adjacency.end()) {
            continue;
        }
        for (const auto &destination : outgoing->second) {
            if (members.contains(destination)) {
                return QStringLiteral(
                           "%1 cannot be collapsed because it is "
                           "not convex: a dependency path leaves member \"%2\", "
                           "passes through \"%3\", and returns to member \"%4\".")
                    .arg(subject, qtext(exit_member.at(outside)),
                         qtext(outside), qtext(destination));
            }
            if (visited.insert(destination).second) {
                frontier.push(destination);
                exit_member.emplace(destination, exit_member.at(outside));
            }
        }
    }
    return {};
}

std::vector<GroupDefinition> discoverGroups(const FramePlanModel &model) {
    std::map<std::string, std::vector<std::string>, std::less<>> members_by_key;
    std::map<std::string, GroupingUnit, std::less<>> units_by_key;
    std::vector<GroupDefinition> warnings;
    if (!model.physical_plan.loweringGraphAvailable()) {
        std::vector<std::string> members;
        members.reserve(model.nodes.size());
        for (const auto &node : model.nodes) {
            members.push_back(node.name);
        }
        const std::string key = "diagnostic:region-grouping-unavailable";
        warnings.push_back(GroupDefinition{
            .key = key,
            .state_key = groupStateKey(model, key),
            .entity_name = "__pelican_group_warning__:lowering-unavailable",
            .label = QStringLiteral("Authored regions unavailable"),
            .members = std::move(members),
            .collapsible = false,
            .warning_only = true,
            .reason = regionGroupingUnavailableReason(model),
        });
        return warnings;
    }
    for (const auto &node : model.nodes) {
        GroupingDecision decision = groupingUnit(model, node);
        if (!decision.reason.isEmpty()) {
            const std::string key = "diagnostic:" +
                                    decision.diagnostic_key;
            warnings.push_back(GroupDefinition{
                .key = key,
                .state_key = groupStateKey(model, key),
                .entity_name = "__pelican_group_warning__:" + node.name,
                .label = QStringLiteral("Node: %1").arg(qtext(node.name)),
                .members = {node.name},
                .collapsible = false,
                .warning_only = true,
                .reason = std::move(decision.reason),
            });
            continue;
        }
        if (!decision.unit) {
            continue;
        }
        members_by_key[decision.unit->key].push_back(node.name);
        units_by_key.emplace(decision.unit->key,
                             std::move(*decision.unit));
    }

    std::set<std::string, std::less<>> occupied_names;
    for (const auto &node : model.nodes) {
        occupied_names.insert(node.name);
    }

    std::vector<GroupDefinition> groups;
    for (auto &[key, members] : members_by_key) {
        if (members.size() < 2) {
            continue;
        }
        std::ranges::sort(members);
        const GroupingUnit &unit = units_by_key.at(key);
        const std::set<std::string, std::less<>> member_set{members.begin(),
                                                            members.end()};
        const QString reason =
            nonConvexReason(model, unit.subject, member_set);

        const std::string &base_name = unit.entity_base;
        std::string entity_name = base_name;
        std::size_t suffix = 2;
        while (occupied_names.contains(entity_name)) {
            entity_name = base_name + "#" + std::to_string(suffix++);
        }
        occupied_names.insert(entity_name);

        groups.push_back(GroupDefinition{
            .key = key,
            .state_key = groupStateKey(model, key),
            .entity_name = std::move(entity_name),
            .label = unit.label,
            .members = std::move(members),
            .collapsible = reason.isEmpty(),
            .reason = reason,
        });
    }
    groups.insert(groups.end(), warnings.begin(), warnings.end());
    return groups;
}

const GroupDefinition *findGroup(const std::vector<GroupDefinition> &groups,
                                 const std::pair<std::string, std::string>
                                     &state_key) {
    const auto found = std::ranges::find(groups, state_key,
                                         &GroupDefinition::state_key);
    return found == groups.end() ? nullptr : &*found;
}

const GroupDefinition *findGroupById(
    const std::vector<GroupDefinition> &groups, const QString &group_id) {
    const auto found = std::ranges::find_if(
        groups, [&](const GroupDefinition &group) {
            return qtext(groupStateId(group.state_key)) == group_id;
        });
    return found == groups.end() ? nullptr : &*found;
}

std::string uniqueSyntheticName(
    std::string base,
    std::set<std::string, std::less<>> &occupied_names) {
    std::string candidate = base;
    std::size_t suffix = 2;
    while (occupied_names.contains(candidate)) {
        candidate = base + "#" + std::to_string(suffix++);
    }
    occupied_names.insert(candidate);
    return candidate;
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

// Column for the subtree layout: the longest path inside the visible subtree,
// so every dependency edge points left to right. model.dependencies is the
// same relation the edges are drawn from, and node.level cannot be used --
// it is the whole-graph depth, which holds one node per level here.
std::map<std::string, int, std::less<>> subtreeNodeColumns(
    const FramePlanModel &model,
    const std::set<std::string, std::less<>> &visible) {
    std::map<std::string, int, std::less<>> columns;
    for (const auto &name : visible) {
        columns.emplace(name, 0);
    }
    // Relaxation terminates because the dependency relation is acyclic; the
    // bound keeps a malformed payload from spinning here.
    for (std::size_t pass = 0; pass < visible.size(); ++pass) {
        bool changed = false;
        for (const auto &dependency : model.dependencies) {
            const auto from = columns.find(dependency.from);
            const auto to = columns.find(dependency.to);
            if (from == columns.end() || to == columns.end()) {
                continue;
            }
            if (to->second < from->second + 1) {
                to->second = from->second + 1;
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }
    return columns;
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

QPainterPath roundedEntityPath(qreal width, qreal height, qreal radius) {
    QPainterPath path;
    path.addRoundedRect(QRectF{0.0, 0.0, width, height}, radius, radius);
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

void addCenteredEntityLabel(QGraphicsPathItem &item, const QString &text,
                            qreal width, qreal height,
                            const std::string &kind,
                            const std::string &graph,
                            const std::string &name) {
    auto *label = new QGraphicsSimpleTextItem(text, &item);
    label->setBrush(QColor{QStringLiteral("#f7f9fb")});
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    const QRectF bounds = label->boundingRect();
    label->setPos((width - bounds.width()) / 2.0,
                  (height - bounds.height()) / 2.0);
    annotateIdentity(*label, kind, graph, name);
}

DependencySummary dependencySummary(
    const std::vector<DependencyRecord> &records) {
    DependencySummary summary;
    for (const auto &record : records) {
        if (!record.resource.empty()) {
            summary.resources.insert(record.resource);
        }
        if (record.barrier_index) {
            ++summary.barrier_count;
            summary.barrier_kinds.insert(record.barrier_kind);
            summary.same_pixel_attachment_count +=
                static_cast<std::size_t>(record.same_pixel_attachment);
            summary.fused_barrier_count +=
                static_cast<std::size_t>(record.fused_scope_absorbed);
        } else {
            ++summary.order_only_count;
            summary.order_reasons.insert(
                dependencyReasonKind(record.reason));
        }
    }

    QStringList parts;
    if (summary.barrier_count != 0) {
        QStringList kinds;
        for (const auto &kind : summary.barrier_kinds) {
            kinds.push_back(qtext(kind));
        }
        parts.push_back(
            summary.barrier_count == 1
                ? QStringLiteral("barrier: %1").arg(
                      kinds.join(QStringLiteral(", ")))
                : QStringLiteral("barriers (%1): %2")
                      .arg(static_cast<qulonglong>(summary.barrier_count))
                      .arg(kinds.join(QStringLiteral(", "))));
    }

    if (!summary.resources.empty()) {
        QStringList resources;
        for (const auto &resource : summary.resources) {
            resources.push_back(qtext(resource));
        }
        parts.push_back(
            summary.resources.size() == 1
                ? QStringLiteral("resource: %1").arg(resources.front())
                : QStringLiteral("resources: %1").arg(
                      resources.join(QStringLiteral(", "))));
    }

    if (summary.same_pixel_attachment_count != 0) {
        QString marker = QStringLiteral("same-pixel attachment");
        if (summary.same_pixel_attachment_count != 1 ||
            summary.barrier_count != 1) {
            marker += QStringLiteral(" (%1)").arg(
                static_cast<qulonglong>(
                    summary.same_pixel_attachment_count));
        }
        parts.push_back(std::move(marker));
    }

    if (summary.fused_barrier_count != 0) {
        QString marker = QStringLiteral("absorbed in fused scope");
        if (summary.fused_barrier_count != 1 ||
            summary.barrier_count != 1) {
            marker += QStringLiteral(" (%1)").arg(
                static_cast<qulonglong>(summary.fused_barrier_count));
        }
        parts.push_back(std::move(marker));
    }

    if (summary.order_only_count != 0) {
        QStringList reasons;
        for (const auto &reason : summary.order_reasons) {
            reasons.push_back(qtext(reason));
        }
        QString marker = QStringLiteral("order only: %1").arg(
            reasons.join(QStringLiteral(", ")));
        if (summary.order_only_count != 1) {
            marker += QStringLiteral(" (%1)").arg(
                static_cast<qulonglong>(summary.order_only_count));
        }
        parts.push_back(std::move(marker));
    }

    summary.label = parts.join(QStringLiteral(" · "));
    if (records.size() > 1) {
        summary.label += QStringLiteral("  x%1").arg(records.size());
    }
    return summary;
}

QString barrierCoverageText(const BarrierCoverage &coverage) {
    if (!coverage.execution_available) {
        return QStringLiteral(
                   "Visible-subtree barriers: 0/%1 on edges; %1 cannot be "
                   "placed because the execution plan is unavailable.")
            .arg(static_cast<qulonglong>(coverage.total));
    }
    if (coverage.total == 0) {
        return QStringLiteral(
            "Visible-subtree barriers: 0/0 on edges; no barriers were "
            "published.");
    }

    QStringList details;
    if (coverage.inside_collapsed_groups != 0) {
        details.push_back(
            QStringLiteral("%1 inside collapsed groups")
                .arg(static_cast<qulonglong>(
                    coverage.inside_collapsed_groups)));
    }
    if (coverage.outside_window != 0) {
        QString outside = QStringLiteral("%1 outside the current window")
                              .arg(static_cast<qulonglong>(
                                  coverage.outside_window));
        if (!coverage.target_selected) {
            outside += QStringLiteral(" (no target selected)");
        }
        details.push_back(std::move(outside));
    }
    if (coverage.unmatched != 0) {
        details.push_back(
            QStringLiteral("%1 could not be matched to dependencies")
                .arg(static_cast<qulonglong>(coverage.unmatched)));
    }
    if (details.isEmpty()) {
        details.push_back(QStringLiteral("none hidden"));
    }
    return QStringLiteral("Visible-subtree barriers: %1/%2 on edges; %3.")
        .arg(static_cast<qulonglong>(coverage.on_edges))
        .arg(static_cast<qulonglong>(coverage.total))
        .arg(details.join(QStringLiteral("; ")));
}

void publishBarrierCoverage(QGraphicsScene &scene,
                            const BarrierCoverage &coverage) {
    scene.setProperty("pelicanBarrierRecordCount",
                      static_cast<qulonglong>(coverage.total));
    scene.setProperty("pelicanVisibleBarrierRecordCount",
                      static_cast<qulonglong>(coverage.on_edges));
    scene.setProperty(
        "pelicanInternalBarrierRecordCount",
        static_cast<qulonglong>(coverage.inside_collapsed_groups));
    scene.setProperty("pelicanOutsideBarrierRecordCount",
                      static_cast<qulonglong>(coverage.outside_window));
    scene.setProperty("pelicanUnmatchedBarrierRecordCount",
                      static_cast<qulonglong>(coverage.unmatched));
    scene.setProperty("pelicanBarrierCoverage",
                      barrierCoverageText(coverage));
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
        const QString kind = itemKind(*item);
        if ((kind == QLatin1String{FramePlanNodeItem} ||
             kind == QLatin1String{FramePlanGroupItem} ||
             kind == QLatin1String{FramePlanBoundaryStubItem}) &&
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

FramePlanGraphicsScene::~FramePlanGraphicsScene() {
    // QGraphicsScene clears selected items in its base destructor. Do it while
    // this class's selection members still exist, with the signal handler
    // guarded, because a multiple selection otherwise emits during teardown.
    rebuilding_ = true;
    clear();
}

void FramePlanGraphicsScene::resetGraph() {
    rebuilding_ = true;
    clear();
    selected_node_.reset();
    selected_nodes_.clear();
    selected_resource_.reset();
    session_node_positions_.clear();
    current_model_.reset();
    current_target_.reset();
    current_depth_ = 1;
    collapsed_groups_.clear();
    current_group_scope_.reset();
    group_feedback_.clear();
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
    setProperty("pelicanCurrentGroupScope", QString{});
    setProperty("pelicanCurrentGroupScopeLabel", QString{});
    setProperty("pelicanNonCollapsibleGroups", QStringList{});
    setProperty("pelicanGroupFeedback", QString{});
    setProperty("pelicanBoundaryStubCount", 0);
    setProperty("pelicanBarrierRecordCount", 0);
    setProperty("pelicanVisibleBarrierRecordCount", 0);
    setProperty("pelicanInternalBarrierRecordCount", 0);
    setProperty("pelicanOutsideBarrierRecordCount", 0);
    setProperty("pelicanUnmatchedBarrierRecordCount", 0);
    setProperty("pelicanBarrierCoverage", QString{});
    setProperty("pelicanExecutionPlanState", QString{});
    setProperty("pelicanExecutionPlanReasonCode", QString{});
    setProperty("pelicanExecutionPlanReason", QString{});
    publishStateProperties();
}

void FramePlanGraphicsScene::populate(
    const FramePlanModel &model,
    const std::optional<FramePlanNodeKey> &selected_target, int depth) {
    current_model_ = model;
    current_target_ = selected_target;
    current_depth_ = std::max(0, depth);
    pruneGroupState();
    renderCurrentGraph();
}

void FramePlanGraphicsScene::pruneGroupState() {
    group_feedback_.clear();
    if (!current_model_) {
        collapsed_groups_.clear();
        current_group_scope_.reset();
        return;
    }

    const auto groups = discoverGroups(*current_model_);
    std::set<GroupStateKey> collapsible_groups;
    for (const auto &group : groups) {
        if (group.collapsible) {
            collapsible_groups.insert(group.state_key);
        }
    }
    std::erase_if(collapsed_groups_, [&](const GroupStateKey &group_key) {
        return !collapsible_groups.contains(group_key);
    });
    if (current_group_scope_) {
        const GroupDefinition *scope =
            findGroup(groups, *current_group_scope_);
        if (scope == nullptr) {
            current_group_scope_.reset();
        } else if (!scope->collapsible) {
            // An update can invalidate a scope that was convex when entered.
            // Do not leave the scene in a state enterGroup() would reject.
            group_feedback_ = scope->reason;
            current_group_scope_.reset();
        }
    }
}

void FramePlanGraphicsScene::renderCurrentGraph() {
    if (!current_model_) {
        return;
    }
    const FramePlanModel &model = *current_model_;
    const int normalized_depth = current_depth_;
    const std::vector<GroupDefinition> groups = discoverGroups(model);
    const GroupDefinition *scope_group =
        current_group_scope_ ? findGroup(groups, *current_group_scope_)
                             : nullptr;
    const auto retained_primary_node = selected_node_;
    const auto retained_nodes = selected_nodes_;

    const bool target_is_valid =
        model.execution_plan.available() && current_target_ &&
        current_target_->graph == model.graph &&
        std::any_of(model.resources.begin(), model.resources.end(),
                    [&](const FramePlanResource &resource) {
                        return resource.name == current_target_->name;
                    });
    selected_resource_ = target_is_valid ? current_target_ : std::nullopt;
    const std::set<std::string, std::less<>> visible_names =
        scope_group
            ? std::set<std::string, std::less<>>{
                  scope_group->members.begin(), scope_group->members.end()}
            : selected_resource_
                  ? subtreeNodeNames(model, selected_resource_->name,
                                     normalized_depth)
                  : std::set<std::string, std::less<>>{};
    const auto node_selection_survives = [&](const FramePlanNodeKey &node) {
        if (node.graph != model.graph ||
            !visible_names.contains(node.name)) {
            return false;
        }
        return scope_group != nullptr ||
               std::ranges::none_of(
                   groups, [&](const GroupDefinition &group) {
                       return group.collapsible &&
                              collapsed_groups_.contains(group.state_key) &&
                              std::ranges::find(group.members, node.name) !=
                                  group.members.end();
                   });
    };
    std::set<FramePlanNodeKey> surviving_nodes;
    for (const auto &node : retained_nodes) {
        if (node_selection_survives(node)) {
            surviving_nodes.insert(node);
        }
    }

    const auto publish_group_properties = [&] {
        QStringList collapsed;
        for (const auto &group_key : collapsed_groups_) {
            collapsed.push_back(qtext(groupStateId(group_key)));
        }
        setProperty("pelicanCollapsedGroups", collapsed);
        setProperty("pelicanCurrentGroupScope",
                    current_group_scope_
                        ? qtext(groupStateId(*current_group_scope_))
                        : QString{});
        setProperty("pelicanCurrentGroupScopeLabel",
                    scope_group ? scope_group->label : QString{});

        QStringList non_collapsible;
        QString first_reason;
        for (const auto &group : groups) {
            if (group.collapsible) {
                continue;
            }
            non_collapsible.push_back(
                qtext(groupStateId(group.state_key)));
            if (first_reason.isEmpty()) {
                first_reason = group.reason;
            }
        }
        setProperty("pelicanNonCollapsibleGroups", non_collapsible);
        setProperty("pelicanGroupFeedback",
                    group_feedback_.isEmpty() ? first_reason
                                              : group_feedback_);
    };

    rebuilding_ = true;
    clear();
    selected_nodes_ = std::move(surviving_nodes);
    if (retained_primary_node &&
        selected_nodes_.contains(*retained_primary_node)) {
        selected_node_ = retained_primary_node;
    } else {
        selected_node_ = selected_nodes_.empty()
                             ? std::nullopt
                             : std::optional{*selected_nodes_.begin()};
    }

    if (!model.execution_plan.available()) {
        selected_resource_.reset();
        const BarrierCoverage barrier_coverage{
            .total = model.barriers.size(),
            .unmatched = model.barriers.size(),
            .execution_available = false,
            .target_selected = current_target_.has_value(),
        };
        const QString coverage_text =
            barrierCoverageText(barrier_coverage);
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
        panel->setToolTip(
            QStringLiteral("%1\n%2")
                .arg(qtext(model.execution_plan.unavailable_reason),
                     coverage_text));

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
        auto *coverage = new QGraphicsSimpleTextItem(
            coverage_text, panel);
        coverage->setBrush(QColor{QStringLiteral("#f6d09a")});
        coverage->setPos(18.0, 106.0);

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
        setProperty("pelicanBoundaryStubCount", 0);
        setProperty("pelicanExecutionPlanState",
                    QStringLiteral("unavailable"));
        setProperty("pelicanExecutionPlanReasonCode",
                    qtext(model.execution_plan.unavailable_reason_code));
        setProperty("pelicanExecutionPlanReason",
                    qtext(model.execution_plan.unavailable_reason));
        publishBarrierCoverage(*this, barrier_coverage);
        publish_group_properties();
        publishStateProperties();
        return;
    }

    const std::map<std::string, int, std::less<>> node_columns =
        subtreeNodeColumns(model, visible_names);
    std::map<std::string, const FramePlanNode *, std::less<>> nodes_by_name;
    for (const auto &node : model.nodes) {
        nodes_by_name.emplace(node.name, &node);
    }
    std::map<std::string, const GroupDefinition *, std::less<>>
        group_for_node;
    for (const auto &group : groups) {
        if (group.warning_only) {
            continue;
        }
        for (const auto &member : group.members) {
            group_for_node.emplace(member, &group);
        }
    }

    std::map<std::string, VisibleEntity, std::less<>> entities;
    std::map<std::string, std::string, std::less<>> entity_for_node;
    if (!scope_group) {
        for (const auto &group : groups) {
            if (!group.collapsible ||
                !collapsed_groups_.contains(group.state_key)) {
                continue;
            }
            std::vector<std::string> visible_members;
            for (const auto &member : group.members) {
                if (visible_names.contains(member)) {
                    visible_members.push_back(member);
                }
            }
            if (visible_members.empty()) {
                continue;
            }

            VisibleEntity entity;
            entity.key = group.entity_name;
            entity.label =
                QStringLiteral("%1\n%2 nodes")
                    .arg(group.label)
                    .arg(static_cast<qulonglong>(group.members.size()));
            entity.members = group.members;
            entity.group_id = groupStateId(group.state_key);
            entity.stable_group_key = group.key;
            entity.group = true;
            entity.order = model.nodes.size();
            entity.source_column = std::numeric_limits<int>::max();
            std::set<std::string, std::less<>> sources;
            for (const auto &member : group.members) {
                const FramePlanNode &node = *nodes_by_name.at(member);
                entity.order = std::min(entity.order, node.order);
                sources.insert(node.source);
            }
            for (const auto &member : visible_members) {
                entity.source_column =
                    std::min(entity.source_column, node_columns.at(member));
                entity_for_node.emplace(member, entity.key);
            }
            entity.source =
                sources.size() == 1 ? *sources.begin() : std::string{"mixed"};
            entities.emplace(entity.key, std::move(entity));
        }
    }

    for (const auto &node : model.nodes) {
        if (!visible_names.contains(node.name) ||
            entity_for_node.contains(node.name)) {
            continue;
        }
        std::string group_id;
        if (const auto found = group_for_node.find(node.name);
            found != group_for_node.end()) {
            group_id = groupStateId(found->second->state_key);
        }
        entity_for_node.emplace(node.name, node.name);
        entities.emplace(
            node.name,
            VisibleEntity{
                .key = node.name,
                .label = qtext(node.name),
                .source = node.source,
                .members = {node.name},
                .group_id = std::move(group_id),
                .order = node.order,
                .source_column = node_columns.at(node.name),
                .anchor = isAnchor(node),
            });
    }

    std::map<std::string, std::string, std::less<>>
        boundary_entity_for_node;
    if (scope_group) {
        struct BoundaryInfo {
            bool incoming = false;
            bool outgoing = false;
        };
        std::map<std::string, BoundaryInfo, std::less<>> boundary_info;
        for (const auto &dependency : model.dependencies) {
            const bool from_inside = visible_names.contains(dependency.from);
            const bool to_inside = visible_names.contains(dependency.to);
            if (from_inside == to_inside) {
                continue;
            }
            if (from_inside) {
                boundary_info[dependency.to].outgoing = true;
            } else {
                boundary_info[dependency.from].incoming = true;
            }
        }

        int minimum_column = 0;
        int maximum_column = 0;
        if (!node_columns.empty()) {
            minimum_column =
                std::min_element(node_columns.begin(), node_columns.end(),
                                 [](const auto &left, const auto &right) {
                                     return left.second < right.second;
                                 })
                    ->second;
            maximum_column =
                std::max_element(node_columns.begin(), node_columns.end(),
                                 [](const auto &left, const auto &right) {
                                     return left.second < right.second;
                                 })
                    ->second;
        }
        std::set<std::string, std::less<>> occupied_names;
        for (const auto &[name, node] : nodes_by_name) {
            (void)node;
            occupied_names.insert(name);
        }
        for (const auto &[key, entity] : entities) {
            (void)entity;
            occupied_names.insert(key);
        }

        for (const auto &[outside, info] : boundary_info) {
            const std::string entity_name = uniqueSyntheticName(
                "__pelican_boundary__:" + outside, occupied_names);
            boundary_entity_for_node.emplace(outside, entity_name);
            const QString direction =
                info.incoming && info.outgoing
                    ? QStringLiteral("incoming,outgoing")
                    : info.incoming ? QStringLiteral("incoming")
                                    : QStringLiteral("outgoing");
            const QString label =
                info.incoming && info.outgoing
                    ? QStringLiteral("External: %1").arg(qtext(outside))
                    : info.incoming
                          ? QStringLiteral("From: %1").arg(qtext(outside))
                          : QStringLiteral("To: %1").arg(qtext(outside));
            const auto outside_node = nodes_by_name.find(outside);
            entities.emplace(
                entity_name,
                VisibleEntity{
                    .key = entity_name,
                    .label = label,
                    .source =
                        outside_node == nodes_by_name.end()
                            ? std::string{"external"}
                            : outside_node->second->source,
                    .boundary_target = outside,
                    .boundary_direction = direction,
                    .order =
                        outside_node == nodes_by_name.end()
                            ? model.nodes.size()
                            : outside_node->second->order,
                    .source_column =
                        info.incoming ? minimum_column - 1
                                      : maximum_column + 1,
                    .boundary_stub = true,
                });
        }
    }

    const std::vector<DependencyRecord> sorted_dependencies =
        dependencyRecords(model);

    std::set<std::size_t> outside_barriers;
    for (std::size_t index = 0; index < model.barriers.size(); ++index) {
        const FramePlanBarrier &barrier = model.barriers[index];
        const bool from_inside = visible_names.contains(barrier.from);
        const bool to_inside = visible_names.contains(barrier.to);
        const bool belongs_to_window =
            scope_group ? from_inside || to_inside
                        : from_inside && to_inside;
        if (!belongs_to_window) {
            outside_barriers.insert(index);
        }
    }

    std::vector<DependencyRecord> visible_dependencies;
    std::map<EntityPair, std::vector<DependencyRecord>> bundles;
    std::set<std::size_t> edge_barriers;
    std::set<std::size_t> internal_barriers;
    for (const auto &dependency : sorted_dependencies) {
        std::string from;
        std::string to;
        if (scope_group) {
            const bool from_inside = visible_names.contains(dependency.from);
            const bool to_inside = visible_names.contains(dependency.to);
            if (!from_inside && !to_inside) {
                continue;
            }
            from = from_inside
                       ? entity_for_node.at(dependency.from)
                       : boundary_entity_for_node.at(dependency.from);
            to = to_inside ? entity_for_node.at(dependency.to)
                           : boundary_entity_for_node.at(dependency.to);
        } else {
            if (!visible_names.contains(dependency.from) ||
                !visible_names.contains(dependency.to)) {
                continue;
            }
            from = entity_for_node.at(dependency.from);
            to = entity_for_node.at(dependency.to);
        }
        visible_dependencies.push_back(dependency);
        if (from == to) {
            entities.at(from).internal_records.push_back(
                recordIdentity(dependency));
            if (dependency.barrier_index) {
                internal_barriers.insert(*dependency.barrier_index);
                ++entities.at(from).internal_barrier_count;
                entities.at(from).internal_fused_barrier_count +=
                    static_cast<std::size_t>(
                        dependency.fused_scope_absorbed);
            }
            continue;
        }
        if (dependency.barrier_index) {
            edge_barriers.insert(*dependency.barrier_index);
        }
        bundles[{std::move(from), std::move(to)}].push_back(dependency);
    }

    const std::size_t classified_barriers =
        edge_barriers.size() + internal_barriers.size() +
        outside_barriers.size();
    const BarrierCoverage barrier_coverage{
        .total = model.barriers.size(),
        .on_edges = edge_barriers.size(),
        .inside_collapsed_groups = internal_barriers.size(),
        .outside_window = outside_barriers.size(),
        .unmatched = classified_barriers <= model.barriers.size()
                         ? model.barriers.size() - classified_barriers
                         : 0,
        .execution_available = true,
        .target_selected = selected_resource_.has_value(),
    };

    std::map<int, std::vector<VisibleEntity *>> levels;
    for (auto &[key, entity] : entities) {
        (void)key;
        levels[entity.source_column].push_back(&entity);
    }
    for (auto &[level, level_entities] : levels) {
        (void)level;
        std::ranges::sort(level_entities, {}, [](const VisibleEntity *entity) {
            return std::pair{entity->order, entity->key};
        });
    }

    std::map<std::string, QGraphicsPathItem *, std::less<>> node_items;
    std::size_t displayed_column = 0;
    qreal column_x = 0.0;
    for (const auto &[level, level_entities] : levels) {
        (void)level;
        static constexpr std::array<qreal, 4> WavePhases{0.0, 1.0, 2.0,
                                                         1.0};
        const qreal wave =
            WavePhases[displayed_column % WavePhases.size()] * StaggerOffset;
        qreal row_y = wave;
        qreal widest_entity = 0.0;
        for (VisibleEntity *entity : level_entities) {
            const qreal width =
                entity->group
                    ? GroupWidth
                    : entity->boundary_stub ? BoundaryStubWidth : NodeWidth;
            const qreal height =
                entity->group
                    ? GroupHeight
                    : entity->boundary_stub ? BoundaryStubHeight : NodeHeight;
            widest_entity = std::max(widest_entity, width);
            const std::string position_kind =
                entity->group ? FramePlanGroupItem : FramePlanNodeItem;
            const std::string stable_identity =
                entity->group ? entity->stable_group_key : entity->key;
            const auto position_key = std::tuple{
                model.graph, position_kind, stable_identity};
            const FramePlanNodeKey routing_key{model.graph, entity->key};
            // x always grows with the topological column, so every edge
            // points left to right for expanded nodes. A collapsed group's
            // source column is the minimum source column of its visible
            // members, the explicit WP341 quotient-layout rule.
            QPointF position{column_x, row_y};
            if (!entity->boundary_stub) {
                if (const auto retained =
                        session_node_positions_.find(position_key);
                    retained != session_node_positions_.end()) {
                    position = retained->second;
                }
            }

            QGraphicsPathItem *item = nullptr;
            if (entity->boundary_stub) {
                item = new QGraphicsPathItem{
                    roundedEntityPath(BoundaryStubWidth, BoundaryStubHeight,
                                      9.0)};
            } else {
                const QPainterPath path =
                    entity->group
                        ? roundedEntityPath(GroupWidth, GroupHeight, 13.0)
                        : nodePath(entity->anchor);
                item = new MovableNodeItem(
                    path, [this, position_key,
                           routing_key](const QPointF &moved_position) {
                    if (rebuilding_) {
                        return;
                    }
                    session_node_positions_[position_key] = moved_position;
                    rerouteConnectedBundles(*this, routing_key.graph,
                                            routing_key.name);
                    // Growing the scene rect from inside itemChange re-enters
                    // this handler through Qt's view update, which recurses
                    // until the stack is exhausted. Defer it to the event
                    // loop, coalescing the requests made during one drag.
                    scheduleSceneRectUpdate();
                });
            }
            addItem(item);
            entity->item = item;
            item->setPos(position);
            item->setZValue(2.0);
            const QColor fill =
                entity->group
                    ? QColor{QStringLiteral("#7653a6")}
                    : entity->boundary_stub
                          ? QColor{QStringLiteral("#34404b")}
                          : sourceColor(entity->source);
            item->setBrush(fill);
            QPen outline{entity->anchor
                             ? QColor{QStringLiteral("#d8e3ec")}
                             : entity->boundary_stub
                                   ? QColor{QStringLiteral("#aeb9c3")}
                                   : QColor{QStringLiteral("#edf2f6")}};
            outline.setWidthF(entity->group ? 2.0 : 1.3);
            if (entity->anchor || entity->boundary_stub) {
                outline.setStyle(Qt::DashLine);
            }
            item->setPen(outline);
            if (!entity->boundary_stub) {
                item->setFlag(QGraphicsItem::ItemIsMovable, true);
                item->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
            }
            item->setFlag(QGraphicsItem::ItemIsSelectable,
                          !entity->group && !entity->boundary_stub);
            const std::string kind =
                entity->group
                    ? FramePlanGroupItem
                    : entity->boundary_stub ? FramePlanBoundaryStubItem
                                            : FramePlanNodeItem;
            annotateIdentity(*item, kind, model.graph, entity->key);
            item->setData(FramePlanSourceRole, qtext(entity->source));
            item->setData(FramePlanColorRole, fill.name(QColor::HexRgb));
            item->setData(FramePlanAnchorRole, entity->anchor);
            item->setData(FramePlanMembersRole, qlist(entity->members));
            item->setData(FramePlanSubtreeDepthRole, normalized_depth);
            if (entity->group) {
                QString visible_label = entity->label;
                if (entity->internal_barrier_count != 0) {
                    visible_label +=
                        QStringLiteral("\ninternal barriers: %1")
                            .arg(static_cast<qulonglong>(
                                entity->internal_barrier_count));
                    if (entity->internal_fused_barrier_count != 0) {
                        visible_label +=
                            QStringLiteral(" · absorbed in fused scope: %1")
                                .arg(static_cast<qulonglong>(
                                    entity->internal_fused_barrier_count));
                    }
                }
                item->setData(FramePlanGroupIdRole,
                              qtext(entity->group_id));
                item->setData(FramePlanGroupCollapsibleRole, true);
                item->setData(FramePlanInternalEdgeRecordsRole,
                              qlist(entity->internal_records));
                item->setData(
                    FramePlanInternalBarrierCountRole,
                    static_cast<qulonglong>(
                        entity->internal_barrier_count));
                item->setData(
                    FramePlanInternalFusedBarrierCountRole,
                    static_cast<qulonglong>(
                        entity->internal_fused_barrier_count));
                item->setToolTip(
                    QStringLiteral(
                        "%1\nDouble-click to enter this group. "
                        "Right-click to expand it.")
                        .arg(visible_label));
                addCenteredEntityLabel(
                    *item, visible_label, GroupWidth, GroupHeight,
                    FramePlanGroupLabelItem, model.graph, entity->key);
            } else if (entity->boundary_stub) {
                item->setData(FramePlanBoundaryTargetRole,
                              qtext(entity->boundary_target));
                item->setData(FramePlanBoundaryDirectionRole,
                              entity->boundary_direction);
                item->setToolTip(
                    QStringLiteral("%1 boundary connection to %2")
                        .arg(entity->boundary_direction,
                             qtext(entity->boundary_target)));
                addCenteredEntityLabel(
                    *item, entity->label, BoundaryStubWidth,
                    BoundaryStubHeight, FramePlanBoundaryStubLabelItem,
                    model.graph, entity->key);
            } else {
                const FramePlanNode &node =
                    *nodes_by_name.at(entity->key);
                const auto node_group = group_for_node.find(node.name);
                if (node_group != group_for_node.end()) {
                    item->setData(FramePlanGroupIdRole,
                                  qtext(groupStateId(
                                      node_group->second->state_key)));
                    item->setData(FramePlanGroupCollapsibleRole,
                                  node_group->second->collapsible);
                    item->setData(FramePlanReasonRole,
                                  node_group->second->reason);
                }
                QString tooltip =
                    QStringLiteral("%1 / %2")
                        .arg(qtext(model.graph), qtext(node.name));
                if (node_group != group_for_node.end()) {
                    tooltip += node_group->second->collapsible
                                   ? QStringLiteral(
                                         "\nRight-click to collapse %1.")
                                         .arg(node_group->second->label)
                                   : QStringLiteral("\n%1").arg(
                                         node_group->second->reason);
                }
                const QString legacy_notice =
                    ignoredLegacyRegionNotice(model, node);
                if (!legacy_notice.isEmpty()) {
                    tooltip += QStringLiteral("\n%1").arg(legacy_notice);
                }
                item->setToolTip(tooltip);
                addNodeLabel(*item, node, model.graph);
                node_items.emplace(node.name, item);
            }
            row_y += height + VerticalGap;
        }
        column_x += widest_entity + HorizontalGap;
        ++displayed_column;
    }

    std::size_t bundle_order = 0;
    std::size_t resource_overlay_count = 0;
    for (const auto &[endpoints, records] : bundles) {
        const DependencySummary summary = dependencySummary(records);
        const bool order_only = summary.barrier_count == 0;
        const bool has_fused_barrier = summary.fused_barrier_count != 0;
        const QColor edge_color =
            order_only
                ? QColor{QStringLiteral("#7d8994")}
                : has_fused_barrier
                      ? QColor{QStringLiteral("#8d72bd")}
                      : QColor{QStringLiteral("#3f83b5")};
        const QColor label_outline =
            order_only
                ? QColor{QStringLiteral("#9aa7b4")}
                : has_fused_barrier
                      ? QColor{QStringLiteral("#8d72bd")}
                      : QColor{QStringLiteral("#4f8fb9")};
        const QColor label_fill =
            order_only
                ? QColor{QStringLiteral("#f5f7f9")}
                : has_fused_barrier
                      ? QColor{QStringLiteral("#f2ecfb")}
                      : QColor{QStringLiteral("#eaf4fb")};

        auto *edge = addPath(QPainterPath{});
        edge->setZValue(0.0);
        QPen edge_pen{edge_color};
        edge_pen.setWidthF(order_only ? 1.4 : 1.9);
        if (order_only) {
            edge_pen.setStyle(Qt::DashLine);
        }
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
        for (const auto &record : records) {
            identities.push_back(recordIdentity(record));
        }
        const std::vector<std::string> resources{
            summary.resources.begin(), summary.resources.end()};
        const std::vector<std::string> barrier_kinds{
            summary.barrier_kinds.begin(), summary.barrier_kinds.end()};
        const auto publish_dependency_data = [&](QGraphicsItem &item) {
            item.setData(FramePlanEdgeRecordsRole, qlist(identities));
            item.setData(FramePlanResourcesRole, qlist(resources));
            item.setData(FramePlanBarrierKindsRole,
                         qlist(barrier_kinds));
            item.setData(FramePlanBarrierCountRole,
                         static_cast<qulonglong>(summary.barrier_count));
            item.setData(FramePlanOrderOnlyCountRole,
                         static_cast<qulonglong>(summary.order_only_count));
            item.setData(
                FramePlanSamePixelAttachmentCountRole,
                static_cast<qulonglong>(
                    summary.same_pixel_attachment_count));
            item.setData(FramePlanFusedBarrierCountRole,
                         static_cast<qulonglong>(
                             summary.fused_barrier_count));
            item.setToolTip(summary.label);
        };
        publish_dependency_data(*edge);
        if (!summary.resources.empty()) {
            ++resource_overlay_count;
        }

        auto *arrow = addPolygon(
            QPolygonF{}, QPen{edge_color}, QBrush{edge_color});
        arrow->setZValue(0.5);
        annotateIdentity(*arrow, FramePlanEdgeArrowItem, model.graph,
                         endpoints.first + "->" + endpoints.second);
        annotateEndpoints(*arrow, endpoints);
        arrow->setData(FramePlanBundleOrderRole,
                       static_cast<qulonglong>(bundle_order));
        publish_dependency_data(*arrow);

        const QString &label_text = summary.label;
        QFont label_font;
        label_font.setPointSizeF(8.5);
        const qreal label_width = std::max(
            EdgeLabelMinimumWidth,
            QFontMetricsF{label_font}.horizontalAdvance(label_text) + 14.0);
        auto *label_box = addRect(
            QRectF{0.0, 0.0, label_width, EdgeLabelHeight},
            QPen{label_outline}, QBrush{label_fill});
        label_box->setZValue(1.0);
        annotateIdentity(*label_box, FramePlanEdgeLabelItem, model.graph,
                         endpoints.first + "->" + endpoints.second);
        annotateEndpoints(*label_box, endpoints);
        label_box->setData(FramePlanBundleOrderRole,
                           static_cast<qulonglong>(bundle_order));
        publish_dependency_data(*label_box);
        auto *label = new QGraphicsSimpleTextItem(label_text, label_box);
        label->setFont(label_font);
        label->setBrush(QColor{QStringLiteral("#263441")});
        annotateEndpoints(*label, endpoints);
        publish_dependency_data(*label);
        const QRectF text_bounds = label->boundingRect();
        label->setPos((label_width - text_bounds.width()) / 2.0,
                      (EdgeLabelHeight - text_bounds.height()) / 2.0);

        routeLogicalBundle(*this, *edge);
        ++bundle_order;
    }

    if (!scope_group) {
        qreal warning_top =
            items().empty() ? 0.0 : itemsBoundingRect().bottom() + 20.0;
        for (const auto &group : groups) {
            if (group.collapsible ||
                std::ranges::none_of(group.members, [&](const auto &member) {
                    return visible_names.contains(member);
                })) {
                continue;
            }
            const qreal warning_width = std::max(
                GroupWarningWidth,
                QFontMetricsF{QFont{}}.horizontalAdvance(group.reason) +
                    24.0);
            auto *warning = addRect(
                QRectF{0.0, 0.0, warning_width, GroupWarningHeight},
                QPen{QColor{QStringLiteral("#d68a32")}, 1.6},
                QBrush{QColor{QStringLiteral("#2b2118")}});
            warning->setPos(0.0, warning_top);
            warning->setZValue(1.2);
            annotateIdentity(*warning, FramePlanGroupWarningItem,
                             model.graph,
                             groupStateId(group.state_key));
            warning->setData(FramePlanGroupIdRole,
                             qtext(groupStateId(group.state_key)));
            warning->setData(FramePlanGroupCollapsibleRole, false);
            warning->setData(FramePlanReasonRole, group.reason);
            warning->setToolTip(group.reason);
            auto *message =
                new QGraphicsSimpleTextItem(group.reason, warning);
            message->setBrush(QColor{QStringLiteral("#f6d09a")});
            message->setPos(12.0, 14.0);
            warning_top += GroupWarningHeight + 10.0;
        }
    }

    if (scope_group) {
        const QString breadcrumb_text =
            QStringLiteral("Frame plan  /  %1  (click to return)")
                .arg(scope_group->label);
        QFont breadcrumb_font;
        breadcrumb_font.setBold(true);
        const qreal breadcrumb_width =
            QFontMetricsF{breadcrumb_font}.horizontalAdvance(
                breadcrumb_text) +
            28.0;
        const QRectF current_bounds = itemsBoundingRect();
        auto *breadcrumb = new BreadcrumbItem(
            QRectF{0.0, 0.0, breadcrumb_width, BreadcrumbHeight},
            [this] {
                QMetaObject::invokeMethod(
                    this, [this] { leaveGroup(); },
                    Qt::QueuedConnection);
            });
        addItem(breadcrumb);
        breadcrumb->setPos(
            current_bounds.isValid() ? current_bounds.left() : 0.0,
            current_bounds.isValid()
                ? current_bounds.top() - BreadcrumbHeight - 18.0
                : 0.0);
        breadcrumb->setZValue(3.0);
        breadcrumb->setPen(
            QPen{QColor{QStringLiteral("#8d72bd")}, 1.5});
        breadcrumb->setBrush(QColor{QStringLiteral("#302641")});
        breadcrumb->setAcceptedMouseButtons(Qt::LeftButton);
        annotateIdentity(*breadcrumb, FramePlanBreadcrumbItem,
                         model.graph,
                         groupStateId(scope_group->state_key));
        breadcrumb->setData(FramePlanGroupIdRole,
                            qtext(groupStateId(scope_group->state_key)));
        breadcrumb->setToolTip(
            QStringLiteral("Return to the outer frame-plan graph"));
        auto *label =
            new QGraphicsSimpleTextItem(breadcrumb_text, breadcrumb);
        label->setFont(breadcrumb_font);
        label->setBrush(QColor{QStringLiteral("#f3ecff")});
        label->setPos(14.0, 7.0);
        label->setAcceptedMouseButtons(Qt::NoButton);
    }

    const qreal logical_bottom = items().empty() ? 0.0
                                                 : itemsBoundingRect().bottom();
    const PhysicalOverlaySummary physical_overlay = addPhysicalOverlay(
        *this, model, logical_bottom, selected_resource_);

    for (const auto &selected_node : selected_nodes_) {
        if (const auto found = node_items.find(selected_node.name);
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
                static_cast<qulonglong>(visible_dependencies.size()));
    setProperty("pelicanVisibleItemCount",
                static_cast<qulonglong>(entities.size()));
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
    setProperty("pelicanBoundaryStubCount",
                static_cast<qulonglong>(
                    boundary_entity_for_node.size()));
    setProperty("pelicanExecutionPlanState", QStringLiteral("available"));
    setProperty("pelicanExecutionPlanReasonCode", QString{});
    setProperty("pelicanExecutionPlanReason", QString{});
    publishBarrierCoverage(*this, barrier_coverage);
    publish_group_properties();
    publishStateProperties();
}

bool FramePlanGraphicsScene::collapseGroup(const QString &group_id) {
    if (!current_model_ || current_group_scope_) {
        return false;
    }
    const auto groups = discoverGroups(*current_model_);
    const GroupDefinition *group =
        findGroupById(groups, group_id);
    if (group == nullptr) {
        return false;
    }
    if (!group->collapsible) {
        group_feedback_ = group->reason;
        setProperty("pelicanGroupFeedback", group->reason);
        return false;
    }
    if (!collapsed_groups_.insert(group->state_key).second) {
        return false;
    }
    group_feedback_.clear();
    renderCurrentGraph();
    return true;
}

bool FramePlanGraphicsScene::expandGroup(const QString &group_id) {
    if (!current_model_ || current_group_scope_) {
        return false;
    }
    const auto groups = discoverGroups(*current_model_);
    const GroupDefinition *group = findGroupById(groups, group_id);
    if (group == nullptr ||
        collapsed_groups_.erase(group->state_key) == 0) {
        return false;
    }
    group_feedback_.clear();
    renderCurrentGraph();
    return true;
}

bool FramePlanGraphicsScene::enterGroup(const QString &group_id) {
    if (!current_model_ || current_group_scope_) {
        return false;
    }
    const auto groups = discoverGroups(*current_model_);
    const GroupDefinition *group =
        findGroupById(groups, group_id);
    if (group == nullptr || !group->collapsible ||
        !collapsed_groups_.contains(group->state_key)) {
        return false;
    }
    current_group_scope_ = group->state_key;
    selected_node_.reset();
    selected_nodes_.clear();
    group_feedback_.clear();
    renderCurrentGraph();
    return true;
}

bool FramePlanGraphicsScene::leaveGroup() {
    if (!current_model_ || !current_group_scope_) {
        return false;
    }
    current_group_scope_.reset();
    selected_node_.reset();
    selected_nodes_.clear();
    group_feedback_.clear();
    renderCurrentGraph();
    return true;
}

void FramePlanGraphicsScene::contextMenuEvent(
    QGraphicsSceneContextMenuEvent *event) {
    QGraphicsItem *semantic_item = itemAt(event->scenePos(), QTransform{});
    while (semantic_item != nullptr) {
        const QString kind = itemKind(*semantic_item);
        if (kind == QLatin1String{FramePlanNodeItem} ||
            kind == QLatin1String{FramePlanGroupItem}) {
            break;
        }
        semantic_item = semantic_item->parentItem();
    }
    if (semantic_item == nullptr || current_group_scope_) {
        QGraphicsScene::contextMenuEvent(event);
        return;
    }

    const QString kind = itemKind(*semantic_item);
    const QString group_id =
        semantic_item->data(FramePlanGroupIdRole).toString();
    if (group_id.isEmpty()) {
        QGraphicsScene::contextMenuEvent(event);
        return;
    }

    QMenu menu;
    if (kind == QLatin1String{FramePlanGroupItem}) {
        QAction *expand = menu.addAction(QStringLiteral("Expand group"));
        connect(expand, &QAction::triggered, this,
                [this, group_id] {
                    QMetaObject::invokeMethod(
                        this,
                        [this, group_id] { expandGroup(group_id); },
                        Qt::QueuedConnection);
                });
    } else if (semantic_item
                   ->data(FramePlanGroupCollapsibleRole)
                   .toBool()) {
        QAction *collapse =
            menu.addAction(QStringLiteral("Collapse group"));
        connect(collapse, &QAction::triggered, this,
                [this, group_id] {
                    QMetaObject::invokeMethod(
                        this,
                        [this, group_id] { collapseGroup(group_id); },
                        Qt::QueuedConnection);
                });
    } else {
        QString reason =
            semantic_item->data(FramePlanReasonRole).toString();
        if (reason.isEmpty()) {
            reason = QStringLiteral("This group cannot be collapsed.");
        }
        QAction *disabled = menu.addAction(reason);
        disabled->setEnabled(false);
    }
    event->accept();
    menu.exec(event->screenPos());
}

void FramePlanGraphicsScene::mouseDoubleClickEvent(
    QGraphicsSceneMouseEvent *event) {
    QGraphicsItem *semantic_item = itemAt(event->scenePos(), QTransform{});
    while (semantic_item != nullptr &&
           itemKind(*semantic_item) !=
               QLatin1String{FramePlanGroupItem}) {
        semantic_item = semantic_item->parentItem();
    }
    if (semantic_item == nullptr) {
        QGraphicsScene::mouseDoubleClickEvent(event);
        return;
    }

    const QString group_id =
        semantic_item->data(FramePlanGroupIdRole).toString();
    if (group_id.isEmpty()) {
        QGraphicsScene::mouseDoubleClickEvent(event);
        return;
    }
    event->accept();
    QMetaObject::invokeMethod(
        this, [this, group_id] { enterGroup(group_id); },
        Qt::QueuedConnection);
}

void FramePlanGraphicsScene::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape && current_group_scope_) {
        event->accept();
        QMetaObject::invokeMethod(
            this, [this] { leaveGroup(); }, Qt::QueuedConnection);
        return;
    }
    QGraphicsScene::keyPressEvent(event);
}

void FramePlanGraphicsScene::applySceneRectNow() {
    scene_rect_update_queued_ = false;
    setSceneRect(itemsBoundingRect().adjusted(-30.0, -30.0, 30.0, 30.0));
}

void FramePlanGraphicsScene::scheduleSceneRectUpdate() {
    if (scene_rect_update_queued_) {
        return;
    }
    scene_rect_update_queued_ = true;
    QMetaObject::invokeMethod(
        this, [this] { applySceneRectNow(); }, Qt::QueuedConnection);
}

void FramePlanGraphicsScene::recordSelection() {
    if (rebuilding_) {
        return;
    }
    std::set<FramePlanNodeKey> current_nodes;
    for (QGraphicsItem *item : selectedItems()) {
        if (itemKind(*item) != QLatin1String{FramePlanNodeItem}) {
            continue;
        }
        current_nodes.insert(FramePlanNodeKey{
            item->data(FramePlanGraphRole).toString().toStdString(),
            item->data(FramePlanNameRole).toString().toStdString(),
        });
    }

    std::optional<FramePlanNodeKey> newly_selected;
    std::size_t newly_selected_count = 0;
    for (const auto &node : current_nodes) {
        if (!selected_nodes_.contains(node)) {
            newly_selected = node;
            ++newly_selected_count;
        }
    }
    selected_nodes_ = std::move(current_nodes);
    if (selected_nodes_.empty()) {
        selected_node_.reset();
    } else if (selected_nodes_.size() == 1) {
        selected_node_ = *selected_nodes_.begin();
    } else if (newly_selected_count == 1) {
        // Ctrl+clicking a node makes that addition the primary selection, so
        // the existing single-node details panel follows the user's click.
        selected_node_ = newly_selected;
    } else if (!selected_node_ ||
               !selected_nodes_.contains(*selected_node_)) {
        // Rubber-band selection has no distinguished click target. Its
        // primary node is deterministic instead of depending on Qt item order.
        selected_node_ = *selected_nodes_.begin();
    }
    publishStateProperties();
}

void FramePlanGraphicsScene::publishStateProperties() {
    QStringList selected_nodes;
    for (const auto &node : selected_nodes_) {
        selected_nodes.push_back(qtext(node.name));
    }
    setProperty("pelicanSelectedGraph",
                selected_node_ ? qtext(selected_node_->graph) : QString{});
    setProperty("pelicanSelectedNode",
                selected_node_ ? qtext(selected_node_->name) : QString{});
    setProperty("pelicanSelectedNodes", selected_nodes);
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
