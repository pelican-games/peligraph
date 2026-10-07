#pragma once

#include "../model/graphcanvasmodel.hpp"
#include "frameplanedit.hpp"
#include "graphcanvasreadcapability.hpp"
#include "nodegraphics.hpp"

#include <QAction>
#include <QGraphicsScene>
#include <QPointF>
#include <QStringList>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QAction;
class QMenu;
class QTabWidget;
class QTimer;
class QGraphicsSceneContextMenuEvent;
class QGraphicsSceneMouseEvent;
class QKeyEvent;

namespace PelicanStudio {

class TabEditOrder;
class RenderEditHistory;

class RenderConfigDigest;

// Data roles make the rendered canvas inspectable without coupling callers to
// private QGraphicsItem subclasses. Identity is the scope plus the node id,
// which is exactly the key a retained position is filed under.
inline constexpr int GraphCanvasOpRole = Qt::UserRole + 3120;
inline constexpr int GraphCanvasDomainRole = Qt::UserRole + 3121;
inline constexpr int GraphCanvasEvaluationRole = Qt::UserRole + 3122;
inline constexpr int GraphCanvasSinkRole = Qt::UserRole + 3123;
inline constexpr int GraphCanvasColumnRole = Qt::UserRole + 3124;
inline constexpr int GraphCanvasDefinitionRole = Qt::UserRole + 3125;
inline constexpr int GraphCanvasInputPortsRole = Qt::UserRole + 3126;
inline constexpr int GraphCanvasOutputPortsRole = Qt::UserRole + 3127;
inline constexpr int GraphCanvasDeclaredPortsRole = Qt::UserRole + 3128;
inline constexpr int GraphCanvasFromPortRole = Qt::UserRole + 3129;
inline constexpr int GraphCanvasToPortRole = Qt::UserRole + 3130;
inline constexpr int GraphCanvasPortRole = Qt::UserRole + 3131;
// "input" | "output" -- which side of the node the socket belongs to, so the
// same port name on both sides stays two distinct endpoints.
inline constexpr int GraphCanvasSocketDirectionRole = Qt::UserRole + 3132;
inline constexpr int GraphCanvasPortTypeRole = Qt::UserRole + 3133;
inline constexpr int GraphCanvasSocketShapeRole = Qt::UserRole + 3134;
inline constexpr int GraphCanvasLabelRole = Qt::UserRole + 3135;
inline constexpr int GraphCanvasValueTextRole = Qt::UserRole + 3136;
inline constexpr int GraphCanvasValueSourceRole = Qt::UserRole + 3137;
// True on the heading band and on everything drawn inside it. A double-click
// there renames the node; anywhere else on the node it descends an instance,
// and one flag keeps that split from depending on which child was hit.
inline constexpr int GraphCanvasHeadingRole = Qt::UserRole + 3138;
inline constexpr int GraphCanvasCollapsedRole = Qt::UserRole + 3139;
// Which control point of an edge, and which slot of a bundle. The automation
// surface builds `graph_reroute#<node>/<port>/<index>` and `graph_tie#<index>`
// out of the name and port roles; these two are what a reader of the item
// needs in order to say the same thing without parsing a string.
inline constexpr int GraphCanvasControlIndexRole = Qt::UserRole + 3140;
inline constexpr int GraphCanvasTieSlotRole = Qt::UserRole + 3141;
// LP-4c: a node the last refused edit pointed at carries the sentence the
// refusal read as (empty otherwise); it is drawn with a red rim.
inline constexpr int GraphCanvasRefusalRole = Qt::UserRole + 3142;
// LP-4d1: an item drawn inside a loop's zone on the root canvas -- a body
// node, its sockets, pills, edges and control points -- carries its
// definition here as well as in its scope. The automation surface puts it in
// front of the item's name (`graph_node#<definition>/<id>`), because a body
// node's id can be a root node's too; an item drawn in the scope on screen
// carries nothing, and keeps the name it always had.
inline constexpr int GraphCanvasZoneScopeRole = Qt::UserRole + 3143;
// A zone's tab: the branch it shows (an index into the loop's branches).
inline constexpr int GraphCanvasBranchRole = Qt::UserRole + 3144;
// LP-4d2. Which edge of its frame a thing on a zone's frame stands on, so the
// frame can follow its body during a drag without being drawn again
// (decision 7): GraphCanvasZoneAnchor below.
inline constexpr int GraphCanvasZoneAnchorRole = Qt::UserRole + 3145;
// Decision 4: the element a line into a loop reads of another loop's `p[]`
// ("element", "2"); empty on an ordinary edge. Such a line is dashed.
inline constexpr int GraphCanvasEdgeElementRole = Qt::UserRole + 3146;
// How tall the line along a zone's bottom is, on the carry arrows that run
// above it.
inline constexpr int GraphCanvasZoneFooterRole = Qt::UserRole + 3147;
// Q-19. An input socket drawn from a render node's shader declarations
// carries its key here -- "in@<binding>" or "in:variadic", which is also its
// port role, so its automation name is `graph_socket#<node>/in@0` -- and,
// when a numbered port is connected there, that port's name, which is what
// the document's edge names and what the edge is drawn to.
inline constexpr int GraphCanvasSocketKeyRole = Qt::UserRole + 3148;
inline constexpr int GraphCanvasBoundPortRole = Qt::UserRole + 3149;
// GL-1: true on a folded node (a feature, a pass no graph node made) and on
// everything drawn for it. Such a node is read only: nothing on it edits.
inline constexpr int GraphCanvasDerivedRole = Qt::UserRole + 3150;
// GL-1, decision 5: the domain a stub's hidden nodes are in.
inline constexpr int GraphCanvasStubDomainRole = Qt::UserRole + 3151;
// GL-1e: on an input socket of a rendering-section pass that takes lines, and
// on the line drawn to one: the index in the pass's `input` it stands for, or
// -1 for the empty `+ input` slot (whose port is "in:+").
inline constexpr int GraphCanvasPassInputRole = Qt::UserRole + 3152;
enum class GraphCanvasZoneAnchor : int {
    none = 0,
    left = 1,
    right = 2,
    bottom = 3,
    // A mark in the heading band that keeps to its right end.
    band_right = 4,
};
inline constexpr int GraphCanvasScopeRole = NodeGraphGraphRole;

inline constexpr auto GraphCanvasNodeItem = "graph_node";
inline constexpr auto GraphCanvasNodeLabelItem = "graph_node_label";
inline constexpr auto GraphCanvasNodeHeaderItem = "graph_node_header";
inline constexpr auto GraphCanvasSocketItem = "graph_socket";
inline constexpr auto GraphCanvasValuePillItem = "graph_value_pill";
// AG-3b: what the published generation left on an output port. It is a band
// and not a pill because it is not editable: a pill is a number the document
// stores and a click opens, this is a reading of what came out.
inline constexpr auto GraphCanvasValueBandItem = "graph_value_band";
inline constexpr auto GraphCanvasEdgeItem = "graph_edge";
inline constexpr auto GraphCanvasEdgeArrowItem = "graph_edge_arrow";
inline constexpr auto GraphCanvasBreadcrumbItem = "graph_breadcrumb";
// The fold triangle in the heading band. Its own kind, because a press on it
// must fold the node rather than start dragging it.
inline constexpr auto GraphCanvasCollapseItem = "graph_node_collapse";
// The line that follows the cursor between grabbing a socket and dropping on
// one. It is not an edge: nothing in the document answers for it yet.
inline constexpr auto GraphCanvasPendingEdgeItem = "graph_pending_edge";
// The in-place editor a pill or a heading opens.
inline constexpr auto GraphCanvasEditorItem = "graph_inline_editor";
// N-6: the two control points. A reroute is one dot on one edge; a tie is one
// bar several edges run through side by side. Both are absolute -- a node that
// moves does not carry them -- which is what makes a bend outlive a drag.
inline constexpr auto GraphCanvasRerouteItem = "graph_reroute";
inline constexpr auto GraphCanvasTieItem = "graph_tie";
// LP-4d1, the loop zone. The frame itself is the loop's `graph_node` (so it
// moves, is picked and is named as the loop always was); these are what it
// holds. `graph_zone#<loop>` is the inside of the frame, where the body is
// drawn; `graph_zone_tab#<loop>/<branch>` one branch's tab;
// `graph_zone_socket#<loop>/out:<port>` the inside end of one body output on
// the right edge (the outside ends are the loop's own sockets, and a body's
// boundary input is its graph.input's `value` socket on the left edge);
// `graph_zone_link` the line from the node that makes a body output to it;
// `graph_zone_label#<loop>` the zone's stage and `shared` marks.
inline constexpr auto GraphCanvasZoneItem = "graph_zone";
inline constexpr auto GraphCanvasZoneTabItem = "graph_zone_tab";
inline constexpr auto GraphCanvasZoneSocketItem = "graph_zone_socket";
inline constexpr auto GraphCanvasZoneLinkItem = "graph_zone_link";
inline constexpr auto GraphCanvasZoneLabelItem = "graph_zone_label";
// LP-4d2. `graph_zone_socket#<loop>/in:+` and `.../out:+` are the empty
// slots at the end of the two edges (decisions 1 and 2);
// `graph_zone_carry#<loop>/<from>/<to>` one pair of the loop's carry, an
// arrow round the bottom of the frame (picked, Delete removes it), with
// `graph_zone_carry_mark#...` beside it when `to` has no loop input edge;
// `graph_zone_label#<loop>/expansion` the line along the bottom (`×N
// copies`, `N materials`) and `graph_zone_button#<loop>/frame_plan` its Show
// in Frame Plan; `graph_edge_label#<node>/<port>` the element a dashed line
// reads.
inline constexpr auto GraphCanvasZoneCarryItem = "graph_zone_carry";
inline constexpr auto GraphCanvasZoneCarryMarkItem = "graph_zone_carry_mark";
inline constexpr auto GraphCanvasZoneButtonItem = "graph_zone_button";
inline constexpr auto GraphCanvasEdgeLabelItem = "graph_edge_label";
// GL-1. A folded node is a `graph_node` (named by its id: the pass's name, or
// `feature:<name>`) carrying GraphCanvasDerivedRole; its rows' ends are
// `graph_derived_socket#<id>/<resource>`; `graph_derived_edge#<from>-><to>`
// is a line the frame plan implies (dashed, read only);
// `graph_domain_stub#<node>/<port>` the cut end of a line to a node the
// domain filter hides; `graph_folded_button#show_decomposed` the button under
// a feature's breadcrumb.
inline constexpr auto GraphCanvasDerivedSocketItem = "graph_derived_socket";
inline constexpr auto GraphCanvasDerivedEdgeItem = "graph_derived_edge";
inline constexpr auto GraphCanvasDomainStubItem = "graph_domain_stub";
inline constexpr auto GraphCanvasFoldedButtonItem = "graph_folded_button";
// GL-1e. `graph_pass_input_edge#<from>-><pass>`: a line the rendering
// section's document names -- one item of a pass's `input` -- drawn solid from
// the node whose output is that resource. Picked, Delete takes it out.
inline constexpr auto GraphCanvasPassInputEdgeItem = "graph_pass_input_edge";
// GL-2b, decision 2. `graph_layer_band#<id>`: the line under a root node
// that says what its lower layers hold (`×3 copies · 2 barriers`).
inline constexpr auto GraphCanvasLayerBandItem = "graph_layer_band";

// The node is a heading band over two socket sections, so its height follows
// its port count rather than a constant. The numbers are here so the scene
// test can name the same geometry the canvas draws.
// UX-5a: the least zoom Fit view goes to -- a node's words still readable in
// the bottom dock. Content bigger than that is looked at by the picked node.
inline constexpr qreal GraphCanvasFitFloor = 0.7;
inline constexpr qreal GraphCanvasNodeMinimumWidth = 196.0;
inline constexpr qreal GraphCanvasNodeMaximumWidth = 340.0;
inline constexpr qreal GraphCanvasHeaderHeight = 32.0;
inline constexpr qreal GraphCanvasSocketRowHeight = 21.0;
inline constexpr qreal GraphCanvasSectionPadding = 7.0;
inline constexpr qreal GraphCanvasSocketSize = 11.0;
inline constexpr qreal GraphCanvasNodeCornerRadius = 8.0;
// N-6. A reroute is drawn as a dot this wide. A tie is a bar of this width,
// carrying one edge per slot `GraphCanvasTieSpacing` apart; each edge crosses
// it level, entering and leaving `GraphCanvasTieReach` either side of its
// centre, which is what makes a bundle read as parallel lines rather than as
// several lines that happen to meet.
inline constexpr qreal GraphCanvasRerouteSize = 9.0;
inline constexpr qreal GraphCanvasTieWidth = 10.0;
inline constexpr qreal GraphCanvasTieReach = 9.0;
inline constexpr qreal GraphCanvasTieSpacing = 16.0;
inline constexpr qreal GraphCanvasTiePadding = 7.0;
// LP-4d1, the zone's frame. The body is drawn in the frame's own coordinates
// shifted by (GraphCanvasZoneContentLeft, the heading and its tabs, then
// GraphCanvasZonePadding): a body node at the definition's `layout.nodes`
// (x, y) is that far from the inside origin. The side columns carry the
// boundary sockets, an outside and an inside one per row.
inline constexpr qreal GraphCanvasZoneColumn = 96.0;
inline constexpr qreal GraphCanvasZoneContentLeft = 136.0;
inline constexpr qreal GraphCanvasZonePadding = 24.0;
inline constexpr qreal GraphCanvasZoneTabHeight = 24.0;
inline constexpr qreal GraphCanvasZoneMinimumWidth = 420.0;
inline constexpr qreal GraphCanvasZoneMinimumContentHeight = 96.0;
// LP-4d2: the line along the bottom of a reported zone, and the lanes its
// carry arrows run in above it.
inline constexpr qreal GraphCanvasZoneFooterHeight = 26.0;
inline constexpr qreal GraphCanvasZoneCarryLane = 7.0;

// N-6. One edge, named by the input end it lands on. That end is unique -- one
// port of one node takes at most one edge -- and it is how the document's
// `layout` names an edge too, so the canvas and the document agree by shape
// rather than by convention.
struct GraphCanvasEdgeKey {
    std::string node;
    std::string port;

