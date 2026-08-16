#include "frameplangraphics.hpp"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

constexpr qreal NodeWidth = 240.0;
constexpr qreal NodeHeight = 64.0;
constexpr qreal GroupWidth = 260.0;
constexpr qreal GroupHeight = 76.0;
constexpr qreal HorizontalGap = 80.0;
constexpr qreal LabelGap = 10.0;
constexpr qreal EdgeLaneGap = 34.0;
constexpr qreal EdgeLabelHeight = 24.0;

struct GroupDefinition {
    std::string key;
    QString label;
    std::vector<std::string> members;
};

struct VisibleEntity {
    std::string key;
    QString label;
    std::string source;
    std::vector<std::string> members;
    std::vector<std::string> internal_records;
    std::size_t order = 0;
    bool anchor = false;
    bool group = false;
    QRectF geometry;
    QGraphicsPathItem *item = nullptr;
};

struct DependencyRecord {
    std::string from;
    std::string to;
    std::string reason;
    std::string resource;
};

using EntityPair = std::pair<std::string, std::string>;

QString qtext(const std::string &value) {
    return QString::fromStdString(value);
}

QString itemKind(const QGraphicsItem &item) {
    return item.data(FramePlanItemKindRole).toString();
}

bool isAnchor(const FramePlanNode &node) {
    return node.kind == "anchor" && node.name.starts_with("__anchor_") &&
           node.reads.empty() &&
           node.history_reads.empty() && node.writes.empty();
}

QColor sourceColor(const std::string &source) {
    if (source == "project") {
        return QColor{QStringLiteral("#3978a8")};
    }
    if (source.starts_with("feature:")) {
        return QColor{QStringLiteral("#c47b25")};
    }
    if (source == "engine") {
        return QColor{QStringLiteral("#66717e")};
    }
    return QColor{QStringLiteral("#806f68")};
}

std::string recordIdentity(const FramePlanDependency &dependency) {
    constexpr char separator = '\x1f';
    return dependency.from + separator + dependency.to + separator +
           dependency.reason + separator + dependency.resource;
}

QStringList qlist(const std::vector<std::string> &values) {
    QStringList result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const auto &value : values) {
        result.push_back(qtext(value));
    }
    return result;
}

std::vector<GroupDefinition> discoverGroups(const FramePlanModel &model) {
    std::vector<GroupDefinition> result;

    std::vector<std::string> bloom_members;
    for (const auto &node : model.nodes) {
        const auto uses_bloom_resource = [](const std::string &resource) {
            return resource.starts_with("Bloom_");
        };
        if (std::any_of(node.reads.begin(), node.reads.end(),
                        uses_bloom_resource) ||
            std::any_of(node.history_reads.begin(), node.history_reads.end(),
                        uses_bloom_resource) ||
            std::any_of(node.writes.begin(), node.writes.end(),
                        uses_bloom_resource)) {
            bloom_members.push_back(node.name);
        }
    }
    if (bloom_members.size() >= 2) {
        std::ranges::sort(bloom_members);
        result.push_back(GroupDefinition{
            .key = "resource-family:Bloom",
            .label = QStringLiteral("Bloom structure"),
            .members = std::move(bloom_members),
        });
    }

    std::map<std::string, std::vector<std::string>, std::less<>> features;
    for (const auto &node : model.nodes) {
        if (!node.provider_feature.empty()) {
            features[node.provider_feature].push_back(node.name);
        }
    }
    for (auto &[feature, members] : features) {
        std::ranges::sort(members);
        result.push_back(GroupDefinition{
            .key = "feature:" + feature,
            .label = QStringLiteral("Feature: %1").arg(qtext(feature)),
            .members = std::move(members),
        });
    }
    return result;
}

QPainterPath entityPath(const VisibleEntity &entity) {
    QPainterPath path;
    if (entity.anchor) {
        const QPointF center{18.0, NodeHeight / 2.0};
        QPolygonF diamond;
        diamond << QPointF{center.x(), center.y() - 16.0}
                << QPointF{center.x() + 16.0, center.y()}
                << QPointF{center.x(), center.y() + 16.0}
                << QPointF{center.x() - 16.0, center.y()};
        path.addPolygon(diamond);
        path.closeSubpath();
    } else {
        path.addRoundedRect(
            QRectF{0.0, 0.0, entity.group ? GroupWidth : NodeWidth,
                   entity.group ? GroupHeight : NodeHeight},
            entity.group ? 13.0 : 9.0, entity.group ? 13.0 : 9.0);
    }
    return path;
}

