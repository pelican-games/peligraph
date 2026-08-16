#pragma once

#include "../model/frameplanmodel.hpp"

#include <QGraphicsScene>

#include <optional>
#include <set>
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

inline constexpr auto FramePlanNodeItem = "node";
inline constexpr auto FramePlanGroupItem = "group";
inline constexpr auto FramePlanEdgeItem = "edge";
inline constexpr auto FramePlanEdgeArrowItem = "edge_arrow";
inline constexpr auto FramePlanNodeLabelItem = "node_label";
inline constexpr auto FramePlanGroupLabelItem = "group_label";
inline constexpr auto FramePlanEdgeLabelItem = "edge_label";

class FramePlanGraphicsScene final : public QGraphicsScene {
  public:
    explicit FramePlanGraphicsScene(QObject *parent = nullptr);

    void populate(const FramePlanModel &model, int group_minimum);
    void resetGraph();

    [[nodiscard]] const std::optional<FramePlanNodeKey> &selectedNode() const
        noexcept {
        return selected_node_;
    }

  private:
    std::optional<FramePlanNodeKey> selected_node_;
    std::set<std::string, std::less<>> collapsed_groups_;
    bool rebuilding_ = false;

    void recordSelection();
    void publishStateProperties();
};

} // namespace PelicanStudio