    friend bool operator==(const GraphCanvasEdgeKey &,
                           const GraphCanvasEdgeKey &) = default;
    friend auto operator<=>(const GraphCanvasEdgeKey &,
                            const GraphCanvasEdgeKey &) = default;
};

// The control points on one edge, in the order the drawing meets them.
struct GraphCanvasReroute {
    GraphCanvasEdgeKey edge;
    std::vector<QPointF> points;
};

// Several edges carried through one place side by side. `position` is where
// slot 0 crosses; slot n is `GraphCanvasTieSpacing` below slot n-1, so the
// order of `edges` is the order of the lines.
struct GraphCanvasTie {
    QPointF position;
    std::vector<GraphCanvasEdgeKey> edges;
};

// Everything the document says about where one scope was drawn. The canvas
// keeps one of these for the scope on screen; it is what `set_layout` writes
// and what `get_graph_instances` reads back.
struct GraphCanvasLayout {
    std::map<std::string, QPointF> nodes;
    std::vector<GraphCanvasReroute> reroutes;
    std::vector<GraphCanvasTie> ties;

    [[nodiscard]] bool empty() const noexcept {
        return nodes.empty() && reroutes.empty() && ties.empty();
    }
};

// The two halves of the one shape. Round-tripping is what keeps the canvas and
// the engine from disagreeing about what a placement is.
GraphCanvasLayout graphCanvasLayoutFromJson(const nlohmann::json &layout);
nlohmann::json graphCanvasLayoutToJson(const GraphCanvasLayout &layout);

// UX-9, decision 4 (engine CT-1): what get_status.audio.voices says of each
// audio.playback, by node id, as its band says it: `playing · 1.2 s`,
// `paused · 1.2 s`, and `idle` in a play session. Outside one (the edit
// state, or a status with no play member) every voice is idle and none is
// named. Empty for a status with no voices.
[[nodiscard]] std::map<std::string, std::string, std::less<>> graphCanvasVoiceBands(
    const nlohmann::json &status);

const NodeGraphKinds &graphCanvasKinds();

// What the socket resolver is told about one port's type, read off the
// catalog the model holds. Separated so the resolution order is testable
// against a real catalog reply rather than a hand-built fact struct.
SocketTypeFacts graphCanvasSocketFacts(const GraphCanvasModel &model,
                                       const std::string &type);

// Editable canvas over one scope of the common graph. Every gesture that
// changes the document goes out as exactly one edit_graph_document the moment
// it finishes. Undo/Redo replay the same validated, digest-checked RPC edits.
class GraphCanvasScene final : public QGraphicsScene {
  public:
    explicit GraphCanvasScene(QObject *parent = nullptr);
    ~GraphCanvasScene() override;

