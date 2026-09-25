#include "frameplangraphics.hpp"

#include "inlineediteventfilter.hpp"
#include "nodelayout.hpp"
#include "studiotheme.hpp"

#include <QAction>
#include <QBrush>
#include <QColor>
#include <QCompleter>
#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsRectItem>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
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
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

constexpr qreal GroupWidth = 260.0;
constexpr qreal GroupHeight = 96.0;
constexpr qreal BoundaryStubWidth = 210.0;
constexpr qreal BoundaryStubHeight = 54.0;
constexpr qreal HorizontalGap = NodeGraphHorizontalGap;
constexpr qreal VerticalGap = NodeGraphVerticalGap;
constexpr qreal StaggerOffset = NodeGraphStaggerOffset;
constexpr qreal GroupWarningWidth = 680.0;
constexpr qreal GroupWarningHeight = 54.0;
constexpr qreal BreadcrumbHeight = NodeGraphBreadcrumbHeight;
constexpr qreal PhysicalPanelGap = 74.0;
constexpr qreal PhysicalPanelMinimumWidth = 760.0;
constexpr qreal PhysicalHeaderHeight = 76.0;
constexpr qreal PhysicalResourceHeight = 92.0;
constexpr qreal PhysicalOutcomeHeight = 34.0;
constexpr qreal LogicalUnavailableWidth = 960.0;
constexpr qreal LogicalUnavailableHeight = 176.0;
constexpr std::size_t ImpracticalConvexHullAdditionalNodes = 24;
constexpr std::size_t ImpracticalConvexHullGraphSize = 16;

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
    bool session_group = false;
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
    bool session_group = false;
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

class ConvexHullProposalItem final : public QGraphicsRectItem {
  public:
    using Accepted = std::function<void()>;

    ConvexHullProposalItem(const QRectF &rect, Accepted accepted)
        : QGraphicsRectItem{rect}, accepted_{std::move(accepted)} {}

  protected:
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && accepted_) {
            event->accept();
            accepted_();
            return;
        }
        QGraphicsRectItem::mousePressEvent(event);
    }

  private:
    Accepted accepted_;
};

QString qtext(const std::string &value) {
    return QString::fromStdString(value);
}

QString itemKind(const QGraphicsItem &item) {
    return item.data(FramePlanItemKindRole).toString();
}

// The item kinds the shared routing has to know about for this scene: which
// items can hold an edge endpoint, and which artifacts follow an edge.
const NodeGraphKinds &framePlanKinds() {
    static const NodeGraphKinds kinds{
        QStringList{QString::fromLatin1(FramePlanNodeItem),
                    QString::fromLatin1(FramePlanGroupItem),
                    QString::fromLatin1(FramePlanBoundaryStubItem)},
        QString::fromLatin1(FramePlanEdgeItem),
        QString::fromLatin1(FramePlanEdgeArrowItem),
        // The edge labels are gone (N-3): the node rows name both ends.
        QString{}};
    return kinds;
}

QGraphicsItem *findFramePlanSocket(QGraphicsScene &scene, const QString &graph,
                                   const QString &node,
                                   const QString &resource,
                                   const QString &direction) {
    for (QGraphicsItem *item : scene.items()) {
        if (item->data(FramePlanItemKindRole).toString() !=
                QLatin1String{FramePlanSocketItem} ||
            item->data(FramePlanGraphRole).toString() != graph ||
            item->data(FramePlanNameRole).toString() != node ||
            item->data(FramePlanPortRole).toString() != resource ||
            item->data(FramePlanSocketDirectionRole).toString() != direction)
            continue;
        return item;
    }
    return nullptr;
}

// VI-3a: the input socket that stands for one position of the pass's `input`.
QGraphicsItem *findFramePlanInputSocket(QGraphicsScene &scene,
                                        const QString &graph,
                                        const QString &node, int input_index) {
    for (QGraphicsItem *item : scene.items()) {
        if (item->data(FramePlanItemKindRole).toString() !=
                QLatin1String{FramePlanSocketItem} ||
            item->data(FramePlanGraphRole).toString() != graph ||
            item->data(FramePlanNameRole).toString() != node ||
            item->data(FramePlanSocketDirectionRole).toString() !=
                QLatin1String{"input"})
            continue;
        const QVariant index = item->data(FramePlanInputIndexRole);
        if (index.isValid() && index.toInt() == input_index) return item;
    }
    return nullptr;
}

// VI-3a: the arrow of the edge that ends on that input.
QGraphicsItem *findFramePlanInputArrow(QGraphicsScene &scene,
                                       const QString &graph,
                                       const QString &from,
                                       const QString &to, int input_index) {
    for (QGraphicsItem *item : scene.items()) {
        if (item->data(FramePlanItemKindRole).toString() !=
                QLatin1String{FramePlanEdgeArrowItem} ||
            item->data(FramePlanGraphRole).toString() != graph ||
            item->data(FramePlanFromNameRole).toString() != from ||
            item->data(FramePlanToNameRole).toString() != to)
            continue;
        const QVariant index = item->data(FramePlanInputIndexRole);
        if (index.isValid() && index.toInt() == input_index) return item;
    }
    return nullptr;
}

// An edge runs socket centre to socket centre, for the resource the bundle is
// about. A bundle whose ends draw no matching socket -- a collapsed group, a
// boundary stub, an ordering dependency that names no resource -- falls back
// to the node boxes, so a missing socket degrades the line rather than losing
// it.
void routeFramePlanBundle(QGraphicsScene &scene, QGraphicsPathItem &edge) {
    const QString graph = edge.data(FramePlanGraphRole).toString();
    const QString from_name = edge.data(FramePlanFromNameRole).toString();
    const QString to_name = edge.data(FramePlanToNameRole).toString();
    QGraphicsItem *from =
        findNodeItem(scene, framePlanKinds(), graph, from_name);
    QGraphicsItem *to = findNodeItem(scene, framePlanKinds(), graph, to_name);
    if (from == nullptr || to == nullptr) {
        return;
    }

    QGraphicsItem *from_socket = nullptr;
    QGraphicsItem *to_socket = nullptr;
    // VI-3a: an edge that ends on one input of a declared pass is routed to
    // that socket, whichever other socket is fed the same resource.
    const QVariant input_index = edge.data(FramePlanInputIndexRole);
    for (const QString &resource :
         edge.data(FramePlanResourcesRole).toStringList()) {
        QGraphicsItem *out = findFramePlanSocket(
            scene, graph, from_name, resource, QStringLiteral("output"));
        QGraphicsItem *in =
            input_index.isValid()
                ? findFramePlanInputSocket(scene, graph, to_name,
                                           input_index.toInt())
                : findFramePlanSocket(scene, graph, to_name, resource,
                                      QStringLiteral("input"));
        if (out != nullptr && in != nullptr) {
            from_socket = out;
            to_socket = in;
            break;
        }
    }

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
            : curveGeometry(from->sceneBoundingRect(),
                            to->sceneBoundingRect());
    edge.setPath(geometry.path);
    QGraphicsItem *artifact =
        input_index.isValid()
            ? findFramePlanInputArrow(scene, graph, from_name, to_name,
                                      input_index.toInt())
            : findEndpointArtifact(scene, framePlanKinds().arrow, graph,
                                   from_name, to_name);
    if (auto *arrow = dynamic_cast<QGraphicsPolygonItem *>(artifact))
        arrow->setPolygon(geometry.arrow);
}

void rerouteFramePlanBundles(QGraphicsScene &scene, const std::string &graph,
                             const std::string &node) {
    const QString expected_graph = qtext(graph);
    const QString expected_node = qtext(node);
    for (QGraphicsItem *item : scene.items()) {
        if (item->data(FramePlanItemKindRole).toString() !=
                QLatin1String{FramePlanEdgeItem} ||
            item->data(FramePlanGraphRole).toString() != expected_graph ||
            (item->data(FramePlanFromNameRole).toString() != expected_node &&
             item->data(FramePlanToNameRole).toString() != expected_node))
            continue;
        if (auto *edge = dynamic_cast<QGraphicsPathItem *>(item)) {
            edge->setData(NodeGraphBendsRole, QVariant{});
            routeFramePlanBundle(scene, *edge);
        }
    }
}

bool isAnchor(const FramePlanNode &node) {
    return node.kind == "anchor" && node.name.starts_with("__anchor_") &&
           node.reads.empty() && node.history_reads.empty() &&
           node.writes.empty();
}

