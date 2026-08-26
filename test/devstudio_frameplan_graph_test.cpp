#include "embeddedviewport.hpp"
#include "frameplangraphics.hpp"
#include "frameplanwidget.hpp"

#include "../src/core/loader/engineresources.hpp"
#include "../src/core/render_algorithms/cube_capture/cubecaptureview.hpp"
#include "../src/core/renderingpass/frameexecutionadapter.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/viewexecutionscheduler.hpp"
#include "../src/project/executionplan.hpp"
#include "../src/project/frameresolutionwire.hpp"
#include "../src/project/renderpipeline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QAction>
#include <QBrush>
#include <QByteArray>
#include <QColor>
#include <QComboBox>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QMenu>
#include <QPainterPath>
#include <QPen>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <queue>
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

Pelican::ResolvedResourceExtent resolveRuntimeExtent(
    const Pelican::VulkanTargetPlan &plan,
    Pelican::ResolvedResourceExtent output_extent,
    std::string_view resource) {
    if (resource == "swapchain") {
        return output_extent;
    }
    const auto found = std::ranges::find(
        plan.resources, resource,
        &Pelican::VulkanPhysicalResourcePlan::logical_resource);
    if (found == plan.resources.end() || !found->extent) {
        throw std::runtime_error(
            "runtime test resolver has no extent contract for " +
            std::string{resource});
    }
    return Pelican::resolveResourceExtent(
        *found->extent, output_extent);
}