    void setModel(GraphCanvasModel *model);
    // Draw the model's current scope. Positions the user moved, the nodes they
    // folded and the selection are kept for (scope, node id) and survive a
    // refresh that leaves the node in place.
    //
    // LP-4d1: in the root, each graph.loop is drawn as a zone -- a frame with
    // the body of the branch its tab shows drawn inside, where it is edited
    // in its own definition. One draw is then several scopes, and everything
    // the scene keeps (positions, folds, placements, the selection) is filed
    // under (scope, id). A folded zone is the loop's node, as LP-4c drew it.
    void repopulate();
    void clear();
    void rebuildRoutingIndex();
    void invalidateRoutingIndex() { routing_index_.invalidate(); }
    quint64 routingGeneration() const { return routing_index_.generation(); }
    // UX-6b, decision 2: each plan node's GPU time (get_status gpu_timing,
    // by plan node name). A root node's band says the sum of its passes';
    // the bands on screen change in place, nothing else is drawn again.
    void setPlanNodeTimes(std::map<std::string, double, std::less<>> times);
    // UX-9, decision 4 (engine CT-1): each audio.playback's voice as the band
    // under it says it (`playing · 1.2 s`), by root node id
    // (graphCanvasVoiceBands); changed in place as the plan times are.
    void setVoiceBands(std::map<std::string, std::string, std::less<>> bands);
    // AB-1s, decision 4: a root node that reads a stale baked table (a
    // value.sample_table's asset, a render.image's initial.asset) says
    // `stale: <reason>` in its band, drawn orange; by root node id.
    void setStaleBands(std::map<std::string, std::string, std::less<>> bands);
    // GL-2b's band under a root node, with UX-6b's words after it: what the
    // plan says its passes do for nothing, then its GPU time; a voice's state.
    [[nodiscard]] QString layerBandText(const std::string &id) const;

    // The scene changed which scope is shown (a double-click descended into a
    // definition, or the breadcrumb came back out).
    std::function<void()> scopeChanged;
    std::function<void()> selectionChanged;
    // A gesture produced an edit: the owner has to drain the model's requests.
    std::function<void()> editSubmitted;
    // The canvas was right-clicked at this scene position; the owner opens the
    // palette. The scene keeps the position so the new node lands there.
    std::function<void(const QPointF &)> addNodeRequested;
    // "Fit view" from the same menu. Only the view can carry it out.
    std::function<void()> fitViewRequested;
    // The scene finished drawing a scope. The view needs this to know when a
    // scope it has never shown is on screen, so it can fit it.
    std::function<void()> repopulated;
    std::function<void()> gestureEnded;
    std::function<void()> geometryChanged;
    bool drag_trace_active = false;
    // LP-4c, decision 6: "Per iteration..." on an input of a root loop. The
    // owner asks which loop's p[] it reads.
    std::function<void(const std::string &node, const std::string &port)>
        perIterationRequested;
    // LP-4c: the two named additions the canvas menu offers besides the
    // palette ("Add loop", "Add fullscreen pass").
    void setAddActions(QAction *add_loop, QAction *add_fullscreen);
    // Q-19, decision 4: "Rename definition…" and "Delete definition", which
    // the canvas menu offers over the breadcrumb (the definition on screen)
    // and over a zone's heading (the definition its tab shows). `target` is
    // told which definition the open menu is about, and nothing once it
    // closes.
    void setDefinitionActions(QAction *rename, QAction *remove,
                              std::function<void(std::optional<std::string>)> target);

    [[nodiscard]] const std::optional<std::string> &selectedNode() const noexcept {
        return selected_node_;
    }
    // UX-5a: where a Fit view that stops at a readable zoom looks -- the
    // picked node's box, else the box of the node added last this session.
    [[nodiscard]] std::optional<QRectF> fitFocus();
    // LP-4d1: the scope of what is picked -- the node, the edges and the
    // control points all belong to one scope ("" is the root). Picking in
    // another scope lets go of the rest.
    [[nodiscard]] const std::string &selectedScope() const noexcept {
        return selection_scope_;
    }
    // The edge the user picked, so Delete knows which input to clear.
    [[nodiscard]] const std::optional<GraphCanvasEdge> &selectedEdge() const noexcept {
        return selected_edge_;
    }
    // In the scope on screen.
    void selectNode(const std::string &id);
    void selectNode(const std::string &scope, const std::string &id);

    // "Auto layout" (N-4, N-6): place this scope again, and keep the result.
    // The lane the placement kept for each edge that flies over a column comes
    // back as a reroute, and lanes running side by side through one column come
    // back as one tie -- so the drawing it produced survives the next node drag,
    // and the whole of it goes to the document as one `set_layout`.
    void autoLayout();
    void undoEdit();
    void redoEdit();
    // The menu entries the owner named. A named QAction can be fired by the
    // automation surface without driving a popup, so the scene is handed the
    // entries rather than inventing a throwaway one per right-click.
    void setMenuActions(QAction *auto_layout, QAction *fit_view,
                        QAction *tie = nullptr, QAction *untie = nullptr);
    // RL-1s, decision 5: "Reload from disk", last in the canvas menu.
    void setReloadFromDiskAction(QAction *action) { reload_from_disk_action_ = action; }
    // UX-8, decision 10: "New definition…", beside the named additions.
    void setNewDefinitionAction(QAction *action) { new_definition_action_ = action; }

    // --- N-6: the placement, and the gestures that change it.

    // What the canvas is drawing the edges through right now, in the scope on
    // screen -- and, LP-4d1, in any scope it draws (a zone's body).
    [[nodiscard]] const GraphCanvasLayout &layout() const noexcept;
    [[nodiscard]] const GraphCanvasLayout &layout(const std::string &scope) const noexcept;
    // A placement change is waiting for the debounce to run out.
    [[nodiscard]] bool layoutPending() const noexcept;
    // Q-5. A placement change nothing has sent yet: the canvas is ahead of
    // every request in the air. Whatever the last save said, this one has not
    // been saved, so the panel says "Saving" rather than the older "saved" --
    // which is also what lets a wait for "saved" wait for *this* change.
    [[nodiscard]] bool placementUnsent() const noexcept;
    // The canvas's placement changed (a drag, a control point, a bundle, the
    // Frame Plan's member). The owner refreshes the notice from it.
    std::function<void()> placementChanged;
    // Q-5. The model read a document that differs from the last one only in
    // placement. True when this canvas is already drawing what that document
    // places -- its own save coming back, or a scope it is not showing -- so
    // the owner keeps the scene, and with it the items the user has picked.
    // False when the document places something else (an Undo, another
    // writer), and the owner redraws.
    bool followDocumentPlacement();
    // Send it now rather than when the timer says so. The gesture is finished
    // the moment the button comes up; the wait only exists so a drag of fifty
    // mouse moves is one edit instead of fifty.
    void flushLayout();
    // How long that wait is. A named constant because the acceptance is about
    // the number, not about a timer being involved.
    static constexpr int GraphCanvasLayoutDebounceMs = 300;
    // UX-1, decision 1: how close, in pixels on the screen (whatever the
    // zoom), a line being drawn has to come to a socket it can land on to
    // be drawn onto it and land there when let go (Node Wrangler's lazy
    // connect).
    static constexpr qreal GraphCanvasSnapRadius = 32.0;

