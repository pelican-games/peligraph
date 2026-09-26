#pragma once

#include "../model/graphcanvasmodel.hpp"
#include "nodegraphics.hpp"
#include "nodelayout.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QString>
#include <QStringList>

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

NodeGraphLayout placeNodes(const std::vector<Node> &nodes,
                          const std::vector<GraphCanvasEdge> &edges, qreal top);

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

struct Zone {
    // The loop, drawn the way its node is: heading, badge, sockets.
    Node node;
    QString definition;
    QString stage;
    QString shared;
    std::vector<ZoneTab> tabs;
    std::vector<ZoneRow> inputs;
    std::vector<ZoneRow> outputs;
    // The body's extent in its own coordinates (null for an empty body).
    QRectF content;
    // Computed by layoutZone: the frame in the item's coordinates (its
    // top-left is the item's position unless the body reaches left of or
    // above its origin), and where the body's origin is in them.
    QRectF frame;
    QPointF origin;
};

// The frame around `zone.content`, and the body's origin, with room for the
// heading, the tabs and the edge rows. Also writes the frame's size into
// `zone.node` so the root placement makes room for all of it.
void layoutZone(Zone &zone, const QFontMetricsF &heading_metrics,
                const QFontMetricsF &row_metrics);

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
