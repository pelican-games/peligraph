#pragma once

#include "../model/frameplanmodel.hpp"
#include "nodegraphics.hpp"

#include <QGraphicsScene>
#include <QPointF>

#include <QString>

#include <map>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

class QAction;
class QGraphicsSceneContextMenuEvent;
class QGraphicsSceneMouseEvent;
class QKeyEvent;

namespace PelicanStudio {

// Data roles make the rendered scene inspectable without coupling callers to
// private QGraphicsItem subclasses.  Node identity always occupies the graph
// and name roles together.
inline constexpr int FramePlanItemKindRole = NodeGraphItemKindRole;
inline constexpr int FramePlanGraphRole = NodeGraphGraphRole;
inline constexpr int FramePlanNameRole = NodeGraphNameRole;
inline constexpr int FramePlanSourceRole = Qt::UserRole + 3063;
inline constexpr int FramePlanColorRole = Qt::UserRole + 3064;
inline constexpr int FramePlanAnchorRole = Qt::UserRole + 3065;
inline constexpr int FramePlanMembersRole = Qt::UserRole + 3066;
inline constexpr int FramePlanFromNameRole = NodeGraphFromNameRole;
inline constexpr int FramePlanToNameRole = NodeGraphToNameRole;
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
inline constexpr int FramePlanLogicalStateRole = Qt::UserRole + 3083;
inline constexpr int FramePlanReasonCodeRole = Qt::UserRole + 3084;
inline constexpr int FramePlanBundleOrderRole = Qt::UserRole + 3085;
inline constexpr int FramePlanCurveRole = Qt::UserRole + 3086;
inline constexpr int FramePlanGroupIdRole = Qt::UserRole + 3088;
inline constexpr int FramePlanGroupCollapsibleRole = Qt::UserRole + 3089;
inline constexpr int FramePlanBoundaryTargetRole = Qt::UserRole + 3090;
inline constexpr int FramePlanBoundaryDirectionRole = Qt::UserRole + 3091;
inline constexpr int FramePlanInternalBarrierCountRole = Qt::UserRole + 3097;
inline constexpr int FramePlanInternalFusedBarrierCountRole =
    Qt::UserRole + 3098;
inline constexpr int FramePlanSessionGroupRole = Qt::UserRole + 3099;
inline constexpr int FramePlanConvexHullMissingMembersRole =
    Qt::UserRole + 3100;
inline constexpr int FramePlanConvexHullImpracticalRole =
    Qt::UserRole + 3101;
inline constexpr int FramePlanGpuTimingStateRole = Qt::UserRole + 3102;
inline constexpr int FramePlanGpuTimingBodyMsRole = Qt::UserRole + 3103;
inline constexpr int FramePlanGpuTimingBarriersMsRole = Qt::UserRole + 3104;
inline constexpr int FramePlanGpuTimingSampleCountRole = Qt::UserRole + 3105;
inline constexpr int FramePlanGpuTimingFrameCountRole = Qt::UserRole + 3106;
inline constexpr int FramePlanGpuTimingWindowSizeRole = Qt::UserRole + 3107;
inline constexpr int FramePlanGpuTimingNodeNameRole = Qt::UserRole + 3108;
// N-3: the node is drawn as a heading band over an output section and an input
// section, the way the common graph draws one. These name the pieces.
inline constexpr int FramePlanPortRole = Qt::UserRole + 3109;
// "input" | "output" -- the same resource can be read and written by one pass,
// so the side is part of a socket's identity.
inline constexpr int FramePlanSocketDirectionRole = Qt::UserRole + 3110;
// N-8: the in-place band. The field id is what the edit means
// ("shader.vertex", "shader.fragment", "dispatch"); the value is what is in it
// now, so a test can read the band without measuring its text.
inline constexpr int FramePlanInlineFieldRole = Qt::UserRole + 3111;
inline constexpr int FramePlanInlineValueRole = Qt::UserRole + 3112;
// The edge the user picked. Delete acts on it, so it has to be visible as a
// fact about the item and not only as a thicker line.
inline constexpr int FramePlanEdgePickedRole = Qt::UserRole + 3113;
// Nothing else is published: a socket's type, shape and a node's kind are
// what the canvas DRAWS (the socket path, its colour, the kind text), and a
// role restating them would be a second answer nothing reads.

inline constexpr auto FramePlanNodeItem = "node";
inline constexpr auto FramePlanGroupItem = "group";
inline constexpr auto FramePlanEdgeItem = "edge";
inline constexpr auto FramePlanEdgeArrowItem = "edge_arrow";
inline constexpr auto FramePlanNodeLabelItem = "node_label";
inline constexpr auto FramePlanGroupLabelItem = "group_label";
inline constexpr auto FramePlanBoundaryStubItem = "boundary_stub";
inline constexpr auto FramePlanGroupWarningItem = "group_warning";
inline constexpr auto FramePlanConvexHullProposalItem =
    "convex_hull_proposal";
inline constexpr auto FramePlanBreadcrumbItem = "group_breadcrumb";
inline constexpr auto FramePlanEdgeLabelItem = "edge_label";
inline constexpr auto FramePlanPhysicalContextItem = "physical_context";
inline constexpr auto FramePlanResourceLifetimeItem = "resource_lifetime";
inline constexpr auto FramePlanAliasOverlayItem = "alias_overlay";
inline constexpr auto FramePlanPhysicalSelectionItem = "physical_selection";
inline constexpr auto FramePlanLogicalUnavailableItem = "logical_unavailable";
// The heading band, one socket, and one resource-row name. They carry the node
// they belong to in FramePlanNameRole, exactly as the node label does.
inline constexpr auto FramePlanNodeHeaderItem = "node_header";
inline constexpr auto FramePlanNodeKindLabelItem = "node_kind_label";
inline constexpr auto FramePlanSocketItem = "node_socket";
inline constexpr auto FramePlanRowLabelItem = "node_row_label";
// N-8: one retypeable field of an editable node, drawn as a band under the
// heading. A node that cannot be edited draws none: a band that refused on
// Enter would be an invitation that is not one.
inline constexpr auto FramePlanInlineFieldItem = "node_inline_field";
// F-3: a node whose shader the engine could not compile on its last reload.
// It carries the node in FramePlanNameRole, and the first log line as its
// tooltip; it is gone when the next reading has no error for the node.
inline constexpr auto FramePlanNodeShaderErrorItem = "node_shader_error";
// F-1: the selected target drawn as a node of its own, beside the passes that
// touch it. Double-click opens its declaration; Delete asks to remove it.
inline constexpr auto FramePlanResourceNodeItem = "resource_node";

// A node's height follows its resource count rather than a constant, so the
// numbers the scene draws with are published for the scene test to name.
inline constexpr qreal FramePlanNodeMinimumWidth = NodeGraphNodeWidth;
inline constexpr qreal FramePlanNodeMaximumWidth = 360.0;
inline constexpr qreal FramePlanHeaderHeight = 34.0;
inline constexpr qreal FramePlanSocketRowHeight = 21.0;
inline constexpr qreal FramePlanSectionPadding = 7.0;
inline constexpr qreal FramePlanSocketSize = 11.0;
inline constexpr qreal FramePlanNodeCornerRadius = 9.0;

// What the shared socket resolver is told about one frame-plan resource. A
// frame-plan resource carries a kind, not a semantic type id, so the canvas
// names the type itself -- "frame_plan.image" and friends -- and the default
// table answers for those names like any other.
SocketTypeFacts framePlanSocketFacts(const FramePlanModel &model,
                                     const std::string &resource,
                                     bool history);

// N-8. One band the canvas offers on a node, supplied by the owner because
// only the owner has the authoring context that says whether the field can be
// written at all.
struct FramePlanInlineBand {
    QString field;
    QString label;
    QString value;
};

// The edge the user picked, named by the end that is unique: one input of one
// node takes one bundle. Delete on it disconnects every resource it carries,
// in one request.
struct FramePlanEdgeSelection {
    std::string graph;
    std::string to_node;
    std::vector<std::string> resources;