    // N-8. `set_layout` replaces the whole `layout` member, so a second panel
    // that keeps a placement in it cannot write on its own without erasing
    // this one. It hands its member here instead: one debounce, one write, both
    // halves. Members this canvas does not understand are read off the document
    // and carried forward untouched, so an older Studio never silently drops
    // the Frame Plan's placement by drawing a graph.
    [[nodiscard]] nlohmann::json layoutMember(const std::string &key) const;
    void setLayoutMember(const std::string &key, nlohmann::json value);

    // One more control point on this edge, at this scene position. This is what
    // a double-click on an edge does.
    bool insertReroute(const GraphCanvasEdge &edge, const QPointF &at);
    bool insertReroute(const std::string &scope, const GraphCanvasEdge &edge,
                       const QPointF &at);
    // Bundle what is picked -- reroutes, edges, or both -- into one tie, in the
    // order they run now. Needs two distinct edges; anything less is not a
    // bundle, and is refused rather than silently making a tie of one.
    bool tieSelection();
    // The other way: every slot of the picked tie becomes a reroute of its own.
    bool untieSelection();
    [[nodiscard]] const std::vector<GraphCanvasEdge> &selectedEdges() const noexcept {
        return selected_edges_;
    }
    [[nodiscard]] const std::vector<std::pair<GraphCanvasEdgeKey, std::size_t>> &
    selectedReroutes() const noexcept {
        return selected_reroutes_;
    }
    [[nodiscard]] const std::vector<std::size_t> &selectedTies() const noexcept {
        return selected_ties_;
    }
    // Picking, for a caller that has no mouse: the same thing a click does,
    // with `additive` standing for the Control key.
    void selectEdge(const GraphCanvasEdge &edge, bool additive);
    void selectReroute(const GraphCanvasEdgeKey &edge, std::size_t index,
                       bool additive);
    void selectTie(std::size_t index, bool additive);
    void selectEdge(const std::string &scope, const GraphCanvasEdge &edge, bool additive);
    void selectReroute(const std::string &scope, const GraphCanvasEdgeKey &edge,
                       std::size_t index, bool additive);
    void selectTie(const std::string &scope, std::size_t index, bool additive);

    // Folding is session state, filed exactly like a moved position: the
    // document says nothing about how a node is being looked at. LP-4d1: a
    // root loop folds from its zone into its node.
    [[nodiscard]] bool collapsed(const std::string &id) const;
    void setCollapsed(const std::string &id, bool collapsed);
    void toggleCollapsed(const std::string &id);
    [[nodiscard]] bool collapsed(const std::string &scope, const std::string &id) const;
    void setCollapsed(const std::string &scope, const std::string &id, bool collapsed);

    // LP-4d1. The zones this draw holds: loop id -> the definition drawn in
    // it. A definition two loops run is drawn in the first one's zone only;
    // the other is drawn as its node.
    [[nodiscard]] const std::map<std::string, std::string> &zones() const noexcept {
        return zones_;
    }
    // A zone's tab: show that branch's body (session state). False for a
    // branch that runs nothing.
    bool showLoopBranch(const std::string &loop, std::size_t branch);

    // LP-4d2, decision 3: one pair of a loop's carry, picked by a press on
    // its arrow; Delete takes it out of the loop's carry.
    struct CarryPick {
        std::string loop;
        std::string from;
        std::string to;

        friend bool operator==(const CarryPick &, const CarryPick &) = default;
    };
    [[nodiscard]] const std::optional<CarryPick> &selectedCarry() const noexcept {
        return selected_carry_;
    }
    void selectCarry(const std::string &loop, const std::string &from, const std::string &to);
    // Decision 5: a zone's Show in Frame Plan was pressed.
    std::function<void(const std::string &loop)> showInFramePlanRequested;
    // A gesture was refused before anything was sent, and the model's notice
    // says why (decision 2's other body without the output).
    std::function<void()> noticeChanged;

    // Delete: an edge clears the input it lands on, a node is removed.
    bool deleteSelection();

    // In-place editing. begin* opens the editor over the thing it edits;
    // commit sends one edit_graph_document, cancel sends nothing.
    bool beginLabelEdit(const std::string &id);
    bool beginValueEdit(const std::string &id, const std::string &port);
    bool beginLabelEdit(const std::string &scope, const std::string &id);
    bool beginValueEdit(const std::string &scope, const std::string &id,
                        const std::string &port);
    [[nodiscard]] bool editingInline() const noexcept { return editor_ != nullptr; }
    // Something on the canvas is being dragged: a node, a control point or a
    // bundle. A redraw deletes every item, so one that arrives mid-gesture
    // takes the thing out from under the hand holding it -- and the drag then
    // moves nothing at all. The owner waits, the same way it waits for an
    // in-place editor to close.
    [[nodiscard]] bool dragging() const { return handling_press_ || mouseGrabberItem() != nullptr; }
    // What the open editor currently holds, and a way to set it without a
    // keyboard: the same text the user would have typed.
    [[nodiscard]] QString inlineEditText() const;
    // UX-2, decision 4: an editor that is a list (a value the model offers
    // choices for) opens its list.
    void popUpInlineChoice();
    void setInlineEditText(const QString &text);
    bool commitInlineEdit();
    void cancelInlineEdit();

    // Add one operation from the palette, at the position the right-click
    // named (or beside the selection when it came from elsewhere), in
    // addScope().
    bool addFromPalette(const std::string &op, const std::string &definition = {});
    // UX-3: `seed` is what the palette chose beside the op (addNodeIn).
    bool addFromPalette(const std::string &scope, const std::string &op,
                        const std::string &definition,
                        const nlohmann::json &seed = nlohmann::json::object());
    // LP-4d1: the scope an addition goes into. The canvas menu opened inside
    // a zone names that zone's body; otherwise a picked zone (or a node inside
    // one) does; otherwise the scope on screen.
    [[nodiscard]] std::string addScope() const;
    // The palette the canvas menu asked for is about the scope the menu was
    // opened over; the owner lets go of it when the palette closes.
    void clearPaletteScope() { palette_scope_.reset(); }
    // GL-5c: the palette another panel asked for is about `scope` (the root's
    // for the Frame Plan's Add).
    void setPaletteScope(std::string scope) { palette_scope_ = std::move(scope); }
    // The scope and id of the node the last palette addition placed.
    [[nodiscard]] const std::optional<std::pair<std::string, std::string>> &lastAdded() const noexcept {
        return last_added_;
    }
    // LP-4c: a loop over `body`, or over a new definition of that name.
    bool addLoop(const std::string &body, const std::string &new_definition);
    // LP-4c: descend into an instance's definition or a loop's body, as a
    // double-click on the node does.
    void enterNode(const std::string &id) { enterInstanceScope(id); }
    // GL-1, decision 3: into a folded node, read only, as a double-click on
    // it does; and a feature's decomposed version, as its button does.
    bool enterFolded(const std::string &id);
    bool showDecomposed();
    // GL-1, decision 5: a stub added its domain to the filter.
    std::function<void()> domainFilterChanged;

    // --- GL-1e: a pass of the rendering section takes lines.

