#pragma once

#include "../model/graphcanvasmodel.hpp"
#include "nodegraphics.hpp"
#include "nodelayout.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QString>
#include <QStringList>

#include <QPainterPath>
#include <QRectF>

#include <functional>
#include <vector>

namespace PelicanStudio {

struct StudioTheme;
struct GraphCanvasEdgeKey;
struct GraphCanvasTie;

// Internal drawing helpers. The scene owns document/session state and all
// item callbacks; this module only measures rows and creates their Qt items.
namespace GraphCanvasDrawing {

QString portSummary(const GraphCanvasPort &port);
QString numberText(double value);

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
};

// Borrowed from the model's node snapshot for the duration of one rebuild.
// No graphics item or callback retains this drawing data.
struct Node {
    const GraphCanvasNode *node = nullptr;
    std::vector<Row> outputs;
    std::vector<Row> inputs;
    qreal width = 0.0;
    qreal height = 0.0;
    bool folded = false;
    // LP-4d1: a root loop whose zone is folded: drawn as its node, sockets
    // and all, with the triangle that opens the zone again.
    bool zone_folded = false;
};

// GL-1b: `extra_nodes` and `extra_edges` go into the same one placement after
// the graph's own -- the root's folded nodes and the lines the plan implies --
// so `bends[i]` still belongs to `edges[i]`.
NodeGraphLayout placeNodes(const std::vector<Node> &nodes,
                          const std::vector<GraphCanvasEdge> &edges, qreal top,
                          const std::vector<LayoutNode> &extra_nodes = {},
                          const std::vector<LayoutEdge> &extra_edges = {});

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
    Node prepareNode(const GraphCanvasModel &model, const GraphCanvasNode &node,
                     bool folded) const;
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

  private:
    void addNodeHeader(QGraphicsPathItem *item, const Node &entry);
    void addNodeRows(QGraphicsPathItem *item, const Node &entry);
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
};

} // namespace GraphCanvasDrawing
} // namespace PelicanStudio