void publishRuntimeResolution(
    Json &wire, const Pelican::VulkanTargetPlan &plan,
    Pelican::ResolvedResourceExtent output_extent) {
    wire["runtime_resolution"] =
        Pelican::frameRuntimeResolutionWireToJson(
            Pelican::makeFrameRuntimeResolutionWire(
                plan, [&](std::string_view resource) {
                    return resolveRuntimeExtent(
                        plan, output_extent, resource);
                }));
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
    publishRuntimeResolution(
        wire, *physical.plans.front(),
        Pelican::ResolvedResourceExtent{160, 90});
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

const Json &taaGroupingFramePlan() {
    static const Json wire = [] {
        Json authored = readJson(
            std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
            "example" / "passes" / "main_rendering_config.json");
        auto &targets = authored.at("render_targets");
        const auto lit_color = std::ranges::find_if(
            targets, [](const Json &target) {
                return target.value("name", std::string{}) == "lit_color";
            });
        if (lit_color == targets.end()) {
            throw std::runtime_error(
                "TAA grouping fixture has no lit_color target");
        }
        (*lit_color)["format_class"] = "scene";
        authored["features"] = Json::array(
            {"engine://features/velocity.json",
             "engine://features/taa.json"});

        auto resolved = Pelican::resolveRenderPipeline(
            Pelican::RenderPipelineRequest{
                .authored_config = std::move(authored),
                .source_name = "wp341a/taa_grouping.json",
            },
            Pelican::RenderEnvironmentCapabilities{
                .runtime_shader_compiler_enabled = true,
                .graph_variant =
                    Pelican::RenderPipelineGraphVariant::flat,
            },
            Pelican::RenderPipelineResolveDependencies{
                .load_feature_json = loadEngineDocument,
                .load_pipeline_json = loadEngineDocument,
            });
        return resolvedFramePlan(std::move(resolved), "wp341a_taa");
    }();
    return wire;
}

#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
const Json &planarReflectionGroupingFramePlan() {
    static const Json wire = [] {
        Json authored{
            {"pipeline",
             {{"preset",
               "engine://render_pipelines/hybrid_v1.json"}}},
            {"features",
             Json::array(
                 {{{"ref",
                    "engine://features/planar_reflection.json"},
                   {"parameters",
                    {{"resolution", 64},
                     {"plane_x", 0.0},
                     {"plane_y", 2.0},
                     {"plane_z", 0.0},
                     {"plane_offset", -2.0},
                     {"preserve_raster_winding", true},
                     {"oblique_near_plane", true}}}}})},
        };

        auto resolved = Pelican::resolveRenderPipeline(
            Pelican::RenderPipelineRequest{
                .authored_config = std::move(authored),
                .source_name = "wp344/planar_reflection.json",
            },
            Pelican::RenderEnvironmentCapabilities{
                .runtime_shader_compiler_enabled =
                    PELICAN_RUNTIME_SHADER_COMPILER != 0,
                .graph_variant =
                    Pelican::RenderPipelineGraphVariant::flat,
            },
            Pelican::RenderPipelineResolveDependencies{
                .load_feature_json = loadEngineDocument,
                .load_pipeline_json = loadEngineDocument,
            });
        return resolvedFramePlan(std::move(resolved),
                                 "wp344/planar_reflection");
    }();
    return wire;
}
#endif

#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS && \
    PELICAN_RUNTIME_SHADER_COMPILER
constexpr std::string_view Wp348CubeRegion =
    "wp348.cube_capture_loop";

struct Wp348CubeCaptureMeasurement {
    Json wire;
    std::vector<Pelican::FrameGraphExecutionNode> execution_nodes;
    std::vector<Pelican::LogicalFrameNodeInvocation> invocations;
    std::vector<std::string> view_ids;
};

const Wp348CubeCaptureMeasurement &wp348CubeCaptureMeasurement() {
    static const Wp348CubeCaptureMeasurement measurement = [] {
        Json authored{
            {"pipeline",
             {{"preset",
               "engine://render_pipelines/hybrid_v1.json"}}},
            {"features",
             Json::array(
                 {{{"ref",
                    "engine://features/cube_capture.json"},
                   {"parameters", {{"resolution", 64}}}}})},
        };

        constexpr std::string_view feature_reference =
            "engine://features/cube_capture.json";
        auto resolved = Pelican::resolveRenderPipeline(
            Pelican::RenderPipelineRequest{
                .authored_config = std::move(authored),
                .source_name = "wp348/cube_capture_measurement.json",
            },
            Pelican::RenderEnvironmentCapabilities{
                .runtime_shader_compiler_enabled = true,
                .graph_variant =
                    Pelican::RenderPipelineGraphVariant::flat,
            },
            Pelican::RenderPipelineResolveDependencies{
                .load_feature_json =
                    [feature_reference](std::string_view reference) {
                        if (reference != feature_reference) {
                            return loadEngineDocument(reference);
                        }
                        Json feature =
                            Json::parse(loadEngineDocument(reference));
                        for (auto &entry : feature.at("passes")) {
                            // Region authoring remains on the production
                            // feature-composition input side. The shipped
                            // feature and every shipping config stay intact.
                            entry.at("pass")["regions"] =
                                Json::array({Wp348CubeRegion});
                        }
                        return feature.dump();
                    },
                .load_pipeline_json = loadEngineDocument,
            });

        for (auto &target :
             resolved.normalized_config.at("render_targets")) {
            if (target.value("format_class", std::string{}) ==
                "display") {
                target["width"] = 160;
                target["height"] = 90;
            }
        }
        const Pelican::CompiledRenderPipeline compiled =
            Pelican::compileRenderPipeline(resolved);
        const auto graphs =
            Pelican::parseFrameGraphDefinitionsFromConfigJson(
                resolved.normalized_config);
        if (graphs.size() != 1) {
            throw std::runtime_error(
                "WP348 fixture must resolve exactly one frame graph");
        }
        const Pelican::FramePlan plan =
            Pelican::planFrameGraph(graphs.front());
        Json wire = Pelican::framePlanToJson(plan, &compiled);
        const Pelican::FrameExecutionPlan execution =
            Pelican::compileFrameExecutionPlan(
                graphs.front(), plan,
                Pelican::ExecutionEndpoint{
                    .id = "device:0",
                    .endpoint_class =
                        Pelican::ExecutionEndpointClass::device,
                    .backend = "vulkan",
                });
        wire["execution_plan"] =
            Pelican::frameExecutionPlanToJson(execution);

        const auto targets =
            Pelican::parseRenderTargetDefinitionsFromJson(
                resolved.normalized_config);
        const auto physical = Pelican::compileRenderingTargetPlans(
            graphs, targets, compiled.sample_count_policy,
            vk::Format::eB8G8R8A8Srgb, planningDeviceFacts(),
            std::nullopt, std::nullopt, compiled.target_planning,
            compiled.vulkan_plan_pins,
            compiled.vulkan_physical_fragments);
        if (physical.plans.size() != 1) {
            throw std::runtime_error(
                "WP348 fixture must compile exactly one physical target plan");
        }
        const auto &target_plan = *physical.plans.front();
        wire["physical_target_plan"] =
            Pelican::vulkanTargetPlanToJson(target_plan);
        publishRuntimeResolution(
            wire, target_plan,
            Pelican::ResolvedResourceExtent{160, 90});

        // compileExecution() builds this same plan-ordered name/family
        // projection before the production renderer calls the scheduler. The
        // scheduler observes vector position as invocation.node_index; the
        // runtime-only pass id stored in FrameGraphExecutionNode::index is not
        // consulted here.
        std::vector<Pelican::FrameGraphExecutionNode> execution_nodes;
        execution_nodes.reserve(plan.nodes.size());
        for (const auto &node : plan.nodes) {
            execution_nodes.push_back(
                Pelican::FrameGraphExecutionNode{
                    .kind = node.kind,
                    .name = node.name,
                    .index = 0,
                    .incoming_barriers = {},
                    .view_family = node.view_family,
                });
        }

        const Pelican::RenderViewFamily cube_views =
            Pelican::buildCubeCaptureViewFamily({});
        const std::array families{
            Pelican::LogicalFrameViewFamilyCardinality{
                Pelican::mainRenderViewFamilyId,
                target_plan.view_execution_plan.view_count},
            Pelican::LogicalFrameViewFamilyCardinality{
                Pelican::cubeCaptureRenderViewFamilyId,
                static_cast<std::uint32_t>(cube_views.views.size())},
        };
        auto invocations =
            Pelican::buildLogicalFrameViewFamilySchedule(
                execution_nodes, target_plan, families);
        std::vector<std::string> view_ids;
        view_ids.reserve(cube_views.views.size());
        for (const auto &view : cube_views.views) {
            view_ids.push_back(view.view_id);
        }

        return Wp348CubeCaptureMeasurement{
            .wire = std::move(wire),
            .execution_nodes = std::move(execution_nodes),
            .invocations = std::move(invocations),
            .view_ids = std::move(view_ids),
        };
    }();
    return measurement;
}

std::string describeWp348Invocation(
    const Wp348CubeCaptureMeasurement &measurement,
    const Pelican::LogicalFrameNodeInvocation &invocation) {
    const auto &node =
        measurement.execution_nodes.at(invocation.node_index);
    const std::string &view_id =
        measurement.view_ids.at(invocation.view_index);
    return node.name + "[node=" +
           std::to_string(invocation.node_index) + ",scope=" +
           std::to_string(invocation.scope_index) + ",scope_node=" +
           std::to_string(invocation.scope_node_index) + "/" +
           std::to_string(invocation.scope_node_count) + ",view=" +
           std::to_string(invocation.view_index) + ":" + view_id +
           ",execution=" +
           std::to_string(invocation.execution_index) + "/" +
           std::to_string(invocation.execution_count) + "]";
}
#endif

Json &authoredRenderingPass(Json &authored, std::string_view name) {
    for (auto &group : authored.at("rendering_passes")) {
        for (auto &pass : group.at("passes")) {
            if (pass.value("name", std::string{}) == name) {
                return pass;
            }
        }
    }
    throw std::runtime_error("WP343 fixture has no authored pass named " +
                             std::string{name});
}

Json wp343GroupingAuthoredExample() {
    Json authored = readJson(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
        "example" / "passes" / "main_rendering_config.json");

    // Keep 19 production-authored render passes in one convex block. The
    // shipped UI feature supplies the nineteenth render node after engine
    // anchors, while the two snapshot copies sit between project render
    // passes. Replace those three detours with one ordinary authored pass so
    // the required filter-removal mutation can really collapse all 19 nodes.
    authored["features"] = Json::array();
    auto &targets = authored.at("render_targets");
    Json retained_targets = Json::array();
    for (auto &target : targets) {
        const std::string name = target.value("name", std::string{});
        if (name != "opaque_color" && name != "opaque_depth") {
            retained_targets.push_back(std::move(target));
        }
    }
    targets = std::move(retained_targets);
    targets.push_back({
        {"name", "wp343_forward_color"},
        {"extent_scale", 1.0},
        {"format", "R16G16B16A16_SFLOAT"},
        {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
        {"role", "color"},
        {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
    });
    targets.push_back({
        {"name", "wp343_scene_color"},
        {"extent_scale", 1.0},
        {"format", "R16G16B16A16_SFLOAT"},
        {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
        {"role", "color"},
        {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
    });

    auto &passes = authored.at("rendering_passes").at(0).at("passes");
    Json retained = Json::array();
    for (auto &pass : passes) {
        const std::string name = pass.value("name", std::string{});
        if (name != "__snapshot_opaque_color" &&
            name != "__snapshot_opaque_depth") {
            pass["canonical_anchor"] = "post_ldr";
            retained.push_back(std::move(pass));
            if (name == "forward_transparent") {
                retained.push_back({
                    {"name", "wp343_render_19"},
                    {"type", "fullscreen"},
                    {"canonical_anchor", "post_ldr"},
                    {"output",
                     {{"color", Json::array({"wp343_scene_color"})},
                      {"depth", nullptr}}},
                    {"input", Json::array({"wp343_forward_color"})},
                    {"shader",
                     {{"vertex", "engine://fullscreen"},
                      {"fragment", "engine://fullscreen"}}},
                    {"clear_color", Json::array({0.0, 0.0, 0.0, 1.0})},
                });
            }
        }
    }
    passes = std::move(retained);
    auto &forward = authoredRenderingPass(authored, "forward_transparent");
    forward["screen_inputs"] = {
        {"opaque_color", "lit_color"},
        {"opaque_depth", "offscreen_depth"},
        {"scene_depth", "offscreen_depth"},
        {"linear_view_depth", "offscreen_depth"},
    };
    forward["output"] = {
        {"color", Json::array({"wp343_forward_color"})},
        {"depth", nullptr},
    };
    forward.erase("color_load_op");
    forward.erase("depth_load_op");
    forward.erase("depth_store_op");
    authoredRenderingPass(authored, "HighLuminanceExtraction")["input"] =
        Json::array({"wp343_scene_color"});
    auto &final_inputs =
        authoredRenderingPass(authored, "FinalBloomComposite").at("input");
    final_inputs.at(0) = "wp343_scene_color";
    return authored;
}

Json resolvedWp343Example(
    std::optional<std::vector<std::string>> regions,
    std::string_view label) {
    Json authored = wp343GroupingAuthoredExample();
    // Write the public authoring field before composition. These are adjacent
    // project passes (not a hand-built FramePlanModel), so every assertion
    // below observes resolve -> compile -> plan -> wire output.
    if (regions) {
        authoredRenderingPass(authored, "HorizontalBlur_0")["regions"] =
            *regions;
        authoredRenderingPass(authored, "VerticalBlur_0")["regions"] =
            *regions;
    }

    auto resolved = Pelican::resolveRenderPipeline(
        Pelican::RenderPipelineRequest{
            .authored_config = std::move(authored),
            .source_name = std::string{label} + "/example.json",
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
    return resolvedFramePlan(std::move(resolved), label);
}

const Json &regionGroupingBaselineFramePlan() {
    static const Json wire =
        resolvedWp343Example(std::nullopt, "wp343/without_region");
    return wire;
}

const Json &authoredRegionFramePlan() {
    static const Json wire = resolvedWp343Example(
        std::vector<std::string>{"wp343.bloom_pair"},
        "wp343/authored_region");
    return wire;
}

const std::vector<std::pair<std::string, bool>> &
legacyRegionBoundaryCases() {
    static const std::vector<std::pair<std::string, bool>> cases{
        {"legacy.x", false},
        {"legacy.日本", false},
        {"legacy.", false},
        {"legacy.render ", false},
        {"legacy", true},
        {"Legacy.x", true},
        {"legacy日本", true},
        {" legacy.x", true},
        {"   ", true},
    };
    return cases;
}

const Json &legacyRegionBoundaryFramePlan() {
    static const Json wire = [] {
        Json authored = wp343GroupingAuthoredExample();
        auto &passes = authored.at("rendering_passes").at(0).at("passes");
        const std::size_t required = legacyRegionBoundaryCases().size() * 2;
        if (passes.size() < required) {
            throw std::runtime_error(
                "WP343 legacy boundary fixture has too few passes");
        }
        std::size_t pass_index = 0;
        for (const auto &[region, authored_region] :
             legacyRegionBoundaryCases()) {
            (void)authored_region;
            for (int member = 0; member < 2; ++member) {
                passes.at(pass_index++)["regions"] =
                    Json::array({region});
            }
        }

        auto resolved = Pelican::resolveRenderPipeline(
            Pelican::RenderPipelineRequest{
                .authored_config = std::move(authored),
                .source_name = "wp343/legacy_boundary.json",
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
        return resolvedFramePlan(std::move(resolved),
                                 "wp343/legacy_boundary");
    }();
    return wire;
}

constexpr std::array<std::string_view, 4> MixedFeatureNodes{
    "wp343_mixed_a", "wp343_mixed_b", "wp343_mixed_c",
    "wp343_mixed_d"};

Json mixedMembershipFeature(bool overlapping) {
    Json feature{
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", "wp343_mixed_membership"},
        {"runtime_shader_compiler", "optional"},
        {"render_targets", Json::array()},
        {"passes", Json::array()},
    };

    for (std::size_t index = 0; index < MixedFeatureNodes.size(); ++index) {
        const std::string name{MixedFeatureNodes[index]};
        const std::string output = name + "_output";
        feature["render_targets"].push_back({
            {"name", output},
            {"extent_scale", 1.0},
            {"format", "R16G16B16A16_SFLOAT"},
            {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
            {"role", "color"},
            {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
        });

        Json pass{
            {"name", name},
            {"type", "fullscreen"},
            {"output", {{"color", output}, {"depth", nullptr}}},
            {"shader",
             {{"vertex", "engine://fullscreen"},
              {"fragment", "engine://fullscreen"}}},
        };
        if (index != 0) {
            pass["input"] = Json::array(
                {std::string{MixedFeatureNodes[index - 1]} + "_output"});
        }
        if (index == 0 || index == 1) {
            pass["regions"] = Json::array({"wp343.mixed"});
        }
        if (overlapping && index == 1) {
            pass["regions"].push_back("wp343.second");
        }
        feature["passes"].push_back({
            {"insert", index == 0
                           ? "end"
                           : "after:" +
                                 std::string{MixedFeatureNodes[index - 1]}},
            {"pass", std::move(pass)},
        });
    }
    return feature;
}

Json resolvedMixedMembershipFramePlan(bool overlapping) {
    Json authored = readJson(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
        "example" / "passes" / "main_rendering_config.json");
    constexpr std::string_view feature_reference =
        "engine://features/wp343_mixed_membership.json";
    authored["features"] =
        Json::array({std::string{feature_reference}});

    auto resolved = Pelican::resolveRenderPipeline(
        Pelican::RenderPipelineRequest{
            .authored_config = std::move(authored),
            .source_name = overlapping
                               ? "wp343/mixed_overlap.json"
                               : "wp343/mixed_priority.json",
        },
        Pelican::RenderEnvironmentCapabilities{
            .runtime_shader_compiler_enabled =
                PELICAN_RUNTIME_SHADER_COMPILER != 0,
            .graph_variant = Pelican::RenderPipelineGraphVariant::flat,
        },
        Pelican::RenderPipelineResolveDependencies{
            .load_feature_json =
                [overlapping, feature_reference](std::string_view reference) {
                if (reference != feature_reference) {
                    return loadEngineDocument(reference);
                }
                return mixedMembershipFeature(overlapping).dump();
            },
            .load_pipeline_json = loadEngineDocument,
        });
    return resolvedFramePlan(
        std::move(resolved),
        overlapping ? "wp343/mixed_overlap" : "wp343/mixed_priority");
}

const Json &mixedMembershipFramePlan(bool overlapping) {
    static const Json priority = resolvedMixedMembershipFramePlan(false);
    static const Json overlap = resolvedMixedMembershipFramePlan(true);
    return overlapping ? overlap : priority;
}

constexpr std::string_view Wp347Producer = "wp347_fused_producer";
constexpr std::string_view Wp347Consumer = "wp347_fused_consumer";
constexpr std::string_view Wp347Ordered = "wp347_ordered_tail";
constexpr std::string_view Wp347Tile = "wp347_tile";
constexpr std::string_view Wp347Intermediate = "wp347_intermediate";
constexpr std::string_view Wp347Output = "wp347_output";
constexpr std::string_view Wp347Region = "wp347.fused_pair";

Json wp347BarrierFeature() {
    const auto fullscreen_shader = [] {
        return Json{{"vertex", "engine://fullscreen"},
                    {"fragment", "engine://fullscreen"}};
    };
    return Json{
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", "wp347_barrier_visibility"},
        {"runtime_shader_compiler", "optional"},
        {"render_targets",
         Json::array(
             {{{"name", Wp347Tile},
               {"extent_scale", 1.0},
               {"format", "R16G16B16A16_SFLOAT"},
               {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
               {"role", "color"},
               {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
              {{"name", Wp347Output},
               {"extent_scale", 1.0},
               {"format", "R16G16B16A16_SFLOAT"},
               {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
               {"role", "color"},
               {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
              {{"name", Wp347Intermediate},
               {"extent_scale", 1.0},
               {"format", "R16G16B16A16_SFLOAT"},
               {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
               {"role", "color"},
               {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}})},
        {"passes",
         Json::array(
             {{{"insert", "end"},
               {"pass",
                {{"name", Wp347Producer},
                 {"type", "fullscreen"},
                 {"input", Json::array({"lit_color"})},
                 {"input_footprints", {{"lit_color", "arbitrary"}}},
                 {"regions", Json::array({Wp347Region})},
                 {"output", {{"color", Wp347Tile}, {"depth", nullptr}}},
                 {"shader", fullscreen_shader()}}}},
              {{"insert", std::string{"after:"} +
                              std::string{Wp347Producer}},
               {"pass",
                {{"name", Wp347Consumer},
                 {"type", "fullscreen"},
                 {"input", Json::array({Wp347Tile})},
                 {"input_footprints", {{Wp347Tile, "same_pixel"}}},
                 {"regions", Json::array({Wp347Region})},
                 {"output",
                  {{"color", Wp347Intermediate}, {"depth", nullptr}}},
                 {"shader", fullscreen_shader()}}}},
              {{"insert", std::string{"after:"} +
                              std::string{Wp347Consumer}},
               {"pass",
                {{"name", Wp347Ordered},
                 {"type", "fullscreen"},
                 {"after", Json::array({Wp347Producer})},
                 {"input", Json::array({Wp347Intermediate})},
                 {"input_footprints",
                  {{Wp347Intermediate, "arbitrary"}}},
                 {"output", {{"color", Wp347Output}, {"depth", nullptr}}},
                 {"shader", fullscreen_shader()}}}}})},
    };
}

const Json &wp347BarrierFramePlan() {
    static const Json wire = [] {
        Json authored = readJson(
            std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
            "example" / "passes" / "main_rendering_config.json");
        constexpr std::string_view feature_reference =
            "engine://features/wp347_barrier_visibility.json";
        authored["features"] =
            Json::array({std::string{feature_reference}});

        auto resolved = Pelican::resolveRenderPipeline(
            Pelican::RenderPipelineRequest{
                .authored_config = std::move(authored),
                .source_name = "wp347/barrier_visibility.json",
            },
            Pelican::RenderEnvironmentCapabilities{
                .runtime_shader_compiler_enabled =
                    PELICAN_RUNTIME_SHADER_COMPILER != 0,
                .graph_variant = Pelican::RenderPipelineGraphVariant::flat,
            },
            Pelican::RenderPipelineResolveDependencies{
                .load_feature_json =
                    [feature_reference](std::string_view reference) {
                        if (reference == feature_reference) {
                            return wp347BarrierFeature().dump();
                        }
                        return loadEngineDocument(reference);
                    },
                .load_pipeline_json = loadEngineDocument,
            });
        return resolvedFramePlan(std::move(resolved),
                                 "wp347/barrier_visibility");
    }();
    return wire;
}

StringSet wireFeatureMembers(const Json &wire,
                             std::string_view feature) {
    StringSet members;
    for (const auto &node : wire.at("nodes")) {
        if (node.value("provider_feature", std::string{}) == feature) {
            members.insert(node.at("name").get<std::string>());
        }
    }
    return members;
}

StringSet wireRegionMembers(const Json &wire, std::string_view region) {
    StringSet members;
    for (const auto &node :
         wire.at("physical_target_plan")
             .at("lowering_graph")
             .at("nodes")) {
        const auto &regions = node.at("regions");
        if (std::ranges::any_of(regions, [&](const Json &value) {
                return value.get<std::string>() == region;
            })) {
            members.insert(node.at("name").get<std::string>());
        }
    }
    return members;
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
    publishRuntimeResolution(
        wire, *compilation.plans.front(),
        Pelican::ResolvedResourceExtent{32, 32});
    return wire;
}

Json fractionalScaleFramePlan() {
    const Json config{
        {"render_targets",
         Json::array(
             {{{"name", "scaled_scene"},
               {"extent_scale", 0.7},
               {"format", "R8G8B8A8_UNORM"},
               {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
              {{"name", "custom_output"},
               {"extent_scale", 1.0},
               {"width", 10},
               {"height", 10},
               {"format", "R8G8B8A8_UNORM"},
               {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}})},
        {"rendering_passes",
         Json::array(
             {{{"name", "fractional_scale"},
               {"passes",
                Json::array(
                    {{{"name", "scene"},
                      {"type", "fullscreen"},
                      {"resolution_domain", "scene"},
                      {"output",
                       {{"color", "scaled_scene"}, {"depth", nullptr}}}},
                     {{"name", "present"},
                      {"type", "fullscreen"},
                      {"resolution_domain", "output"},
                      {"input", Json::array({"scaled_scene"})},
                      {"output",
                       {{"color", "custom_output"},
                        {"depth", nullptr}}}}})}}})},
    };
    const auto graphs =
        Pelican::parseFrameGraphDefinitionsFromConfigJson(config);
    const auto targets =
        Pelican::parseRenderTargetDefinitionsFromJson(config);
    const auto compilation = Pelican::compileRenderingTargetPlans(
        graphs, targets, Pelican::compileSampleCountPolicy(config),
        vk::Format::eB8G8R8A8Unorm, planningDeviceFacts());
    if (graphs.size() != 1 || compilation.plans.size() != 1) {
        throw std::runtime_error(
            "fractional scale producer must compile one graph and one "
            "physical plan");
    }

    const Pelican::FramePlan plan =
        Pelican::planFrameGraph(graphs.front());
    Json wire = Pelican::framePlanToJson(plan);
    wire["execution_plan"] = Pelican::frameExecutionPlanToJson(
        Pelican::compileFrameExecutionPlan(
            graphs.front(), plan,
            Pelican::ExecutionEndpoint{
                .id = "device:0",
                .endpoint_class =
                    Pelican::ExecutionEndpointClass::device,
                .backend = "vulkan",
            }));
    wire["physical_target_plan"] =
        Pelican::vulkanTargetPlanToJson(*compilation.plans.front());
    publishRuntimeResolution(
        wire, *compilation.plans.front(),
        Pelican::ResolvedResourceExtent{10, 10});
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

QComboBox &targetSelector(FramePlanWidget &widget) {
    auto *selector = widget.findChild<QComboBox *>(
        QStringLiteral("pelican.framePlanTarget"));
    if (selector == nullptr) {
        throw std::runtime_error("frame-plan target control was not installed");
    }
    return *selector;
}

QSpinBox &subtreeDepth(FramePlanWidget &widget) {
    auto *spin = widget.findChild<QSpinBox *>(
        QStringLiteral("pelican.framePlanDepth"));
    if (spin == nullptr) {
        throw std::runtime_error("frame-plan depth control was not installed");
    }
    return *spin;
}

QLabel &framePlanStatus(FramePlanWidget &widget) {
    auto *status = widget.findChild<QLabel *>(
        QStringLiteral("pelican.framePlanStatus"));
    if (status == nullptr) {
        throw std::runtime_error("frame-plan status label was not installed");
    }
    return *status;
}

QGraphicsView &logicalView(FramePlanWidget &widget) {
    auto *view = widget.findChild<QGraphicsView *>(
        QStringLiteral("pelican.framePlanLogicalView"));
    if (view == nullptr) {
        throw std::runtime_error("frame-plan logical view was not installed");
    }
    return *view;
}

QTreeWidget &logicalDetails(FramePlanWidget &widget) {
    auto *details = widget.findChild<QTreeWidget *>(
        QStringLiteral("pelican.framePlanLogicalDetails"));
    if (details == nullptr) {
        throw std::runtime_error(
            "frame-plan logical details pane was not installed");
    }
    return *details;
}

QLineEdit &framePlanFilter(FramePlanWidget &widget) {
    auto *filter = widget.findChild<QLineEdit *>(
        QStringLiteral("pelican.framePlanFilter"));
    if (filter == nullptr) {
        throw std::runtime_error("frame-plan filter was not installed");
    }
    return *filter;
}

QString joinedValues(const std::vector<std::string> &values) {
    QStringList result;
    for (const auto &value : values) {
        result.push_back(QString::fromStdString(value));
    }
    return result.join(QStringLiteral(", "));
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

StringSet sceneNodeNames(QGraphicsScene &value) {
    StringSet result;
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanNodeItem)) {
        result.insert(item->data(FramePlanNameRole).toString().toStdString());
    }
    return result;
}

struct BundleObservation {
    std::string from;
    std::string to;
    qulonglong order = 0;
    QStringList records;
    QPainterPath path;

    bool operator==(const BundleObservation &) const = default;
};

std::vector<BundleObservation> sceneBundles(QGraphicsScene &value) {
    std::vector<BundleObservation> result;
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanEdgeItem)) {
        auto *path = dynamic_cast<QGraphicsPathItem *>(item);
        if (path == nullptr) {
            throw std::runtime_error("frame-plan edge was not a path item");
        }
        result.push_back(BundleObservation{
            item->data(FramePlanFromNameRole).toString().toStdString(),
            item->data(FramePlanToNameRole).toString().toStdString(),
            item->data(FramePlanBundleOrderRole).toULongLong(),
            item->data(FramePlanEdgeRecordsRole).toStringList(), path->path()});
    }
    std::ranges::sort(result, {}, &BundleObservation::order);
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

StringSet sceneGroupMembers(QGraphicsScene &value,
                            const QString &group_id) {
    StringSet members;
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanNodeItem)) {
        if (item->data(FramePlanGroupIdRole).toString() == group_id) {
            members.insert(
                item->data(FramePlanNameRole).toString().toStdString());
        }
    }
    return members;
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

QGraphicsItem *singleGroupItem(QGraphicsScene &value) {
    const auto groups = itemsOfKind(value, FramePlanGroupItem);
    REQUIRE(groups.size() == 1);
    return groups.front();
}

StringSet boundaryTargets(QGraphicsScene &value) {
    StringSet result;
    for (QGraphicsItem *item :
         itemsOfKind(value, FramePlanBoundaryStubItem)) {
        result.insert(
            item->data(FramePlanBoundaryTargetRole).toString().toStdString());
    }
    return result;
}

struct SceneItemIdentity {
    std::string kind;
    std::string graph;
    std::string name;
    std::string from;
    std::string to;
    std::vector<std::string> members;

    bool operator<(const SceneItemIdentity &other) const {
        return std::tie(kind, graph, name, from, to, members) <
               std::tie(other.kind, other.graph, other.name, other.from,
                        other.to, other.members);
    }

    bool operator==(const SceneItemIdentity &) const = default;
};

using SceneItemSet = std::multiset<SceneItemIdentity>;

SceneItemSet annotatedSceneItems(QGraphicsScene &value) {
    SceneItemSet result;
    for (QGraphicsItem *item : value.items()) {
        const QString item_kind =
            item->data(FramePlanItemKindRole).toString();
        if (item_kind.isEmpty()) {
            continue;
        }
        std::vector<std::string> members;
        for (const QString &member :
             item->data(FramePlanMembersRole).toStringList()) {
            members.push_back(member.toStdString());
        }
        result.insert(SceneItemIdentity{
            item_kind.toStdString(),
            item->data(FramePlanGraphRole).toString().toStdString(),
            item->data(FramePlanNameRole).toString().toStdString(),
            item->data(FramePlanFromNameRole).toString().toStdString(),
            item->data(FramePlanToNameRole).toString().toStdString(),
            std::move(members),
        });
    }
    return result;
}

SceneItemSet quotientSceneItems(const SceneItemSet &expanded,
                                const StringSet &members,
                                const std::string &graph,
                                const std::string &group_name) {
    SceneItemSet result;
    std::set<SceneItemIdentity> quotient_edges;
    const auto quotient_name = [&](const std::string &name) {
        return members.contains(name) ? group_name : name;
    };
    for (SceneItemIdentity item : expanded) {
        if ((item.kind == FramePlanNodeItem ||
             item.kind == FramePlanNodeLabelItem) &&
            members.contains(item.name)) {
            continue;
        }
        const bool logical_edge =
            item.kind == FramePlanEdgeItem ||
            item.kind == FramePlanEdgeArrowItem ||
            item.kind == FramePlanEdgeLabelItem;
        if (logical_edge) {
            item.from = quotient_name(item.from);
            item.to = quotient_name(item.to);
            if (item.from == item.to) {
                continue;
            }
            item.name = item.from + "->" + item.to;
            quotient_edges.insert(std::move(item));
            continue;
        }
        result.insert(std::move(item));
    }
    result.insert(SceneItemIdentity{
        FramePlanGroupItem, graph, group_name, {}, {},
        std::vector<std::string>{members.begin(), members.end()},
    });
    result.insert(SceneItemIdentity{
        FramePlanGroupLabelItem, graph, group_name, {}, {}, {},
    });
    result.insert(quotient_edges.begin(), quotient_edges.end());
    return result;
}

std::vector<QGraphicsItem *> namedShapesAndLabels(
    QGraphicsScene &value, std::string_view name) {
    const QString expected = QString::fromUtf8(
        name.data(), static_cast<qsizetype>(name.size()));
    std::vector<QGraphicsItem *> result;
    for (QGraphicsItem *item : value.items()) {
        if (item->data(FramePlanNameRole).toString() != expected) {
            continue;
        }
        if (dynamic_cast<QGraphicsPathItem *>(item) != nullptr ||
            dynamic_cast<QGraphicsSimpleTextItem *>(item) != nullptr) {
            result.push_back(item);
        }
    }
    return result;
}

std::vector<QGraphicsItem *> edgeShapesAndLabels(
    QGraphicsScene &value, std::string_view from, std::string_view to) {
    const QString expected_from = QString::fromUtf8(
        from.data(), static_cast<qsizetype>(from.size()));
    const QString expected_to = QString::fromUtf8(
        to.data(), static_cast<qsizetype>(to.size()));
    std::vector<QGraphicsItem *> result;
    for (QGraphicsItem *item : value.items()) {
        if (item->data(FramePlanFromNameRole).toString() != expected_from ||
            item->data(FramePlanToNameRole).toString() != expected_to) {
            continue;
        }
        if (dynamic_cast<QGraphicsPathItem *>(item) != nullptr ||
            dynamic_cast<QGraphicsRectItem *>(item) != nullptr ||
            dynamic_cast<QGraphicsSimpleTextItem *>(item) != nullptr) {
            result.push_back(item);
        }
    }
    return result;
}

QString edgeVisibleText(QGraphicsScene &value, std::string_view from,
                        std::string_view to) {
    QStringList labels;
    for (QGraphicsItem *item : edgeShapesAndLabels(value, from, to)) {
        if (const auto *text =
                dynamic_cast<const QGraphicsSimpleTextItem *>(item);
            text != nullptr && text->isVisible()) {
            labels.push_back(text->text());
        }
    }
    REQUIRE(labels.size() == 1);
    return labels.front();
}

bool visibleSceneTextContains(QGraphicsScene &value,
                              const QString &needle) {
    return std::ranges::any_of(value.items(), [&](QGraphicsItem *item) {
        const auto *text =
            dynamic_cast<const QGraphicsSimpleTextItem *>(item);
        return text != nullptr && text->isVisible() &&
               text->text().contains(needle);
    });
}

StringSet boundaryTargetsFor(const FramePlanModel &model,
                             const StringSet &members) {
    StringSet result;
    for (const auto &dependency : model.dependencies) {
        const bool from_inside = members.contains(dependency.from);
        const bool to_inside = members.contains(dependency.to);
        if (from_inside == to_inside) {
            continue;
        }
        result.insert(from_inside ? dependency.to : dependency.from);
    }
    return result;
}

void renameNode(FramePlanModel &model, std::string_view old_name,
                std::string new_name) {
    const auto node = std::ranges::find(model.nodes, old_name,
                                        &FramePlanNode::name);
    if (node == model.nodes.end()) {
        throw std::runtime_error("could not rename missing frame-plan node");
    }
    const std::string previous = node->name;
    node->name = std::move(new_name);
    for (auto &dependency : model.dependencies) {
        if (dependency.from == previous) {
            dependency.from = node->name;
        }
        if (dependency.to == previous) {
            dependency.to = node->name;
        }
    }
}

EdgeSet transitiveReachability(const StringSet &nodes,
                               const EdgeSet &edges) {
    std::map<std::string, StringSet, std::less<>> adjacency;
    for (const auto &[from, to] : edges) {
        adjacency[from].insert(to);
    }

    EdgeSet result;
    for (const auto &start : nodes) {
        std::queue<std::string> frontier;
        StringSet visited;
        if (const auto found = adjacency.find(start);
            found != adjacency.end()) {
            for (const auto &next : found->second) {
                frontier.push(next);
            }
        }
        while (!frontier.empty()) {
            std::string current = std::move(frontier.front());
            frontier.pop();
            if (!visited.insert(current).second) {
                continue;
            }
            result.emplace(start, current);
            if (const auto found = adjacency.find(current);
                found != adjacency.end()) {
                for (const auto &next : found->second) {
                    frontier.push(next);
                }
            }
        }
    }
    return result;
}

QString edgeLabelText(QGraphicsScene &value, std::string_view from,
                      std::string_view to) {
    QGraphicsItem *box = nullptr;
    const QString expected_from = QString::fromUtf8(
        from.data(), static_cast<qsizetype>(from.size()));
    const QString expected_to = QString::fromUtf8(
        to.data(), static_cast<qsizetype>(to.size()));
    for (QGraphicsItem *candidate :
         itemsOfKind(value, FramePlanEdgeLabelItem)) {
        if (candidate->data(FramePlanFromNameRole).toString() ==
                expected_from &&
            candidate->data(FramePlanToNameRole).toString() ==
                expected_to) {
            REQUIRE(box == nullptr);
            box = candidate;
        }
    }
    REQUIRE(box != nullptr);
    QStringList labels;
    for (QGraphicsItem *child : box->childItems()) {
        if (auto *text = dynamic_cast<QGraphicsSimpleTextItem *>(child)) {
            labels.push_back(text->text());
        }
    }
    REQUIRE(labels.size() == 1);
    return labels.front();
}

FramePlanNode groupingNode(std::string name, std::size_t order,
                           std::string feature = {}) {
    FramePlanNode node;
    node.name = std::move(name);
    node.kind = "raster";
    node.source =
        feature.empty() ? "project" : "feature:" + feature;
    node.provider_feature = std::move(feature);
    node.order = order;
    node.reads = {"focus"};
    return node;
}

FramePlanLoweringNode groupingLoweringNode(
    std::string name, std::vector<std::string> regions) {
    return FramePlanLoweringNode{
        .name = std::move(name),
        .kind = "render",
        .dialect = "raster",
        .regions = std::move(regions),
    };
}

void makeGroupingPhysicalPlanAvailable(FramePlanModel &model) {
    model.physical_plan.state = FramePlanPhysicalPlanState::available;
    model.physical_plan.unavailable_reason_code.clear();
    model.physical_plan.unavailable_reason.clear();
    model.physical_plan.graph = model.graph;
    model.physical_plan.lowering_graph_state =
        FramePlanLoweringGraphState::available;
    model.physical_plan.lowering_graph_unavailable_reason_code.clear();
    model.physical_plan.lowering_graph_unavailable_reason.clear();
}

FramePlanModel nonConvexGroupingModel() {
    FramePlanModel model;
    model.graph = "non_convex_grouping_graph";
    model.execution_plan.state =
        FramePlanExecutionPlanState::available;
    model.execution_plan.unavailable_reason_code.clear();
    model.execution_plan.unavailable_reason.clear();
    model.execution_plan.graph = model.graph;

    FramePlanResource focus;
    focus.name = "focus";
    model.resources.push_back(std::move(focus));
    model.nodes = {
        groupingNode("inside_a", 0, "non_convex"),
        groupingNode("outside", 1),
        groupingNode("inside_b", 2, "non_convex"),
        groupingNode("project_peer", 3),
        groupingNode("single_feature_node", 4, "single"),
    };
    model.dependencies = {
        {"inside_a", "outside", "pelican.dependency.leave@1", "leave"},
        {"outside", "inside_b", "pelican.dependency.return@1", "return"},
    };
    makeGroupingPhysicalPlanAvailable(model);
    for (const auto &node : model.nodes) {
        model.physical_plan.lowering_nodes.push_back(
            groupingLoweringNode(node.name, {"legacy.render"}));
    }
    return model;
}

FramePlanModel nonConvexRegionGroupingModel() {
    FramePlanModel model = nonConvexGroupingModel();
    model.graph = "non_convex_region_grouping_graph";
    model.execution_plan.graph = model.graph;
    makeGroupingPhysicalPlanAvailable(model);
    for (auto &node : model.nodes) {
        node.provider_feature.clear();
        node.source = "project";
    }
    model.physical_plan.lowering_nodes = {
        groupingLoweringNode("inside_a",
                            {"legacy.render", "wp343.non_convex"}),
        groupingLoweringNode("outside", {"legacy.render"}),
        groupingLoweringNode("inside_b",
                            {"legacy.render", "wp343.non_convex"}),
        groupingLoweringNode("project_peer", {"legacy.render"}),
        groupingLoweringNode("single_feature_node", {"legacy.render"}),
    };
    return model;
}

FramePlanModel overlappingRegionGroupingModel() {
    FramePlanModel model;
    model.graph = "overlapping_region_grouping_graph";
    model.execution_plan.state =
        FramePlanExecutionPlanState::available;
    model.execution_plan.unavailable_reason_code.clear();
    model.execution_plan.unavailable_reason.clear();
    model.execution_plan.graph = model.graph;

    FramePlanResource focus;
    focus.name = "focus";
    model.resources.push_back(std::move(focus));
    model.nodes = {
        groupingNode("overlap", 0, "feature_fallback"),
        groupingNode("feature_peer", 1, "feature_fallback"),
        groupingNode("alpha_peer", 2),
        groupingNode("beta_peer", 3),
    };
    makeGroupingPhysicalPlanAvailable(model);
    model.physical_plan.lowering_nodes = {
        groupingLoweringNode(
            "overlap", {"legacy.render", "wp343.alpha", "wp343.beta"}),
        groupingLoweringNode("feature_peer", {"legacy.render"}),
        groupingLoweringNode("alpha_peer",
                            {"legacy.render", "wp343.alpha"}),
        groupingLoweringNode("beta_peer",
                            {"legacy.render", "wp343.beta"}),
    };
    return model;
}

void triggerContextAction(FramePlanGraphicsScene &scene,
                          QGraphicsItem &item,
                          const QString &action_text) {
    bool triggered = false;
    QTimer chooser;
    chooser.setInterval(0);
    QObject::connect(&chooser, &QTimer::timeout, &scene, [&] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *menu = qobject_cast<QMenu *>(widget);
            if (menu == nullptr || !menu->isVisible()) {
                continue;
            }
            for (QAction *action : menu->actions()) {
                if (action->text().contains(action_text)) {
                    triggered = true;
                    action->trigger();
                    menu->close();
                    return;
                }
            }
            menu->close();
        }
    });
    chooser.start();

    QGraphicsSceneContextMenuEvent event{
        QEvent::GraphicsSceneContextMenu};
    event.setScenePos(item.sceneBoundingRect().center());
    event.setScreenPos(QPoint{100, 100});
    QApplication::sendEvent(&scene, &event);
    chooser.stop();
    QApplication::processEvents();
    REQUIRE(triggered);
}

void doubleClickSceneItem(FramePlanGraphicsScene &scene,
                          QGraphicsItem &item) {
    QGraphicsSceneMouseEvent event{
        QEvent::GraphicsSceneMouseDoubleClick};
    event.setScenePos(item.sceneBoundingRect().center());
    event.setButton(Qt::LeftButton);
    event.setButtons(Qt::LeftButton);
    QApplication::sendEvent(&scene, &event);
    QApplication::processEvents();
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
    "WP343 authored region replaces its real nodes after the production wire path",
    "[devstudio][frame-plan][grouping][region][wp343][negative-contrast]") {
    (void)application();
    const StringSet members{"HorizontalBlur_0", "VerticalBlur_0"};

    // Negative half: the shipped JSON has no authored region. Its real
    // compose -> compile -> plan -> wire result exposes all 19 automatic
    // legacy.render tags, but none is a grouping unit.
    const Json &without_region = regionGroupingBaselineFramePlan();
    const StringSet legacy_render_members =
        wireRegionMembers(without_region, "legacy.render");
    REQUIRE(legacy_render_members.size() == 19);
    const FramePlanModel without_model =
        buildFramePlanModel(without_region.dump());
    INFO(without_model.physical_plan.unavailable_reason);
    REQUIRE(without_model.physical_plan.state ==
            FramePlanPhysicalPlanState::available);
    REQUIRE(without_model.physical_plan.loweringGraphAvailable());
    FramePlanGraphicsScene without_scene;
    without_scene.populate(
        without_model,
        FramePlanNodeKey{without_model.graph, "swapchain"}, 64);
    REQUIRE(std::ranges::includes(sceneNodeNames(without_scene), members));
    REQUIRE(itemsOfKind(without_scene, FramePlanGroupItem).empty());
    REQUIRE(itemsOfKind(without_scene, FramePlanGroupLabelItem).empty());
    for (const auto &member : members) {
        REQUIRE(namedShapesAndLabels(without_scene, member).size() == 2);
    }

    // This conditional is a mutation sentinel. In production it is inert.
    // If the legacy-prefix filter is removed, all 19 render nodes share one
    // id, the call really collapses them, and the empty-group assertion below
    // fails with an actual group item in the scene.
    StringSet unexpected_legacy_group_ids;
    for (const auto &name : legacy_render_members) {
        QGraphicsItem *item = nodeItem(without_scene, name);
        REQUIRE(item != nullptr);
        const std::string group_id =
            item->data(FramePlanGroupIdRole).toString().toStdString();
        if (!group_id.empty()) {
            unexpected_legacy_group_ids.insert(group_id);
        }
    }
    if (!unexpected_legacy_group_ids.empty()) {
        REQUIRE(unexpected_legacy_group_ids.size() == 1);
        INFO(nodeItem(without_scene, *legacy_render_members.begin())
                 ->data(FramePlanReasonRole)
                 .toString()
                 .toStdString());
        REQUIRE(without_scene.collapseGroup(QString::fromStdString(
            *unexpected_legacy_group_ids.begin())));
        REQUIRE(strings(singleGroupItem(without_scene)->data(
                    FramePlanMembersRole)) == legacy_render_members);
    }
    REQUIRE(itemsOfKind(without_scene, FramePlanGroupItem).empty());

    // Positive half: the public "regions" field was written into the actual
    // pass JSON before composition. The lowering graph, Studio parser, and
    // scene all consume the resulting wire document.
    const Json &with_region = authoredRegionFramePlan();
    REQUIRE(wireRegionMembers(with_region, "wp343.bloom_pair") ==
            members);
    const FramePlanModel with_model =
        buildFramePlanModel(with_region.dump());
    INFO(with_model.physical_plan.unavailable_reason);
    REQUIRE(with_model.physical_plan.state ==
            FramePlanPhysicalPlanState::available);
    REQUIRE(with_model.physical_plan.loweringGraphAvailable());
    FramePlanGraphicsScene with_scene;
    with_scene.populate(
        with_model, FramePlanNodeKey{with_model.graph, "swapchain"}, 64);
    const SceneItemSet expanded_items = annotatedSceneItems(with_scene);
    REQUIRE(itemsOfKind(with_scene, FramePlanGroupItem).empty());
    REQUIRE(itemsOfKind(with_scene, FramePlanGroupLabelItem).empty());
    for (const auto &member : members) {
        REQUIRE(namedShapesAndLabels(with_scene, member).size() == 2);
    }

    QGraphicsItem *collapse_member =
        nodeItem(with_scene, "HorizontalBlur_0");
    REQUIRE(collapse_member != nullptr);
    const QString region_group_id =
        collapse_member->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(region_group_id.isEmpty());
    REQUIRE(collapse_member
                ->data(FramePlanGroupCollapsibleRole)
                .toBool());
    REQUIRE(with_scene.collapseGroup(region_group_id));

    REQUIRE(itemsOfKind(with_scene, FramePlanGroupItem).size() == 1);
    REQUIRE(itemsOfKind(with_scene, FramePlanGroupLabelItem).size() == 1);
    for (const auto &member : members) {
        // Kind-independent: a member shape or label with a blank/different
        // FramePlanItemKindRole still fails this assertion.
        REQUIRE(namedShapesAndLabels(with_scene, member).empty());
    }
    QGraphicsItem *group = singleGroupItem(with_scene);
    REQUIRE(strings(group->data(FramePlanMembersRole)) == members);
    const std::string group_name =
        group->data(FramePlanNameRole).toString().toStdString();
    REQUIRE(group_name ==
            "__pelican_group__:region:wp343.bloom_pair");
    REQUIRE(annotatedSceneItems(with_scene) ==
            quotientSceneItems(expanded_items, members, with_model.graph,
                               group_name));
    REQUIRE(visibleSceneTextContains(
        with_scene, QStringLiteral("Region: wp343.bloom_pair")));
}

TEST_CASE(
    "WP343 mixed production membership splits one feature by authored region priority",
    "[devstudio][frame-plan][grouping][region][feature][priority][wp343]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER);
    const Json &wire = mixedMembershipFramePlan(false);
    const StringSet region_members{"wp343_mixed_a", "wp343_mixed_b"};
    const StringSet fallback_members{"wp343_mixed_c", "wp343_mixed_d"};
    const StringSet feature_members =
        wireFeatureMembers(wire, "wp343_mixed_membership");
    REQUIRE(feature_members ==
            StringSet{"wp343_mixed_a", "wp343_mixed_b",
                      "wp343_mixed_c", "wp343_mixed_d"});
    REQUIRE(wireRegionMembers(wire, "wp343.mixed") ==
            region_members);

    const FramePlanModel model = buildFramePlanModel(wire.dump());
    REQUIRE(model.physical_plan.loweringGraphAvailable());
    FramePlanGraphicsScene logical;
    logical.populate(
        model,
        FramePlanNodeKey{model.graph, "wp343_mixed_d_output"}, 64);

    QGraphicsItem *a = nodeItem(logical, "wp343_mixed_a");
    QGraphicsItem *c = nodeItem(logical, "wp343_mixed_c");
    REQUIRE(a != nullptr);
    REQUIRE(c != nullptr);
    const QString region_group_id =
        a->data(FramePlanGroupIdRole).toString();
    const QString feature_group_id =
        c->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(region_group_id.isEmpty());
    REQUIRE_FALSE(feature_group_id.isEmpty());
    REQUIRE(region_group_id != feature_group_id);

    // The actual scene membership, not just the selected label, is the
    // negative control against absorbing the whole feature into the region.
    REQUIRE(sceneGroupMembers(logical, region_group_id) == region_members);
    REQUIRE(sceneGroupMembers(logical, feature_group_id) ==
            fallback_members);
    REQUIRE(logical.collapseGroup(region_group_id));
    REQUIRE(logical.collapseGroup(feature_group_id));

    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).size() == 2);
    for (QGraphicsItem *group :
         itemsOfKind(logical, FramePlanGroupItem)) {
        const QString id =
            group->data(FramePlanGroupIdRole).toString();
        if (id == region_group_id) {
            REQUIRE(strings(group->data(FramePlanMembersRole)) ==
                    region_members);
        } else {
            REQUIRE(id == feature_group_id);
            REQUIRE(strings(group->data(FramePlanMembersRole)) ==
                    fallback_members);
        }
    }
    REQUIRE(visibleSceneTextContains(
        logical, QStringLiteral("Region: wp343.mixed")));
    REQUIRE(visibleSceneTextContains(
        logical,
        QStringLiteral("Feature: wp343_mixed_membership")));
}

TEST_CASE(
    "WP343 lowering lookup distinguishes found nodes from missing nodes and unavailable graphs",
    "[devstudio][frame-plan][grouping][region][availability][wp343a]") {
    (void)application();

    // A later runtime-resolution failure makes the overall physical view
    // unavailable, but the already validated lowering graph remains a known,
    // usable source of authored-region membership.
    Json runtime_missing = authoredRegionFramePlan();
    runtime_missing.erase("runtime_resolution");
    const FramePlanModel residual =
        buildFramePlanModel(runtime_missing.dump());
    REQUIRE_FALSE(residual.physical_plan.available());
    REQUIRE(residual.physical_plan.unavailable_reason_code ==
            "physical_plan_runtime_resolution_missing");
    REQUIRE(residual.physical_plan.loweringGraphAvailable());
    REQUIRE(residual.physical_plan.lowering_graph_unavailable_reason.empty());

    FramePlanGraphicsScene residual_scene;
    residual_scene.populate(
        residual, FramePlanNodeKey{residual.graph, "swapchain"}, 64);
    QGraphicsItem *residual_member =
        nodeItem(residual_scene, "HorizontalBlur_0");
    REQUIRE(residual_member != nullptr);
    const QString residual_group_id =
        residual_member->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(residual_group_id.isEmpty());
    REQUIRE(sceneGroupMembers(residual_scene, residual_group_id) ==
            StringSet{"HorizontalBlur_0", "VerticalBlur_0"});
    REQUIRE_FALSE(visibleSceneTextContains(
        residual_scene, QStringLiteral("region_grouping_unavailable")));

    // A valid graph with one broken exact-name join is a third state. Even
    // though A has provider_feature, it must not be absorbed by that feature.
    FramePlanModel missing_node = buildFramePlanModel(
        mixedMembershipFramePlan(false).dump());
    const auto authored_a = std::ranges::find(
        missing_node.nodes, std::string{"wp343_mixed_a"},
        &FramePlanNode::name);
    REQUIRE(authored_a != missing_node.nodes.end());
    REQUIRE(authored_a->provider_feature ==
            "wp343_mixed_membership");
    std::erase_if(
        missing_node.physical_plan.lowering_nodes,
        [](const FramePlanLoweringNode &node) {
            return node.name == "wp343_mixed_a";
        });
    REQUIRE(missing_node.physical_plan.loweringGraphAvailable());

    FramePlanGraphicsScene missing_scene;
    missing_scene.populate(
        missing_node,
        FramePlanNodeKey{missing_node.graph,
                         "wp343_mixed_d_output"},
        64);
    QGraphicsItem *missing =
        nodeItem(missing_scene, "wp343_mixed_a");
    REQUIRE(missing != nullptr);
    REQUIRE(missing->data(FramePlanGroupIdRole).toString().isEmpty());
    const auto warnings =
        itemsOfKind(missing_scene, FramePlanGroupWarningItem);
    REQUIRE(warnings.size() == 1);
    const QString missing_reason =
        warnings.front()->data(FramePlanReasonRole).toString();
    REQUIRE(missing_reason.contains(
        QStringLiteral("region_grouping_node_missing")));
    REQUIRE(missing_reason.contains(
        QStringLiteral("wp343_mixed_a")));
    REQUIRE(visibleSceneTextContains(missing_scene, missing_reason));
}

TEST_CASE(
    "WP343 a multi-region production node joins neither region nor feature fallback",
    "[devstudio][frame-plan][grouping][region][feature][priority][overlap][wp343]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER);
    const Json &wire = mixedMembershipFramePlan(true);
    REQUIRE(wireFeatureMembers(wire, "wp343_mixed_membership") ==
            StringSet{"wp343_mixed_a", "wp343_mixed_b",
                      "wp343_mixed_c", "wp343_mixed_d"});
    REQUIRE(wireRegionMembers(wire, "wp343.mixed") ==
            StringSet{"wp343_mixed_a", "wp343_mixed_b"});
    REQUIRE(wireRegionMembers(wire, "wp343.second") ==
            StringSet{"wp343_mixed_b"});

    const FramePlanModel model = buildFramePlanModel(wire.dump());
    FramePlanGraphicsScene logical;
    logical.populate(
        model,
        FramePlanNodeKey{model.graph, "wp343_mixed_d_output"}, 64);

    QGraphicsItem *a = nodeItem(logical, "wp343_mixed_a");
    QGraphicsItem *b = nodeItem(logical, "wp343_mixed_b");
    QGraphicsItem *c = nodeItem(logical, "wp343_mixed_c");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    REQUIRE(a->data(FramePlanGroupIdRole).toString().isEmpty());
    REQUIRE(b->data(FramePlanGroupIdRole).toString().isEmpty());
    const QString fallback_group_id =
        c->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(fallback_group_id.isEmpty());
    REQUIRE(sceneGroupMembers(logical, fallback_group_id) ==
            StringSet{"wp343_mixed_c", "wp343_mixed_d"});
    REQUIRE(logical.collapseGroup(fallback_group_id));
    REQUIRE(strings(singleGroupItem(logical)->data(
                FramePlanMembersRole)) ==
            StringSet{"wp343_mixed_c", "wp343_mixed_d"});
    REQUIRE(namedShapesAndLabels(logical, "wp343_mixed_a").size() == 2);
    REQUIRE(namedShapesAndLabels(logical, "wp343_mixed_b").size() == 2);

    const auto warnings =
        itemsOfKind(logical, FramePlanGroupWarningItem);
    REQUIRE(warnings.size() == 1);
    const QString reason =
        warnings.front()->data(FramePlanReasonRole).toString();
    REQUIRE(reason.contains(QStringLiteral("wp343_mixed_b")));
    REQUIRE(reason.contains(QStringLiteral("wp343.mixed")));
    REQUIRE(reason.contains(QStringLiteral("wp343.second")));
    REQUIRE(visibleSceneTextContains(logical, reason));
}

TEST_CASE(
    "WP343 legacy prefix boundary is byte-exact on the production path",
    "[devstudio][frame-plan][grouping][region][legacy][boundary][wp343]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER);
    const Json &wire = legacyRegionBoundaryFramePlan();
    const FramePlanModel model = buildFramePlanModel(wire.dump());
    REQUIRE(model.physical_plan.loweringGraphAvailable());
    FramePlanGraphicsScene logical;
    logical.populate(model,
                     FramePlanNodeKey{model.graph, "swapchain"}, 64);

    for (const auto &[region, authored] : legacyRegionBoundaryCases()) {
        CAPTURE(region, authored);
        const StringSet members = wireRegionMembers(wire, region);
        REQUIRE(members.size() == 2);
        QString group_id;
        for (const auto &member : members) {
            QGraphicsItem *item = nodeItem(logical, member);
            REQUIRE(item != nullptr);
            const QString actual_id =
                item->data(FramePlanGroupIdRole).toString();
            if (authored) {
                REQUIRE_FALSE(actual_id.isEmpty());
                if (group_id.isEmpty()) {
                    group_id = actual_id;
                }
                REQUIRE(actual_id == group_id);
            } else {
                REQUIRE(actual_id.isEmpty());
            }
            REQUIRE(item->toolTip().contains(
                QStringLiteral("\"legacy.\"")));
            REQUIRE(item->toolTip().contains(
                QStringLiteral("ignored for grouping")));
        }
        if (authored) {
            REQUIRE(sceneGroupMembers(logical, group_id) == members);
        }
    }
}

TEST_CASE(
    "WP343 a node in multiple regions stays expanded with a visible reason",
    "[devstudio][frame-plan][grouping][region][overlap][wp343][negative-contrast]") {
    (void)application();
    // This overlap cannot be produced by a valid collapsed production scene,
    // so WP343 explicitly permits a hand-built model for this case only.
    const FramePlanModel model = overlappingRegionGroupingModel();
    REQUIRE(model.physical_plan.available());
    REQUIRE(model.physical_plan.graph == model.graph);
    REQUIRE(model.physical_plan.unavailable_reason_code.empty());
    REQUIRE(model.physical_plan.unavailable_reason.empty());
    REQUIRE(model.physical_plan.loweringGraphAvailable());
    FramePlanGraphicsScene logical;
    logical.populate(model, FramePlanNodeKey{model.graph, "focus"}, 1);

    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(sceneNodeNames(logical) ==
            StringSet{"alpha_peer", "beta_peer", "feature_peer",
                      "overlap"});
    for (const auto &name : sceneNodeNames(logical)) {
        REQUIRE(namedShapesAndLabels(logical, name).size() == 2);
        REQUIRE(nodeItem(logical, name)
                    ->data(FramePlanGroupIdRole)
                    .toString()
                    .isEmpty());
    }

    const auto warnings =
        itemsOfKind(logical, FramePlanGroupWarningItem);
    REQUIRE(warnings.size() == 1);
    const QString reason =
        warnings.front()->data(FramePlanReasonRole).toString();
    REQUIRE(reason.contains(QStringLiteral("multiple authored regions")));
    REQUIRE(reason.contains(QStringLiteral("wp343.alpha")));
    REQUIRE(reason.contains(QStringLiteral("wp343.beta")));
    REQUIRE(reason.contains(QStringLiteral("overlapping")));
    REQUIRE(visibleSceneTextContains(logical, reason));
    REQUIRE(logical.property("pelicanGroupFeedback").toString() ==
            reason);
}

TEST_CASE(
    "WP343 non-convex region reuses the WP341 rejection and visible reason",
    "[devstudio][frame-plan][grouping][region][convexity][wp343][negative-contrast]") {
    (void)application();
    // Arbitrary authored region membership can be non-convex, but the
    // production planner cannot emit this fixture reliably. The graph shape
    // is the same WP341 leave-and-return counterexample.
    const FramePlanModel model = nonConvexRegionGroupingModel();
    REQUIRE(model.physical_plan.available());
    REQUIRE(model.physical_plan.graph == model.graph);
    REQUIRE(model.physical_plan.unavailable_reason_code.empty());
    REQUIRE(model.physical_plan.unavailable_reason.empty());
    REQUIRE(model.physical_plan.loweringGraphAvailable());
    FramePlanGraphicsScene logical;
    logical.populate(model, FramePlanNodeKey{model.graph, "focus"}, 1);

    QGraphicsItem *inside_a = nodeItem(logical, "inside_a");
    QGraphicsItem *inside_b = nodeItem(logical, "inside_b");
    REQUIRE(inside_a != nullptr);
    REQUIRE(inside_b != nullptr);
    const QString group_id =
        inside_a->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(group_id.isEmpty());
    REQUIRE(inside_b->data(FramePlanGroupIdRole).toString() ==
            group_id);
    REQUIRE_FALSE(
        inside_a->data(FramePlanGroupCollapsibleRole).toBool());
    REQUIRE_FALSE(logical.collapseGroup(group_id));
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(namedShapesAndLabels(logical, "inside_a").size() == 2);
    REQUIRE(namedShapesAndLabels(logical, "inside_b").size() == 2);

    const auto warnings =
        itemsOfKind(logical, FramePlanGroupWarningItem);
    REQUIRE(warnings.size() == 1);
    const QString reason =
        warnings.front()->data(FramePlanReasonRole).toString();
    REQUIRE(reason.contains(
        QStringLiteral("Region \"wp343.non_convex\"")));
    REQUIRE(reason.contains(QStringLiteral("not convex")));
    REQUIRE(reason.contains(QStringLiteral("outside")));
    REQUIRE(visibleSceneTextContains(logical, reason));
}

TEST_CASE(
    "WP347 production edges distinguish barriers order-only dependencies and fused absorption",
    "[devstudio][frame-plan][barrier-visibility][grouping][wp347][negative-contrast]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER);
    const Json &wire = wp347BarrierFramePlan();
    const FramePlanModel model = buildFramePlanModel(wire.dump());
    const StringSet members{std::string{Wp347Producer},
                            std::string{Wp347Consumer}};
    REQUIRE(model.execution_plan.available());
    REQUIRE(model.physical_plan.available());
    REQUIRE(model.physical_plan.loweringGraphAvailable());
    REQUIRE(wireRegionMembers(wire, Wp347Region) == members);
    REQUIRE_FALSE(model.barriers.empty());

    const auto consumer = std::ranges::find(
        model.nodes, std::string{Wp347Consumer}, &FramePlanNode::name);
    REQUIRE(consumer != model.nodes.end());
    const auto tile_use = std::ranges::find(
        consumer->resource_uses, std::string{Wp347Tile},
        &FramePlanResourceUse::resource);
    REQUIRE(tile_use != consumer->resource_uses.end());
    REQUIRE(tile_use->footprint == "same_pixel");

    const auto same_pixel_attachment_barrier = std::ranges::find_if(
        model.barriers, [&](const FramePlanBarrier &barrier) {
            const auto destination = std::ranges::find(
                model.nodes, barrier.to, &FramePlanNode::name);
            return destination != model.nodes.end() &&
                   std::ranges::any_of(
                       destination->resource_uses,
                       [&](const FramePlanResourceUse &use) {
                           return use.resource == barrier.resource &&
                                  use.footprint == "same_pixel" &&
                                  use.intent == "attachment";
                       });
        });
    REQUIRE(same_pixel_attachment_barrier != model.barriers.end());

    const auto fused_scope = std::ranges::find_if(
        model.physical_plan.scopes,
        [&](const FramePlanPhysicalScope &scope) {
            return std::ranges::includes(
                       StringSet{scope.nodes.begin(), scope.nodes.end()},
                       members) &&
                   std::ranges::find(scope.local_reads,
                                     std::string{Wp347Tile}) !=
                       scope.local_reads.end();
        });
    REQUIRE(fused_scope != model.physical_plan.scopes.end());

    const auto internal_barrier = [&](const FramePlanBarrier &barrier) {
        return members.contains(barrier.from) &&
               members.contains(barrier.to);
    };
    const std::size_t internal_barrier_count =
        static_cast<std::size_t>(
            std::ranges::count_if(model.barriers, internal_barrier));
    REQUIRE(internal_barrier_count > 0);
    const std::size_t internal_fused_count =
        static_cast<std::size_t>(std::ranges::count_if(
            model.barriers, [&](const FramePlanBarrier &barrier) {
                return internal_barrier(barrier) &&
                       std::ranges::find(fused_scope->local_reads,
                                         barrier.resource) !=
                           fused_scope->local_reads.end();
            }));
    REQUIRE(internal_fused_count > 0);

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.resize(1200, 800);
    widget.show();
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    targetSelector(widget).setCurrentText(
        QString::fromStdString(std::string{Wp347Output}));
    subtreeDepth(widget).setValue(64);
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    auto *logical_scene = dynamic_cast<FramePlanGraphicsScene *>(&logical);
    REQUIRE(logical_scene != nullptr);

    for (const auto &member : members) {
        REQUIRE(namedShapesAndLabels(logical, member).size() == 2);
    }

    // These searches are deliberately independent of FramePlanItemKindRole.
    // A mutation that merely renames or clears an edge item's kind still has
    // to preserve the actual shape and visible label distinction.
    const auto barrier_items = edgeShapesAndLabels(
        logical, Wp347Producer, Wp347Consumer);
    const auto order_items = edgeShapesAndLabels(
        logical, Wp347Producer, Wp347Ordered);
    REQUIRE_FALSE(barrier_items.empty());
    REQUIRE_FALSE(order_items.empty());
    const QString barrier_text = edgeVisibleText(
        logical, Wp347Producer, Wp347Consumer);
    const QString order_text = edgeVisibleText(
        logical, Wp347Producer, Wp347Ordered);
    REQUIRE(barrier_text != order_text);
    REQUIRE(barrier_text.contains(QStringLiteral("barrier")));
    for (const auto &barrier : model.barriers) {
        if (barrier.from == Wp347Producer &&
            barrier.to == Wp347Consumer) {
            REQUIRE(barrier_text.contains(
                QString::fromStdString(barrier.kind)));
        }
    }
    REQUIRE(barrier_text.contains(
        QStringLiteral("absorbed in fused scope")));
    REQUIRE_FALSE(barrier_text.contains(QStringLiteral("stage")));
    REQUIRE_FALSE(barrier_text.contains(QStringLiteral("layout")));
    REQUIRE(order_text.contains(
        QStringLiteral("order only: explicit_after")));
    REQUIRE_FALSE(order_text.contains(QStringLiteral("barrier")));

    const auto by_region_items = edgeShapesAndLabels(
        logical, same_pixel_attachment_barrier->from,
        same_pixel_attachment_barrier->to);
    REQUIRE_FALSE(by_region_items.empty());
    const QString by_region_text = edgeVisibleText(
        logical, same_pixel_attachment_barrier->from,
        same_pixel_attachment_barrier->to);
    REQUIRE(by_region_text.contains(QStringLiteral("barrier")));
    REQUIRE(by_region_text.contains(
        QStringLiteral("same-pixel attachment")));
    REQUIRE_FALSE(by_region_text.contains(QStringLiteral("stage")));
    REQUIRE_FALSE(by_region_text.contains(QStringLiteral("layout")));

    QGraphicsItem *member = nodeItem(logical, Wp347Producer);
    REQUIRE(member != nullptr);
    const QString group_id =
        member->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(group_id.isEmpty());
    REQUIRE(logical_scene->collapseGroup(group_id));

    for (const auto &name : members) {
        REQUIRE(namedShapesAndLabels(logical, name).empty());
    }
    REQUIRE(edgeShapesAndLabels(logical, Wp347Producer,
                                Wp347Consumer)
                .empty());
    QGraphicsItem *group = singleGroupItem(logical);
    REQUIRE(group->data(FramePlanInternalBarrierCountRole).toULongLong() ==
            internal_barrier_count);
    REQUIRE(group->data(FramePlanInternalFusedBarrierCountRole)
                .toULongLong() == internal_fused_count);
    REQUIRE(logical.property("pelicanInternalBarrierRecordCount")
                .toULongLong() == internal_barrier_count);
    REQUIRE(visibleSceneTextContains(
        logical,
        QStringLiteral("internal barriers: %1")
            .arg(static_cast<qulonglong>(internal_barrier_count))));
    REQUIRE(visibleSceneTextContains(
        logical, QStringLiteral("absorbed in fused scope: %1")
                     .arg(static_cast<qulonglong>(internal_fused_count))));
}

TEST_CASE(
    "WP347 production subtree reports outside unselected and unavailable barriers",
    "[devstudio][frame-plan][barrier-visibility][window][wp347][negative-contrast]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER);
    const Json &wire = wp347BarrierFramePlan();
    const FramePlanModel model = buildFramePlanModel(wire.dump());
    REQUIRE(model.execution_plan.available());
    REQUIRE_FALSE(model.barriers.empty());

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.resize(1200, 800);
    widget.show();
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    QComboBox &selector = targetSelector(widget);
    QSpinBox &depth = subtreeDepth(widget);
    selector.setCurrentText(
        QString::fromStdString(std::string{Wp347Output}));
    depth.setValue(1);
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);

    const StringSet narrow_nodes = sceneNodeNames(logical);
    const std::size_t expected_outside =
        static_cast<std::size_t>(std::ranges::count_if(
            model.barriers, [&](const FramePlanBarrier &barrier) {
                return !narrow_nodes.contains(barrier.from) ||
                       !narrow_nodes.contains(barrier.to);
            }));
    REQUIRE(expected_outside > 0);
    REQUIRE(logical.property("pelicanBarrierRecordCount").toULongLong() ==
            model.barriers.size());
    REQUIRE(logical.property("pelicanOutsideBarrierRecordCount")
                .toULongLong() == expected_outside);
    const QString outside_phrase =
        QStringLiteral("%1 outside the current window")
            .arg(static_cast<qulonglong>(expected_outside));
    REQUIRE(logical.property("pelicanBarrierCoverage")
                .toString()
                .contains(outside_phrase));
    REQUIRE(framePlanStatus(widget).text().contains(outside_phrase));

    selector.setCurrentIndex(-1);
    QApplication::processEvents();
    REQUIRE(sceneNodeNames(logical).empty());
    for (const auto &node : model.nodes) {
        REQUIRE(namedShapesAndLabels(logical, node.name).empty());
    }
    REQUIRE(model.barriers == buildFramePlanModel(wire.dump()).barriers);
    REQUIRE(logical.property("pelicanBarrierRecordCount").toULongLong() ==
            model.barriers.size());
    REQUIRE(logical.property("pelicanVisibleBarrierRecordCount")
                .toULongLong() == 0);
    REQUIRE(logical.property("pelicanOutsideBarrierRecordCount")
                .toULongLong() == model.barriers.size());
    const QString no_target_phrase =
        QStringLiteral("%1 outside the current window (no target selected)")
            .arg(static_cast<qulonglong>(model.barriers.size()));
    REQUIRE(logical.property("pelicanBarrierCoverage")
                .toString()
                .contains(no_target_phrase));
    REQUIRE(framePlanStatus(widget).text().contains(no_target_phrase));

    Json missing_execution = wire;
    missing_execution.erase("execution_plan");
    const FramePlanModel unavailable_model =
        buildFramePlanModel(missing_execution.dump());
    REQUIRE_FALSE(unavailable_model.execution_plan.available());
    REQUIRE(unavailable_model.dependencies.empty());
    REQUIRE(unavailable_model.barriers == model.barriers);
    widget.receiveResult(
        QByteArray::fromStdString(missing_execution.dump()));
    QApplication::processEvents();

    REQUIRE(logical.property("pelicanExecutionPlanState").toString() ==
            QStringLiteral("unavailable"));
    REQUIRE(logical.property("pelicanVisibleBarrierRecordCount")
                .toULongLong() == 0);
    REQUIRE(itemsOfKind(logical, FramePlanEdgeItem).empty());
    const QString unavailable_phrase =
        QStringLiteral(
            "%1 cannot be placed because the execution plan is unavailable")
            .arg(static_cast<qulonglong>(model.barriers.size()));
    REQUIRE(logical.property("pelicanBarrierCoverage")
                .toString()
                .contains(unavailable_phrase));
    REQUIRE(visibleSceneTextContains(logical, unavailable_phrase));
    REQUIRE(framePlanStatus(widget).text().contains(unavailable_phrase));
}

TEST_CASE(
    "WP344 shipped planar prefilter region collapses and opens through the production path",
    "[devstudio][frame-plan][grouping][region][planar-reflection][wp344]") {
#if !PELICAN_RUNTIME_SHADER_COMPILER
    SKIP("WP344 production composition requires the runtime shader compiler");
#elif !PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
    SKIP("WP344 planar_reflection is registered only with standard render algorithms");
#else
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER);
    const StringSet members{
        "planar_reflection_filter_mip_1",
        "planar_reflection_filter_mip_2",
        "planar_reflection_filter_mip_3",
        "planar_reflection_filter_mip_4",
        "planar_reflection_filter_mip_5",
        "planar_reflection_filter_mip_6",
    };

    // The shipped feature reference is composed before resolve, compile,
    // frame planning, execution wiring, and physical target planning. Studio
    // then parses that wire document; no FramePlanModel is assembled by hand.
    const Json &wire = planarReflectionGroupingFramePlan();
    const StringSet actual_members =
        wireRegionMembers(wire, "planar_reflection.prefilter");
    const FramePlanModel model = buildFramePlanModel(wire.dump());
    std::vector<std::string> observed_member_orders;
    for (const auto &node : model.nodes) {
        if (actual_members.contains(node.name)) {
            observed_member_orders.push_back(
                node.name + "=" + std::to_string(node.order));
        }
    }
    INFO("actual planar prefilter orders: "
         << joinedValues(observed_member_orders).toStdString());
    INFO(model.physical_plan.unavailable_reason);
    REQUIRE(model.physical_plan.state ==
            FramePlanPhysicalPlanState::available);
    REQUIRE(model.physical_plan.loweringGraphAvailable());

    FramePlanGraphicsScene logical;
    logical.populate(model,
                     FramePlanNodeKey{model.graph, "swapchain"}, 64);
    QGraphicsItem *collapse_member =
        nodeItem(logical, "planar_reflection_filter_mip_1");
    REQUIRE(collapse_member != nullptr);
    const QString group_id =
        collapse_member->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(group_id.isEmpty());
    REQUIRE(sceneGroupMembers(logical, group_id) == actual_members);
    const bool region_is_collapsible =
        collapse_member->data(FramePlanGroupCollapsibleRole).toBool();
    INFO("actual planar prefilter region member count: "
         << actual_members.size());
    INFO("actual planar prefilter region is convex/collapsible: "
         << (region_is_collapsible ? "true" : "false"));
    REQUIRE(region_is_collapsible);
    REQUIRE(actual_members == members);
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());

    REQUIRE(logical.collapseGroup(group_id));
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).size() == 1);
    REQUIRE(itemsOfKind(logical, FramePlanGroupLabelItem).size() == 1);
    for (const auto &member : members) {
        // Kind-independent: a member shape or label with a blank/different
        // FramePlanItemKindRole still fails this assertion.
        REQUIRE(namedShapesAndLabels(logical, member).empty());
    }

    QGraphicsItem *group = singleGroupItem(logical);
    REQUIRE(strings(group->data(FramePlanMembersRole)) == members);
    const std::string group_name =
        group->data(FramePlanNameRole).toString().toStdString();
    REQUIRE(group_name ==
            "__pelican_group__:region:planar_reflection.prefilter");

    std::size_t entering_edges = 0;
    std::size_t leaving_edges = 0;
    std::vector<std::string> entering_sources;
    std::vector<std::string> leaving_targets;
    for (QGraphicsItem *edge :
         itemsOfKind(logical, FramePlanEdgeItem)) {
        const std::string from =
            edge->data(FramePlanFromNameRole).toString().toStdString();
        const std::string to =
            edge->data(FramePlanToNameRole).toString().toStdString();
        entering_edges += static_cast<std::size_t>(to == group_name);
        leaving_edges += static_cast<std::size_t>(from == group_name);
        if (to == group_name) {
            entering_sources.push_back(from);
        }
        if (from == group_name) {
            leaving_targets.push_back(
                to + " [" +
                edge->data(FramePlanEdgeRecordsRole)
                    .toStringList()
                    .join(QStringLiteral(", "))
                    .toStdString() +
                "]");
        }
    }
    INFO("collapsed planar prefilter entering sources: "
         << joinedValues(entering_sources).toStdString());
    INFO("collapsed planar prefilter leaving targets: "
         << joinedValues(leaving_targets).toStdString());
    REQUIRE(entering_edges == 1);
    REQUIRE(leaving_edges == 2);
    REQUIRE(edgeItem(logical,
                     "planar_reflection_forward_transparent",
                     group_name) != nullptr);
    REQUIRE(edgeItem(logical, group_name,
                     "forward_transparent") != nullptr);
    // In production composition, featurecompose.cpp:458 adds the output_transform terminal dependency.
    REQUIRE(edgeItem(logical, group_name,
                     "output_transform") != nullptr);

    REQUIRE(logical.enterGroup(group_id));
    REQUIRE(sceneNodeNames(logical) == members);
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(boundaryTargets(logical) ==
            StringSet{"forward_transparent",
                      "output_transform",
                      "planar_reflection_forward_transparent"});
    REQUIRE(itemsOfKind(logical, FramePlanBoundaryStubItem).size() == 3);
    REQUIRE(logical.property("pelicanBoundaryStubCount").toULongLong() ==
            3);
#endif
}

TEST_CASE(
    "WP348 cube capture measures the non-degenerate view-family loop on the production path",
    "[devstudio][frame-plan][view-family][grouping][wp348]") {
#if !PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
    SKIP("WP348 cube_capture execution is supplied by the standard render algorithms package");
#elif !PELICAN_RUNTIME_SHADER_COMPILER
    SKIP("WP348 cube_capture declares the runtime shader compiler as required");
#else
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER,
            PELICAN_WITH_STANDARD_RENDER_ALGORITHMS);
    const Wp348CubeCaptureMeasurement &measurement =
        wp348CubeCaptureMeasurement();
    const FramePlanModel model =
        buildFramePlanModel(measurement.wire.dump());
    const StringSet members =
        wireRegionMembers(measurement.wire, Wp348CubeRegion);
    const StringSet expected_members{
        "cube_capture_geometry",
        "cube_capture_ssao",
        "cube_capture_ssao_blur",
        "cube_capture_lighting",
        "cube_capture_forward_opaque",
        "cube_capture_snapshot_opaque_color",
        "cube_capture_snapshot_opaque_depth",
        "cube_capture_forward_transparent",
    };
    REQUIRE(model.execution_plan.available());
    REQUIRE(model.physical_plan.available());
    REQUIRE(model.physical_plan.loweringGraphAvailable());
    REQUIRE(members == expected_members);

    std::vector<std::string> cube_invocations;
    for (const auto &invocation : measurement.invocations) {
        if (invocation.view_family ==
            Pelican::cubeCaptureRenderViewFamilyId) {
            cube_invocations.push_back(
                describeWp348Invocation(measurement, invocation));
        }
    }
    const std::array<std::string_view, 8> expected_node_order{
        "cube_capture_geometry",
        "cube_capture_ssao",
        "cube_capture_ssao_blur",
        "cube_capture_lighting",
        "cube_capture_forward_opaque",
        "cube_capture_snapshot_opaque_color",
        "cube_capture_snapshot_opaque_depth",
        "cube_capture_forward_transparent",
    };
    const std::array<std::string_view, 6> expected_view_order{
        "$face/+x", "$face/-x", "$face/+y",
        "$face/-y", "$face/+z", "$face/-z",
    };
    const std::vector<std::string> expected_view_ids{
        expected_view_order.begin(), expected_view_order.end()};
    REQUIRE(measurement.view_ids == expected_view_ids);

    // These literals are the observed 48-column schedule, not a scheduler
    // oracle. A node-major/view-major reversal, scope fusion, or different
    // node_index now changes at least one complete invocation record.
    std::vector<std::string> expected_invocations;
    expected_invocations.reserve(48);
    for (std::size_t node_index = 0;
         node_index < expected_node_order.size(); ++node_index) {
        for (std::size_t view_index = 0;
             view_index < expected_view_order.size(); ++view_index) {
            expected_invocations.push_back(
                std::string{expected_node_order[node_index]} +
                "[node=" + std::to_string(node_index) +
                ",scope=" + std::to_string(node_index) +
                ",scope_node=0/1,view=" +
                std::to_string(view_index) + ":" +
                std::string{expected_view_order[view_index]} +
                ",execution=" + std::to_string(view_index) +
                "/6]");
        }
    }
    INFO("WP348 measured invocations: "
         << joinedValues(cube_invocations).toStdString());
    REQUIRE(cube_invocations.size() == 48);
    REQUIRE(cube_invocations == expected_invocations);

    std::vector<std::string> cube_resources;
    for (const auto &resource : model.resources) {
        if (resource.name.starts_with("cube_capture_")) {
            cube_resources.push_back(
                resource.name + "[layers=" +
                std::to_string(resource.array_layers) + ",layout=" +
                resource.view_layout + "]");
        }
    }
    const std::vector<std::string> expected_cube_resources{
        "cube_capture_albedo[layers=6,layout=sequential_2d]",
        "cube_capture_ao[layers=6,layout=sequential_2d]",
        "cube_capture_ao_blur[layers=6,layout=sequential_2d]",
        "cube_capture_color[layers=6,layout=sequential_2d]",
        "cube_capture_depth[layers=6,layout=sequential_2d]",
        "cube_capture_emissive[layers=6,layout=sequential_2d]",
        "cube_capture_material[layers=6,layout=sequential_2d]",
        "cube_capture_normal[layers=6,layout=sequential_2d]",
        "cube_capture_opaque_color[layers=6,layout=sequential_2d]",
        "cube_capture_opaque_depth[layers=6,layout=sequential_2d]",
        "cube_capture_worldpos[layers=6,layout=sequential_2d]",
    };
    INFO("WP348 measured physical resources: "
         << joinedValues(cube_resources).toStdString());
    REQUIRE(cube_resources == expected_cube_resources);

    const auto repeated_node_barrier = [&members](const auto &record) {
        return record.from == record.to &&
               members.contains(record.from);
    };
    const std::size_t repeated_node_barriers =
        static_cast<std::size_t>(std::ranges::count_if(
            model.barriers, repeated_node_barrier));
    const Json &wire_dependencies =
        measurement.wire.at("execution_plan").at("dependencies");
    const std::size_t repeated_node_dependencies =
        static_cast<std::size_t>(std::ranges::count_if(
            wire_dependencies, [&members](const Json &dependency) {
                const std::string from =
                    dependency.at("from").get<std::string>();
                return from ==
                           dependency.at("to").get<std::string>() &&
                       members.contains(from);
            }));
    INFO("WP348 measured same-node model.barriers: "
         << repeated_node_barriers);
    INFO("WP348 measured same-node execution_plan.dependencies: "
         << repeated_node_dependencies);
    REQUIRE(model.dependencies.size() == wire_dependencies.size());
    REQUIRE(repeated_node_barriers == 0);
    REQUIRE(repeated_node_dependencies == 0);

    std::vector<std::string> full_boundary_dependencies;
    StringSet full_entering_sources;
    StringSet full_leaving_targets;
    for (const auto &dependency : model.dependencies) {
        const bool from_inside = members.contains(dependency.from);
        const bool to_inside = members.contains(dependency.to);
        if (from_inside == to_inside) {
            continue;
        }
        full_boundary_dependencies.push_back(
            dependency.from + " -> " + dependency.to + " [" +
            dependency.reason + "," + dependency.resource + "]");
        if (to_inside) {
            full_entering_sources.insert(dependency.from);
        } else {
            full_leaving_targets.insert(dependency.to);
        }
    }
    INFO("WP348 measured full quotient boundary dependencies: "
         << joinedValues(full_boundary_dependencies).toStdString());
    REQUIRE(full_boundary_dependencies.size() == 9);
    REQUIRE(full_entering_sources.empty());
    REQUIRE(full_leaving_targets ==
            StringSet{"__anchor_sprite", "deferred_geometry"});

    FramePlanGraphicsScene logical;
    logical.populate(
        model,
        FramePlanNodeKey{model.graph, "cube_capture_color"}, 64);
    QGraphicsItem *member =
        nodeItem(logical, "cube_capture_geometry");
    REQUIRE(member != nullptr);
    const QString group_id =
        member->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(group_id.isEmpty());
    REQUIRE(sceneGroupMembers(logical, group_id) == members);
    const bool convex =
        member->data(FramePlanGroupCollapsibleRole).toBool();
    INFO("WP348 measured region convexity: "
         << (convex ? "true" : "false"));
    REQUIRE(convex);
    REQUIRE(logical.collapseGroup(group_id));

    QGraphicsItem *group = singleGroupItem(logical);
    REQUIRE(strings(group->data(FramePlanMembersRole)) == members);
    REQUIRE(group->data(FramePlanNameRole).toString() ==
            QStringLiteral(
                "__pelican_group__:region:wp348.cube_capture_loop"));
    for (const auto &name : members) {
        REQUIRE(namedShapesAndLabels(logical, name).empty());
    }
    // cube_capture_color's resource subtree contains only the region. The
    // full quotient has the two measured exits above, but both destinations
    // are outside this visible window, so WP347 draws no outer edge here.
    REQUIRE(itemsOfKind(logical, FramePlanEdgeItem).empty());

    REQUIRE(logical.enterGroup(group_id));
    REQUIRE(sceneNodeNames(logical) == members);
    REQUIRE(boundaryTargets(logical) ==
            StringSet{"__anchor_sprite", "deferred_geometry"});
    const auto boundary_stubs =
        itemsOfKind(logical, FramePlanBoundaryStubItem);
    REQUIRE(boundary_stubs.size() == 2);
    REQUIRE(logical.property("pelicanBoundaryStubCount").toULongLong() ==
            2);
    for (QGraphicsItem *stub : boundary_stubs) {
        REQUIRE(stub->data(FramePlanBoundaryDirectionRole).toString() ==
                QStringLiteral("outgoing"));
    }
#endif
}

TEST_CASE(
    "WP341 composed TAA group collapses to a convex quotient and supports both scoped exits",
    "[devstudio][frame-plan][grouping][wp341][negative-contrast]") {
    (void)application();
    const Json &wire = taaGroupingFramePlan();
    const StringSet members{"taa_composite", "taa_resolve"};
    REQUIRE(wireFeatureMembers(wire, "taa") == members);
    const FramePlanModel model = buildFramePlanModel(wire.dump());

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.resize(1100, 760);
    widget.show();
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    targetSelector(widget).setCurrentText(QStringLiteral("lit_color"));
    subtreeDepth(widget).setValue(64);
    QApplication::processEvents();
    auto *logical_scene =
        dynamic_cast<FramePlanGraphicsScene *>(&scene(widget));
    REQUIRE(logical_scene != nullptr);
    FramePlanGraphicsScene &logical = *logical_scene;

    // Required negative control: the exact same scene starts expanded.
    const StringSet expanded_node_names = sceneNodeNames(logical);
    const EdgeSet expanded_edges = sceneEdges(logical);
    const StringSet expanded_records = sceneDependencyRecords(logical);
    const auto expanded_positions = sceneNodePositions(logical);
    const SceneItemSet expanded_items = annotatedSceneItems(logical);
    std::size_t expected_internal_record_count = 0;
    for (QGraphicsItem *edge :
         itemsOfKind(logical, FramePlanEdgeItem)) {
        const std::string from =
            edge->data(FramePlanFromNameRole).toString().toStdString();
        const std::string to =
            edge->data(FramePlanToNameRole).toString().toStdString();
        if (members.contains(from) && members.contains(to)) {
            expected_internal_record_count += static_cast<std::size_t>(
                edge->data(FramePlanEdgeRecordsRole).toStringList().size());
        }
    }
    REQUIRE(expected_internal_record_count > 0);
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(itemsOfKind(logical, FramePlanGroupLabelItem).empty());
    REQUIRE(std::ranges::includes(expanded_node_names, members));
    for (const auto &member : members) {
        const auto actual_items = namedShapesAndLabels(logical, member);
        REQUIRE(actual_items.size() == 2);
        REQUIRE(std::ranges::any_of(actual_items, [](QGraphicsItem *item) {
            return dynamic_cast<QGraphicsPathItem *>(item) != nullptr;
        }));
        REQUIRE(std::ranges::any_of(actual_items, [](QGraphicsItem *item) {
            return dynamic_cast<QGraphicsSimpleTextItem *>(item) != nullptr;
        }));
    }
    QGraphicsItem *collapse_member =
        nodeItem(logical, "taa_resolve");
    REQUIRE(collapse_member != nullptr);
    const QString group_id =
        collapse_member->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(group_id.isEmpty());
    REQUIRE(collapse_member
                ->data(FramePlanGroupCollapsibleRole)
                .toBool());
    collapse_member->setSelected(true);
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanSelectedNode").toString() ==
            QStringLiteral("taa_resolve"));

    // Exercise the user-facing context-menu entry, not only the state method.
    triggerContextAction(logical, *collapse_member,
                         QStringLiteral("Collapse"));

    // Positive half of the same negative-control test: every member item is
    // gone and exactly one group item has replaced all of them.
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).size() == 1);
    REQUIRE(itemsOfKind(logical, FramePlanGroupLabelItem).size() == 1);
    for (const auto &member : members) {
        // Deliberately independent of FramePlanItemKindRole: a ghost member
        // shape or label with an empty/different kind is still a failure.
        REQUIRE(namedShapesAndLabels(logical, member).empty());
    }
    QGraphicsItem *group = singleGroupItem(logical);
    REQUIRE(group->data(FramePlanGroupIdRole).toString() == group_id);
    REQUIRE(strings(group->data(FramePlanMembersRole)) == members);
    REQUIRE(logical.property("pelicanCollapsedGroups").toStringList() ==
            QStringList{group_id});
    REQUIRE(logical.selectedItems().empty());
    REQUIRE_FALSE(logical.selectedNode().has_value());
    REQUIRE(logical.property("pelicanSelectedNode").toString().isEmpty());
    REQUIRE(logical.property("pelicanSelectedGraph").toString().isEmpty());

    const std::string group_name =
        group->data(FramePlanNameRole).toString().toStdString();
    REQUIRE(group_name == "__pelican_group__:taa");
    REQUIRE_FALSE(expanded_node_names.contains(group_name));
    REQUIRE(annotatedSceneItems(logical) ==
            quotientSceneItems(expanded_items, members, model.graph,
                               group_name));

    qreal minimum_member_column = std::numeric_limits<qreal>::max();
    for (const auto &member : members) {
        minimum_member_column =
            std::min(minimum_member_column,
                     expanded_positions.at(member).x());
    }
    REQUIRE(group->scenePos().x() == minimum_member_column);

    const auto quotient_name = [&](const std::string &name) {
        return members.contains(name) ? group_name : name;
    };
    EdgeSet expected_quotient_edges;
    for (const auto &[from, to] : expanded_edges) {
        const std::string mapped_from = quotient_name(from);
        const std::string mapped_to = quotient_name(to);
        if (mapped_from != mapped_to) {
            expected_quotient_edges.emplace(mapped_from, mapped_to);
        }
    }
    REQUIRE(sceneEdges(logical) == expected_quotient_edges);

    const EdgeSet expanded_reachability =
        transitiveReachability(expanded_node_names, expanded_edges);
    EdgeSet expected_quotient_reachability;
    for (const auto &[from, to] : expanded_reachability) {
        const std::string mapped_from = quotient_name(from);
        const std::string mapped_to = quotient_name(to);
        if (mapped_from != mapped_to) {
            expected_quotient_reachability.emplace(mapped_from,
                                                   mapped_to);
        }
    }
    StringSet quotient_nodes;
    for (const auto &name : expanded_node_names) {
        quotient_nodes.insert(quotient_name(name));
    }
    REQUIRE(transitiveReachability(quotient_nodes, sceneEdges(logical)) ==
            expected_quotient_reachability);

    // Internal records are retained on the group but create no edge.
    REQUIRE(static_cast<std::size_t>(
                group->data(FramePlanInternalEdgeRecordsRole)
                    .toStringList()
                    .size()) == expected_internal_record_count);
    REQUIRE(sceneDependencyRecords(logical) == expanded_records);
    for (QGraphicsItem *edge :
         itemsOfKind(logical, FramePlanEdgeItem)) {
        REQUIRE_FALSE(members.contains(
            edge->data(FramePlanFromNameRole).toString().toStdString()));
        REQUIRE_FALSE(members.contains(
            edge->data(FramePlanToNameRole).toString().toStdString()));
    }

    QGraphicsItem *parallel_bundle = nullptr;
    for (QGraphicsItem *edge :
         itemsOfKind(logical, FramePlanEdgeItem)) {
        if (edge->data(FramePlanEdgeRecordsRole).toStringList().size() > 1) {
            parallel_bundle = edge;
            break;
        }
    }
    REQUIRE(parallel_bundle != nullptr);
    const auto parallel_records =
        parallel_bundle->data(FramePlanEdgeRecordsRole).toStringList();
    const std::string parallel_from =
        parallel_bundle->data(FramePlanFromNameRole).toString().toStdString();
    const std::string parallel_to =
        parallel_bundle->data(FramePlanToNameRole).toString().toStdString();
    REQUIRE(edgeLabelText(logical, parallel_from, parallel_to)
                .contains(QStringLiteral("x%1").arg(
                    parallel_records.size())));

    const auto collapsed_outer_items = annotatedSceneItems(logical);
    const StringSet expected_boundary_targets =
        boundaryTargetsFor(model, members);
    REQUIRE_FALSE(expected_boundary_targets.empty());

    // Double-click enters the group. Only member nodes remain; every actual
    // external endpoint becomes one boundary stub.
    doubleClickSceneItem(logical, *group);
    REQUIRE(sceneNodeNames(logical) == members);
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(boundaryTargets(logical) == expected_boundary_targets);
    REQUIRE(itemsOfKind(logical, FramePlanBoundaryStubItem).size() ==
            expected_boundary_targets.size());
    REQUIRE(logical.property("pelicanBoundaryStubCount").toULongLong() ==
            expected_boundary_targets.size());
    REQUIRE(logical.property("pelicanCurrentGroupScope").toString() ==
            group_id);
    REQUIRE(itemsOfKind(logical, FramePlanBreadcrumbItem).size() == 1);

    // The breadcrumb is a real clickable scene control.
    QGraphicsView &view = logicalView(widget);
    QGraphicsItem *breadcrumb =
        itemsOfKind(logical, FramePlanBreadcrumbItem).front();
    view.centerOn(breadcrumb);
    QApplication::processEvents();
    QTest::mouseClick(
        view.viewport(), Qt::LeftButton, Qt::NoModifier,
        view.mapFromScene(breadcrumb->sceneBoundingRect().center()));
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanCurrentGroupScope")
                .toString()
                .isEmpty());
    REQUIRE(annotatedSceneItems(logical) == collapsed_outer_items);

    // Enter again, then exercise the independent Escape exit.
    group = singleGroupItem(logical);
    doubleClickSceneItem(logical, *group);
    REQUIRE(sceneNodeNames(logical) == members);
    QLineEdit &filter = framePlanFilter(widget);
    widget.raise();
    widget.activateWindow();
    filter.setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    REQUIRE(filter.hasFocus());
    auto *leave_action = widget.findChild<QAction *>(
        QStringLiteral("pelican.framePlanLeaveGroup"));
    REQUIRE(leave_action != nullptr);
    REQUIRE(leave_action->shortcutContext() ==
            Qt::WidgetWithChildrenShortcut);
    QTest::keyClick(&filter, Qt::Key_Escape);
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanCurrentGroupScope")
                .toString()
                .isEmpty());
    REQUIRE(annotatedSceneItems(logical) == collapsed_outer_items);

    // The context-menu expand operation restores the exact expanded graph.
    group = singleGroupItem(logical);
    triggerContextAction(logical, *group,
                         QStringLiteral("Expand"));
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(sceneNodeNames(logical) == expanded_node_names);
    REQUIRE(sceneEdges(logical) == expanded_edges);
    REQUIRE(annotatedSceneItems(logical) == expanded_items);
    REQUIRE(logical.property("pelicanCollapsedGroups")
                .toStringList()
                .isEmpty());
}

TEST_CASE(
    "WP341 collapsed state survives updates, drops orphans, and reset clears dragged positions",
    "[devstudio][frame-plan][grouping][state][drag][wp341]") {
    (void)application();
    const FramePlanModel model =
        buildFramePlanModel(taaGroupingFramePlan().dump());
    const FramePlanNodeKey target{model.graph, "lit_color"};

    FramePlanGraphicsScene logical;
    logical.populate(model, target, 64);
    QGraphicsItem *member = nodeItem(logical, "taa_resolve");
    REQUIRE(member != nullptr);
    const QString group_id =
        member->data(FramePlanGroupIdRole).toString();
    REQUIRE(logical.collapseGroup(group_id));
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).size() == 1);

    logical.populate(model, target, 64);
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).size() == 1);
    REQUIRE(logical.property("pelicanCollapsedGroups").toStringList() ==
            QStringList{group_id});

    FramePlanModel without_group = model;
    for (auto &node : without_group.nodes) {
        if (node.provider_feature == "taa") {
            node.provider_feature.clear();
            node.source = "project";
        }
    }
    logical.populate(without_group, target, 64);
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(logical.property("pelicanCollapsedGroups")
                .toStringList()
                .isEmpty());
    REQUIRE(nodeItem(logical, "taa_resolve") != nullptr);
    REQUIRE(nodeItem(logical, "taa_composite") != nullptr);

    FramePlanGraphicsScene positions;
    positions.populate(model, target, 64);
    QGraphicsItem *resolve = nodeItem(positions, "taa_resolve");
    REQUIRE(resolve != nullptr);
    const QPointF default_position = resolve->scenePos();
    resolve->setPos(default_position + QPointF{177.0, 93.0});
    REQUIRE(resolve->scenePos() != default_position);
    positions.resetGraph();
    positions.populate(model, target, 64);
    resolve = nodeItem(positions, "taa_resolve");
    REQUIRE(resolve != nullptr);
    REQUIRE(resolve->scenePos() == default_position);
}

TEST_CASE(
    "WP341 group state uses a structural graph and feature key",
    "[devstudio][frame-plan][grouping][state][collision][wp341a]") {
    (void)application();
    FramePlanModel first =
        buildFramePlanModel(taaGroupingFramePlan().dump());
    first.graph = "g";
    first.execution_plan.graph = first.graph;
    for (auto &node : first.nodes) {
        if (node.provider_feature == "taa") {
            node.provider_feature = "x\x1f" "feature:y";
        }
    }
    const FramePlanNodeKey first_target{first.graph, "taa_accum"};

    FramePlanGraphicsScene logical;
    logical.populate(first, first_target, 1);
    QGraphicsItem *first_member = nodeItem(logical, "taa_resolve");
    REQUIRE(first_member != nullptr);
    const QString first_id =
        first_member->data(FramePlanGroupIdRole).toString();
    REQUIRE(logical.collapseGroup(first_id));

    FramePlanModel second =
        buildFramePlanModel(taaGroupingFramePlan().dump());
    second.graph = "g\x1f" "feature:x";
    second.execution_plan.graph = second.graph;
    for (auto &node : second.nodes) {
        if (node.provider_feature == "taa") {
            node.provider_feature = "y";
        }
    }
    const FramePlanNodeKey second_target{second.graph, "taa_accum"};
    FramePlanGraphicsScene second_scene;
    second_scene.populate(second, second_target, 1);
    QGraphicsItem *second_member =
        nodeItem(second_scene, "taa_resolve");
    REQUIRE(second_member != nullptr);
    const QString second_id =
        second_member->data(FramePlanGroupIdRole).toString();
    REQUIRE(first_id != second_id);

    // Updating the same scene must not transfer the first structural state to
    // the old delimiter-colliding pair.
    logical.populate(second, second_target, 1);
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(nodeItem(logical, "taa_resolve") != nullptr);
    REQUIRE(logical.property("pelicanCollapsedGroups")
                .toStringList()
                .isEmpty());
}

TEST_CASE(
    "WP341 group drag position ignores display suffixes and node collisions",
    "[devstudio][frame-plan][grouping][drag][identity][wp341a]") {
    (void)application();
    const FramePlanModel original =
        buildFramePlanModel(taaGroupingFramePlan().dump());
    FramePlanModel colliding = original;
    const std::string group_base = "__pelican_group__:taa";
    renameNode(colliding, "lighting_pass", group_base);
    const FramePlanNodeKey target{original.graph, "lit_color"};

    FramePlanGraphicsScene logical;
    logical.populate(colliding, target, 64);
    QGraphicsItem *member = nodeItem(logical, "taa_resolve");
    REQUIRE(member != nullptr);
    REQUIRE(logical.collapseGroup(
        member->data(FramePlanGroupIdRole).toString()));
    QGraphicsItem *group = singleGroupItem(logical);
    REQUIRE(group->data(FramePlanNameRole).toString() ==
            QStringLiteral("__pelican_group__:taa#2"));
    QGraphicsItem *colliding_node = nodeItem(logical, group_base);
    REQUIRE(colliding_node != nullptr);

    const QPointF group_position =
        group->scenePos() + QPointF{211.0, 87.0};
    const QPointF user_position =
        colliding_node->scenePos() + QPointF{-133.0, 149.0};
    REQUIRE(group_position != user_position);
    group->setPos(group_position);
    colliding_node->setPos(user_position);
    QApplication::processEvents();

    logical.populate(original, target, 64);
    group = singleGroupItem(logical);
    REQUIRE(group->data(FramePlanNameRole).toString() ==
            QStringLiteral("__pelican_group__:taa"));
    REQUIRE(group->scenePos() == group_position);
    REQUIRE(group->scenePos() != user_position);
}

TEST_CASE(
    "WP341 non-convex provider feature stays expanded with an explicit UI reason",
    "[devstudio][frame-plan][grouping][convexity][wp341][negative-contrast]") {
    (void)application();
    const FramePlanModel model = nonConvexGroupingModel();
    const FramePlanNodeKey target{model.graph, "focus"};

    FramePlanGraphicsScene logical;
    logical.populate(model, target, 1);
    QGraphicsItem *inside_a = nodeItem(logical, "inside_a");
    QGraphicsItem *inside_b = nodeItem(logical, "inside_b");
    REQUIRE(inside_a != nullptr);
    REQUIRE(inside_b != nullptr);
    const QString group_id =
        inside_a->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(group_id.isEmpty());
    REQUIRE(inside_b->data(FramePlanGroupIdRole).toString() ==
            group_id);
    REQUIRE_FALSE(
        inside_a->data(FramePlanGroupCollapsibleRole).toBool());

    // Same source is not a grouping fallback, and an N=1 feature is not a
    // collapsible group.
    REQUIRE(nodeItem(logical, "outside")
                ->data(FramePlanGroupIdRole)
                .toString()
                .isEmpty());
    REQUIRE(nodeItem(logical, "project_peer")
                ->data(FramePlanGroupIdRole)
                .toString()
                .isEmpty());
    REQUIRE(nodeItem(logical, "single_feature_node")
                ->data(FramePlanGroupIdRole)
                .toString()
                .isEmpty());

    REQUIRE_FALSE(logical.collapseGroup(group_id));
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(nodeItem(logical, "inside_a") != nullptr);
    REQUIRE(nodeItem(logical, "inside_b") != nullptr);
    REQUIRE(logical.property("pelicanCollapsedGroups")
                .toStringList()
                .isEmpty());

    const auto warnings =
        itemsOfKind(logical, FramePlanGroupWarningItem);
    REQUIRE(warnings.size() == 1);
    const QString reason =
        warnings.front()->data(FramePlanReasonRole).toString();
    REQUIRE(reason.contains(QStringLiteral("not convex")));
    REQUIRE(reason.contains(QStringLiteral("outside")));
    REQUIRE(warnings.front()->isVisible());
    REQUIRE(warnings.front()->toolTip() == reason);
    REQUIRE(logical.property("pelicanGroupFeedback").toString() ==
            reason);
    REQUIRE(logical.property("pelicanNonCollapsibleGroups")
                .toStringList()
                .contains(group_id));
    bool visible_reason = false;
    for (QGraphicsItem *child : warnings.front()->childItems()) {
        if (const auto *text =
                dynamic_cast<QGraphicsSimpleTextItem *>(child)) {
            visible_reason = text->text() == reason;
        }
    }
    REQUIRE(visible_reason);

    // A group entered while convex must be evicted if an update adds a path
    // that leaves and re-enters it. The rejection reason remains visible in
    // the outer scope after the forced exit.
    FramePlanModel initially_convex = model;
    initially_convex.dependencies = {
        {"inside_a", "inside_b", "pelican.dependency.inside@1",
         "inside"},
    };
    FramePlanGraphicsScene scope_update;
    scope_update.populate(initially_convex, target, 1);
    QGraphicsItem *convex_member =
        nodeItem(scope_update, "inside_a");
    REQUIRE(convex_member != nullptr);
    const QString entered_group_id =
        convex_member->data(FramePlanGroupIdRole).toString();
    REQUIRE(entered_group_id == group_id);
    REQUIRE(scope_update.collapseGroup(entered_group_id));
    REQUIRE(scope_update.enterGroup(entered_group_id));
    REQUIRE(scope_update.property("pelicanCurrentGroupScope").toString() ==
            entered_group_id);

    scope_update.populate(model, target, 1);
    REQUIRE(scope_update.property("pelicanCurrentGroupScope")
                .toString()
                .isEmpty());
    REQUIRE(scope_update.property("pelicanCollapsedGroups")
                .toStringList()
                .isEmpty());
    REQUIRE(scope_update.property("pelicanGroupFeedback").toString() ==
            reason);
    REQUIRE(itemsOfKind(scope_update, FramePlanBreadcrumbItem).empty());
    REQUIRE(itemsOfKind(scope_update, FramePlanGroupWarningItem).size() ==
            1);
    REQUIRE(sceneNodeNames(scope_update) ==
            StringSet{"inside_a", "inside_b", "outside", "project_peer",
                      "single_feature_node"});
}

TEST_CASE(
    "WP341 removed layout constants and no longer writes empty internal-edge roles",
    "[devstudio][frame-plan][grouping][source-audit][wp341]") {
    const std::string source = readText(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "src" /
        "devstudio" / "view" / "frameplangraphics.cpp");
    REQUIRE(source.find("ColumnsPerRow") == std::string::npos);
    REQUIRE(source.find("WrapRowHeight") == std::string::npos);
    REQUIRE(source.find(
                "FramePlanInternalEdgeRecordsRole, QStringList{}") ==
            std::string::npos);
}

TEST_CASE(
    "Studio consumes producer resolved fractional extents and a non-display output source",
    "[devstudio][frame-plan][wp315][resolution][negative-contrast]") {
    const Json wire = fractionalScaleFramePlan();
    const auto runtime =
        Pelican::frameRuntimeResolutionWireFromJson(
            wire.at("runtime_resolution"));
    REQUIRE(runtime.render_source_resource == "scaled_scene");
    REQUIRE(runtime.output_source_resource == "custom_output");
    REQUIRE(runtime.output_source_resource != "display");
    REQUIRE((runtime.render_extent ==
             Pelican::ResolvedResourceExtent{7, 7}));
    REQUIRE((runtime.output_extent ==
             Pelican::ResolvedResourceExtent{10, 10}));

    const FramePlanModel model =
        buildFramePlanModel(wire.dump());
    REQUIRE(model.physical_plan.available());
    REQUIRE(model.physical_plan.output_width ==
            runtime.output_extent.width);
    REQUIRE(model.physical_plan.output_height ==
            runtime.output_extent.height);
    const auto scaled = std::ranges::find(
        model.resources, std::string{"scaled_scene"},
        &FramePlanResource::name);
    REQUIRE(scaled != model.resources.end());
    REQUIRE(scaled->width == runtime.render_extent.width);
    REQUIRE(scaled->height == runtime.render_extent.height);

    Json without_runtime_resolution = wire;
    without_runtime_resolution.erase("runtime_resolution");
    const FramePlanModel unavailable =
        buildFramePlanModel(without_runtime_resolution.dump());
    REQUIRE_FALSE(unavailable.physical_plan.available());
    REQUIRE(unavailable.physical_plan.unavailable_reason_code ==
            "physical_plan_runtime_resolution_missing");
    const auto unresolved = std::ranges::find(
        unavailable.resources, std::string{"scaled_scene"},
        &FramePlanResource::name);
    REQUIRE(unresolved != unavailable.resources.end());
    REQUIRE_FALSE(unresolved->width.has_value());
    REQUIRE_FALSE(unresolved->height.has_value());
}

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
    REQUIRE(valid_nodes.size() == 1);
    REQUIRE(logical.property("pelicanSubtreeDepth").toInt() == 1);
    REQUIRE(logical.property("pelicanVisibleItemCount").toULongLong() == 1);
    REQUIRE(itemsOfKind(logical, FramePlanEdgeItem).empty());
    for (const QGraphicsItem *node : valid_nodes) {
        REQUIRE(logical.sceneRect().contains(node->sceneBoundingRect()));
    }
}

