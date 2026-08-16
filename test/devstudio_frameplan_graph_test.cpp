#include "embeddedviewport.hpp"
#include "frameplangraphics.hpp"
#include "frameplanwidget.hpp"

#include "../src/core/loader/engineresources.hpp"
#include "../src/core/renderingpass/frameexecutionadapter.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/project/executionplan.hpp"
#include "../src/project/renderpipeline.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QByteArray>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
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

Pelican::RenderingTargetPlanDeviceFacts planningDeviceFacts() {
    return Pelican::RenderingTargetPlanDeviceFacts{
        .multiview = true,
        .max_multiview_view_count = 8,
        .query_attachment_samples =
            [](const Pelican::RenderTargetDefinition &) {
                return std::vector<std::uint32_t>{1};
            },
        .query_image_format_capability =
            [](const Pelican::RenderTargetDefinition &) {
                return Pelican::RenderingImageFormatCapability{
                    .image_usage_supported = true,
                    .supported_samples = {1},
                    .max_mip_levels = 16,
                    .max_array_layers = 8,
                    .transient_attachment_supported = true,
                    .local_read_attachment_supported = true,
                };
            },
        .transient_attachments = true,
        .dynamic_rendering_local_read = true,
    };
}

Json resolvedFramePlan(Pelican::ResolvedRenderPipeline resolved,
                       std::string_view label,
                       bool publish_physical_plan = true) {
    // Runtime resolution publishes the concrete display extent before frame
    // planning. Reproduce that CPU-only boundary without duplicating Studio's
    // separate extent arithmetic (WP315).
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
        throw std::runtime_error(std::string{label} +
                                 " must resolve to exactly one frame graph");
    }
    const Pelican::FramePlan plan = Pelican::planFrameGraph(graphs.front());
    Json wire = Pelican::framePlanToJson(plan, &compiled);
    const Pelican::FrameExecutionPlan execution =
        Pelican::compileFrameExecutionPlan(
            graphs.front(), plan,
            Pelican::ExecutionEndpoint{
                .id = "device:0",
                .endpoint_class = Pelican::ExecutionEndpointClass::device,
                .backend = "vulkan",
            });
    wire["execution_plan"] = Pelican::frameExecutionPlanToJson(execution);
    if (!publish_physical_plan) {
        return wire;
    }
    const auto targets = Pelican::parseRenderTargetDefinitionsFromJson(
        resolved.normalized_config);
    const auto physical = Pelican::compileRenderingTargetPlans(
        graphs, targets, compiled.sample_count_policy,
        vk::Format::eB8G8R8A8Srgb, planningDeviceFacts(), std::nullopt,
        std::nullopt, compiled.target_planning, compiled.vulkan_plan_pins,
        compiled.vulkan_physical_fragments);
    if (physical.plans.size() != 1) {
        throw std::runtime_error(std::string{label} +
                                 " must compile exactly one physical target "
                                 "plan");
    }
    wire["physical_target_plan"] =
        Pelican::vulkanTargetPlanToJson(*physical.plans.front());
    return wire;
}

Json animgraphFramePlan(
    bool shadow_directional_enabled,
    Pelican::RenderPipelineGraphVariant variant =
        Pelican::RenderPipelineGraphVariant::flat) {
    Json authored = readJson(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                             "projects" / "animgraph_demo" / "passes" /
                             "main.json");
#if PELICAN_RUNTIME_SHADER_COMPILER
    if (!shadow_directional_enabled) {
        Json retained = Json::array();
        for (const auto &feature : authored.at("features")) {
            if (feature != "engine://features/shadow_directional.json") {
                retained.push_back(feature);
            }
        }
        authored["features"] = std::move(retained);
    }
#else
    (void)shadow_directional_enabled;
    authored["features"] = Json::array();
#endif

    auto resolved = Pelican::resolveRenderPipeline(
        Pelican::RenderPipelineRequest{
            .authored_config = std::move(authored),
            .source_name = "projects/animgraph_demo/passes/main.json",
        },
        Pelican::RenderEnvironmentCapabilities{
            .runtime_shader_compiler_enabled =
                PELICAN_RUNTIME_SHADER_COMPILER != 0,
            .graph_variant = variant,
        },
        Pelican::RenderPipelineResolveDependencies{
            .load_feature_json = loadEngineDocument,
            .load_pipeline_json = loadEngineDocument,
        });
    return resolvedFramePlan(
        std::move(resolved), "animgraph_demo",
        variant != Pelican::RenderPipelineGraphVariant::preview);
}

