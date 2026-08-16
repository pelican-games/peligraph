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
#include <catch2/generators/catch_generators.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QBrush>
#include <QByteArray>
#include <QColor>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QPainterPath>
#include <QPen>
#include <QSpinBox>
#include <QStringList>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <ranges>
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

struct ExpectedNode {
    std::string name;
    std::string source;
    bool anchor = false;
};

struct ExpectedDependency {
    std::string from;
    std::string to;
    std::string reason;
    std::string resource;
};

const std::vector<ExpectedNode> &expectedExpandedNodes() {
    static const std::vector<ExpectedNode> expected{
        {"gbuffer_pass", "project"},
        {"ssao_pass", "project"},
        {"ssao_blur_pass", "project"},
        {"lighting_pass", "project"},
        {"__snapshot_opaque_color", "project"},
        {"__snapshot_opaque_depth", "project"},
        {"forward_transparent", "project"},
        {"__anchor_sprite", "engine", true},
        {"__anchor_post_main", "engine", true},
        {"__anchor_tonemap", "engine", true},
        {"__anchor_post_ldr", "engine", true},
        {"HighLuminanceExtraction", "project"},
        {"HorizontalBlur_0", "project"},
        {"VerticalBlur_0", "project"},
        {"HorizontalBlur_1", "project"},
        {"VerticalBlur_1", "project"},
        {"HorizontalBlur_2", "project"},
        {"VerticalBlur_2", "project"},
        {"HorizontalBlur_3", "project"},
        {"VerticalBlur_3", "project"},
        {"UpsampleBlend_3", "project"},
        {"UpsampleBlend_2", "project"},
        {"UpsampleBlend_1", "project"},
        {"FinalBloomComposite", "project"},
        {"__anchor_pelican_ui", "engine", true},
        {"pelican_ui", "feature:ui"},
        {"__anchor_debug_draw", "engine", true},
        {"__anchor_debug_text", "engine", true},
        {"__anchor_imgui", "engine", true},
        {"output_transform", "engine"},
    };
    return expected;
}

const StringSet &expectedBloomMembers() {
    static const StringSet expected{
        "FinalBloomComposite",      "HighLuminanceExtraction",
        "HorizontalBlur_0",         "HorizontalBlur_1",
        "HorizontalBlur_2",         "HorizontalBlur_3",
        "UpsampleBlend_1",          "UpsampleBlend_2",
        "UpsampleBlend_3",          "VerticalBlur_0",
        "VerticalBlur_1",           "VerticalBlur_2",
        "VerticalBlur_3",
    };
    return expected;
}

