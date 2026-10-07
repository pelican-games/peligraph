#pragma once
#include <QPointer>

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QEvent>
#include <QLineF>
#include <QVector>
#include <QGraphicsPathItem>
#include <QGraphicsView>
#include <QPainterPath>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTransform>
#include <Qt>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

class QGraphicsItem;
class QGraphicsScene;
class QKeyEvent;
class QMouseEvent;
class QPainter;
class QWheelEvent;
class QTimer;

namespace PelicanStudio {

// DL-1: opt-in, bounded, GUI-thread trace. Serialization happens only after
// input has returned to the event loop; recording never writes a log.
class GraphCanvasScene;
class NodeDragTrace {
public:
    static constexpr std::size_t Capacity = 65536;
    struct Record {
        const char *stage = "";
        qint64 begin = 0, end = 0;
        quint64 move = 0;
        double x = 0, y = 0;
    };
    void setEnabled(bool enabled) { enabled_ = enabled; }
    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] bool recording() const { return recording_; }
    [[nodiscard]] quint64 generation() const { return generation_; }
    [[nodiscard]] quint64 latestMove() const { return move_; }
    [[nodiscard]] quint64 latestChangedMove() const { return changed_; }
    [[nodiscard]] QRectF changedBounds() const { return changed_bounds_; }
    static qint64 now();
    static NodeDragTrace *forScene(QGraphicsScene *scene);
    static NodeDragTrace *forScene(GraphCanvasScene *scene);
    void begin(QGraphicsScene &scene, QGraphicsItem *node);
    void release();
    ~NodeDragTrace();
    void stop();
    quint64 nextMove() { return ++move_; }
    void positionChanged(QGraphicsItem &item);
    void append(const char *stage, qint64 begin, qint64 end, quint64 move,
                double x = 0, double y = 0);
    [[nodiscard]] QJsonObject dump() const;
    bool write(const QString &path) const;
private:
    QPointer<QGraphicsScene> scene_;
    static thread_local NodeDragTrace *active_;
    bool enabled_ = false, recording_ = false;
    quint64 generation_ = 0, move_ = 0, changed_ = 0;
    qint64 origin_ = 0, release_ = 0;
    std::vector<Record> records_;
    std::size_t written_ = 0;
    QJsonObject metadata_;
    QRectF changed_bounds_;
};

class NodeDragTraceSpan {
public:
    NodeDragTraceSpan(QGraphicsScene *scene, const char *stage);
    NodeDragTraceSpan(GraphCanvasScene *scene, const char *stage);
    NodeDragTraceSpan(GraphCanvasScene &scene, const char *stage) : NodeDragTraceSpan(&scene, stage) {}
    NodeDragTraceSpan(QGraphicsScene &scene, const char *stage)
        : NodeDragTraceSpan(&scene, stage) {}
    ~NodeDragTraceSpan();
    NodeDragTraceSpan(const NodeDragTraceSpan &) = delete;
    NodeDragTraceSpan &operator=(const NodeDragTraceSpan &) = delete;
private:
    NodeDragTrace *trace_;
    const char *stage_;
    qint64 begin_;
    quint64 move_;
};


// Scene-owned routing identity index. Owners invalidate before a structural
// edit and publish a complete index afterwards; geometry never invalidates it.
// Scenes without an index (e.g. Frame Plan) keep the shared linear fallback.
class NodeGraphIndex {
public:
    explicit NodeGraphIndex(QGraphicsScene &scene);
    ~NodeGraphIndex();
    NodeGraphIndex(const NodeGraphIndex &) = delete;
    NodeGraphIndex &operator=(const NodeGraphIndex &) = delete;
    static NodeGraphIndex *forScene(QGraphicsScene &scene);
    void invalidate();
    void publish(const QList<QGraphicsItem *> &items);
    void insert(const QStringList &key, QGraphicsItem *item, bool last = false);
    [[nodiscard]] QGraphicsItem *find(const QStringList &key) const;
    void connect(const QStringList &key, QGraphicsItem *item);
    [[nodiscard]] QList<QGraphicsItem *> connected(const QStringList &key) const;
    [[nodiscard]] const QList<QGraphicsItem *> &items() const { return items_; }
    [[nodiscard]] quint64 generation() const { return generation_; }
    void touch(QGraphicsItem &item);
    QRectF takeChangedBounds();
private:
    QGraphicsScene &scene_;
    bool valid_ = false;
    quint64 generation_ = 0;
    QList<QGraphicsItem *> items_;
    QHash<QStringList, QGraphicsItem *> identities_;
    QHash<QStringList, QList<QGraphicsItem *>> connections_;
    QRectF changed_bounds_;
};