void annotateIdentity(QGraphicsItem &item, const std::string &kind,
                      const std::string &graph, const std::string &name) {
    item.setData(FramePlanItemKindRole, qtext(kind));
    item.setData(FramePlanGraphRole, qtext(graph));
    item.setData(FramePlanNameRole, qtext(name));
}

void addEntityLabel(VisibleEntity &entity, const std::string &graph) {
    auto *label = new QGraphicsSimpleTextItem(entity.label, entity.item);
    label->setBrush(QColor{QStringLiteral("#f7f9fb")});
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    const QRectF bounds = label->boundingRect();
    if (entity.anchor) {
        label->setPos(43.0, (NodeHeight - bounds.height()) / 2.0);
    } else {
        const qreal width = entity.group ? GroupWidth : NodeWidth;
        const qreal height = entity.group ? GroupHeight : NodeHeight;
        label->setPos((width - bounds.width()) / 2.0,
                      (height - bounds.height()) / 2.0);
    }
    annotateIdentity(*label,
                     entity.group ? FramePlanGroupLabelItem
                                  : FramePlanNodeLabelItem,
                     graph, entity.key);
}

QString dependencyLabel(const std::vector<DependencyRecord> &records) {
    std::set<std::string, std::less<>> resources;
    std::set<std::string, std::less<>> reasons;
    for (const auto &record : records) {
        if (!record.resource.empty()) {
            resources.insert(record.resource);
        }
        reasons.insert(record.reason);
    }
    QStringList parts;
    for (const auto &resource : resources) {
        parts.push_back(qtext(resource));
    }
    if (parts.isEmpty()) {
        for (const auto &reason : reasons) {
            const auto marker = reason.rfind('.');
            const auto version = reason.rfind('@');
            parts.push_back(qtext(reason.substr(
                marker == std::string::npos ? 0 : marker + 1,
                version == std::string::npos
                    ? std::string::npos
                    : version - (marker == std::string::npos ? 0
                                                              : marker + 1))));
        }
    }
    QString label = parts.join(QStringLiteral(", "));
    if (records.size() > 1) {
        label += QStringLiteral("  x%1").arg(records.size());
    }
    return label;
}

void addArrow(QGraphicsScene &scene, const QPointF &tip,
              const EntityPair &endpoints, const std::string &graph) {
    QPolygonF arrow;
    arrow << tip << QPointF{tip.x() - 10.0, tip.y() - 5.0}
          << QPointF{tip.x() - 10.0, tip.y() + 5.0};
    auto *item = scene.addPolygon(
        arrow, QPen{QColor{QStringLiteral("#748394")}},
        QBrush{QColor{QStringLiteral("#748394")}});
    item->setZValue(0.5);
    annotateIdentity(*item, FramePlanEdgeArrowItem, graph,
                     endpoints.first + "->" + endpoints.second);
    item->setData(FramePlanFromNameRole, qtext(endpoints.first));
    item->setData(FramePlanToNameRole, qtext(endpoints.second));
}

} // namespace

FramePlanGraphicsScene::FramePlanGraphicsScene(QObject *parent)
    : QGraphicsScene{parent} {
    setItemIndexMethod(QGraphicsScene::NoIndex);
    connect(this, &QGraphicsScene::selectionChanged, this,
            [this] { recordSelection(); });
    resetGraph();
}

void FramePlanGraphicsScene::resetGraph() {
    rebuilding_ = true;
    clear();
    selected_node_.reset();
    collapsed_groups_.clear();
    setSceneRect({});
    rebuilding_ = false;
    setProperty("pelicanGraph", QString{});
    setProperty("pelicanNodeRecordCount", 0);
    setProperty("pelicanDependencyRecordCount", 0);
    setProperty("pelicanVisibleItemCount", 0);
    setProperty("pelicanEdgeBundleCount", 0);
    setProperty("pelicanResourceOverlayCount", 0);
    setProperty("pelicanCollapsedGroups", QStringList{});
    publishStateProperties();
}