TEST_CASE(
    "WP329 logical node selection shows model details and clearing selection removes them",
    "[devstudio][frame-plan][logical-graph][selection][wp329][negative-contrast]") {
    (void)application();
    const std::string captured = readText(PELICAN_TEST_FRAME_PLAN_FIXTURE);
    const FramePlanModel model = buildFramePlanModel(captured);
    REQUIRE_FALSE(model.nodes.empty());

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(captured));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    QTreeWidget &details = logicalDetails(widget);

    const auto visible_nodes = itemsOfKind(logical, FramePlanNodeItem);
    REQUIRE_FALSE(visible_nodes.empty());
    QGraphicsItem *selected_item = visible_nodes.front();
    const std::string selected_name =
        selected_item->data(FramePlanNameRole).toString().toStdString();
    const auto expected = std::ranges::find(
        model.nodes, selected_name, &FramePlanNode::name);
    REQUIRE(expected != model.nodes.end());

    selected_item->setSelected(true);
    QApplication::processEvents();
    REQUIRE(details.topLevelItemCount() == 1);
    QTreeWidgetItem *selected_details = details.topLevelItem(0);
    const QStringList selected_observation{
        selected_details->text(1), selected_details->text(2),
        selected_details->text(3), selected_details->text(4)};
    const QStringList expected_observation{
        QString::fromStdString(expected->name),
        QString::fromStdString(expected->kind), joinedValues(expected->reads),
        joinedValues(expected->writes)};
    REQUIRE(selected_observation == expected_observation);

    logical.clearSelection();
    QApplication::processEvents();
    REQUIRE(details.topLevelItemCount() == 1);
    QTreeWidgetItem *no_selection = details.topLevelItem(0);
    const QStringList no_selection_observation{
        no_selection->text(1), no_selection->text(2), no_selection->text(3),
        no_selection->text(4)};
    REQUIRE(no_selection_observation != selected_observation);
    REQUIRE_FALSE(no_selection->text(1).isEmpty());
    REQUIRE(no_selection->text(1) != QString::fromStdString(expected->name));
    REQUIRE(no_selection->text(2).isEmpty());
    REQUIRE(no_selection->text(3).isEmpty());
    REQUIRE(no_selection->text(4).isEmpty());
}

