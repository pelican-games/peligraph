#include "nodegraphics.hpp"

#include "studiotheme.hpp"

#include "battery/embed.hpp"

#include <QGraphicsItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QScrollBar>
#include <QVector>
#include <QVariant>
#include <QWheelEvent>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <utility>

namespace PelicanStudio {
namespace {

QString qtext(const std::string &value) {
    return QString::fromStdString(value);
}

using Json = nlohmann::json;

std::optional<SocketShape> shapeFromName(std::string_view name) {
    if (name == "circle") return SocketShape::circle;
    if (name == "diamond") return SocketShape::diamond;
    if (name == "square") return SocketShape::square;
    if (name == "hexagon") return SocketShape::hexagon;
    return std::nullopt;
}

// A declaration that names a colour Qt cannot parse is a declaration that says
// nothing about colour; it must not black out the socket.
void applyEntry(SocketStyle &style, const Json &entry) {
    if (!entry.is_object()) return;
    if (const auto shape = entry.find("shape");
        shape != entry.end() && shape->is_string())
        if (const auto parsed = shapeFromName(shape->get<std::string>()))
            style.shape = *parsed;
    for (const char *key : {"fill", "color"}) {
        const auto fill = entry.find(key);
        if (fill == entry.end() || !fill->is_string()) continue;
        const QColor colour{qtext(fill->get<std::string>())};
        if (colour.isValid()) style.fill = colour;
    }
    if (const auto outline = entry.find("outline");
        outline != entry.end() && outline->is_string()) {
        const QColor colour{qtext(outline->get<std::string>())};
        if (colour.isValid()) style.outline = colour;
    }
    if (const auto size = entry.find("size"); size != entry.end() && size->is_number())
        style.size = std::clamp(size->get<qreal>(), 5.0, 24.0);
    if (const auto width = entry.find("outline_width");
        width != entry.end() && width->is_number())
        style.outline_width = std::clamp(width->get<qreal>(), 0.5, 6.0);
}

// The one place the shipped table lives once it is parsed. A malformed
// resource leaves an empty table rather than taking the canvas down: every
// type then draws through the traits rule, which is exactly the control.
Json &socketDefaults() {
    static Json table = Json::object();
    return table;
}
bool &socketDefaultsLoaded() {
    static bool loaded = false;
    return loaded;
}

const Json &socketDefaultsTable() {
    if (!socketDefaultsLoaded()) resetSocketStyleDefaults();
    return socketDefaults();
}

} // namespace

void setSocketStyleDefaults(std::string_view json_text) {
    socketDefaultsLoaded() = true;
    socketDefaults() = Json::parse(json_text, nullptr, false);
    if (!socketDefaults().is_object()) socketDefaults() = Json::object();
}

void resetSocketStyleDefaults() {
    static const std::string shipped =
        b::embed<"graph_socket_styles.json">().str();
    setSocketStyleDefaults(shipped);
}

SocketStyle socketStyleFor(const SocketTypeFacts &facts) {
    const Json &table = socketDefaultsTable();
    // The last resort, and the base every rule below only overrides parts of:
    // a socket in the theme's own two colours says "this canvas has no claim
    // about the type". The fills below are type identity and stay put in both
    // themes; the rim and the no-claim fill are the theme's, so a dark body
    // and a light one both keep the socket legible.
    SocketStyle style;
    const StudioTheme &theme = currentStudioTheme();
    style.fill = theme.canvas.heading_text;
    style.outline = theme.canvas.node_border;
    applyEntry(style, table.value("unknown", Json::object()));

    // (i) The module declared its own look. It wins outright: a type's owner
    // knows more about it than a table Studio ships.
    if (facts.has_display) {
        Json declaration = Json::object();
        if (!facts.display_shape.empty()) declaration["shape"] = facts.display_shape;
        if (!facts.display_color.empty()) declaration["color"] = facts.display_color;
        applyEntry(style, declaration);
        return style;
    }

    // (ii) The Studio default table, by exact type id and then by namespace.
    if (!facts.type.empty()) {
        const auto types = table.find("types");
        if (types != table.end() && types->is_object()) {
            const auto named = types->find(facts.type);
            if (named != types->end()) {
                applyEntry(style, *named);
                return style;
            }
        }
        const auto prefixes = table.find("prefixes");
        if (prefixes != table.end() && prefixes->is_array())
            for (const auto &rule : *prefixes) {
                if (!rule.is_object()) continue;
                const auto prefix = rule.value("prefix", std::string{});
                if (prefix.empty() || !facts.type.starts_with(prefix)) continue;
                applyEntry(style, rule);
                return style;
            }
    }

    // (iii) The traits. The table may extend them; "opaque" is the one rule
    // that holds without a table at all, because a handle the graph never
    // reads is not a value and must not look like one.
    const auto trait_rules = table.find("traits");
    if (trait_rules != table.end() && trait_rules->is_array())
        for (const auto &rule : *trait_rules) {
            if (!rule.is_object()) continue;
            const auto trait = rule.value("trait", std::string{});
            if (trait.empty() ||
                std::ranges::find(facts.traits, trait) == facts.traits.end())
                continue;
            applyEntry(style, rule);
            return style;
        }
    if (std::ranges::find(facts.traits, "opaque") != facts.traits.end())
        style.shape = SocketShape::diamond;
    return style;
}

QPainterPath socketPath(SocketShape shape, qreal size) {
    const qreal radius = size / 2.0;
    QPainterPath path;
    switch (shape) {
    case SocketShape::circle:
        path.addEllipse(QPointF{0.0, 0.0}, radius, radius);
        break;
    case SocketShape::square:
        path.addRect(QRectF{-radius, -radius, size, size});
        break;
    case SocketShape::diamond: {
        QPolygonF diamond;
        const qreal reach = radius * 1.2;
        diamond << QPointF{0.0, -reach} << QPointF{reach, 0.0}
                << QPointF{0.0, reach} << QPointF{-reach, 0.0};
        path.addPolygon(diamond);
        path.closeSubpath();
        break;
    }
    case SocketShape::hexagon: {
        QPolygonF hexagon;
        const qreal reach = radius * 1.15;
        for (int corner = 0; corner < 6; ++corner) {
            const qreal angle = std::numbers::pi_v<qreal> * (corner / 3.0);
            hexagon << QPointF{reach * std::cos(angle), reach * std::sin(angle)};
        }
        path.addPolygon(hexagon);
        path.closeSubpath();
        break;
    }
    }
    return path;
}

QString nodeGraphItemKind(const QGraphicsItem &item) {
    return item.data(NodeGraphItemKindRole).toString();
}

QVariant MovableNodeItem::itemChange(GraphicsItemChange change,
                                     const QVariant &value) {
    QVariant result = QGraphicsPathItem::itemChange(change, value);
    if (change == QGraphicsItem::ItemPositionHasChanged && moved_) {
        moved_(pos());
    }
    return result;
}

CurveGeometry curveGeometry(const QRectF &from, const QRectF &to) {
    QPointF start;
    QPointF tip;
    QPointF control_one;
    QPointF control_two;

    const qreal horizontal_distance =
        std::abs(from.center().x() - to.center().x());
    if (horizontal_distance < 20.0) {
        const qreal direction = to.center().y() >= from.center().y() ? 1.0
                                                                     : -1.0;
        start = direction > 0.0
                    ? QPointF{from.center().x(), from.bottom()}
                    : QPointF{from.center().x(), from.top()};
        tip = direction > 0.0 ? QPointF{to.center().x(), to.top()}
                              : QPointF{to.center().x(), to.bottom()};
        const qreal control_distance =
            std::max<qreal>(52.0, std::abs(tip.y() - start.y()) * 0.45);
        control_one = start + QPointF{0.0, direction * control_distance};
        control_two = tip - QPointF{0.0, direction * control_distance};
    } else {
        const qreal direction = to.center().x() >= from.center().x() ? 1.0
                                                                     : -1.0;
        start = direction > 0.0 ? QPointF{from.right(), from.center().y()}
                                : QPointF{from.left(), from.center().y()};
        tip = direction > 0.0 ? QPointF{to.left(), to.center().y()}
                              : QPointF{to.right(), to.center().y()};
        const qreal control_distance =
            std::max<qreal>(58.0, std::abs(tip.x() - start.x()) * 0.45);
        control_one = start + QPointF{direction * control_distance, 0.0};
        control_two = tip - QPointF{direction * control_distance, 0.0};
    }

    QPainterPath path{start};
    path.cubicTo(control_one, control_two, tip);

    QLineF tangent{control_two, tip};
    if (tangent.length() < 0.001) {
        tangent = QLineF{start, tip};
    }
    if (tangent.length() < 0.001) {
        tangent = QLineF{QPointF{0.0, 0.0}, QPointF{1.0, 0.0}};
    }
    const QPointF unit{tangent.dx() / tangent.length(),
                       tangent.dy() / tangent.length()};
    const QPointF perpendicular{-unit.y(), unit.x()};
    const QPointF base = tip - unit * 11.0;
    QPolygonF arrow;
    arrow << tip << base + perpendicular * 5.0
          << base - perpendicular * 5.0;

    const QPointF label_center = path.pointAtPercent(0.5);
    return CurveGeometry{std::move(path), std::move(arrow), label_center};
}

QVector<QPointF> nodeGraphBends(const QGraphicsItem &item) {
    QVector<QPointF> bends;
    for (const QVariant &point : item.data(NodeGraphBendsRole).toList())
        bends.push_back(point.toPointF());
    return bends;
}

void setNodeGraphBends(QGraphicsItem &item, const QVector<QPointF> &bends) {
    QVariantList stored;
    stored.reserve(bends.size());
    for (const QPointF &point : bends) stored.push_back(point);
    item.setData(NodeGraphBendsRole, stored);
}

CurveGeometry curveGeometryThrough(const QPointF &start,
                                   const QVector<QPointF> &bends,
                                   const QPointF &tip) {
    QVector<QPointF> stops;
    stops.push_back(start);
    for (const QPointF &bend : bends) stops.push_back(bend);
    stops.push_back(tip);

    QPainterPath path{stops.front()};
    QPointF last_control = stops.front();
    for (int leg = 0; leg + 1 < stops.size(); ++leg) {
        const QPointF from = stops[leg];
        const QPointF to = stops[leg + 1];
        // Horizontal handles: a leg between two bends in the same column has
        // the same y at both ends and comes out as a level line, so the slope
        // lives in the gaps, where no node does.
        const qreal reach =
            std::max<qreal>(16.0, std::abs(to.x() - from.x()) * 0.42);
        const QPointF control_one = from + QPointF{reach, 0.0};
        const QPointF control_two = to - QPointF{reach, 0.0};
        path.cubicTo(control_one, control_two, to);
        last_control = control_two;
    }

    QLineF tangent{last_control, tip};
    if (tangent.length() < 0.001) tangent = QLineF{start, tip};
    if (tangent.length() < 0.001)
        tangent = QLineF{QPointF{0.0, 0.0}, QPointF{1.0, 0.0}};
    const QPointF unit{tangent.dx() / tangent.length(),
                       tangent.dy() / tangent.length()};
    const QPointF perpendicular{-unit.y(), unit.x()};
    const QPointF base = tip - unit * 10.0;
    QPolygonF arrow;
    arrow << tip << base + perpendicular * 4.5 << base - perpendicular * 4.5;
    const QPointF label_center = path.pointAtPercent(0.5);
    return CurveGeometry{std::move(path), std::move(arrow), label_center};
}

CurveGeometry curveGeometryBetween(const QPointF &start, const QPointF &tip) {
    // Both tangents point outward, the way a node editor draws them: an edge
    // that has to double back loops rather than cutting through the nodes.
    const qreal control_distance =
        std::max<qreal>(46.0, std::abs(tip.x() - start.x()) * 0.5);
    const QPointF control_one = start + QPointF{control_distance, 0.0};
    const QPointF control_two = tip - QPointF{control_distance, 0.0};

    QPainterPath path{start};
    path.cubicTo(control_one, control_two, tip);

    QLineF tangent{control_two, tip};
    if (tangent.length() < 0.001) tangent = QLineF{start, tip};
    if (tangent.length() < 0.001)
        tangent = QLineF{QPointF{0.0, 0.0}, QPointF{1.0, 0.0}};
    const QPointF unit{tangent.dx() / tangent.length(),
                       tangent.dy() / tangent.length()};
    const QPointF perpendicular{-unit.y(), unit.x()};
    const QPointF base = tip - unit * 10.0;
    QPolygonF arrow;
    arrow << tip << base + perpendicular * 4.5 << base - perpendicular * 4.5;
    const QPointF label_center = path.pointAtPercent(0.5);
    return CurveGeometry{std::move(path), std::move(arrow), label_center};
}

QPainterPath nodePath(bool anchor) {
    QPainterPath path;
    if (anchor) {
        const QPointF center{18.0, NodeGraphNodeHeight / 2.0};
        QPolygonF diamond;
        diamond << QPointF{center.x(), center.y() - 16.0}
                << QPointF{center.x() + 16.0, center.y()}
                << QPointF{center.x(), center.y() + 16.0}
                << QPointF{center.x() - 16.0, center.y()};
        path.addPolygon(diamond);
        path.closeSubpath();
    } else {
        path.addRoundedRect(
            QRectF{0.0, 0.0, NodeGraphNodeWidth, NodeGraphNodeHeight}, 9.0,
            9.0);
    }
    return path;
}

QPainterPath roundedEntityPath(qreal width, qreal height, qreal radius) {
    QPainterPath path;
    path.addRoundedRect(QRectF{0.0, 0.0, width, height}, radius, radius);
    return path;
}

QGraphicsItem *findNodeItem(QGraphicsScene &scene,
                            const NodeGraphKinds &kinds, const QString &graph,
                            const QString &name) {
    for (QGraphicsItem *item : scene.items()) {
        if (kinds.node_kinds.contains(nodeGraphItemKind(*item)) &&
            item->data(NodeGraphGraphRole).toString() == graph &&
            item->data(NodeGraphNameRole).toString() == name) {
            return item;
        }
    }
    return nullptr;
}

QGraphicsItem *findEndpointArtifact(QGraphicsScene &scene,
                                    const QString &kind_value,
                                    const QString &graph, const QString &from,
                                    const QString &to) {
    for (QGraphicsItem *item : scene.items()) {
        if (nodeGraphItemKind(*item) == kind_value &&
            item->data(NodeGraphGraphRole).toString() == graph &&
            item->data(NodeGraphFromNameRole).toString() == from &&
            item->data(NodeGraphToNameRole).toString() == to) {
            return item;
        }
    }
    return nullptr;
}

void routeLogicalBundle(QGraphicsScene &scene, QGraphicsPathItem &edge,
                        const NodeGraphKinds &kinds) {
    const QString graph = edge.data(NodeGraphGraphRole).toString();
    const QString from_name = edge.data(NodeGraphFromNameRole).toString();
    const QString to_name = edge.data(NodeGraphToNameRole).toString();
    QGraphicsItem *from = findNodeItem(scene, kinds, graph, from_name);
    QGraphicsItem *to = findNodeItem(scene, kinds, graph, to_name);
    if (from == nullptr || to == nullptr) {
        return;
    }

    const QVector<QPointF> bends = nodeGraphBends(edge);
    const CurveGeometry geometry =
        bends.isEmpty()
            ? curveGeometry(from->sceneBoundingRect(), to->sceneBoundingRect())
            : curveGeometryThrough(
                  QPointF{from->sceneBoundingRect().right(),
                          from->sceneBoundingRect().center().y()},
                  bends,
                  QPointF{to->sceneBoundingRect().left(),
                          to->sceneBoundingRect().center().y()});
    edge.setPath(geometry.path);
    if (auto *arrow = dynamic_cast<QGraphicsPolygonItem *>(
            findEndpointArtifact(scene, kinds.arrow, graph, from_name,
                                 to_name))) {
        arrow->setPolygon(geometry.arrow);
    }
    if (QGraphicsItem *label = findEndpointArtifact(scene, kinds.label, graph,
                                                    from_name, to_name)) {
        const QRectF bounds = label->boundingRect();
        label->setPos(geometry.label_center - bounds.center());
    }
}

void rerouteConnectedBundles(QGraphicsScene &scene,
                             const NodeGraphKinds &kinds,
                             const std::string &graph,
                             const std::string &node) {
    const QString expected_graph = qtext(graph);
    const QString expected_node = qtext(node);
    for (QGraphicsItem *item : scene.items()) {
        if (nodeGraphItemKind(*item) != kinds.edge ||
            item->data(NodeGraphGraphRole).toString() != expected_graph ||
            (item->data(NodeGraphFromNameRole).toString() != expected_node &&
             item->data(NodeGraphToNameRole).toString() != expected_node)) {
            continue;
        }
        if (auto *edge = dynamic_cast<QGraphicsPathItem *>(item)) {
            // One end has just been placed by hand, so the bends the automatic
            // layout chose for this edge are about a drawing that no longer
            // exists. Drop them and let the direct curve take over.
            edge->setData(NodeGraphBendsRole, QVariant{});
            routeLogicalBundle(scene, *edge, kinds);
        }
    }
}

std::map<std::string, int, std::less<>> longestPathColumns(
    const std::vector<NodeGraphEdgeKey> &edges,
    const std::set<std::string, std::less<>> &visible) {
    std::map<std::string, int, std::less<>> columns;
    for (const auto &name : visible) {
        columns.emplace(name, 0);
    }
    for (std::size_t pass = 0; pass < visible.size(); ++pass) {
        bool changed = false;
        for (const auto &edge : edges) {
            const auto from = columns.find(edge.from);
            const auto to = columns.find(edge.to);
            if (from == columns.end() || to == columns.end()) {
                continue;
            }
            if (to->second < from->second + 1) {
                to->second = from->second + 1;
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }
    return columns;
}

NodeCanvasView::NodeCanvasView(QGraphicsScene *scene, QWidget *parent)
    : QGraphicsView{scene, parent} {
    // Zooming has to keep the point under the cursor still: that is what makes
    // a wheel feel like a magnifier rather than a scrollbar.
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    // Q-2. The ground is a flat fill and a grid: the same picture every paint
    // until the view moves, so Qt keeps it as a pixmap and scrolls it instead
    // of asking for it again.
    setCacheMode(QGraphicsView::CacheBackground);
    // The view saved and restored the painter state around every item it
    // drew. Nothing here needs that: no item in either canvas overrides
    // paint(), so every one of them is a stock Qt shape that sets the pen and
    // brush it draws with. Antialiasing itself stays on -- this is the
    // bookkeeping around the drawing, not the drawing.
    //
    // `DontAdjustForAntialiasing` is deliberately *not* set beside it. Those
    // same stock items pad their boundingRect by the pen width alone, and an
    // antialiased edge reaches past that; dropping Qt's margin would leave
    // trails on a partial repaint, which is what a pan is made of.
    setOptimizationFlag(QGraphicsView::DontSavePainterState);
    setProperty("pelicanZoomScale", 1.0);
    setProperty("pelicanZoomMinimum", DefaultMinimumZoom);
    setProperty("pelicanZoomMaximum", DefaultMaximumZoom);
    setProperty("pelicanZoomBoundary", QString{});
    setProperty("pelicanFitCount", 0);
    setProperty("pelicanViewScope", QString{});
}

void NodeCanvasView::drawBackground(QPainter *painter, const QRectF &rect) {
    if (painter == nullptr) return;
    const StudioTheme &theme = currentStudioTheme();
    painter->fillRect(rect, theme.canvas.background);
    // The grid is what tells the eye the canvas is panning rather than the
    // nodes moving, so its pitch is in scene units and not in pixels.
    constexpr qreal pitch = 48.0;
    QPen pen{theme.canvas.grid};
    pen.setCosmetic(true);
    pen.setWidthF(1.0);
    painter->setPen(pen);
    // Q-2: the same exposed rect under the same theme produces the same lines.
    // Building them again was an allocation of up to ~180 QLineF on every
    // repaint, and a repaint is what a pan is made of.
    if (grid_rect_ != rect || grid_colour_ != theme.canvas.grid) {
        grid_rect_ = rect;
        grid_colour_ = theme.canvas.grid;
        grid_lines_.clear();
        const qreal left = std::floor(rect.left() / pitch) * pitch;
        const qreal top = std::floor(rect.top() / pitch) * pitch;
        for (qreal x = left; x < rect.right(); x += pitch)
            grid_lines_.push_back(QLineF{x, rect.top(), x, rect.bottom()});
        for (qreal y = top; y < rect.bottom(); y += pitch)
            grid_lines_.push_back(QLineF{rect.left(), y, rect.right(), y});
    }
    painter->drawLines(grid_lines_);
}

void NodeCanvasView::changeEvent(QEvent *event) {
    QGraphicsView::changeEvent(event);
    if (event == nullptr) return;
    const QEvent::Type type = event->type();
    if (type != QEvent::PaletteChange &&
        type != QEvent::ApplicationPaletteChange &&
        type != QEvent::StyleChange) {
        return;
    }
    // Both caches hold the old theme's ground. Without this a theme switch
    // repaints the nodes and leaves the canvas standing on the colour it had.
    grid_rect_ = {};
    grid_lines_.clear();
    resetCachedContent();
}

void NodeCanvasView::publishZoom(qreal scale, const QString &boundary) {
    setProperty("pelicanZoomScale", scale);
    setProperty("pelicanZoomBoundary", boundary);
    if (zoom_observer_) zoom_observer_(scale, boundary);
}

void NodeCanvasView::applyZoom(qreal requested) {
    const qreal current = transform().m11();
    const qreal bounded =
        std::clamp(requested, DefaultMinimumZoom, DefaultMaximumZoom);
    QString boundary;
    if (requested < DefaultMinimumZoom) {
        boundary = QStringLiteral("minimum");
    } else if (requested > DefaultMaximumZoom) {
        boundary = QStringLiteral("maximum");
    }
    if (std::abs(bounded - current) > 0.000001)
        scale(bounded / current, bounded / current);
    publishZoom(bounded, boundary);
}

void NodeCanvasView::wheelEvent(QWheelEvent *event) {
    int delta = event->angleDelta().y();
    if (delta == 0) delta = event->pixelDelta().y();
    if (delta == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    applyZoom(transform().m11() *
              std::pow(1.0015, static_cast<qreal>(delta)));
    event->accept();
}

void NodeCanvasView::growPanRoom() {
    if (scene() == nullptr) return;
    const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
    if (visible.width() <= 0.0 || visible.height() <= 0.0) return;
    QRectF room = scene()->sceneRect().united(visible);
    // One viewport of slack on every side. With the view's scene rect strictly
    // larger than the viewport, QGraphicsView stops centring it -- and only
    // then does moving the view actually move what is on screen.
    room.adjust(-visible.width(), -visible.height(), visible.width(),
                visible.height());
    if (sceneRect() != room) setSceneRect(room);
}

void NodeCanvasView::panBy(const QPointF &pixels) {
    const qreal scale = transform().m11();
    if (scale <= 0.0 || viewport() == nullptr) return;
    // The pan moves the view, not the scroll bars. Two things make that work
    // for a drawing that already fits the viewport -- the case the scroll bars
    // could not move at all, because there was nothing to scroll, so the
    // canvas decided for the user where they were allowed to look.
    //
    // The first is the room (see growPanRoom). The second is that where the
    // view looks is then *set*, not nudged: QGraphicsView restores its centre
    // point after every change to the transform, so translating the transform
    // is undone before it is ever painted, and a scroll bar at the end of its
    // range refuses to move further. Naming the point the view is to look at
    // is the one instruction neither of those can quietly ignore.
    growPanRoom();
    const QPointF centre = mapToScene(viewport()->rect().center());
    centerOn(centre - pixels / scale);
    growPanRoom();
}

void NodeCanvasView::fitView() {
    if (scene() == nullptr) return;
    QRectF content = scene()->itemsBoundingRect();
    if (!content.isValid() || content.width() <= 0.0 ||
        content.height() <= 0.0)
        return;
    content.adjust(-24.0, -24.0, 24.0, 24.0);
    // A fit is about the content, so the room a pan opened up goes away first.
    setSceneRect(QRectF{});
    fitInView(content, Qt::KeepAspectRatio);
    // fitInView answers with whatever scale the content needs; the clamp is
    // still the clamp, so a huge graph stops shrinking at the minimum.
    const qreal reached = transform().m11();
    const qreal bounded =
        std::clamp(reached, DefaultMinimumZoom, DefaultMaximumZoom);
    if (std::abs(bounded - reached) > 0.000001)
        scale(bounded / reached, bounded / reached);
    // UX-5a: no smaller than words can be read. What no longer fits is
    // looked at where the canvas says -- the node in hand -- not shrunk.
    if (const qreal now = transform().m11(); fit_floor_ > 0.0 && now < fit_floor_) {
        scale(fit_floor_ / now, fit_floor_ / now);
        const std::optional<QRectF> focus = fit_focus_ ? fit_focus_() : std::nullopt;
        growPanRoom();
        centerOn(focus ? focus->center() : content.center());
        growPanRoom();
    }
    ++fit_count_;
    setProperty("pelicanFitCount", static_cast<qulonglong>(fit_count_));
    publishZoom(transform().m11(), QString{});
}

void NodeCanvasView::rememberScope() {
    if (!scope_known_) return;
    session_views_[scope_] =
        ViewState{transform(), sceneRect(), horizontalScrollBar()->value(),
                  verticalScrollBar()->value()};
}

void NodeCanvasView::showScope(const QString &scope) {
    if (scope_known_ && scope == scope_) {
        // The same scope redrawn: the user's zoom and scroll are theirs.
        rememberScope();
        return;
    }
    rememberScope();
    scope_ = scope;
    scope_known_ = true;
    setProperty("pelicanViewScope", scope);
    const auto seen = session_views_.find(scope);
    if (seen == session_views_.end()) {
        // Never looked at: neither the previous scope's zoom nor a default 1.0
        // says anything about this content, so show all of it.
        fitView();
        rememberScope();
        return;
    }
    // The room a pan opened up is part of where the user was looking: without
    // it the restored scroll values mean something else.
    setSceneRect(seen->second.room);
    setTransform(seen->second.transform);
    horizontalScrollBar()->setValue(seen->second.horizontal);
    verticalScrollBar()->setValue(seen->second.vertical);
    publishZoom(transform().m11(), QString{});
}

void NodeCanvasView::mousePressEvent(QMouseEvent *event) {
    const bool pan_gesture =
        event->button() == Qt::MiddleButton ||
        (event->button() == Qt::LeftButton && space_held_);
    if (pan_gesture) {
        panning_ = true;
        pan_button_ = event->button();
        pan_origin_ = event->position().toPoint();
        pan_previous_mode_ = dragMode();
        setDragMode(QGraphicsView::NoDrag);
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void NodeCanvasView::mouseMoveEvent(QMouseEvent *event) {
    if (!panning_) {
        QGraphicsView::mouseMoveEvent(event);
        return;
    }
    const QPoint position = event->position().toPoint();
    const QPoint delta = position - pan_origin_;
    pan_origin_ = position;
    panBy(QPointF{delta});
    event->accept();
}

void NodeCanvasView::mouseReleaseEvent(QMouseEvent *event) {
    if (panning_ && event->button() == pan_button_) {
        panning_ = false;
        pan_button_ = Qt::NoButton;
        setDragMode(pan_previous_mode_);
        unsetCursor();
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void NodeCanvasView::keyPressEvent(QKeyEvent *event) {
    // An open in-place editor owns the keyboard: "F" is a letter there.
    const bool editing = scene() != nullptr && scene()->focusItem() != nullptr;
    if (!editing && event->key() == Qt::Key_F &&
        event->modifiers() == Qt::NoModifier) {
        fitView();
        event->accept();
        return;
    }
    if (!editing && event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        space_held_ = true;
        setCursor(Qt::OpenHandCursor);
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

void NodeCanvasView::keyReleaseEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        space_held_ = false;
        if (!panning_) unsetCursor();
        event->accept();
        return;
    }
    QGraphicsView::keyReleaseEvent(event);
}

} // namespace PelicanStudio
