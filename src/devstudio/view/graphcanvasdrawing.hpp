#pragma once

#include "../model/graphcanvaslayerstack.hpp"
#include "../model/graphcanvasmodel.hpp"
#include "../model/graphcanvasthumbnail.hpp"
#include "nodegraphics.hpp"
#include "nodelayout.hpp"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QString>
#include <QStringList>

#include <QPainterPath>
#include <QRectF>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

struct StudioTheme;
class GraphCanvasThumbnails;
class GraphCanvasThumbnailFrame;
struct GraphCanvasEdgeKey;
struct GraphCanvasTie;

// Internal drawing helpers. The scene owns document/session state and all
// item callbacks; this module only measures rows and creates their Qt items.
namespace GraphCanvasDrawing {

QString portSummary(const GraphCanvasPort &port);
QString numberText(double value);
// AB-1s, decision 4: a layer band that says a baked table is stale is
// drawn orange; any other in the theme's dim text.
QColor layerBandColor(const QString &text, const StudioTheme &theme);

struct Value {
    QString text;
    QString source;
    [[nodiscard]] bool shown() const { return !text.isEmpty(); }
};

struct Row {
    QString port;
    QString label;
    QString type;
    SocketStyle style;
    Value value;
    bool connected = false;
    bool declared = false;
    // Q-19: a socket drawn from the pass's shader declarations. `port` is
    // then its key ("in@0", "in:variadic"); `bound_port` the numbered port
    // connected there. `band` marks the variadic group's members and its
    // drop target, which are drawn on one band; `drop` the target itself.
    QString socket_key;
    QString bound_port;
    bool band = false;
    bool drop = false;
    // NB-1 (design_node_bodies 2.2): a line ends on this row -- an authored
    // edge into the input, or anything drawn from the output (an edge, a
    // rendering-section pass's input, a zone's body output, a cut end). What
    // the compact view keeps.
    bool linked = false;
    // NB-1, set by the node's body when it measures the node: the row's
    // height, and whether the node's view draws it at all.
    qreal height = 0.0;
    bool shown = true;
};

class NodeBody;

// NB-2b (design_node_bodies 2.3): one row of a canvas.stack's body -- one
// element of its `layers`, the rows top first -- and the words its marks say.
// (NB-3: a layer's filters are no mark on its row; they are the filter.*
// nodes on the line into it.)
struct LayerLine {
    GraphCanvasStackRow row;
    // The blend's words when it is not normal ("Screen"), else empty.
    QString blend;
    // A group or an import: `›` goes into it.
    bool enters = false;
};

// Borrowed from the model's node snapshot for the duration of one rebuild.
// No graphics item or callback retains this drawing data.
struct Node {
    const GraphCanvasNode *node = nullptr;
    std::vector<Row> outputs;
    std::vector<Row> inputs;
    qreal width = 0.0;
    qreal height = 0.0;
    // NB-1: how much of the node is drawn, and the body that draws it (by
    // the node's op, nodeBodyFor).
    GraphCanvasNodeView view = GraphCanvasNodeView::open;
    const NodeBody *body = nullptr;
    // LP-4d1: a root loop whose zone is folded: drawn as its node, sockets
    // and all, with the triangle that opens the zone again.
    bool zone_folded = false;
    // NB-2b: a canvas.stack's layers (top first), whether its scope is an
    // imported canvas (read only), and the input of the row picked in it.
    std::vector<LayerLine> layers;
    bool layers_read_only = false;
    QString picked_layer;