TEST_CASE(
    "WP318 every example target stays within five direct nodes and named depths are exact",
    "[devstudio][frame-plan][target-subtree][wp318]") {
    (void)application();
    Json wire = Json::parse(readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    QComboBox &selector = targetSelector(widget);
    QSpinBox &depth = subtreeDepth(widget);

    REQUIRE(selector.count() == 22);
    REQUIRE(depth.value() == 1);
    REQUIRE(logical.property("pelicanNodeRecordCount").toULongLong() == 30);

    for (int index = 0; index < selector.count(); ++index) {
        selector.setCurrentIndex(index);
        QApplication::processEvents();
        CAPTURE(selector.currentText().toStdString());
        REQUIRE(itemsOfKind(logical, FramePlanNodeItem).size() <= 5);
        REQUIRE(logical.property("pelicanVisibleItemCount").toULongLong() <=
                5);
        REQUIRE(logical.property("pelicanTarget").toString() ==
                selector.currentText());
        REQUIRE(itemsOfKind(logical, FramePlanResourceLifetimeItem).size() ==
                1);
    }

    const std::map<std::string, StringSet, std::less<>> direct{
        {"gbuffer_albedo", {"gbuffer_pass", "lighting_pass"}},
        {"ssao_blur", {"lighting_pass", "ssao_blur_pass"}},
        {"lit_color",
         {"FinalBloomComposite", "HighLuminanceExtraction",
          "__snapshot_opaque_color", "forward_transparent",
          "lighting_pass"}},
        {"Bloom_Threshold_RT",
         {"HighLuminanceExtraction", "HorizontalBlur_0"}},
        {"display",
         {"FinalBloomComposite", "output_transform", "pelican_ui"}},
    };
    for (const auto &[target, expected] : direct) {
        selector.setCurrentText(QString::fromStdString(target));
        QApplication::processEvents();
        REQUIRE(sceneNodeNames(logical) == expected);
    }

    selector.setCurrentText(QStringLiteral("Bloom_Threshold_RT"));
    const std::array expected_by_depth{
        StringSet{},
        StringSet{"HighLuminanceExtraction", "HorizontalBlur_0"},
        StringSet{"FinalBloomComposite", "HighLuminanceExtraction",
                  "HorizontalBlur_0", "UpsampleBlend_1", "VerticalBlur_0",
                  "__snapshot_opaque_color", "forward_transparent",
                  "lighting_pass"},
    };
    for (int value = 0; value <= 2; ++value) {
        depth.setValue(value);
        QApplication::processEvents();
        CAPTURE(value);
        REQUIRE(sceneNodeNames(logical) == expected_by_depth[value]);
        REQUIRE(logical.property("pelicanSubtreeDepth").toInt() == value);
    }
    REQUIRE(expected_by_depth[2].size() == 8);
    REQUIRE(expected_by_depth[2].size() < wire.at("nodes").size());
}

TEST_CASE(
    "WP318 subtree coordinates and bundle order ignore every input array order",
    "[devstudio][frame-plan][target-subtree][determinism][wp318]") {
    (void)application();
    Json wire = Json::parse(readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");
    FramePlanModel ordered = buildFramePlanModel(wire.dump());
    FramePlanModel reversed = ordered;
    std::reverse(reversed.nodes.begin(), reversed.nodes.end());
    std::reverse(reversed.dependencies.begin(), reversed.dependencies.end());
    std::reverse(reversed.resources.begin(), reversed.resources.end());
    std::reverse(reversed.physical_plan.alias_groups.begin(),
                 reversed.physical_plan.alias_groups.end());
    std::reverse(reversed.physical_plan.alias_candidates.begin(),
                 reversed.physical_plan.alias_candidates.end());
    for (auto &node : reversed.nodes) {
        std::reverse(node.reads.begin(), node.reads.end());
        std::reverse(node.writes.begin(), node.writes.end());
    }

    const FramePlanNodeKey target{ordered.graph, "Bloom_Threshold_RT"};
    FramePlanGraphicsScene first;
    FramePlanGraphicsScene second;
    first.populate(ordered, target, 2);
    second.populate(reversed, target, 2);
    REQUIRE(sceneNodePositions(first) == sceneNodePositions(second));
    REQUIRE(sceneBundles(first) == sceneBundles(second));
}

TEST_CASE(
    "WP318 moving a node reroutes its curve arrow and label and positions live only in one scene",
    "[devstudio][frame-plan][drag][curve][session][wp318]") {
    (void)application();
    Json wire = Json::parse(readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");
    const QByteArray captured = QByteArray::fromStdString(wire.dump());

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(captured);
    targetSelector(widget).setCurrentText(
        QStringLiteral("Bloom_Threshold_RT"));
    subtreeDepth(widget).setValue(2);
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);

    QGraphicsItem *node = nodeItem(logical, "HorizontalBlur_0");
    auto *edge = dynamic_cast<QGraphicsPathItem *>(
        edgeItem(logical, "HighLuminanceExtraction", "HorizontalBlur_0"));
    REQUIRE(node != nullptr);
    REQUIRE(edge != nullptr);
    REQUIRE(node->flags().testFlag(QGraphicsItem::ItemIsMovable));
    REQUIRE(node->flags().testFlag(QGraphicsItem::ItemSendsGeometryChanges));
    REQUIRE(edge->data(FramePlanCurveRole).toBool());
    bool has_curve = false;
    for (int index = 0; index < edge->path().elementCount(); ++index) {
        has_curve = has_curve ||
                    edge->path().elementAt(index).type ==
                        QPainterPath::CurveToElement;
    }
    REQUIRE(has_curve);

    QGraphicsItem *arrow = nullptr;
    QGraphicsItem *label = nullptr;
    for (QGraphicsItem *item : logical.items()) {
        if (item->data(FramePlanFromNameRole).toString() !=
                QStringLiteral("HighLuminanceExtraction") ||
            item->data(FramePlanToNameRole).toString() !=
                QStringLiteral("HorizontalBlur_0")) {
            continue;
        }
        if (kind(*item) == QLatin1String{FramePlanEdgeArrowItem}) {
            arrow = item;
        } else if (kind(*item) == QLatin1String{FramePlanEdgeLabelItem}) {
            label = item;
        }
    }
    auto *arrow_polygon = dynamic_cast<QGraphicsPolygonItem *>(arrow);
    REQUIRE(arrow_polygon != nullptr);
    REQUIRE(label != nullptr);
    QGraphicsItem *node_label = nodeLabelItem(logical, "HorizontalBlur_0");
    REQUIRE(node_label != nullptr);

    const QPointF default_position = node->pos();
    const QPainterPath old_path = edge->path();
    const QPolygonF old_arrow = arrow_polygon->polygon();
    const QPointF old_node_label = node_label->scenePos();
    const QPointF moved_position = default_position + QPointF{73.0, 81.0};
    node->setPos(moved_position);
    QApplication::processEvents();
    REQUIRE(edge->path() != old_path);
    REQUIRE(arrow_polygon->polygon() != old_arrow);
    REQUIRE(QLineF{arrow_polygon->mapToScene(arrow_polygon->polygon().first()),
                   edge->mapToScene(edge->path().currentPosition())}
                .length() < 0.01);
    REQUIRE(QLineF{label->sceneBoundingRect().center(),
                   edge->mapToScene(edge->path().pointAtPercent(0.5))}
                .length() < 0.01);
    REQUIRE(node_label->scenePos() != old_node_label);

    subtreeDepth(widget).setValue(0);
    subtreeDepth(widget).setValue(2);
    QApplication::processEvents();
    REQUIRE(nodeItem(logical, "HorizontalBlur_0")->pos() ==
            moved_position);

    EmbeddedViewport fresh_viewport;
    FramePlanWidget fresh{&fresh_viewport};
    fresh.receiveResult(captured);
    targetSelector(fresh).setCurrentText(
        QStringLiteral("Bloom_Threshold_RT"));
    subtreeDepth(fresh).setValue(2);
    QApplication::processEvents();
    REQUIRE(nodeItem(scene(fresh), "HorizontalBlur_0")->pos() ==
            default_position);
    REQUIRE(nodeItem(scene(fresh), "HorizontalBlur_0")->pos() !=
            moved_position);
}

TEST_CASE(
    "WP320 dragging through the real view keeps movement and scene bounds finite",
    "[devstudio][frame-plan][drag][mouse][wp320]") {
    (void)application();
    Json wire = Json::parse(readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.resize(520, 420);
    widget.show();
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    targetSelector(widget).setCurrentText(
        QStringLiteral("Bloom_Threshold_RT"));
    subtreeDepth(widget).setValue(2);
    QApplication::processEvents();

    QGraphicsScene &logical = scene(widget);
    QGraphicsView &view = logicalView(widget);
    const auto nodes = itemsOfKind(logical, FramePlanNodeItem);
    REQUIRE_FALSE(nodes.empty());
    QGraphicsItem *dragged = *std::ranges::max_element(
        nodes, {}, [](const QGraphicsItem *item) {
            return item->sceneBoundingRect().right();
        });
    REQUIRE(dragged != nullptr);
    REQUIRE(dragged->flags().testFlag(QGraphicsItem::ItemIsMovable));

    view.centerOn(dragged);
    QApplication::processEvents();
    QWidget *const viewport_widget = view.viewport();
    REQUIRE(viewport_widget != nullptr);
    REQUIRE(viewport_widget->isVisible());
    const QPoint press_position =
        view.mapFromScene(dragged->mapToScene(QPointF{18.0, 18.0}));
    const std::array move_offsets{
        QPoint{15, 10}, QPoint{30, 20}, QPoint{45, 30},
        QPoint{60, 40}, QPoint{75, 50}, QPoint{90, 60},
        QPoint{105, 70}, QPoint{120, 80},
    };
    REQUIRE(viewport_widget->rect().contains(press_position));
    REQUIRE(viewport_widget->rect().contains(press_position +
                                             move_offsets.back()));

    const auto finite_rect = [](const QRectF &rect) {
        return std::isfinite(rect.x()) && std::isfinite(rect.y()) &&
               std::isfinite(rect.width()) && std::isfinite(rect.height());
    };
    const QRectF before_scene_rect = logical.sceneRect();
    REQUIRE(finite_rect(before_scene_rect));
    const auto before_positions = sceneNodePositions(logical);
    const QPointF before_position = dragged->pos();
    const QPointF expected_delta =
        view.mapToScene(press_position + move_offsets.back()) -
        view.mapToScene(press_position);
    const std::string dragged_name =
        dragged->data(FramePlanNameRole).toString().toStdString();

    QTest::mousePress(viewport_widget, Qt::LeftButton, Qt::NoModifier,
                      press_position);
    for (const QPoint &offset : move_offsets) {
        QTest::mouseMove(viewport_widget, press_position + offset, 1);
    }
    QTest::mouseRelease(viewport_widget, Qt::LeftButton, Qt::NoModifier,
                        press_position + move_offsets.back());
    QApplication::processEvents();

    const QPointF actual_delta = dragged->pos() - before_position;
    REQUIRE(QLineF{actual_delta, expected_delta}.length() < 0.01);
    const auto after_positions = sceneNodePositions(logical);
    REQUIRE(after_positions.size() == before_positions.size());
    for (const auto &[name, position] : before_positions) {
        CAPTURE(name);
        if (name == dragged_name) {
            REQUIRE(after_positions.at(name) != position);
        } else {
            REQUIRE(after_positions.at(name) == position);
        }
    }

    const QRectF settled_scene_rect = logical.sceneRect();
    REQUIRE(finite_rect(settled_scene_rect));
    REQUIRE(settled_scene_rect.contains(dragged->sceneBoundingRect()));
    REQUIRE(settled_scene_rect ==
            logical.itemsBoundingRect().adjusted(-30.0, -30.0, 30.0, 30.0));
    for (int index = 0; index < 4; ++index) {
        QApplication::processEvents();
        REQUIRE(logical.sceneRect() == settled_scene_rect);
    }
}

TEST_CASE(
    "WP318 mouse anchored wheel zoom clamps with visible boundary feedback",
    "[devstudio][frame-plan][zoom][wp318]") {
    (void)application();
    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(
        readText(PELICAN_TEST_FRAME_PLAN_FIXTURE)));
    QApplication::processEvents();
    QGraphicsView &view = logicalView(widget);
    REQUIRE(view.transformationAnchor() == QGraphicsView::AnchorUnderMouse);

    const auto wheel = [&](int delta) {
        QWheelEvent event{QPointF{12.0, 12.0}, QPointF{12.0, 12.0},
                          QPoint{}, QPoint{0, delta}, Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false};
        QApplication::sendEvent(view.viewport(), &event);
    };
    for (int index = 0; index < 20; ++index) {
        wheel(120);
    }
    REQUIRE(std::abs(view.transform().m11() - 4.0) < 0.000001);
    REQUIRE(view.property("pelicanZoomBoundary").toString() ==
            QStringLiteral("maximum"));
    auto *zoom_status = widget.findChild<QLabel *>(
        QStringLiteral("pelican.framePlanZoomStatus"));
    REQUIRE(zoom_status != nullptr);
    REQUIRE(zoom_status->text().contains(QStringLiteral("maximum")));

    for (int index = 0; index < 40; ++index) {
        wheel(-120);
    }
    REQUIRE(std::abs(view.transform().m11() - 0.25) < 0.000001);
    REQUIRE(view.property("pelicanZoomBoundary").toString() ==
            QStringLiteral("minimum"));
    REQUIRE(zoom_status->text().contains(QStringLiteral("minimum")));
}

TEST_CASE(
    "WP318 selected target preserves every WP307 physical fact and scopes both alias outcomes",
    "[devstudio][frame-plan][physical-overlay][target][wp318]") {
    (void)application();
    Json wire = Json::parse(readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");
    const Json &physical = wire.at("physical_target_plan");
    const Json &published_resources = physical.at("resources");
    const std::string endpoint = selectedPlanningEndpoint(physical);

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    QComboBox &selector = targetSelector(widget);

    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QString::fromStdString(endpoint));
    REQUIRE(published_resources.size() == 22);

    std::size_t arbitrary_materialized_count = 0;
    std::size_t same_pixel_count = 0;
    for (const auto &published : published_resources) {
        const std::string name =
            published.at("logical_resource").get<std::string>();
        selector.setCurrentText(QString::fromStdString(name));
        QApplication::processEvents();
        QGraphicsItem *row = physicalResourceItem(logical, name);
        INFO("physical resource overlay: " << name);
        REQUIRE(row != nullptr);
        REQUIRE(itemsOfKind(logical, FramePlanResourceLifetimeItem).size() ==
                1);
        REQUIRE(logical.property("pelicanPhysicalResourceCount")
                    .toULongLong() == 1);
        REQUIRE(logical.property("pelicanSelectedTarget").toString() ==
                QString::fromStdString(name));
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
        REQUIRE(logical.sceneRect().contains(row->sceneBoundingRect()));

        QString visible_facts;
        for (QGraphicsItem *child : row->childItems()) {
            if (auto *text =
                    dynamic_cast<QGraphicsSimpleTextItem *>(child);
                text != nullptr && text->isVisible()) {
                visible_facts += text->text();
            }
        }
        REQUIRE(visible_facts.contains(QString::fromStdString(name)));
        REQUIRE(visible_facts.contains(QString::fromStdString(
            published.at("reason").get<std::string>())));
        REQUIRE(visible_facts.contains(QString::fromStdString(
            published.at("widest_read").get<std::string>())));
        REQUIRE(visible_facts.contains(QString::fromStdString(
            published.at("representation").get<std::string>())));
        REQUIRE(visible_facts.contains(
            published.at("aliasable").get<bool>()
                ? QStringLiteral("aliasable: yes")
                : QStringLiteral("aliasable: no")));
        REQUIRE(visible_facts.contains(expected_lifetime));
    }
    REQUIRE(arbitrary_materialized_count == 16);
    REQUIRE(same_pixel_count == 1);

    selector.setCurrentText(QStringLiteral("Bloom_Threshold_RT"));
    QApplication::processEvents();
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
        QStringLiteral("not adopted")));
    REQUIRE(logical.property("pelicanSelectedResourceReason").toString() ==
            QStringLiteral("arbitrary read requires a materialized resource"));
    REQUIRE(logical.property("pelicanSelectedResourceWidestRead").toString() ==
            QStringLiteral("arbitrary"));
    REQUIRE(logical.property("pelicanSelectedResourceAliasable").toBool());
    REQUIRE(logical.property("pelicanSelectedResourceRepresentation")
                .toString() == QStringLiteral("materialized_image"));
    REQUIRE(logical.property("pelicanSelectedResourceLifetime").toString() ==
            QStringLiteral("[11, 12]"));
}