Json exampleFramePlan(bool ui_enabled) {
    Json authored = readJson(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                             "projects" / "example" / "passes" /
                             "main_rendering_config.json");
#if PELICAN_RUNTIME_SHADER_COMPILER
    if (!ui_enabled) {
        authored["features"] = Json::array();
    }
#else
    (void)ui_enabled;
    authored["features"] = Json::array();
#endif
    auto resolved = Pelican::resolveRenderPipeline(
        Pelican::RenderPipelineRequest{
            .authored_config = std::move(authored),
            .source_name =
                "projects/example/passes/main_rendering_config.json",
        },
        Pelican::RenderEnvironmentCapabilities{
            .runtime_shader_compiler_enabled =
                PELICAN_RUNTIME_SHADER_COMPILER != 0,
            .graph_variant = Pelican::RenderPipelineGraphVariant::flat,
        },
        Pelican::RenderPipelineResolveDependencies{
            .load_feature_json = loadEngineDocument,
            .load_pipeline_json = loadEngineDocument,
        });
    return resolvedFramePlan(std::move(resolved), "example");
}

Json independentOpportunityFramePlan(
    Pelican::PlanningProfileKind profile_kind) {
    const Json config{
        {"render_targets",
         Json::array(
             {{{"name", "alpha_output"},
               {"extent_scale", 1.0},
               {"width", 32},
               {"height", 32},
               {"format", "R8G8B8A8_UNORM"},
               {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
              {{"name", "beta_output"},
               {"extent_scale", 1.0},
               {"width", 32},
               {"height", 32},
               {"format", "R8G8B8A8_UNORM"},
               {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}})},
        {"rendering_passes",
         Json::array(
             {{{"name", "independent_opportunities"},
               {"passes",
                Json::array(
                    {{{"name", "alpha"},
                      {"type", "fullscreen"},
                      {"output",
                       {{"color", "alpha_output"}, {"depth", nullptr}}}},
                     {{"name", "beta"},
                      {"type", "fullscreen"},
                      {"output",
                       {{"color", "beta_output"},
                        {"depth", nullptr}}}}})}}})},
    };
    const auto graphs =
        Pelican::parseFrameGraphDefinitionsFromConfigJson(config);
    const auto targets =
        Pelican::parseRenderTargetDefinitionsFromJson(config);
    const auto compilation = Pelican::compileRenderingTargetPlans(
        graphs, targets, Pelican::compileSampleCountPolicy(config),
        vk::Format::eB8G8R8A8Unorm,
        planningDeviceFacts(),
        std::nullopt, std::nullopt,
        Pelican::TargetPlanningPolicy{
            .profile = Pelican::PlanningProfile{.kind = profile_kind},
        });
    if (graphs.size() != 1 || compilation.plans.size() != 1) {
        throw std::runtime_error(
            "independent opportunity producer must compile one graph and one "
            "physical plan");
    }

    const Pelican::FramePlan plan = Pelican::planFrameGraph(graphs.front());
    Json wire = Pelican::framePlanToJson(plan);
    const Pelican::FrameExecutionPlan execution =
        Pelican::compileFrameExecutionPlan(
            graphs.front(), plan,
            Pelican::ExecutionEndpoint{
                .id = "device:0",
                .endpoint_class = Pelican::ExecutionEndpointClass::device,
                .backend = "vulkan",
            });
    wire["execution_plan"] = Pelican::frameExecutionPlanToJson(execution);
    wire["physical_target_plan"] =
        Pelican::vulkanTargetPlanToJson(*compilation.plans.front());
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

QGraphicsItem *physicalResourceItem(QGraphicsScene &value,
                                    std::string_view name) {
    const QString expected = QString::fromUtf8(
        name.data(), static_cast<qsizetype>(name.size()));
    for (QGraphicsItem *item :
         itemsOfKind(value, FramePlanResourceLifetimeItem)) {
        if (item->data(FramePlanNameRole).toString() == expected) {
            return item;
        }
    }
    return nullptr;
}

QGraphicsItem *physicalItemWithState(QGraphicsScene &value,
                                     const char *item_kind,
                                     std::string_view state) {
    const QString expected = QString::fromUtf8(
        state.data(), static_cast<qsizetype>(state.size()));
    for (QGraphicsItem *item : itemsOfKind(value, item_kind)) {
        if (item->data(FramePlanPhysicalStateRole).toString() == expected) {
            return item;
        }
    }
    return nullptr;
}

std::string selectedPlanningEndpoint(const Json &physical) {
    const auto &selection = physical.at("backend_selection");
    const std::string selected =
        selection.at("selected_candidate").get<std::string>();
    for (const auto &candidate : selection.at("candidates")) {
        if (candidate.at("candidate") == selected) {
            return candidate.at("endpoint").get<std::string>();
        }
    }
    throw std::runtime_error(
        "physical plan selected backend candidate has no endpoint");
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
    "WP307 production widget overlays every example physical resource and distinguishes reused from legal-not-adopted aliasing",
    "[devstudio][frame-plan][physical-overlay][wp307]") {
    (void)application();
    const std::string captured = readText(PELICAN_TEST_FRAME_PLAN_FIXTURE);
    const Json wire = Json::parse(captured);
    const Json &physical = wire.at("physical_target_plan");
    const Json &published_resources = physical.at("resources");
    const std::string endpoint = selectedPlanningEndpoint(physical);

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(captured));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);

    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QString::fromStdString(endpoint));
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            published_resources.size());
    REQUIRE(published_resources.size() == 22);

    const auto rows =
        itemsOfKind(logical, FramePlanResourceLifetimeItem);
    REQUIRE(rows.size() == published_resources.size());
    std::size_t arbitrary_materialized_count = 0;
    std::size_t same_pixel_count = 0;
    for (const auto &published : published_resources) {
        const std::string name =
            published.at("logical_resource").get<std::string>();
        QGraphicsItem *row = physicalResourceItem(logical, name);
        INFO("physical resource overlay: " << name);
        REQUIRE(row != nullptr);
        REQUIRE(row->data(FramePlanProfileRole).toString() ==
                QStringLiteral("optimized"));
        REQUIRE(row->data(FramePlanEndpointRole).toString() ==
                QString::fromStdString(endpoint));
        REQUIRE(row->data(FramePlanReasonRole).toString().toStdString() ==
                published.at("reason").get<std::string>());
        REQUIRE(row->data(FramePlanWidestReadRole).toString().toStdString() ==
                published.at("widest_read").get<std::string>());
        REQUIRE(row->data(FramePlanAliasableRole).toBool() ==
                published.at("aliasable").get<bool>());
        REQUIRE(
            row->data(FramePlanRepresentationRole).toString().toStdString() ==
            published.at("representation").get<std::string>());

        const Json &lifetime = published.at("lifetime");
        const bool used = lifetime.at("used").get<bool>();
        REQUIRE(row->data(FramePlanLifetimeUsedRole).toBool() == used);
        if (used) {
            REQUIRE(row->data(FramePlanLifetimeFirstRole).toULongLong() ==
                    lifetime.at("first_use").get<std::size_t>());
            REQUIRE(row->data(FramePlanLifetimeLastRole).toULongLong() ==
                    lifetime.at("last_use").get<std::size_t>());
        } else {
            REQUIRE_FALSE(row->data(FramePlanLifetimeFirstRole).isValid());
            REQUIRE_FALSE(row->data(FramePlanLifetimeLastRole).isValid());
        }

        arbitrary_materialized_count +=
            published.at("reason") ==
            "arbitrary read requires a materialized resource";
        same_pixel_count += published.at("widest_read") == "same_pixel";

        logical.clearSelection();
        row->setSelected(true);
        QApplication::processEvents();
        REQUIRE(logical.property("pelicanSelectedResource").toString() ==
                QString::fromStdString(name));
        REQUIRE(logical.property("pelicanSelectedResourceReason")
                    .toString()
                    .toStdString() ==
                published.at("reason").get<std::string>());
        REQUIRE(logical.property("pelicanSelectedResourceWidestRead")
                    .toString()
                    .toStdString() ==
                published.at("widest_read").get<std::string>());
        REQUIRE(logical.property("pelicanSelectedResourceAliasable")
                    .toBool() == published.at("aliasable").get<bool>());
        REQUIRE(logical.property("pelicanSelectedResourceRepresentation")
                    .toString()
                    .toStdString() ==
                published.at("representation").get<std::string>());
        const QString expected_lifetime =
            used ? QStringLiteral("[%1, %2]")
                       .arg(static_cast<qulonglong>(
                           lifetime.at("first_use").get<std::size_t>()))
                       .arg(static_cast<qulonglong>(
                           lifetime.at("last_use").get<std::size_t>()))
                 : QStringLiteral("unused");
        REQUIRE(logical.property("pelicanSelectedResourceLifetime")
                    .toString() == expected_lifetime);
        const auto selection_items =
            itemsOfKind(logical, FramePlanPhysicalSelectionItem);
        REQUIRE(selection_items.size() == 1);
        const auto *selection_text =
            dynamic_cast<const QGraphicsSimpleTextItem *>(
                selection_items.front());
        REQUIRE(selection_text != nullptr);
        const QString visible = selection_text->text();
        REQUIRE(visible.contains(QString::fromStdString(name)));
        REQUIRE(visible.contains(QString::fromStdString(
            published.at("reason").get<std::string>())));
        REQUIRE(visible.contains(QString::fromStdString(
            published.at("widest_read").get<std::string>())));
        REQUIRE(visible.contains(QString::fromStdString(
            published.at("representation").get<std::string>())));
        REQUIRE(visible.contains(
            published.at("aliasable").get<bool>()
                ? QStringLiteral("aliasable: yes")
                : QStringLiteral("aliasable: no")));
        REQUIRE(visible.contains(expected_lifetime));
    }
    REQUIRE(arbitrary_materialized_count == 16);
    REQUIRE(same_pixel_count == 1);

    REQUIRE(logical.property("pelicanAdoptedAliasCount").toULongLong() == 1);
    REQUIRE(logical.property("pelicanNotAdoptedAliasCount").toULongLong() ==
            1);
    const QGraphicsItem *adopted = physicalItemWithState(
        logical, FramePlanAliasOverlayItem, "adopted");
    const QGraphicsItem *not_adopted = physicalItemWithState(
        logical, FramePlanAliasOverlayItem, "not_adopted");
    REQUIRE(adopted != nullptr);
    REQUIRE(not_adopted != nullptr);
    REQUIRE(strings(adopted->data(FramePlanMembersRole)) ==
            StringSet{"Bloom_Threshold_RT", "g_emissive"});
    REQUIRE(strings(not_adopted->data(FramePlanMembersRole)) ==
            StringSet{"Bloom_Threshold_RT", "gbuffer_albedo"});
    REQUIRE_FALSE(not_adopted->data(FramePlanReasonRole).isValid());
    REQUIRE(not_adopted->toolTip().contains(
        QStringLiteral("No selection reason was published")));

    REQUIRE(logical.property("pelicanFusionCandidateCount").toULongLong() ==
            0);
    REQUIRE(logical.property("pelicanParallelCandidateCount").toULongLong() ==
            0);
    REQUIRE(logical.property("pelicanPhysicalExplicitEmptyCount")
                .toULongLong() == 2);
    const auto empty = itemsOfKind(logical, FramePlanPhysicalEmptyItem);
    REQUIRE(empty.size() == 2);
    REQUIRE(std::count_if(empty.begin(), empty.end(),
                          [](const QGraphicsItem *item) {
                              return item
                                         ->data(FramePlanPhysicalStateRole)
                                         .toString() ==
                                     QStringLiteral("reported_empty");
                          }) == 2);
}