// A routing operation uses the scene index when published, otherwise one
// ordered snapshot. No structural edit is allowed inside this scope.
class NodeGraphLookupScope {
public:
    explicit NodeGraphLookupScope(QGraphicsScene &scene);
    ~NodeGraphLookupScope();
    NodeGraphLookupScope(const NodeGraphLookupScope &) = delete;
    NodeGraphLookupScope &operator=(const NodeGraphLookupScope &) = delete;
private:
    friend QList<QGraphicsItem *> nodeGraphItems(QGraphicsScene &);
    QGraphicsScene &scene_;
    QList<QGraphicsItem *> items_;
    NodeGraphLookupScope *previous_;
    static thread_local NodeGraphLookupScope *current_;
};
QList<QGraphicsItem *> nodeGraphItems(QGraphicsScene &scene);

// Every node canvas in Studio -- the Frame Plan and the common graph -- draws
// the same shapes and reroutes the same way. The identity roles live here so
// one routing function can serve both scenes; the numeric values are the ones
// the Frame Plan already published, so its item data is unchanged.
inline constexpr int NodeGraphItemKindRole = Qt::UserRole + 3060;
inline constexpr int NodeGraphGraphRole = Qt::UserRole + 3061;
inline constexpr int NodeGraphNameRole = Qt::UserRole + 3062;
inline constexpr int NodeGraphFromNameRole = Qt::UserRole + 3067;
inline constexpr int NodeGraphToNameRole = Qt::UserRole + 3068;
// The bend points the automatic layout gave this edge, in scene coordinates,
// as a QVariantList of QPointF. An edge that flies over a column is drawn
// through them so it never crosses a node it has nothing to do with. The
// moment a person drags either end, the bends are dropped: the layout no
// longer speaks for an edge whose ends the user has placed by hand.
inline constexpr int NodeGraphBendsRole = Qt::UserRole + 3059;
// Where a press meant for this item lands, in the item's own coordinates, when
// its middle is not that place: a loop's zone holds its body's nodes, so the
// middle of the frame is often one of them rather than the loop (LP-4d1). The
// automation surface's `click` on the item's name aims here.
inline constexpr int NodeGraphHotspotRole = Qt::UserRole + 3058;

inline constexpr qreal NodeGraphNodeWidth = 240.0;
inline constexpr qreal NodeGraphNodeHeight = 64.0;
inline constexpr qreal NodeGraphHorizontalGap = 88.0;
inline constexpr qreal NodeGraphVerticalGap = 74.0;
inline constexpr qreal NodeGraphStaggerOffset = 132.0;
inline constexpr qreal NodeGraphEdgeLabelHeight = 24.0;
inline constexpr qreal NodeGraphEdgeLabelMinimumWidth = 120.0;
inline constexpr qreal NodeGraphBreadcrumbHeight = 34.0;

// The kind strings one scene uses. Routing has to know which items can be an
// endpoint and which artifacts follow an edge, and those names differ per
// scene, so the caller names them rather than the shared code assuming them.
struct NodeGraphKinds {
    QStringList node_kinds;
    QString edge;
    QString arrow;
    QString label;
};

QString nodeGraphItemKind(const QGraphicsItem &item);

// A path item that reports every position change. Node drags are editor
// session state: the owner records the position and reroutes from here.
class MovableNodeItem final : public QGraphicsPathItem {
  public:
    using Moved = std::function<void(const QPointF &)>;

    MovableNodeItem(QPainterPath path, Moved moved)
        : QGraphicsPathItem{std::move(path)}, moved_{std::move(moved)} {}

  protected:
    QVariant itemChange(GraphicsItemChange change,
                        const QVariant &value) override;

  private:
    Moved moved_;
};

struct CurveGeometry {
    QPainterPath path;
    QPolygonF arrow;
    QPointF label_center;
};