TEST_CASE(
    "WP318 selected target updates its profile and endpoint through the production widget boundary",
    "[devstudio][frame-plan][physical-overlay][profile][wp318]") {
    (void)application();
    const Json optimized = independentOpportunityFramePlan(
        Pelican::PlanningProfileKind::optimized);
    const Json conservative = independentOpportunityFramePlan(
        Pelican::PlanningProfileKind::conservative_debug);

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(optimized.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
    REQUIRE(itemsOfKind(logical, FramePlanResourceLifetimeItem).size() == 1);
    QGraphicsItem *optimized_row =
        itemsOfKind(logical, FramePlanResourceLifetimeItem).front();
    REQUIRE(optimized_row->data(FramePlanProfileRole).toString() ==
            QStringLiteral("optimized"));
    const QString selected =
        logical.property("pelicanSelectedTarget").toString();
    REQUIRE_FALSE(selected.isEmpty());

    widget.receiveResult(QByteArray::fromStdString(conservative.dump()));
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("conservative_debug"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
    REQUIRE(logical.property("pelicanSelectedTarget").toString() == selected);
    REQUIRE(itemsOfKind(logical, FramePlanResourceLifetimeItem).size() == 1);
    QGraphicsItem *conservative_row =
        itemsOfKind(logical, FramePlanResourceLifetimeItem).front();
    REQUIRE(conservative_row->data(FramePlanProfileRole).toString() ==
            QStringLiteral("conservative_debug"));
    REQUIRE(conservative_row->data(FramePlanEndpointRole).toString() ==
            QStringLiteral("device:0"));
}

TEST_CASE(
    "WP318 flat compiler feature toggle purges removed subtree nodes in one widget",
    "[devstudio][frame-plan][target-subtree][feature-toggle][wp318]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER, PELICAN_WITH_IMGUI);
#if !PELICAN_RUNTIME_SHADER_COMPILER
    const Json base = exampleFramePlan(false);
    REQUIRE_FALSE(wireNodeNames(base).contains("pelican_ui"));
    EmbeddedViewport off_viewport;
    FramePlanWidget off_widget{&off_viewport};
    off_widget.receiveResult(QByteArray::fromStdString(base.dump()));
    QApplication::processEvents();
    QGraphicsScene &off_logical = scene(off_widget);
    REQUIRE(off_logical.property("pelicanPhysicalResourceCount")
                .toULongLong() == 1);
    REQUIRE(off_logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(off_logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
    return;
#else
    const Json enabled = exampleFramePlan(true);
    const Json disabled = exampleFramePlan(false);
    REQUIRE(wireNodeNames(enabled).contains("pelican_ui"));
    REQUIRE_FALSE(wireNodeNames(disabled).contains("pelican_ui"));

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(enabled.dump()));
    targetSelector(widget).setCurrentText(QStringLiteral("display"));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    QGraphicsItem *ui = nodeItem(logical, "pelican_ui");
    REQUIRE(ui != nullptr);
    ui->setSelected(true);
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanSelectedNode").toString() ==
            QStringLiteral("pelican_ui"));
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            1);

    widget.receiveResult(QByteArray::fromStdString(disabled.dump()));
    QApplication::processEvents();
    REQUIRE(nodeItem(logical, "pelican_ui") == nullptr);
    REQUIRE(logical.property("pelicanSelectedNode").toString().isEmpty());
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            1);
    REQUIRE(logical.property("pelicanSelectedTarget").toString() ==
            QStringLiteral("display"));
    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
#endif
}