    auto operator<=>(const FramePlanEdgeSelection &) const = default;
};

struct FramePlanGpuTimingRow {
    std::uint32_t view_index = 0;
    std::string node_kind;
    std::string node_name;
    double barriers_ms = 0.0;
    double body_ms = 0.0;
    bool body_supported = true;
    std::size_t sample_count = 0;
};

struct FramePlanGpuTimingSnapshot {
    bool received = false;
    bool enabled = false;
    bool supported = false;
    std::string reason = "waiting_for_rpc";
    std::size_t window_size = 0;
    std::size_t frame_count = 0;
    std::vector<FramePlanGpuTimingRow> nodes;
};

class FramePlanGraphicsScene final : public QGraphicsScene {
  public:
    explicit FramePlanGraphicsScene(QObject *parent = nullptr);
    ~FramePlanGraphicsScene() override;

    // The selected target is the root resource.  Depth zero intentionally has
    // no pass nodes; depth one is exactly unique(writers U readers).  Further
    // depths cross resources touched by the preceding node frontier.
    void populate(const FramePlanModel &model,
                  const std::optional<FramePlanNodeKey> &selected_target,
                  int depth);
    // F-7: the whole frame. Every pass is drawn, and every declared target no
    // pass writes -- a target only declared so far among them -- is drawn as
    // a resource node beside them, so the target and the pass that should
    // read it are on one canvas and a drag can join them. populate() leaves
    // this view again.
    void populateWholeFrame(const FramePlanModel &model);
    [[nodiscard]] bool wholeFrame() const noexcept { return whole_frame_; }
    void resetGraph();
    bool cancelConnectionGesture();
    std::function<void(const FramePlanNodeKey &, const FramePlanNodeKey &)> connectionRequested;
    std::function<void(const QString &)> connectionFeedback;
    // The scene's context menu offers "Fit view"; only the view can perform
    // it, so the owner wires this to the view that shows this scene.
    std::function<void()> fitViewRequested;
    // "Auto layout" (N-4): drop the positions moved by hand in this graph and
    // place it again. Positions are editor session state, so this changes
    // nothing the engine or the document knows about.
    void autoLayout();
    // The two menu entries the owner named. A named QAction can be fired by
    // the automation surface without opening a popup, so the scene is handed
    // the entries rather than building a throwaway one per right-click.
    void setMenuActions(QAction *auto_layout, QAction *fit_view);
    void setGpuTiming(FramePlanGpuTimingSnapshot snapshot);
    // F-3: node name -> the first line of its shader's compile log. Kept
    // across redraws; an empty map takes every mark down.
    void setShaderErrorMarks(std::map<std::string, QString> marks);
    [[nodiscard]] const std::map<std::string, QString> &shaderErrorMarks()
        const noexcept {
        return shader_error_marks_;
    }

