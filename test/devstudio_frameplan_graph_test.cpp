#include "embeddedviewport.hpp"
#include "frameplangraphics.hpp"
#include "frameplanwidget.hpp"

#include "../src/core/loader/engineresources.hpp"
#include "../src/core/renderingpass/frameexecutionadapter.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/project/executionplan.hpp"
#include "../src/project/renderpipeline.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QByteArray>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QSpinBox>
#include <QStringList>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;
using StringSet = std::set<std::string, std::less<>>;
using EdgePair = std::pair<std::string, std::string>;
using EdgeSet = std::set<EdgePair>;

QApplication &application() {
    static int argument_count = 1;
    static char application_name[] = "pelican_frameplan_graph_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error("could not read " + path.string());
    }
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

Json readJson(const std::filesystem::path &path) {
    return Json::parse(readText(path));
}

std::string loadEngineDocument(std::string_view reference) {
    constexpr std::string_view prefix = "engine://";
    if (!reference.starts_with(prefix)) {
        throw std::runtime_error("expected an engine document reference: " +
                                 std::string{reference});
    }
    return Pelican::engineResourceOrThrow(reference.substr(prefix.size()));
}

Json animgraphFramePlan(bool shadow_directional_enabled) {
    Json authored = readJson(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                             "projects" / "animgraph_demo" / "passes" /
                             "main.json");
    if (!shadow_directional_enabled) {
        Json retained = Json::array();
        for (const auto &feature : authored.at("features")) {
            if (feature != "engine://features/shadow_directional.json") {
                retained.push_back(feature);
            }
        }
        authored["features"] = std::move(retained);
    }

    auto resolved = Pelican::resolveRenderPipeline(
        Pelican::RenderPipelineRequest{
            .authored_config = std::move(authored),
            .source_name = "projects/animgraph_demo/passes/main.json",
        },
        Pelican::RenderEnvironmentCapabilities{
            .runtime_shader_compiler_enabled = true,
            .graph_variant = Pelican::RenderPipelineGraphVariant::flat,
        },
        Pelican::RenderPipelineResolveDependencies{
            .load_feature_json = loadEngineDocument,
            .load_pipeline_json = loadEngineDocument,
        });
    // Runtime resolution publishes the concrete display extent before frame
    // planning.  Reproduce that CPU-only boundary so snapshot byte sizes are
    // derived by the same planner code without starting a GPU process.
    for (auto &target : resolved.normalized_config.at("render_targets")) {
        if (target.value("format_class", std::string{}) == "display") {
            target["width"] = 160;
            target["height"] = 90;
        }
    }
    const Pelican::CompiledRenderPipeline compiled =
        Pelican::compileRenderPipeline(resolved);
    const auto graphs = Pelican::parseFrameGraphDefinitionsFromConfigJson(
        resolved.normalized_config);
    if (graphs.size() != 1) {
        throw std::runtime_error(
            "animgraph_demo must resolve to exactly one frame graph");
    }
    const Pelican::FramePlan plan = Pelican::planFrameGraph(graphs.front());
    Json wire = Pelican::framePlanToJson(plan, &compiled);
    const Pelican::FrameExecutionPlan execution =
        Pelican::compileFrameExecutionPlan(
            graphs.front(), plan,
            Pelican::ExecutionEndpoint{
                .id = "device:wp306",
                .endpoint_class = Pelican::ExecutionEndpointClass::device,
                .backend = "vulkan",
            });
    wire["execution_plan"] = Pelican::frameExecutionPlanToJson(execution);
    return wire;
}

QGraphicsScene &scene(FramePlanWidget &widget) {
    auto *view = widget.findChild<QGraphicsView *>(
        QStringLiteral("pelican.framePlanLogicalView"));
    if (view == nullptr || view->scene() == nullptr) {
        throw std::runtime_error("logical frame-plan scene was not installed");
    }
    return *view->scene();
}

QSpinBox &groupMinimum(FramePlanWidget &widget) {
    auto *spin = widget.findChild<QSpinBox *>(
        QStringLiteral("pelican.framePlanGroupMinimum"));
    if (spin == nullptr) {
        throw std::runtime_error("frame-plan grouping control was not installed");
    }
    return *spin;
}

QString kind(const QGraphicsItem &item) {
    return item.data(FramePlanItemKindRole).toString();
}

std::vector<QGraphicsItem *> itemsOfKind(QGraphicsScene &value,
                                         const char *expected) {
    std::vector<QGraphicsItem *> result;
    for (QGraphicsItem *item : value.items()) {
        if (kind(*item) == QLatin1String{expected}) {
            result.push_back(item);
        }
    }
    return result;
}