TEST_CASE(
    "WP318 flat preview and XR variants expose the specified logical and physical target states",
    "[devstudio][frame-plan][target-subtree][variant][wp318]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER, PELICAN_WITH_IMGUI,
            PELICAN_WITH_OPENXR);
    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};

    const Json flat = animgraphFramePlan(
        true, Pelican::RenderPipelineGraphVariant::flat);
    widget.receiveResult(QByteArray::fromStdString(flat.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    REQUIRE_FALSE(itemsOfKind(logical, FramePlanNodeItem).empty());
    REQUIRE(itemsOfKind(logical, FramePlanNodeItem).size() <= 5);
    REQUIRE(itemsOfKind(logical, FramePlanResourceLifetimeItem).size() == 1);
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            1);

    const Json preview = animgraphFramePlan(
        true, Pelican::RenderPipelineGraphVariant::preview);
    REQUIRE_FALSE(preview.contains("physical_target_plan"));
    const FramePlanModel preview_model =
        buildFramePlanModel(preview.dump());
    REQUIRE_FALSE(preview_model.physical_plan.loweringGraphAvailable());
    REQUIRE(preview_model.physical_plan
                .lowering_graph_unavailable_reason_code ==
            "physical_plan_missing");
    widget.receiveResult(QByteArray::fromStdString(preview.dump()));
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanGraph").toString() ==
            QString::fromStdString(preview.at("graph").get<std::string>()));
    REQUIRE_FALSE(itemsOfKind(logical, FramePlanNodeItem).empty());
    REQUIRE(itemsOfKind(logical, FramePlanNodeItem).size() <= 5);
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            0);
    const auto preview_context =
        itemsOfKind(logical, FramePlanPhysicalContextItem);
    REQUIRE(preview_context.size() == 1);
    REQUIRE(preview_context.front()
                ->data(FramePlanPhysicalStateRole)
                .toString() == QStringLiteral("unavailable"));
    REQUIRE(preview_context.front()->toolTip().contains(
        QStringLiteral("physical_plan_missing")));
    const auto grouping_unavailable =
        itemsOfKind(logical, FramePlanGroupWarningItem);
    REQUIRE(grouping_unavailable.size() == 1);
    const QString grouping_reason =
        grouping_unavailable.front()
            ->data(FramePlanReasonRole)
            .toString();
    REQUIRE(grouping_reason.contains(
        QStringLiteral("region_grouping_unavailable")));
    REQUIRE(grouping_reason.contains(
        QStringLiteral("physical_plan_missing")));
    REQUIRE(visibleSceneTextContains(logical, grouping_reason));
    for (QGraphicsItem *item :
         itemsOfKind(logical, FramePlanNodeItem)) {
        REQUIRE(item->data(FramePlanGroupIdRole).toString().isEmpty());
    }

