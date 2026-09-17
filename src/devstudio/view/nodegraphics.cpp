#include "nodegraphics.hpp"

#include <QGraphicsItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsScene>
#include <QLineF>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <utility>

namespace PelicanStudio {
namespace {

QString qtext(const std::string &value) {
    return QString::fromStdString(value);
}

} // namespace

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

    const CurveGeometry geometry =
        curveGeometry(from->sceneBoundingRect(), to->sceneBoundingRect());
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

} // namespace PelicanStudio