    // --- N-8: editing on the canvas.

    // A plain drag from an output socket onto a node asks for a connection; a
    // plain drag off an input socket, or Delete on a picked edge, asks for the
    // cut. Shift-drag is untouched and still opens the dialogs.
    std::function<void(const FramePlanNodeKey &consumer,
                       const QStringList &resources, bool connect)>
        resourceEdgeRequested;
    // Delete on a picked node.
    std::function<void(const FramePlanNodeKey &)> nodeRemoveRequested;
    // Double-click on a node body: the detailed editor for everything the
    // bands do not carry.
    std::function<void(const FramePlanNodeKey &)> nodeEditRequested;
    // "Add fullscreen pass..." / "Add compute task...", with the node the
    // right-click landed on, so the new one goes after it.
    std::function<void(const FramePlanNodeKey &after, bool task)>
        nodeAddRequested;
    // A band was retyped and committed with Enter.
    std::function<void(const FramePlanNodeKey &, const QString &field,
                       const QString &value)>
        inlineFieldCommitted;
    // What bands a node offers. Asked once per node per draw.
    std::function<std::vector<FramePlanInlineBand>(const FramePlanNodeKey &)>
        inlineFieldProvider;
    // A node was moved, or the placement was dropped. The owner writes it.
    std::function<void()> placementChanged;
    // F-1: double-click on the resource node, and Delete while it is picked.
    std::function<void(const FramePlanNodeKey &)> resourceEditRequested;
    std::function<void(const FramePlanNodeKey &)> resourceRemoveRequested;
    // "New render target..." -- offered on a right-click over open canvas.
    void setRenderTargetAction(QAction *action);
    [[nodiscard]] const std::optional<FramePlanNodeKey> &pickedResource()
        const noexcept {
        return picked_resource_;
    }
    // The two insert entries, named so the automation surface can fire them
    // without driving a popup -- the same arrangement N-4 made for placement.
    void setInsertActions(QAction *add_pass, QAction *add_task);
    // The node the last right-click landed on, or the current selection.
    [[nodiscard]] std::optional<FramePlanNodeKey> insertAnchor() const;