    // A line dropped on a pass's input socket (or its `+ input`), or one of
    // its lines deleted: the edit_preset_pass the Frame Plan's socket drop
    // sends, built by the same functions from the authoring context the
    // model read. The owner sends it; `rebuild` makes it again from the
    // context read after a refusal for a moved document. One the builders
    // refused is not handed on: the notice says why.
    std::function<void(const FramePlanEditRequest &,
                       std::function<FramePlanEditRequest()> rebuild)>
        passInputEditRequested;
    // Decision 1: the input sockets of one folded pass node, when it takes
    // lines -- a fullscreen pass the root declares (editable_root_pass),
    // drawn on the real root. Nothing for any other.
    [[nodiscard]] std::optional<FramePlanInputSockets> passInputSockets(
        const std::string &pass) const;
    // The two edits, from the context the model holds now (the owner builds
    // one again this way after the document moved under it).
    [[nodiscard]] FramePlanEditRequest passInputConnectRequest(
        const std::string &pass, const std::string &resource,
        const FramePlanSocketDrop &drop) const;
    [[nodiscard]] FramePlanEditRequest passInputDisconnectRequest(
        const std::string &pass, std::size_t input_index) const;
    // A picked line into such a pass: its pass and its index in `input`.
    struct PassInputPick {
        std::string pass;
        std::size_t input_index = 0;

        friend bool operator==(const PassInputPick &, const PassInputPick &) = default;
    };
    [[nodiscard]] const std::optional<PassInputPick> &selectedPassInput() const noexcept {
        return selected_pass_input_;
    }
    void selectPassInput(const std::string &pass, std::size_t input_index);
    // Delete on the picked line (or its right-click Delete).
    bool deletePassInput();
    // RT-D: the widget owns the tab order; the scene only guards gestures.
    void setEditOrder(TabEditOrder *order) { edit_order_ = order; }
    [[nodiscard]] bool canUndoAny() const;
    [[nodiscard]] bool canRedoAny() const;
    [[nodiscard]] const QPointF &lastContextPosition() const noexcept {
        return context_position_;
    }

    // --- GL-3a: fold groups. The owner draws a group's frame, heading and
    // folded node over the root after each draw (graphcanvasfoldgroups); a
    // press, a double-click or a right-click on one of those items is the
    // owner's to answer, before anything the canvas does with the place.
    // Each hook is asked about every item under the cursor and says whether
    // it took the gesture.
    std::function<bool(QGraphicsItem &item, Qt::MouseButton button)> foldItemPressed;
    std::function<bool(QGraphicsItem &item)> foldItemDoubleClicked;
    std::function<bool(QGraphicsItem &item, const QPoint &screen)> foldItemMenu;
    // The canvas menu's entries for the picked nodes (Fold into group…).
    std::function<void(QMenu &menu)> foldMenuEntries;
    // GL-5d: the canvas menu over node `node` of `scope` (a folded node's id
    // at the root), or -- `line` -- over a line into that pass: the owner
    // picks it and adds the entries that act on it.
    std::function<void(QMenu &menu, const std::string &scope, const std::string &node, bool line)>
        nodeMenuEntries;
    // GL-5e, decision 3: Delete (the key) with folded node `node` of the root
    // picked: the owner removes it when it is a pass the project authored,
    // and says true; false lets the key go on as before.
    std::function<bool(const std::string &node)> foldedNodeDelete;
    // GL-5e, decision 2: a click -- a press and a release on the same input
    // socket, no line drawn -- on socket `key` (`in@<binding>`) of `node` in
    // `scope` (a folded node's at the root when `folded`): when the pass
    // draws with engine://passthrough, that input is the one shown -- the
    // Frame Plan's framePlanPassthroughInputRequest for a pass of the
    // rendering section, the same define (PELICAN_PASSTHROUGH_INPUT) in a
    // graph node's shader.defines -- and true. False keeps what a click did.
    bool clickInputSocket(const std::string &scope, const std::string &node, const std::string &key,
                          bool folded);
    // GL-5e, decision 4: a pass input edit for `pass` the builders refused
    // before it was sent (the shader declares no input: `passthrough_inputs`
    // set).
    std::function<void(const std::string &pass, const FramePlanEditRequest &request)>
        passInputRefused;
    // GL-5e, decision 6: a line from an output let go over folded node
    // `node` of the root (its body or one of its input rows, `row` the
    // resource that row reads, else empty), carrying `resource`. The owner
    // answers it -- the Frame Plan's Shift-drag dialog, or the notice's line
    // and Expand to edit… on a read-only node -- and says true.
    std::function<bool(const std::string &node, const std::string &row, const std::string &resource)>
        foldedNodeDrop;
    // Let go of everything picked on the canvas (a group's heading was
    // picked instead).
    void clearPicks();