TEST_CASE(
    "WP307 production planning compile contrasts nonzero optimized opportunities with reported-empty conservative profile in one widget",
    "[devstudio][frame-plan][physical-overlay][opportunities][wp307]") {
    (void)application();
    const Json optimized = independentOpportunityFramePlan(
        Pelican::PlanningProfileKind::optimized);
    const Json conservative = independentOpportunityFramePlan(
        Pelican::PlanningProfileKind::conservative_debug);
    const Json &optimized_opportunities =
        optimized.at("physical_target_plan").at("planning_opportunities");
    const Json &conservative_opportunities =
        conservative.at("physical_target_plan").at("planning_opportunities");
    REQUIRE(optimized_opportunities.at("profile") == "optimized");
    REQUIRE_FALSE(optimized_opportunities.at("fusion_candidates").empty());
    REQUIRE_FALSE(optimized_opportunities.at("parallel_candidates").empty());
    REQUIRE(conservative_opportunities.at("profile") ==
            "conservative_debug");
    REQUIRE(conservative_opportunities.at("fusion_candidates").empty());
    REQUIRE(conservative_opportunities.at("parallel_candidates").empty());

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(optimized.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);

    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
    REQUIRE(logical.property("pelicanFusionCandidateCount").toULongLong() ==
            optimized_opportunities.at("fusion_candidates").size());
    REQUIRE(logical.property("pelicanParallelCandidateCount").toULongLong() ==
            optimized_opportunities.at("parallel_candidates").size());
    const auto fusion = itemsOfKind(logical, FramePlanFusionOverlayItem);
    const auto parallel = itemsOfKind(logical, FramePlanParallelOverlayItem);
    REQUIRE_FALSE(fusion.empty());
    REQUIRE_FALSE(parallel.empty());
    REQUIRE(std::any_of(fusion.begin(), fusion.end(),
                        [](const QGraphicsItem *item) {
                            return strings(item->data(FramePlanMembersRole)) ==
                                   StringSet{"alpha", "beta"};
                        }));
    REQUIRE(std::any_of(parallel.begin(), parallel.end(),
                        [](const QGraphicsItem *item) {
                            return strings(item->data(FramePlanMembersRole)) ==
                                   StringSet{"alpha", "beta"};
                        }));
    REQUIRE(itemsOfKind(logical, FramePlanPhysicalEmptyItem).empty());

    // Same FramePlanWidget::receiveResult -> Impl::populate production path;
    // only the producer's planning profile changes between these compiles.
    widget.receiveResult(QByteArray::fromStdString(conservative.dump()));
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("conservative_debug"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
    REQUIRE(logical.property("pelicanFusionCandidateCount").toULongLong() ==
            0);
    REQUIRE(logical.property("pelicanParallelCandidateCount").toULongLong() ==
            0);
    REQUIRE(itemsOfKind(logical, FramePlanFusionOverlayItem).empty());
    REQUIRE(itemsOfKind(logical, FramePlanParallelOverlayItem).empty());
    REQUIRE(itemsOfKind(logical, FramePlanPhysicalEmptyItem).size() == 2);
}

TEST_CASE(
    "WP307 example flat feature toggle recompiles and replaces the physical overlay through one widget",
    "[devstudio][frame-plan][physical-overlay][feature-toggle][wp307]") {
    (void)application();
#if !PELICAN_RUNTIME_SHADER_COMPILER
    const Json base = exampleFramePlan(false);
    REQUIRE_FALSE(wireNodeNames(base).contains("pelican_ui"));
    EmbeddedViewport off_viewport;
    FramePlanWidget off_widget{&off_viewport};
    off_widget.receiveResult(QByteArray::fromStdString(base.dump()));
    QApplication::processEvents();
    QGraphicsScene &off_logical = scene(off_widget);
    REQUIRE(off_logical.property("pelicanPhysicalResourceCount")
                .toULongLong() ==
            base.at("physical_target_plan").at("resources").size());
    REQUIRE(off_logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(off_logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
    return;
#endif
    const Json enabled = exampleFramePlan(true);
    const Json disabled = exampleFramePlan(false);
    REQUIRE(wireNodeNames(enabled).contains("pelican_ui"));
    REQUIRE_FALSE(wireNodeNames(disabled).contains("pelican_ui"));
    REQUIRE(enabled.at("physical_target_plan")
                .at("planning_opportunities")
                .at("profile") == "optimized");
    REQUIRE(disabled.at("physical_target_plan")
                .at("planning_opportunities")
                .at("profile") == "optimized");
    REQUIRE(enabled.at("physical_target_plan").at("lowering_graph").at("nodes")
                .size() !=
            disabled.at("physical_target_plan")
                .at("lowering_graph")
                .at("nodes")
                .size());

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(enabled.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    QGraphicsItem *ui = nodeItem(logical, "pelican_ui");
    REQUIRE(ui != nullptr);
    ui->setSelected(true);
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanSelectedNode").toString() ==
            QStringLiteral("pelican_ui"));
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            enabled.at("physical_target_plan").at("resources").size());
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));

    widget.receiveResult(QByteArray::fromStdString(disabled.dump()));
    QApplication::processEvents();
    REQUIRE(nodeItem(logical, "pelican_ui") == nullptr);
    REQUIRE(logical.property("pelicanSelectedNode").toString().isEmpty());
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            disabled.at("physical_target_plan").at("resources").size());
    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
}