    // The placement the document holds for one graph, and the one the canvas
    // is drawing. Positions the user has moved live here; everything else is
    // the automatic placement and is not written.
    void setPlacement(const std::string &graph,
                      const std::map<std::string, QPointF> &nodes);
    [[nodiscard]] std::map<std::string, QPointF> placement(
        const std::string &graph) const;

    bool beginInlineEdit(const FramePlanNodeKey &node, const QString &field);
    [[nodiscard]] bool editingInline() const noexcept;
    [[nodiscard]] QString inlineEditText() const;
    void setInlineEditText(const QString &text);
    bool commitInlineEdit();
    void cancelInlineEdit();

    [[nodiscard]] const std::optional<FramePlanEdgeSelection> &selectedEdge()
        const noexcept {
        return selected_edge_;
    }
    // Picking an edge without a mouse: the same thing a click on it does.
    void selectEdge(const FramePlanEdgeSelection &edge);

    // These are the state-changing operations used by the scene's context
    // menu, double-click, breadcrumb, and Escape handlers. Group ids are read
    // from FramePlanGroupIdRole and remain stable across frame-plan updates.
    bool collapseGroup(const QString &group_id);
    bool expandGroup(const QString &group_id);
    bool enterGroup(const QString &group_id);
    bool leaveGroup();

    // A session group is made from the live WP349 selection and never leaves
    // this scene. A non-convex selection records a visible convex-hull
    // proposal instead of creating a group; accepting that proposal installs
    // the rechecked hull. Only session groups can be removed here -- authored
    // regions and provider-feature groups remain read-only config views.
    bool createGroupFromSelection();
    bool acceptConvexHullProposal();
    bool removeSessionGroup(const QString &group_id);

    [[nodiscard]] const std::optional<FramePlanNodeKey> &selectedNode() const
        noexcept {
        return selected_node_;
    }
    [[nodiscard]] const std::set<FramePlanNodeKey> &selectedNodes() const
        noexcept {
        return selected_nodes_;
    }
    [[nodiscard]] const std::optional<FramePlanNodeKey> &selectedResource() const
        noexcept {
        return selected_resource_;
    }

