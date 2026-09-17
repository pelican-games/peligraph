#pragma once

#include <QColor>
#include <QGraphicsPathItem>
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <Qt>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

class QGraphicsItem;
class QGraphicsScene;

namespace PelicanStudio {

// Every node canvas in Studio -- the Frame Plan and the common graph -- draws
// the same shapes and reroutes the same way. The identity roles live here so
// one routing function can serve both scenes; the numeric values are the ones
// the Frame Plan already published, so its item data is unchanged.
inline constexpr int NodeGraphItemKindRole = Qt::UserRole + 3060;
inline constexpr int NodeGraphGraphRole = Qt::UserRole + 3061;
inline constexpr int NodeGraphNameRole = Qt::UserRole + 3062;
inline constexpr int NodeGraphFromNameRole = Qt::UserRole + 3067;
inline constexpr int NodeGraphToNameRole = Qt::UserRole + 3068;

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

// A socket says what type flows through it, by shape as well as by colour: a
// canvas the user can add types to cannot spend a hue per type, and shape
// survives both colour blindness and a dark theme.
enum class SocketShape { circle, diamond, square, hexagon };

struct SocketStyle {
    SocketShape shape = SocketShape::circle;
    QColor fill;
    QColor outline;
    qreal size = 11.0;
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

} // namespace PelicanStudio