    // The heading alone: the old folded node.
    [[nodiscard]] bool minimal() const noexcept { return view == GraphCanvasNodeView::minimal; }
};

class Builder;

// NB-1 (design_node_bodies 2.1). How one kind of node draws what is under its
// heading band: the rows and their order, each row's height and parts, where
// the sockets stand, and what each view of the fold leaves of them. Chosen by
// op (nodeBodyFor); a body draws, it never edits -- an edit goes out as the
// op list a model function builds, as before. The heading band, the node's
// outline and its item data are the Builder's, the same for every body.
//
// A body puts each socket through Builder::addSocket (a row's) or
// Builder::addAnchor (the one point a hidden row's lines gather on): they
// carry the data the lines find their ends by -- the socket kind,
// NodeGraphNameRole, GraphCanvasPortRole and the direction -- so edge routing
// does not know which body drew a node.
//
// NB-2's layer stack (`canvas.stack`) and NB-3's card (the canvas sources and
// filters) are bodies of their own, added to nodeBodyFor's table.
class NodeBody {
  public:
    NodeBody() = default;
    NodeBody(const NodeBody &) = delete;
    NodeBody &operator=(const NodeBody &) = delete;
    virtual ~NodeBody() = default;
    // Given the width the heading needs (entry.width, at least the minimum),
    // sets the node's width and height for entry.view, and each row's
    // `height` and `shown`.
    virtual void measure(Node &entry, const Builder &builder) const = 0;
    // Draws everything under the heading band onto the node's item.
    virtual void draw(Builder &builder, QGraphicsPathItem &item, const Node &entry) const = 0;
    // The view a press on the heading's triangle goes to from `view`.
    [[nodiscard]] virtual GraphCanvasNodeView next(GraphCanvasNodeView view) const;
};

// The body that draws `op`'s nodes: the default rows unless the table names
// another.
[[nodiscard]] const NodeBody &nodeBodyFor(std::string_view op);
// The default body: a band of output rows, then the input rows, 21 high.
// Compact keeps the linked rows, minimal the heading with one point a side.
[[nodiscard]] const NodeBody &defaultNodeBody();
// NB-2b (design_node_bodies 2.3, graphcanvasstackbody.cpp): a canvas.stack's
// body. Open: the composite's frame and the output, the layer rows top first
// (each with its layer's socket, the eye, a thumbnail frame, the name and the
// marks), and the band with + ▾, the bin and `+ layer`. Compact: the
// composite's frame and one slim row per layer, its sockets kept. Minimal:
// the heading, as every body's.
[[nodiscard]] const NodeBody &layerStackNodeBody();
// NB-3 (design_node_bodies 2.4): the card of a canvas source or filter
// (graphCanvasCardOperation). Open: the picture of the layer it hands on
// (64 px) under the heading, then the default body's socket rows. Compact:
// the heading and the picture, the lines on one point a side. A filter says
// its one number beside its heading (graphCanvasFilterSummary).
[[nodiscard]] const NodeBody &cardNodeBody();
// The card's picture section: the 64 px frame and its margin above and below.
inline constexpr qreal CardPictureSize = 64.0;
inline constexpr qreal CardPictureMargin = 6.0;
// NB-2b: whether `scope` is an imported canvas (a PSD's definition, which a
// layer of some stack of the document reads): its stacks are read only.
[[nodiscard]] bool layerScopeReadOnly(const GraphCanvasModel &model, std::string_view scope);
// The rows of a stack body as the canvas draws them: open rows are this tall
// (a 32 px thumbnail and its margin), compact ones a socket row's height.
inline constexpr qreal LayerRowHeight = 40.0;
inline constexpr qreal LayerSlimRowHeight = 21.0;

// GL-1b: `extra_nodes` and `extra_edges` go into the same one placement after
// the graph's own -- the root's folded nodes and the lines the plan implies --
// so `bends[i]` still belongs to `edges[i]`. UX-1, decision 6: the ids in
// `last_column` (LayoutOptions::last_column) stand in the last column.
NodeGraphLayout placeNodes(const std::vector<Node> &nodes,
                          const std::vector<GraphCanvasEdge> &edges, qreal top,
                          const std::vector<LayoutNode> &extra_nodes = {},
                          const std::vector<LayoutEdge> &extra_edges = {},
                          const std::set<std::string, std::less<>> &last_column = {});

// LP-4d1, the loop zone. One row of a frame's left or right edge: the loop's
// own port outside (empty when the loop has none of that name) and the body's
// end of it inside -- a boundary input's graph.input id on the left, a body
// output's name on the right (empty when the body has none).
struct ZoneRow {
    QString outer;
    QString inner;
    QString label;
    Row outer_row;
};

struct ZoneTab {
    QString text;
    std::size_t branch = 0;
    bool current = false;
    // A branch that runs nothing (`"body": null`) has no body to show.
    bool runs = true;
    // UX-1, decision 4: the tab of another branch that runs the same body
    // ("else", "branch 1"), drawn as a mark on the tab; empty when the body
    // is this branch's alone.
    QString shared_with;
    // Why a press on the tab would change nothing (a body the document does
    // not define); the tab is drawn faint and says so in its tooltip.
    QString reason;
};

// LP-4d2, decision 3: one pair of the loop's `carry`, drawn as an arrow
// round the bottom of the frame from the body output `from` (right edge,
// inside) back to the boundary input `to` (left edge, inside). `unbound` is
// LP-4c's warning: `to` has no loop input edge to start from.
struct ZoneCarry {
    QString from;
    QString to;
    bool unbound = false;
    QString warning;
    bool selected = false;
};

// What the frame is drawn around besides its body: the body's origin in the
// frame item, the least the frame may be (heading, tabs, edge rows, footer),
// and the footer's height. Kept by the scene, so a frame can follow its body
// while one of its nodes is being dragged (decision 7).
struct ZoneFrameMetrics {
    QPointF origin;
    qreal min_width = 0.0;
    qreal min_height = 0.0;
    qreal footer = 0.0;
};

struct Zone {
    // The loop, drawn the way its node is: heading, badge, sockets.
    Node node;
    QString definition;
    QString stage;
    QString shared;
    std::vector<ZoneTab> tabs;
    // LP-4f1, decision 3: a loop over a view axis with one branch names its
    // views (+X, -X, ...) small, where the tabs would be.
    std::vector<QString> axis_labels;
    std::vector<ZoneRow> inputs;
    std::vector<ZoneRow> outputs;
    // LP-4d2, decision 1 and 2: an empty slot at the end of each edge, where
    // a line is dropped to make a new loop input (left, `in:+`) or a new body
    // output (right, `out:+`). Only while the document can be edited.
    bool edge_slots = false;
    std::vector<ZoneCarry> carries;
    // Decision 5: one line along the bottom of the frame -- `×N copies` for
    // an expand loop (with Show in Frame Plan), `N materials` for a
    // draw_loop. Empty before a report.
    QString expansion;
    bool frame_plan_button = false;
    // The body's extent in its own coordinates (null for an empty body).
    QRectF content;
    // Computed by layoutZone: the frame in the item's coordinates (its
    // top-left is the item's position unless the body reaches left of or
    // above its origin), and where the body's origin is in them.
    QRectF frame;
    QPointF origin;
    ZoneFrameMetrics metrics;
};

// The frame around `zone.content`, and the body's origin, with room for the
// heading, the tabs and the edge rows. Also writes the frame's size into
// `zone.node` so the root placement makes room for all of it.
void layoutZone(Zone &zone, const QFontMetricsF &heading_metrics,
                const QFontMetricsF &row_metrics);
// The frame around a body of this extent (its own coordinates), in the
// frame item's: what layoutZone computes once the least size is known.
QRectF zoneFrame(const ZoneFrameMetrics &metrics, const QRectF &content);
// Decision 7: draw an existing frame item again at another size without
// rebuilding it -- its outline, the inside, the heading band, and every
// thing on its edges and its bottom moved with the edge it stands on. The
// body inside does not move.
void refitZoneFrame(QGraphicsPathItem &frame, const QRectF &from, const QRectF &to);
// The path of one carry's arrow: down from the output's inside socket to
// `lane_y` (under the body), across, and up to the input's. In the frame
// item's coordinates.
QPainterPath zoneCarryPath(const QPointF &from, const QPointF &to, qreal lane_y);
// Draws one carry arrow (graph_zone_carry, a child of its frame) between the
// two sockets it joins, in its lane above the frame's bottom line.
void routeZoneCarry(QGraphicsPathItem &carry);

class Builder {
  public:
    using Moved = std::function<void(const QPointF &)>;