QGraphicsItem *nodeItem(QGraphicsScene &value, std::string_view name) {
    const QString expected = QString::fromUtf8(name.data(),
                                               static_cast<qsizetype>(name.size()));
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanNodeItem)) {
        if (item->data(FramePlanNameRole).toString() == expected) {
            return item;
        }
    }
    return nullptr;
}

QGraphicsItem *edgeItem(QGraphicsScene &value, std::string_view from,
                        std::string_view to) {
    const QString expected_from = QString::fromUtf8(
        from.data(), static_cast<qsizetype>(from.size()));
    const QString expected_to =
        QString::fromUtf8(to.data(), static_cast<qsizetype>(to.size()));
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanEdgeItem)) {
        if (item->data(FramePlanFromNameRole).toString() == expected_from &&
            item->data(FramePlanToNameRole).toString() == expected_to) {
            return item;
        }
    }
    return nullptr;
}

StringSet strings(const QVariant &value) {
    StringSet result;
    for (const QString &entry : value.toStringList()) {
        result.insert(entry.toStdString());
    }
    return result;
}

StringSet sceneDependencyRecords(QGraphicsScene &value) {
    StringSet result;
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanEdgeItem)) {
        const StringSet records = strings(item->data(FramePlanEdgeRecordsRole));
        result.insert(records.begin(), records.end());
    }
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanGroupItem)) {
        const StringSet records =
            strings(item->data(FramePlanInternalEdgeRecordsRole));
        result.insert(records.begin(), records.end());
    }
    return result;
}

EdgeSet sceneEdges(QGraphicsScene &value) {
    EdgeSet result;
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanEdgeItem)) {
        result.emplace(item->data(FramePlanFromNameRole).toString().toStdString(),
                       item->data(FramePlanToNameRole).toString().toStdString());
    }
    return result;
}

StringSet wireNodeNames(const Json &wire) {
    StringSet result;
    for (const auto &node : wire.at("nodes")) {
        result.insert(node.at("name").get<std::string>());
    }
    return result;
}

EdgeSet wireEdges(const Json &wire) {
    EdgeSet result;
    for (const auto &dependency :
         wire.at("execution_plan").at("dependencies")) {
        result.emplace(dependency.at("from").get<std::string>(),
                       dependency.at("to").get<std::string>());
    }
    return result;
}

template <typename Set>
Set removedFrom(const Set &enabled, const Set &disabled) {
    Set result;
    std::set_difference(enabled.begin(), enabled.end(), disabled.begin(),
                        disabled.end(), std::inserter(result, result.end()));
    return result;
}

std::string edgeSetText(const EdgeSet &edges) {
    std::ostringstream output;
    for (const auto &[from, to] : edges) {
        output << from << " -> " << to << '\n';
    }
    return output.str();
}

struct ItemSnapshot {
    QRectF bounds;
    QPointF position;
    QString color;
    QStringList records;

    bool operator==(const ItemSnapshot &) const = default;
};

using SceneSnapshot = std::map<std::string, ItemSnapshot, std::less<>>;

SceneSnapshot snapshot(QGraphicsScene &value) {
    SceneSnapshot result;
    for (QGraphicsItem *item : value.items()) {
        const QString item_kind = kind(*item);
        if (item_kind != QLatin1String{FramePlanNodeItem} &&
            item_kind != QLatin1String{FramePlanGroupItem} &&
            item_kind != QLatin1String{FramePlanEdgeItem}) {
            continue;
        }
        std::string identity = item_kind.toStdString() + "|" +
                               item->data(FramePlanNameRole)
                                   .toString()
                                   .toStdString();
        result.emplace(
            std::move(identity),
            ItemSnapshot{
                .bounds = item->sceneBoundingRect(),
                .position = item->scenePos(),
                .color = item->data(FramePlanColorRole).toString(),
                .records =
                    item->data(FramePlanEdgeRecordsRole).toStringList(),
            });
    }
    return result;
}

void requireReadableLabels(QGraphicsScene &value) {
    std::vector<QRectF> labels;
    for (QGraphicsItem *item : value.items()) {
        const QString item_kind = kind(*item);
        if (item_kind == QLatin1String{FramePlanNodeLabelItem} ||
            item_kind == QLatin1String{FramePlanGroupLabelItem} ||
            item_kind == QLatin1String{FramePlanEdgeLabelItem}) {
            labels.push_back(item->sceneBoundingRect());
        }
    }
    REQUIRE_FALSE(labels.empty());
    const qreal minimum =
        value.property("pelicanMinimumLabelSpacing").toReal();
    REQUIRE(minimum >= 8.0);
    for (std::size_t left = 0; left < labels.size(); ++left) {
        for (std::size_t right = left + 1; right < labels.size(); ++right) {
            const QRectF padded = labels[left].adjusted(
                -minimum, -minimum, minimum, minimum);
            REQUIRE_FALSE(padded.intersects(labels[right]));
        }
    }
}

