#pragma once

#include "../model/graphcanvasmodel.hpp"
#include "graphcanvasreadcapability.hpp"
#include "nodegraphics.hpp"

#include <QGraphicsScene>
#include <QPointF>
#include <QWidget>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QAction;
class QTimer;
class QGraphicsSceneContextMenuEvent;
class QGraphicsSceneMouseEvent;
class QKeyEvent;

namespace PelicanStudio {

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
// it finishes: there is no Apply button, and no undo (v0).
class GraphCanvasScene final : public QGraphicsScene {
  public:
    explicit GraphCanvasScene(QObject *parent = nullptr);
    ~GraphCanvasScene() override;

    void setModel(GraphCanvasModel *model);
    // Draw the model's current scope. Positions the user moved, the nodes they
    // folded and the selection are kept for (scope, node id) and survive a
    // refresh that leaves the node in place.
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

    [[nodiscard]] const std::optional<std::string> &selectedNode() const noexcept {
        return selected_node_;
    }
    // The edge the user picked, so Delete knows which input to clear.
    [[nodiscard]] const std::optional<GraphCanvasEdge> &selectedEdge() const noexcept {
        return selected_edge_;
    }
    void selectNode(const std::string &id);

    // "Auto layout" (N-4, N-6): place this scope again, and keep the result.
    // The lane the placement kept for each edge that flies over a column comes
    // back as a reroute, and lanes running side by side through one column come
    // back as one tie -- so the drawing it produced survives the next node drag,
    // and the whole of it goes to the document as one `set_layout`.
    void autoLayout();
    // The menu entries the owner named. A named QAction can be fired by the
    // automation surface without driving a popup, so the scene is handed the
    // entries rather than inventing a throwaway one per right-click.
    void setMenuActions(QAction *auto_layout, QAction *fit_view,
                        QAction *tie = nullptr, QAction *untie = nullptr);

    // --- N-6: the placement, and the gestures that change it.

    // What the canvas is drawing the edges through right now.
    [[nodiscard]] const GraphCanvasLayout &layout() const noexcept {
        return layout_;
    }
    // A placement change is waiting for the debounce to run out.
    [[nodiscard]] bool layoutPending() const noexcept { return layout_dirty_; }
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

    // Folding is session state, filed exactly like a moved position: the
    // document says nothing about how a node is being looked at.
    [[nodiscard]] bool collapsed(const std::string &id) const;
    void setCollapsed(const std::string &id, bool collapsed);
    void toggleCollapsed(const std::string &id);

    // Delete: an edge clears the input it lands on, a node is removed.
    bool deleteSelection();

    // In-place editing. begin* opens the editor over the thing it edits;
    // commit sends one edit_graph_document, cancel sends nothing.
    bool beginLabelEdit(const std::string &id);
    bool beginValueEdit(const std::string &id, const std::string &port);
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
    // named (or beside the selection when it came from elsewhere).
    bool addFromPalette(const std::string &op, const std::string &definition = {});
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
        std::string node;
        std::string port;
        bool from_output = true;
        QPointF origin;
    };
    struct InlineEditor;

    GraphCanvasModel *model_ = nullptr;
    // Editor session state: node positions never reach disk, and they are
    // filed under the scope so the same node id in two definitions does not
    // share one position.
    std::map<std::pair<std::string, std::string>, QPointF> session_node_positions_;
    std::map<std::pair<std::string, std::string>, bool> session_collapsed_;
    // N-6: the document's placement for the scope on screen, and the
    // bookkeeping that keeps one gesture from becoming fifty edits.
    // `layout_sent_` is the last payload that went out, so a refresh can tell
    // "the document has caught up" from "the document is still behind the drag".
    GraphCanvasLayout layout_;
    std::string layout_scope_;
    // Null until something has actually been sent: an empty object is a real
    // placement (one that says "nothing is placed"), and a document that has
    // never carried one must not read as "the canvas has caught up".
    nlohmann::json layout_sent_;
    // N-8: the members of the same `layout` object that belong to another
    // panel. Read back from the document on every load and sent again on every
    // write, so this canvas never has to understand them to preserve them.
    nlohmann::json layout_foreign_ = nlohmann::json::object();
    bool layout_dirty_ = false;
    QTimer *layout_timer_ = nullptr;
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
    // N-6.
    void loadLayout(const std::string &scope);
    void pruneLayout();
    void noteLayoutChange();
    [[nodiscard]] QVector<QPointF> bendsFor(const GraphCanvasEdge &edge) const;
    void applyEdgeBends();
    void moveReroute(const GraphCanvasEdgeKey &edge, std::size_t index,
                     const QPointF &at);
    void moveTie(std::size_t index, const QPointF &at);
    GraphCanvasReroute &rerouteFor(const GraphCanvasEdgeKey &edge);
    void detachFromTies(const GraphCanvasEdgeKey &edge);
    bool deleteControlSelection();
    void beginConnection(const QGraphicsItem &socket);
    void finishConnection(const QPointF &position);
    void clearPendingConnection();
    void closeEditor();
    void placeNewNode(const std::string &id);
    void afterEdit(bool submitted);
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
    // The same ingestion boundary the production RPC callback uses.
    void receiveResult(qint64 request_id, const QByteArray &result_json);
    void receiveFailure(qint64 request_id, const QString &message);
    void setConnected(bool connected);

    GraphCanvasModel &model();
    GraphCanvasScene &scene();
};

} // namespace PelicanStudio