    Builder(QGraphicsScene &scene, QString scope);
    // NB-1: `linked_outputs` names the node's outputs a line is drawn from
    // (the compact view keeps their rows); the inputs say so themselves.
    // NB-2b: `scope` is the scope `node` is in when it is not the Builder's
    // (a zone's body); a canvas.stack reads its layers' rows from it.
    Node prepareNode(const GraphCanvasModel &model, const GraphCanvasNode &node,
                     GraphCanvasNodeView view,
                     const std::set<std::string, std::less<>> &linked_outputs = {},
                     const std::optional<std::string> &scope = std::nullopt) const;
    // NB-2b: the layer row picked on the canvas (its stack's scope and id,
    // and the layer's input), drawn picked.
    void setPickedLayer(std::string scope, std::string stack, std::string input);
    // LP-4d1: draw inside a zone's body item instead of at the top of the
    // scene. Positions are then the body's own, and every item carries the
    // definition in GraphCanvasZoneScopeRole.
    void setContainer(QGraphicsItem *container);
    void addNode(const Node &entry, int column, const QPointF &position, Moved moved);
    // The frame, its heading, tabs, marks and edge sockets. Returns the item
    // the body is drawn in (positioned at the body's origin).
    QGraphicsItem *addZone(const Zone &zone, int column, const QPointF &position, Moved moved);
    // LP-4d2, decision 4: an edge that reads one element of a `p[]` is drawn
    // dashed, with the element on a small label (graph_edge_label).
    QGraphicsPathItem *addEdge(const GraphCanvasEdge &edge, const QVector<QPointF> &bends);
    // A body output's line: from the port that makes it to the frame's inside
    // socket for it. Drawing only.
    QGraphicsPathItem *addZoneLink(const QString &loop, const QString &output,
                                   const QString &from_node, const QString &from_port);
    void addReroute(const GraphCanvasEdgeKey &key, std::size_t index,
                   const QPointF &position, Moved moved);
    void addTie(const GraphCanvasTie &tie, std::size_t index, Moved moved);
    void addBreadcrumb(const QStringList &trail, qreal top,
                       std::function<void()> leave_scope);