void FramePlanGraphicsScene::populate(const FramePlanModel &model,
                                      int group_minimum) {
    const auto retained_selection = selected_node_;
    const bool selection_survives =
        retained_selection && retained_selection->graph == model.graph &&
        std::any_of(model.nodes.begin(), model.nodes.end(),
                    [&](const FramePlanNode &node) {
                        return node.name == retained_selection->name;
                    });

    rebuilding_ = true;
    clear();
    collapsed_groups_.clear();
    selected_node_ = selection_survives ? retained_selection : std::nullopt;

    std::map<std::string, const FramePlanNode *, std::less<>> nodes;
    for (const auto &node : model.nodes) {
        nodes.emplace(node.name, &node);
    }

    std::vector<GroupDefinition> collapsed;
    std::set<std::string, std::less<>> claimed_nodes;
    for (auto group : discoverGroups(model)) {
        if (static_cast<int>(group.members.size()) < group_minimum ||
            std::any_of(group.members.begin(), group.members.end(),
                        [&](const std::string &member) {
                            return claimed_nodes.contains(member);
                        })) {
            continue;
        }
        claimed_nodes.insert(group.members.begin(), group.members.end());
        collapsed_groups_.insert(model.graph + "\x1f" + group.key);
        collapsed.push_back(std::move(group));
    }

    std::map<std::string, std::string, std::less<>> entity_for_node;
    std::map<std::string, VisibleEntity, std::less<>> entities;
    for (const auto &group : collapsed) {
        VisibleEntity entity;
        entity.key = "group:" + group.key;
        entity.label = QStringLiteral("%1\n%2 nodes")
                           .arg(group.label)
                           .arg(group.members.size());
        entity.members = group.members;
        entity.group = true;
        entity.order = model.nodes.size();
        std::set<std::string, std::less<>> sources;
        for (const auto &member : group.members) {
            const auto *node = nodes.at(member);
            entity.order = std::min(entity.order, node->order);
            sources.insert(node->source);
            entity_for_node.emplace(member, entity.key);
        }
        entity.source = sources.size() == 1 ? *sources.begin() : "mixed";
        entities.emplace(entity.key, std::move(entity));
    }
    for (const auto &node : model.nodes) {
        if (entity_for_node.contains(node.name)) {
            continue;
        }
        entity_for_node.emplace(node.name, node.name);
        entities.emplace(
            node.name,
            VisibleEntity{
                .key = node.name,
                .label = qtext(node.name),
                .source = node.source,
                .members = {node.name},
                .order = node.order,
                .anchor = isAnchor(node),
            });
    }

    std::vector<DependencyRecord> sorted_dependencies;
    sorted_dependencies.reserve(model.dependencies.size());
    for (const auto &dependency : model.dependencies) {
        sorted_dependencies.push_back(DependencyRecord{
            dependency.from, dependency.to, dependency.reason,
            dependency.resource});
    }
    std::ranges::sort(sorted_dependencies, {}, [](const DependencyRecord &item) {
        return std::tie(item.from, item.to, item.reason, item.resource);
    });

    std::map<EntityPair, std::vector<DependencyRecord>> bundles;
    for (const auto &dependency : sorted_dependencies) {
        const std::string &from = entity_for_node.at(dependency.from);
        const std::string &to = entity_for_node.at(dependency.to);
        const std::string identity =
            recordIdentity(FramePlanDependency{dependency.from, dependency.to,
                                               dependency.reason,
                                               dependency.resource});
        if (from == to) {
            entities.at(from).internal_records.push_back(identity);
            continue;
        }
        bundles[{from, to}].push_back(dependency);
    }

    std::vector<VisibleEntity *> ordered_entities;
    ordered_entities.reserve(entities.size());
    for (auto &[key, entity] : entities) {
        ordered_entities.push_back(&entity);
    }
    std::ranges::sort(ordered_entities, {}, [](const VisibleEntity *entity) {
        return std::pair{entity->order, entity->key};
    });

    qreal cursor_x = 0.0;
    for (VisibleEntity *entity : ordered_entities) {
        const qreal width = entity->group ? GroupWidth : NodeWidth;
        const qreal height = entity->group ? GroupHeight : NodeHeight;
        entity->geometry = QRectF{cursor_x, 0.0, width, height};
        cursor_x += width + HorizontalGap;

        auto *item = addPath(entityPath(*entity));
        entity->item = item;
        item->setPos(entity->geometry.topLeft());
        item->setZValue(2.0);
        const QColor fill = entity->group
                                ? QColor{QStringLiteral("#7653a6")}
                                : sourceColor(entity->source);
        item->setBrush(fill);
        QPen outline{entity->anchor
                         ? QColor{QStringLiteral("#d8e3ec")}
                         : QColor{QStringLiteral("#edf2f6")}};
        outline.setWidthF(entity->group ? 2.0 : 1.3);
        if (entity->anchor) {
            outline.setStyle(Qt::DashLine);
        }
        item->setPen(outline);
        item->setFlag(QGraphicsItem::ItemIsSelectable, !entity->group);
        annotateIdentity(*item,
                         entity->group ? FramePlanGroupItem
                                       : FramePlanNodeItem,
                         model.graph, entity->key);
        item->setData(FramePlanSourceRole, qtext(entity->source));
        item->setData(FramePlanColorRole, fill.name(QColor::HexRgb));
        item->setData(FramePlanAnchorRole, entity->anchor);
        item->setData(FramePlanMembersRole, qlist(entity->members));
        item->setData(FramePlanInternalEdgeRecordsRole,
                      qlist(entity->internal_records));
        item->setToolTip(
            entity->anchor
                ? QStringLiteral("Anchor insertion point (no reads or writes)")
                : entity->group
                      ? QStringLiteral("Collapsed structure; lower the grouping minimum to change granularity")
                      : QStringLiteral("%1 / %2")
                            .arg(qtext(model.graph), qtext(entity->key)));
        addEntityLabel(*entity, model.graph);
    }

    qreal max_node_bottom = 0.0;
    for (const auto *entity : ordered_entities) {
        max_node_bottom =
            std::max(max_node_bottom, entity->geometry.bottom());
    }
    const qreal first_lane = max_node_bottom + 70.0;
    std::size_t lane = 0;
    std::size_t resource_overlay_count = 0;
    for (const auto &[endpoints, records] : bundles) {
        const auto &from = entities.at(endpoints.first).geometry;
        const auto &to = entities.at(endpoints.second).geometry;
        const qreal lane_y = first_lane + static_cast<qreal>(lane) * EdgeLaneGap;
        ++lane;

        const QPointF start{from.right(), from.center().y()};
        const QPointF tip{to.left(), to.center().y()};
        const qreal left_turn = start.x() + 18.0;
        const qreal right_turn = tip.x() - 18.0;
        QPainterPath path{start};
        path.lineTo(left_turn, start.y());
        path.lineTo(left_turn, lane_y);
        path.lineTo(right_turn, lane_y);
        path.lineTo(right_turn, tip.y());
        path.lineTo(tip);
        auto *edge = addPath(path);
        edge->setZValue(0.0);
        QPen edge_pen{QColor{QStringLiteral("#748394")}};
        edge_pen.setWidthF(1.35);
        edge->setPen(edge_pen);
        edge->setBrush(Qt::NoBrush);
        annotateIdentity(*edge, FramePlanEdgeItem, model.graph,
                         endpoints.first + "->" + endpoints.second);
        edge->setData(FramePlanFromNameRole, qtext(endpoints.first));
        edge->setData(FramePlanToNameRole, qtext(endpoints.second));

        std::vector<std::string> identities;
        std::set<std::string, std::less<>> resources;
        identities.reserve(records.size());
        for (const auto &record : records) {
            identities.push_back(recordIdentity(FramePlanDependency{
                record.from, record.to, record.reason, record.resource}));
            if (!record.resource.empty()) {
                resources.insert(record.resource);
            }
        }
        edge->setData(FramePlanEdgeRecordsRole, qlist(identities));
        edge->setData(
            FramePlanResourcesRole,
            qlist(std::vector<std::string>{resources.begin(), resources.end()}));
        if (!resources.empty()) {
            ++resource_overlay_count;
        }
        addArrow(*this, tip, endpoints, model.graph);

        const QString label_text = dependencyLabel(records);
        QFont label_font;
        label_font.setPointSizeF(8.5);
        const qreal label_width =
            std::max<qreal>(56.0, QFontMetricsF{label_font}.horizontalAdvance(
                                      label_text) +
                                      14.0);
        const qreal label_x =
            ((left_turn + right_turn) - label_width) / 2.0;
        const qreal label_y = lane_y - EdgeLabelHeight / 2.0;
        auto *label_box = addRect(
            QRectF{0.0, 0.0, label_width, EdgeLabelHeight},
            QPen{QColor{QStringLiteral("#9aa7b4")}},
            QBrush{QColor{QStringLiteral("#f5f7f9")}});
        label_box->setPos(label_x, label_y);
        label_box->setZValue(1.0);
        annotateIdentity(*label_box, FramePlanEdgeLabelItem, model.graph,
                         endpoints.first + "->" + endpoints.second);
        label_box->setData(FramePlanFromNameRole, qtext(endpoints.first));
        label_box->setData(FramePlanToNameRole, qtext(endpoints.second));
        label_box->setData(FramePlanEdgeRecordsRole, qlist(identities));
        label_box->setData(
            FramePlanResourcesRole,
            qlist(std::vector<std::string>{resources.begin(), resources.end()}));
        auto *label = new QGraphicsSimpleTextItem(label_text, label_box);
        label->setFont(label_font);
        label->setBrush(QColor{QStringLiteral("#263441")});
        const QRectF text_bounds = label->boundingRect();
        label->setPos((label_width - text_bounds.width()) / 2.0,
                      (EdgeLabelHeight - text_bounds.height()) / 2.0);
    }

    if (selected_node_) {
        const auto entity = entities.find(selected_node_->name);
        if (entity != entities.end() && !entity->second.group) {
            entity->second.item->setSelected(true);
        }
    }

    const QRectF bounds = itemsBoundingRect();
    setSceneRect(bounds.adjusted(-30.0, -30.0, 30.0, 30.0));
    rebuilding_ = false;

    setProperty("pelicanGraph", qtext(model.graph));
    setProperty("pelicanNodeRecordCount",
                static_cast<qulonglong>(model.nodes.size()));
    setProperty("pelicanDependencyRecordCount",
                static_cast<qulonglong>(model.dependencies.size()));
    setProperty("pelicanVisibleItemCount",
                static_cast<qulonglong>(entities.size()));
    setProperty("pelicanEdgeBundleCount",
                static_cast<qulonglong>(bundles.size()));
    setProperty("pelicanResourceOverlayCount",
                static_cast<qulonglong>(resource_overlay_count));
    QStringList collapsed_names;
    for (const auto &group : collapsed_groups_) {
        collapsed_names.push_back(qtext(group));
    }
    setProperty("pelicanCollapsedGroups", collapsed_names);
    setProperty("pelicanMinimumLabelSpacing", LabelGap);
    publishStateProperties();
}

void FramePlanGraphicsScene::recordSelection() {
    if (rebuilding_) {
        return;
    }
    selected_node_.reset();
    for (QGraphicsItem *item : selectedItems()) {
        if (itemKind(*item) != QLatin1String{FramePlanNodeItem}) {
            continue;
        }
        selected_node_ = FramePlanNodeKey{
            item->data(FramePlanGraphRole).toString().toStdString(),
            item->data(FramePlanNameRole).toString().toStdString(),
        };
        break;
    }
    publishStateProperties();
}

void FramePlanGraphicsScene::publishStateProperties() {
    setProperty("pelicanSelectedGraph",
                selected_node_ ? qtext(selected_node_->graph) : QString{});
    setProperty("pelicanSelectedNode",
                selected_node_ ? qtext(selected_node_->name) : QString{});
}

} // namespace PelicanStudio