const std::vector<ExpectedDependency> &expectedExpandedDependencies() {
    static const auto expected = [] {
        constexpr std::string_view explicit_after =
            "pelican.dependency.explicit_after@1";
        constexpr std::string_view read_after_write =
            "pelican.dependency.read_after_write@1";
        constexpr std::string_view snapshot_after =
            "pelican.dependency.snapshot_after@1";
        std::vector<ExpectedDependency> result;
        result.reserve(100);
        const auto add = [&](std::string_view from, std::string_view to,
                             std::string_view reason,
                             std::string_view resource = {}) {
            result.push_back(ExpectedDependency{
                std::string{from}, std::string{to}, std::string{reason},
                std::string{resource}});
        };

        add("__anchor_debug_draw", "__anchor_debug_text", explicit_after);
        add("__anchor_debug_text", "__anchor_imgui", explicit_after);
        add("__anchor_imgui", "output_transform", explicit_after);
        add("__anchor_pelican_ui", "__anchor_debug_draw", explicit_after);
        add("__anchor_pelican_ui", "pelican_ui", explicit_after);
        add("__anchor_post_main", "__anchor_tonemap", explicit_after);
        add("__anchor_sprite", "__anchor_post_main", explicit_after);
        add("__anchor_tonemap", "__anchor_post_ldr", explicit_after);

        add("__snapshot_opaque_color", "__anchor_sprite", explicit_after);
        add("__snapshot_opaque_color", "__snapshot_opaque_depth",
            explicit_after);
        add("__snapshot_opaque_color", "forward_transparent",
            read_after_write, "opaque_color");
        add("__snapshot_opaque_depth", "__anchor_sprite", explicit_after);
        add("__snapshot_opaque_depth", "forward_transparent",
            explicit_after);
        add("__snapshot_opaque_depth", "forward_transparent",
            read_after_write, "opaque_depth");
        add("forward_transparent", "__anchor_sprite", explicit_after);
        add("forward_transparent", "FinalBloomComposite", read_after_write,
            "lit_color");
        add("forward_transparent", "HighLuminanceExtraction",
            read_after_write, "lit_color");

        add("gbuffer_pass", "__anchor_sprite", explicit_after);
        add("gbuffer_pass", "__snapshot_opaque_depth", read_after_write,
            "offscreen_depth");
        add("gbuffer_pass", "forward_transparent", read_after_write,
            "offscreen_depth");
        for (const std::string_view resource :
             {std::string_view{"g_emissive"},
              std::string_view{"gbuffer_albedo"},
              std::string_view{"gbuffer_material"},
              std::string_view{"gbuffer_normal"},
              std::string_view{"gbuffer_worldpos"}}) {
            add("gbuffer_pass", "lighting_pass", read_after_write, resource);
        }
        add("gbuffer_pass", "ssao_pass", explicit_after);
        add("gbuffer_pass", "ssao_pass", read_after_write,
            "gbuffer_normal");
        add("gbuffer_pass", "ssao_pass", read_after_write,
            "gbuffer_worldpos");

        add("lighting_pass", "__anchor_sprite", explicit_after);
        add("lighting_pass", "__snapshot_opaque_color", explicit_after);
        add("lighting_pass", "__snapshot_opaque_color", read_after_write,
            "lit_color");
        add("lighting_pass", "__snapshot_opaque_color", snapshot_after);
        add("lighting_pass", "__snapshot_opaque_depth", snapshot_after);
        add("lighting_pass", "forward_transparent", read_after_write,
            "lit_color");
        add("pelican_ui", "__anchor_debug_draw", explicit_after);
        add("pelican_ui", "output_transform", read_after_write, "display");
        add("ssao_blur_pass", "__anchor_sprite", explicit_after);
        add("ssao_blur_pass", "lighting_pass", explicit_after);
        add("ssao_blur_pass", "lighting_pass", read_after_write,
            "ssao_blur");
        add("ssao_pass", "__anchor_sprite", explicit_after);
        add("ssao_pass", "ssao_blur_pass", explicit_after);
        add("ssao_pass", "ssao_blur_pass", read_after_write,
            "ssao_output");

        constexpr std::array bloom_members{
            std::string_view{"FinalBloomComposite"},
            std::string_view{"HighLuminanceExtraction"},
            std::string_view{"HorizontalBlur_0"},
            std::string_view{"HorizontalBlur_1"},
            std::string_view{"HorizontalBlur_2"},
            std::string_view{"HorizontalBlur_3"},
            std::string_view{"UpsampleBlend_1"},
            std::string_view{"UpsampleBlend_2"},
            std::string_view{"UpsampleBlend_3"},
            std::string_view{"VerticalBlur_0"},
            std::string_view{"VerticalBlur_1"},
            std::string_view{"VerticalBlur_2"},
            std::string_view{"VerticalBlur_3"},
        };
        for (const auto member : bloom_members) {
            add("__anchor_post_ldr", member, explicit_after);
        }
        add("__anchor_post_ldr", "__anchor_pelican_ui", explicit_after);

        add("HighLuminanceExtraction", "HorizontalBlur_0",
            explicit_after);
        add("HighLuminanceExtraction", "HorizontalBlur_0",
            read_after_write, "Bloom_Threshold_RT");
        add("HighLuminanceExtraction", "__anchor_pelican_ui",
            explicit_after);
        for (int index = 0; index != 3; ++index) {
            const std::string horizontal =
                "HorizontalBlur_" + std::to_string(index);
            const std::string vertical =
                "VerticalBlur_" + std::to_string(index);
            const std::string upsample =
                "UpsampleBlend_" + std::to_string(index + 1);
            const std::string resource =
                "Bloom_Downsample_H_" + std::to_string(index) + "_RT";
            add(horizontal, vertical, explicit_after);
            add(horizontal, vertical, read_after_write, resource);
            add(horizontal, upsample, read_after_write, resource);
            add(horizontal, "__anchor_pelican_ui", explicit_after);
        }
        add("HorizontalBlur_3", "VerticalBlur_3", explicit_after);
        add("HorizontalBlur_3", "VerticalBlur_3", read_after_write,
            "Bloom_Downsample_H_3_RT");
        add("HorizontalBlur_3", "__anchor_pelican_ui", explicit_after);

        for (int index = 0; index != 3; ++index) {
            const std::string vertical =
                "VerticalBlur_" + std::to_string(index);
            const std::string horizontal =
                "HorizontalBlur_" + std::to_string(index + 1);
            const std::string upsample =
                "UpsampleBlend_" + std::to_string(index + 1);
            const std::string resource =
                "Bloom_Upsample_V_" + std::to_string(index) + "_RT";
            add(vertical, horizontal, explicit_after);
            add(vertical, horizontal, read_after_write, resource);
            add(vertical, upsample, read_after_write, resource);
            add(vertical, "__anchor_pelican_ui", explicit_after);
        }
        add("VerticalBlur_3", "UpsampleBlend_3", explicit_after);
        add("VerticalBlur_3", "UpsampleBlend_3", read_after_write,
            "Bloom_Upsample_V_3_RT");
        add("VerticalBlur_3", "__anchor_pelican_ui", explicit_after);

        for (int index = 3; index != 0; --index) {
            const std::string from =
                "UpsampleBlend_" + std::to_string(index);
            const std::string to =
                index == 1
                    ? std::string{"FinalBloomComposite"}
                    : "UpsampleBlend_" + std::to_string(index - 1);
            const std::string resource =
                "Bloom_Downsample_H_" + std::to_string(index - 1) +
                "_RT";
            add(from, to, explicit_after);
            add(from, to, read_after_write, resource);
            add(from, "__anchor_pelican_ui", explicit_after);
        }
        add("FinalBloomComposite", "__anchor_pelican_ui", explicit_after);
        add("FinalBloomComposite", "pelican_ui", read_after_write,
            "display");

        if (result.size() != 100) {
            throw std::logic_error(
                "independent WP316 dependency expectation must contain 100 records");
        }
        return result;
    }();
    return expected;
}