  protected:
    bool handling_press_ = false;
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override;
    void contextMenuEvent(QGraphicsSceneContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private:
    // One socket grabbed and not yet dropped. A drag that starts on an input
    // and ends nowhere is how an edge is cut.
    struct PendingConnection {
        std::string scope;
        std::string node;
        std::string port;
        bool from_output = true;
        QPointF origin;
        // LP-4d2, decision 3: a line from a body output's inside end on a
        // zone's right edge, which lands on a boundary input as a carry.
        std::string carry_loop;
        // Q-19: grabbed on a socket of a render node's shader declarations
        // (its key); `port` is then the key too.
        std::string socket_key;
        // UX-5c: grabbed on a folded node's output row; `port` is the
        // resource it names, and only a rendering-section pass's input
        // takes it.
        bool from_folded = false;
    };
    // Decision 7: what a zone's frame is drawn around, per body, so the
    // frame can follow a body node through a drag.
    struct ZoneGeometry {
        QPointF origin;
        qreal min_width = 0.0;
        qreal min_height = 0.0;
        qreal footer = 0.0;
        QRectF frame;
    };
    struct InlineEditor;
    // An open in-place editor, said in terms that outlive the items it sits
    // on. A rebuild deletes every item, the proxy included, so the editor is
    // opened again over the new ones rather than taken out from under the
    // person typing into it -- and what they had typed comes with it.
    struct CarriedInlineEdit {
        bool label = false;
        std::string scope;
        std::string node;
        std::string port;
        QString text;
    };
    // N-6: the document's placement for one scope, and the bookkeeping that
    // keeps one gesture from becoming fifty edits. `sent` is the last payload
    // that went out, so a refresh can tell "the document has caught up" from
    // "the document is still behind the drag". LP-4d1 keeps one per scope the
    // canvas draws: a zone's body is placed in its own definition.
    struct ScopePlacement {
        GraphCanvasLayout layout;
        // Null until something has actually been sent: an empty object is a
        // real placement (one that says "nothing is placed"), and a document
        // that has never carried one must not read as "the canvas has caught
        // up".
        nlohmann::json sent;
        // N-8: the members of the same `layout` object that belong to another
        // panel. Read back from the document on every load and sent again on
        // every write, so this canvas never has to understand them to
        // preserve them.
        nlohmann::json foreign = nlohmann::json::object();
        bool dirty = false;
        // Loaded from the document at least once.
        bool loaded = false;
    };

    GraphCanvasModel *model_ = nullptr;
    // Editor session state: node positions never reach disk, and they are
    // filed under the scope so the same node id in two definitions does not
    // share one position.
    std::map<std::pair<std::string, std::string>, QPointF> session_node_positions_;
    // UX-5a: the node placeNewNode placed last ({scope, id}), for fitFocus.
    std::optional<std::pair<std::string, std::string>> last_added_;
    // GL-1c: the places a placement gave the folded nodes (not a drag), kept
    // for the session under foldedPositionKey. They hold while they stay
    // clear of what the graph draws.
    std::map<std::pair<std::string, std::string>, QPointF> folded_places_;
    // UX-5a: where the folded nodes stood when a node was added before the
    // document placed any (the one placement would move them onto it). Held
    // while they stay clear of the graph; Auto layout lets go of them.
    std::map<std::pair<std::string, std::string>, QPointF> added_folded_places_;
    std::map<std::pair<std::string, std::string>, bool> session_collapsed_;
    std::map<std::string, ScopePlacement> placements_;
    QTimer *layout_timer_ = nullptr;
    // LP-4d1: the scopes the last draw drew (the one on screen first, then
    // each zone's body), the zones (loop -> body), and the item each body is
    // drawn inside -- its coordinates are the definition's own.
    std::vector<std::string> drawn_scopes_;
    std::map<std::string, std::string> zones_;
    std::map<std::string, QGraphicsItem *> zone_content_;
    std::map<std::string, QRectF> zone_content_rect_;
    std::map<std::string, ZoneGeometry> zone_geometry_;
    std::optional<CarryPick> selected_carry_;
    bool zone_refit_queued_ = false;
    // Which scope the canvas menu was opened over, while it is open, and the
    // one its "Add node..." asked the palette about.
    std::optional<std::string> menu_scope_;
    std::optional<std::string> palette_scope_;
    std::string selection_scope_;
    // What the automatic placement chose on the last draw: the boxes, and the
    // lane every edge that flies over a column was given. "Auto layout" turns
    // this into reroutes and ties; nothing else reads it.
    std::map<std::string, QPointF> placement_boxes_;
    std::vector<std::pair<GraphCanvasEdgeKey, std::vector<QPointF>>> placement_bends_;
    std::optional<std::uint64_t> pruned_revision_;
    std::optional<std::string> selected_node_;
    std::optional<GraphCanvasEdge> selected_edge_;
    // Picking more than one thing is what a tie is made of, so the edge pick is
    // a list and `selected_edge_` is its last entry -- the one Delete cuts.
    std::vector<GraphCanvasEdge> selected_edges_;
    std::vector<std::pair<GraphCanvasEdgeKey, std::size_t>> selected_reroutes_;
    std::vector<std::size_t> selected_ties_;
    std::optional<PendingConnection> pending_connection_;
    QGraphicsPathItem *pending_edge_ = nullptr;
    std::optional<PassInputPick> selected_pass_input_;
    TabEditOrder *edit_order_ = nullptr;
    // The builders' inputs, from the model: the authoring context parsed
    // once per revision, and the plan in the Frame Plan's terms.
    [[nodiscard]] FramePlanAuthoring passAuthoring() const;
    mutable std::optional<std::pair<std::uint64_t, FramePlanAuthoring>> pass_authoring_;
    // A drop on a pass's input socket: the edit, or the notice why not.
    void dropOnPassInput(const std::string &pass, const QGraphicsItem &socket,
                         const std::string &from_node, const std::string &from_port);
    // UX-5c: and of a resource by name (a folded node's output row).
    void dropResourceOnPassInput(const std::string &pass, const QGraphicsItem &socket,
                                 const std::string &resource);
    void handPassInputEdit(const FramePlanEditRequest &request,
                           std::function<FramePlanEditRequest()> rebuild,
                           const std::string &pass = {});
    std::unique_ptr<InlineEditor> editor_;
    QAction *auto_layout_action_ = nullptr;
    QAction *fit_view_action_ = nullptr;
    QAction *tie_action_ = nullptr;
    QAction *untie_action_ = nullptr;
    QAction *reload_from_disk_action_ = nullptr;
    QAction *new_definition_action_ = nullptr;
    QAction *add_loop_action_ = nullptr;
    QAction *add_fullscreen_action_ = nullptr;
    QAction *rename_definition_action_ = nullptr;
    QAction *delete_definition_action_ = nullptr;
    std::function<void(std::optional<std::string>)> definition_target_;
    QPointF context_position_;
    // Where the next added node goes, when the palette was opened over empty
    // canvas rather than beside a selected node.
    bool rebuilding_ = false;
    bool scene_rect_update_queued_ = false;
    bool scene_rect_finalize_ = false;
    // A breadcrumb click rebuilds the scene from inside the base class's event
    // dispatch. Anything that ran before it must not then read the new items
    // as if they were the ones the user pressed on.
    std::uint64_t rebuild_generation_ = 0;
    // UX-6b, decision 2.
    std::map<std::string, double, std::less<>> plan_node_times_;
    std::map<std::string, std::string, std::less<>> voice_bands_;
    std::map<std::string, std::string, std::less<>> stale_bands_;
    // The bands on screen take their words again where they stand; a root
    // node that had none gets one.
    void refreshLayerBands();
    // The theme table's registration, dropped in the destructor: a listener
    // that outlived its scene would repopulate a deleted one.
    int theme_listener_ = 0;

    void leaveCurrentScope();
    void enterInstanceScope(const std::string &id);
    // Only the edges that touch the selected node are lit.
    void applyEdgeEmphasis();
    void pruneSessionPositions();
    // GL-1b, decision 1: where a folded node's session place is filed (per
    // view), and keeping the places the folded nodes are drawn at now (GL-1c:
    // in folded_places_, apart from the places a drag gave).
    [[nodiscard]] std::pair<std::string, std::string> foldedPositionKey(const std::string &id) const;
    void keepFoldedPlaces();
    // UX-1, decision 3: the root's graph nodes keep, for the session, the
    // places they are drawn at now (a body node is being dragged in a root
    // the document does not place).
    void holdRootPlaces(const std::string &root);
    NodeGraphIndex routing_index_{*this};
    void scheduleSceneRectUpdate();
    void applySceneRectNow(bool grow_only = false);
    void clearControlSelection();
    // Picking something in `scope` lets go of what was picked elsewhere.
    void enterSelectionScope(const std::string &scope);
    // N-6.
    ScopePlacement &placement(const std::string &scope);
    void loadLayout(const std::string &scope);
    void pruneLayout(const std::string &scope);
    void noteLayoutChange(const std::string &scope);
    // The whole `layout` object this canvas would write for one scope: its own
    // members and the other panel's, which ride along.
    [[nodiscard]] nlohmann::json placementPayload(const std::string &scope) const;
    [[nodiscard]] QVector<QPointF> bendsFor(const std::string &scope,
                                            const GraphCanvasEdge &edge) const;
    void applyEdgeBends();
    void moveReroute(const std::string &scope, const GraphCanvasEdgeKey &edge,
                     std::size_t index, const QPointF &at);
    void moveTie(const std::string &scope, std::size_t index, const QPointF &at);
    GraphCanvasReroute &rerouteFor(const std::string &scope, const GraphCanvasEdgeKey &edge);
    void detachFromTies(const std::string &scope, const GraphCanvasEdgeKey &edge);
    bool deleteControlSelection();
    void beginConnection(const QGraphicsItem &socket);
    void finishConnection(const QPointF &position);
    void clearPendingConnection();
    // UX-1, decision 1. What letting the line go over `item` would do -- the
    // one judgement both the drop and the lit sockets use. Empty when the
    // item takes no line from this connection.
    [[nodiscard]] std::function<void()> dropAction(const PendingConnection &pending,
                                                   QGraphicsItem &item);
    // While a line is drawn: the sockets it can land on are lit, the others
    // dimmed, and the line is drawn onto the nearest one in reach.
    struct LitSocket {
        QGraphicsItem *item = nullptr;
        qreal opacity = 1.0;
        qreal scale = 1.0;
        QPointF centre;
        bool target = false;
    };
    std::vector<LitSocket> lit_sockets_;
    QGraphicsItem *snap_target_ = nullptr;
    // The scopes' nodes, read once while one line asks dropAction about
    // every socket; emptied when it is done.
    std::map<std::string, std::vector<GraphCanvasNode>> drop_nodes_;
    void lightDropTargets(const QGraphicsItem *origin);
    void followCursor(const QPointF &at);
    void closeEditor();
    // The two halves of carrying an editor across a rebuild.
    [[nodiscard]] std::optional<CarriedInlineEdit> carryInlineEdit() const;
    void restoreInlineEdit(const CarriedInlineEdit &carried);
    // LP-4f1, decision 5: beside the picked node (a zone's frame, for a
    // loop), at its real width, and clear of every box already drawn in the
    // scope. `op` says how large the new one will be (a loop is a zone).
    void placeNewNode(const std::string &scope, const std::string &id,
                      const std::string &op = {});
    // UX-8, decision 8: the nodes one addition brings beside it (a clock, a
    // constant, an image), left of `main` and clear of everything drawn.
    void placeCompanions(const std::string &scope, const std::string &main,
                         const std::vector<std::string> &companions);
    void afterEdit(bool submitted);
    // LP-4d1. A scene position in a scope's own coordinates: a zone's body is
    // drawn inside an item, the scope on screen is not.
    [[nodiscard]] QPointF toScopePoint(const std::string &scope, const QPointF &at) const;
    // The scope under a scene position: a zone's body inside its frame, the
    // scope on screen elsewhere.
    [[nodiscard]] std::string scopeAt(const QPointF &at) const;
    // A body node moved: the frame is drawn again around it once the hand
    // lets go.
    void scheduleZoneRefit();
    // LP-4d2, decision 7: and follows it while the hand is still on it --
    // the frame's items are moved, nothing is drawn again.
    void refitZoneLive(const std::string &body);
    // The loop whose zone draws this body, or empty.
    [[nodiscard]] std::string zoneLoopOf(const std::string &body) const;
    void beginCarry(const QGraphicsItem &socket, const std::string &loop,
                    const std::string &output);
};

// The Graph tab of the render tools dock.
class GraphCanvasWidget final : public QWidget {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit GraphCanvasWidget(GraphCanvasReadCapability graph,
                               QWidget *parent = nullptr);
    ~GraphCanvasWidget() override;