  protected:
    void contextMenuEvent(QGraphicsSceneContextMenuEvent *event) override;
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private:
    // selected_node_ is the compatibility/primary selection consumed by the
    // existing details widget and pelicanSelectedNode property. The complete
    // selection is retained separately for rubber-band and Ctrl selection.
    std::optional<FramePlanNodeKey> selected_node_;
    std::set<FramePlanNodeKey> selected_nodes_;
    std::optional<FramePlanNodeKey> selected_resource_;
    // Deliberately owned only by the live scene.  Node positions are editor
    // session state, not QMainWindow workspace state and never reach disk.
    // Display names are deliberately absent from this key. In particular, a
    // group's synthetic endpoint name may acquire or lose a collision suffix
    // without changing the editor-session position that belongs to the group.
    std::map<std::tuple<std::string, std::string, std::string>, QPointF>
        session_node_positions_;
    std::optional<FramePlanModel> current_model_;
    std::optional<FramePlanNodeKey> current_target_;
    bool whole_frame_ = false;
    FramePlanGpuTimingSnapshot gpu_timing_;
    std::map<std::string, QString> shader_error_marks_;
    int current_depth_ = 1;
    using GroupStateKey = std::pair<std::string, std::string>;
    std::set<GroupStateKey> collapsed_groups_;
    std::optional<GroupStateKey> current_group_scope_;
    std::map<GroupStateKey, std::vector<std::string>> session_groups_;
    std::size_t next_session_group_id_ = 1;
    struct ConvexHullProposalState {
        std::string graph;
        std::vector<std::string> selected_members;
        std::vector<std::string> hull_members;
        std::vector<std::string> missing_members;
        bool impractical = false;
        QString message;
    };
    std::optional<ConvexHullProposalState> convex_hull_proposal_;
    QAction *auto_layout_action_ = nullptr;
    QAction *fit_view_action_ = nullptr;
    QString group_feedback_;
    // Never call this from inside itemChange: changing the scene rect makes
    // the view update, which moves the dragged item, which re-enters
    // itemChange. Schedule it instead; requests coalesce.
    void scheduleSceneRectUpdate();
    void applySceneRectNow();

    std::optional<FramePlanNodeKey> connection_source_;
    QGraphicsLineItem *connection_line_ = nullptr;
    bool connection_gesture_ = false;
    // N-8. A plain drag that started on a socket. Shift-drag leaves this unset
    // and keeps its own path through the dialogs.
    struct SocketGesture {
        FramePlanNodeKey node;
        std::string resource;
        bool output = true;
    };
    std::optional<SocketGesture> socket_gesture_;
    std::optional<FramePlanEdgeSelection> selected_edge_;
    struct InlineEditor;
    std::unique_ptr<InlineEditor> inline_editor_;
    // Q-4. An open in-place editor, said in terms that outlive the items it
    // sits on. A redraw deletes every item, the editor's proxy included, so
    // the editor is opened again over the new ones rather than taken out from
    // under the person typing into it -- and what they had typed comes with it.
    struct CarriedInlineEdit {
        FramePlanNodeKey node;
        QString field;
        QString text;
    };
    QAction *add_pass_action_ = nullptr;
    QAction *add_task_action_ = nullptr;
    QAction *render_target_action_ = nullptr;
    // F-1: the resource node the user clicked. Delete acts on it.
    std::optional<FramePlanNodeKey> picked_resource_;
    void applyResourcePick();
    std::optional<FramePlanNodeKey> context_node_;
    void clearConnectionGesture();
    void applyEdgeSelection();
    void closeInlineEditor();
    // The two halves of carrying an editor across a redraw.
    [[nodiscard]] std::optional<CarriedInlineEdit> carryInlineEdit() const;
    void restoreInlineEdit(const CarriedInlineEdit &carried);
    bool rebuilding_ = false;
    bool scene_rect_update_queued_ = false;
    // The theme table's registration, dropped in the destructor.
    int theme_listener_ = 0;

    void renderCurrentGraph();
    // Only the edges that touch a selected node stay at full strength. The
    // edge's own colour still says what the dependency is; the emphasis is
    // applied on top of it so a selection never hides that fact.
    void applyEdgeEmphasis();
    void applyShaderErrorMarks();
    void pruneGroupState();
    bool installSessionGroup(std::vector<std::string> members);
    void recordSelection();
    void publishStateProperties();
};

} // namespace PelicanStudio