std::string expectedRecordIdentity(const ExpectedDependency &dependency) {
    constexpr char separator = '\x1f';
    return dependency.from + separator + dependency.to + separator +
           dependency.reason + separator + dependency.resource;
}

QStringList expectedStrings(const std::vector<std::string> &values) {
    QStringList result;
    for (const auto &value : values) {
        result.push_back(QString::fromStdString(value));
    }
    return result;
}

using ExpectedBundles =
    std::map<EdgePair, std::vector<ExpectedDependency>, std::less<>>;

ExpectedBundles expectedExpandedBundles() {
    auto dependencies = expectedExpandedDependencies();
    std::ranges::sort(
        dependencies, {}, [](const ExpectedDependency &dependency) {
            return std::tie(dependency.from, dependency.to,
                            dependency.reason, dependency.resource);
        });
    ExpectedBundles result;
    for (const auto &dependency : dependencies) {
        result[{dependency.from, dependency.to}].push_back(dependency);
    }
    return result;
}

QString expectedDependencyLabel(
    const std::vector<ExpectedDependency> &dependencies) {
    std::set<std::string, std::less<>> resources;
    std::set<std::string, std::less<>> reasons;
    for (const auto &dependency : dependencies) {
        if (!dependency.resource.empty()) {
            resources.insert(dependency.resource);
        }
        reasons.insert(dependency.reason);
    }
    QStringList parts;
    for (const auto &resource : resources) {
        parts.push_back(QString::fromStdString(resource));
    }
    if (parts.empty()) {
        for (const auto &reason : reasons) {
            const auto marker = reason.rfind('.');
            const auto version = reason.rfind('@');
            parts.push_back(QString::fromStdString(reason.substr(
                marker == std::string::npos ? 0 : marker + 1,
                version == std::string::npos
                    ? std::string::npos
                    : version - (marker == std::string::npos ? 0
                                                              : marker + 1))));
        }
    }
    QString result = parts.join(QStringLiteral(", "));
    if (dependencies.size() > 1) {
        result += QStringLiteral("  x%1").arg(dependencies.size());
    }
    return result;
}

std::vector<QGraphicsItem *> itemsWithEndpoints(QGraphicsScene &value,
                                                 const char *item_kind,
                                                 std::string_view from,
                                                 std::string_view to) {
    std::vector<QGraphicsItem *> result;
    const QString expected_from = QString::fromUtf8(
        from.data(), static_cast<qsizetype>(from.size()));
    const QString expected_to =
        QString::fromUtf8(to.data(), static_cast<qsizetype>(to.size()));
    for (QGraphicsItem *item : itemsOfKind(value, item_kind)) {
        if (item->data(FramePlanFromNameRole).toString() == expected_from &&
            item->data(FramePlanToNameRole).toString() == expected_to) {
            result.push_back(item);
        }
    }
    return result;
}

QGraphicsItem *singleItemWithEndpoints(QGraphicsScene &value,
                                       const char *item_kind,
                                       std::string_view from,
                                       std::string_view to) {
    const auto matches = itemsWithEndpoints(value, item_kind, from, to);
    REQUIRE(matches.size() == 1);
    return matches.front();
}

QGraphicsItem *nodeLabelItem(QGraphicsScene &value, std::string_view name) {
    const QString expected = QString::fromUtf8(
        name.data(), static_cast<qsizetype>(name.size()));
    std::vector<QGraphicsItem *> matches;
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanNodeLabelItem)) {
        if (item->data(FramePlanNameRole).toString() == expected) {
            matches.push_back(item);
        }
    }
    REQUIRE(matches.size() == 1);
    return matches.front();
}

QColor expectedNodeColor(std::string_view source) {
    if (source == "project") {
        return QColor{QStringLiteral("#3978a8")};
    }
    if (source.starts_with("feature:")) {
        return QColor{QStringLiteral("#c47b25")};
    }
    return QColor{QStringLiteral("#66717e")};
}

QPainterPath expectedNodePath(bool anchor) {
    QPainterPath path;
    if (anchor) {
        QPolygonF diamond;
        diamond << QPointF{18.0, 16.0} << QPointF{34.0, 32.0}
                << QPointF{18.0, 48.0} << QPointF{2.0, 32.0};
        path.addPolygon(diamond);
        path.closeSubpath();
    } else {
        path.addRoundedRect(QRectF{0.0, 0.0, 240.0, 64.0}, 9.0, 9.0);
    }
    return path;
}

std::map<std::string, QPointF, std::less<>> expandedNodePositions() {
    std::map<std::string, QPointF, std::less<>> result;
    qreal x = 0.0;
    for (const auto &node : expectedExpandedNodes()) {
        result.emplace(node.name, QPointF{x, 0.0});
        x += 320.0;
    }
    return result;
}

