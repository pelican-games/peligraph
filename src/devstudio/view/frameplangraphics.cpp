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
constexpr qreal PhysicalPanelGap = 74.0;
constexpr qreal PhysicalHeaderHeight = 118.0;
constexpr qreal PhysicalResourceRowHeight = 36.0;
constexpr qreal PhysicalResourceLabelWidth = 390.0;
constexpr qreal PhysicalOpportunityLaneGap = 28.0;

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

struct PhysicalOverlaySummary {
    std::size_t resource_count = 0;
    std::size_t adopted_alias_count = 0;
    std::size_t not_adopted_alias_count = 0;
    std::size_t fusion_count = 0;
    std::size_t parallel_count = 0;
    std::size_t explicit_empty_count = 0;
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

QString lifetimeText(const FramePlanLifetime &lifetime) {
    if (!lifetime.used) {
        return QStringLiteral("unused");
    }
    return QStringLiteral("[%1, %2]")
        .arg(static_cast<qulonglong>(*lifetime.first_use))
        .arg(static_cast<qulonglong>(*lifetime.last_use));
}

void annotatePhysicalContext(QGraphicsItem &item,
                             const FramePlanModel &model) {
    item.setData(FramePlanProfileRole,
                 qtext(model.physical_plan.planning_profile));
    item.setData(FramePlanEndpointRole,
                 qtext(model.physical_plan.planning_endpoint));
}

void annotatePhysicalResource(QGraphicsItem &item,
                              const FramePlanModel &model,
                              const FramePlanResource &resource) {
    annotateIdentity(item, FramePlanResourceLifetimeItem, model.graph,
                     resource.name);
    annotatePhysicalContext(item, model);
    item.setData(FramePlanReasonRole, qtext(resource.reason));
    item.setData(FramePlanWidestReadRole, qtext(resource.widest_read));
    item.setData(FramePlanAliasableRole, resource.aliasable);
    item.setData(FramePlanRepresentationRole,
                 qtext(resource.representation));
    item.setData(FramePlanLifetimeUsedRole, resource.lifetime.used);
    if (resource.lifetime.first_use) {
        item.setData(FramePlanLifetimeFirstRole,
                     static_cast<qulonglong>(
                         *resource.lifetime.first_use));
    }
    if (resource.lifetime.last_use) {
        item.setData(FramePlanLifetimeLastRole,
                     static_cast<qulonglong>(
                         *resource.lifetime.last_use));
    }
    item.setData(
        FramePlanPhysicalStateRole,
        resource.alias_group.empty()
            ? QStringLiteral("separate")
            : QStringLiteral("reused:%1")
                  .arg(qtext(resource.alias_group)));
}

QString physicalResourceToolTip(const FramePlanResource &resource) {
    return QStringLiteral(
               "%1\nrepresentation: %2\nwidest read: %3\naliasable: "
               "%4\nlifetime: %5\nreason: %6")
        .arg(qtext(resource.name), qtext(resource.representation),
             qtext(resource.widest_read),
             resource.aliasable ? QStringLiteral("yes")
                                : QStringLiteral("no"),
             lifetimeText(resource.lifetime), qtext(resource.reason));
}

PhysicalOverlaySummary addPhysicalOverlay(
    QGraphicsScene &scene, const FramePlanModel &model,
    const std::map<std::string, VisibleEntity, std::less<>> &entities,
    const std::map<std::string, std::string, std::less<>> &entity_for_node,
    qreal logical_bottom,
    const std::optional<FramePlanNodeKey> &selected_resource) {
    PhysicalOverlaySummary summary;
    summary.fusion_count = model.physical_plan.fusion_candidates.size();
    summary.parallel_count = model.physical_plan.parallel_candidates.size();
    summary.adopted_alias_count = model.physical_plan.alias_groups.size();
    summary.not_adopted_alias_count =
        static_cast<std::size_t>(std::count_if(
            model.physical_plan.alias_candidates.begin(),
            model.physical_plan.alias_candidates.end(),
            [](const FramePlanOpportunityPair &candidate) {
                return !candidate.adopted;
            }));

    qreal logical_right = 640.0;
    for (const auto &[name, entity] : entities) {
        (void)name;
        logical_right = std::max(logical_right, entity.geometry.right());
    }
    const qreal panel_left = -PhysicalResourceLabelWidth;
    const qreal panel_width = logical_right - panel_left;
    const qreal header_y = logical_bottom + PhysicalPanelGap;

    auto *context = scene.addRect(
        QRectF{panel_left, header_y, panel_width, PhysicalHeaderHeight},
        QPen{QColor{QStringLiteral("#6e7b88")}, 1.4},
        QBrush{QColor{QStringLiteral("#182129")}});
    context->setZValue(1.0);
    annotateIdentity(*context, FramePlanPhysicalContextItem, model.graph,
                     "physical_planning_context");
    annotatePhysicalContext(*context, model);

    const QString profile = model.physical_plan.planning_profile.empty()
                                ? QStringLiteral("not published")
                                : qtext(model.physical_plan.planning_profile);
    const QString endpoint = model.physical_plan.planning_endpoint.empty()
                                 ? QStringLiteral("not published")
                                 : qtext(model.physical_plan.planning_endpoint);
    auto *title = new QGraphicsSimpleTextItem(context);
    QFont title_font = title->font();
    title_font.setBold(true);
    title_font.setPointSizeF(10.5);
    title->setFont(title_font);
    title->setBrush(QColor{QStringLiteral("#f1f5f8")});
    title->setPos(panel_left + 12.0, header_y + 8.0);

    if (!model.physical_plan.available()) {
        context->setData(FramePlanPhysicalStateRole,
                         QStringLiteral("unavailable"));
        title->setText(QStringLiteral("Physical planning unavailable"));
        auto *reason = new QGraphicsSimpleTextItem(
            qtext(model.physical_plan.unavailable_reason), context);
        reason->setBrush(QColor{QStringLiteral("#efb366")});
        reason->setPos(panel_left + 12.0, header_y + 39.0);
        context->setToolTip(qtext(model.physical_plan.unavailable_reason));
        return summary;
    }

    context->setData(FramePlanPhysicalStateRole,
                     QStringLiteral("available"));
    title->setText(
        QStringLiteral("Physical planning  |  profile: %1  |  endpoint: %2")
            .arg(profile, endpoint));
    auto *counts = new QGraphicsSimpleTextItem(
        QStringLiteral(
            "Reused alias groups: %1  |  legal alias candidates not adopted: "
            "%2  |  fusion candidates: %3  |  parallel candidates: %4")
            .arg(static_cast<qulonglong>(summary.adopted_alias_count))
            .arg(static_cast<qulonglong>(summary.not_adopted_alias_count))
            .arg(static_cast<qulonglong>(summary.fusion_count))
            .arg(static_cast<qulonglong>(summary.parallel_count)),
        context);
    counts->setBrush(QColor{QStringLiteral("#b9c6d1")});
    counts->setPos(panel_left + 12.0, header_y + 42.0);
    auto *selection = new QGraphicsSimpleTextItem(
        QStringLiteral("Select a physical resource to inspect its planning facts."),
        context);
    selection->setBrush(QColor{QStringLiteral("#d7e2ea")});
    selection->setPos(panel_left + 12.0, header_y + 69.0);
    annotateIdentity(*selection, FramePlanPhysicalSelectionItem, model.graph,
                     "selected_physical_resource");
    annotatePhysicalContext(*selection, model);
    context->setToolTip(
        QStringLiteral("These physical decisions were produced for profile "
                       "'%1' on endpoint '%2'.")
            .arg(profile, endpoint));

    const auto add_empty_marker = [&](std::string_view opportunity,
                                      qreal right_offset) {
        const qreal marker_x = logical_right - right_offset;
        const qreal marker_y = header_y + PhysicalHeaderHeight + 8.0;
        auto *marker = scene.addRect(
            QRectF{marker_x, marker_y, 172.0, 26.0},
            QPen{QColor{QStringLiteral("#647482")}, 1.0, Qt::DashLine},
            QBrush{QColor{QStringLiteral("#182129")}});
        marker->setZValue(1.2);
        annotateIdentity(*marker, FramePlanPhysicalEmptyItem, model.graph,
                         std::string{opportunity} + ":empty");
        annotatePhysicalContext(*marker, model);
        marker->setData(FramePlanOpportunityKindRole,
                        qtext(std::string{opportunity}));
        marker->setData(FramePlanPhysicalStateRole,
                        QStringLiteral("reported_empty"));
        marker->setToolTip(
            QStringLiteral("%1 candidates: 0 (reported by the engine)")
                .arg(qtext(std::string{opportunity})));
        auto *label = new QGraphicsSimpleTextItem(
            QStringLiteral("%1: 0 (reported)")
                .arg(qtext(std::string{opportunity})),
            marker);
        label->setBrush(QColor{QStringLiteral("#b9c6d1")});
        label->setPos(marker_x + 8.0, marker_y + 4.0);
        ++summary.explicit_empty_count;
    };
    if (summary.fusion_count == 0) {
        add_empty_marker("fusion", 364.0);
    }
    if (summary.parallel_count == 0) {
        add_empty_marker("parallel", 182.0);
    }

    std::size_t opportunity_lane = 0;
    const auto add_node_opportunities =
        [&](const std::vector<FramePlanOpportunityPair> &opportunities,
            const char *item_kind, std::string_view opportunity_kind,
            const QColor &color, Qt::PenStyle style) {
            for (const auto &candidate : opportunities) {
                const auto first_mapping =
                    entity_for_node.find(candidate.first);
                const auto second_mapping =
                    entity_for_node.find(candidate.second);
                if (first_mapping == entity_for_node.end() ||
                    second_mapping == entity_for_node.end()) {
                    continue;
                }
                const auto &first = entities.at(first_mapping->second);
                const auto &second = entities.at(second_mapping->second);
                const qreal lane_y =
                    -48.0 - static_cast<qreal>(opportunity_lane++) *
                                PhysicalOpportunityLaneGap;
                QPainterPath path;
                if (first.key == second.key) {
                    path.addEllipse(
                        QRectF{first.geometry.center().x() - 20.0, lane_y,
                               40.0, 20.0});
                } else {
                    const QPointF start{first.geometry.center().x(),
                                        first.geometry.top()};
                    const QPointF end{second.geometry.center().x(),
                                      second.geometry.top()};
                    path.moveTo(start);
                    path.lineTo(start.x(), lane_y);
                    path.lineTo(end.x(), lane_y);
                    path.lineTo(end);
                }
                auto *item = scene.addPath(path);
                QPen pen{color};
                pen.setWidthF(2.2);
                pen.setStyle(style);
                item->setPen(pen);
                item->setBrush(Qt::NoBrush);
                item->setZValue(1.1);
                annotateIdentity(
                    *item, item_kind, model.graph,
                    candidate.first + "+" + candidate.second);
                annotatePhysicalContext(*item, model);
                item->setData(FramePlanMembersRole,
                              qlist({candidate.first, candidate.second}));
                item->setData(FramePlanOpportunityKindRole,
                              qtext(std::string{opportunity_kind}));
                item->setData(
                    FramePlanPhysicalStateRole,
                    opportunity_kind == "fusion" && candidate.adopted
                        ? QStringLiteral("adopted")
                        : QStringLiteral("candidate"));
                item->setToolTip(
                    QStringLiteral("%1 opportunity: %2 + %3")
                        .arg(qtext(std::string{opportunity_kind}),
                             qtext(candidate.first), qtext(candidate.second)));
            }
        };
    add_node_opportunities(model.physical_plan.fusion_candidates,
                           FramePlanFusionOverlayItem, "fusion",
                           QColor{QStringLiteral("#c087ff")}, Qt::DashLine);
    add_node_opportunities(model.physical_plan.parallel_candidates,
                           FramePlanParallelOverlayItem, "parallel",
                           QColor{QStringLiteral("#55cbd3")}, Qt::DotLine);

    std::set<std::string, std::less<>> not_adopted_members;
    for (const auto &candidate : model.physical_plan.alias_candidates) {
        if (!candidate.adopted) {
            not_adopted_members.insert(candidate.first);
            not_adopted_members.insert(candidate.second);
        }
    }

    std::vector<const FramePlanResource *> physical_resources;
    for (const auto &resource : model.resources) {
        if (!resource.representation.empty()) {
            physical_resources.push_back(&resource);
        }
    }
    std::ranges::sort(physical_resources, {},
                      [](const FramePlanResource *resource) {
                          return resource->name;
                      });
    summary.resource_count = physical_resources.size();

    const qreal resources_y =
        header_y + PhysicalHeaderHeight +
        (summary.explicit_empty_count == 0 ? 18.0 : 48.0);
    std::map<std::string, qreal, std::less<>> row_centers;
    const auto lifetime_x = [&](std::size_t use_index, bool end) {
        if (use_index < model.physical_plan.lowering_nodes.size()) {
            const auto mapping = entity_for_node.find(
                model.physical_plan.lowering_nodes[use_index].name);
            if (mapping != entity_for_node.end()) {
                const auto &geometry = entities.at(mapping->second).geometry;
                return end ? geometry.right() : geometry.left();
            }
        }
        const qreal fraction =
            model.physical_plan.lowering_nodes.empty()
                ? 0.0
                : static_cast<qreal>(use_index) /
                      static_cast<qreal>(
                          model.physical_plan.lowering_nodes.size());
        return fraction * logical_right;
    };

    for (std::size_t index = 0; index < physical_resources.size(); ++index) {
        const FramePlanResource &resource = *physical_resources[index];
        const qreal row_y =
            resources_y + static_cast<qreal>(index) *
                              PhysicalResourceRowHeight;
        const QColor row_color =
            index % 2 == 0 ? QColor{QStringLiteral("#232c34")}
                           : QColor{QStringLiteral("#1e272f")};
        QColor outline{QStringLiteral("#52616e")};
        if (!resource.alias_group.empty()) {
            outline = QColor{QStringLiteral("#4fbc78")};
        } else if (not_adopted_members.contains(resource.name)) {
            outline = QColor{QStringLiteral("#e3a84c")};
        }
        auto *row = scene.addRect(
            QRectF{panel_left, row_y, panel_width,
                   PhysicalResourceRowHeight - 2.0},
            QPen{outline, resource.alias_group.empty() ? 0.8 : 1.8},
            QBrush{row_color});
        row->setZValue(0.8);
        row->setFlag(QGraphicsItem::ItemIsSelectable, true);
        annotatePhysicalResource(*row, model, resource);
        row->setToolTip(physicalResourceToolTip(resource));

        auto *name = new QGraphicsSimpleTextItem(qtext(resource.name), row);
        QFont name_font = name->font();
        name_font.setBold(true);
        name->setFont(name_font);
        name->setBrush(QColor{QStringLiteral("#edf3f7")});
        name->setPos(panel_left + 9.0, row_y + 7.0);

        auto *details = new QGraphicsSimpleTextItem(
            QStringLiteral("%1 | %2 | aliasable %3 | lifetime %4")
                .arg(qtext(resource.representation),
                     qtext(resource.widest_read),
                     resource.aliasable ? QStringLiteral("yes")
                                        : QStringLiteral("no"),
                     lifetimeText(resource.lifetime)),
            row);
        QFont detail_font = details->font();
        detail_font.setPointSizeF(8.0);
        details->setFont(detail_font);
        details->setBrush(QColor{QStringLiteral("#aebbc6")});
        details->setPos(panel_left + 174.0, row_y + 8.0);

        if (resource.lifetime.used) {
            const qreal left =
                lifetime_x(*resource.lifetime.first_use, false);
            const qreal right =
                std::max(left + 8.0,
                         lifetime_x(*resource.lifetime.last_use, true));
            const QColor bar_color =
                !resource.alias_group.empty()
                    ? QColor{QStringLiteral("#4fbc78")}
                    : not_adopted_members.contains(resource.name)
                          ? QColor{QStringLiteral("#e3a84c")}
                          : QColor{QStringLiteral("#71889a")};
            auto *bar = new QGraphicsRectItem(
                QRectF{left, row_y + PhysicalResourceRowHeight - 9.0,
                       right - left, 4.0},
                row);
            bar->setPen(Qt::NoPen);
            bar->setBrush(bar_color);
        }

        row_centers.emplace(
            resource.name,
            row_y + (PhysicalResourceRowHeight - 2.0) / 2.0);
        if (selected_resource &&
            selected_resource->name == resource.name) {
            row->setSelected(true);
        }
    }

    const auto add_alias_connector =
        [&](const std::vector<std::string> &members, const std::string &name,
            QString state, const QColor &color, Qt::PenStyle style,
            qreal x_offset, QString tool_tip) {
            std::vector<qreal> rows;
            for (const auto &member : members) {
                if (const auto row = row_centers.find(member);
                    row != row_centers.end()) {
                    rows.push_back(row->second);
                }
            }
            if (rows.size() < 2) {
                return;
            }
            std::ranges::sort(rows);
            const qreal connector_x = -x_offset;
            QPainterPath path;
            path.moveTo(connector_x, rows.front());
            path.lineTo(connector_x, rows.back());
            for (const qreal row : rows) {
                path.moveTo(connector_x, row);
                path.lineTo(connector_x + 16.0, row);
            }
            auto *connector = scene.addPath(path);
            QPen pen{color};
            pen.setWidthF(3.0);
            pen.setStyle(style);
            connector->setPen(pen);
            connector->setBrush(Qt::NoBrush);
            connector->setZValue(1.5);
            annotateIdentity(*connector, FramePlanAliasOverlayItem,
                             model.graph, name);
            annotatePhysicalContext(*connector, model);
            connector->setData(FramePlanMembersRole, qlist(members));
            connector->setData(FramePlanOpportunityKindRole,
                               QStringLiteral("alias"));
            connector->setData(FramePlanPhysicalStateRole,
                               std::move(state));
            connector->setToolTip(std::move(tool_tip));
        };
    for (const auto &group : model.physical_plan.alias_groups) {
        add_alias_connector(
            group.resources, group.id, QStringLiteral("adopted"),
            QColor{QStringLiteral("#4fbc78")}, Qt::SolidLine, 18.0,
            QStringLiteral("Reused allocation %1: %2")
                .arg(qtext(group.id),
                     qlist(group.resources).join(QStringLiteral(" + "))));
    }
    for (const auto &candidate : model.physical_plan.alias_candidates) {
        if (candidate.adopted) {
            continue;
        }
        const std::vector<std::string> members{candidate.first,
                                                candidate.second};
        add_alias_connector(
            members, "not_adopted:" + candidate.first + "+" + candidate.second,
            QStringLiteral("not_adopted"),
            QColor{QStringLiteral("#e3a84c")}, Qt::DashLine, 46.0,
            QStringLiteral(
                "Legal alias candidate, not adopted: %1 + %2\nNo selection "
                "reason was published by the engine.")
                .arg(qtext(candidate.first), qtext(candidate.second)));
    }

    return summary;
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
    selected_resource_.reset();
    collapsed_groups_.clear();
    setSceneRect({});
    rebuilding_ = false;
    setProperty("pelicanGraph", QString{});
    setProperty("pelicanNodeRecordCount", 0);
    setProperty("pelicanDependencyRecordCount", 0);
    setProperty("pelicanVisibleItemCount", 0);
    setProperty("pelicanEdgeBundleCount", 0);
    setProperty("pelicanResourceOverlayCount", 0);
    setProperty("pelicanPhysicalResourceCount", 0);
    setProperty("pelicanAdoptedAliasCount", 0);
    setProperty("pelicanNotAdoptedAliasCount", 0);
    setProperty("pelicanFusionCandidateCount", 0);
    setProperty("pelicanParallelCandidateCount", 0);
    setProperty("pelicanPhysicalExplicitEmptyCount", 0);
    setProperty("pelicanPlanningProfile", QString{});
    setProperty("pelicanPlanningEndpoint", QString{});
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
    const auto retained_resource = selected_resource_;
    const bool resource_selection_survives =
        retained_resource && retained_resource->graph == model.graph &&
        std::any_of(model.resources.begin(), model.resources.end(),
                    [&](const FramePlanResource &resource) {
                        return resource.name == retained_resource->name &&
                               !resource.representation.empty();
                    });

    rebuilding_ = true;
    clear();
    collapsed_groups_.clear();
    selected_node_ = selection_survives ? retained_selection : std::nullopt;
    selected_resource_ = resource_selection_survives
                             ? retained_resource
                             : std::nullopt;

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

    const qreal logical_bottom =
        lane == 0
            ? max_node_bottom
            : first_lane + static_cast<qreal>(lane - 1) * EdgeLaneGap +
                  EdgeLabelHeight;
    const PhysicalOverlaySummary physical_overlay = addPhysicalOverlay(
        *this, model, entities, entity_for_node, logical_bottom,
        selected_resource_);

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
    setProperty("pelicanPhysicalResourceCount",
                static_cast<qulonglong>(physical_overlay.resource_count));
    setProperty("pelicanAdoptedAliasCount",
                static_cast<qulonglong>(
                    physical_overlay.adopted_alias_count));
    setProperty("pelicanNotAdoptedAliasCount",
                static_cast<qulonglong>(
                    physical_overlay.not_adopted_alias_count));
    setProperty("pelicanFusionCandidateCount",
                static_cast<qulonglong>(physical_overlay.fusion_count));
    setProperty("pelicanParallelCandidateCount",
                static_cast<qulonglong>(physical_overlay.parallel_count));
    setProperty("pelicanPhysicalExplicitEmptyCount",
                static_cast<qulonglong>(
                    physical_overlay.explicit_empty_count));
    setProperty("pelicanPlanningProfile",
                qtext(model.physical_plan.planning_profile));
    setProperty("pelicanPlanningEndpoint",
                qtext(model.physical_plan.planning_endpoint));
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
    selected_resource_.reset();
    for (QGraphicsItem *item : selectedItems()) {
        if (itemKind(*item) !=
            QLatin1String{FramePlanResourceLifetimeItem}) {
            continue;
        }
        selected_resource_ = FramePlanNodeKey{
            item->data(FramePlanGraphRole).toString().toStdString(),
            item->data(FramePlanNameRole).toString().toStdString(),
        };
        publishStateProperties();
        return;
    }
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
    setProperty("pelicanSelectedResource",
                selected_resource_ ? qtext(selected_resource_->name)
                                   : QString{});

    QGraphicsItem *resource_item = nullptr;
    if (selected_resource_) {
        for (QGraphicsItem *item : items()) {
            if (itemKind(*item) ==
                    QLatin1String{FramePlanResourceLifetimeItem} &&
                item->data(FramePlanNameRole).toString() ==
                    qtext(selected_resource_->name)) {
                resource_item = item;
                break;
            }
        }
    }
    setProperty("pelicanSelectedResourceReason",
                resource_item
                    ? resource_item->data(FramePlanReasonRole).toString()
                    : QString{});
    setProperty("pelicanSelectedResourceWidestRead",
                resource_item
                    ? resource_item->data(FramePlanWidestReadRole).toString()
                    : QString{});
    setProperty("pelicanSelectedResourceAliasable",
                resource_item
                    ? resource_item->data(FramePlanAliasableRole)
                    : QVariant{});
    setProperty("pelicanSelectedResourceRepresentation",
                resource_item
                    ? resource_item->data(FramePlanRepresentationRole)
                          .toString()
                    : QString{});
    QString lifetime;
    if (resource_item) {
        lifetime = resource_item->data(FramePlanLifetimeUsedRole).toBool()
                       ? QStringLiteral("[%1, %2]")
                             .arg(resource_item
                                      ->data(FramePlanLifetimeFirstRole)
                                      .toULongLong())
                             .arg(resource_item
                                      ->data(FramePlanLifetimeLastRole)
                                      .toULongLong())
                       : QStringLiteral("unused");
    }
    setProperty("pelicanSelectedResourceLifetime", lifetime);

    QString selection_summary =
        QStringLiteral("Select a physical resource to inspect its planning facts.");
    if (resource_item) {
        selection_summary =
            QStringLiteral(
                "Selected %1 | representation: %2 | widest read: %3 | "
                "aliasable: %4 | lifetime: %5\nreason: %6")
                .arg(resource_item->data(FramePlanNameRole).toString(),
                     resource_item->data(FramePlanRepresentationRole)
                         .toString(),
                     resource_item->data(FramePlanWidestReadRole).toString(),
                     resource_item->data(FramePlanAliasableRole).toBool()
                         ? QStringLiteral("yes")
                         : QStringLiteral("no"),
                     lifetime,
                     resource_item->data(FramePlanReasonRole).toString());
    }
    for (QGraphicsItem *item : items()) {
        if (itemKind(*item) !=
            QLatin1String{FramePlanPhysicalSelectionItem}) {
            continue;
        }
        if (auto *label = dynamic_cast<QGraphicsSimpleTextItem *>(item)) {
            label->setText(selection_summary);
            setSceneRect(itemsBoundingRect().adjusted(-30.0, -30.0, 30.0,
                                                      30.0));
        }
        break;
    }
}

} // namespace PelicanStudio