CurveGeometry curveGeometry(const QRectF &from, const QRectF &to);

// The same curve between two exact points rather than two boxes. A socket is
// a point on the node's edge, so an edge that leaves one has no rectangle to
// be clipped against: it starts and ends where the socket is drawn.
CurveGeometry curveGeometryBetween(const QPointF &start, const QPointF &tip);

// The same curve, made to pass through the bend points the layout chose. Each
// leg is one cubic with horizontal handles, so a bend inside a column draws as
// a level line and only the gaps between columns slope -- which is what keeps
// a long edge off the nodes it flies over.
CurveGeometry curveGeometryThrough(const QPointF &start,
                                   const QVector<QPointF> &bends,
                                   const QPointF &tip);

// The bends an edge item carries, read back off NodeGraphBendsRole.
QVector<QPointF> nodeGraphBends(const QGraphicsItem &item);
void setNodeGraphBends(QGraphicsItem &item, const QVector<QPointF> &bends);

// A socket says what type flows through it, by shape as well as by colour: a
// canvas the user can add types to cannot spend a hue per type, and shape
// survives both colour blindness and a dark theme.
enum class SocketShape { circle, diamond, square, hexagon };

struct SocketStyle {
    SocketShape shape = SocketShape::circle;
    QColor fill;
    QColor outline;
    qreal size = 11.0;
    // A thick rim is the one variation left once shape and colour are spent.
    // The Frame Plan spends it on "this read is last frame's value".
    qreal outline_width = 1.1;
};

// Everything the one resolver is allowed to know about a port's type. The
// canvas reads these off the catalog; the resolver never reads the catalog
// itself, so the same call answers for a test's hand-built type and for a
// module that shipped one.
struct SocketTypeFacts {
    // The semantic type id, e.g. "pelican.value.scalar@1". Empty when the
    // catalog never declared a type for the port -- which is a state to draw,
    // not a reason to guess.
    std::string type;
    // (i) The catalog's type schema declared its own look.
    bool has_display = false;
    std::string display_shape;
    std::string display_color;
    // (iii) The traits the catalog's type schema carries.
    std::vector<std::string> traits;
};

// The single resolver. Order: the module's own declaration, then the Studio
// default table, then the traits, then the table's "unknown" entry.
SocketStyle socketStyleFor(const SocketTypeFacts &facts);

// The default table (ii), parsed from the embedded JSON resource on first use.
// Replacing it is how the control runs: an empty table must send every type
// through the traits rule and the built-in fallback.
void setSocketStyleDefaults(std::string_view json_text);
void resetSocketStyleDefaults();

// The socket's outline, centred on the origin so the caller positions it by
// the point the edges attach to.
QPainterPath socketPath(SocketShape shape, qreal size);

QPainterPath nodePath(bool anchor);
QPainterPath roundedEntityPath(qreal width, qreal height, qreal radius);

QGraphicsItem *findNodeItem(QGraphicsScene &scene,
                            const NodeGraphKinds &kinds,
                            const QString &graph, const QString &name);
QGraphicsItem *findEndpointArtifact(QGraphicsScene &scene,
                                    const QString &kind_value,
                                    const QString &graph, const QString &from,
                                    const QString &to);

void routeLogicalBundle(QGraphicsScene &scene, QGraphicsPathItem &edge,
                        const NodeGraphKinds &kinds);
void rerouteConnectedBundles(QGraphicsScene &scene,
                             const NodeGraphKinds &kinds,
                             const std::string &graph,
                             const std::string &node);

// One edge of the relation the columns are computed from. Deliberately not a
// model type: the Frame Plan feeds its dependencies and the common graph feeds
// its port connections into the same longest-path rule.
struct NodeGraphEdgeKey {
    std::string from;
    std::string to;
};

// The longest path inside the visible set, so every edge points left to right.
// A whole-graph depth cannot be used: the visible subset may hold one node per
// level. Relaxation terminates because the relation is acyclic; the pass bound
// keeps a malformed payload from spinning here.
std::map<std::string, int, std::less<>> longestPathColumns(
    const std::vector<NodeGraphEdgeKey> &edges,
    const std::set<std::string, std::less<>> &visible);