std::map<std::string, QPointF, std::less<>>
sceneNodePositions(QGraphicsScene &value) {
    std::map<std::string, QPointF, std::less<>> result;
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanNodeItem)) {
        result.emplace(item->data(FramePlanNameRole).toString().toStdString(),
                       item->scenePos());
    }
    return result;
}

void requireExpectedExpandedScene(QGraphicsScene &value) {
    constexpr auto graph = "main_render";
    const auto &expected_nodes = expectedExpandedNodes();
    const auto expected_positions = expandedNodePositions();
    const auto expected_bundles = expectedExpandedBundles();

    REQUIRE(itemsOfKind(value, FramePlanNodeItem).size() ==
            expected_nodes.size());
    REQUIRE(itemsOfKind(value, FramePlanNodeLabelItem).size() ==
            expected_nodes.size());
    REQUIRE(itemsOfKind(value, FramePlanGroupItem).empty());
    REQUIRE(itemsOfKind(value, FramePlanGroupLabelItem).empty());
    for (const auto &expected : expected_nodes) {
        QGraphicsItem *item = nodeItem(value, expected.name);
        REQUIRE(item != nullptr);
        auto *path_item = dynamic_cast<QGraphicsPathItem *>(item);
        REQUIRE(path_item != nullptr);
        REQUIRE(item->isVisible());
        REQUIRE(item->scenePos() == expected_positions.at(expected.name));
        REQUIRE(item->data(FramePlanGraphRole).toString() ==
                QLatin1String{graph});
        REQUIRE(item->data(FramePlanNameRole).toString() ==
                QString::fromStdString(expected.name));
        REQUIRE(item->data(FramePlanSourceRole).toString() ==
                QString::fromStdString(expected.source));
        const QColor fill = expectedNodeColor(expected.source);
        REQUIRE(item->data(FramePlanColorRole).toString() ==
                fill.name(QColor::HexRgb));
        REQUIRE(item->data(FramePlanAnchorRole).toBool() == expected.anchor);
        REQUIRE(item->data(FramePlanMembersRole).toStringList() ==
                QStringList{QString::fromStdString(expected.name)});
        REQUIRE(item->data(FramePlanInternalEdgeRecordsRole)
                    .toStringList()
                    .empty());
        REQUIRE(path_item->path() == expectedNodePath(expected.anchor));
        REQUIRE(path_item->brush() == QBrush{fill});
        QPen expected_pen{expected.anchor
                              ? QColor{QStringLiteral("#d8e3ec")}
                              : QColor{QStringLiteral("#edf2f6")}};
        expected_pen.setWidthF(1.3);
        if (expected.anchor) {
            expected_pen.setStyle(Qt::DashLine);
        }
        REQUIRE(path_item->pen() == expected_pen);

        QGraphicsItem *label_item = nodeLabelItem(value, expected.name);
        auto *label =
            dynamic_cast<QGraphicsSimpleTextItem *>(label_item);
        REQUIRE(label != nullptr);
        REQUIRE(label->parentItem() == item);
        REQUIRE(label->isVisible());
        REQUIRE(label->text() == QString::fromStdString(expected.name));
        REQUIRE(label->brush() ==
                QBrush{QColor{QStringLiteral("#f7f9fb")}});
        REQUIRE(label->font().bold());
        REQUIRE(label->data(FramePlanGraphRole).toString() ==
                QLatin1String{graph});
        REQUIRE(label->data(FramePlanNameRole).toString() ==
                QString::fromStdString(expected.name));
    }

    REQUIRE(itemsOfKind(value, FramePlanEdgeItem).size() ==
            expected_bundles.size());
    REQUIRE(itemsOfKind(value, FramePlanEdgeArrowItem).size() ==
            expected_bundles.size());
    REQUIRE(itemsOfKind(value, FramePlanEdgeLabelItem).size() ==
            expected_bundles.size());
    const QPen edge_pen = [] {
        QPen result{QColor{QStringLiteral("#748394")}};
        result.setWidthF(1.35);
        return result;
    }();
    const QPen arrow_pen{QColor{QStringLiteral("#748394")}};
    const QBrush arrow_brush{QColor{QStringLiteral("#748394")}};
    const QPen label_pen{QColor{QStringLiteral("#9aa7b4")}};
    const QBrush label_brush{QColor{QStringLiteral("#f5f7f9")}};
    std::size_t lane = 0;
    StringSet expected_records;
    for (const auto &[endpoints, dependencies] : expected_bundles) {
        const auto &[from, to] = endpoints;
        INFO("expected logical edge: " << from << " -> " << to);
        QGraphicsItem *item = singleItemWithEndpoints(
            value, FramePlanEdgeItem, from, to);
        auto *path_item = dynamic_cast<QGraphicsPathItem *>(item);
        REQUIRE(path_item != nullptr);
        REQUIRE(item->isVisible());
        const QString identity = QString::fromStdString(from + "->" + to);
        REQUIRE(item->data(FramePlanGraphRole).toString() ==
                QLatin1String{graph});
        REQUIRE(item->data(FramePlanNameRole).toString() == identity);
        REQUIRE(item->data(FramePlanFromNameRole).toString() ==
                QString::fromStdString(from));
        REQUIRE(item->data(FramePlanToNameRole).toString() ==
                QString::fromStdString(to));

        std::vector<std::string> identities;
        std::set<std::string, std::less<>> resources;
        for (const auto &dependency : dependencies) {
            identities.push_back(expectedRecordIdentity(dependency));
            expected_records.insert(identities.back());
            if (!dependency.resource.empty()) {
                resources.insert(dependency.resource);
            }
        }
        const std::vector<std::string> resource_values{resources.begin(),
                                                       resources.end()};
        REQUIRE(item->data(FramePlanEdgeRecordsRole).toStringList() ==
                expectedStrings(identities));
        REQUIRE(item->data(FramePlanResourcesRole).toStringList() ==
                expectedStrings(resource_values));

        const QPointF from_position = expected_positions.at(from);
        const QPointF to_position = expected_positions.at(to);
        const QPointF start{from_position.x() + 240.0, 32.0};
        const QPointF tip{to_position.x(), 32.0};
        const qreal left_turn = start.x() + 18.0;
        const qreal right_turn = tip.x() - 18.0;
        const qreal lane_y = 134.0 + static_cast<qreal>(lane) * 34.0;
        QPainterPath expected_path{start};
        expected_path.lineTo(left_turn, start.y());
        expected_path.lineTo(left_turn, lane_y);
        expected_path.lineTo(right_turn, lane_y);
        expected_path.lineTo(right_turn, tip.y());
        expected_path.lineTo(tip);
        REQUIRE(path_item->path() == expected_path);
        REQUIRE(path_item->pen() == edge_pen);
        REQUIRE(path_item->brush().style() == Qt::NoBrush);

        QGraphicsItem *arrow_item = singleItemWithEndpoints(
            value, FramePlanEdgeArrowItem, from, to);
        auto *arrow = dynamic_cast<QGraphicsPolygonItem *>(arrow_item);
        REQUIRE(arrow != nullptr);
        REQUIRE(arrow->isVisible());
        QPolygonF expected_arrow;
        expected_arrow << tip << QPointF{tip.x() - 10.0, tip.y() - 5.0}
                       << QPointF{tip.x() - 10.0, tip.y() + 5.0};
        REQUIRE(arrow->polygon() == expected_arrow);
        REQUIRE(arrow->pen() == arrow_pen);
        REQUIRE(arrow->brush() == arrow_brush);
        REQUIRE(arrow->data(FramePlanGraphRole).toString() ==
                QLatin1String{graph});
        REQUIRE(arrow->data(FramePlanNameRole).toString() == identity);
        REQUIRE(arrow->data(FramePlanFromNameRole).toString() ==
                QString::fromStdString(from));
        REQUIRE(arrow->data(FramePlanToNameRole).toString() ==
                QString::fromStdString(to));

        QGraphicsItem *label_item = singleItemWithEndpoints(
            value, FramePlanEdgeLabelItem, from, to);
        auto *label_box = dynamic_cast<QGraphicsRectItem *>(label_item);
        REQUIRE(label_box != nullptr);
        REQUIRE(label_box->isVisible());
        REQUIRE(label_box->rect().topLeft() == QPointF{});
        REQUIRE(label_box->rect().height() == 24.0);
        REQUIRE(label_box->scenePos().y() == lane_y - 12.0);
        REQUIRE(label_box->scenePos().x() +
                    label_box->rect().width() / 2.0 ==
                (left_turn + right_turn) / 2.0);
        REQUIRE(label_box->pen() == label_pen);
        REQUIRE(label_box->brush() == label_brush);
        REQUIRE(label_box->data(FramePlanGraphRole).toString() ==
                QLatin1String{graph});
        REQUIRE(label_box->data(FramePlanNameRole).toString() == identity);
        REQUIRE(label_box->data(FramePlanFromNameRole).toString() ==
                QString::fromStdString(from));
        REQUIRE(label_box->data(FramePlanToNameRole).toString() ==
                QString::fromStdString(to));
        REQUIRE(label_box->data(FramePlanEdgeRecordsRole).toStringList() ==
                expectedStrings(identities));
        REQUIRE(label_box->data(FramePlanResourcesRole).toStringList() ==
                expectedStrings(resource_values));
        REQUIRE(label_box->childItems().size() == 1);
        auto *label = dynamic_cast<QGraphicsSimpleTextItem *>(
            label_box->childItems().front());
        REQUIRE(label != nullptr);
        REQUIRE(label->isVisible());
        REQUIRE(label->text() == expectedDependencyLabel(dependencies));
        REQUIRE(label->brush() ==
                QBrush{QColor{QStringLiteral("#263441")}});
        ++lane;
    }
    REQUIRE(sceneDependencyRecords(value) == expected_records);
}