QColor sourceColor(const std::string &source) {
    const StudioTheme &theme = currentStudioTheme();
    if (source == "project") {
        return theme.sourceTint(QStringLiteral("project"));
    }
    if (source.starts_with("feature:")) {
        return theme.sourceTint(QStringLiteral("feature"));
    }
    if (source == "engine") {
        return theme.sourceTint(QStringLiteral("engine"));
    }
    return theme.sourceTint(QStringLiteral("unknown"));
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

struct ConvexHullResult {
    std::vector<std::string> members;
    std::vector<std::string> missing;
    bool impractical = false;
    bool covers_entire_graph = false;
};

// Keep nonConvexReason() above unchanged: it is the exhaustively checked
// convexity decision. The hull is derived from the same reachability rule. A
// node is forced into the hull exactly when the selection can reach it and it
// can reach the selection, i.e. it lies on a leave-and-return path. The
// intersection is already closed under paths, so no iterative approximation
// or alternate convexity predicate is involved.
ConvexHullResult convexHull(
    const FramePlanModel &model,
    const std::set<std::string, std::less<>> &selected) {
    std::map<std::string, std::vector<std::string>, std::less<>> adjacency;
    std::map<std::string, std::vector<std::string>, std::less<>> reverse;
    for (const auto &dependency : model.dependencies) {
        adjacency[dependency.from].push_back(dependency.to);
        reverse[dependency.to].push_back(dependency.from);
    }
    const auto normalize = [](auto &edges) {
        for (auto &[from, destinations] : edges) {
            (void)from;
            std::ranges::sort(destinations);
            destinations.erase(
                std::unique(destinations.begin(), destinations.end()),
                destinations.end());
        }
    };
    normalize(adjacency);
    normalize(reverse);

    const auto reachable = [&](const auto &edges) {
        std::set<std::string, std::less<>> visited = selected;
        std::queue<std::string> frontier;
        for (const auto &member : selected) {
            frontier.push(member);
        }
        while (!frontier.empty()) {
            std::string node = std::move(frontier.front());
            frontier.pop();
            const auto outgoing = edges.find(node);
            if (outgoing == edges.end()) {
                continue;
            }
            for (const auto &destination : outgoing->second) {
                if (visited.insert(destination).second) {
                    frontier.push(destination);
                }
            }
        }
        return visited;
    };

    const auto forward = reachable(adjacency);
    const auto backward = reachable(reverse);
    ConvexHullResult result;
    for (const auto &node : model.nodes) {
        if (!forward.contains(node.name) ||
            !backward.contains(node.name)) {
            continue;
        }
        result.members.push_back(node.name);
        if (!selected.contains(node.name)) {
            result.missing.push_back(node.name);
        }
    }
    std::ranges::sort(result.members);
    std::ranges::sort(result.missing);
    result.covers_entire_graph =
        !model.nodes.empty() && result.members.size() == model.nodes.size();
    const bool dominates_large_graph =
        model.nodes.size() >= ImpracticalConvexHullGraphSize &&
        result.members.size() * 4 >= model.nodes.size() * 3;
    result.impractical =
        result.missing.size() >= ImpracticalConvexHullAdditionalNodes ||
        dominates_large_graph;
    return result;
}

QString convexHullProposalText(const ConvexHullResult &hull,
                               std::size_t graph_node_count) {
    QStringList quoted_missing;
    for (const auto &member : hull.missing) {
        quoted_missing.push_back(
            QStringLiteral("\"%1\"").arg(qtext(member)));
    }
    const QString names =
        quoted_missing.join(QStringLiteral(", "));
    if (!hull.impractical) {
        return QStringLiteral(
                   "Selection is not convex. Include %1 %2 to make it "
                   "convex (%3 additional; %4 total). Click to accept this "
                   "convex hull.")
            .arg(hull.missing.size() == 1 ? QStringLiteral("node")
                                          : QStringLiteral("nodes"),
                 names)
            .arg(static_cast<qulonglong>(hull.missing.size()))
            .arg(static_cast<qulonglong>(hull.members.size()));
    }

    QString scale =
        QStringLiteral(
            "The convex hull is probably impractical: it would add %1 "
            "nodes (%2 total out of %3).")
            .arg(static_cast<qulonglong>(hull.missing.size()))
            .arg(static_cast<qulonglong>(hull.members.size()))
            .arg(static_cast<qulonglong>(graph_node_count));
    if (hull.covers_entire_graph) {
        scale += QStringLiteral(" It covers the entire graph.");
    }
    return QStringLiteral(
               "Selection is not convex. %1 Missing nodes: %2. Click to "
               "accept this convex hull anyway.")
        .arg(scale, names);
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

std::vector<GroupDefinition> discoverGroups(
    const FramePlanModel &model,
    const std::map<std::pair<std::string, std::string>,
                   std::vector<std::string>> &session_groups) {
    std::vector<GroupDefinition> configured = discoverGroups(model);
    std::set<std::string, std::less<>> occupied_names;
    for (const auto &node : model.nodes) {
        occupied_names.insert(node.name);
    }
    for (const auto &group : configured) {
        occupied_names.insert(group.entity_name);
    }

    std::set<std::string, std::less<>> session_members;
    std::vector<GroupDefinition> session_definitions;
    for (const auto &[state_key, stored_members] : session_groups) {
        if (state_key.first != model.graph || stored_members.size() < 2) {
            continue;
        }
        std::vector<std::string> members = stored_members;
        std::ranges::sort(members);
        members.erase(std::unique(members.begin(), members.end()),
                      members.end());
        session_members.insert(members.begin(), members.end());

        std::string number = state_key.second;
        constexpr std::string_view prefix = "session:";
        if (number.starts_with(prefix)) {
            number.erase(0, prefix.size());
        }
        const QString label =
            QStringLiteral("Session group %1").arg(qtext(number));
        const std::set<std::string, std::less<>> member_set{
            members.begin(), members.end()};
        const QString reason = nonConvexReason(
            model,
            QStringLiteral("Session group \"%1\"").arg(qtext(number)),
            member_set);

        const std::string base_name =
            "__pelican_session_group__:" + number;
        std::string entity_name = base_name;
        std::size_t suffix = 2;
        while (occupied_names.contains(entity_name)) {
            entity_name = base_name + "#" + std::to_string(suffix++);
        }
        occupied_names.insert(entity_name);
        session_definitions.push_back(GroupDefinition{
            .key = state_key.second,
            .state_key = state_key,
            .entity_name = std::move(entity_name),
            .label = label,
            .members = std::move(members),
            .collapsible = reason.isEmpty(),
            .session_group = true,
            .reason = reason,
        });
    }

    // Priority is explicit session group > authored region > provider
    // feature. groupingUnit() already establishes the latter two. Suppress a
    // whole lower-priority group on any overlap instead of displaying a
    // misleading partial authored region. Removing the session group reveals
    // the config-derived group unchanged.
    std::erase_if(configured, [&](const GroupDefinition &group) {
        return !group.warning_only &&
               std::ranges::any_of(group.members, [&](const auto &member) {
                   return session_members.contains(member);
               });
    });
    session_definitions.insert(session_definitions.end(), configured.begin(),
                               configured.end());
    return session_definitions;
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
    std::vector<NodeGraphEdgeKey> edges;
    edges.reserve(model.dependencies.size());
    for (const auto &dependency : model.dependencies) {
        edges.push_back(NodeGraphEdgeKey{dependency.from, dependency.to});
    }
    return longestPathColumns(edges, visible);
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

// F-7: the whole frame is every pass, whatever it reads or writes.
std::set<std::string, std::less<>> allNodeNames(const FramePlanModel &model) {
    std::set<std::string, std::less<>> names;
    for (const auto &node : model.nodes) names.insert(node.name);
    return names;
}

// F-7: the declared targets no pass writes. Nothing else on the canvas has an
// output socket for them, so each is drawn as a resource node of its own.
std::vector<const FramePlanResource *> unwrittenResources(
    const FramePlanModel &model) {
    std::set<std::string, std::less<>> written;
    for (const auto &node : model.nodes)
        written.insert(node.writes.begin(), node.writes.end());
    std::vector<const FramePlanResource *> unwritten;
    for (const auto &resource : model.resources)
        if (!written.contains(resource.name)) unwritten.push_back(&resource);
    return unwritten;
}

QFont framePlanHeadingFont() {
    QFont font;
    font.setBold(true);
    return font;
}

QFont framePlanRowFont() {
    QFont font;
    font.setPointSizeF(font.pointSizeF() - 0.5);
    return font;
}

// One line of a node's output or input section: the resource the pass writes
// or reads, and the socket its edges attach to.
struct DrawnRow {
    QString resource;
    QString type;
    SocketStyle style;
    bool history = false;
    // VI-3a: a row drawn from the shader's declarations. `label` is its text,
    // `key` the port half of its name; `empty` draws the socket hollow.
    bool declared = false;
    QString label;
    QString key;
    std::optional<std::size_t> input_index;
    std::optional<std::size_t> binding;
    bool empty = false;
    bool removed = false;
    QString warning;
    // VI-3b: the input engine://passthrough shows.
    bool shown = false;
};

struct NodeBodyLayout {
    qreal width = FramePlanNodeMinimumWidth;
    qreal height = FramePlanHeaderHeight;
    std::vector<DrawnRow> outputs;
    std::vector<DrawnRow> inputs;
    // N-8: the retypeable fields, between the heading and the sockets.
    std::vector<FramePlanInlineBand> bands;
    // VI-3a: the rows of the variadic group (its members and its drop end),
    // as [first, last] indices into `inputs`.
    std::optional<std::pair<std::size_t, std::size_t>> variadic_rows;
};

DrawnRow framePlanRow(const FramePlanModel &model,
                      const std::string &resource, bool history) {
    const SocketTypeFacts facts =
        framePlanSocketFacts(model, resource, history);
    return DrawnRow{qtext(resource), qtext(facts.type),
                    socketStyleFor(facts), history};
}

// VI-3a: one row of a node whose input sockets come from its shader.
DrawnRow declaredRow(const FramePlanModel &model,
                     const FramePlanInputSocket &socket) {
    DrawnRow row;
    if (socket.connected()) {
        row = framePlanRow(model, socket.resource, socket.history);
    } else {
        // An empty declaration: the kind is the declaration's, not a
        // resource's, and the socket is drawn hollow.
        SocketTypeFacts facts;
        facts.type = socket.buffer ? "frame_plan.buffer" : "frame_plan.image";
        row.type = qtext(facts.type);
        row.style = socketStyleFor(facts);
        row.empty = true;
    }
    row.declared = true;
    row.label = qtext(socket.label);
    row.key = qtext(socket.key());
    row.input_index = socket.input_index;
    row.binding = socket.binding;
    row.removed = socket.removed;
    row.warning = qtext(socket.warning);
    row.shown = socket.shown;
    return row;
}

NodeBodyLayout nodeBodyLayout(const FramePlanModel &model,
                              const FramePlanNode &node,
                              std::vector<FramePlanInlineBand> bands = {},
                              const FramePlanInputSockets *sockets = nullptr) {
    NodeBodyLayout layout;
    layout.bands = std::move(bands);
    // Sorted, not in declaration order: WP318 fixes that the scene ignores the
    // order of every input array, and a row's y position is scene geometry.
    std::vector<std::string> written{node.writes.begin(), node.writes.end()};
    std::ranges::sort(written);
    for (const auto &resource : written)
        layout.outputs.push_back(framePlanRow(model, resource, false));
    if (sockets != nullptr && sockets->declared) {
        // VI-3a: binding order, as the shader declares it -- the one place
        // the order of the input array is scene geometry, because the order
        // is the binding.
        for (const auto &socket : sockets->sockets) {
            if (socket.kind == FramePlanInputSocket::Kind::variadic ||
                socket.kind == FramePlanInputSocket::Kind::variadic_drop) {
                const std::size_t index = layout.inputs.size();
                if (!layout.variadic_rows)
                    layout.variadic_rows = std::pair{index, index};
                else
                    layout.variadic_rows->second = index;
            }
            layout.inputs.push_back(declaredRow(model, socket));
        }
    } else {
        std::vector<std::string> read{node.reads.begin(), node.reads.end()};
        std::ranges::sort(read);
        for (const auto &resource : read)
            layout.inputs.push_back(framePlanRow(model, resource, false));
        std::vector<std::string> history{node.history_reads.begin(),
                                         node.history_reads.end()};
        std::ranges::sort(history);
        for (const auto &resource : history)
            layout.inputs.push_back(framePlanRow(model, resource, true));
    }

    const QFontMetricsF heading_metrics{framePlanHeadingFont()};
    const QFontMetricsF row_metrics{framePlanRowFont()};
    const QString heading = qtext(node.name);
    const QString kind = qtext(node.kind);
    layout.width = std::max<qreal>(
        FramePlanNodeMinimumWidth,
        14.0 + heading_metrics.horizontalAdvance(heading) + 14.0 +
            row_metrics.horizontalAdvance(kind) + 12.0);
    const auto widen = [&](const DrawnRow &row) {
        layout.width = std::max<qreal>(
            layout.width,
            row_metrics.horizontalAdvance(row.declared ? row.label
                                                       : row.resource) +
                40.0);
    };
    for (const auto &row : layout.outputs) widen(row);
    for (const auto &row : layout.inputs) widen(row);
    for (const auto &band : layout.bands)
        layout.width = std::max<qreal>(
            layout.width,
            row_metrics.horizontalAdvance(band.label + ": " + band.value) +
                34.0);
    layout.width = std::min(layout.width, FramePlanNodeMaximumWidth);

    const auto rows = static_cast<qreal>(layout.outputs.size() +
                                         layout.inputs.size() +
                                         layout.bands.size());
    // The GPU-timing line keeps its own row: it is published for every node,
    // including an anchor that has no resource of its own.
    layout.height = FramePlanHeaderHeight + FramePlanSectionPadding +
                    FramePlanSocketRowHeight +
                    rows * FramePlanSocketRowHeight + FramePlanSectionPadding;
    return layout;
}

// The heading band shares the node's rounded top corners and squares off where
// the socket sections begin.
QPainterPath framePlanHeaderPath(qreal width) {
    QPainterPath path;
    const qreal radius = FramePlanNodeCornerRadius;
    path.moveTo(0.0, FramePlanHeaderHeight);
    path.lineTo(0.0, radius);
    path.quadTo(0.0, 0.0, radius, 0.0);
    path.lineTo(width - radius, 0.0);
    path.quadTo(width, 0.0, width, radius);
    path.lineTo(width, FramePlanHeaderHeight);
    path.closeSubpath();
    return path;
}

void applyGpuTimingLabel(QGraphicsSimpleTextItem &label,
                         const FramePlanGpuTimingSnapshot &snapshot,
                         std::string_view node_name, const QPointF &position) {
    label.setData(FramePlanGpuTimingBodyMsRole, QVariant{});
    label.setData(FramePlanGpuTimingBarriersMsRole, QVariant{});
    label.setData(FramePlanGpuTimingSampleCountRole, QVariant{});
    label.setData(FramePlanGpuTimingFrameCountRole,
                  static_cast<qulonglong>(snapshot.frame_count));
    label.setData(FramePlanGpuTimingWindowSizeRole,
                  static_cast<qulonglong>(snapshot.window_size));
    label.setData(FramePlanReasonRole, qtext(snapshot.reason));

    QString text;
    QString state;
    if (!snapshot.received) {
        state = QStringLiteral("waiting");
        text = QStringLiteral("GPU timing: waiting for RPC");
    } else if (!snapshot.enabled || !snapshot.supported) {
        state = QStringLiteral("unavailable");
        text = QStringLiteral("GPU timing unavailable: %1")
                   .arg(qtext(snapshot.reason));
    } else if (snapshot.nodes.empty()) {
        state = QStringLiteral("waiting");
        text = QStringLiteral("GPU timing: waiting for samples · %1/%2f")
                   .arg(static_cast<qulonglong>(snapshot.frame_count))
                   .arg(static_cast<qulonglong>(snapshot.window_size));
    } else {
        const auto found = std::ranges::find(
            snapshot.nodes, node_name, &FramePlanGpuTimingRow::node_name);
        if (found == snapshot.nodes.end()) {
            state = QStringLiteral("node_not_timed");
            label.setData(FramePlanReasonRole,
                          QStringLiteral("node_not_timed"));
            text = QStringLiteral(
                "GPU timing unavailable: node_not_timed");
        } else {
            label.setData(FramePlanGpuTimingBarriersMsRole,
                          found->barriers_ms);
            label.setData(FramePlanGpuTimingSampleCountRole,
                          static_cast<qulonglong>(found->sample_count));
            if (found->body_supported) {
                state = QStringLiteral("measured");
                label.setData(FramePlanGpuTimingBodyMsRole,
                              found->body_ms);
                text = QStringLiteral(
                           "body %1 | barriers %2 ms · %3/%4f")
                           .arg(found->body_ms, 0, 'f', 3)
                           .arg(found->barriers_ms, 0, 'f', 3)
                           .arg(static_cast<qulonglong>(
                               found->sample_count))
                           .arg(static_cast<qulonglong>(
                               snapshot.window_size));
            } else {
                state = QStringLiteral("body_unsupported");
                label.setData(FramePlanReasonRole,
                              QStringLiteral("no_gpu_work"));
                text = QStringLiteral(
                           "body unsupported | barriers %1 ms · %2/%3f")
                           .arg(found->barriers_ms, 0, 'f', 3)
                           .arg(static_cast<qulonglong>(
                               found->sample_count))
                           .arg(static_cast<qulonglong>(
                               snapshot.window_size));
            }
        }
    }
    label.setData(FramePlanGpuTimingStateRole, state);
    label.setText(text);
    label.setToolTip(text);
    label.setPos(position);
}

// The node the user reads: a heading band carrying the pass name and its kind,
// then one line per written target and one per read resource, each with the
// socket its edges attach to. Shape and colour of the socket are the resource's
// kind, resolved by the one shared resolver.
void addNodeBody(QGraphicsPathItem &item, const FramePlanNode &node,
                 const NodeBodyLayout &layout, const std::string &graph,
                 const QColor &heading_color,
                 const FramePlanGpuTimingSnapshot &gpu_timing) {
    const QFont heading_font = framePlanHeadingFont();
    const QFont row_font = framePlanRowFont();
    const QFontMetricsF heading_metrics{heading_font};
    const QFontMetricsF row_metrics{row_font};
    const QString node_name = qtext(node.name);

    const auto tag = [&](QGraphicsItem *child, const char *kind) {
        child->setData(FramePlanItemKindRole, QString::fromLatin1(kind));
        child->setData(FramePlanGraphRole, qtext(graph));
        child->setData(FramePlanNameRole, node_name);
        // Everything inside the node is decoration: a press on it must reach
        // the node itself, which is what drags and selects.
        child->setAcceptedMouseButtons(Qt::NoButton);
    };

    auto *band =
        new QGraphicsPathItem(framePlanHeaderPath(layout.width), &item);
    band->setBrush(heading_color);
    band->setPen(Qt::NoPen);
    tag(band, FramePlanNodeHeaderItem);

    auto *label = new QGraphicsSimpleTextItem(node_name, band);
    label->setFont(heading_font);
    label->setBrush(currentStudioTheme().canvas.heading_text);
    label->setPos(14.0,
                  (FramePlanHeaderHeight - heading_metrics.height()) / 2.0);
    annotateIdentity(*label, FramePlanNodeLabelItem, graph, node.name);
    label->setAcceptedMouseButtons(Qt::NoButton);

    // The kind stays visible beside the name: an anchor now looks like every
    // other node, so the word is the only thing that says what it is.
    const QString kind_text = qtext(node.kind);
    auto *kind = new QGraphicsSimpleTextItem(kind_text, band);
    kind->setFont(row_font);
    kind->setBrush(currentStudioTheme().canvas.dim_text);
    kind->setOpacity(0.78);
    kind->setPos(layout.width - 12.0 -
                     row_metrics.horizontalAdvance(kind_text),
                 (FramePlanHeaderHeight - row_metrics.height()) / 2.0);
    tag(kind, FramePlanNodeKindLabelItem);

    auto *timing = new QGraphicsSimpleTextItem(&item);
    timing->setBrush(QColor{QStringLiteral("#d6e8b5")});
    QFont timing_font = timing->font();
    timing_font.setPointSizeF(8.0);
    timing->setFont(timing_font);
    timing->setData(FramePlanGpuTimingNodeNameRole, node_name);
    timing->setAcceptedMouseButtons(Qt::NoButton);
    applyGpuTimingLabel(
        *timing, gpu_timing, node.name,
        QPointF{14.0, FramePlanHeaderHeight + FramePlanSectionPadding + 2.0});

    // VI-3a: what a declared row carries on every item that stands for it,
    // so a gesture and a scenario can name the binding rather than the
    // resource.
    const auto declare = [](QGraphicsItem *child, const DrawnRow &row) {
        if (!row.declared) return;
        if (!row.key.isEmpty()) child->setData(FramePlanSocketKeyRole, row.key);
        if (row.input_index)
            child->setData(FramePlanInputIndexRole,
                           static_cast<int>(*row.input_index));
        if (row.binding)
            child->setData(FramePlanSocketBindingRole,
                           static_cast<int>(*row.binding));
        child->setData(FramePlanSocketRemovedRole, row.removed);
        child->setData(FramePlanSocketShownRole, row.shown);
    };
    const QColor removed_color{QStringLiteral("#d94c3d")};
    const auto socket = [&](const DrawnRow &row, const QPointF &centre,
                            const char *direction) {
        auto *dot = new QGraphicsPathItem(
            socketPath(row.style.shape, row.style.size), &item);
        dot->setPos(centre);
        dot->setZValue(1.0);
        if (row.removed) {
            dot->setBrush(removed_color);
            dot->setPen(QPen{removed_color.lighter(140), 1.8});
        } else if (row.empty) {
            // Nothing connected: the outline alone, in the kind's colour.
            dot->setBrush(currentStudioTheme().canvas.node_body);
            dot->setPen(QPen{row.style.fill, 1.6});
        } else if (row.shown) {
            // VI-3b: the input engine://passthrough shows -- filled, and
            // ringed in white so it stands out from the other members.
            dot->setBrush(row.style.fill.lighter(125));
            dot->setPen(QPen{QColor{QStringLiteral("#ffffff")}, 2.2});
        } else {
            dot->setBrush(row.style.fill);
            dot->setPen(QPen{row.style.outline, row.style.outline_width});
        }
        QString tooltip = row.type.isEmpty()
                              ? QStringLiteral("%1 (kind not declared)")
                                    .arg(row.resource)
                              : QStringLiteral("%1 : %2")
                                    .arg(row.resource, row.type);
        if (row.declared) {
            tooltip = row.binding
                          ? QStringLiteral("binding %1: %2")
                                .arg(static_cast<qulonglong>(*row.binding))
                                .arg(row.label)
                          : row.label;
            if (!row.type.isEmpty())
                tooltip += QStringLiteral(" : %1").arg(row.type);
            if (row.removed)
                tooltip += QStringLiteral(
                    "\nThe reloaded shader no longer declares this binding.");
            if (!row.warning.isEmpty())
                tooltip += QStringLiteral("\n%1").arg(row.warning);
            if (row.shown)
                tooltip += QStringLiteral(
                    "\nengine://passthrough shows this input.");
            else if (row.key.startsWith(QStringLiteral("in@")) && !row.empty)
                tooltip += QStringLiteral(
                    "\nOn engine://passthrough, click to show this input.");
        }
        dot->setToolTip(tooltip);
        tag(dot, FramePlanSocketItem);
        dot->setData(FramePlanPortRole, row.resource);
        dot->setData(FramePlanSocketDirectionRole,
                     QString::fromLatin1(direction));
        declare(dot, row);
    };

    qreal cursor_y = FramePlanHeaderHeight + FramePlanSectionPadding +
                     FramePlanSocketRowHeight;
    // N-8: the retypeable fields. They are drawn before the sockets because
    // they are about the node itself rather than about what flows through it,
    // and a click on one opens an editor over exactly this rectangle.
    for (const auto &band : layout.bands) {
        auto *plate = new QGraphicsRectItem(
            QRectF{6.0, cursor_y + 1.0, layout.width - 12.0,
                   FramePlanSocketRowHeight - 2.0},
            &item);
        plate->setBrush(currentStudioTheme().canvas.node_body.lighter(122));
        plate->setPen(QPen{currentStudioTheme().canvas.edge, 0.8});
        tag(plate, FramePlanInlineFieldItem);
        plate->setData(FramePlanInlineFieldRole, band.field);
        plate->setData(FramePlanInlineValueRole, band.value);
        plate->setToolTip(
            QStringLiteral("%1: %2\nClick to retype; Enter saves.")
                .arg(band.label, band.value));
        const QString shown = row_metrics.elidedText(
            band.label + QStringLiteral(": ") + band.value, Qt::ElideMiddle,
            layout.width - 24.0);
        auto *text = new QGraphicsSimpleTextItem(shown, plate);
        text->setFont(row_font);
        text->setBrush(QColor{QStringLiteral("#e4ecf3")});
        text->setPos(12.0, cursor_y + 1.0 +
                               (FramePlanSocketRowHeight - 2.0 -
                                row_metrics.height()) /
                                   2.0);
        text->setAcceptedMouseButtons(Qt::NoButton);
        cursor_y += FramePlanSocketRowHeight;
    }
    for (const auto &row : layout.outputs) {
        const qreal centre_y = cursor_y + FramePlanSocketRowHeight / 2.0;
        const QString text = row_metrics.elidedText(
            row.resource, Qt::ElideMiddle, layout.width - 30.0);
        auto *name = new QGraphicsSimpleTextItem(text, &item);
        name->setFont(row_font);
        name->setBrush(QColor{QStringLiteral("#e4ecf3")});
        name->setPos(layout.width - 14.0 - row_metrics.horizontalAdvance(text),
                     centre_y - row_metrics.height() / 2.0);
        tag(name, FramePlanRowLabelItem);
        name->setData(FramePlanPortRole, row.resource);
        name->setData(FramePlanSocketDirectionRole,
                      QStringLiteral("output"));
        socket(row, QPointF{layout.width, centre_y}, "output");
        cursor_y += FramePlanSocketRowHeight;
    }
    // VI-3a: the variadic group is one long socket -- the socket split in
    // two, with a band between the halves that holds one socket per connected
    // member and ends in an empty one to drop on.
    if (layout.variadic_rows) {
        const auto [first, last] = *layout.variadic_rows;
        const qreal top = cursor_y +
                          static_cast<qreal>(first) * FramePlanSocketRowHeight +
                          2.0;
        const qreal bottom = cursor_y +
                             static_cast<qreal>(last + 1) *
                                 FramePlanSocketRowHeight -
                             2.0;
        const qreal half = FramePlanSocketSize / 2.0 + 2.0;
        QPainterPath capsule;
        capsule.addRoundedRect(QRectF{-half, top, half * 2.0, bottom - top},
                               half, half);
        auto *band_item = new QGraphicsPathItem(capsule, &item);
        const DrawnRow &sample = layout.inputs.at(last);
        QColor band_fill = sample.style.fill;
        band_fill.setAlpha(70);
        band_item->setBrush(band_fill);
        band_item->setPen(QPen{sample.style.fill, 1.4});
        band_item->setZValue(0.8);
        band_item->setToolTip(QStringLiteral(
            "The shader's variadic inputs: drop an output on the empty end "
            "to add one."));
        tag(band_item, FramePlanSocketBandItem);
    }
    for (const auto &row : layout.inputs) {
        const qreal centre_y = cursor_y + FramePlanSocketRowHeight / 2.0;
        const QString shown =
            row.declared  ? row.label
            : row.history ? QStringLiteral("%1 (history)").arg(row.resource)
                          : row.resource;
        const QString text = row_metrics.elidedText(
            shown, Qt::ElideMiddle, layout.width - 30.0);
        auto *name = new QGraphicsSimpleTextItem(text, &item);
        name->setFont(row_font);
        name->setBrush(row.removed ? removed_color.lighter(130)
                       : row.empty ? QColor{QStringLiteral("#8d9aa6")}
                                   : QColor{QStringLiteral("#c6d3de")});
        name->setPos(14.0, centre_y - row_metrics.height() / 2.0);
        tag(name, FramePlanRowLabelItem);
        name->setData(FramePlanPortRole, row.resource);
        name->setData(FramePlanSocketDirectionRole, QStringLiteral("input"));
        declare(name, row);
        socket(row, QPointF{0.0, centre_y}, "input");
        if (row.key == QLatin1String{"in:variadic"}) {
            // VI-3b: Fix sockets, beside the band's end -- the group becomes
            // one fixed declaration per connected input.
            const QString caption = QStringLiteral("Fix");
            const qreal pill_width = row_metrics.horizontalAdvance(caption) + 12.0;
            const qreal pill_height = row_metrics.height() + 2.0;
            QPainterPath pill;
            pill.addRoundedRect(QRectF{layout.width - pill_width - 8.0,
                                       centre_y - pill_height / 2.0,
                                       pill_width, pill_height},
                                pill_height / 2.0, pill_height / 2.0);
            auto *fix = new QGraphicsPathItem(pill, &item);
            fix->setBrush(QColor{QStringLiteral("#2f4a5f")});
            fix->setPen(QPen{QColor{QStringLiteral("#6f9fc2")}, 1.0});
            fix->setZValue(1.1);
            fix->setToolTip(QStringLiteral(
                "Fix sockets: the shader declares each connected input by "
                "name instead of the variadic group."));
            tag(fix, FramePlanSocketFixItem);
            auto *fix_text = new QGraphicsSimpleTextItem(caption, fix);
            fix_text->setFont(row_font);
            fix_text->setBrush(QColor{QStringLiteral("#dbe8f2")});
            fix_text->setPos(layout.width - pill_width - 2.0,
                             centre_y - row_metrics.height() / 2.0);
            fix_text->setAcceptedMouseButtons(Qt::NoButton);
        }
        if (!row.warning.isEmpty()) {
            // Rule 5: an amber mark after the row's text; the tooltip says
            // which name and which resource disagree.
            constexpr qreal diameter = 9.0;
            auto *mark = new QGraphicsEllipseItem(
                QRectF{14.0 + row_metrics.horizontalAdvance(text) + 5.0,
                       centre_y - diameter / 2.0, diameter, diameter},
                &item);
            mark->setBrush(QColor{QStringLiteral("#e0a030")});
            mark->setPen(QPen{QColor{QStringLiteral("#2b2118")}, 1.0});
            mark->setZValue(1.2);
            mark->setToolTip(row.warning);
            tag(mark, FramePlanSocketWarningItem);
            mark->setData(FramePlanReasonRole, row.warning);
            declare(mark, row);
        }
        cursor_y += FramePlanSocketRowHeight;
    }
}

QGraphicsSimpleTextItem *addCenteredLabel(QGraphicsPathItem &item,
                                          const QString &text, qreal width,
                                          qreal height) {
    auto *label = new QGraphicsSimpleTextItem(text, &item);
    label->setBrush(currentStudioTheme().canvas.heading_text);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    const QRectF bounds = label->boundingRect();
    label->setPos((width - bounds.width()) / 2.0,
                  (height - bounds.height()) / 2.0);
    return label;
}

void addCenteredEntityLabel(QGraphicsPathItem &item, const QString &text,
                            qreal width, qreal height,
                            const std::string &kind,
                            const std::string &graph,
                            const std::string &name) {
    auto *label = addCenteredLabel(item, text, width, height);
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
    scene.setProperty("pelicanBarrierCoverage",
                      barrierCoverageText(coverage));
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

// F-1. The selected target as a node: its name, its format and its size, drawn
// to the left of the passes that touch it. It is the handle the target's own
// gestures need -- a target nobody reads or writes yet has no socket at all.
constexpr qreal ResourceNodeWidth = 210.0;
constexpr qreal ResourceNodeHeight = 58.0;

// The node itself, not yet in a scene: F-7 needs its width before the layout
// places it, and F-1 places it against what is already drawn.
QGraphicsRectItem *makeResourceNode(const FramePlanModel &model,
                                    const FramePlanResource &resource,
                                    bool picked) {
    auto *node = new QGraphicsRectItem(
        QRectF{0.0, 0.0, ResourceNodeWidth, ResourceNodeHeight});
    node->setPen(QPen{picked ? QColor{QStringLiteral("#f0c05a")}
                             : QColor{QStringLiteral("#5f9fcf")},
                      picked ? 2.4 : 1.4});
    node->setBrush(QBrush{QColor{QStringLiteral("#1d2a36")}});
    node->setZValue(2.0);
    annotateIdentity(*node, FramePlanResourceNodeItem, model.graph,
                     resource.name);
    node->setData(FramePlanEdgePickedRole, picked);
    QString size;
    if (resource.extent) {
        size = resource.extent->kind == "output_relative"
                   ? QStringLiteral("screen x%1")
                         .arg(resource.extent->scale_x, 0, 'g', 4)
                   : QStringLiteral("%1 x %2")
                         .arg(static_cast<qulonglong>(resource.extent->width))
                         .arg(static_cast<qulonglong>(resource.extent->height));
    }
    node->setToolTip(QStringLiteral("%1\nDouble-click: edit the declaration. "
                                    "Delete: remove it.")
                         .arg(qtext(resource.name)));
    auto *title = new QGraphicsSimpleTextItem(qtext(resource.name), node);
    QFont title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);
    title->setBrush(QColor{QStringLiteral("#e8f1f8")});
    title->setPos(10.0, 8.0);
    title->setAcceptedMouseButtons(Qt::NoButton);
    auto *facts = new QGraphicsSimpleTextItem(
        QStringLiteral("render target  %1  %2")
            .arg(qtext(resource.format), size),
        node);
    facts->setBrush(QColor{QStringLiteral("#9fb6c8")});
    facts->setPos(10.0, 32.0);
    facts->setAcceptedMouseButtons(Qt::NoButton);
    // As wide as its longest line, so a long name or size is never cut.
    const qreal width = std::max(
        ResourceNodeWidth, std::max(title->boundingRect().width(),
                                    facts->boundingRect().width()) +
                               20.0);
    node->setRect(QRectF{0.0, 0.0, width, ResourceNodeHeight});
    // F-7: the output socket. Dragging from it onto a pass asks that pass to
    // read the target -- the same N-8 request an output socket on a pass
    // makes -- so a target nothing writes yet still has a place to start the
    // connection from. It is named like a pass's socket, with the target
    // standing in for the node.
    const DrawnRow row = framePlanRow(model, resource.name, false);
    auto *socket = new QGraphicsPathItem(
        socketPath(row.style.shape, row.style.size), node);
    socket->setPos(width, ResourceNodeHeight / 2.0);
    socket->setZValue(1.0);
    socket->setBrush(row.style.fill);
    socket->setPen(QPen{row.style.outline, row.style.outline_width});
    socket->setToolTip(
        QStringLiteral("%1\nDrag onto a pass to make it read this target.")
            .arg(row.type.isEmpty() ? row.resource
                                    : QStringLiteral("%1 : %2")
                                          .arg(row.resource, row.type)));
    socket->setData(FramePlanItemKindRole,
                    QString::fromLatin1(FramePlanSocketItem));
    socket->setData(FramePlanGraphRole, qtext(model.graph));
    socket->setData(FramePlanNameRole, qtext(resource.name));
    socket->setData(FramePlanPortRole, qtext(resource.name));
    socket->setData(FramePlanSocketDirectionRole, QStringLiteral("output"));
    socket->setAcceptedMouseButtons(Qt::NoButton);
    return node;
}

QGraphicsRectItem *addResourceNode(QGraphicsScene &scene,
                                   const FramePlanModel &model,
                                   const FramePlanResource &resource,
                                   bool picked) {
    const QRectF bounds = scene.itemsBoundingRect();
    const bool beside_drawing = !scene.items().empty() && bounds.isValid();
    const QPointF origin =
        beside_drawing
            ? QPointF{bounds.left() - ResourceNodeWidth - 70.0, bounds.top()}
            : QPointF{0.0, 0.0};
    auto *node = makeResourceNode(model, resource, picked);
    scene.addItem(node);
    node->setPos(origin);
    const qreal width = node->rect().width();
    if (beside_drawing && width > ResourceNodeWidth)
        node->setPos(origin - QPointF{width - ResourceNodeWidth, 0.0});
    return node;
}

std::optional<FramePlanNodeKey> resourceNodeAt(QGraphicsScene &scene,
                                               QPointF point) {
    for (QGraphicsItem *item : scene.items(point, Qt::IntersectsItemShape,
                                           Qt::DescendingOrder)) {
        for (QGraphicsItem *at = item; at != nullptr; at = at->parentItem()) {
            if (itemKind(*at) != QLatin1String{FramePlanResourceNodeItem})
                continue;
            return FramePlanNodeKey{
                at->data(FramePlanGraphRole).toString().toStdString(),
                at->data(FramePlanNameRole).toString().toStdString()};
        }
    }
    return std::nullopt;
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

SocketTypeFacts framePlanSocketFacts(const FramePlanModel &model,
                                     const std::string &resource,
                                     bool history) {
    SocketTypeFacts facts;
    // A history read is a different thing from the resource it names: it is
    // last frame's copy. The shape says so before the row text does.
    if (history) {
        facts.type = "frame_plan.history";
        return facts;
    }
    const auto found = std::ranges::find(model.resources, resource,
                                         &FramePlanResource::name);
    // A resource the plan never declared is a state to draw, not one to guess
    // at: an empty type falls through to the table's `unknown` entry.
    if (found == model.resources.end()) return facts;
    if (found->usage)
        for (const auto &usage : *found->usage)
            if (usage.find("ACCELERATION_STRUCTURE") !=
                    std::string::npos ||
                usage.find("acceleration_structure") !=
                    std::string::npos) {
                facts.type = "frame_plan.acceleration_structure";
                return facts;
            }
    if (found->kind == "buffer") {
        facts.type = "frame_plan.buffer";
    } else if (found->kind == "frame_target") {
        // The frame target is the swapchain image the frame ends on.
        facts.type = "frame_plan.swapchain";
    } else if (found->kind == "render_target") {
        facts.type = "frame_plan.image";
    }
    return facts;
}

FramePlanGraphicsScene::FramePlanGraphicsScene(QObject *parent)
    : QGraphicsScene{parent} {
    setItemIndexMethod(QGraphicsScene::NoIndex);
    // The items hold the colours they were built with: following the theme is
    // drawing the current graph again.
    theme_listener_ = addStudioThemeListener([this] { renderCurrentGraph(); });
    connect(this, &QGraphicsScene::selectionChanged, this,
            [this] { recordSelection(); });
    resetGraph();
}

FramePlanGraphicsScene::~FramePlanGraphicsScene() {
    removeStudioThemeListener(theme_listener_);
    // The in-place editor is a widget this scene owns through a proxy item;
    // clear() below would delete the proxy under the unique_ptr.
    closeInlineEditor();
    // QGraphicsScene clears selected items in its base destructor. Do it while
    // this class's selection members still exist, with the signal handler
    // guarded, because a multiple selection otherwise emits during teardown.
    rebuilding_ = true;
    clear();
}

void FramePlanGraphicsScene::resetGraph() {
    clearConnectionGesture();
    closeInlineEditor();
    selected_edge_.reset();
    context_node_.reset();
    picked_resource_.reset();
    rebuilding_ = true;
    clear();
    selected_node_.reset();
    selected_nodes_.clear();
    selected_resource_.reset();
    session_node_positions_.clear();
    current_model_.reset();
    current_target_.reset();
    whole_frame_ = false;
    current_depth_ = 1;
    collapsed_groups_.clear();
    current_group_scope_.reset();
    session_groups_.clear();
    next_session_group_id_ = 1;
    convex_hull_proposal_.reset();
    group_feedback_.clear();
    setSceneRect({});
    rebuilding_ = false;
    setProperty("pelicanGraph", QString{});
    setProperty("pelicanTarget", QString{});
    setProperty("pelicanSubtreeDepth", 1);
    setProperty("pelicanNodeRecordCount", 0);
    setProperty("pelicanDependencyRecordCount", 0);
    setProperty("pelicanVisibleItemCount", 0);
    setProperty("pelicanPhysicalResourceCount", 0);
    setProperty("pelicanAdoptedAliasCount", 0);
    setProperty("pelicanNotAdoptedAliasCount", 0);
    setProperty("pelicanPlanningProfile", QString{});
    setProperty("pelicanPlanningEndpoint", QString{});
    setProperty("pelicanCollapsedGroups", QStringList{});
    setProperty("pelicanCurrentGroupScope", QString{});
    setProperty("pelicanNonCollapsibleGroups", QStringList{});
    setProperty("pelicanGroupFeedback", QString{});
    setProperty("pelicanSessionGroups", QStringList{});
    setProperty("pelicanBoundaryStubCount", 0);
    setProperty("pelicanBarrierRecordCount", 0);
    setProperty("pelicanVisibleBarrierRecordCount", 0);
    setProperty("pelicanInternalBarrierRecordCount", 0);
    setProperty("pelicanOutsideBarrierRecordCount", 0);
    setProperty("pelicanBarrierCoverage", QString{});
    setProperty("pelicanExecutionPlanState", QString{});
    setProperty("pelicanExecutionPlanReasonCode", QString{});
    publishStateProperties();
}

void FramePlanGraphicsScene::setGpuTiming(
    FramePlanGpuTimingSnapshot snapshot) {
    gpu_timing_ = std::move(snapshot);
    for (QGraphicsItem *item : items()) {
        const QVariant node_name =
            item->data(FramePlanGpuTimingNodeNameRole);
        if (!node_name.isValid()) {
            continue;
        }
        auto *label = dynamic_cast<QGraphicsSimpleTextItem *>(item);
        if (label == nullptr) {
            continue;
        }
        // The label keeps the row the node body gave it: only the text and
        // the published numbers change when a new snapshot arrives.
        applyGpuTimingLabel(*label, gpu_timing_,
                            node_name.toString().toStdString(),
                            label->pos());
    }
}

void FramePlanGraphicsScene::setShaderErrorMarks(
    std::map<std::string, QString> marks) {
    if (marks == shader_error_marks_) return;
    shader_error_marks_ = std::move(marks);
    applyShaderErrorMarks();
}

// F-3. A red badge on the heading's right corner, over everything the node
// draws, with the first line of the compile log as its tooltip. The badge is
// the node's child, so it moves with the node and goes when the node goes.
void FramePlanGraphicsScene::applyShaderErrorMarks() {
    std::vector<QGraphicsItem *> stale;
    std::vector<QGraphicsItem *> nodes;
    for (QGraphicsItem *item : items()) {
        const QString kind = item->data(FramePlanItemKindRole).toString();
        if (kind == QLatin1String{FramePlanNodeShaderErrorItem})
            stale.push_back(item);
        else if (kind == QLatin1String{FramePlanNodeItem})
            nodes.push_back(item);
    }
    for (QGraphicsItem *item : stale) delete item;
    std::size_t marked = 0;
    for (QGraphicsItem *node : nodes) {
        const QString shader_line = QStringLiteral("\nShader: ");
        const QString tooltip = node->toolTip().section(shader_line, 0, 0);
        const auto found = shader_error_marks_.find(
            node->data(FramePlanNameRole).toString().toStdString());
        if (found == shader_error_marks_.end()) {
            node->setToolTip(tooltip);
            continue;
        }
        constexpr qreal diameter = 16.0;
        const QRectF bounds = node->boundingRect();
        auto *badge = new QGraphicsEllipseItem(
            QRectF{bounds.right() - diameter * 0.75,
                   bounds.top() - diameter * 0.25, diameter, diameter},
            node);
        badge->setBrush(QColor{QStringLiteral("#d94c3d")});
        badge->setPen(QPen{QColor{QStringLiteral("#ffffff")}, 1.4});
        badge->setZValue(4.0);
        badge->setData(FramePlanItemKindRole,
                       QString::fromLatin1(FramePlanNodeShaderErrorItem));
        badge->setData(FramePlanGraphRole, node->data(FramePlanGraphRole));
        badge->setData(FramePlanNameRole, node->data(FramePlanNameRole));
        badge->setToolTip(found->second);
        badge->setAcceptedMouseButtons(Qt::NoButton);
        auto *mark = new QGraphicsSimpleTextItem(QStringLiteral("!"), badge);
        QFont font = mark->font();
        font.setBold(true);
        font.setPointSizeF(8.5);
        mark->setFont(font);
        mark->setBrush(QColor{QStringLiteral("#ffffff")});
        const QRectF glyph = mark->boundingRect();
        mark->setPos(badge->rect().center() - glyph.center());
        mark->setAcceptedMouseButtons(Qt::NoButton);
        node->setToolTip(tooltip + shader_line + found->second);
        ++marked;
    }
    setProperty("pelicanShaderErrorMarkCount",
                static_cast<qulonglong>(marked));
}

void FramePlanGraphicsScene::populate(
    const FramePlanModel &model,
    const std::optional<FramePlanNodeKey> &selected_target, int depth) {
    current_model_ = model;
    current_target_ = selected_target;
    current_depth_ = std::max(0, depth);
    whole_frame_ = false;
    // A proposal describes one exact plan snapshot. A normal frame-plan
    // refresh may change reachability even when every selected name survives,
    // so never carry the suggestion across populate().
    convex_hull_proposal_.reset();
    pruneGroupState();
    renderCurrentGraph();
}

void FramePlanGraphicsScene::populateWholeFrame(const FramePlanModel &model) {
    current_model_ = model;
    current_target_.reset();
    whole_frame_ = true;
    convex_hull_proposal_.reset();
    pruneGroupState();
    renderCurrentGraph();
}

void FramePlanGraphicsScene::pruneGroupState() {
    group_feedback_.clear();
    if (!current_model_) {
        collapsed_groups_.clear();
        current_group_scope_.reset();
        session_groups_.clear();
        convex_hull_proposal_.reset();
        return;
    }

    std::set<std::string, std::less<>> model_nodes;
    for (const auto &node : current_model_->nodes) {
        model_nodes.insert(node.name);
    }
    for (auto group = session_groups_.begin();
         group != session_groups_.end();) {
        const bool wrong_graph =
            group->first.first != current_model_->graph;
        const bool missing_member = std::ranges::any_of(
            group->second, [&](const auto &member) {
                return !model_nodes.contains(member);
            });
        if (!wrong_graph && !missing_member && group->second.size() >= 2) {
            ++group;
            continue;
        }
        const auto position_key = std::tuple{
            group->first.first, std::string{FramePlanGroupItem},
            group->first.second};
        session_node_positions_.erase(position_key);
        group = session_groups_.erase(group);
    }

    const auto groups =
        discoverGroups(*current_model_, session_groups_);
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
    clearConnectionGesture();
    // Q-4. The editor is a widget in the scene; a redraw deletes every item,
    // so it has to go first or its proxy is deleted twice. What it was and
    // what it held is taken down with it and opened again over the bands this
    // draw produces: a refresh, a theme switch or a reply arriving mid-edit is
    // not a reason to throw away what somebody is in the middle of typing.
    const auto carried = rebuilding_ ? std::nullopt : carryInlineEdit();
    closeInlineEditor();
    if (!current_model_) {
        return;
    }
    const FramePlanModel &model = *current_model_;
    // N-8: what each node lets the user retype, asked once per node per draw.
    // The answer needs the authoring context, which only the owner has.
    std::map<std::string, std::vector<FramePlanInlineBand>, std::less<>>
        inline_bands;
    const auto bandsFor = [&](const std::string &name)
        -> const std::vector<FramePlanInlineBand> & {
        const auto found = inline_bands.find(name);
        if (found != inline_bands.end()) return found->second;
        auto bands = inlineFieldProvider
                         ? inlineFieldProvider(
                               FramePlanNodeKey{model.graph, name})
                         : std::vector<FramePlanInlineBand>{};
        return inline_bands.emplace(name, std::move(bands)).first->second;
    };
    // VI-3a: a pass whose shader's declarations the engine published draws
    // its input sockets from them. The order of its `input` array is the
    // binding, and only the owner's authoring context has it.
    std::map<std::string, FramePlanInputSockets, std::less<>> input_sockets;
    declared_nodes_.clear();
    variadic_nodes_.clear();
    for (const auto &node : model.nodes) {
        if (!framePlanNodeDeclaresInputs(node)) continue;
        const FramePlanNodeKey key{model.graph, node.name};
        auto sockets = framePlanInputSockets(
            model, node,
            passInputsProvider ? passInputsProvider(key) : std::nullopt,
            passthroughShownProvider ? passthroughShownProvider(key)
                                     : std::nullopt);
        declared_nodes_.insert(node.name);
        if (sockets.variadic) variadic_nodes_.insert(node.name);
        input_sockets.emplace(node.name, std::move(sockets));
    }
    const auto socketsFor =
        [&](const std::string &name) -> const FramePlanInputSockets * {
        const auto found = input_sockets.find(name);
        return found == input_sockets.end() ? nullptr : &found->second;
    };
    const int normalized_depth = current_depth_;
    const std::vector<GroupDefinition> groups =
        discoverGroups(model, session_groups_);
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
            : whole_frame_ ? allNodeNames(model)
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

        QStringList session_group_ids;
        for (const auto &[state_key, members] : session_groups_) {
            (void)members;
            if (state_key.first == model.graph) {
                session_group_ids.push_back(
                    qtext(groupStateId(state_key)));
            }
        }
        setProperty("pelicanSessionGroups", session_group_ids);
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
        setProperty("pelicanNodeRecordCount", 0);
        setProperty("pelicanDependencyRecordCount", 0);
        setProperty("pelicanVisibleItemCount", 0);
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
        publishBarrierCoverage(*this, barrier_coverage);
        publish_group_properties();
        publishStateProperties();
        // The editor goes back over the band it was on, if this draw
        // still has one.
        if (carried) restoreInlineEdit(*carried);
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
            entity.session_group = group.session_group;
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

    // N-4: the shared, Qt-free placement. The Frame Plan keeps its own column
    // rule -- a collapsed group sits at the minimum column of its visible
    // members, a boundary stub one column outside the window -- and hands it
    // over, so only the ordering inside a column, the vertical coordinate and
    // the lanes long edges travel in are the layout's to choose.
    const auto entitySize = [&](const VisibleEntity &entity) {
        if (entity.group) return std::pair{GroupWidth, GroupHeight};
        if (entity.boundary_stub)
            return std::pair{BoundaryStubWidth, BoundaryStubHeight};
        const NodeBodyLayout body =
            nodeBodyLayout(model, *nodes_by_name.at(entity.key),
                           bandsFor(entity.key), socketsFor(entity.key));
        return std::pair{body.width, body.height};
    };
    std::vector<LayoutNode> layout_nodes;
    layout_nodes.reserve(entities.size());
    for (const auto &[level, level_entities] : levels) {
        (void)level;
        for (const VisibleEntity *entity : level_entities) {
            const auto [width, height] = entitySize(*entity);
            layout_nodes.push_back(LayoutNode{entity->key, width, height});
        }
    }
    std::vector<LayoutEdge> layout_edges;
    layout_edges.reserve(bundles.size());
    for (const auto &[endpoints, records] : bundles) {
        (void)records;
        layout_edges.push_back(LayoutEdge{endpoints.first, endpoints.second});
    }
    LayoutOptions layout_options;
    layout_options.horizontal_gap = HorizontalGap;
    layout_options.vertical_gap = VerticalGap;
    for (const auto &[key, entity] : entities)
        layout_options.columns.emplace(key, entity.source_column);
    // F-7: in the whole frame, each target no pass writes is a node of its
    // own, placed by the same layout one column before the first pass that
    // reads it -- or before the whole drawing, when nothing reads it yet.
    struct LooseResource {
        const FramePlanResource *resource = nullptr;
        std::string layout_key;
        QGraphicsRectItem *item = nullptr;
    };
    std::vector<LooseResource> loose_resources;
    if (whole_frame_ && !scope_group) {
        std::optional<int> first_column;
        std::set<std::string, std::less<>> occupied_keys;
        for (const auto &[key, entity] : entities) {
            occupied_keys.insert(key);
            first_column = std::min(first_column.value_or(entity.source_column),
                                    entity.source_column);
        }
        for (const FramePlanResource *resource : unwrittenResources(model)) {
            std::optional<int> reader_column;
            for (const auto &node : model.nodes) {
                if (!containsMember(node.reads, resource->name) &&
                    !containsMember(node.history_reads, resource->name))
                    continue;
                const auto entity = entity_for_node.find(node.name);
                if (entity == entity_for_node.end()) continue;
                const int column = entities.at(entity->second).source_column;
                reader_column =
                    std::min(reader_column.value_or(column), column);
            }
            LooseResource loose{
                .resource = resource,
                .layout_key = uniqueSyntheticName(
                    "__pelican_resource__:" + resource->name, occupied_keys),
                .item = makeResourceNode(
                    model, *resource,
                    picked_resource_ && picked_resource_->graph == model.graph &&
                        picked_resource_->name == resource->name),
            };
            layout_nodes.push_back(LayoutNode{loose.layout_key,
                                              loose.item->rect().width(),
                                              loose.item->rect().height()});
            layout_options.columns.emplace(
                loose.layout_key,
                reader_column.value_or(first_column.value_or(0)) - 1);
            loose_resources.push_back(std::move(loose));
        }
    }
    const NodeGraphLayout layout =
        layoutNodeGraph(layout_nodes, layout_edges, layout_options);

    std::map<std::string, QGraphicsPathItem *, std::less<>> node_items;
    for (const auto &[level, level_entities] : levels) {
        (void)level;
        for (VisibleEntity *entity : level_entities) {
            // A node is as tall as the resources it touches, so the row pitch
            // is what this entity actually needs rather than one constant.
            const NodeBodyLayout body =
                entity->group || entity->boundary_stub
                    ? NodeBodyLayout{}
                    : nodeBodyLayout(model, *nodes_by_name.at(entity->key),
                                     bandsFor(entity->key),
                                     socketsFor(entity->key));
            const qreal width =
                entity->group
                    ? GroupWidth
                    : entity->boundary_stub ? BoundaryStubWidth : body.width;
            const qreal height =
                entity->group
                    ? GroupHeight
                    : entity->boundary_stub ? BoundaryStubHeight
                                            : body.height;
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
            const auto placed = layout.boxes.find(entity->key);
            QPointF position =
                placed == layout.boxes.end()
                    ? QPointF{0.0, 0.0}
                    : QPointF{placed->second.x, placed->second.y};
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
                        : roundedEntityPath(width, height,
                                            FramePlanNodeCornerRadius);
                item = new MovableNodeItem(
                    path, [this, position_key,
                           routing_key](const QPointF &moved_position) {
                    if (rebuilding_) {
                        return;
                    }
                    session_node_positions_[position_key] = moved_position;
                    // N-8: the document keeps the placement now. The owner
                    // debounces; this only says that something moved.
                    if (placementChanged) placementChanged();
                    rerouteFramePlanBundles(*this, routing_key.graph,
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
            // The heading band carries the source colour; the body stays
            // neutral so a pass that touches ten resources is not a wall of
            // one hue. FramePlanColorRole still answers with the source
            // colour, which is the fact the canvas publishes about a node.
            const QColor fill =
                entity->group
                    ? QColor{QStringLiteral("#7653a6")}
                    : entity->boundary_stub
                          ? QColor{QStringLiteral("#34404b")}
                          : sourceColor(entity->source);
            const StudioTheme &theme = currentStudioTheme();
            item->setBrush(entity->boundary_stub
                               ? fill
                               : entity->group
                                     ? theme.domainTint(
                                           QStringLiteral("instance"))
                                           .darker(220)
                                     : theme.canvas.node_body);
            // The rim is what lifts the node off the canvas, so it is the
            // theme's text colour rather than its border colour: a dark body
            // gets a light rim and a light body a dark one.
            QPen outline{entity->anchor
                             ? theme.canvas.dim_text
                             : entity->boundary_stub
                                   ? theme.canvas.edge
                                   : theme.canvas.heading_text};
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
                item->setData(FramePlanSessionGroupRole,
                              entity->session_group);
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
                // A collapsed group reads like a node with its rows folded
                // away: the same heading band, with "group" where a node
                // carries its kind, over the label that says how many nodes
                // are inside. The label text itself is unchanged.
                auto *group_band = new QGraphicsPathItem(
                    framePlanHeaderPath(GroupWidth), item);
                group_band->setBrush(QColor{QStringLiteral("#7653a6")});
                group_band->setPen(Qt::NoPen);
                group_band->setAcceptedMouseButtons(Qt::NoButton);
                const QFontMetricsF group_metrics{framePlanRowFont()};
                auto *group_kind = new QGraphicsSimpleTextItem(
                    QStringLiteral("group"), group_band);
                group_kind->setFont(framePlanRowFont());
                group_kind->setBrush(QColor{QStringLiteral("#ece4f8")});
                group_kind->setAcceptedMouseButtons(Qt::NoButton);
                group_kind->setPos(
                    GroupWidth - 12.0 -
                        group_metrics.horizontalAdvance(
                            QStringLiteral("group")),
                    (FramePlanHeaderHeight - group_metrics.height()) / 2.0);
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
                addCenteredLabel(*item, entity->label, BoundaryStubWidth,
                                 BoundaryStubHeight);
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
                    item->setData(FramePlanSessionGroupRole,
                                  node_group->second->session_group);
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
                addNodeBody(*item, node, body, model.graph,
                            sourceColor(entity->source), gpu_timing_);
                node_items.emplace(node.name, item);
            }
        }
    }

    std::size_t bundle_order = 0;
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
        std::vector<std::string> identities;
        for (const auto &record : records) {
            identities.push_back(recordIdentity(record));
        }
        const std::vector<std::string> resources{
            summary.resources.begin(), summary.resources.end()};
        // VI-3a: into a pass whose sockets come from its shader, one line per
        // socket the bundle feeds -- two, when one resource feeds two
        // bindings -- each ending on its own socket.
        std::vector<const FramePlanInputSocket *> fed;
        if (const FramePlanInputSockets *sockets =
                declared_nodes_.contains(endpoints.second)
                    ? socketsFor(endpoints.second)
                    : nullptr) {
            for (const auto &socket : sockets->sockets)
                if (socket.connected() && socket.input_index &&
                    summary.resources.contains(socket.resource))
                    fed.push_back(&socket);
        }
        if (fed.empty()) fed.push_back(nullptr);
        const QColor removed_color{QStringLiteral("#d94c3d")};
        for (const FramePlanInputSocket *socket : fed) {
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
            // The lane the layout kept for a bundle that flies over a column,
            // so it passes between those nodes rather than across them. An end
            // the user has dragged loses the lane (rerouteFramePlanBundles
            // clears it).
            if (bundle_order < layout.bends.size() &&
                !layout.bends[bundle_order].empty() &&
                !session_node_positions_.contains(
                    std::tuple{model.graph, std::string{FramePlanNodeItem},
                               endpoints.first}) &&
                !session_node_positions_.contains(
                    std::tuple{model.graph, std::string{FramePlanNodeItem},
                               endpoints.second})) {
                QVector<QPointF> bends;
                for (const LayoutPoint &point : layout.bends[bundle_order])
                    bends.push_back(QPointF{point.x, point.y});
                setNodeGraphBends(*edge, bends);
            }

            const auto publish_dependency_data = [&](QGraphicsItem &item) {
                item.setData(FramePlanEdgeRecordsRole, qlist(identities));
                item.setData(
                    FramePlanResourcesRole,
                    socket != nullptr
                        ? QStringList{qtext(socket->resource)}
                        : qlist(resources));
                item.setToolTip(summary.label);
                if (socket == nullptr) return;
                item.setData(FramePlanInputIndexRole,
                             static_cast<int>(*socket->input_index));
                const auto key = socket->key();
                if (!key.empty()) item.setData(FramePlanSocketKeyRole, qtext(key));
                item.setData(FramePlanSocketRemovedRole, socket->removed);
            };
            publish_dependency_data(*edge);

            // VI-3a rule 4: the end of a line whose binding the reloaded
            // shader no longer declares is red, like its socket.
            const QColor arrow_color =
                socket != nullptr && socket->removed ? removed_color
                                                     : edge_color;
            auto *arrow = addPolygon(
                QPolygonF{}, QPen{arrow_color}, QBrush{arrow_color});
            arrow->setZValue(0.5);
            annotateIdentity(*arrow, FramePlanEdgeArrowItem, model.graph,
                             endpoints.first + "->" + endpoints.second);
            annotateEndpoints(*arrow, endpoints);
            arrow->setData(FramePlanBundleOrderRole,
                           static_cast<qulonglong>(bundle_order));
            publish_dependency_data(*arrow);

            // No edge label box any more (N-3): the resource names are on the
            // node rows at both ends now, so a box in the middle of every line
            // said the same thing a third time. The summary stays on the
            // edge's tooltip, and the records stay on FramePlanEdgeRecordsRole.
            routeFramePlanBundle(*this, *edge);
        }
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

        if (convex_hull_proposal_ &&
            convex_hull_proposal_->graph == model.graph) {
            const qreal proposal_width = std::max(
                GroupWarningWidth,
                QFontMetricsF{QFont{}}.horizontalAdvance(
                    convex_hull_proposal_->message) +
                    24.0);
            auto *proposal = new ConvexHullProposalItem(
                QRectF{0.0, 0.0, proposal_width, GroupWarningHeight},
                [this] {
                    QMetaObject::invokeMethod(
                        this, [this] { acceptConvexHullProposal(); },
                        Qt::QueuedConnection);
                });
            addItem(proposal);
            proposal->setPos(0.0, warning_top);
            proposal->setZValue(1.3);
            proposal->setPen(
                QPen{QColor{QStringLiteral("#8d72bd")}, 1.8});
            proposal->setBrush(
                QColor{QStringLiteral("#302641")});
            proposal->setAcceptedMouseButtons(Qt::LeftButton);
            annotateIdentity(*proposal, FramePlanConvexHullProposalItem,
                             model.graph, "convex-hull-proposal");
            proposal->setData(FramePlanReasonRole,
                              convex_hull_proposal_->message);
            proposal->setData(
                FramePlanConvexHullMissingMembersRole,
                qlist(convex_hull_proposal_->missing_members));
            proposal->setData(
                FramePlanConvexHullImpracticalRole,
                convex_hull_proposal_->impractical);
            proposal->setToolTip(convex_hull_proposal_->message);
            auto *message = new QGraphicsSimpleTextItem(
                convex_hull_proposal_->message, proposal);
            message->setBrush(QColor{QStringLiteral("#f3ecff")});
            message->setPos(12.0, 14.0);
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

    for (const LooseResource &loose : loose_resources) {
        addItem(loose.item);
        const auto placed = layout.boxes.find(loose.layout_key);
        if (placed != layout.boxes.end())
            loose.item->setPos(placed->second.x, placed->second.y);
    }
    const bool picked_is_drawn =
        picked_resource_ &&
        ((selected_resource_ && *picked_resource_ == *selected_resource_) ||
         (picked_resource_->graph == model.graph &&
          std::ranges::any_of(loose_resources,
                              [&](const LooseResource &loose) {
                                  return loose.resource->name ==
                                         picked_resource_->name;
                              })));
    if (!picked_is_drawn) picked_resource_.reset();
    if (selected_resource_ && !scope_group) {
        const auto resource = std::ranges::find(
            model.resources, selected_resource_->name, &FramePlanResource::name);
        if (resource != model.resources.end())
            addResourceNode(*this, model, *resource,
                            picked_resource_.has_value());
    }
    setProperty("pelicanPickedResource",
                picked_resource_ ? qtext(picked_resource_->name) : QString{});

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
    applyEdgeEmphasis();
    // N-8: the picked edge is still picked after a redraw, unless the graph it
    // was in is no longer the one on screen.
    if (selected_edge_ && selected_edge_->graph != model.graph)
        selected_edge_.reset();
    applyEdgeSelection();

    setProperty("pelicanGraph", qtext(model.graph));
    setProperty("pelicanTarget",
                selected_resource_ ? qtext(selected_resource_->name)
                                   : QString{});
    setProperty("pelicanWholeFrame", whole_frame_);
    setProperty("pelicanSubtreeDepth", normalized_depth);
    setProperty("pelicanNodeRecordCount",
                static_cast<qulonglong>(model.nodes.size()));
    setProperty("pelicanDependencyRecordCount",
                static_cast<qulonglong>(model.dependencies.size()));
    setProperty("pelicanVisibleItemCount",
                static_cast<qulonglong>(entities.size()));
    // What the automatic placement achieved, so `layout_stats` can ask a live
    // window how tangled the drawing is.
    setProperty("pelicanLayoutCrossings",
                static_cast<qulonglong>(layout.crossings));
    setProperty("pelicanLayoutOverlaps",
                static_cast<qulonglong>(layoutOverlaps(layout.boxes)));
    setProperty("pelicanLayoutNodes",
                static_cast<qulonglong>(layout.boxes.size()));
    setProperty("pelicanLayoutReversedEdges",
                static_cast<qulonglong>(layout.reversed_edges.size()));
    setProperty("pelicanPhysicalResourceCount",
                static_cast<qulonglong>(physical_overlay.resource_count));
    setProperty("pelicanAdoptedAliasCount",
                static_cast<qulonglong>(
                    physical_overlay.adopted_alias_count));
    setProperty("pelicanNotAdoptedAliasCount",
                static_cast<qulonglong>(
                    physical_overlay.not_adopted_alias_count));
    setProperty("pelicanPlanningProfile",
                qtext(model.physical_plan.planning_profile));
    setProperty("pelicanPlanningEndpoint",
                qtext(model.physical_plan.planning_endpoint));
    setProperty("pelicanBoundaryStubCount",
                static_cast<qulonglong>(
                    boundary_entity_for_node.size()));
    setProperty("pelicanExecutionPlanState", QStringLiteral("available"));
    setProperty("pelicanExecutionPlanReasonCode", QString{});
    publishBarrierCoverage(*this, barrier_coverage);
    publish_group_properties();
    publishStateProperties();
    applyShaderErrorMarks();
    if (carried) restoreInlineEdit(*carried);
}

bool FramePlanGraphicsScene::collapseGroup(const QString &group_id) {
    if (!current_model_ || current_group_scope_) {
        return false;
    }
    const auto groups =
        discoverGroups(*current_model_, session_groups_);
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
    const auto groups =
        discoverGroups(*current_model_, session_groups_);
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
    const auto groups =
        discoverGroups(*current_model_, session_groups_);
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

bool FramePlanGraphicsScene::installSessionGroup(
    std::vector<std::string> members) {
    if (!current_model_ || current_group_scope_ || members.size() < 2) {
        return false;
    }
    std::ranges::sort(members);
    members.erase(std::unique(members.begin(), members.end()), members.end());
    if (members.size() < 2) {
        return false;
    }

    std::set<std::string, std::less<>> model_nodes;
    for (const auto &node : current_model_->nodes) {
        model_nodes.insert(node.name);
    }
    if (std::ranges::any_of(members, [&](const auto &member) {
            return !model_nodes.contains(member);
        })) {
        group_feedback_ = QStringLiteral(
            "The frame plan changed before the session group could be "
            "created.");
        setProperty("pelicanGroupFeedback", group_feedback_);
        return false;
    }
    if (std::ranges::any_of(
            session_groups_, [&](const auto &existing) {
                return std::ranges::any_of(
                    existing.second, [&](const auto &existing_member) {
                        return std::ranges::find(members, existing_member) !=
                               members.end();
                    });
            })) {
        group_feedback_ = QStringLiteral(
            "Nested or overlapping session groups are not supported; "
            "remove the existing session group first.");
        setProperty("pelicanGroupFeedback", group_feedback_);
        return false;
    }

    const std::set<std::string, std::less<>> member_set{members.begin(),
                                                        members.end()};
    const QString convexity = nonConvexReason(
        *current_model_, QStringLiteral("The selected nodes"), member_set);
    if (!convexity.isEmpty()) {
        group_feedback_ = convexity;
        setProperty("pelicanGroupFeedback", group_feedback_);
        return false;
    }

    const GroupStateKey state_key{
        current_model_->graph,
        "session:" + std::to_string(next_session_group_id_++)};
    session_groups_.emplace(state_key, std::move(members));
    collapsed_groups_.insert(state_key);
    convex_hull_proposal_.reset();
    group_feedback_.clear();
    renderCurrentGraph();
    return true;
}

bool FramePlanGraphicsScene::createGroupFromSelection() {
    if (!current_model_ || current_group_scope_ ||
        selected_nodes_.size() < 2) {
        return false;
    }

    std::set<std::string, std::less<>> selected;
    for (const auto &node : selected_nodes_) {
        if (node.graph != current_model_->graph) {
            return false;
        }
        selected.insert(node.name);
    }
    const QString convexity = nonConvexReason(
        *current_model_, QStringLiteral("The selected nodes"), selected);
    if (convexity.isEmpty()) {
        return installSessionGroup(
            std::vector<std::string>{selected.begin(), selected.end()});
    }

    const ConvexHullResult hull = convexHull(*current_model_, selected);
    if (hull.missing.empty()) {
        // This should be unreachable when nonConvexReason() reports a
        // leave-and-return path, but fail visibly instead of manufacturing a
        // group if a malformed model ever violates that invariant.
        group_feedback_ = convexity;
        setProperty("pelicanGroupFeedback", group_feedback_);
        return false;
    }
    const QString proposal_text =
        convexHullProposalText(hull, current_model_->nodes.size());
    convex_hull_proposal_ = ConvexHullProposalState{
        .graph = current_model_->graph,
        .selected_members = {selected.begin(), selected.end()},
        .hull_members = hull.members,
        .missing_members = hull.missing,
        .impractical = hull.impractical,
        .message = proposal_text,
    };
    group_feedback_ = proposal_text;
    renderCurrentGraph();
    return false;
}

bool FramePlanGraphicsScene::acceptConvexHullProposal() {
    if (!current_model_ || current_group_scope_ ||
        !convex_hull_proposal_ ||
        convex_hull_proposal_->graph != current_model_->graph) {
        return false;
    }

    const std::set<std::string, std::less<>> selected{
        convex_hull_proposal_->selected_members.begin(),
        convex_hull_proposal_->selected_members.end()};
    const ConvexHullResult current_hull = convexHull(*current_model_, selected);
    if (current_hull.members != convex_hull_proposal_->hull_members ||
        current_hull.missing != convex_hull_proposal_->missing_members) {
        convex_hull_proposal_.reset();
        group_feedback_ = QStringLiteral(
            "The frame plan changed; create the convex-hull proposal again.");
        renderCurrentGraph();
        return false;
    }
    const std::set<std::string, std::less<>> hull_members{
        current_hull.members.begin(), current_hull.members.end()};
    if (!nonConvexReason(*current_model_,
                         QStringLiteral("The proposed convex hull"),
                         hull_members)
             .isEmpty()) {
        group_feedback_ = QStringLiteral(
            "The proposed members are no longer convex.");
        setProperty("pelicanGroupFeedback", group_feedback_);
        return false;
    }
    return installSessionGroup(current_hull.members);
}

bool FramePlanGraphicsScene::removeSessionGroup(const QString &group_id) {
    const auto found = std::ranges::find_if(
        session_groups_, [&](const auto &entry) {
            return qtext(groupStateId(entry.first)) == group_id;
        });
    if (!current_model_ || found == session_groups_.end()) {
        return false;
    }

    const GroupStateKey state_key = found->first;
    session_groups_.erase(found);
    collapsed_groups_.erase(state_key);
    if (current_group_scope_ == state_key) {
        current_group_scope_.reset();
    }
    session_node_positions_.erase(std::tuple{
        state_key.first, std::string{FramePlanGroupItem}, state_key.second});
    convex_hull_proposal_.reset();
    group_feedback_.clear();
    renderCurrentGraph();
    return true;
}

void FramePlanGraphicsScene::autoLayout() {
    if (!current_model_) return;
    const std::string graph = current_model_->graph;
    const auto before = session_node_positions_.size();
    std::erase_if(session_node_positions_, [&](const auto &entry) {
        return std::get<0>(entry.first) == graph;
    });
    // Dropping what the user moved is itself a placement change: the document
    // holds those positions, so it has to hear that they are gone.
    if (before != session_node_positions_.size() && placementChanged)
        placementChanged();
    renderCurrentGraph();
}

void FramePlanGraphicsScene::setMenuActions(QAction *auto_layout,
                                            QAction *fit_view) {
    auto_layout_action_ = auto_layout;
    fit_view_action_ = fit_view;
}

void FramePlanGraphicsScene::setInsertActions(QAction *add_pass,
                                              QAction *add_task) {
    add_pass_action_ = add_pass;
    add_task_action_ = add_task;
}

void FramePlanGraphicsScene::setSocketActions(QAction *fix, QAction *variadic) {
    fix_sockets_action_ = fix;
    variadic_sockets_action_ = variadic;
}

std::optional<FramePlanNodeKey> FramePlanGraphicsScene::insertAnchor() const {
    return context_node_ ? context_node_ : selected_node_;
}

void FramePlanGraphicsScene::setPlacement(
    const std::string &graph, const std::map<std::string, QPointF> &nodes) {
    std::erase_if(session_node_positions_, [&](const auto &entry) {
        return std::get<0>(entry.first) == graph &&
               std::get<1>(entry.first) == std::string{FramePlanNodeItem};
    });
    for (const auto &[name, position] : nodes)
        session_node_positions_.insert_or_assign(
            std::tuple{graph, std::string{FramePlanNodeItem}, name}, position);
    renderCurrentGraph();
}

std::map<std::string, QPointF> FramePlanGraphicsScene::placement(
    const std::string &graph) const {
    std::map<std::string, QPointF> nodes;
    for (const auto &[key, position] : session_node_positions_) {
        if (std::get<0>(key) != graph ||
            std::get<1>(key) != std::string{FramePlanNodeItem})
            continue;
        nodes.emplace(std::get<2>(key), position);
    }
    return nodes;
}

// The picked edge is drawn heavier; nothing else about it changes, because the
// colour of a frame-plan edge already says what the dependency is.
void FramePlanGraphicsScene::applyEdgeSelection() {
    for (QGraphicsItem *item : items()) {
        if (itemKind(*item) != QLatin1String{FramePlanEdgeItem}) continue;
        auto *edge = dynamic_cast<QGraphicsPathItem *>(item);
        if (edge == nullptr) continue;
        std::vector<std::string> resources;
        for (const QString &resource :
             edge->data(FramePlanResourcesRole).toStringList())
            resources.push_back(resource.toStdString());
        std::optional<std::size_t> input_index;
        if (const QVariant index = edge->data(FramePlanInputIndexRole);
            index.isValid())
            input_index = static_cast<std::size_t>(index.toInt());
        const bool picked =
            selected_edge_.has_value() &&
            edge->data(FramePlanGraphRole).toString().toStdString() ==
                selected_edge_->graph &&
            edge->data(FramePlanToNameRole).toString().toStdString() ==
                selected_edge_->to_node &&
            resources == selected_edge_->resources &&
            input_index == selected_edge_->input_index;
        const bool already = edge->data(FramePlanEdgePickedRole).toBool();
        if (picked == already) continue;
        QPen pen = edge->pen();
        pen.setWidthF(picked ? pen.widthF() + 1.6
                             : std::max<qreal>(0.5, pen.widthF() - 1.6));
        edge->setPen(pen);
        edge->setData(FramePlanEdgePickedRole, picked);
    }
}

void FramePlanGraphicsScene::selectEdge(const FramePlanEdgeSelection &edge) {
    selected_edge_ = edge;
    applyEdgeSelection();
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
    QMenu menu;
    // Everything a node or a group answers for needs one under the cursor.
    // The two placement entries at the bottom do not: they are about the
    // canvas, so a right-click on empty ground still offers them.
    const bool on_entity = semantic_item != nullptr && !current_group_scope_;
    const QString kind =
        on_entity ? itemKind(*semantic_item) : QString{};
    const QString group_id =
        on_entity ? semantic_item->data(FramePlanGroupIdRole).toString()
                  : QString{};
    context_node_.reset();
    if (on_entity && kind == QLatin1String{FramePlanNodeItem}) {
        const FramePlanNodeKey clicked{
            semantic_item->data(FramePlanGraphRole).toString().toStdString(),
            semantic_item->data(FramePlanNameRole).toString().toStdString(),
        };
        // N-8: the two insert entries act after the node under the cursor, so
        // the cursor's node is remembered for the named actions as well.
        context_node_ = clicked;
        // Q-12: a graph node's task is edited where the node is, so the way
        // there comes first.
        if (open_in_graph_action_ != nullptr && openInGraphOffered &&
            openInGraphOffered(clicked)) {
            menu.addAction(open_in_graph_action_);
            menu.addSeparator();
        }
        // VI-3b: a variadic group can be fixed; sockets Fix sockets wrote
        // can be made variadic again.
        bool socket_entries = false;
        if (fix_sockets_action_ != nullptr &&
            variadic_nodes_.contains(clicked.name)) {
            menu.addAction(fix_sockets_action_);
            socket_entries = true;
        }
        if (variadic_sockets_action_ != nullptr && fixedSocketsOffered &&
            fixedSocketsOffered(clicked)) {
            menu.addAction(variadic_sockets_action_);
            socket_entries = true;
        }
        if (socket_entries) menu.addSeparator();
        if (add_pass_action_ != nullptr) menu.addAction(add_pass_action_);
        if (material_pass_action_ != nullptr)
            menu.addAction(material_pass_action_);
        if (add_task_action_ != nullptr) menu.addAction(add_task_action_);
        if (selected_nodes_.size() >= 2 &&
            selected_nodes_.contains(clicked)) {
            QAction *create = menu.addAction(
                QStringLiteral("Create session group from selection"));
            connect(create, &QAction::triggered, this, [this] {
                QMetaObject::invokeMethod(
                    this, [this] { createGroupFromSelection(); },
                    Qt::QueuedConnection);
            });
        }
        if (convex_hull_proposal_) {
            QAction *accept = menu.addAction(
                QStringLiteral("Accept proposed convex hull"));
            connect(accept, &QAction::triggered, this, [this] {
                QMetaObject::invokeMethod(
                    this, [this] { acceptConvexHullProposal(); },
                    Qt::QueuedConnection);
            });
        }
    }

    const std::vector<GroupDefinition> groups =
        on_entity && current_model_
            ? discoverGroups(*current_model_, session_groups_)
            : std::vector<GroupDefinition>{};
    const GroupDefinition *group =
        group_id.isEmpty() ? nullptr : findGroupById(groups, group_id);
    const bool session_group =
        group != nullptr && session_groups_.contains(group->state_key);

    if (on_entity && kind == QLatin1String{FramePlanGroupItem}) {
        QAction *expand = menu.addAction(QStringLiteral("Expand group"));
        connect(expand, &QAction::triggered, this,
                [this, group_id] {
                    QMetaObject::invokeMethod(
                        this,
                        [this, group_id] { expandGroup(group_id); },
                        Qt::QueuedConnection);
                });
    } else if (group != nullptr &&
               semantic_item
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
    } else if (group != nullptr) {
        QString reason =
            semantic_item->data(FramePlanReasonRole).toString();
        if (reason.isEmpty()) {
            reason = QStringLiteral("This group cannot be collapsed.");
        }
        QAction *disabled = menu.addAction(reason);
        disabled->setEnabled(false);
    }

    if (session_group) {
        QAction *remove =
            menu.addAction(QStringLiteral("Remove session group"));
        connect(remove, &QAction::triggered, this,
                [this, group_id] {
                    QMetaObject::invokeMethod(
                        this,
                        [this, group_id] {
                            removeSessionGroup(group_id);
                        },
                        Qt::QueuedConnection);
                });
    }
    if (!on_entity && render_target_action_ != nullptr)
        menu.addAction(render_target_action_);
    if (!on_entity && material_pass_action_ != nullptr)
        menu.addAction(material_pass_action_);
    if (!menu.actions().isEmpty()) menu.addSeparator();
    // The same two objects the automation surface fires by name, so a scenario
    // reaches them without driving a popup.
    if (auto_layout_action_ != nullptr) {
        menu.addAction(auto_layout_action_);
    } else {
        QAction *automatic = menu.addAction(QStringLiteral("Auto layout"));
        connect(automatic, &QAction::triggered, this, [this] {
            QMetaObject::invokeMethod(
                this, [this] { autoLayout(); }, Qt::QueuedConnection);
        });
    }
    if (fit_view_action_ != nullptr) {
        menu.addAction(fit_view_action_);
    } else if (fitViewRequested) {
        QAction *fit = menu.addAction(QStringLiteral("Fit view"));
        fit->setShortcut(QKeySequence{Qt::Key_F});
        connect(fit, &QAction::triggered, this, [this] {
            QMetaObject::invokeMethod(
                this, [this] { if (fitViewRequested) fitViewRequested(); },
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

namespace {
std::optional<FramePlanNodeKey> connectionNodeAt(QGraphicsScene &scene, QPointF point) {
    auto *item = scene.itemAt(point, QTransform{});
    while (item) {
        if (itemKind(*item) == QLatin1String{FramePlanGroupItem}) return std::nullopt;
        if (itemKind(*item) == QLatin1String{FramePlanNodeItem})
            return FramePlanNodeKey{item->data(FramePlanGraphRole).toString().toStdString(),
                                    item->data(FramePlanNameRole).toString().toStdString()};
        item = item->parentItem();
    }
    return std::nullopt;
}
}

namespace {
// The socket the point landed on, with everything the gesture needs: which
// node, which resource, and which side. A socket is a child of its node, so
// this is also how the node is found.
struct SocketHit {
    FramePlanNodeKey node;
    std::string resource;
    bool output = true;
    // VI-3a: a socket drawn from the shader's declarations.
    QString key;
    std::optional<std::size_t> input_index;
    std::optional<std::size_t> binding;
};

SocketHit socketHit(const QGraphicsItem &item) {
    SocketHit hit{
        FramePlanNodeKey{
            item.data(FramePlanGraphRole).toString().toStdString(),
            item.data(FramePlanNameRole).toString().toStdString()},
        item.data(FramePlanPortRole).toString().toStdString(),
        item.data(FramePlanSocketDirectionRole).toString() ==
            QLatin1String{"output"}};
    hit.key = item.data(FramePlanSocketKeyRole).toString();
    if (const QVariant index = item.data(FramePlanInputIndexRole);
        index.isValid())
        hit.input_index = static_cast<std::size_t>(index.toInt());
    if (const QVariant binding = item.data(FramePlanSocketBindingRole);
        binding.isValid())
        hit.binding = static_cast<std::size_t>(binding.toInt());
    return hit;
}

std::optional<SocketHit> socketAt(QGraphicsScene &scene, QPointF point) {
    for (QGraphicsItem *item : scene.items(point, Qt::IntersectsItemShape,
                                           Qt::DescendingOrder)) {
        if (itemKind(*item) != QLatin1String{FramePlanSocketItem}) continue;
        return socketHit(*item);
    }
    return std::nullopt;
}

// VI-3a. Where a drop on a declared pass landed. A socket is small, so the
// left part of its row counts as the socket; the rest of the node is its body.
FramePlanSocketDrop socketDropAt(QGraphicsScene &scene, QPointF point,
                                 const FramePlanNodeKey &node) {
    const auto aimed = [&](const SocketHit &hit) -> std::optional<FramePlanSocketDrop> {
        if (hit.output || hit.node != node) return std::nullopt;
        if (hit.key == QLatin1String{"in:variadic"})
            return FramePlanSocketDrop{FramePlanSocketDrop::Kind::variadic, 0};
        if (hit.binding)
            return FramePlanSocketDrop{FramePlanSocketDrop::Kind::socket,
                                       *hit.binding};
        return std::nullopt;
    };
    if (const auto hit = socketAt(scene, point))
        if (const auto drop = aimed(*hit)) return *drop;
    for (QGraphicsItem *item : scene.items()) {
        if (itemKind(*item) != QLatin1String{FramePlanSocketItem} ||
            item->parentItem() == nullptr)
            continue;
        const SocketHit hit = socketHit(*item);
        const auto drop = aimed(hit);
        if (!drop) continue;
        const QRectF body = item->parentItem()->sceneBoundingRect();
        const QPointF centre = item->sceneBoundingRect().center();
        const QRectF row{body.left() - FramePlanSocketSize,
                         centre.y() - FramePlanSocketRowHeight / 2.0,
                         body.width() * 0.4 + FramePlanSocketSize,
                         FramePlanSocketRowHeight};
        if (row.contains(point)) return *drop;
    }
    return FramePlanSocketDrop{};
}

QGraphicsItem *inlineFieldAt(QGraphicsScene &scene, QPointF point) {
    for (QGraphicsItem *item : scene.items(point, Qt::IntersectsItemShape,
                                           Qt::DescendingOrder))
        if (itemKind(*item) == QLatin1String{FramePlanInlineFieldItem})
            return item;
    return nullptr;
}

std::optional<FramePlanEdgeSelection> edgeAt(QGraphicsScene &scene,
                                             QPointF point) {
    for (QGraphicsItem *item : scene.items(point, Qt::IntersectsItemShape,
                                           Qt::DescendingOrder)) {
        if (itemKind(*item) != QLatin1String{FramePlanEdgeItem}) continue;
        FramePlanEdgeSelection edge;
        edge.graph = item->data(FramePlanGraphRole).toString().toStdString();
        edge.to_node =
            item->data(FramePlanToNameRole).toString().toStdString();
        for (const QString &resource :
             item->data(FramePlanResourcesRole).toStringList())
            edge.resources.push_back(resource.toStdString());
        if (edge.resources.empty()) continue; // an ordering-only dependency
        if (const QVariant index = item->data(FramePlanInputIndexRole);
            index.isValid())
            edge.input_index = static_cast<std::size_t>(index.toInt());
        return edge;
    }
    return std::nullopt;
}
} // namespace

namespace {

using FramePlanInlineEditFilter = InlineEditEventFilter;

} // namespace

// The line edit that sits over one band while it is being retyped. It is a
// widget in the scene rather than a dialog: the value is one string, and a
// dialog for one string is a place to lose the drawing.
struct FramePlanGraphicsScene::InlineEditor {
    FramePlanNodeKey node;
    QString field;
    QGraphicsProxyWidget *proxy = nullptr;
    QLineEdit *edit = nullptr;
    std::unique_ptr<FramePlanInlineEditFilter> filter;
};

bool FramePlanGraphicsScene::editingInline() const noexcept {
    return inline_editor_ != nullptr;
}

QString FramePlanGraphicsScene::inlineEditText() const {
    return inline_editor_ && inline_editor_->edit
               ? inline_editor_->edit->text()
               : QString{};
}

void FramePlanGraphicsScene::setInlineEditText(const QString &text) {
    if (inline_editor_ && inline_editor_->edit)
        inline_editor_->edit->setText(text);
}

void FramePlanGraphicsScene::closeInlineEditor() {
    if (!inline_editor_) return;
    auto editor = std::move(inline_editor_);
    inline_editor_.reset();
    if (editor->proxy == nullptr) return;
    // Closing usually happens from inside the line edit's own returnPressed,
    // so the widget must outlive this call. Taking the proxy out of the scene
    // first is what keeps a later clear() from deleting it a second time.
    if (editor->edit != nullptr) {
        editor->edit->disconnect(this);
        // Q-4. Taking the proxy out of the scene takes the keyboard focus off
        // the line edit, and that arrives at the filter as an ordinary focus
        // move -- which would commit an edit this call is in the middle of
        // ending, over whatever editor is open by the time the queued commit
        // runs. The editor being taken down is not the editor reporting that
        // it ended.
        if (editor->filter) {
            editor->edit->removeEventFilter(editor->filter.get());
            editor->filter.reset();
        }
    }
    removeItem(editor->proxy);
    editor->proxy->deleteLater();
}

void FramePlanGraphicsScene::cancelInlineEdit() { closeInlineEditor(); }

bool FramePlanGraphicsScene::commitInlineEdit() {
    if (!inline_editor_) return false;
    const FramePlanNodeKey node = inline_editor_->node;
    const QString field = inline_editor_->field;
    const QString value = inlineEditText();
    closeInlineEditor();
    if (!inlineFieldCommitted) return false;
    // Queued: committing sends an RPC, whose reply redraws this scene, and a
    // redraw from inside a key event takes the items out from under Qt.
    QMetaObject::invokeMethod(
        this,
        [this, node, field, value] {
            if (inlineFieldCommitted) inlineFieldCommitted(node, field, value);
        },
        Qt::QueuedConnection);
    return true;
}

bool FramePlanGraphicsScene::beginInlineEdit(const FramePlanNodeKey &node,
                                             const QString &field) {
    closeInlineEditor();
    QGraphicsItem *band = nullptr;
    for (QGraphicsItem *item : items()) {
        if (itemKind(*item) != QLatin1String{FramePlanInlineFieldItem} ||
            item->data(FramePlanGraphRole).toString().toStdString() !=
                node.graph ||
            item->data(FramePlanNameRole).toString().toStdString() !=
                node.name ||
            item->data(FramePlanInlineFieldRole).toString() != field)
            continue;
        band = item;
        break;
    }
    if (band == nullptr) return false;
    auto editor = std::make_unique<InlineEditor>();
    editor->node = node;
    editor->field = field;
    editor->edit = new QLineEdit;
    editor->edit->setObjectName(
        QStringLiteral("pelican.framePlanInlineEditor"));
    editor->edit->setText(band->data(FramePlanInlineValueRole).toString());
    editor->edit->selectAll();
    // T-1: a shader band completes to the shaders there are. The popup only
    // borrows focus (InlineEditEventFilter lets it), and the text is still
    // whatever is typed.
    if (inlineFieldChoices) {
        const QStringList choices = inlineFieldChoices(node, field);
        if (!choices.isEmpty()) {
            auto *completer = new QCompleter(choices, editor->edit);
            completer->setCaseSensitivity(Qt::CaseInsensitive);
            completer->setFilterMode(Qt::MatchContains);
            completer->setCompletionMode(QCompleter::PopupCompletion);
            editor->edit->setCompleter(completer);
        }
    }
    editor->proxy = addWidget(editor->edit);
    const QRectF bounds = band->sceneBoundingRect();
    editor->proxy->setGeometry(bounds);
    editor->proxy->setZValue(1200.0);
    // Q-4. Enter used to be the only way out of this editor. Escape, and a
    // focus move that really is one, now end it too -- and the window blinking
    // does not.
    editor->filter = std::make_unique<FramePlanInlineEditFilter>(
        [this](bool commit) {
            // The widget is still dispatching the event that ended the edit.
            QMetaObject::invokeMethod(
                this,
                [this, commit] {
                    if (commit) commitInlineEdit();
                    else cancelInlineEdit();
                },
                Qt::QueuedConnection);
        });
    editor->edit->installEventFilter(editor->filter.get());
    inline_editor_ = std::move(editor);
    inline_editor_->edit->setFocus();
    return true;
}

std::optional<FramePlanGraphicsScene::CarriedInlineEdit>
FramePlanGraphicsScene::carryInlineEdit() const {
    if (!inline_editor_) return std::nullopt;
    return CarriedInlineEdit{inline_editor_->node, inline_editor_->field,
                             inlineEditText()};
}

void FramePlanGraphicsScene::restoreInlineEdit(
    const CarriedInlineEdit &carried) {
    // A band the draw no longer holds simply has nothing to open over, and
    // the edit ends there -- the one case where a redraw may take it away.
    if (!beginInlineEdit(carried.node, carried.field)) return;
    setInlineEditText(carried.text);
    if (inline_editor_ && inline_editor_->edit != nullptr) {
        inline_editor_->edit->setFocus(Qt::OtherFocusReason);
    }
}

void FramePlanGraphicsScene::clearConnectionGesture() {
    delete connection_line_;
    connection_line_ = nullptr;
    connection_source_.reset();
    socket_gesture_.reset();
    connection_gesture_ = false;
}

bool FramePlanGraphicsScene::cancelConnectionGesture() {
    if (!connection_gesture_) return false;
    clearConnectionGesture();
    connection_gesture_ = true; // Consume the pending release.
    return true;
}

void FramePlanGraphicsScene::mousePressEvent(QGraphicsSceneMouseEvent *event) {
    if (event->button() != Qt::LeftButton || !(event->modifiers() & Qt::ShiftModifier)) {
        // N-8. A plain press is still a press: it drags nodes, it rubber-bands,
        // it selects. What is new is that a press that lands on a socket, on a
        // band, or on an edge means that thing rather than the node under it.
        if (event->button() == Qt::LeftButton &&
            event->modifiers() == Qt::NoModifier) {
            // A press inside the open editor belongs to the editor: it is how
            // a caret is placed. Only a press outside it closes it.
            if (inline_editor_ && inline_editor_->proxy != nullptr &&
                inline_editor_->proxy->sceneBoundingRect().contains(
                    event->scenePos())) {
                QGraphicsScene::mousePressEvent(event);
                return;
            }
            if (editingInline()) closeInlineEditor();
            // F-7: a resource node's socket is the start of a connection, not
            // a pick of the node it sits on.
            if (const auto resource = resourceNodeAt(*this, event->scenePos());
                resource && !socketAt(*this, event->scenePos())) {
                picked_resource_ = resource;
                if (selected_edge_) {
                    selected_edge_.reset();
                    applyEdgeSelection();
                }
                clearSelection();
                applyResourcePick();
                event->accept();
                return;
            }
            if (picked_resource_) {
                picked_resource_.reset();
                applyResourcePick();
            }
            if (QGraphicsItem *band = inlineFieldAt(*this, event->scenePos());
                band != nullptr) {
                const FramePlanNodeKey node{
                    band->data(FramePlanGraphRole).toString().toStdString(),
                    band->data(FramePlanNameRole).toString().toStdString()};
                const QString field =
                    band->data(FramePlanInlineFieldRole).toString();
                event->accept();
                beginInlineEdit(node, field);
                return;
            }
            // VI-3b: the band's Fix button.
            for (QGraphicsItem *hit : items(event->scenePos(),
                                            Qt::IntersectsItemShape,
                                            Qt::DescendingOrder)) {
                if (itemKind(*hit) != QLatin1String{FramePlanSocketFixItem})
                    continue;
                const FramePlanNodeKey node{
                    hit->data(FramePlanGraphRole).toString().toStdString(),
                    hit->data(FramePlanNameRole).toString().toStdString()};
                event->accept();
                QMetaObject::invokeMethod(
                    this,
                    [this, node] {
                        if (socketsFixRequested) socketsFixRequested(node);
                    },
                    Qt::QueuedConnection);
                return;
            }
            // VI-3a: an empty socket (a declaration nothing feeds, the
            // variadic band's end) has nothing to cut; a press on it is a
            // press on its node.
            if (const auto socket = socketAt(*this, event->scenePos());
                socket && (socket->output || !socket->resource.empty())) {
                clearConnectionGesture();
                connection_gesture_ = true;
                socket_gesture_ =
                    SocketGesture{socket->node, socket->resource,
                                  socket->output, socket->input_index};
                socket_gesture_->pressed_at = event->scenePos();
                socket_gesture_->binding = socket->binding;
                connection_source_ = socket->node;
                connection_line_ = addLine(
                    QLineF(event->scenePos(), event->scenePos()),
                    QPen(QColor{70, 200, 150}, 3, Qt::DashLine));
                connection_line_->setZValue(1000);
                connection_line_->setAcceptedMouseButtons(Qt::NoButton);
                event->accept();
                return;
            }
            // An edge is picked only where it runs over open canvas. A line
            // that happens to cross a box must not take the press away from
            // the box: dragging a node is the commoner gesture by far.
            if (!connectionNodeAt(*this, event->scenePos())) {
                if (const auto edge = edgeAt(*this, event->scenePos())) {
                    selected_edge_ = edge;
                    applyEdgeSelection();
                    event->accept();
                    return;
                }
            }
            if (selected_edge_) {
                selected_edge_.reset();
                applyEdgeSelection();
            }
        }
        QGraphicsScene::mousePressEvent(event);
        return;
    }
    clearConnectionGesture();
    connection_gesture_ = true;
    connection_source_ = connectionNodeAt(*this, event->scenePos());
    if (connection_source_) {
        connection_line_ = addLine(QLineF(event->scenePos(), event->scenePos()), QPen(QColor{70, 150, 240}, 3, Qt::DashLine));
        connection_line_->setZValue(1000);
        connection_line_->setAcceptedMouseButtons(Qt::NoButton);
    }
    event->accept();
}

void FramePlanGraphicsScene::mouseMoveEvent(QGraphicsSceneMouseEvent *event) {
    if (!connection_gesture_) { QGraphicsScene::mouseMoveEvent(event); return; }
    if (connection_line_) connection_line_->setLine(QLineF(connection_line_->line().p1(), event->scenePos()));
    event->accept();
}

void FramePlanGraphicsScene::mouseReleaseEvent(QGraphicsSceneMouseEvent *event) {
    if (!connection_gesture_) { QGraphicsScene::mouseReleaseEvent(event); return; }
    // Remove the preview before hit-testing the destination underneath it.
    delete connection_line_;
    connection_line_ = nullptr;
    const auto from = connection_source_;
    const auto socket = socket_gesture_;
    const auto to = connectionNodeAt(*this, event->scenePos());
    clearConnectionGesture();
    event->accept();
    // N-8: the plain gesture. It carries one named resource, so it edits the
    // declaration directly instead of opening a form to pick one.
    if (socket) {
        if (!resourceEdgeRequested) {
            if (connectionFeedback)
                connectionFeedback(tr("Resource connection editing is unavailable."));
            return;
        }
        const QStringList resources{qtext(socket->resource)};
        if (socket->output) {
            if (to && to->graph == socket->node.graph &&
                *to != socket->node && declared_nodes_.contains(to->name) &&
                socketDropRequested) {
                // VI-3a: the drop is aimed at a socket, and the socket is a
                // binding number.
                socketDropRequested(*to, qtext(socket->resource),
                                    socketDropAt(*this, event->scenePos(), *to));
            } else if (to && to->graph == socket->node.graph &&
                *to != socket->node) {
                resourceEdgeRequested(*to, resources, true);
            } else if (connectionFeedback) {
                connectionFeedback(
                    tr("Drop on another pass or task in the same graph to "
                       "connect %1.")
                        .arg(qtext(socket->resource)));
            }
            return;
        }
        // An input socket dragged away from its node cuts the connection; one
        // dropped back on the node it belongs to does nothing, because that is
        // how a gesture is abandoned.
        if (to && *to == socket->node) {
            // VI-3b: released where it was pressed, it is a click on the
            // socket (engine://passthrough shows that input).
            constexpr qreal click_slop = 4.0;
            if (socket->binding && socketClicked &&
                QLineF{socket->pressed_at, event->scenePos()}.length() <=
                    click_slop) {
                const FramePlanNodeKey node = socket->node;
                const std::size_t binding = *socket->binding;
                QMetaObject::invokeMethod(
                    this,
                    [this, node, binding] {
                        if (socketClicked) socketClicked(node, binding);
                    },
                    Qt::QueuedConnection);
            }
            return;
        }
        if (socket->input_index && socketCutRequested) {
            socketCutRequested(socket->node, *socket->input_index);
            return;
        }
        resourceEdgeRequested(socket->node, resources, false);
        return;
    }
    if (from && to && from->graph == to->graph && *from != *to) {
        if (connectionRequested) connectionRequested(*from, *to);
        else if (connectionFeedback) connectionFeedback(tr("Resource connection editing is unavailable."));
    } else if (from && connectionFeedback) {
        connectionFeedback(tr("Drop on a different task or fullscreen pass in the same graph to connect a resource."));
    }
}

void FramePlanGraphicsScene::mouseDoubleClickEvent(
    QGraphicsSceneMouseEvent *event) {
    if (const auto resource = resourceNodeAt(*this, event->scenePos());
        resource && resourceEditRequested) {
        event->accept();
        const FramePlanNodeKey key = *resource;
        QMetaObject::invokeMethod(
            this,
            [this, key] {
                if (resourceEditRequested) resourceEditRequested(key);
            },
            Qt::QueuedConnection);
        return;
    }
    QGraphicsItem *semantic_item = itemAt(event->scenePos(), QTransform{});
    while (semantic_item != nullptr &&
           itemKind(*semantic_item) !=
               QLatin1String{FramePlanGroupItem}) {
        semantic_item = semantic_item->parentItem();
    }
    if (semantic_item == nullptr) {
        // N-8: not a group, so it may be a node -- and a double-click on a node
        // is the way to everything the bands do not carry (attachments, raster
        // state, the draw, the material settings): the existing editors.
        if (const auto node = connectionNodeAt(*this, event->scenePos());
            node && nodeEditRequested) {
            event->accept();
            const FramePlanNodeKey key = *node;
            QMetaObject::invokeMethod(
                this,
                [this, key] {
                    if (nodeEditRequested) nodeEditRequested(key);
                },
                Qt::QueuedConnection);
            return;
        }
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
    if (event->key() == Qt::Key_Escape && editingInline()) {
        event->accept();
        cancelInlineEdit();
        return;
    }
    if (event->key() == Qt::Key_Escape && cancelConnectionGesture()) {
        event->accept();
        return;
    }
    // N-8. Delete cuts the picked edge, or removes the picked node. The edge
    // comes first: it is the smaller of the two, and it is what the user just
    // clicked on.
    if ((event->key() == Qt::Key_Delete ||
         event->key() == Qt::Key_Backspace) &&
        !editingInline()) {
        if (selected_edge_ && selected_edge_->input_index &&
            socketCutRequested) {
            // VI-3a: the edge ends on one input; that input goes.
            const FramePlanNodeKey consumer{selected_edge_->graph,
                                            selected_edge_->to_node};
            const std::size_t input_index = *selected_edge_->input_index;
            selected_edge_.reset();
            applyEdgeSelection();
            event->accept();
            QMetaObject::invokeMethod(
                this,
                [this, consumer, input_index] {
                    if (socketCutRequested)
                        socketCutRequested(consumer, input_index);
                },
                Qt::QueuedConnection);
            return;
        }
        if (selected_edge_ && resourceEdgeRequested) {
            const FramePlanNodeKey consumer{selected_edge_->graph,
                                            selected_edge_->to_node};
            QStringList resources;
            for (const auto &resource : selected_edge_->resources)
                resources.push_back(qtext(resource));
            selected_edge_.reset();
            applyEdgeSelection();
            event->accept();
            QMetaObject::invokeMethod(
                this,
                [this, consumer, resources] {
                    if (resourceEdgeRequested)
                        resourceEdgeRequested(consumer, resources, false);
                },
                Qt::QueuedConnection);
            return;
        }
        if (picked_resource_ && resourceRemoveRequested) {
            const FramePlanNodeKey resource = *picked_resource_;
            event->accept();
            QMetaObject::invokeMethod(
                this,
                [this, resource] {
                    if (resourceRemoveRequested)
                        resourceRemoveRequested(resource);
                },
                Qt::QueuedConnection);
            return;
        }
        if (selected_node_ && nodeRemoveRequested) {
            const FramePlanNodeKey node = *selected_node_;
            event->accept();
            QMetaObject::invokeMethod(
                this,
                [this, node] {
                    if (nodeRemoveRequested) nodeRemoveRequested(node);
                },
                Qt::QueuedConnection);
            return;
        }
    }
    if (event->key() == Qt::Key_Escape && current_group_scope_) {
        event->accept();
        QMetaObject::invokeMethod(
            this, [this] { leaveGroup(); }, Qt::QueuedConnection);
        return;
    }
    QGraphicsScene::keyPressEvent(event);
}

void FramePlanGraphicsScene::applyEdgeEmphasis() {
    // Dimming the rest, rather than recolouring the chosen ones, is what keeps
    // the dependency colours (order-only / barrier / fused) readable.
    const bool anything_selected = !selected_nodes_.empty();
    for (QGraphicsItem *item : items()) {
        const QString kind = itemKind(*item);
        if (kind != QLatin1String{FramePlanEdgeItem} &&
            kind != QLatin1String{FramePlanEdgeArrowItem}) {
            continue;
        }
        bool touches = false;
        for (const auto &node : selected_nodes_) {
            const QString name = qtext(node.name);
            if (item->data(FramePlanFromNameRole).toString() == name ||
                item->data(FramePlanToNameRole).toString() == name) {
                touches = true;
                break;
            }
        }
        item->setOpacity(!anything_selected || touches ? 1.0 : 0.38);
    }
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
    applyEdgeEmphasis();
    publishStateProperties();
}

void FramePlanGraphicsScene::setRenderTargetAction(QAction *action) {
    render_target_action_ = action;
}

void FramePlanGraphicsScene::setMaterialPassAction(QAction *action) {
    material_pass_action_ = action;
}

void FramePlanGraphicsScene::setOpenInGraphAction(QAction *action) {
    open_in_graph_action_ = action;
}

void FramePlanGraphicsScene::applyResourcePick() {
    for (QGraphicsItem *item : items()) {
        if (itemKind(*item) != QLatin1String{FramePlanResourceNodeItem})
            continue;
        const bool picked =
            picked_resource_ &&
            item->data(FramePlanNameRole).toString() ==
                qtext(picked_resource_->name);
        item->setData(FramePlanEdgePickedRole, picked);
        if (auto *rect = dynamic_cast<QGraphicsRectItem *>(item))
            rect->setPen(QPen{picked ? QColor{QStringLiteral("#f0c05a")}
                                     : QColor{QStringLiteral("#5f9fcf")},
                              picked ? 2.4 : 1.4});
    }
    setProperty("pelicanPickedResource",
                picked_resource_ ? qtext(picked_resource_->name) : QString{});
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
