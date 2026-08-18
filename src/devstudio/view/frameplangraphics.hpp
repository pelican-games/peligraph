#pragma once

#include "../model/frameplanmodel.hpp"

#include <QGraphicsScene>
#include <QPointF>

#include <map>
#include <optional>
#include <string>

namespace PelicanStudio {

// Data roles make the rendered scene inspectable without coupling callers to
// private QGraphicsItem subclasses.  Node identity always occupies the graph
// and name roles together.
inline constexpr int FramePlanItemKindRole = Qt::UserRole + 3060;
inline constexpr int FramePlanGraphRole = Qt::UserRole + 3061;
inline constexpr int FramePlanNameRole = Qt::UserRole + 3062;
inline constexpr int FramePlanSourceRole = Qt::UserRole + 3063;
inline constexpr int FramePlanColorRole = Qt::UserRole + 3064;
inline constexpr int FramePlanAnchorRole = Qt::UserRole + 3065;
inline constexpr int FramePlanMembersRole = Qt::UserRole + 3066;
inline constexpr int FramePlanFromNameRole = Qt::UserRole + 3067;
inline constexpr int FramePlanToNameRole = Qt::UserRole + 3068;
inline constexpr int FramePlanEdgeRecordsRole = Qt::UserRole + 3069;
inline constexpr int FramePlanResourcesRole = Qt::UserRole + 3070;
inline constexpr int FramePlanInternalEdgeRecordsRole = Qt::UserRole + 3071;
inline constexpr int FramePlanPhysicalStateRole = Qt::UserRole + 3072;
inline constexpr int FramePlanProfileRole = Qt::UserRole + 3073;
inline constexpr int FramePlanEndpointRole = Qt::UserRole + 3074;
inline constexpr int FramePlanReasonRole = Qt::UserRole + 3075;
inline constexpr int FramePlanWidestReadRole = Qt::UserRole + 3076;
inline constexpr int FramePlanAliasableRole = Qt::UserRole + 3077;
inline constexpr int FramePlanRepresentationRole = Qt::UserRole + 3078;
inline constexpr int FramePlanLifetimeUsedRole = Qt::UserRole + 3079;
inline constexpr int FramePlanLifetimeFirstRole = Qt::UserRole + 3080;
inline constexpr int FramePlanLifetimeLastRole = Qt::UserRole + 3081;
inline constexpr int FramePlanOpportunityKindRole = Qt::UserRole + 3082;
inline constexpr int FramePlanLogicalStateRole = Qt::UserRole + 3083;
inline constexpr int FramePlanReasonCodeRole = Qt::UserRole + 3084;
inline constexpr int FramePlanBundleOrderRole = Qt::UserRole + 3085;
inline constexpr int FramePlanCurveRole = Qt::UserRole + 3086;
inline constexpr int FramePlanSubtreeDepthRole = Qt::UserRole + 3087;

inline constexpr auto FramePlanNodeItem = "node";
inline constexpr auto FramePlanGroupItem = "group";
inline constexpr auto FramePlanEdgeItem = "edge";
inline constexpr auto FramePlanEdgeArrowItem = "edge_arrow";
inline constexpr auto FramePlanNodeLabelItem = "node_label";
inline constexpr auto FramePlanGroupLabelItem = "group_label";
inline constexpr auto FramePlanEdgeLabelItem = "edge_label";
inline constexpr auto FramePlanPhysicalContextItem = "physical_context";
inline constexpr auto FramePlanResourceLifetimeItem = "resource_lifetime";
inline constexpr auto FramePlanAliasOverlayItem = "alias_overlay";
inline constexpr auto FramePlanFusionOverlayItem = "fusion_overlay";
inline constexpr auto FramePlanParallelOverlayItem = "parallel_overlay";
inline constexpr auto FramePlanPhysicalEmptyItem = "physical_empty";
inline constexpr auto FramePlanPhysicalSelectionItem = "physical_selection";
inline constexpr auto FramePlanLogicalUnavailableItem = "logical_unavailable";

class FramePlanGraphicsScene final : public QGraphicsScene {
  public:
    explicit FramePlanGraphicsScene(QObject *parent = nullptr);

    // The selected target is the root resource.  Depth zero intentionally has
    // no pass nodes; depth one is exactly unique(writers U readers).  Further
    // depths cross resources touched by the preceding node frontier.
    void populate(const FramePlanModel &model,
                  const std::optional<FramePlanNodeKey> &selected_target,
                  int depth);
    void resetGraph();

    [[nodiscard]] const std::optional<FramePlanNodeKey> &selectedNode() const
        noexcept {
        return selected_node_;
    }
    [[nodiscard]] const std::optional<FramePlanNodeKey> &selectedResource() const
        noexcept {
        return selected_resource_;
    }

  private:
    std::optional<FramePlanNodeKey> selected_node_;
    std::optional<FramePlanNodeKey> selected_resource_;
    // Deliberately owned only by the live scene.  Node positions are editor
    // session state, not QMainWindow workspace state and never reach disk.
    std::map<FramePlanNodeKey, QPointF> session_node_positions_;
    bool rebuilding_ = false;

    void recordSelection();
    void publishStateProperties();
};

} // namespace PelicanStudio