void requireReadableLabels(QGraphicsScene &value) {
    // Compare reserved layout regions, not glyph bounds. Glyph metrics vary
    // between native Windows and the offscreen QPA even for the same family.
    std::vector<QRectF> reserved_regions;
    for (QGraphicsItem *item : value.items()) {
        const QString item_kind = kind(*item);
        if (item_kind == QLatin1String{FramePlanNodeLabelItem} ||
            item_kind == QLatin1String{FramePlanGroupLabelItem}) {
            auto *label = dynamic_cast<QGraphicsSimpleTextItem *>(item);
            REQUIRE(label != nullptr);
            REQUIRE_FALSE(label->text().isEmpty());
            REQUIRE(label->parentItem() != nullptr);
            const bool group =
                item_kind == QLatin1String{FramePlanGroupLabelItem};
            reserved_regions.emplace_back(
                label->parentItem()->scenePos(),
                QSizeF{group ? 260.0 : 240.0, group ? 76.0 : 64.0});
        } else if (item_kind == QLatin1String{FramePlanEdgeLabelItem}) {
            auto *label_box = dynamic_cast<QGraphicsRectItem *>(item);
            REQUIRE(label_box != nullptr);
            REQUIRE(label_box->childItems().size() == 1);
            auto *label = dynamic_cast<QGraphicsSimpleTextItem *>(
                label_box->childItems().front());
            REQUIRE(label != nullptr);
            REQUIRE_FALSE(label->text().isEmpty());
            // The production box width follows QFontMetricsF.  Use a fixed,
            // conservative per-code-unit envelope around its lane center so
            // QPA font metrics cannot change the overlap conclusion.
            const QPointF center =
                label_box->mapToScene(label_box->rect().center());
            const qreal fixed_width = std::max<qreal>(
                56.0, 14.0 + 12.0 * static_cast<qreal>(label->text().size()));
            QRectF fixed_region{0.0, 0.0, fixed_width, 24.0};
            fixed_region.moveCenter(center);
            reserved_regions.push_back(fixed_region);
        }
    }
    REQUIRE_FALSE(reserved_regions.empty());
    const qreal minimum =
        value.property("pelicanMinimumLabelSpacing").toReal();
    REQUIRE(minimum >= 8.0);
    for (std::size_t left = 0; left < reserved_regions.size(); ++left) {
        for (std::size_t right = left + 1;
             right < reserved_regions.size(); ++right) {
            const QRectF padded = reserved_regions[left].adjusted(
                -minimum, -minimum, minimum, minimum);
            REQUIRE_FALSE(padded.intersects(reserved_regions[right]));
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
    "WP317 missing execution plan is a named logical error rather than a valid zero-dependency graph",
    "[devstudio][frame-plan][logical-graph][wp317][negative-contrast]") {
    (void)application();
    const Json zero_dependencies = independentOpportunityFramePlan(
        Pelican::PlanningProfileKind::conservative_debug);
    REQUIRE(zero_dependencies.at("execution_plan")
                .at("dependencies")
                .empty());
    REQUIRE_FALSE(zero_dependencies.at("nodes").empty());

    Json missing = zero_dependencies;
    missing.erase("execution_plan");
    REQUIRE(missing.at("nodes") == zero_dependencies.at("nodes"));
    REQUIRE(missing.at("barriers") == zero_dependencies.at("barriers"));

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(missing.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);

    REQUIRE(logical.property("pelicanExecutionPlanState").toString() ==
            QStringLiteral("unavailable"));
    REQUIRE(logical.property("pelicanExecutionPlanReasonCode").toString() ==
            QStringLiteral("execution_plan_missing"));
    REQUIRE(logical.property("pelicanDependencyRecordCount").toULongLong() ==
            0);
    REQUIRE(itemsOfKind(logical, FramePlanNodeItem).empty());
    REQUIRE(itemsOfKind(logical, FramePlanEdgeItem).empty());
    const auto unavailable =
        itemsOfKind(logical, FramePlanLogicalUnavailableItem);
    REQUIRE(unavailable.size() == 1);
    REQUIRE(unavailable.front()
                ->data(FramePlanLogicalStateRole)
                .toString() == QStringLiteral("unavailable"));
    REQUIRE(unavailable.front()
                ->data(FramePlanReasonCodeRole)
                .toString() == QStringLiteral("execution_plan_missing"));
    REQUIRE(unavailable.front()->toolTip().contains(
        QStringLiteral("execution_plan was not published")));
    auto *unavailable_panel =
        dynamic_cast<QGraphicsRectItem *>(unavailable.front());
    REQUIRE(unavailable_panel != nullptr);
    const QRectF reserved_error_region =
        unavailable_panel->sceneBoundingRect();
    REQUIRE(reserved_error_region.width() >= 600.0);
    REQUIRE(reserved_error_region.height() >= 120.0);
    REQUIRE(logical.sceneRect().contains(reserved_error_region));

    // Same production entry and widget; the only difference is the valid,
    // compiler-published execution plan whose dependency array is truly empty.
    widget.receiveResult(
        QByteArray::fromStdString(zero_dependencies.dump()));
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanExecutionPlanState").toString() ==
            QStringLiteral("available"));
    REQUIRE(logical.property("pelicanExecutionPlanReasonCode")
                .toString()
                .isEmpty());
    REQUIRE(logical.property("pelicanDependencyRecordCount").toULongLong() ==
            0);
    REQUIRE(itemsOfKind(logical, FramePlanLogicalUnavailableItem).empty());
    const auto valid_nodes = itemsOfKind(logical, FramePlanNodeItem);
    REQUIRE(valid_nodes.size() == zero_dependencies.at("nodes").size());
    REQUIRE_FALSE(valid_nodes.empty());
    REQUIRE(itemsOfKind(logical, FramePlanEdgeItem).empty());
    for (const QGraphicsItem *node : valid_nodes) {
        REQUIRE(logical.sceneRect().contains(node->sceneBoundingRect()));
    }
}

TEST_CASE(
    "WP306 logical graph exposes lost edges grouping identity and readable deterministic layout",
    "[devstudio][frame-plan][logical-graph][wp306]") {
    (void)application();
    const std::string captured = readText(PELICAN_TEST_FRAME_PLAN_FIXTURE);

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
    requireExpectedExpandedScene(logical);
    requireReadableLabels(logical);
    const auto ordered_positions = sceneNodePositions(logical);

    QSpinBox &minimum = groupMinimum(widget);
    minimum.setValue(13);
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanVisibleItemCount").toULongLong() == 18);
    REQUIRE(itemsOfKind(logical, FramePlanNodeItem).size() == 17);
    const auto groups = itemsOfKind(logical, FramePlanGroupItem);
    REQUIRE(groups.size() == 1);
    auto *group = dynamic_cast<QGraphicsPathItem *>(groups.front());
    REQUIRE(group != nullptr);
    const StringSet actual_members =
        strings(group->data(FramePlanMembersRole));
    REQUIRE(actual_members.size() == 13);
    StringSet one_wrong_member = expectedBloomMembers();
    one_wrong_member.erase("HorizontalBlur_2");
    one_wrong_member.insert("lighting_pass");
    REQUIRE(actual_members != one_wrong_member);
    REQUIRE(actual_members == expectedBloomMembers());
    REQUIRE(group->isVisible());
    REQUIRE(group->data(FramePlanGraphRole).toString() ==
            QStringLiteral("main_render"));
    REQUIRE(group->data(FramePlanNameRole).toString() ==
            QStringLiteral("group:resource-family:Bloom"));
    REQUIRE(group->data(FramePlanSourceRole).toString() ==
            QStringLiteral("project"));
    REQUIRE_FALSE(group->data(FramePlanAnchorRole).toBool());
    QPainterPath expected_group_path;
    expected_group_path.addRoundedRect(
        QRectF{0.0, 0.0, 260.0, 76.0}, 13.0, 13.0);
    REQUIRE(group->path() == expected_group_path);
    REQUIRE(group->brush() ==
            QBrush{QColor{QStringLiteral("#7653a6")}});
    QPen expected_group_pen{QColor{QStringLiteral("#edf2f6")}};
    expected_group_pen.setWidthF(2.0);
    REQUIRE(group->pen() == expected_group_pen);
    const auto group_labels = itemsOfKind(logical, FramePlanGroupLabelItem);
    REQUIRE(group_labels.size() == 1);
    auto *group_label = dynamic_cast<QGraphicsSimpleTextItem *>(
        group_labels.front());
    REQUIRE(group_label != nullptr);
    REQUIRE(group_label->parentItem() == group);
    REQUIRE(group_label->isVisible());
    REQUIRE(group_label->text() ==
            QStringLiteral("Bloom structure\n13 nodes"));
    REQUIRE(group_label->brush() ==
            QBrush{QColor{QStringLiteral("#f7f9fb")}});
    REQUIRE(group_label->font().bold());
    REQUIRE(group_label->data(FramePlanGraphRole).toString() ==
            QStringLiteral("main_render"));
    REQUIRE(group_label->data(FramePlanNameRole).toString() ==
            QStringLiteral("group:resource-family:Bloom"));

    StringSet expected_records;
    for (const auto &dependency : expectedExpandedDependencies()) {
        expected_records.insert(expectedRecordIdentity(dependency));
    }
    REQUIRE(sceneDependencyRecords(logical) == expected_records);
    requireReadableLabels(logical);

    minimum.setValue(14);
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanVisibleItemCount").toULongLong() == 30);
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    requireExpectedExpandedScene(logical);
    requireReadableLabels(logical);

    // buildFramePlanModel() has already applied its order-field sort here.
    // Reverse that view-independent result itself, then feed the reordered
    // input directly to the layout boundary.
    FramePlanModel reordered = buildFramePlanModel(captured);
    std::reverse(reordered.nodes.begin(), reordered.nodes.end());
    std::reverse(reordered.dependencies.begin(), reordered.dependencies.end());
    auto *graphics = dynamic_cast<FramePlanGraphicsScene *>(&logical);
    REQUIRE(graphics != nullptr);
    graphics->populate(reordered, 14);
    QApplication::processEvents();
    REQUIRE(sceneNodePositions(logical) == ordered_positions);
    requireExpectedExpandedScene(logical);
    requireReadableLabels(logical);
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
    const auto variant_case = GENERATE(table<
        Pelican::RenderPipelineGraphVariant, std::string, bool>({
        {Pelican::RenderPipelineGraphVariant::flat, "flat", true},
        {Pelican::RenderPipelineGraphVariant::preview, "preview", false},
        {Pelican::RenderPipelineGraphVariant::xr, "xr", true},
    }));
    const auto &[variant, variant_name, publishes_physical_plan] =
        variant_case;
    CAPTURE(variant_name, PELICAN_RUNTIME_SHADER_COMPILER);

#if !PELICAN_WITH_OPENXR
    if (variant == Pelican::RenderPipelineGraphVariant::xr) {
        for (const bool feature_enabled : {false, true}) {
            try {
                (void)animgraphFramePlan(feature_enabled, variant);
                FAIL("XR production variant unexpectedly compiled without OpenXR");
            } catch (const std::runtime_error &error) {
                REQUIRE(std::string_view{error.what()}.find(
                            "XR graph variant is unavailable in this build") !=
                        std::string_view::npos);
            }
        }
        return;
    }
#endif

#if !PELICAN_RUNTIME_SHADER_COMPILER
    {
        const Json enabled = animgraphFramePlan(true, variant);
        const Json disabled = animgraphFramePlan(false, variant);
        REQUIRE(wireNodeNames(enabled) == wireNodeNames(disabled));
        REQUIRE(wireEdges(enabled) == wireEdges(disabled));
        REQUIRE_FALSE(wireNodeNames(enabled).contains("shadow_depth"));

        EmbeddedViewport off_viewport;
        FramePlanWidget off_widget{&off_viewport};
        groupMinimum(off_widget).setValue(64);
        off_widget.receiveResult(QByteArray::fromStdString(disabled.dump()));
        QApplication::processEvents();
        QGraphicsScene &off_logical = scene(off_widget);
        REQUIRE(sceneEdges(off_logical) == wireEdges(disabled));
        REQUIRE(off_logical.property("pelicanGraph").toString() ==
                QString::fromStdString(
                    disabled.at("graph").get<std::string>()));
        if (publishes_physical_plan) {
            REQUIRE(disabled.contains("physical_target_plan"));
            REQUIRE(off_logical.property("pelicanPhysicalResourceCount")
                        .toULongLong() ==
                    disabled.at("physical_target_plan")
                        .at("resources")
                        .size());
            REQUIRE(off_logical.property("pelicanPlanningEndpoint")
                        .toString() == QStringLiteral("device:0"));
        } else {
            REQUIRE_FALSE(disabled.contains("physical_target_plan"));
            REQUIRE(off_logical.property("pelicanPhysicalResourceCount")
                        .toULongLong() == 0);
            const auto context =
                itemsOfKind(off_logical, FramePlanPhysicalContextItem);
            REQUIRE(context.size() == 1);
            REQUIRE(context.front()
                        ->data(FramePlanPhysicalStateRole)
                        .toString() == QStringLiteral("unavailable"));
        }
        requireNoReference(off_logical, "shadow_depth", "shadow_map");
        requireReadableLabels(off_logical);
        return;
    }
#endif
    const Json enabled = animgraphFramePlan(true, variant);
    const Json disabled = animgraphFramePlan(false, variant);

    if (publishes_physical_plan) {
        REQUIRE(enabled.contains("physical_target_plan"));
        REQUIRE(disabled.contains("physical_target_plan"));
        for (const auto &resource :
             enabled.at("physical_target_plan").at("resources")) {
            REQUIRE_FALSE(resource.at("logical_resource")
                              .get<std::string>()
                              .starts_with("Bloom_"));
        }
    } else {
        REQUIRE_FALSE(enabled.contains("physical_target_plan"));
        REQUIRE_FALSE(disabled.contains("physical_target_plan"));
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
    if (publishes_physical_plan) {
        REQUIRE(logical.property("pelicanPhysicalResourceCount")
                    .toULongLong() ==
                enabled.at("physical_target_plan").at("resources").size());
        REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
                QStringLiteral("optimized"));
        REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
                QStringLiteral("device:0"));
    } else {
        REQUIRE(logical.property("pelicanPhysicalResourceCount")
                    .toULongLong() == 0);
    }

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
    if (publishes_physical_plan) {
        REQUIRE(logical.property("pelicanPhysicalResourceCount")
                    .toULongLong() ==
                disabled.at("physical_target_plan").at("resources").size());
    } else {
        REQUIRE(logical.property("pelicanPhysicalResourceCount")
                    .toULongLong() == 0);
    }

    minimum.setValue(64);
    QApplication::processEvents();
    REQUIRE(sceneEdges(logical) == wireEdges(disabled));
    REQUIRE(removedFrom(wireEdges(enabled), sceneEdges(logical)) ==
            expected_removed_edges);
    REQUIRE(nodeItem(logical, "shadow_depth") == nullptr);
    requireReadableLabels(logical);
}

} // namespace PelicanStudio