void requireNoReference(QGraphicsScene &value, std::string_view node,
                        std::string_view resource) {
    const QString node_text = QString::fromStdString(std::string{node});
    const QString resource_text = QString::fromStdString(std::string{resource});
    for (QGraphicsItem *item : value.items()) {
        REQUIRE(item->data(FramePlanNameRole).toString() != node_text);
        REQUIRE(item->data(FramePlanFromNameRole).toString() != node_text);
        REQUIRE(item->data(FramePlanToNameRole).toString() != node_text);
        REQUIRE_FALSE(item->data(FramePlanMembersRole)
                          .toStringList()
                          .contains(node_text));
        REQUIRE_FALSE(item->data(FramePlanResourcesRole)
                          .toStringList()
                          .contains(resource_text));
        for (const QString &record :
             item->data(FramePlanEdgeRecordsRole).toStringList()) {
            REQUIRE_FALSE(record.contains(node_text));
            REQUIRE_FALSE(record.contains(resource_text));
        }
        for (const QString &record :
             item->data(FramePlanInternalEdgeRecordsRole).toStringList()) {
            REQUIRE_FALSE(record.contains(node_text));
            REQUIRE_FALSE(record.contains(resource_text));
        }
    }
}

} // namespace

TEST_CASE(
    "WP306 logical graph exposes lost edges grouping identity and readable deterministic layout",
    "[devstudio][frame-plan][logical-graph][wp306]") {
    (void)application();
    const std::string captured = readText(PELICAN_TEST_FRAME_PLAN_FIXTURE);
    const Json captured_json = Json::parse(captured);

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(captured));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);

    REQUIRE(logical.property("pelicanNodeRecordCount").toULongLong() == 30);
    REQUIRE(logical.property("pelicanVisibleItemCount").toULongLong() == 30);
    const auto expanded_nodes = itemsOfKind(logical, FramePlanNodeItem);
    REQUIRE(expanded_nodes.size() == 30);
    REQUIRE(std::count_if(expanded_nodes.begin(), expanded_nodes.end(),
                          [](const QGraphicsItem *item) {
                              return item->data(FramePlanAnchorRole).toBool();
                          }) == 8);

    const std::vector<EdgePair> lost_edges{
        {"VerticalBlur_0", "UpsampleBlend_1"},
        {"VerticalBlur_1", "UpsampleBlend_2"},
        {"VerticalBlur_2", "UpsampleBlend_3"},
        {"gbuffer_pass", "lighting_pass"},
        {"forward_transparent", "FinalBloomComposite"},
    };
    for (const auto &[from, to] : lost_edges) {
        INFO("lost topological edge: " << from << " -> " << to);
        REQUIRE(edgeItem(logical, from, to) != nullptr);
    }

    const auto project = nodeItem(logical, "gbuffer_pass");
    const auto feature = nodeItem(logical, "pelican_ui");
    const auto engine = nodeItem(logical, "output_transform");
    REQUIRE(project != nullptr);
    REQUIRE(feature != nullptr);
    REQUIRE(engine != nullptr);
    REQUIRE(project->data(FramePlanGraphRole).toString() ==
            QStringLiteral("main_render"));
    REQUIRE(project->data(FramePlanSourceRole).toString() ==
            QStringLiteral("project"));
    REQUIRE(feature->data(FramePlanSourceRole).toString() ==
            QStringLiteral("feature:ui"));
    REQUIRE(engine->data(FramePlanSourceRole).toString() ==
            QStringLiteral("engine"));
    REQUIRE(project->data(FramePlanColorRole) !=
            feature->data(FramePlanColorRole));
    REQUIRE(feature->data(FramePlanColorRole) !=
            engine->data(FramePlanColorRole));
    REQUIRE(project->data(FramePlanColorRole) !=
            engine->data(FramePlanColorRole));
    requireReadableLabels(logical);

    const SceneSnapshot ordered = snapshot(logical);
    Json shuffled = captured_json;
    std::reverse(shuffled["nodes"].begin(), shuffled["nodes"].end());
    std::reverse(shuffled["barriers"].begin(), shuffled["barriers"].end());
    std::reverse(shuffled["resources"].begin(), shuffled["resources"].end());
    std::reverse(shuffled["execution_plan"]["nodes"].begin(),
                 shuffled["execution_plan"]["nodes"].end());
    std::reverse(shuffled["execution_plan"]["dependencies"].begin(),
                 shuffled["execution_plan"]["dependencies"].end());
    widget.receiveResult(QByteArray::fromStdString(shuffled.dump()));
    QApplication::processEvents();
    REQUIRE(snapshot(logical) == ordered);

    const StringSet expanded_records = sceneDependencyRecords(logical);
    REQUIRE(expanded_records.size() ==
            captured_json.at("execution_plan").at("dependencies").size());
    QSpinBox &minimum = groupMinimum(widget);
    minimum.setValue(13);
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanVisibleItemCount").toULongLong() == 18);
    REQUIRE(itemsOfKind(logical, FramePlanNodeItem).size() == 17);
    const auto groups = itemsOfKind(logical, FramePlanGroupItem);
    REQUIRE(groups.size() == 1);
    REQUIRE(groups.front()->data(FramePlanMembersRole).toStringList().size() ==
            13);
    REQUIRE(groups.front()
                ->data(FramePlanMembersRole)
                .toStringList()
                .contains(QStringLiteral("HighLuminanceExtraction")));
    REQUIRE(groups.front()
                ->data(FramePlanMembersRole)
                .toStringList()
                .contains(QStringLiteral("FinalBloomComposite")));
    REQUIRE(sceneDependencyRecords(logical) == expanded_records);
    requireReadableLabels(logical);

    minimum.setValue(14);
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanVisibleItemCount").toULongLong() == 30);
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(sceneDependencyRecords(logical) == expanded_records);
    REQUIRE(snapshot(logical) == ordered);
}