#if PELICAN_WITH_OPENXR
    const Json xr = animgraphFramePlan(
        true, Pelican::RenderPipelineGraphVariant::xr);
    widget.receiveResult(QByteArray::fromStdString(xr.dump()));
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanGraph").toString() ==
            QString::fromStdString(xr.at("graph").get<std::string>()));
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            1);
    REQUIRE_FALSE(itemsOfKind(logical, FramePlanNodeItem).empty());
    REQUIRE(itemsOfKind(logical, FramePlanNodeItem).size() <= 5);
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
    "WP318 compiler feature removal purges target subtree selection and physical details in one widget",
    "[devstudio][frame-plan][target-subtree][purge][wp318]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER, PELICAN_WITH_IMGUI);
    const Json enabled = animgraphFramePlan(
        true, Pelican::RenderPipelineGraphVariant::flat);
    const Json disabled = animgraphFramePlan(
        false, Pelican::RenderPipelineGraphVariant::flat);

#if !PELICAN_RUNTIME_SHADER_COMPILER
    REQUIRE(wireNodeNames(enabled) == wireNodeNames(disabled));
    REQUIRE_FALSE(wireNodeNames(enabled).contains("shadow_depth"));
    EmbeddedViewport off_viewport;
    FramePlanWidget off_widget{&off_viewport};
    off_widget.receiveResult(QByteArray::fromStdString(disabled.dump()));
    QApplication::processEvents();
    REQUIRE(targetSelector(off_widget).findText(
                QStringLiteral("shadow_map")) == -1);
    REQUIRE(scene(off_widget)
                .property("pelicanPhysicalResourceCount")
                .toULongLong() == 1);
    return;
