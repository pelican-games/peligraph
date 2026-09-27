#pragma once

#include "../model/graphcanvasmodel.hpp"
#include "graphcanvasreadcapability.hpp"
#include "nodegraphics.hpp"

#include <QGraphicsScene>
#include <QPointF>
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
class QTabWidget;
class QTimer;
class QGraphicsSceneContextMenuEvent;
class QGraphicsSceneMouseEvent;
class QKeyEvent;

namespace PelicanStudio {

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

// The node is a heading band over two socket sections, so its height follows
// its port count rather than a constant. The numbers are here so the scene
// test can name the same geometry the canvas draws.
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
    [[nodiscard]] bool dragging() const { return mouseGrabberItem() != nullptr; }
    // What the open editor currently holds, and a way to set it without a
    // keyboard: the same text the user would have typed.
    [[nodiscard]] QString inlineEditText() const;
    void setInlineEditText(const QString &text);
    bool commitInlineEdit();
    void cancelInlineEdit();

    // Add one operation from the palette, at the position the right-click
    // named (or beside the selection when it came from elsewhere), in
    // addScope().
    bool addFromPalette(const std::string &op, const std::string &definition = {});
    bool addFromPalette(const std::string &scope, const std::string &op,
                        const std::string &definition);
    // LP-4d1: the scope an addition goes into. The canvas menu opened inside
    // a zone names that zone's body; otherwise a picked zone (or a node inside
    // one) does; otherwise the scope on screen.
    [[nodiscard]] std::string addScope() const;
    // The palette the canvas menu asked for is about the scope the menu was
    // opened over; the owner lets go of it when the palette closes.
    void clearPaletteScope() { palette_scope_.reset(); }
    // LP-4c: a loop over `body`, or over a new definition of that name.
    bool addLoop(const std::string &body, const std::string &new_definition);
    // LP-4c: descend into an instance's definition or a loop's body, as a
    // double-click on the node does.
    void enterNode(const std::string &id) { enterInstanceScope(id); }
    [[nodiscard]] const QPointF &lastContextPosition() const noexcept {
        return context_position_;
    }

  protected:
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
    std::unique_ptr<InlineEditor> editor_;
    QAction *auto_layout_action_ = nullptr;
    QAction *fit_view_action_ = nullptr;
    QAction *tie_action_ = nullptr;
    QAction *untie_action_ = nullptr;
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
    // A breadcrumb click rebuilds the scene from inside the base class's event
    // dispatch. Anything that ran before it must not then read the new items
    // as if they were the ones the user pressed on.
    std::uint64_t rebuild_generation_ = 0;
    // The theme table's registration, dropped in the destructor: a listener
    // that outlived its scene would repopulate a deleted one.
    int theme_listener_ = 0;

    void leaveCurrentScope();
    void enterInstanceScope(const std::string &id);
    // Only the edges that touch the selected node are lit.
    void applyEdgeEmphasis();
    void pruneSessionPositions();
    void scheduleSceneRectUpdate();
    void applySceneRectNow();
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
    void closeEditor();
    // The two halves of carrying an editor across a rebuild.
    [[nodiscard]] std::optional<CarriedInlineEdit> carryInlineEdit() const;
    void restoreInlineEdit(const CarriedInlineEdit &carried);
    // LP-4f1, decision 5: beside the picked node (a zone's frame, for a
    // loop), at its real width, and clear of every box already drawn in the
    // scope. `op` says how large the new one will be (a loop is a zone).
    void placeNewNode(const std::string &scope, const std::string &id,
                      const std::string &op = {});
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
    // LP-4e. Show a root node another panel's edit just made (Decompose into
    // loop's graph.loop): the document is read again, and the node is
    // selected as soon as a document that holds it arrives -- at once when
    // the one in hand already does.
    void revealNodeWhenPresent(const QString &node);
    // The same ingestion boundary the production RPC callback uses.
    void receiveResult(qint64 request_id, const QByteArray &result_json);
    void receiveFailure(qint64 request_id, const QString &message);
    void setConnected(bool connected);
    // Q-8. The project whose UI document names the nodes a ui.* sink can
    // write. Read again whenever the list is about to be offered.
    void setProjectRoot(const std::filesystem::path &project_root);
    // Q-13. The rendering document's digest, shared with the other panels
    // that edit it: the tab says which revision it read, and reads the
    // document again when another panel's commit moved it.
    void setRenderConfigDigest(std::shared_ptr<RenderConfigDigest> digest);

    GraphCanvasModel &model();
    GraphCanvasScene &scene();
};

// Q-12. "Open in Graph": reveal the node, and only when it is there make the
// Graph tab the current one of `tabs`. A node the Graph tab cannot show
// leaves the tab the user was on, where the caller says why.
bool openGraphNodeInTabs(QTabWidget &tabs, GraphCanvasWidget &graph,
                         const QString &scope, const QString &node,
                         const QString &instance);

} // namespace PelicanStudio