TEST_CASE(
    "WP306 animgraph feature purge updates one widget without node edge overlay selection or fold orphans",
    "[devstudio][frame-plan][logical-graph][purge][wp306]") {
    (void)application();
    const Json enabled = animgraphFramePlan(true);
    const Json disabled = animgraphFramePlan(false);

    const StringSet removed_nodes =
        removedFrom(wireNodeNames(enabled), wireNodeNames(disabled));
    const EdgeSet removed_edges =
        removedFrom(wireEdges(enabled), wireEdges(disabled));
    INFO("actual removed animgraph edges:\n" << edgeSetText(removed_edges));
    REQUIRE(removed_nodes == StringSet{"shadow_depth"});
    // This list is deliberately literal: it is the complete producer-side
    // contrast for disabling exactly shadow_directional in animgraph_demo.
    const EdgeSet expected_removed_edges{
        {"shadow_depth", "__anchor_sprite"},
        {"shadow_depth", "deferred_geometry"},
        {"shadow_depth", "deferred_lighting"},
        {"shadow_depth", "forward_opaque"},
        {"shadow_depth", "forward_transparent"},
    };
    REQUIRE(removed_edges == expected_removed_edges);

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    QSpinBox &minimum = groupMinimum(widget);
    minimum.setValue(64);
    widget.receiveResult(QByteArray::fromStdString(enabled.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    REQUIRE(sceneEdges(logical) == wireEdges(enabled));

    QGraphicsItem *shadow = nodeItem(logical, "shadow_depth");
    REQUIRE(shadow != nullptr);
    shadow->setSelected(true);
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanSelectedGraph").toString() ==
            QString::fromStdString(enabled.at("graph").get<std::string>()));
    REQUIRE(logical.property("pelicanSelectedNode").toString() ==
            QStringLiteral("shadow_depth"));

    minimum.setValue(1);
    QApplication::processEvents();
    const QStringList enabled_folds =
        logical.property("pelicanCollapsedGroups").toStringList();
    REQUIRE(std::any_of(enabled_folds.begin(), enabled_folds.end(),
                        [](const QString &group) {
                            return group.contains(
                                QStringLiteral("feature:shadow_directional"));
                        }));

    // Same widget, same receiveResult -> populate production path.
    widget.receiveResult(QByteArray::fromStdString(disabled.dump()));
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanSelectedGraph").toString().isEmpty());
    REQUIRE(logical.property("pelicanSelectedNode").toString().isEmpty());
    const QStringList disabled_folds =
        logical.property("pelicanCollapsedGroups").toStringList();
    REQUIRE(std::none_of(disabled_folds.begin(), disabled_folds.end(),
                         [](const QString &group) {
                             return group.contains(QStringLiteral(
                                 "feature:shadow_directional"));
                         }));
    requireNoReference(logical, "shadow_depth", "shadow_map");

    minimum.setValue(64);
    QApplication::processEvents();
    REQUIRE(sceneEdges(logical) == wireEdges(disabled));
    REQUIRE(removedFrom(wireEdges(enabled), sceneEdges(logical)) ==
            expected_removed_edges);
    REQUIRE(nodeItem(logical, "shadow_depth") == nullptr);
    requireReadableLabels(logical);
}

} // namespace PelicanStudio