#else
    REQUIRE(wireNodeNames(enabled).contains("shadow_depth"));
    REQUIRE_FALSE(wireNodeNames(disabled).contains("shadow_depth"));

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(enabled.dump()));
    QComboBox &selector = targetSelector(widget);
    REQUIRE(selector.findText(QStringLiteral("shadow_map")) >= 0);
    selector.setCurrentText(QStringLiteral("shadow_map"));
    subtreeDepth(widget).setValue(1);
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);

    REQUIRE(logical.property("pelicanSelectedTarget").toString() ==
            QStringLiteral("shadow_map"));
    REQUIRE_FALSE(sceneNodeNames(logical).empty());
    REQUIRE(sceneNodeNames(logical).contains("shadow_depth"));
    REQUIRE(itemsOfKind(logical, FramePlanResourceLifetimeItem).size() == 1);
    REQUIRE_FALSE(logical.property("pelicanSelectedResourceReason")
                      .toString()
                      .isEmpty());
    QGraphicsItem *shadow = nodeItem(logical, "shadow_depth");
    REQUIRE(shadow != nullptr);
    shadow->setSelected(true);
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanSelectedNode").toString() ==
            QStringLiteral("shadow_depth"));

    widget.receiveResult(QByteArray::fromStdString(disabled.dump()));
    QApplication::processEvents();

    REQUIRE(selector.findText(QStringLiteral("shadow_map")) == -1);
    REQUIRE(selector.currentIndex() == -1);
    REQUIRE(logical.property("pelicanSelectedTarget").toString().isEmpty());
    REQUIRE(logical.property("pelicanSelectedResource").toString().isEmpty());
    REQUIRE(logical.property("pelicanSelectedNode").toString().isEmpty());
    REQUIRE(sceneNodeNames(logical).empty());
    REQUIRE(itemsOfKind(logical, FramePlanResourceLifetimeItem).empty());
    REQUIRE(itemsOfKind(logical, FramePlanAliasOverlayItem).empty());
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            0);
    REQUIRE(logical.property("pelicanSelectedResourceReason")
                .toString()
                .isEmpty());
    REQUIRE(logical.property("pelicanSelectedResourceWidestRead")
                .toString()
                .isEmpty());
    REQUIRE(logical.property("pelicanSelectedResourceRepresentation")
                .toString()
                .isEmpty());
    REQUIRE(logical.property("pelicanSelectedResourceLifetime")
                .toString()
                .isEmpty());
#endif
}


TEST_CASE(
    "WP320 stress: interleaved target, depth, zoom and repeated drags stay stable",
    "[devstudio][frame-plan][drag][stress][wp320]") {
    (void)application();
    Json wire = Json::parse(readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.show();
    widget.resize(1200, 800);
    QApplication::processEvents();
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();

    QGraphicsScene &logical = scene(widget);
    QGraphicsView &view = logicalView(widget);
    QComboBox &selector = targetSelector(widget);
    QSpinBox &depth = subtreeDepth(widget);
    QWidget *const vp = view.viewport();
    REQUIRE(vp != nullptr);

    const auto finite_rect = [](const QRectF &r) {
        return std::isfinite(r.x()) && std::isfinite(r.y()) &&
               std::isfinite(r.width()) && std::isfinite(r.height());
    };

    for (int target = 0; target < selector.count(); ++target) {
        selector.setCurrentIndex(target);
        QApplication::processEvents();
        for (int d = 1; d <= 3; ++d) {
            depth.setValue(d);
            QApplication::processEvents();

            // zoom in and out around the view centre
            for (int w = 0; w < 3; ++w) {
                QWheelEvent in{QPointF{vp->rect().center()},
                               vp->mapToGlobal(vp->rect().center()),
                               QPoint{0, 0}, QPoint{0, 120},
                               Qt::NoButton, Qt::NoModifier,
                               Qt::NoScrollPhase, false};
                QApplication::sendEvent(vp, &in);
            }
            QApplication::processEvents();

            const auto nodes = itemsOfKind(logical, FramePlanNodeItem);
            for (QGraphicsItem *node : nodes) {
                const QPoint press =
                    view.mapFromScene(node->sceneBoundingRect().center());
                if (!vp->rect().contains(press)) continue;
                QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, press);
                // long drag with many intermediate moves
                for (int step = 1; step <= 40; ++step) {
                    const QPoint at = press + QPoint{step * 7, step * 5};
                    if (!vp->rect().contains(at)) break;
                    QTest::mouseMove(vp, at, 1);
                }
                QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier,
                                    press + QPoint{40 * 7, 40 * 5});
                QApplication::processEvents();
                REQUIRE(finite_rect(logical.sceneRect()));
            }

            for (int w = 0; w < 3; ++w) {
                QWheelEvent out{QPointF{vp->rect().center()},
                                vp->mapToGlobal(vp->rect().center()),
                                QPoint{0, 0}, QPoint{0, -120},
                                Qt::NoButton, Qt::NoModifier,
                                Qt::NoScrollPhase, false};
                QApplication::sendEvent(vp, &out);
            }
            QApplication::processEvents();
            REQUIRE(finite_rect(logical.sceneRect()));
        }
    }
    REQUIRE(finite_rect(logical.sceneRect()));
}


TEST_CASE(
    "WP320 a frame plan arriving mid-drag does not destabilise the scene",
    "[devstudio][frame-plan][drag][refresh][wp320]") {
    (void)application();
    Json wire = Json::parse(readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");
    const QByteArray captured = QByteArray::fromStdString(wire.dump());

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.show();
    widget.resize(1200, 800);
    QApplication::processEvents();
    widget.receiveResult(captured);
    QApplication::processEvents();

    QGraphicsScene &logical = scene(widget);
    QGraphicsView &view = logicalView(widget);
    QWidget *const vp = view.viewport();
    REQUIRE(vp != nullptr);

    const auto finite_rect = [](const QRectF &r) {
        return std::isfinite(r.x()) && std::isfinite(r.y()) &&
               std::isfinite(r.width()) && std::isfinite(r.height());
    };

    const auto nodes = itemsOfKind(logical, FramePlanNodeItem);
    REQUIRE_FALSE(nodes.empty());
    const QPoint press =
        view.mapFromScene(nodes.front()->sceneBoundingRect().center());
    REQUIRE(vp->rect().contains(press));

    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, press);
    for (int step = 1; step <= 30; ++step) {
        QTest::mouseMove(vp, press + QPoint{step * 6, step * 4}, 1);
        // the live studio keeps receiving frame plans while the user drags
        if (step % 5 == 0) {
            widget.receiveResult(captured);
            QApplication::processEvents();
        }
        REQUIRE(finite_rect(logical.sceneRect()));
    }
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier,
                        press + QPoint{30 * 6, 30 * 4});
    QApplication::processEvents();
    REQUIRE(finite_rect(logical.sceneRect()));
}


} // namespace PelicanStudio