    void refresh();
    // One tick of the tab's own clock: what the 250 ms timer does, named so a
    // test can be the clock instead of waiting for one.
    void poll();
    // Show one node of the root document: the Inspector's "Open the source"
    // names a producer, and this is where it lands.
    bool revealNode(const QString &node);
    // Q-12. Show the render.compute node that made a task, as
    // get_compute_task_authoring's graph_node names it: `scope` is "root" or
    // a definition, `node` the id written there, `instance` the instance path
    // that placed it ("outer/left"; "" in the root scope). The scope is
    // reached through that instance when the path leads there -- the trail and
    // the watched instance are then the instance's -- and opened directly
    // otherwise. The node is selected and brought to the middle of the view.
    // False, with nothing changed, when the document holds no such node.
    bool revealGraphNode(const QString &scope, const QString &node,
                         const QString &instance);
    // Whether the document the tab holds has that node, without moving
    // anything.
    [[nodiscard]] bool canRevealGraphNode(const QString &scope,
                                          const QString &node) const;
    // LP-4c, decision 10. Show the body node an expanded pass or task came
    // from (get_frame_plan's loop_origin): the loop's body for that element's
    // branch, entered through the loop, with the node selected. False, with
    // nothing changed, when the document holds no such loop, branch or node.
    // LP-4d2, decision 6: on the root canvas, in the loop's zone -- its tab
    // on that element's branch, the node picked there -- rather than entered
    // through the loop. A body the zone cannot show (a folded zone opens; a
    // definition another loop's zone draws) is entered as before.
    bool revealLoopOrigin(const QString &loop, qint64 element, const QString &node);
    // Decision 5: what a zone's Show in Frame Plan does -- the owner brings
    // the Frame Plan forward with the loop's copies picked. False when it
    // could not.
    void setFramePlanLoopAction(std::function<bool(const QString &loop)> action);
    // Q-18. Called once for each edit of this tab that committed and can
    // change the frame plan (anything but a placement), when the engine says
    // it committed: the owner has the Frame Plan read the plan again.
    void setPlanCommitAction(std::function<void()> action);
    // GL-1e / RU-1: the render edit history's Undo and Redo (the window's).
    // A pass's input edit sent from this tab is taken back by the tab's own
    // Undo through them, as by the Frame Plan's.
    void setRenderHistoryActions(QAction *undo, QAction *redo);
    void setRenderHistory(RenderEditHistory *history);
    // GL-1b, decision 2. An edit that committed with restart_required hands
    // the restart to this action -- the window's, the one the Frame Plan's
    // G-4 / G-5 calls -- with the line the tab's notice shows. When the
    // restart is declined, engineRestartDeclined says why and reads the
    // document the running engine now has.
    void setEngineRestartAction(std::function<void(const QString &reason)> action);
    void engineRestartDeclined(const QString &message);
    // UX-12b, decision 2. A feature that goes after a pass the pipeline does
    // not have ("Place Deferred lighting first"): a fullscreen pass named
    // `name` is added to the root (or, when there is one, picked), and its
    // Pass fields open New... on the shader template of that name (UX-5b),
    // for Create. False when the tab cannot edit now.
    bool placeNamedFullscreenPass(const QString &name);
    // LP-4e. Show a root node another panel's edit just made (Decompose into
    // loop's graph.loop): the document is read again, and the node is
    // selected as soon as a document that holds it arrives -- at once when
    // the one in hand already does.
    void revealNodeWhenPresent(const QString &node);
    // GL-1, decision 6. The Frame Plan's Open in Graph on any pass: the
    // real root, with the node the pass is drawn under -- the graph node that
    // made it, its feature's folded node, or its own -- selected and brought
    // to the middle (its domain shown if the filter hid it). GL-2b, decision
    // 4: read by the plan's origin -- a loop's copy is its body node in the
    // zone, on the tab of the element's branch; a node inside an instance is
    // reached through that instance. False, with nothing changed, when the
    // plan the tab last read has no such pass.
    bool revealPlanPass(const QString &pass);
    // GL-5c, decision 3. Whether the pass of the plan is edited on this tab --
    // a graph node made it (in the root, an instance's body or a loop's), or
    // it is a pass of the rendering section, which the detail column edits
    // (GL-5a) -- rather than read only here (a preset's or a feature's).
    [[nodiscard]] bool editsPlanPass(const QString &pass) const;
    // GL-5c, decisions 1 and 2. The Frame Plan's Add material pass... / Add
    // fullscreen pass... / Add compute task... on a graph root: the palette
    // opens in the root on `op` (render.material_pass, render.compute) -- at
    // once, or as soon as the tab holds a document it can add to -- and what
    // it places is picked, its detail column open, once the document holds
    // it. The palette is the one Add node... opens; the node goes into the
    // graph's `nodes` through the same addFromPalette. render.fullscreen has
    // nothing to pick beside it and is placed at once, as the tab's own Add
    // fullscreen pass places it.
    void placeFromPalette(const QString &op);
    // GL-2b, decision 3: what a click on an L1 row of the Layers section does
    // -- the owner brings the Frame Plan forward with that plan node picked.
    void setFramePlanNodeAction(std::function<void(const QString &node)> action);
    // UX-11, decision 4: what a press on a material pass's Materials drawn
    // row does -- the owner brings the Frame Plan forward with its Materials
    // rows of that pass (the plan's name) picked.
    void setFramePlanMaterialsAction(std::function<void(const QString &pass)> action);
    // GL-1, decision 4: what "Expand to edit…" runs -- the Frame Plan's
    // Decompose into loop on a material pass (`graph` is the plan's), the
    // Features tab's Replace with loop version on a feature.
    void setFoldedExpandActions(
        std::function<void(const QString &graph, const QString &pass)> decompose,
        std::function<void(const QString &feature)> replace);
    // GL-5d, decisions 1 and 2. What only the Frame Plan offered, on the node
    // picked here -- its right-click and a row of buttons in the detail
    // column fire the named actions pelican.graphCanvas.orderPasses,
    // .fixSockets, .makeSocketsVariadic (and .editTarget, .deleteTarget,
    // .expandToEdit, which this tab carries out itself). `order` is the Frame
    // Plan's Order... (offered on a pass of the rendering section);
    // `fix_sockets` / `variadic_sockets` its Fix sockets / Make sockets
    // variadic on the plan's pass (`graph` is the plan's): the rendering
    // section's, or the one a root fullscreen or raster node made, for which
    // `graph_fragment` moves the node onto a copied shader.
    void setFramePlanPassActions(
        std::function<void()> order,
        std::function<void(const QString &graph, const QString &pass,
                           std::function<bool(const QString &reference)> graph_fragment)>
            fix_sockets,
        std::function<void(const QString &graph, const QString &pass)> variadic_sockets);
    // GL-5d: a line one of those left on the Frame Plan's status row, on
    // this tab's notice.
    void showFramePlanLine(const QString &line);
    // GL-5e. The rest of what only the Frame Plan offered, on what is picked
    // here: each hook is the Frame Plan's (or the Pass Editor's / Task
    // Editor's) own function, and an entry whose hook is empty is not
    // offered. Named actions, in the right-click and in the detail column's
    // row (pelican.graphCanvasNodeActions.<name>):
    //   decision 3, pelican.graphCanvas.deletePass (and the Delete key) on a
    //     folded node standing for a pass the project authored (a managed
    //     fragment, passes/authoring/pass-<id>.json): `remove_pass`;
    //   decision 4, pelican.graphCanvas.usePassthrough / .openContractShader
    //     after a pass's edit was refused because its shader does not read
    //     its inputs, .disconnectRefused / .openRefusedShader on a pass whose
    //     shader reload the engine refused: `shader_contract`, `reload_refusal`
    //     (also as buttons under the notice, pelican.graphCanvasNoticeActions.*);
    //   decision 5, the Layers section's right-click: Pin this candidate /
    //     Unpin on a backend candidate's row, Freeze this physical plan, Eject
    //     again and Remove the frozen plan, and Remove TRANSFER_SRC on a
    //     resource row (this tab's removeImageTransferSource);
    //   decision 6, a line dropped on a feature's authored compute task or an
    //     editable preset pass: the Frame Plan's Shift-drag dialogs.
    struct ShaderContract {
        std::string graph;
        std::string pass;
        std::string fragment;
        std::vector<std::string> inputs;
        bool unsupplied = false;
        std::optional<std::size_t> binding;
        bool use_passthrough = false;
    };
    struct FramePlanEdits {
        std::function<void(const QString &graph, const QString &pass)> remove_pass;
        std::function<void(const ShaderContract &contract, bool use_passthrough)> shader_contract;
        std::function<void(const QString &graph, const QString &pass, bool disconnect)> reload_refusal;
        std::function<void(const QString &candidate)> pin;
        std::function<void()> unpin;
        std::function<void()> freeze;
        std::function<void()> remove_frozen;
        std::function<void(const QString &task, const QStringList &outputs)> task_connection;
        std::function<void(const QString &graph, const QString &pass, const QStringList &outputs)>
            preset_pass_connection;
        // GL-5f: the Frame Plan's Edit task -- the Task Editor on an authored
        // compute task (its dispatch, View, reads and writes).
        std::function<void(const QString &task)> edit_task;
    };
    void setFramePlanEdits(FramePlanEdits edits);
    // The same ingestion boundary the production RPC callback uses.
    void receiveResult(qint64 request_id, const QByteArray &result_json);
    void receiveFailure(qint64 request_id, const QString &message);
    void setConnected(bool connected);
    // Q-8. The project whose UI document names the nodes a ui.* sink can
    // write. Read again whenever the list is about to be offered.
    void setProjectRoot(const std::filesystem::path &project_root);
    // RTL-2s, decision 1: a ray tracing task's Enable hit groups… (a project
    // without a ray group layout) opens the Ray layout.
    void setOpenRayLayout(std::function<void()> open);
    // RTL-2s-c: a ray tracing task's Callable records... opens the callable
    // record initial values (get/edit_ray_callable_records, Render Features'
    // Records).
    void setOpenCallableRecords(std::function<void()> open);
    // UX-8, decision 9: File > Import Sound… and the Sound list's Import…:
    // the files picked (or dropped) are copied into the project's
    // assets/audio, `_2` beside a name already there, and the first is
    // played by the picked voice. chooseSoundsToImport opens the dialog from
    // the event loop.
    void chooseSoundsToImport();
    void importSounds(const std::vector<std::filesystem::path> &files);
    // Q-13. The rendering document's digest, shared with the other panels
    // that edit it: the tab says which revision it read, and reads the
    // document again when another panel's commit moved it.
    void setRenderConfigDigest(std::shared_ptr<RenderConfigDigest> digest);
    // PM-3a (design_play_mode 12.6): the Timeline's scrub is refused while
    // `reason` is not empty (the game is playing), and the strip says it.
    void setTimelineScrubBlocked(const QString &reason);
    // UX-6a, decision 2: the project shader files the render passes of the
    // document read (`project://shaders/<name>`, by stage), keyed by node
    // (`<id>` at the root, `<definition>/<id>` in a body), handed to
    // `action` each time the document moves them -- to the one sender of
    // reload_shaders, the Frame Plan. shadersReloaded is what that sender
    // calls after a reload: the document and the plan's declarations are
    // read again, so the sockets follow the shaders without Refresh.
    void setShaderSourcesAction(
        std::function<void(std::map<std::string, std::vector<std::filesystem::path>, std::less<>>)>
            action);
    void shadersReloaded();
    // IA-1s, decision 5: the assets the document's render.image nodes start
    // from (`initial.asset`, GraphCanvasModel::initialAssets), handed to
    // `action` each time the document changes them.
    void setInitialAssetsAction(std::function<void(std::vector<std::string>)> action);
    // RL-1s. The render documents were read again from disk: the Graph's
    // own Undo history ends here (what it holds inverts a document that is
    // no longer the one on disk), and the document is read again.
    void documentsReloadedFromDisk();
    // A committed Render Undo/Redo can leave the graph unchanged. Unlike an
    // external reload, that exact revision may retain Graph Undo and Redo.
    void renderHistoryRestored(const std::string &digest);
    // RL-1s. What the reload from disk said, on this tab's notice row
    // (kind None takes down only a line this call put up).
    void showDiskReloadNotice(const InspectorNotice &notice);
    // GL-5g, decision 3: a shader reload the engine did not take (the log
    // the Frame Plan hands the Pass Editor too), on this tab's notice row:
    // which node's shader, the log's first error line, and that the engine
    // keeps the previous version (the Problems window lists it). An empty
    // log -- a reload that went through -- takes down only a line this call
    // put up.
    void showShaderReloadNotice(const QString &log);
    // RL-1s, decision 5: "Reload from disk" in the canvas menu (the window
    // owns the action, and what it does).
    void setReloadFromDiskAction(QAction *action);
    // UX-6a, decision 4: take TRANSFER_SRC off the render.image a plan
    // resource is (`origin` for a loop's copy: the body node it came from),
    // as one edit. False, with nothing sent, when the document has no such
    // image, it has no TRANSFER_SRC, or the tab cannot edit now.
    bool removeImageTransferSource(const QString &resource, const QString &loop,
                                   qint64 element, const QString &node);
    // UX-6b, decisions 1 and 3: the line over the canvas -- the frame's GPU
    // time and the plan's size -- and what the last edit moved. The row
    // keeps its height either way. Decision 2: each plan node's GPU time, for
    // the bands. The window's PlanPerfHub reads them; the tab reads nothing.
    void setPerfLines(const QString &summary, const QString &delta);
    void setPlanNodeTimes(std::map<std::string, double, std::less<>> times);
    // UX-9, decision 4: a get_status result, whoever asked for it (the
    // viewport's one parse, UX-7 decision 10): its audio.voices go on the
    // bands of the audio.playback nodes.
    void observeStatus(const nlohmann::json &status);

    GraphCanvasModel &model();
    GraphCanvasScene &scene();
};

// How the Graph tab's view is set up: a press on empty canvas draws a
// rubber band, and -- UX-1, decision 2 -- both scroll bars are always shown.
// A scene rect that grew under a drag made one appear; the viewport shrank
// by its width and the view, which keeps its middle on a resize, moved
// everything on screen by half of it (the zone under the hand slid away).
void configureGraphCanvasView(NodeCanvasView &view);

// Q-12. "Open in Graph": reveal the node, and only when it is there make the
// Graph tab the current one of `tabs`. A node the Graph tab cannot show
// leaves the tab the user was on, where the caller says why.
bool openGraphNodeInTabs(QTabWidget &tabs, GraphCanvasWidget &graph,
                         const QString &scope, const QString &node,
                         const QString &instance);

} // namespace PelicanStudio