    // --- GL-1: folded nodes, the lines the frame plan implies, the cut ends
    // of lines to hidden domains, and a folded view's button.

    // The size a folded node is drawn at: its heading, its band (a feature
    // says how many passes), and one row per resource it hands on or reads.
    [[nodiscard]] QSizeF derivedNodeSize(const GraphCanvasFoldedNode &node) const;
    // Dashed and a step dimmer than a graph node, with the plan's source in
    // its tooltip. Movable for the session only (`moved`).
    QGraphicsItem *addDerivedNode(const GraphCanvasFoldedNode &node, const QPointF &position,
                                  Moved moved);
    QGraphicsPathItem *addDerivedEdge(const GraphCanvasDerivedEdge &edge);
    // GL-1e: one item of a rendering-section pass's `input`, drawn solid from
    // the node (and output) the resource comes from.
    QGraphicsPathItem *addPassInputEdge(const QString &from, const QString &from_port,
                                        const QString &pass, const QString &resource,
                                        std::size_t input_index);
    // `<- value (N)` beside an input socket, or `-> pose (N)` beside an
    // output socket; drawn on the socket's node, so it goes where it goes.
    void addDomainStub(const GraphCanvasDomainStub &stub, QGraphicsItem &socket);
    void addFoldedButton(const QString &name, const QString &text, const QPointF &at);
    // GL-2b, decision 2: one line under a root node's lower edge, what its
    // lower layers hold (`×3 copies · 2 barriers`). Read only; clicks go
    // through it.
    void addLayerBand(QGraphicsItem &node, const QString &name, const QString &text);
    [[nodiscard]] const QFontMetricsF &headingMetrics() const noexcept { return heading_metrics_; }
    [[nodiscard]] const QFontMetricsF &rowMetrics() const noexcept { return row_metrics_; }

    // --- NB-1: what a NodeBody draws with.
    [[nodiscard]] const StudioTheme &theme() const noexcept { return theme_; }
    [[nodiscard]] const QFont &detailFont() const noexcept { return detail_font_; }
    [[nodiscard]] const QFont &labelFont() const noexcept { return label_font_; }
    // Kind, scope and node name on a part of `entry`'s node.
    void tagPart(QGraphicsItem &part, const Node &entry, const char *kind) const;
    // One row's socket at `centre` (the item's coordinates), `direction`
    // "input" or "output": what an edge into or out of `row.port` ends on.
    QGraphicsPathItem *addSocket(QGraphicsItem &item, const Node &entry, const Row &row,
                                 const QPointF &centre, const char *direction);
    // The one point a side every line of the rows not drawn gathers on: the
    // empty port, which stands for every port at once.
    QGraphicsPathItem *addAnchor(QGraphicsItem &item, const Node &entry, const QPointF &centre,
                                 const char *direction);
    // NB-3: the scope the nodes are drawn in, and the pictures' maker (none:
    // the frames stay empty, as before NB-3).
    [[nodiscard]] const QString &scope() const noexcept { return scope_text_; }
    void setThumbnails(GraphCanvasThumbnails *thumbnails) { thumbnails_ = thumbnails; }
    // A picture's frame at `frame` on `item`: grey while the picture is made
    // (and when it cannot be, the tooltip saying why), the picture once it is.
    GraphCanvasThumbnailFrame *addThumbnail(QGraphicsItem &item, const QRectF &frame,
                                            const GraphCanvasThumbnailTarget &target, const QString &tooltip) const;

  private:
    void addNodeHeader(QGraphicsPathItem *item, const Node &entry);
    MovableNodeItem *addNodeItem(const Node &entry, int column, const QPainterPath &path,
                                 const QPointF &position, Moved moved);
    void place(QGraphicsItem *item);
    void tag(QGraphicsItem *item) const;

    QGraphicsScene &scene_;
    QString scope_text_;
    QGraphicsItem *container_ = nullptr;
    const StudioTheme &theme_;
    QFont label_font_;
    QFont detail_font_;
    QFont heading_font_;
    QFontMetricsF heading_metrics_;
    QFontMetricsF row_metrics_;
    // NB-2b.
    std::string picked_layer_scope_;
    std::string picked_layer_stack_;
    std::string picked_layer_input_;
    mutable std::map<std::string, bool, std::less<>> read_only_scopes_;
    // NB-3.
    GraphCanvasThumbnails *thumbnails_ = nullptr;
};

} // namespace GraphCanvasDrawing
} // namespace PelicanStudio