TEST_CASE(
    "WP307 preview and XR production variants publish scoped physical overlays or a named unavailable result",
    "[devstudio][frame-plan][physical-overlay][variant][wp307]") {
    (void)application();
    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};

    const Json preview = animgraphFramePlan(
        true, Pelican::RenderPipelineGraphVariant::preview);
    REQUIRE_FALSE(preview.contains("physical_target_plan"));
    widget.receiveResult(QByteArray::fromStdString(preview.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    REQUIRE(logical.property("pelicanGraph").toString() ==
            QString::fromStdString(preview.at("graph").get<std::string>()));
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() == 0);
    const auto preview_context =
        itemsOfKind(logical, FramePlanPhysicalContextItem);
    REQUIRE(preview_context.size() == 1);
    REQUIRE(preview_context.front()
                ->data(FramePlanPhysicalStateRole)
                .toString() == QStringLiteral("unavailable"));
    REQUIRE(preview_context.front()->toolTip().contains(
        QStringLiteral("physical_plan_missing")));

#if PELICAN_WITH_OPENXR
    const Json xr = animgraphFramePlan(
        true, Pelican::RenderPipelineGraphVariant::xr);
    widget.receiveResult(QByteArray::fromStdString(xr.dump()));
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanGraph").toString() ==
            QString::fromStdString(xr.at("graph").get<std::string>()));
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            xr.at("physical_target_plan").at("resources").size());
    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