// One viewing behaviour for every node canvas in Studio. Both canvases draw
// graphs bigger than the pane, so both need the same three gestures -- zoom at
// the cursor, drag to pan, and one key that puts the whole thing back on
// screen -- and neither should invent its own numbers for them.
class NodeCanvasView : public QGraphicsView {
  public:
    // scale, and "minimum"/"maximum"/"" for the clamp the wheel just hit.
    using ZoomObserver = std::function<void(qreal, const QString &)>;

    // The Frame Plan shipped these limits (WP318) and its test fixes the upper
    // one; the common graph adopts them rather than the two canvases zooming
    // differently.
    static constexpr qreal DefaultMinimumZoom = 0.25;
    static constexpr qreal DefaultMaximumZoom = 4.0;

    explicit NodeCanvasView(QGraphicsScene *scene, QWidget *parent = nullptr);

    void setZoomObserver(ZoomObserver observer) {
        zoom_observer_ = std::move(observer);
    }

    // Put the whole content on screen, within the zoom clamp.
    void fitView();
    NodeDragTrace &dragTrace() { return drag_trace_; }
    static constexpr int DragDurationMs = 2000, DragIntervalMs = 8, DragHoldMs = 0;
    static constexpr int DragDeltaX = 200, DragDeltaY = 60;
    QJsonObject dragPaced(const QPoint &from, const QPoint &delta,
                         int duration_ms = DragDurationMs, int interval_ms = DragIntervalMs,
                         int hold_ms = DragHoldMs);

    // UX-5a: a canvas whose words must stay readable after a fit. The fit
    // stops at `floor`; content still bigger than the pane is then looked at
    // where `focus` says (a scene rect: the picked node, say), or at its
    // middle when `focus` names nothing. 0, the default, is no floor.
    void setFitFloor(qreal floor, std::function<std::optional<QRectF>()> focus = {}) {
        fit_floor_ = floor;
        fit_focus_ = std::move(focus);
    }

    // Move the view by this many pixels, the way the held middle button does.
    // Public so a test can drive the gesture without a desktop.
    void panBy(const QPointF &pixels);

    // The canvas is now showing this scope. A scope seen for the first time
    // (which includes the first draw after a project is opened) is fitted; one
    // being returned to gets back the zoom and scroll it was left at.
    void showScope(const QString &scope);

    [[nodiscard]] qreal zoomScale() const { return transform().m11(); }
    [[nodiscard]] bool panning() const noexcept { return panning_; }

  protected:
    // The ground both canvases stand on: the theme's canvas colour and one
    // grid at a fixed scene pitch. A view that painted its own background
    // would be a second place a theme has to be taught about.
    void paintEvent(QPaintEvent *event) override;
    void drawBackground(QPainter *painter, const QRectF &rect) override;
    // A theme switch changes the ground the cached background pixmap holds.
    void changeEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

  private:
    struct ViewState {
        QTransform transform;
        QRectF room;
        int horizontal = 0;
        int vertical = 0;
    };

    void applyZoom(qreal requested);
    void publishZoom(qreal scale, const QString &boundary);
    void rememberScope();
    // Keep the view's own scene rect big enough that there is always somewhere
    // to pan to. QGraphicsView centres a scene rect that fits the viewport, and
    // that centring cancels a translated transform exactly -- which is why a
    // pan on a small graph used to do nothing at all.
    void growPanRoom();

    NodeDragTrace drag_trace_;
    QTimer *trace_probe_ = nullptr;
    ZoomObserver zoom_observer_;
    // The grid drawn last time, kept so an exposed rect the view has already
    // ground out does not have to be ground out again. The theme colour is
    // part of the key: the same rect under a different theme is a different
    // grid.
    QRectF grid_rect_;
    QColor grid_colour_;
    QVector<QLineF> grid_lines_;
    std::map<QString, ViewState> session_views_;
    QString scope_;
    bool scope_known_ = false;
    bool panning_ = false;
    bool space_held_ = false;
    QPoint pan_origin_;
    Qt::MouseButton pan_button_ = Qt::NoButton;
    DragMode pan_previous_mode_ = QGraphicsView::NoDrag;
    std::size_t fit_count_ = 0;
    qreal fit_floor_ = 0.0;
    std::function<std::optional<QRectF>()> fit_focus_;
};

} // namespace PelicanStudio