#else
    try {
        (void)animgraphFramePlan(
            true, Pelican::RenderPipelineGraphVariant::xr);
        FAIL("XR production variant unexpectedly compiled without OpenXR");
    } catch (const std::runtime_error &error) {
        REQUIRE(std::string_view{error.what()}.find(
                    "XR graph variant is unavailable in this build") !=
                std::string_view::npos);
    }
#endif
}

TEST_CASE(
    "WP306 animgraph feature purge updates one widget without node edge overlay selection or fold orphans",
    "[devstudio][frame-plan][logical-graph][purge][wp306]") {
    (void)application();
#if !PELICAN_RUNTIME_SHADER_COMPILER
    const Json base = animgraphFramePlan(false);
    EmbeddedViewport off_viewport;
    FramePlanWidget off_widget{&off_viewport};
    off_widget.receiveResult(QByteArray::fromStdString(base.dump()));
    QApplication::processEvents();
    QGraphicsScene &off_logical = scene(off_widget);
    REQUIRE(sceneEdges(off_logical) == wireEdges(base));
    REQUIRE(off_logical.property("pelicanPhysicalResourceCount")
                .toULongLong() ==
            base.at("physical_target_plan").at("resources").size());
    REQUIRE(off_logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
    return;
#endif
    const Json enabled = animgraphFramePlan(true);
    const Json disabled = animgraphFramePlan(false);

    for (const auto &resource :
         enabled.at("physical_target_plan").at("resources")) {
        REQUIRE_FALSE(resource.at("logical_resource")
                          .get<std::string>()
                          .starts_with("Bloom_"));
    }

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
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            enabled.at("physical_target_plan").at("resources").size());
    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));

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
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            disabled.at("physical_target_plan").at("resources").size());

    minimum.setValue(64);
    QApplication::processEvents();
    REQUIRE(sceneEdges(logical) == wireEdges(disabled));
    REQUIRE(removedFrom(wireEdges(enabled), sceneEdges(logical)) ==
            expected_removed_edges);
    REQUIRE(nodeItem(logical, "shadow_depth") == nullptr);
    requireReadableLabels(logical);
}

} // namespace PelicanStudio
