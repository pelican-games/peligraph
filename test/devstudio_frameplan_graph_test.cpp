#include "enginefailuremodel.hpp"
#include "render_graph_root_test_support.hpp"
#include "engineprocess.hpp"
#include "nativewindowhost.hpp"
#include "viewportgeometry.hpp"

#include <QElapsedTimer>
#include <QPoint>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include <optional>

// EmbeddedViewport is final and intentionally has no production test seam.
// Expose only this test translation unit's existing process/readiness members
// so the widget can be exercised through its real RPC transport.
#define private public
#include "embeddedviewport.hpp"
#undef private

#include "frameplanedit.hpp"
#include "frameplaneditcapability.hpp"
#include "inspectormodel.hpp"
#include "panelhelp.hpp"
#include "frameplangraphics.hpp"
#include "frameplanwidget.hpp"
#include "fullscreenshadertemplate.hpp"
#include "renderconfigdigest.hpp"
#include "materialpassedit.hpp"
#include "materialpassform.hpp"
#include "rendertargetform.hpp"
#include "shaderreloadfollower.hpp"
#include "jsonrpcrequest.hpp"
#include "taskconnectiongraphics.hpp"
#include "taskresourceconnections.hpp"

#include "../src/core/loader/engineresources.hpp"
#include "../src/project/renderconfigsection.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/render_algorithms/cube_capture/cubecaptureview.hpp"
#include "../src/core/renderingpass/frameexecutionadapter.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/renderingpass.hpp"
#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/viewexecutionscheduler.hpp"
#include "../src/core/shader/shaderreference.hpp"
#include "../src/core/vkcore/shaderresolution.hpp"
#include "../src/project/executionplan.hpp"
#include "../src/project/frameresolutionwire.hpp"
#include "../src/project/renderpipeline.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Studio RPC preserves record values or refuses lossy integer transport",
          "[studio][rpc-json]") {
    using nlohmann::json;
    const json request = {{"asset_name", "sphere"}, {"record", {
        {"radius", 1.125}, {"mask", 4294967295u},
        {"nested", json::array({"text", true, nullptr, -17})}}}};
    const auto converted = PelicanStudio::exactJsonRpcObject(request);
    CHECK(json::parse(QJsonDocument(converted).toJson().toStdString()) == request);
    auto large = request;
    large["record"]["nested"].push_back(std::uint64_t{18446744073709551615ull});
    CHECK_THROWS_AS(PelicanStudio::exactJsonRpcObject(large), std::invalid_argument);
    CHECK(large["record"]["nested"].back() == std::uint64_t{18446744073709551615ull});
    CHECK_THROWS_AS(PelicanStudio::exactJsonRpcObject(json::array()), std::invalid_argument);
}
#include <catch2/generators/catch_generators.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QAction>
#include <QBrush>
#include <QByteArray>
#include <QCheckBox>
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
#include <QCursor>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QFocusEvent>
#include <QGraphicsProxyWidget>
#include <QPainterPath>
#include <QPen>
#include <QLabel>
#include <QLineEdit>
#include <QCompleter>
#include <QAbstractItemModel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>
#include <QTemporaryDir>
#include <QTimer>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cctype>
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

void addRuntimeShaderContract(Json &plan) {
    plan["profile"] = "runtime";
    for (auto &node : plan.at("nodes")) {
        node["shader_resolution"] =
            Json{{"state", "not_applicable"}};
    }
}

QApplication &application() {
    static int argument_count = 1;
    static char application_name[] = "pelican_frameplan_graph_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

template <typename Predicate>
bool waitUntil(Predicate predicate, int timeout_ms) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeout_ms) {
        QTest::qWait(10);
    }
    return predicate();
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

Json capturedRuntimeFramePlan() {
    Json plan = Json::parse(
        readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    addRuntimeShaderContract(plan);
    return plan;
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
    addRuntimeShaderContract(wire);
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
    // MG-2b: the shipped document is a `pelican.graph` root; its features
    // live in the `rendering` section.
    auto &section = Pelican::renderingSectionOf(authored);
#if PELICAN_RUNTIME_SHADER_COMPILER
    if (!shadow_directional_enabled) {
        Json retained = Json::array();
        for (const auto &feature : section.at("features")) {
            if (feature != "engine://features/shadow_directional.json") {
                retained.push_back(feature);
            }
        }
        section["features"] = std::move(retained);
    }
#else
    (void)shadow_directional_enabled;
    section["features"] = Json::array();
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
    // P-2a-4b: the shipping project is a `pelican.graph` root, so the edits
    // below go through the production section locator. The document handed to
    // `resolveRenderPipeline` stays a graph root -- that is the shipping shape.
    Json authored = readJson(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                             "projects" / "example" / "passes" /
                             "main_rendering_config.json");
#if PELICAN_RUNTIME_SHADER_COMPILER
    if (!ui_enabled) {
        Pelican::renderingSectionOf(authored)["features"] = Json::array();
    }
#else
    (void)ui_enabled;
    Pelican::renderingSectionOf(authored)["features"] = Json::array();
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
        auto &section = Pelican::renderingSectionOf(authored);
        auto &targets = section.at("render_targets");
        const auto lit_color = std::ranges::find_if(
            targets, [](const Json &target) {
                return target.value("name", std::string{}) == "lit_color";
            });
        if (lit_color == targets.end()) {
            throw std::runtime_error(
                "TAA grouping fixture has no lit_color target");
        }
        (*lit_color)["format_class"] = "scene";
        section["features"] = Json::array(
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
                .authored_config = Pelican::TestSupport::renderGraphRoot(std::move(authored)),
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
                .authored_config = Pelican::TestSupport::renderGraphRoot(std::move(authored)),
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
        addRuntimeShaderContract(wire);
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

Json &authoredRenderingPass(Json &document, std::string_view name) {
    // P-2a-4b: callers pass the shipping `pelican.graph` root, so the passes
    // are looked up through the production section locator.
    for (auto &group :
         Pelican::renderingSectionOf(document).at("rendering_passes")) {
        for (auto &pass : group.at("passes")) {
            if (pass.value("name", std::string{}) == name) {
                return pass;
            }
        }
    }
    throw std::runtime_error("WP343 fixture has no authored pass named " +
                             std::string{name});
}

// MG-2b: the fixture is a `pelican.graph` root like every authored config.
// Its splice carries the bloom component's `buffers`, which a graph root
// reserves for the graph itself (the lowering refuses `rendering.buffers` by
// name), so they reach the composition from this feature instead. Buffers are
// not render nodes, so the 19-node block below is unchanged.
constexpr std::string_view Wp343BloomBuffersReference =
    "engine://features/wp343_bloom_buffers.json";

std::string loadWp343Document(std::string_view reference) {
    if (reference != Wp343BloomBuffersReference) {
        return loadEngineDocument(reference);
    }
    const Json bloom = readJson(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "src" / "core" /
        "resources" / "features" / "bloom.json");
    return Json{
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", "wp343_bloom_buffers"},
        {"runtime_shader_compiler", "optional"},
        {"buffers", bloom.at("buffers")},
    }.dump();
}

Json wp343GroupingAuthoredExample() {
    // P-2a-4b: start from the shipping `rendering` section through the
    // production locator.
    const Json shipping = readJson(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
        "example" / "passes" / "main_rendering_config.json");
    Json authored = Pelican::renderingSectionOf(shipping);

    // P-2a-1b moved the 13 bloom passes and their 9 render targets into
    // engine://features/bloom.json. This fixture needs them as authored
    // project passes -- it clears `features` below and depends on one convex
    // block of 19 adjacent authored render nodes -- so splice the component's
    // declarations back in at the position the project used to hold them.
    const Json bloom = readJson(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "src" / "core" /
        "resources" / "features" / "bloom.json");
    for (const auto &target : bloom.at("render_targets")) {
        authored.at("render_targets").push_back(target);
    }
    // P-2a-2: the composite also reads the component's own `bloom.intensity`
    // runtime_f32 buffer through a resource port, so the splice carries the
    // buffer declarations too -- through Wp343BloomBuffersReference (MG-2b).
    for (const auto &entry : bloom.at("passes")) {
        authored.at("rendering_passes").at(0).at("passes").push_back(
            entry.at("pass"));
    }

    // Keep 19 production-authored render passes in one convex block. The
    // shipped UI feature supplies the nineteenth render node after engine
    // anchors, while the two snapshot copies sit between project render
    // passes. Replace those three detours with one ordinary authored pass so
    // the required filter-removal mutation can really collapse all 19 nodes.
    authored["features"] =
        Json::array({std::string{Wp343BloomBuffersReference}});
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
    authored = Pelican::TestSupport::renderGraphRoot(std::move(authored));
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
            .load_feature_json = loadWp343Document,
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
        auto &passes = Pelican::renderingSectionOf(authored)
                           .at("rendering_passes").at(0).at("passes");
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
                .load_feature_json = loadWp343Document,
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

Json mixedMembershipFeature(
    bool overlapping,
    std::size_t node_count = MixedFeatureNodes.size()) {
    if (node_count < 2 || node_count > MixedFeatureNodes.size()) {
        throw std::runtime_error("WP343 mixed fixture node count is invalid");
    }
    Json feature{
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", "wp343_mixed_membership"},
        {"runtime_shader_compiler", "optional"},
        {"render_targets", Json::array()},
        {"passes", Json::array()},
    };

    for (std::size_t index = 0; index < node_count; ++index) {
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

Json resolvedMixedMembershipFramePlan(
    bool overlapping,
    std::size_t node_count = MixedFeatureNodes.size(),
    const Json &extra_render_targets = Json::array()) {
    Json authored = readJson(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
        "example" / "passes" / "main_rendering_config.json");
    constexpr std::string_view feature_reference =
        "engine://features/wp343_mixed_membership.json";
    Pelican::renderingSectionOf(authored)["features"] =
        Json::array({std::string{feature_reference}});
    for (const auto &target : extra_render_targets)
        Pelican::renderingSectionOf(authored)["render_targets"].push_back(
            target);

    const std::string label =
        overlapping
            ? "wp343/mixed_overlap"
            : node_count == MixedFeatureNodes.size()
                  ? "wp343/mixed_priority"
                  : "wp350/mixed_member_removed";
    auto resolved = Pelican::resolveRenderPipeline(
        Pelican::RenderPipelineRequest{
            .authored_config = std::move(authored),
            .source_name = label + ".json",
        },
        Pelican::RenderEnvironmentCapabilities{
            .runtime_shader_compiler_enabled =
                PELICAN_RUNTIME_SHADER_COMPILER != 0,
            .graph_variant = Pelican::RenderPipelineGraphVariant::flat,
        },
        Pelican::RenderPipelineResolveDependencies{
            .load_feature_json =
                [overlapping, node_count,
                 feature_reference](std::string_view reference) {
                if (reference != feature_reference) {
                    return loadEngineDocument(reference);
                }
                return mixedMembershipFeature(overlapping, node_count).dump();
            },
            .load_pipeline_json = loadEngineDocument,
        });
    return resolvedFramePlan(std::move(resolved), label);
}

const Json &mixedMembershipFramePlan(bool overlapping) {
    static const Json priority = resolvedMixedMembershipFramePlan(false);
    static const Json overlap = resolvedMixedMembershipFramePlan(true);
    return overlapping ? overlap : priority;
}

const Json &sessionGroupMemberRemovedFramePlan() {
    static const Json wire = resolvedMixedMembershipFramePlan(false, 3);
    return wire;
}

constexpr std::size_t Wp350LargeHullNodeCount = 30;

std::string wp350LargeHullNodeName(std::size_t index) {
    return "wp350_large_hull_" + std::to_string(index);
}

Json wp350LargeHullFeature() {
    Json feature{
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", "wp350_large_hull"},
        {"runtime_shader_compiler", "optional"},
        {"render_targets", Json::array()},
        {"passes", Json::array()},
    };
    for (std::size_t index = 0; index < Wp350LargeHullNodeCount; ++index) {
        const std::string name = wp350LargeHullNodeName(index);
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
                {wp350LargeHullNodeName(index - 1) + "_output"});
        }
        feature["passes"].push_back({
            {"insert", index == 0
                           ? "end"
                           : "after:" +
                                 wp350LargeHullNodeName(index - 1)},
            {"pass", std::move(pass)},
        });
    }
    return feature;
}

const Json &wp350LargeHullFramePlan() {
    static const Json wire = [] {
        Json authored = readJson(
            std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
            "example" / "passes" / "main_rendering_config.json");
        constexpr std::string_view feature_reference =
            "engine://features/wp350_large_hull.json";
        Pelican::renderingSectionOf(authored)["features"] =
            Json::array({std::string{feature_reference}});
        auto resolved = Pelican::resolveRenderPipeline(
            Pelican::RenderPipelineRequest{
                .authored_config = std::move(authored),
                .source_name = "wp350/large_hull.json",
            },
            Pelican::RenderEnvironmentCapabilities{
                .runtime_shader_compiler_enabled =
                    PELICAN_RUNTIME_SHADER_COMPILER != 0,
                .graph_variant =
                    Pelican::RenderPipelineGraphVariant::flat,
            },
            Pelican::RenderPipelineResolveDependencies{
                .load_feature_json =
                    [feature_reference](std::string_view reference) {
                    if (reference != feature_reference) {
                        return loadEngineDocument(reference);
                    }
                    return wp350LargeHullFeature().dump();
                },
                .load_pipeline_json = loadEngineDocument,
            });
        return resolvedFramePlan(std::move(resolved),
                                 "wp350/large_hull");
    }();
    return wire;
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
        Pelican::renderingSectionOf(authored)["features"] =
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
    addRuntimeShaderContract(wire);
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
    addRuntimeShaderContract(wire);
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

// F-7: a graph opens on the whole frame. A test about one target's subtree
// chooses the first target -- the one that used to be the default.
void showFirstTarget(FramePlanWidget &widget) {
    targetSelector(widget).setCurrentIndex(1);
    QApplication::processEvents();
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
        // N-3: a node is a heading band over resource rows, and every piece of
        // it carries the node's own name. Collapsing the node takes them all.
        if ((item.kind == FramePlanNodeItem ||
             item.kind == FramePlanNodeLabelItem ||
             item.kind == FramePlanNodeHeaderItem ||
             item.kind == FramePlanNodeKindLabelItem ||
             item.kind == FramePlanSocketItem ||
             item.kind == FramePlanRowLabelItem) &&
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

// N-3: an entity is no longer exactly one shape plus one label. A node is a
// shape carrying a heading band, the name, the kind, and one labelled row per
// resource -- all of them under the same FramePlanNameRole. What every caller
// of the "== 2" assertion was actually fixing is unchanged and asserted here:
// the entity is drawn exactly once as a top-level shape, and it carries
// visible text. The check stays independent of FramePlanItemKindRole, so a
// mutation that clears an item's kind still has to keep the shape and label.
void requireDrawnOnce(QGraphicsScene &value, std::string_view name) {
    std::size_t shapes = 0;
    std::size_t labels = 0;
    for (QGraphicsItem *item : namedShapesAndLabels(value, name)) {
        if (dynamic_cast<QGraphicsSimpleTextItem *>(item) != nullptr) {
            ++labels;
        } else if (item->parentItem() == nullptr) {
            ++shapes;
        }
    }
    REQUIRE(shapes == 1);
    REQUIRE(labels >= 1);
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

// N-3 removed the label box painted in the middle of every edge, so the words
// that distinguished one dependency from another now reach the user as the
// edge's tooltip. The callers fix that the words differ, and that is read
// where the words now are; that the LINE itself also differs is asserted at
// the call site, on the pen.
QString edgeVisibleText(QGraphicsScene &value, std::string_view from,
                        std::string_view to) {
    QStringList summaries;
    for (QGraphicsItem *item : edgeShapesAndLabels(value, from, to)) {
        if (dynamic_cast<const QGraphicsPathItem *>(item) == nullptr ||
            !item->isVisible()) {
            continue;
        }
        summaries.push_back(item->toolTip());
    }
    REQUIRE(summaries.size() == 1);
    REQUIRE_FALSE(summaries.front().isEmpty());
    return summaries.front();
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

// N-3 removed the label box drawn in the middle of every edge: both ends now
// carry the resource name on their own rows, so the box said it a third time.
// The dependency summary it printed is the edge's tooltip, which is where this
// reads it -- the same string, from the same summary.
QString edgeLabelText(QGraphicsScene &value, std::string_view from,
                      std::string_view to) {
    REQUIRE(itemsOfKind(value, FramePlanEdgeLabelItem).empty());
    QGraphicsItem *edge = edgeItem(value, from, to);
    REQUIRE(edge != nullptr);
    const QString summary = edge->toolTip();
    REQUIRE_FALSE(summary.isEmpty());
    return summary;
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
            const std::string resource =
                "Bloom_Downsample_H_" + std::to_string(index) + "_RT";
            add(horizontal, vertical, explicit_after);
            add(horizontal, vertical, read_after_write, resource);
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
                "Bloom_Upsample_V_" + std::to_string(index - 1) +
                "_RT";
            add(from, to, explicit_after);
            add(from, to, read_after_write, resource);
            add(from, "__anchor_pelican_ui", explicit_after);
        }
        add("FinalBloomComposite", "__anchor_pelican_ui", explicit_after);
        add("FinalBloomComposite", "pelican_ui", read_after_write,
            "display");

        if (result.size() != 97) {
            throw std::logic_error(
                "independent WP357 dependency expectation must contain 97 records");
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
    "WP351 Studio shows averaged GPU timing and explicit unavailable states on frame-plan nodes",
    "[devstudio][frame-plan][gpu-timing][wp351][negative-contrast]") {
    (void)application();
    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(
        QByteArray::fromStdString(exampleFramePlan(true).dump()));
    subtreeDepth(widget).setValue(64);
    QApplication::processEvents();

    auto &logical = scene(widget);
    const auto node_names = sceneNodeNames(logical);
    REQUIRE_FALSE(node_names.empty());
    const std::string node_name = *node_names.begin();
    const auto timing_item = [&]() -> QGraphicsSimpleTextItem * {
        for (QGraphicsItem *item : logical.items()) {
            if (item->data(FramePlanGpuTimingNodeNameRole)
                    .toString()
                    .toStdString() ==
                node_name) {
                return dynamic_cast<QGraphicsSimpleTextItem *>(item);
            }
        }
        return nullptr;
    };
    REQUIRE(timing_item() != nullptr);
    REQUIRE(timing_item()->parentItem() != nullptr);
    REQUIRE(timing_item()->parentItem()
                ->data(FramePlanItemKindRole)
                .toString() == QLatin1String{FramePlanNodeItem});
    REQUIRE_FALSE(
        timing_item()->data(FramePlanItemKindRole).isValid());
    REQUIRE(timing_item()->data(FramePlanGpuTimingStateRole).toString() !=
            QStringLiteral("measured"));
    REQUIRE_FALSE(
        timing_item()->data(FramePlanGpuTimingBodyMsRole).isValid());

    const auto response = [&](bool enabled, bool supported,
                              std::string reason, Json nodes,
                              std::size_t frame_count = 3) {
        return QByteArray::fromStdString(Json{
            {"schema", "pelican.gpu_timing_node_averages"},
            {"version", 1},
            {"enabled", enabled},
            {"supported", supported},
            {"reason", std::move(reason)},
            {"window_size", 30},
            {"frame_count", frame_count},
            {"nodes", std::move(nodes)},
        }.dump());
    };

    // Negative name-join control: a valid timing row for a different node
    // must never turn into a numeric zero on the visible node.
    widget.receiveGpuTimingResult(response(
        true, true, "enabled",
        Json::array({Json{
            {"logical_frame", 8}, {"graph_variant", "flat"},
            {"view_index", 0}, {"node_ordinal", 99},
            {"node_kind", "render"}, {"node_name", "not_in_plan"},
            {"barriers_ms", 0.25}, {"body_ms", 1.5},
            {"body_supported", true}, {"sample_count", 3},
        }})));
    REQUIRE(timing_item()->data(FramePlanGpuTimingStateRole).toString() ==
            QStringLiteral("node_not_timed"));
    REQUIRE(timing_item()->data(FramePlanReasonRole).toString() ==
            QStringLiteral("node_not_timed"));
    REQUIRE_FALSE(
        timing_item()->data(FramePlanGpuTimingBodyMsRole).isValid());
    REQUIRE_FALSE(timing_item()->text().contains(QStringLiteral("0.000")));

    widget.receiveGpuTimingResult(response(
        true, true, "enabled",
        Json::array({Json{
            {"logical_frame", 9}, {"graph_variant", "flat"},
            {"view_index", 0}, {"node_ordinal", 2},
            {"node_kind", "render"}, {"node_name", node_name},
            {"barriers_ms", 0.25}, {"body_ms", 1.5},
            {"body_supported", true}, {"sample_count", 3},
        }})));
    REQUIRE(timing_item()->data(FramePlanGpuTimingStateRole).toString() ==
            QStringLiteral("measured"));
    REQUIRE(timing_item()->data(FramePlanGpuTimingBodyMsRole).toDouble() ==
            1.5);
    REQUIRE(
        timing_item()->data(FramePlanGpuTimingBarriersMsRole).toDouble() ==
        0.25);
    REQUIRE(timing_item()->data(FramePlanGpuTimingSampleCountRole)
                .toULongLong() == 3);
    REQUIRE(timing_item()->data(FramePlanGpuTimingFrameCountRole)
                .toULongLong() == 3);
    REQUIRE(timing_item()->data(FramePlanGpuTimingWindowSizeRole)
                .toULongLong() == 30);
    REQUIRE(timing_item()->text().contains(QStringLiteral("body 1.500")));
    REQUIRE(timing_item()->text().contains(
        QStringLiteral("barriers 0.250")));
    REQUIRE(timing_item()->text().contains(QStringLiteral("3/30f")));

    widget.receiveGpuTimingResult(response(
        false, false, "feature_not_enabled", Json::array(), 0));
    REQUIRE(timing_item()->data(FramePlanGpuTimingStateRole).toString() ==
            QStringLiteral("unavailable"));
    REQUIRE(timing_item()->data(FramePlanReasonRole).toString() ==
            QStringLiteral("feature_not_enabled"));
    REQUIRE(timing_item()->text().contains(
        QStringLiteral("feature_not_enabled")));
    REQUIRE_FALSE(
        timing_item()->data(FramePlanGpuTimingBodyMsRole).isValid());
    REQUIRE_FALSE(timing_item()->text().contains(QStringLiteral("0.000")));

    widget.receiveGpuTimingResult(response(
        true, false, "graphics_queue_timestamps_unsupported",
        Json::array(), 0));
    REQUIRE(timing_item()->data(FramePlanReasonRole).toString() ==
            QStringLiteral("graphics_queue_timestamps_unsupported"));
    REQUIRE(timing_item()->text().contains(
        QStringLiteral("graphics_queue_timestamps_unsupported")));

    auto *poll = widget.findChild<QTimer *>(
        QStringLiteral("pelican.gpuTimingPollTimer"));
    REQUIRE(poll != nullptr);
    REQUIRE(poll->interval() == 1000);
    REQUIRE(widget.property("pelicanGpuTimingPollIntervalMs").toInt() ==
            1000);

    // Q-2. Three facts decide whether the clock runs, and each one alone can
    // stop it: an engine to ask, a panel on screen to show the answer on, and
    // an engine that still has an answer worth having. A project that did not
    // turn GPU timing on cannot begin reporting it while it is loaded, so
    // once the engine has said so there is nothing left to ask.
    viewport.rpc_ready_ = true;
    widget.show();
    QApplication::processEvents();
    widget.receiveGpuTimingResult(response(
        true, true, "enabled", Json::array()));
    REQUIRE(poll->isActive());

    // Nobody is looking at the panel: the numbers on it are not worth a
    // round trip through the engine's render thread once a second.
    widget.hide();
    QApplication::processEvents();
    REQUIRE_FALSE(poll->isActive());
    widget.show();
    QApplication::processEvents();
    REQUIRE(poll->isActive());

    widget.receiveGpuTimingResult(response(
        false, false, "feature_not_enabled", Json::array(), 0));
    REQUIRE_FALSE(poll->isActive());

    widget.receiveGpuTimingResult(response(
        true, false, "graphics_queue_timestamps_unsupported", Json::array(),
        0));
    REQUIRE_FALSE(poll->isActive());

    // And with the engine gone there is nobody to ask at all.
    widget.receiveGpuTimingResult(response(
        true, true, "enabled", Json::array()));
    REQUIRE(poll->isActive());
    viewport.rpc_ready_ = false;
    widget.hide();
    widget.show();
    QApplication::processEvents();
    REQUIRE_FALSE(poll->isActive());
}

TEST_CASE(
    "WP351a Studio GPU timing poll traverses the timer and request-id routing",
    "[devstudio][frame-plan][gpu-timing][rpc][poll][wp351a]") {
    (void)application();
    EmbeddedViewport viewport;
    QString transport_log;
    QObject::connect(
        &viewport, &EmbeddedViewport::engineStandardErrorReceived,
        [&](const QString &text) { transport_log += text; });

    QString error;
    REQUIRE(viewport.process_.start(
        {QString::fromUtf8(PELICAN_PROCESS_FIXTURE_PATH),
         {QStringLiteral("rpc-loop")}, {}},
        &error));
    REQUIRE(error.isEmpty());
    REQUIRE(viewport.process_.waitForStarted(5000));
    viewport.rpc_ready_ = true;

    FramePlanWidget widget{&viewport};
    widget.receiveResult(
        QByteArray::fromStdString(exampleFramePlan(true).dump()));
    subtreeDepth(widget).setValue(64);
    QApplication::processEvents();

    const auto timing_item = [&]() -> QGraphicsSimpleTextItem * {
        for (QGraphicsItem *item : scene(widget).items()) {
            if (item->data(FramePlanGpuTimingNodeNameRole).isValid()) {
                return dynamic_cast<QGraphicsSimpleTextItem *>(item);
            }
        }
        return nullptr;
    };
    REQUIRE(timing_item() != nullptr);

    auto *poll = widget.findChild<QTimer *>(
        QStringLiteral("pelican.gpuTimingPollTimer"));
    REQUIRE(poll != nullptr);
    REQUIRE(poll->interval() == 1000);
    // Not asserted active here: the fixture answers within the same event
    // loop turn, and its answer -- neither enabled nor supported -- is what
    // stops the clock. Whether it ran is settled below, by the request the
    // fixture logged.
    QSignalSpy timeout_spy{poll, &QTimer::timeout};

    const QString fixture_reason =
        QStringLiteral("wp351a_fixture_response");
    (void)waitUntil(
        [&] {
            auto *item = timing_item();
            return item != nullptr &&
                   item->data(FramePlanReasonRole).toString() ==
                       fixture_reason;
        },
        3000);
    auto *routed_item = timing_item();
    REQUIRE(routed_item != nullptr);
    const QString routed_state =
        routed_item->data(FramePlanGpuTimingStateRole).toString();
    const QString routed_reason =
        routed_item->data(FramePlanReasonRole).toString();
    INFO("routed GPU timing state=" << routed_state.toStdString()
                                      << " reason="
                                      << routed_reason.toStdString());
    INFO("fixture transport log=" << transport_log.toStdString());
    REQUIRE(routed_state == QStringLiteral("unavailable"));
    REQUIRE(routed_reason == fixture_reason);

    const auto gpu_request_count = [&] {
        return transport_log.count(
            QStringLiteral("method=get_gpu_timing"));
    };
    // The routed reply says the feature is neither enabled nor supported, so
    // Q-2 stops the clock: the one request that fetched that answer is the
    // only one there is any point in making. Three seconds is three of the
    // poll's old intervals.
    const int answered_count = gpu_request_count();
    REQUIRE(answered_count >= 1);
    (void)waitUntil([&] { return gpu_request_count() > answered_count; },
                    3000);
    INFO("QTimer timeout count=" << timeout_spy.count());
    INFO("observed get_gpu_timing request count="
         << gpu_request_count());
    INFO("fixture transport log=" << transport_log.toStdString());
    REQUIRE_FALSE(poll->isActive());
    REQUIRE(gpu_request_count() == answered_count);
}

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
        requireDrawnOnce(without_scene, member);
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
        requireDrawnOnce(with_scene, member);
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
    requireDrawnOnce(logical, "wp343_mixed_a");
    requireDrawnOnce(logical, "wp343_mixed_b");

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
        requireDrawnOnce(logical, name);
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
    requireDrawnOnce(logical, "inside_a");
    requireDrawnOnce(logical, "inside_b");

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
        requireDrawnOnce(logical, member);
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
    // The canvas distinction that replaced the two label boxes: an order-only
    // dependency is a dashed grey line, a barrier is a solid coloured one.
    auto *barrier_edge = dynamic_cast<QGraphicsPathItem *>(
        edgeItem(logical, Wp347Producer, Wp347Consumer));
    auto *order_edge = dynamic_cast<QGraphicsPathItem *>(
        edgeItem(logical, Wp347Producer, Wp347Ordered));
    REQUIRE(barrier_edge != nullptr);
    REQUIRE(order_edge != nullptr);
    REQUIRE(order_edge->pen().style() == Qt::DashLine);
    REQUIRE(barrier_edge->pen().style() != Qt::DashLine);
    REQUIRE(barrier_edge->pen().color() != order_edge->pen().color());

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
        requireDrawnOnce(logical, member);
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
    "WP349 rubber band and Ctrl selection move exactly the selected production nodes",
    "[devstudio][frame-plan][selection][drag][production][wp349]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER);
    const Json &wire = mixedMembershipFramePlan(false);
    const FramePlanModel model = buildFramePlanModel(wire.dump());
    REQUIRE(model.execution_plan.available());
    REQUIRE(model.physical_plan.loweringGraphAvailable());
    REQUIRE(wireFeatureMembers(wire, "wp343_mixed_membership") ==
            StringSet{"wp343_mixed_a", "wp343_mixed_b",
                      "wp343_mixed_c", "wp343_mixed_d"});

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.resize(1400, 900);
    widget.show();
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    targetSelector(widget).setCurrentText(
        QStringLiteral("wp343_mixed_d_output"));
    subtreeDepth(widget).setValue(64);
    QApplication::processEvents();

    auto *logical_scene =
        dynamic_cast<FramePlanGraphicsScene *>(&scene(widget));
    REQUIRE(logical_scene != nullptr);
    FramePlanGraphicsScene &logical = *logical_scene;
    QGraphicsView &view = logicalView(widget);
    QWidget *const viewport_widget = view.viewport();
    REQUIRE(viewport_widget != nullptr);
    REQUIRE(view.dragMode() == QGraphicsView::RubberBandDrag);

    constexpr std::string_view first_name = "wp343_mixed_a";
    constexpr std::string_view second_name = "wp343_mixed_b";
    constexpr std::string_view third_name = "wp343_mixed_c";
    const StringSet moved_names{std::string{first_name},
                                std::string{second_name},
                                std::string{third_name}};
    QGraphicsItem *first = nodeItem(logical, first_name);
    QGraphicsItem *second = nodeItem(logical, second_name);
    QGraphicsItem *third = nodeItem(logical, third_name);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(third != nullptr);

    // Put two nodes in an isolated vertical pair so a real rubber-band gesture
    // has an unambiguous expected result independent of layout evolution.
    const QRectF original_bounds = logical.itemsBoundingRect();
    const QPointF cluster_origin{original_bounds.right() + 480.0,
                                 original_bounds.top() + 120.0};
    first->setPos(cluster_origin);
    second->setPos(cluster_origin + QPointF{0.0, 130.0});
    third->setPos(cluster_origin + QPointF{350.0, 65.0});
    QApplication::processEvents();

    const QRectF rubber_band =
        first->sceneBoundingRect()
            .united(second->sceneBoundingRect())
            .adjusted(-12.0, -12.0, 12.0, 12.0);
    const QRectF cluster =
        rubber_band.united(third->sceneBoundingRect()).adjusted(
            -30.0, -30.0, 30.0, 30.0);
    view.centerOn(cluster.center());
    QApplication::processEvents();
    const QPoint band_start = view.mapFromScene(rubber_band.topLeft());
    const QPoint band_end = view.mapFromScene(rubber_band.bottomRight());
    REQUIRE(viewport_widget->rect().contains(band_start));
    REQUIRE(viewport_widget->rect().contains(band_end));
    QTest::mousePress(viewport_widget, Qt::LeftButton, Qt::NoModifier,
                      band_start);
    QTest::mouseMove(viewport_widget, (band_start + band_end) / 2, 1);
    QTest::mouseMove(viewport_widget, band_end, 1);
    QTest::mouseRelease(viewport_widget, Qt::LeftButton, Qt::NoModifier,
                        band_end);
    QApplication::processEvents();
    REQUIRE(strings(logical.property("pelicanSelectedNodes")) ==
            StringSet{std::string{first_name}, std::string{second_name}});
    REQUIRE(logical.selectedNodes().size() == 2);

    // The legacy property remains exactly the clicked node for a single
    // selection. Ctrl+click then adds, removes, and re-adds nodes.
    logical.clearSelection();
    QApplication::processEvents();
    const auto click_node = [&](QGraphicsItem &item,
                                Qt::KeyboardModifiers modifiers) {
        view.centerOn(cluster.center());
        QApplication::processEvents();
        const QPoint at =
            view.mapFromScene(item.sceneBoundingRect().center());
        REQUIRE(viewport_widget->rect().contains(at));
        QTest::mouseClick(viewport_widget, Qt::LeftButton, modifiers, at);
        QApplication::processEvents();
    };
    click_node(*first, Qt::NoModifier);
    REQUIRE(logical.property("pelicanSelectedNode").toString() ==
            QString::fromUtf8(first_name.data(),
                              static_cast<qsizetype>(first_name.size())));
    REQUIRE(strings(logical.property("pelicanSelectedNodes")) ==
            StringSet{std::string{first_name}});
    click_node(*second, Qt::ControlModifier);
    REQUIRE(strings(logical.property("pelicanSelectedNodes")) ==
            StringSet{std::string{first_name}, std::string{second_name}});
    click_node(*second, Qt::ControlModifier);
    REQUIRE(strings(logical.property("pelicanSelectedNodes")) ==
            StringSet{std::string{first_name}});
    click_node(*second, Qt::ControlModifier);
    click_node(*third, Qt::ControlModifier);
    REQUIRE(strings(logical.property("pelicanSelectedNodes")) ==
            moved_names);
    REQUIRE(logical.selectedNodes().size() == moved_names.size());

    const auto before_positions = sceneNodePositions(logical);
    REQUIRE(before_positions.size() > moved_names.size());
    const QPoint press =
        view.mapFromScene(first->sceneBoundingRect().center());
    const QPoint drag_offset{82, 54};
    REQUIRE(viewport_widget->rect().contains(press));
    REQUIRE(viewport_widget->rect().contains(press + drag_offset));
    const QPointF expected_delta =
        view.mapToScene(press + drag_offset) - view.mapToScene(press);
    QTest::mousePress(viewport_widget, Qt::LeftButton, Qt::NoModifier,
                      press);
    for (int step = 1; step <= 4; ++step) {
        QTest::mouseMove(viewport_widget,
                         press + drag_offset * step / 4, 1);
    }
    QTest::mouseRelease(viewport_widget, Qt::LeftButton, Qt::NoModifier,
                        press + drag_offset);
    QApplication::processEvents();

    const auto moved_positions = sceneNodePositions(logical);
    REQUIRE(moved_positions.size() == before_positions.size());
    for (const auto &[name, before] : before_positions) {
        CAPTURE(name);
        const QPointF actual_delta = moved_positions.at(name) - before;
        if (moved_names.contains(name)) {
            // Positive control: every selected node moves by the drag delta.
            REQUIRE(QLineF{actual_delta, expected_delta}.length() < 0.01);
        } else {
            // Negative control in the same test: no unselected node moves.
            REQUIRE(moved_positions.at(name) == before);
        }
    }

    // A normal frame-plan refresh rebuilds every item. Session positions keep
    // using the existing {graph, kind, stable identity} key across that update.
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();
    REQUIRE(targetSelector(widget).currentText() ==
            QStringLiteral("wp343_mixed_d_output"));
    REQUIRE(sceneNodePositions(logical) == moved_positions);
    REQUIRE(strings(logical.property("pelicanSelectedNodes")) ==
            moved_names);
}

TEST_CASE(
    "WP349 collapse removes only hidden members from a multiple selection and scope changes clear it",
    "[devstudio][frame-plan][selection][grouping][production][wp349]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER);
    const Json &wire = mixedMembershipFramePlan(false);
    const FramePlanModel model = buildFramePlanModel(wire.dump());
    REQUIRE(model.execution_plan.available());
    REQUIRE(model.physical_plan.loweringGraphAvailable());

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.resize(1200, 760);
    widget.show();
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    targetSelector(widget).setCurrentText(
        QStringLiteral("wp343_mixed_d_output"));
    subtreeDepth(widget).setValue(64);
    QApplication::processEvents();

    auto *logical_scene =
        dynamic_cast<FramePlanGraphicsScene *>(&scene(widget));
    REQUIRE(logical_scene != nullptr);
    FramePlanGraphicsScene &logical = *logical_scene;
    QGraphicsItem *outside = nodeItem(logical, "wp343_mixed_c");
    QGraphicsItem *member = nodeItem(logical, "wp343_mixed_a");
    REQUIRE(outside != nullptr);
    REQUIRE(member != nullptr);
    const QString group_id =
        member->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(group_id.isEmpty());
    REQUIRE(member->data(FramePlanGroupCollapsibleRole).toBool());

    outside->setSelected(true);
    member->setSelected(true);
    QApplication::processEvents();
    REQUIRE(strings(logical.property("pelicanSelectedNodes")) ==
            StringSet{"wp343_mixed_a", "wp343_mixed_c"});
    REQUIRE(logical.property("pelicanSelectedNode").toString() ==
            QStringLiteral("wp343_mixed_a"));

    REQUIRE(logical.collapseGroup(group_id));
    QApplication::processEvents();
    REQUIRE(nodeItem(logical, "wp343_mixed_a") == nullptr);
    outside = nodeItem(logical, "wp343_mixed_c");
    REQUIRE(outside != nullptr);
    REQUIRE(outside->isSelected());
    REQUIRE(logical.selectedItems().size() == 1);
    REQUIRE(logical.selectedNodes().size() == 1);
    REQUIRE(strings(logical.property("pelicanSelectedNodes")) ==
            StringSet{"wp343_mixed_c"});
    REQUIRE(logical.property("pelicanSelectedNode").toString() ==
            QStringLiteral("wp343_mixed_c"));
    REQUIRE(logical.property("pelicanSelectedGraph").toString() ==
            QString::fromStdString(model.graph));

    REQUIRE(logical.enterGroup(group_id));
    REQUIRE(logical.selectedItems().empty());
    REQUIRE(logical.selectedNodes().empty());
    REQUIRE(logical.property("pelicanSelectedNodes")
                .toStringList()
                .isEmpty());
    REQUIRE(logical.property("pelicanSelectedNode").toString().isEmpty());
    REQUIRE(logical.property("pelicanSelectedGraph").toString().isEmpty());
    REQUIRE(logical.leaveGroup());
    REQUIRE(logical.selectedItems().empty());
    REQUIRE(logical.selectedNodes().empty());
}

TEST_CASE(
    "WP350 a convex production selection becomes a removable session group and coexists with authored regions",
    "[devstudio][frame-plan][grouping][selection][production][wp350][negative-contrast]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER);
    const std::filesystem::path config_path =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
        "example" / "passes" / "main_rendering_config.json";
    const std::string config_before = readText(config_path);
    const Json &wire = mixedMembershipFramePlan(false);
    const FramePlanModel model = buildFramePlanModel(wire.dump());
    REQUIRE(model.execution_plan.available());
    REQUIRE(model.physical_plan.loweringGraphAvailable());

    FramePlanGraphicsScene logical;
    logical.populate(
        model,
        FramePlanNodeKey{model.graph, "wp343_mixed_d_output"}, 64);
    const SceneItemSet before_items = annotatedSceneItems(logical);
    QGraphicsItem *c = nodeItem(logical, "wp343_mixed_c");
    QGraphicsItem *d = nodeItem(logical, "wp343_mixed_d");
    REQUIRE(c != nullptr);
    REQUIRE(d != nullptr);
    c->setSelected(true);
    d->setSelected(true);
    QApplication::processEvents();
    REQUIRE(strings(logical.property("pelicanSelectedNodes")) ==
            StringSet{"wp343_mixed_c", "wp343_mixed_d"});

    // Exercise the production context-menu entry fed by WP349's live
    // multiple selection. Creation immediately uses WP341's collapsed form.
    triggerContextAction(logical, *c,
                         QStringLiteral("Create session group"));
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).size() == 1);
    QGraphicsItem *session_group = singleGroupItem(logical);
    REQUIRE(session_group->data(FramePlanSessionGroupRole).toBool());
    REQUIRE(strings(session_group->data(FramePlanMembersRole)) ==
            StringSet{"wp343_mixed_c", "wp343_mixed_d"});
    const QString session_group_id =
        session_group->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(session_group_id.isEmpty());
    REQUIRE(strings(logical.property("pelicanSessionGroups")) ==
            StringSet{session_group_id.toStdString()});
    const StringSet session_members{"wp343_mixed_c", "wp343_mixed_d"};
    const std::string session_group_name =
        session_group->data(FramePlanNameRole).toString().toStdString();
    REQUIRE(annotatedSceneItems(logical) ==
            quotientSceneItems(before_items, session_members, model.graph,
                               session_group_name));
    for (const auto &member : {"wp343_mixed_c", "wp343_mixed_d"}) {
        // Kind-independent negative control: neither the member shape nor its
        // label may survive under a blank or changed item kind.
        REQUIRE(namedShapesAndLabels(logical, member).empty());
    }

    REQUIRE(logical.enterGroup(session_group_id));
    REQUIRE(sceneNodeNames(logical) ==
            StringSet{"wp343_mixed_c", "wp343_mixed_d"});
    REQUIRE(logical.leaveGroup());

    // The authored wp343.mixed region is disjoint from this session group.
    // Both use the same quotient machinery and can be collapsed together.
    QGraphicsItem *authored_member =
        nodeItem(logical, "wp343_mixed_a");
    REQUIRE(authored_member != nullptr);
    const QString authored_group_id =
        authored_member->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(authored_group_id.isEmpty());
    REQUIRE_FALSE(
        authored_member->data(FramePlanSessionGroupRole).toBool());
    REQUIRE(logical.collapseGroup(authored_group_id));
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).size() == 2);
    REQUIRE(std::ranges::count_if(
                itemsOfKind(logical, FramePlanGroupItem),
                [](QGraphicsItem *item) {
                    return item->data(FramePlanSessionGroupRole).toBool();
                }) == 1);

    REQUIRE(logical.expandGroup(authored_group_id));
    REQUIRE(logical.removeSessionGroup(session_group_id));
    REQUIRE(logical.property("pelicanSessionGroups")
                .toStringList()
                .isEmpty());
    REQUIRE(annotatedSceneItems(logical) == before_items);

    // On overlap, the live session operation has priority over the authored
    // region. Removing it reveals the exact authored grouping again.
    FramePlanGraphicsScene priority;
    priority.populate(
        model,
        FramePlanNodeKey{model.graph, "wp343_mixed_d_output"}, 64);
    QGraphicsItem *a = nodeItem(priority, "wp343_mixed_a");
    QGraphicsItem *b = nodeItem(priority, "wp343_mixed_b");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    const QString original_region_id =
        a->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(original_region_id.isEmpty());
    a->setSelected(true);
    b->setSelected(true);
    QApplication::processEvents();
    REQUIRE(priority.createGroupFromSelection());
    QGraphicsItem *priority_group = singleGroupItem(priority);
    REQUIRE(priority_group->data(FramePlanSessionGroupRole).toBool());
    REQUIRE(strings(priority_group->data(FramePlanMembersRole)) ==
            StringSet{"wp343_mixed_a", "wp343_mixed_b"});
    const QString priority_session_id =
        priority_group->data(FramePlanGroupIdRole).toString();
    REQUIRE(priority_session_id != original_region_id);
    REQUIRE(priority.removeSessionGroup(priority_session_id));
    a = nodeItem(priority, "wp343_mixed_a");
    REQUIRE(a != nullptr);
    REQUIRE(a->data(FramePlanGroupIdRole).toString() ==
            original_region_id);
    REQUIRE_FALSE(a->data(FramePlanSessionGroupRole).toBool());

    // Frame Plan remains a read-only view: the real project config is read
    // byte-for-byte again after every grouping, scope, and removal operation.
    REQUIRE(readText(config_path) == config_before);
}

TEST_CASE(
    "WP350 a non-convex production selection visibly proposes and accepts its named convex hull",
    "[devstudio][frame-plan][grouping][selection][convex-hull][production][wp350][negative-contrast]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER);
    const FramePlanModel model =
        buildFramePlanModel(mixedMembershipFramePlan(false).dump());
    FramePlanGraphicsScene logical;
    logical.populate(
        model,
        FramePlanNodeKey{model.graph, "wp343_mixed_d_output"}, 64);
    const SceneItemSet before_items = annotatedSceneItems(logical);
    QGraphicsItem *a = nodeItem(logical, "wp343_mixed_a");
    QGraphicsItem *c = nodeItem(logical, "wp343_mixed_c");
    REQUIRE(a != nullptr);
    REQUIRE(c != nullptr);
    a->setSelected(true);
    c->setSelected(true);
    QApplication::processEvents();

    // A -> B -> C is not convex without B. It must not silently create the
    // two-member group; instead the actual visible banner names B.
    REQUIRE_FALSE(logical.createGroupFromSelection());
    REQUIRE(logical.property("pelicanSessionGroups")
                .toStringList()
                .isEmpty());
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    const auto proposals =
        itemsOfKind(logical, FramePlanConvexHullProposalItem);
    REQUIRE(proposals.size() == 1);
    const QString proposal_text =
        proposals.front()->data(FramePlanReasonRole).toString();
    REQUIRE(proposal_text.contains(QStringLiteral("not convex")));
    REQUIRE(proposal_text.contains(
        QStringLiteral("wp343_mixed_b")));
    REQUIRE(visibleSceneTextContains(logical, proposal_text));
    REQUIRE(visibleSceneTextContains(
        logical, QStringLiteral("wp343_mixed_b")));
    REQUIRE(strings(proposals.front()->data(
                FramePlanConvexHullMissingMembersRole)) ==
            StringSet{"wp343_mixed_b"});
    requireDrawnOnce(logical, "wp343_mixed_a");
    requireDrawnOnce(logical, "wp343_mixed_c");

    REQUIRE(logical.acceptConvexHullProposal());
    REQUIRE(itemsOfKind(logical, FramePlanConvexHullProposalItem).empty());
    QGraphicsItem *group = singleGroupItem(logical);
    REQUIRE(group->data(FramePlanSessionGroupRole).toBool());
    REQUIRE(group->data(FramePlanGroupCollapsibleRole).toBool());
    REQUIRE(strings(group->data(FramePlanMembersRole)) ==
            StringSet{"wp343_mixed_a", "wp343_mixed_b",
                      "wp343_mixed_c"});
    const QString group_id =
        group->data(FramePlanGroupIdRole).toString();
    for (const auto &member : {"wp343_mixed_a", "wp343_mixed_b",
                               "wp343_mixed_c"}) {
        REQUIRE(namedShapesAndLabels(logical, member).empty());
    }

    REQUIRE(logical.removeSessionGroup(group_id));
    REQUIRE(annotatedSceneItems(logical) == before_items);
}

TEST_CASE(
    "WP350 an impractically large production convex hull reports names and counts",
    "[devstudio][frame-plan][grouping][selection][convex-hull][large][production][wp350]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER);
    const Json &wire = wp350LargeHullFramePlan();
    REQUIRE(wireFeatureMembers(wire, "wp350_large_hull").size() ==
            Wp350LargeHullNodeCount);
    const FramePlanModel model = buildFramePlanModel(wire.dump());
    FramePlanGraphicsScene logical;
    logical.populate(
        model,
        FramePlanNodeKey{
            model.graph,
            wp350LargeHullNodeName(Wp350LargeHullNodeCount - 1) +
                "_output"},
        64);
    QGraphicsItem *first =
        nodeItem(logical, wp350LargeHullNodeName(0));
    QGraphicsItem *last = nodeItem(
        logical,
        wp350LargeHullNodeName(Wp350LargeHullNodeCount - 1));
    REQUIRE(first != nullptr);
    REQUIRE(last != nullptr);
    first->setSelected(true);
    last->setSelected(true);
    QApplication::processEvents();
    REQUIRE_FALSE(logical.createGroupFromSelection());

    const auto proposals =
        itemsOfKind(logical, FramePlanConvexHullProposalItem);
    REQUIRE(proposals.size() == 1);
    REQUIRE(proposals.front()
                ->data(FramePlanConvexHullImpracticalRole)
                .toBool());
    const QStringList missing =
        proposals.front()
            ->data(FramePlanConvexHullMissingMembersRole)
            .toStringList();
    REQUIRE(missing.size() >=
            static_cast<qsizetype>(Wp350LargeHullNodeCount - 2));
    const QString text =
        proposals.front()->data(FramePlanReasonRole).toString();
    REQUIRE(text.contains(QStringLiteral("probably impractical")));
    REQUIRE(text.contains(QStringLiteral("Missing nodes")));
    REQUIRE(text.contains(QString::number(missing.size())));
    REQUIRE(text.contains(QString::fromStdString(
        wp350LargeHullNodeName(1))));
    REQUIRE(visibleSceneTextContains(logical, text));
}

TEST_CASE(
    "WP350 session groups survive ordinary updates and disappear with a missing member and scope",
    "[devstudio][frame-plan][grouping][selection][state][production][wp350]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER);
    const FramePlanModel original =
        buildFramePlanModel(mixedMembershipFramePlan(false).dump());
    FramePlanGraphicsScene logical;
    const FramePlanNodeKey original_target{
        original.graph, "wp343_mixed_d_output"};
    logical.populate(original, original_target, 64);
    QGraphicsItem *c = nodeItem(logical, "wp343_mixed_c");
    QGraphicsItem *d = nodeItem(logical, "wp343_mixed_d");
    REQUIRE(c != nullptr);
    REQUIRE(d != nullptr);
    c->setSelected(true);
    d->setSelected(true);
    QApplication::processEvents();
    REQUIRE(logical.createGroupFromSelection());
    const QString group_id =
        singleGroupItem(logical)->data(FramePlanGroupIdRole).toString();
    REQUIRE(logical.enterGroup(group_id));

    // The same production plan is a normal refresh: membership, collapse,
    // and current scope all remain session state.
    logical.populate(original, original_target, 64);
    REQUIRE(logical.property("pelicanCurrentGroupScope").toString() ==
            group_id);
    REQUIRE(strings(logical.property("pelicanSessionGroups")) ==
            StringSet{group_id.toStdString()});
    REQUIRE(sceneNodeNames(logical) ==
            StringSet{"wp343_mixed_c", "wp343_mixed_d"});

    // This second model also passed resolve -> compile -> plan -> wire, but D
    // no longer exists. The whole session group is discarded (not shrunk),
    // its scope is exited, and no collapsed/scope orphan remains.
    const FramePlanModel updated = buildFramePlanModel(
        sessionGroupMemberRemovedFramePlan().dump());
    REQUIRE(updated.graph == original.graph);
    logical.populate(
        updated,
        FramePlanNodeKey{updated.graph, "wp343_mixed_c_output"}, 64);
    REQUIRE(logical.property("pelicanSessionGroups")
                .toStringList()
                .isEmpty());
    REQUIRE(logical.property("pelicanCollapsedGroups")
                .toStringList()
                .isEmpty());
    REQUIRE(logical.property("pelicanCurrentGroupScope")
                .toString()
                .isEmpty());
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(nodeItem(logical, "wp343_mixed_d") == nullptr);
    requireDrawnOnce(logical, "wp343_mixed_c");
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
    showFirstTarget(widget);
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
    const std::string captured = capturedRuntimeFramePlan().dump();
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
    Json wire = capturedRuntimeFramePlan();
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    QComboBox &selector = targetSelector(widget);
    QSpinBox &depth = subtreeDepth(widget);

    // F-7: the whole frame, then the 22 targets.
    REQUIRE(selector.count() == 23);
    REQUIRE(selector.itemText(0) == QStringLiteral("Whole frame"));
    REQUIRE(depth.value() == 1);
    REQUIRE(logical.property("pelicanNodeRecordCount").toULongLong() == 30);

    for (int index = 1; index < selector.count(); ++index) {
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
                  "HorizontalBlur_0", "VerticalBlur_0",
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
    REQUIRE(expected_by_depth[2].size() == 7);
    REQUIRE(expected_by_depth[2].size() < wire.at("nodes").size());
}

TEST_CASE(
    "WP318 subtree coordinates and bundle order ignore every input array order",
    "[devstudio][frame-plan][target-subtree][determinism][wp318]") {
    (void)application();
    Json wire = capturedRuntimeFramePlan();
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
    Json wire = capturedRuntimeFramePlan();
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
    // N-3: no edge label box is drawn any more.
    REQUIRE(label == nullptr);
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
    Json wire = capturedRuntimeFramePlan();
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
        capturedRuntimeFramePlan().dump()));
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

// ---------------------------------------------------------------------------
// N-3: the Frame Plan draws the node the common graph draws -- a heading band
// over an output section and an input section, with one typed socket per
// resource -- and every edge ends on a socket centre.
// ---------------------------------------------------------------------------

TEST_CASE(
    "N-3 frame plan pass nodes are a heading band over resource rows and their "
    "edges end on socket centres",
    "[devstudio][frame-plan][node-visual][n3]") {
    (void)application();
    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(
        capturedRuntimeFramePlan().dump()));
    targetSelector(widget).setCurrentText(
        QStringLiteral("Bloom_Threshold_RT"));
    subtreeDepth(widget).setValue(2);
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);

    const auto part = [&](const char *item_kind, std::string_view node,
                          const QString &port, const QString &direction) {
        const QString expected = QString::fromUtf8(
            node.data(), static_cast<qsizetype>(node.size()));
        std::vector<QGraphicsItem *> matches;
        for (QGraphicsItem *item : itemsOfKind(logical, item_kind)) {
            if (item->data(FramePlanNameRole).toString() != expected) continue;
            if (!port.isNull() &&
                item->data(FramePlanPortRole).toString() != port)
                continue;
            if (!direction.isNull() &&
                item->data(FramePlanSocketDirectionRole).toString() !=
                    direction)
                continue;
            matches.push_back(item);
        }
        INFO("node " << node << " part " << item_kind);
        REQUIRE(matches.size() == 1);
        return matches.front();
    };

    // HighLuminanceExtraction reads lit_color and writes Bloom_Threshold_RT,
    // so the node is the band, the GPU-timing line, one output row and one
    // input row -- three rows, not a fixed 64px box.
    QGraphicsItem *node = nodeItem(logical, "HighLuminanceExtraction");
    REQUIRE(node != nullptr);
    auto *node_path = dynamic_cast<QGraphicsPathItem *>(node);
    REQUIRE(node_path != nullptr);
    const QRectF body = node_path->path().boundingRect();
    REQUIRE(body.topLeft() == QPointF{0.0, 0.0});
    REQUIRE(body.width() >= FramePlanNodeMinimumWidth);
    REQUIRE(body.width() <= FramePlanNodeMaximumWidth);
    REQUIRE(std::abs(body.height() -
                     (FramePlanHeaderHeight + FramePlanSectionPadding +
                      3.0 * FramePlanSocketRowHeight +
                      FramePlanSectionPadding)) < 0.001);

    // (a) the heading band: the source colour, the pass name in bold, the kind
    QGraphicsItem *band =
        part(FramePlanNodeHeaderItem, "HighLuminanceExtraction", {}, {});
    auto *band_path = dynamic_cast<QGraphicsPathItem *>(band);
    REQUIRE(band_path != nullptr);
    REQUIRE(band->parentItem() == node);
    const QRectF band_bounds = band_path->path().boundingRect();
    REQUIRE(std::abs(band_bounds.height() - FramePlanHeaderHeight) < 0.001);
    REQUIRE(std::abs(band_bounds.width() - body.width()) < 0.001);
    REQUIRE(band_path->brush().color() ==
            QColor{QStringLiteral("#3978a8")});
    auto *heading = dynamic_cast<QGraphicsSimpleTextItem *>(
        nodeLabelItem(logical, "HighLuminanceExtraction"));
    REQUIRE(heading != nullptr);
    REQUIRE(heading->parentItem() == band);
    REQUIRE(heading->text() == QStringLiteral("HighLuminanceExtraction"));
    REQUIRE(heading->font().bold());
    auto *kind_text = dynamic_cast<QGraphicsSimpleTextItem *>(
        part(FramePlanNodeKindLabelItem, "HighLuminanceExtraction", {}, {}));
    REQUIRE(kind_text != nullptr);
    REQUIRE(kind_text->text() == QStringLiteral("render"));
    REQUIRE_FALSE(kind_text->font().bold());

    // (b) the output row is the target the pass writes, socket on the right
    auto *out_row = dynamic_cast<QGraphicsSimpleTextItem *>(
        part(FramePlanRowLabelItem, "HighLuminanceExtraction",
             QStringLiteral("Bloom_Threshold_RT"),
             QStringLiteral("output")));
    REQUIRE(out_row != nullptr);
    REQUIRE(out_row->text().contains(QStringLiteral("Bloom_Threshold_RT")));
    auto *out_socket = dynamic_cast<QGraphicsPathItem *>(
        part(FramePlanSocketItem, "HighLuminanceExtraction",
             QStringLiteral("Bloom_Threshold_RT"),
             QStringLiteral("output")));
    REQUIRE(out_socket != nullptr);
    REQUIRE(std::abs(out_socket->pos().x() - body.width()) < 0.001);
    // A render target is drawn as the table's blue circle.
    REQUIRE(out_socket->path() ==
            socketPath(SocketShape::circle, FramePlanSocketSize));
    REQUIRE(out_socket->brush().color() ==
            QColor{QStringLiteral("#4a86c8")});

    // (c) the input row is the resource the pass reads, socket on the left
    auto *in_socket = dynamic_cast<QGraphicsPathItem *>(
        part(FramePlanSocketItem, "HighLuminanceExtraction",
             QStringLiteral("lit_color"), QStringLiteral("input")));
    REQUIRE(in_socket != nullptr);
    REQUIRE(std::abs(in_socket->pos().x()) < 0.001);
    REQUIRE(in_socket->pos().y() > out_socket->pos().y());
    (void)part(FramePlanRowLabelItem, "HighLuminanceExtraction",
               QStringLiteral("lit_color"), QStringLiteral("input"));

    // (d) the edge leaves the output socket's centre and arrives at the input
    // socket's centre -- not at the node's outline.
    auto *edge = dynamic_cast<QGraphicsPathItem *>(edgeItem(
        logical, "HighLuminanceExtraction", "HorizontalBlur_0"));
    REQUIRE(edge != nullptr);
    QGraphicsItem *downstream =
        part(FramePlanSocketItem, "HorizontalBlur_0",
             QStringLiteral("Bloom_Threshold_RT"), QStringLiteral("input"));
    REQUIRE(edge->path().elementCount() > 0);
    const QPointF start =
        edge->mapToScene(QPointF{edge->path().elementAt(0)});
    REQUIRE(QLineF{start, out_socket->sceneBoundingRect().center()}
                .length() < 0.01);
    REQUIRE(QLineF{edge->mapToScene(edge->path().currentPosition()),
                   downstream->sceneBoundingRect().center()}
                .length() < 0.01);
    // The node outline is no longer an endpoint: the socket sits on a row, so
    // the two answers really are different.
    REQUIRE(QLineF{start, QPointF{node->sceneBoundingRect().right(),
                                  node->sceneBoundingRect().center().y()}}
                .length() > 1.0);

    // Nothing draws an edge label any more; the summary is the edge's tooltip.
    REQUIRE(itemsOfKind(logical, FramePlanEdgeLabelItem).empty());
    REQUIRE_FALSE(edge->toolTip().isEmpty());
}

TEST_CASE(
    "N-3 the frame plan socket shape follows the resource kind and the shipped "
    "table is what answers",
    "[devstudio][frame-plan][node-visual][socket-style][n3]") {
    (void)application();
    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(
        capturedRuntimeFramePlan().dump()));
    targetSelector(widget).setCurrentText(QStringLiteral("swapchain"));
    subtreeDepth(widget).setValue(1);
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);

    const auto socket = [&](const QString &port, const QString &direction) {
        std::vector<QGraphicsPathItem *> matches;
        for (QGraphicsItem *item : itemsOfKind(logical, FramePlanSocketItem)) {
            if (item->data(FramePlanNameRole).toString() !=
                    QStringLiteral("output_transform") ||
                item->data(FramePlanPortRole).toString() != port ||
                item->data(FramePlanSocketDirectionRole).toString() !=
                    direction)
                continue;
            matches.push_back(dynamic_cast<QGraphicsPathItem *>(item));
        }
        REQUIRE(matches.size() == 1);
        REQUIRE(matches.front() != nullptr);
        return matches.front();
    };

    // output_transform reads `display` (a render target) and writes
    // `swapchain` (the frame target). Two kinds, two shapes.
    QGraphicsPathItem *image = socket(QStringLiteral("display"),
                                      QStringLiteral("input"));
    QGraphicsPathItem *frame = socket(QStringLiteral("swapchain"),
                                      QStringLiteral("output"));
    REQUIRE(image->path() ==
            socketPath(SocketShape::circle, FramePlanSocketSize));
    REQUIRE(image->brush().color() == QColor{QStringLiteral("#4a86c8")});
    REQUIRE(frame->path() ==
            socketPath(SocketShape::hexagon, FramePlanSocketSize));
    REQUIRE(frame->brush().color() == QColor{QStringLiteral("#d2704a")});
    REQUIRE(frame->toolTip().contains(
        QStringLiteral("frame_plan.swapchain")));

    // The control: with the shipped default table emptied, no frame_plan.*
    // entry can be answering, and every socket falls back to the white circle.
    const auto redraw = [&] {
        subtreeDepth(widget).setValue(2);
        subtreeDepth(widget).setValue(1);
        QApplication::processEvents();
    };
    setSocketStyleDefaults("{}");
    redraw();
    const auto sockets = itemsOfKind(logical, FramePlanSocketItem);
    REQUIRE_FALSE(sockets.empty());
    for (QGraphicsItem *item : sockets) {
        auto *path = dynamic_cast<QGraphicsPathItem *>(item);
        REQUIRE(path != nullptr);
        REQUIRE(path->path() ==
                socketPath(SocketShape::circle, FramePlanSocketSize));
        REQUIRE(path->brush().color() == QColor{QStringLiteral("#f4f7fa")});
    }
    resetSocketStyleDefaults();
    redraw();
    REQUIRE(socket(QStringLiteral("swapchain"), QStringLiteral("output"))
                ->path() ==
            socketPath(SocketShape::hexagon, FramePlanSocketSize));
}

TEST_CASE(
    "N-3 the shared canvas view zooms at the cursor, fits on F and pans with "
    "the middle button",
    "[devstudio][frame-plan][zoom][fit][n3]") {
    (void)application();
    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.resize(900, 640);
    widget.show();
    widget.receiveResult(QByteArray::fromStdString(
        capturedRuntimeFramePlan().dump()));
    targetSelector(widget).setCurrentText(
        QStringLiteral("Bloom_Threshold_RT"));
    subtreeDepth(widget).setValue(2);
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    QGraphicsView &view = logicalView(widget);
    QWidget *const port = view.viewport();
    REQUIRE(port != nullptr);
    REQUIRE(port->isVisible());

    // (a) the wheel changes the magnification, anchored at the cursor. That the
    // anchored point really does stay still is measured on the shared view
    // itself, below, where the scene rect cannot clamp the scroll.
    const QPoint cursor = port->rect().center() + QPoint{70, 45};
    REQUIRE(view.transformationAnchor() ==
            QGraphicsView::AnchorUnderMouse);
    const qreal before_scale = view.transform().m11();
    QWheelEvent wheel{QPointF{cursor}, QPointF{cursor}, QPoint{},
                      QPoint{0, 240}, Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false};
    QApplication::sendEvent(port, &wheel);
    QApplication::processEvents();
    REQUIRE(view.transform().m11() > before_scale);

    // (b) F fits: the whole content is inside what the viewport shows.
    view.setFocus();
    QTest::keyClick(&view, Qt::Key_F);
    QApplication::processEvents();
    const QRectF visible = view.mapToScene(port->rect()).boundingRect();
    REQUIRE(view.transform().m11() >
            NodeCanvasView::DefaultMinimumZoom + 0.000001);
    REQUIRE(visible.contains(logical.itemsBoundingRect()));

    // (c) the middle button pans, and moves nothing in the scene. Zoom in
    // first: with everything fitted there is nothing left to scroll to, so a
    // pan that worked would still look like a pan that did not.
    for (int turn = 0; turn < 6; ++turn) {
        QWheelEvent in{QPointF{cursor},  QPointF{cursor}, QPoint{},
                       QPoint{0, 240},   Qt::NoButton,    Qt::NoModifier,
                       Qt::NoScrollPhase, false};
        QApplication::sendEvent(port, &in);
    }
    QApplication::processEvents();
    REQUIRE(view.horizontalScrollBar()->maximum() >
            view.horizontalScrollBar()->minimum());
    // Centred first, so the scroll bars have room in both directions and a
    // clamp at one end cannot look like a pan that did nothing.
    view.centerOn(logical.itemsBoundingRect().center());
    QApplication::processEvents();
    const auto positions = sceneNodePositions(logical);
    const QPoint pan_from = port->rect().center();
    const QPoint pan_to = pan_from + QPoint{60, 40};
    const QPointF centre_before = view.mapToScene(pan_from);
    const auto send = [&](QEvent::Type type, const QPoint &at,
                          Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent event{type,
                          QPointF{at},
                          QPointF{at},
                          port->mapToGlobal(QPointF{at}),
                          button,
                          held,
                          Qt::NoModifier};
        QApplication::sendEvent(port, &event);
    };
    send(QEvent::MouseButtonPress, pan_from, Qt::MiddleButton,
         Qt::MiddleButton);
    send(QEvent::MouseMove, pan_to, Qt::NoButton, Qt::MiddleButton);
    send(QEvent::MouseButtonRelease, pan_to, Qt::MiddleButton,
         Qt::NoButton);
    QApplication::processEvents();
    REQUIRE(sceneNodePositions(logical) == positions);
    REQUIRE(QLineF{view.mapToScene(pan_from), centre_before}.length() > 1.0);
}

TEST_CASE(
    "N-3 the shared canvas view fits a scope it has not shown before and "
    "restores one it has",
    "[devstudio][canvas-view][fit][scope][n3]") {
    (void)application();
    QGraphicsScene canvas;
    // Small enough that fitting it stays inside the shared zoom clamp: the
    // clamp is the clamp, and this test is about the fit, not the limit.
    canvas.addRect(QRectF{0.0, 0.0, 800.0, 500.0});
    NodeCanvasView view{&canvas};
    view.resize(420, 320);
    view.show();
    QApplication::processEvents();

    // The wheel magnifies, and it magnifies around the cursor: Qt's
    // AnchorUnderMouse is what fixes the point under the pointer, and it is
    // gated on a real pointer being inside the widget, which the offscreen
    // platform cannot establish. The anchor itself is asserted instead of a
    // pixel that would only be measuring the test harness.
    canvas.setSceneRect(QRectF{-20000.0, -20000.0, 40000.0, 40000.0});
    REQUIRE(view.transformationAnchor() == QGraphicsView::AnchorUnderMouse);
    const QPoint cursor = view.viewport()->rect().center() + QPoint{80, 55};
    const qreal before_wheel = view.transform().m11();
    QWheelEvent wheel{QPointF{cursor},   QPointF{cursor}, QPoint{},
                      QPoint{0, 240},    Qt::NoButton,    Qt::NoModifier,
                      Qt::NoScrollPhase, false};
    QApplication::sendEvent(view.viewport(), &wheel);
    QApplication::processEvents();
    REQUIRE(view.transform().m11() > before_wheel);
    QWheelEvent out{QPointF{cursor},   QPointF{cursor}, QPoint{},
                    QPoint{0, -240},   Qt::NoButton,    Qt::NoModifier,
                    Qt::NoScrollPhase, false};
    QApplication::sendEvent(view.viewport(), &out);
    QApplication::processEvents();
    REQUIRE(std::abs(view.transform().m11() - before_wheel) < 0.000001);
    canvas.setSceneRect(QRectF{});

    view.showScope(QStringLiteral("root"));
    const QRectF root_visible =
        view.mapToScene(view.viewport()->rect()).boundingRect();
    REQUIRE(root_visible.contains(canvas.itemsBoundingRect()));
    const qreal root_scale = view.transform().m11();
    REQUIRE(view.property("pelicanFitCount").toULongLong() == 1);

    // A different scope, whose content is nowhere near the previous one: this
    // is the control for the fit. Without it the view keeps looking where it
    // was, and the visible rect does not even intersect the new content.
    canvas.clear();
    canvas.addRect(QRectF{6000.0, 5000.0, 300.0, 220.0});
    canvas.setSceneRect(canvas.itemsBoundingRect().adjusted(-4000.0, -4000.0,
                                                            30.0, 30.0));
    QApplication::processEvents();
    const QRectF unfitted =
        view.mapToScene(view.viewport()->rect()).boundingRect();
    REQUIRE_FALSE(unfitted.intersects(canvas.itemsBoundingRect()));
    view.showScope(QStringLiteral("inner"));
    const QRectF inner_visible =
        view.mapToScene(view.viewport()->rect()).boundingRect();
    REQUIRE(inner_visible.contains(canvas.itemsBoundingRect()));
    REQUIRE(view.property("pelicanFitCount").toULongLong() == 2);

    // Coming back to a scope the session has already shown restores the zoom
    // it was left at rather than fitting again.
    view.showScope(QStringLiteral("root"));
    REQUIRE(std::abs(view.transform().m11() - root_scale) < 0.000001);
    REQUIRE(view.property("pelicanFitCount").toULongLong() == 2);
}

TEST_CASE(
    "WP318 selected target preserves every WP307 physical fact and scopes both alias outcomes",
    "[devstudio][frame-plan][physical-overlay][target][wp318]") {
    (void)application();
    Json wire = capturedRuntimeFramePlan();
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
    showFirstTarget(widget);
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
    showFirstTarget(widget);
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
    showFirstTarget(widget);
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
    showFirstTarget(widget);
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
    Json wire = capturedRuntimeFramePlan();
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
    "WP354 Studio shader details materialize logical refs only through the Open action seam",
    "[devstudio][frame-plan][wp354][shader-resolution][widget]") {
    (void)application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = std::filesystem::path{
        temporary.path().toStdWString()};
    std::filesystem::create_directories(root / "shaders");
    {
        std::ofstream project{root / "project.json", std::ios::binary};
        project << Json{
            {"schema", "pelican.project"},
            {"version", 1},
            {"name", "wp354-widget"},
        }.dump();
        std::ofstream vertex{root / "shaders" / "effective.vert",
                             std::ios::binary};
        vertex << "#version 450\nvoid main(){}\n";
        std::ofstream fragment{root / "shaders" / "effective.frag",
                               std::ios::binary};
        fragment << "#version 450\nvoid main(){}\n";
    }

    const auto stage = [](std::string name, std::string suffix) {
        return Json{
            {"stage", std::move(name)},
            {"declared_ref", "shaders/declared"},
            {"effective_ref", "shaders/effective"},
            {"origin", "provider"},
            {"source_open_ref",
             "project://shaders/effective." + std::move(suffix)},
        };
    };
    Json wire{
        {"schema", "pelican.frame_plan"},
        {"version", 1},
        {"profile", "runtime"},
        {"graph", "wp354_widget"},
        {"nodes",
         Json::array({
             {{"name", "provided"},
              {"kind", "render"},
              {"declaration_index", 0},
              {"order", 0},
              {"level", 0},
              {"reads", Json::array()},
              {"writes", Json::array()},
              {"shader_resolution",
               {{"state", "resolved"},
                {"stages",
                 Json::array({stage("vertex", "vert"),
                              stage("fragment", "frag")})}}}},
             {{"name", "shadow"},
              {"kind", "render"},
              {"declaration_index", 1},
              {"order", 1},
              {"level", 1},
              {"reads", Json::array()},
              {"writes", Json::array()},
              {"shader_resolution",
               {{"state", "resolved"},
                {"stages",
                 Json::array({
                     {{"stage", "vertex"},
                      {"effective_ref", "engine://shadow_depth"},
                      {"origin", "engine_default"},
                      {"source_open_ref", nullptr},
                      {"source_open_reason",
                       "embedded_engine_resource"}},
                 })}}}},
         })},
        {"barriers", Json::array()},
        {"resources", Json::array()},
    };

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.setProjectRoot(root);
    std::optional<std::filesystem::path> opened;
    widget.setShaderSourceOpenAction(
        [&](const std::filesystem::path &path) {
            opened = path;
            return true;
        });
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();

    const auto buttons =
        widget.findChildren<QPushButton *>(
            QStringLiteral("pelican.shaderSourceOpen"));
    REQUIRE(buttons.size() == 3);
    QPushButton *vertex_open = nullptr;
    QPushButton *shadow_disabled = nullptr;
    for (auto *button : buttons) {
        if (button->property("pelicanNode") ==
                QStringLiteral("provided") &&
            button->property("pelicanStage") ==
                QStringLiteral("vertex")) {
            vertex_open = button;
        }
        if (button->property("pelicanNode") ==
            QStringLiteral("shadow")) {
            shadow_disabled = button;
        }
    }
    REQUIRE(vertex_open != nullptr);
    REQUIRE(vertex_open->isEnabled());
    REQUIRE(vertex_open->property("pelicanSourceOpenRef") ==
            QStringLiteral("project://shaders/effective.vert"));
    vertex_open->click();
    REQUIRE(opened == std::filesystem::canonical(
                          root / "shaders" / "effective.vert"));

    REQUIRE(shadow_disabled != nullptr);
    REQUIRE_FALSE(shadow_disabled->isEnabled());
    REQUIRE(shadow_disabled->text() ==
            QStringLiteral("embedded_engine_resource"));
}

TEST_CASE(
    "WP367a Studio displays and opens production anyhit physical slots",
    "[WP367][WP367a][devstudio][frame-plan][shader-resolution][widget][production][mutation]") {
    (void)application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = std::filesystem::path{
        temporary.path().toStdWString()};
    std::filesystem::create_directories(root / "shaders");
    {
        std::ofstream project{root / "project.json", std::ios::binary};
        project << Json{
            {"schema", "pelican.project"},
            {"version", 1},
            {"name", "wp367a-widget"},
        }.dump();
    }
    for (const auto file : {
             "trace.rgen", "open.rmiss", "hit1.rchit",
             "filter1.rahit"}) {
        std::ofstream shader_file{root / "shaders" / file,
                                  std::ios::binary};
        shader_file << "#version 460\nvoid main() {}\n";
    }

    Pelican::ComputeTaskDefinition task;
    task.name = "trace";
    task.shader_declaration_parsed = true;
    task.ray_tracing = Pelican::RayTracingTaskShaderDefinition{
        .raygen = Pelican::makeShaderReference(
            "shaders/trace", Pelican::ShaderStage::raygen),
        .misses = {Pelican::makeShaderReference(
            "shaders/open", Pelican::ShaderStage::miss)},
        .uses_group_layout = true,
        .traced_ray_type_indices = {1},
        .hit_shader_slots = {
            std::nullopt,
            Pelican::makeShaderReference(
                "shaders/hit1", Pelican::ShaderStage::closesthit)},
        .any_hit_shader_slots = {
            std::nullopt,
            Pelican::makeShaderReference(
                "shaders/filter1", Pelican::ShaderStage::anyhit)},
    };
    task.declared_shader_refs = {
        {Pelican::DeclaredShaderStage::raygen, std::nullopt,
         "shaders/trace"},
        {Pelican::DeclaredShaderStage::miss, 0, "shaders/open"},
        {Pelican::DeclaredShaderStage::closesthit, 1, "shaders/hit1"},
        {Pelican::DeclaredShaderStage::anyhit, 1, "shaders/filter1"},
    };
    Pelican::CompiledRenderingPass compiled;
    compiled.compute_tasks.push_back(Pelican::CompiledComputeTask{
        .definition = std::move(task),
        .task_id = Pelican::ComputeTaskId{0},
    });
    Json wire{
        {"schema", "pelican.frame_plan"},
        {"version", 1},
        {"graph", "wp367a_widget"},
        {"nodes", Json::array({
             {{"name", "trace"},
              {"kind", "compute"},
              {"declaration_index", 0},
              {"order", 0},
              {"level", 0},
              {"reads", Json::array()},
              {"writes", Json::array()}},
         })},
        {"barriers", Json::array()},
        {"resources", Json::array()},
    };
    Pelican::PathResolver resolver;
    resolver.setup(root, false);
    Pelican::appendShaderResolution(wire, compiled, resolver);
    const auto &production_resolution =
        wire.at("nodes").at(0).at("shader_resolution");
    REQUIRE(production_resolution.at("hit_slot_count") == 2);
    REQUIRE(production_resolution.at("closesthit_slots").size() == 1);
    REQUIRE(production_resolution.at("anyhit_slots").at(0).at("hit_slot") ==
            1);

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.setProjectRoot(root);
    std::optional<std::filesystem::path> opened;
    widget.setShaderSourceOpenAction(
        [&](const std::filesystem::path &path) {
            opened = path;
            return true;
        });
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();

    qsizetype anyhit_label_count = 0;
    for (const auto *tree : widget.findChildren<QTreeWidget *>()) {
        anyhit_label_count += tree->findItems(
            QStringLiteral("anyhit[hit_slot=1]"),
            Qt::MatchExactly | Qt::MatchRecursive, 1).size();
    }
    CHECK(anyhit_label_count == 1);

    const auto buttons = widget.findChildren<QPushButton *>(
        QStringLiteral("pelican.shaderSourceOpen"));
    const auto anyhit_open = std::find_if(
        buttons.begin(), buttons.end(), [](const auto *button) {
            return button->property("pelicanNode") ==
                       QStringLiteral("trace") &&
                   button->property("pelicanStage") ==
                       QStringLiteral("anyhit") &&
                   button->property("pelicanHitSlot").toULongLong() == 1;
        });
    REQUIRE(anyhit_open != buttons.end());
    REQUIRE((*anyhit_open)->isEnabled());
    REQUIRE((*anyhit_open)->property("pelicanSourceOpenRef") ==
            QStringLiteral("project://shaders/filter1.rahit"));
    (*anyhit_open)->click();
    REQUIRE(opened == std::filesystem::canonical(
                          root / "shaders" / "filter1.rahit"));
    wire["nodes"][0]["authored_name"] = "source_trace";
    wire["nodes"][0]["shader_resolution"]["closesthit_slots"][0]["group_name"] = "surface";
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    QString edited_task;
    widget.setTaskEditAction([&](const QString &name) { edited_task = name; });
    widget.openTasksForHitGroup(QStringLiteral("absent"));
    CHECK(edited_task.isEmpty());
    widget.openTasksForHitGroup(QStringLiteral("surface"));
    CHECK(edited_task == QStringLiteral("source_trace"));
    wire["nodes"][0]["runtime_callable_records"] = {
        {"source", "runtime_cpu_settings"},
        {"records", Json::array({{{"index", 1}, {"fields", Json::array({
            {{"name", "increment"}, {"type", "u32"}, {"offset", 0}, {"value", 400}}})}}})}};
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    qsizetype record_fields = 0;
    for (const auto *tree : widget.findChildren<QTreeWidget *>()) {
        const auto fields = tree->findItems(QStringLiteral("increment (u32, offset 0)"),
            Qt::MatchExactly | Qt::MatchRecursive, 1);
        for (const auto *field : fields) {
            CHECK(field->text(2) == QStringLiteral("400"));
            ++record_fields;
        }
    }
    CHECK(record_fields == 1);
}


TEST_CASE(
    "WP320 a frame plan arriving mid-drag does not destabilise the scene",
    "[devstudio][frame-plan][drag][refresh][wp320]") {
    (void)application();
    Json wire = capturedRuntimeFramePlan();
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
    // F-7: the whole frame is wider than the view; bring the node into it.
    view.centerOn(nodes.front());
    QApplication::processEvents();
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


TEST_CASE("Task connection graph reconnects a selected port group and rejects resource kind mismatch",
          "[studio][task-connection-graph]") {
    application();
    TaskConnectionGraphics view;
    view.resize(900, 300);
    const std::vector<TaskConnectionGraphics::Resource> resources{
        {"image_a", "image"}, {"image_b", "image"}, {"args", "buffer"}};
    Json draft{{"resource_ports", {{"input", "image_a"}, {"alias", "image_a"}}},
               {"reads", {"image_a"}}};
    int edits = 0;
    QString diagnostic;
    view.resourceRebound = [&](QString from, QString to) {
        draft = rebindTaskResources(draft, {{from.toStdString(), to.toStdString()}});
        ++edits;
        view.setConnections({{to, {"input", "alias"}}}, resources);
    };
    view.diagnostic = [&](QString message) { diagnostic = message; };
    view.setConnections({{"image_a", {"input", "alias"}}, {"missing", {"unresolved_port"}}}, resources);
    view.show();
    QApplication::processEvents();
    const auto click = [&](const QString &name, int side) {
        QGraphicsItem *target = nullptr;
        for (auto *item : view.scene()->items()) {
            if (item->data(TaskConnectionNameRole).toString() == name &&
                item->data(TaskConnectionSideRole).isValid() &&
                item->data(TaskConnectionSideRole).toInt() == side) target = item;
        }
        REQUIRE(target != nullptr);
        view.ensureVisible(target);
        QApplication::processEvents();
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier,
                          view.mapFromScene(target->sceneBoundingRect().center()));
    };
    click("image_b", 2);
    CHECK(edits == 0);
    click("image_a", 1);
    click("args", 2);
    CHECK(edits == 0);
    CHECK_FALSE(diagnostic.isEmpty());
    click("missing", 0);
    CHECK(edits == 0);
    click("image_b", 2);
    REQUIRE(edits == 1);
    CHECK(draft.at("resource_ports").at("input") == "image_b");
    CHECK(draft.at("resource_ports").at("alias") == "image_b");
    CHECK(draft.at("reads") == Json::array({"image_b"}));
    view.setTargetFilter({"image_a"});
    for (auto *item : view.scene()->items())
        CHECK_FALSE((item->data(TaskConnectionNameRole).toString() == "args" &&
                     item->data(TaskConnectionSideRole).toInt() == 2));
    click("image_b", 1);
    click("image_a", 2);
    CHECK(edits == 2);
    CHECK(draft.at("resource_ports").at("input") == "image_a");
}

TEST_CASE("Frame Plan Shift drag requests a connection without moving nodes and Escape cancels",
          "[devstudio][frame-plan][connection-drag]") {
    application();
    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.resize(1400, 900);
    widget.show();
    auto wire = mixedMembershipFramePlan(false);
    for (auto &node : wire["nodes"]) {
        if (node.at("name") == "wp343_mixed_b") {
            node["kind"] = "compute";
            node["authored_name"] = "authored_consumer";
        }
    }
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    targetSelector(widget).setCurrentText("wp343_mixed_d_output");
    subtreeDepth(widget).setValue(64);
    QApplication::processEvents();
    auto &logical = dynamic_cast<FramePlanGraphicsScene &>(scene(widget));
    auto &view = logicalView(widget);
    auto *first = nodeItem(logical, "wp343_mixed_a");
    auto *second = nodeItem(logical, "wp343_mixed_b");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    const auto base = logical.itemsBoundingRect().bottomRight() + QPointF{400, 200};
    first->setPos(base);
    second->setPos(base + QPointF{400, 0});
    QApplication::processEvents();
    view.fitInView(first->sceneBoundingRect().united(second->sceneBoundingRect()).adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio);
    const auto firstPosition = first->pos();
    const auto secondPosition = second->pos();
    int requests = 0;
    widget.setTaskConnectionAction([&](const QString &task, const QStringList &outputs) {
        CHECK(task == "authored_consumer");
        CHECK(outputs == QStringList{"wp343_mixed_a_output"});
        ++requests;
    });
    const auto start = view.mapFromScene(first->sceneBoundingRect().center());
    const auto end = view.mapFromScene(second->sceneBoundingRect().center());
    REQUIRE(view.viewport()->rect().contains(start));
    REQUIRE(view.viewport()->rect().contains(end));
    const auto gesture = [&](QPoint destination, bool cancel) {
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, start);
        QTest::mouseMove(view.viewport(), (start + destination) / 2, 1);
        if (cancel) QTest::keyClick(view.viewport(), Qt::Key_Escape);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, destination);
        QApplication::processEvents();
    };
    gesture(end, false);
    CHECK(requests == 1);
    CHECK(first->pos() == firstPosition);
    CHECK(second->pos() == secondPosition);

    // N-3: a socket is part of the node it hangs on, so starting the same
    // gesture on the output socket rather than on the body does the same
    // thing. The socket is what the user will aim at now that there is one.
    QGraphicsItem *socket = nullptr;
    for (QGraphicsItem *item : itemsOfKind(logical, FramePlanSocketItem)) {
        if (item->data(FramePlanNameRole).toString() ==
                QStringLiteral("wp343_mixed_a") &&
            item->data(FramePlanSocketDirectionRole).toString() ==
                QStringLiteral("output")) {
            socket = item;
            break;
        }
    }
    REQUIRE(socket != nullptr);
    REQUIRE(socket->parentItem() == first);
    const QPoint from_socket =
        view.mapFromScene(socket->sceneBoundingRect().center());
    REQUIRE(view.viewport()->rect().contains(from_socket));
    REQUIRE(logical.itemAt(socket->sceneBoundingRect().center(),
                           view.transform()) == socket);
    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::ShiftModifier,
                      from_socket);
    QTest::mouseMove(view.viewport(), (from_socket + end) / 2, 1);
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::ShiftModifier,
                        end);
    QApplication::processEvents();
    CHECK(requests == 2);
    CHECK(first->pos() == firstPosition);
    CHECK(second->pos() == secondPosition);

    gesture(end, true);
    CHECK(requests == 2);
    gesture(start, false);
    CHECK(requests == 2);
    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, end);
    QTest::mouseMove(view.viewport(), (start + end) / 2, 1);
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, start);
    QApplication::processEvents();
    CHECK(requests == 2); // A raster pass is not an editable task destination.
    int pass_requests = 0;
    widget.setPresetPassConnectionAction([&](const QString &graph, const QString &pass, const QStringList &outputs) {
        CHECK(graph.toStdString() == wire.at("graph").get<std::string>());
        CHECK(pass == "wp343_mixed_a");
        CHECK(outputs == QStringList{"wp343_mixed_b_output"});
        ++pass_requests;
    });
    QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, end);
    QTest::mouseMove(view.viewport(), (start + end) / 2, 1);
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, start);
    QApplication::processEvents();
    CHECK(pass_requests == 1);
    CHECK(first->pos() == firstPosition);
    CHECK(second->pos() == secondPosition);
}


// --- N-8. Editing on the Frame Plan canvas.

namespace {

// The closed set of authoring calls the canvas is allowed to make, recorded.
// A test reads the method name and the exact payload, which is the whole point
// of building the payload outside the dialogs.
struct FramePlanEditRecorder {
    qint64 next_id = 1000;
    qint64 context_id = 0;
    qint64 task_id = 0;
    qint64 submit_id = 0;
    qint64 ticket_id = 0;
    QString method;
    QJsonObject params;
    int submits = 0;
    // Every method sent, in order (G-1: Save can be two).
    std::vector<QString> methods;
    FramePlanEditCapability::ResultHandler result;
    FramePlanEditCapability::FailureHandler failure;

    FramePlanEditCapability capability() {
        FramePlanEditCapability capability;
        capability.requestAuthoringContext = [this](QString *) {
            context_id = next_id++;
            return context_id;
        };
        capability.requestTaskAuthoring = [this](QString *) {
            task_id = next_id++;
            return task_id;
        };
        const auto record = [this](const char *name) {
            return [this, name](const QJsonObject &sent, QString *) {
                method = QString::fromLatin1(name);
                methods.push_back(method);
                params = sent;
                ++submits;
                submit_id = next_id++;
                return submit_id;
            };
        };
        capability.editPresetPass = record("edit_preset_pass");
        capability.removeAuthoredPass = record("remove_authored_pass");
        capability.editComputeTask = record("edit_compute_task");
        capability.removeComputeTask = record("remove_compute_task");
        capability.editRenderTargets = record("edit_render_targets");
        capability.addAuthoredPass = record("add_authored_pass");
        capability.decomposeMaterialPass = record("decompose_material_pass");
        // UX-15b.
        capability.editVulkanPlanPins = record("edit_vulkan_plan_pins");
        capability.putVulkanPhysicalFragment = record("put_vulkan_physical_fragment");
        capability.requestEditResult = [this](const QString &, QString *) {
            ticket_id = next_id++;
            return ticket_id;
        };
        capability.result = [this](QObject *,
                                   FramePlanEditCapability::ResultHandler h) {
            result = std::move(h);
        };
        capability.failure = [this](QObject *,
                                    FramePlanEditCapability::FailureHandler h) {
            failure = std::move(h);
        };
        return capability;
    }
};

constexpr auto RootDigest =
    "1111111111111111111111111111111111111111111111111111111111111111";
constexpr auto PresetDigest =
    "2222222222222222222222222222222222222222222222222222222222222222";
constexpr auto TaskDigest =
    "3333333333333333333333333333333333333333333333333333333333333333";

// The declaration the WP343 fixture's feature really carries for one pass, so
// the payload the canvas sends can be compared against it field by field.
Json mixedPassDeclaration(std::size_t index) {
    const std::string name{MixedFeatureNodes[index]};
    Json declaration{
        {"name", name},
        {"type", "fullscreen"},
        {"output", {{"color", name + "_output"}, {"depth", nullptr}}},
        {"shader", {{"vertex", "engine://fullscreen"},
                    {"fragment", "engine://fullscreen"}}}};
    if (index != 0)
        declaration["input"] = Json::array(
            {std::string{MixedFeatureNodes[index - 1]} + "_output"});
    return declaration;
}

// One authoring context over the WP343 graph: `a` is an editable project
// preset pass, `c` is a managed authored fragment with no editable
// declaration, `d` comes from a feature that has not been materialized.
Json mixedAuthoringContext(const std::string &graph) {
    auto passes = Json::array();
    for (std::size_t index = 0; index < MixedFeatureNodes.size(); ++index) {
        const std::string name{MixedFeatureNodes[index]};
        Json pass{{"name", name},
                  {"declaration_index", index},
                  {"declaration", mixedPassDeclaration(index)},
                  {"editable_preset_pass", false},
                  {"editable_feature_pass", false}};
        if (name == "wp343_mixed_a") {
            pass["editable_preset_pass"] = true;
            pass["preset_declaration"] = mixedPassDeclaration(index);
            pass["provenance"] = {{"source", "preset"},
                                  {"provider_feature", ""},
                                  {"provider_reference", ""}};
        } else if (name == "wp343_mixed_c") {
            pass["provenance"] = {
                {"source", "feature"},
                {"provider_feature", "wp343_mixed_membership"},
                {"provider_reference", "project://passes/mixed.json"}};
            pass["managed"] = true;
        } else if (name == "wp343_mixed_d") {
            pass["provenance"] = {
                {"source", "feature"},
                {"provider_feature", "wp343_mixed_membership"},
                {"provider_reference", "engine://features/mixed.json"}};
        }
        passes.push_back(std::move(pass));
    }
    auto anchors = Json::array();
    anchors.push_back({{"position", 0},
                       {"insert", "before:" +
                                      std::string{MixedFeatureNodes.front()}}});
    for (std::size_t index = 0; index < MixedFeatureNodes.size(); ++index)
        anchors.push_back(
            {{"position", static_cast<int>(index) + 1},
             {"insert", "after:" + std::string{MixedFeatureNodes[index]}}});
    return Json{
        {"source_digest", {{"hex", RootDigest}}},
        {"pipeline_preset",
         {{"reference", "project://passes/preset.json"},
          {"source_digest", {{"hex", PresetDigest}}}}},
        {"graphs", Json::array({Json{{"name", graph},
                                     {"passes", std::move(passes)},
                                     {"anchor_candidates", std::move(anchors)}}})}};
}

Json authoredTaskCatalog(Json reads = Json::array()) {
    Json declaration{{"name", "authored_consumer"},
                     {"shader", "project://shaders/consume.comp"},
                     {"resource_ports", Json::object()},
                     {"reads", std::move(reads)},
                     {"dispatch", {{"groups", Json::array({1, 1, 1})}}}};
    return Json{{"source_digest", {{"hex", RootDigest}}},
                {"tasks", Json::array({Json{
                     {"name", "authored_consumer"},
                     {"kind", "compute"},
                     {"source_reference", "project://passes/task.json"},
                     {"editable", true},
                     {"declaration", std::move(declaration)},
                     {"source_digest", {{"hex", TaskDigest}}}}})},
                {"resources", Json::array()}};
}

QGraphicsItem *socketItem(QGraphicsScene &value, const QString &node,
                          const QString &resource, const QString &direction) {
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanSocketItem)) {
        if (item->data(FramePlanNameRole).toString() == node &&
            item->data(FramePlanPortRole).toString() == resource &&
            item->data(FramePlanSocketDirectionRole).toString() == direction)
            return item;
    }
    return nullptr;
}

QGraphicsItem *bandItem(QGraphicsScene &value, const QString &node,
                        const QString &field) {
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanInlineFieldItem)) {
        if (item->data(FramePlanNameRole).toString() == node &&
            item->data(FramePlanInlineFieldRole).toString() == field)
            return item;
    }
    return nullptr;
}

QString statusText(FramePlanWidget &widget) {
    auto *status = widget.findChild<QLabel *>(
        QStringLiteral("pelican.framePlanStatus"));
    return status == nullptr ? QString{} : status->text();
}

// The WP343 fixture with `wp343_mixed_b` standing in for an authored compute
// task, exactly as the Shift-drag test arranges it.
Json mixedEditingWire() {
    Json wire = mixedMembershipFramePlan(false);
    for (auto &node : wire["nodes"]) {
        if (node.at("name") == "wp343_mixed_b") {
            node["kind"] = "compute";
            node["authored_name"] = "authored_consumer";
        }
    }
    return wire;
}

struct EditingFixture {
    EmbeddedViewport viewport;
    FramePlanEditRecorder recorder;
    FramePlanWidget widget{&viewport};
    Json wire = mixedEditingWire();
    Json catalog = authoredTaskCatalog();
    std::string graph;
    // Q-10: the rendering document the authoring context names, when a test
    // needs the canvas to know it.
    std::string root_reference;
    // VI-3a: a change to the authoring context before it is handed over.
    std::function<void(Json &)> tweak_context;

    void open() {
        widget.resize(1400, 900);
        widget.show();
        widget.setEditCapability(recorder.capability());
        widget.receiveResult(QByteArray::fromStdString(wire.dump()));
        graph = wire.at("graph").get<std::string>();
        targetSelector(widget).setCurrentText("wp343_mixed_d_output");
        subtreeDepth(widget).setValue(64);
        QApplication::processEvents();
        REQUIRE(recorder.result);
        auto context = mixedAuthoringContext(graph);
        if (!root_reference.empty()) context["source_reference"] = root_reference;
        if (tweak_context) tweak_context(context);
        recorder.result(recorder.context_id,
                        QByteArray::fromStdString(context.dump()));
        recorder.result(recorder.task_id,
                        QByteArray::fromStdString(catalog.dump()));
        QApplication::processEvents();
    }

    FramePlanGraphicsScene &logical() {
        return dynamic_cast<FramePlanGraphicsScene &>(scene(widget));
    }

    // One plain drag: press on a socket, move, release over a destination.
    void dragSocket(const QString &node, const QString &resource,
                    const QString &direction, QGraphicsItem *destination) {
        auto &view = logicalView(widget);
        QGraphicsItem *socket =
            socketItem(logical(), node, resource, direction);
        REQUIRE(socket != nullptr);
        const QRectF span =
            destination == nullptr
                ? socket->sceneBoundingRect().adjusted(-600, -400, 600, 400)
                : socket->sceneBoundingRect().united(
                      destination->sceneBoundingRect());
        view.fitInView(span.adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
        QApplication::processEvents();
        const QPoint start =
            view.mapFromScene(socket->sceneBoundingRect().center());
        const QPoint end =
            destination == nullptr
                ? QPoint{view.viewport()->rect().right() - 4,
                         view.viewport()->rect().bottom() - 4}
                : view.mapFromScene(
                      destination->sceneBoundingRect().center());
        REQUIRE(view.viewport()->rect().contains(start));
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier,
                          start);
        QTest::mouseMove(view.viewport(), (start + end) / 2, 1);
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier,
                            end);
        QApplication::processEvents();
    }
};

} // namespace

TEST_CASE("N-8 a plain drag between sockets edits the declaration through one RPC",
          "[devstudio][frame-plan][n8][canvas-edit]") {
    application();
    EditingFixture fixture;
    fixture.open();
    auto &logical = fixture.logical();
    REQUIRE(nodeItem(logical, "wp343_mixed_a") != nullptr);
    REQUIRE(nodeItem(logical, "wp343_mixed_d") != nullptr);

    SECTION("an editable pass gains exactly one input and keeps every other field") {
        fixture.dragSocket(QStringLiteral("wp343_mixed_d"),
                           QStringLiteral("wp343_mixed_d_output"),
                           QStringLiteral("output"),
                           nodeItem(logical, "wp343_mixed_a"));
        REQUIRE(fixture.recorder.submits == 1);
        CHECK(fixture.recorder.method == QStringLiteral("edit_preset_pass"));
        const auto sent = Json::parse(
            QJsonDocument(fixture.recorder.params).toJson().toStdString());
        CHECK(sent.at("graph") == fixture.graph);
        CHECK(sent.at("pass_name") == "wp343_mixed_a");
        CHECK(sent.at("base_source_digest") == RootDigest);
        CHECK(sent.at("base_preset_digest") == PresetDigest);
        // One more input than the declaration had, and it is the dragged one.
        const auto declaration = mixedPassDeclaration(0);
        CHECK(sent.at("input") == Json::array({"wp343_mixed_d_output"}));
        // The control: everything else is the current declaration, verbatim.
        CHECK(sent.at("output") == declaration.at("output"));
        CHECK(sent.at("shader") == declaration.at("shader"));
        CHECK_FALSE(sent.contains("draw"));
        CHECK_FALSE(sent.contains("raster_state"));

        // The reply path: accepted, then committed, then a refresh.
        fixture.recorder.result(
            fixture.recorder.submit_id,
            QByteArray::fromStdString(
                Json{{"status", "accepted"}, {"ticket", "t-1"}}.dump()));
        QApplication::processEvents();
        REQUIRE(fixture.recorder.ticket_id != 0);
        fixture.recorder.result(
            fixture.recorder.ticket_id,
            QByteArray::fromStdString(
                Json{{"status", "committed"}, {"committed", true}}.dump()));
        QApplication::processEvents();
        CHECK(statusText(fixture.widget).contains(QStringLiteral("committed")));
    }

    SECTION("a pass that cannot be edited refuses by name and sends nothing") {
        fixture.dragSocket(QStringLiteral("wp343_mixed_a"),
                           QStringLiteral("wp343_mixed_a_output"),
                           QStringLiteral("output"),
                           nodeItem(logical, "wp343_mixed_d"));
        CHECK(fixture.recorder.submits == 0);
        const auto notice = statusText(fixture.widget);
        CHECK(notice.contains(QStringLiteral("wp343_mixed_d")));
        CHECK(notice.contains(QStringLiteral("materialize")));
    }

    SECTION("an input socket dragged into open canvas reaches that pass") {
        fixture.dragSocket(QStringLiteral("wp343_mixed_d"),
                           QStringLiteral("wp343_mixed_c_output"),
                           QStringLiteral("input"), nullptr);
        // wp343_mixed_d is not editable, so the cut is refused by name -- the
        // gesture reached the right pass, which is what this asserts.
        CHECK(fixture.recorder.submits == 0);
        CHECK(statusText(fixture.widget)
                  .contains(QStringLiteral("wp343_mixed_d")));
    }

    SECTION("a drag onto an authored compute task edits the task once") {
        fixture.dragSocket(QStringLiteral("wp343_mixed_a"),
                           QStringLiteral("wp343_mixed_a_output"),
                           QStringLiteral("output"),
                           nodeItem(logical, "wp343_mixed_b"));
        REQUIRE(fixture.recorder.submits == 1);
        CHECK(fixture.recorder.method == QStringLiteral("edit_compute_task"));
        const auto sent = Json::parse(
            QJsonDocument(fixture.recorder.params).toJson().toStdString());
        CHECK(sent.at("task_name") == "authored_consumer");
        CHECK(sent.at("base_task_source_digest") == TaskDigest);
        CHECK(sent.at("task").at("reads") ==
              Json::array({"wp343_mixed_a_output"}));
        // Everything else in the task declaration is the one that was read.
        CHECK(sent.at("task").at("shader") == "project://shaders/consume.comp");
        CHECK(sent.at("task").at("dispatch") ==
              Json{{"groups", Json::array({1, 1, 1})}});
    }
}

// --- VI-3a: a pass's input sockets come from its shader's declarations, and
// a drop connects by binding number.

namespace {

constexpr auto Vi3aPass = "wp343_mixed_a";

Json vi3aDeclaration(std::size_t binding, const std::string &name,
                     bool variadic = false,
                     const std::string &type = "sampled_image") {
    return Json{{"binding", binding},
                {"type", type},
                {"name", name},
                {"variadic", variadic}};
}

// VI-1's example 2: three fixed inputs, a variadic group (3, 8), and a
// storage buffer after the group's last binding.
Json vi3aPassInputs() {
    auto declarations = Json::array();
    for (const char *name : {"in_a", "in_b", "in_c"})
        declarations.push_back(vi3aDeclaration(declarations.size(), name));
    for (std::size_t slot = 0; slot < 8; ++slot)
        declarations.push_back(vi3aDeclaration(
            3 + slot, "pelican_variadic_input_" + std::to_string(slot), true));
    declarations.push_back(
        vi3aDeclaration(11, "vi1_value", false, "storage_buffer"));
    return Json{{"state", "declared"},
                {"fragment", "project://shaders/vi"},
                {"declarations", std::move(declarations)},
                {"variadic_range", {{"first", 3}, {"max", 8}}},
                {"pass_texture_inputs", 0},
                {"pass_buffer_inputs", 0},
                {"buffer_binding_base", 11}};
}

Json vi3aFixedInputs(std::vector<std::string> names) {
    auto declarations = Json::array();
    for (const auto &name : names)
        declarations.push_back(vi3aDeclaration(declarations.size(), name));
    return Json{{"state", "declared"},
                {"fragment", "project://shaders/vi"},
                {"declarations", std::move(declarations)},
                {"variadic_range", nullptr},
                {"pass_texture_inputs", 0},
                {"pass_buffer_inputs", 0},
                {"buffer_binding_base", names.size()}};
}

void setVi3aPassInputs(Json &wire, const Json &pass_inputs) {
    for (auto &node : wire["nodes"])
        if (node.at("name") == Vi3aPass) node["pass_inputs"] = pass_inputs;
}

// The WP343 editing wire with `a` drawing through a shader that publishes
// `pass_inputs`, and two buffers the plan declares.
FramePlanModel vi3aModel(const Json &pass_inputs = vi3aPassInputs()) {
    Json wire = mixedEditingWire();
    setVi3aPassInputs(wire, pass_inputs);
    auto model = buildFramePlanModel(wire.dump());
    for (const char *name : {"vi_buffer", "vi_buffer2"}) {
        FramePlanResource buffer;
        buffer.name = name;
        buffer.kind = "buffer";
        model.resources.push_back(std::move(buffer));
    }
    return model;
}

const FramePlanNode &vi3aNode(const FramePlanModel &model,
                              std::string_view name = Vi3aPass) {
    const auto found =
        std::ranges::find(model.nodes, std::string{name}, &FramePlanNode::name);
    REQUIRE(found != model.nodes.end());
    return *found;
}

// `a`'s input list, in the composed declaration and the preset's own.
void setVi3aInputs(Json &context, const std::vector<std::string> &inputs) {
    for (auto &graph : context["graphs"])
        for (auto &pass : graph["passes"])
            if (pass.at("name") == Vi3aPass) {
                pass["declaration"]["input"] = inputs;
                pass["preset_declaration"]["input"] = inputs;
            }
}

FramePlanAuthoring vi3aAuthoring(const std::string &graph,
                                 const std::vector<std::string> &inputs) {
    auto context = mixedAuthoringContext(graph);
    setVi3aInputs(context, inputs);
    return framePlanAuthoringFromContext(context);
}

using Vi3aRow = std::tuple<std::string, std::string, std::optional<std::size_t>,
                           bool>;

std::vector<Vi3aRow> vi3aRows(const FramePlanInputSockets &sockets) {
    std::vector<Vi3aRow> rows;
    for (const auto &socket : sockets.sockets)
        rows.emplace_back(socket.key(), socket.label, socket.input_index,
                          socket.buffer);
    return rows;
}

QGraphicsItem *declaredSocket(QGraphicsScene &value, const QString &node,
                              const QString &key) {
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanSocketItem))
        if (item->data(FramePlanNameRole).toString() == node &&
            item->data(FramePlanSocketKeyRole).toString() == key)
            return item;
    return nullptr;
}

} // namespace

TEST_CASE("VI-3a a pass's input sockets come from its shader's declarations, "
          "in binding order; a pass without them keeps its rows",
          "[devstudio][frame-plan][vi3a][sockets]") {
    const auto model = vi3aModel();
    const auto &node = vi3aNode(model);
    REQUIRE(node.pass_inputs);
    CHECK(node.pass_inputs->declared());
    CHECK(node.pass_inputs->declarations.size() == 12);
    CHECK(node.pass_inputs->variadic_range ==
          FramePlanVariadicRange{.first = 3, .max = 8});
    CHECK(node.pass_inputs->buffer_binding_base == 11);
    CHECK(framePlanNodeDeclaresInputs(node));

    // Textures count from binding 0 (history among them), buffers from 11.
    const std::vector<std::string> inputs{"t_a",  "t_b",       "t_c@history",
                                          "t_v0", "vi_buffer", "t_v1"};
    const auto sockets = framePlanInputSockets(model, node, inputs);
    REQUIRE(sockets.declared);
    CHECK(vi3aRows(sockets) ==
          std::vector<Vi3aRow>{
              {"in@0", "in_a ← t_a", 0, false},
              {"in@1", "in_b ← t_b", 1, false},
              {"in@2", "in_c ← t_c (history)", 2, false},
              {"in@3", "← t_v0", 3, false},
              {"in@4", "← t_v1", 5, false},
              {"in:variadic", "+ input", std::nullopt, false},
              {"in@11", "vi1_value ← vi_buffer", 4, true}});

    SECTION("what is not fed is an empty socket, and the band only its end") {
        const auto few = framePlanInputSockets(model, node,
                                               std::vector<std::string>{"t_a"});
        CHECK(vi3aRows(few) ==
              std::vector<Vi3aRow>{
                  {"in@0", "in_a ← t_a", 0, false},
                  {"in@1", "in_b", std::nullopt, false},
                  {"in@2", "in_c", std::nullopt, false},
                  {"in:variadic", "+ input", std::nullopt, false},
                  {"in@11", "vi1_value", std::nullopt, true}});
    }

    SECTION("an input no declaration takes keeps a row of its own, at the end") {
        const auto one = vi3aModel(vi3aFixedInputs({"in_a"}));
        const auto rows = framePlanInputSockets(
            one, vi3aNode(one), std::vector<std::string>{"t_a", "t_b"});
        CHECK(vi3aRows(rows) ==
              std::vector<Vi3aRow>{{"in@0", "in_a ← t_a", 0, false},
                                   {"", "t_b", 1, false}});
    }

    SECTION("rule 5: a declaration named for another connected resource") {
        const auto swapped = vi3aModel(vi3aFixedInputs({"in_t_b", "in_t_a"}));
        const auto rows = framePlanInputSockets(
            swapped, vi3aNode(swapped), std::vector<std::string>{"t_a", "t_b"});
        REQUIRE(rows.sockets.size() == 2);
        CHECK(rows.sockets[0].warning ==
              "This declaration is named for t_b, but binding 0 is fed t_a.");
        CHECK(rows.sockets[1].warning ==
              "This declaration is named for t_a, but binding 1 is fed t_b.");
        // A name that points at nothing connected is not judged.
        const auto other = framePlanInputSockets(
            swapped, vi3aNode(swapped), std::vector<std::string>{"t_a", "t_x"});
        CHECK(other.sockets[0].warning.empty());
    }

    SECTION("the control: no declarations, no sockets from them") {
        CHECK_FALSE(vi3aNode(model, "wp343_mixed_c").pass_inputs);
        CHECK_FALSE(framePlanInputSockets(model, vi3aNode(model, "wp343_mixed_c"),
                                          std::nullopt)
                        .declared);
        auto unavailable = Json{{"state", "unavailable"}, {"reason", "no pipeline"}};
        const auto none = vi3aModel(unavailable);
        CHECK_FALSE(framePlanNodeDeclaresInputs(vi3aNode(none)));
        // A shader that declares no input at all has nothing to drop on.
        const auto empty = vi3aModel(vi3aFixedInputs({}));
        CHECK_FALSE(framePlanNodeDeclaresInputs(vi3aNode(empty)));
    }

    SECTION("the canvas draws them top to bottom, the band around the group") {
        application();
        FramePlanGraphicsScene canvas;
        canvas.passInputsProvider = [&](const FramePlanNodeKey &key)
            -> std::optional<std::vector<std::string>> {
            if (key.name == Vi3aPass) return inputs;
            return std::nullopt;
        };
        canvas.populateWholeFrame(model);
        std::vector<std::pair<qreal, QString>> rows;
        for (QGraphicsItem *item : itemsOfKind(canvas, FramePlanSocketItem))
            if (item->data(FramePlanNameRole).toString() ==
                    QLatin1String{Vi3aPass} &&
                item->data(FramePlanSocketDirectionRole).toString() ==
                    QLatin1String{"input"})
                rows.emplace_back(item->sceneBoundingRect().center().y(),
                                  item->data(FramePlanSocketKeyRole).toString());
        std::ranges::sort(rows);
        QStringList keys;
        for (const auto &[y, key] : rows) keys.push_back(key);
        CHECK(keys == QStringList{"in@0", "in@1", "in@2", "in@3", "in@4",
                                  "in:variadic", "in@11"});
        // The band holds the group's members and its end, and nothing else.
        QGraphicsItem *band = nullptr;
        for (QGraphicsItem *item : itemsOfKind(canvas, FramePlanSocketBandItem))
            if (item->data(FramePlanNameRole).toString() ==
                QLatin1String{Vi3aPass})
                band = item;
        REQUIRE(band != nullptr);
        const QRectF span = band->sceneBoundingRect();
        REQUIRE(rows.size() == 7);
        CHECK(span.top() < rows[3].first);
        CHECK(span.bottom() > rows[5].first);
        CHECK(span.top() > rows[2].first);
        CHECK(span.bottom() < rows[6].first);
        // The label says what feeds each declaration.
        QStringList labels;
        for (QGraphicsItem *item : itemsOfKind(canvas, FramePlanRowLabelItem))
            if (item->data(FramePlanNameRole).toString() ==
                    QLatin1String{Vi3aPass} &&
                item->data(FramePlanSocketKeyRole).toString() ==
                    QLatin1String{"in@2"})
                labels.push_back(
                    static_cast<QGraphicsSimpleTextItem *>(item)->text());
        CHECK(labels == QStringList{QStringLiteral("in_c ← t_c (history)")});
        // The control: a pass without declarations names its rows by
        // resource, and carries no binding.
        QGraphicsItem *plain = socketItem(canvas, QStringLiteral("wp343_mixed_c"),
                                          QStringLiteral("wp343_mixed_b_output"),
                                          QStringLiteral("input"));
        REQUIRE(plain != nullptr);
        CHECK_FALSE(plain->data(FramePlanSocketKeyRole).isValid());
        CHECK_FALSE(plain->data(FramePlanInputIndexRole).isValid());
    }
}

TEST_CASE("VI-3a a drop connects by number: at the next binding, in place, or "
          "not at all with the line that says why",
          "[devstudio][frame-plan][vi3a][connect]") {
    const auto model = vi3aModel();
    const std::string graph = model.graph;
    using Kind = FramePlanSocketDrop::Kind;
    const auto request = [&](const std::vector<std::string> &current,
                             const std::string &resource,
                             FramePlanSocketDrop drop) {
        return framePlanSocketConnectRequest(vi3aAuthoring(graph, current),
                                             model, graph, Vi3aPass, resource,
                                             drop);
    };
    const auto sent = [](const FramePlanEditRequest &made) {
        REQUIRE(made.sendable());
        CHECK(made.method == "edit_preset_pass");
        CHECK(made.params.at("pass_name") == Vi3aPass);
        return made.params.at("input");
    };
    const std::string fill_first =
        " of wp343_mixed_a first: a pass's inputs fill the bindings in order.";

    SECTION("an empty socket at the next binding: added at the end") {
        CHECK(sent(request({"t_a"}, "t_b", {Kind::socket, 1})) ==
              Json::array({"t_a", "t_b"}));
        // Buffers are counted apart from images.
        CHECK(sent(request({"t_a", "vi_buffer"}, "t_b", {Kind::socket, 1})) ==
              Json::array({"t_a", "vi_buffer", "t_b"}));
    }
    SECTION("past it: refused, nothing sent") {
        auto made = request({"t_a"}, "t_c", {Kind::socket, 2});
        CHECK_FALSE(made.sendable());
        CHECK(made.refusal == "Connect binding 1" + fill_first);
        made = request({}, "t_c", {Kind::socket, 2});
        CHECK(made.refusal == "Connect binding 0..1" + fill_first);
    }
    SECTION("a connected socket: that input is replaced, nothing moves") {
        CHECK(sent(request({"t_a", "t_b"}, "t_x", {Kind::socket, 0})) ==
              Json::array({"t_x", "t_b"}));
        CHECK(sent(request({"t_a", "t_b@history"}, "t_x", {Kind::socket, 1})) ==
              Json::array({"t_a", "t_x"}));
    }
    SECTION("the band: added at the end once every fixed socket is fed") {
        CHECK(sent(request({"t_a", "t_b", "t_c"}, "t_v", {Kind::variadic})) ==
              Json::array({"t_a", "t_b", "t_c", "t_v"}));
        CHECK(request({"t_a"}, "t_v", {Kind::variadic}).refusal ==
              "Connect binding 1..2" + fill_first);
        // An image the pass already reads is not named twice: the engine
        // refuses duplicate_input.
        const auto again = request({"t_a", "t_b", "t_c", "t_v"}, "t_v", {Kind::variadic});
        CHECK_FALSE(again.sendable());
        CHECK(again.refusal == "wp343_mixed_a already reads t_v.");
    }
    SECTION("a full band refuses") {
        std::vector<std::string> full{"t_a", "t_b", "t_c"};
        for (int slot = 0; slot < 8; ++slot)
            full.push_back("t_v" + std::to_string(slot));
        const auto made = request(full, "t_x", {Kind::variadic});
        CHECK_FALSE(made.sendable());
        CHECK(made.refusal ==
              "wp343_mixed_a's shader takes at most 8 inputs in its variadic "
              "group.");
    }
    SECTION("the buffer socket counts buffers only") {
        CHECK(sent(request({"t_a"}, "vi_buffer", {Kind::socket, 11})) ==
              Json::array({"t_a", "vi_buffer"}));
        CHECK(sent(request({"vi_buffer", "t_a"}, "vi_buffer2",
                           {Kind::socket, 11})) ==
              Json::array({"vi_buffer2", "t_a"}));
        CHECK(request({"t_a"}, "t_b", {Kind::socket, 11}).refusal ==
              "t_b is an image: binding 11 of wp343_mixed_a takes a buffer.");
    }
    SECTION("the body adds nothing") {
        const auto made = request({"t_a"}, "t_b", {Kind::body});
        CHECK_FALSE(made.sendable());
        CHECK(made.refusal == "Drop on a socket of wp343_mixed_a: its inputs "
                              "follow its shader's declarations.");
        // The control: a pass without declarations still takes it.
        const auto plain = vi3aModel(Json(nullptr));
        const auto old = framePlanSocketConnectRequest(
            vi3aAuthoring(graph, {"t_a"}), plain, graph, Vi3aPass, "t_b",
            {Kind::body});
        CHECK(sent(old) == Json::array({"t_a", "t_b"}));
    }
    SECTION("disconnect: what follows moves up a binding, and says so") {
        const auto cut = [&](const std::vector<std::string> &current,
                             std::size_t index) {
            return framePlanSocketDisconnectRequest(
                vi3aAuthoring(graph, current), model, graph, Vi3aPass, index);
        };
        auto made = cut({"t_a", "t_b", "t_c"}, 1);
        CHECK(sent(made) == Json::array({"t_a", "t_c"}));
        CHECK(made.notice ==
              "wp343_mixed_a: the inputs after binding 1 moved up by one.");
        made = cut({"t_a", "t_b", "t_c"}, 2);
        CHECK(sent(made) == Json::array({"t_a", "t_b"}));
        CHECK(made.notice.empty());
        made = cut({"t_a", "vi_buffer", "vi_buffer2"}, 1);
        CHECK(sent(made) == Json::array({"t_a", "vi_buffer2"}));
        CHECK(made.notice ==
              "wp343_mixed_a: the inputs after binding 11 moved up by one.");
    }
}

TEST_CASE("VI-3a on the canvas a drop aims at a socket; the body is refused",
          "[devstudio][frame-plan][vi3a][canvas-edit]") {
    application();
    EditingFixture fixture;
    setVi3aPassInputs(fixture.wire, vi3aPassInputs());
    fixture.tweak_context = [](Json &context) {
        setVi3aInputs(context, {"t_a", "t_b", "t_c"});
    };
    fixture.open();
    auto &logical = fixture.logical();
    REQUIRE(nodeItem(logical, Vi3aPass) != nullptr);
    const auto sentInputs = [&] {
        return Json::parse(
                   QJsonDocument(fixture.recorder.params).toJson().toStdString())
            .at("input");
    };

    SECTION("the body: nothing is sent, and the line says where to drop") {
        fixture.dragSocket(QStringLiteral("wp343_mixed_d"),
                           QStringLiteral("wp343_mixed_d_output"),
                           QStringLiteral("output"),
                           nodeItem(logical, Vi3aPass));
        CHECK(fixture.recorder.submits == 0);
        CHECK(statusText(fixture.widget) ==
              QStringLiteral("Drop on a socket of wp343_mixed_a: its inputs "
                             "follow its shader's declarations."));
    }
    SECTION("the band's end: one more input") {
        QGraphicsItem *end = declaredSocket(logical, QString::fromLatin1(Vi3aPass),
                                            QStringLiteral("in:variadic"));
        REQUIRE(end != nullptr);
        fixture.dragSocket(QStringLiteral("wp343_mixed_d"),
                           QStringLiteral("wp343_mixed_d_output"),
                           QStringLiteral("output"), end);
        REQUIRE(fixture.recorder.submits == 1);
        CHECK(sentInputs() ==
              Json::array({"t_a", "t_b", "t_c", "wp343_mixed_d_output"}));
    }
    SECTION("a connected socket: in its place") {
        QGraphicsItem *second = declaredSocket(
            logical, QString::fromLatin1(Vi3aPass), QStringLiteral("in@1"));
        REQUIRE(second != nullptr);
        fixture.dragSocket(QStringLiteral("wp343_mixed_d"),
                           QStringLiteral("wp343_mixed_d_output"),
                           QStringLiteral("output"), second);
        REQUIRE(fixture.recorder.submits == 1);
        CHECK(sentInputs() ==
              Json::array({"t_a", "wp343_mixed_d_output", "t_c"}));
    }
    SECTION("an input dragged away: that one goes, and what follows moves up") {
        fixture.dragSocket(QString::fromLatin1(Vi3aPass), QStringLiteral("t_a"),
                           QStringLiteral("input"), nullptr);
        REQUIRE(fixture.recorder.submits == 1);
        CHECK(sentInputs() == Json::array({"t_b", "t_c"}));
        fixture.recorder.result(
            fixture.recorder.submit_id,
            QByteArray::fromStdString(
                Json{{"status", "accepted"}, {"ticket", "t-1"}}.dump()));
        QApplication::processEvents();
        fixture.recorder.result(
            fixture.recorder.ticket_id,
            QByteArray::fromStdString(
                Json{{"status", "committed"}, {"committed", true}}.dump()));
        QApplication::processEvents();
        CHECK(statusText(fixture.widget) ==
              QStringLiteral(
                  "wp343_mixed_a: the inputs after binding 0 moved up by one."));
    }
}

TEST_CASE("VI-3a a reload refused for a pass is a failure: its lost binding is "
          "red, and the line offers Disconnect",
          "[devstudio][frame-plan][vi3a][shader-reload]") {
    application();

    SECTION("the follower reads refused_passes, not committed alone") {
        std::vector<qint64> reloads;
        std::vector<ShaderReloadOutcome> outcomes;
        ShaderReloadFollower follower{ShaderReloadFollower::Hooks{
            .request_status = [](QString *) { return qint64{0}; },
            .apply = [](const ShaderReloadMarks &, const ShaderReloadReading &,
                        bool) {},
            .request_reload =
                [&](QString *) {
                    reloads.push_back(static_cast<qint64>(reloads.size()) + 7100);
                    return reloads.back();
                },
            .reloaded =
                [&](const ShaderReloadOutcome &outcome) {
                    outcomes.push_back(outcome);
                },
        }};
        const auto reload = [&](const char *answer) {
            follower.sourceChanged();
            QTest::qWait(ShaderReloadFollower::ReloadDebounceMs + 150);
            REQUIRE(!reloads.empty());
            REQUIRE(follower.receive(reloads.back(), QByteArray{answer}));
            REQUIRE(!outcomes.empty());
            return outcomes.back();
        };
        const auto refused = reload(
            R"({"committed":true,"changed":["project://shaders/present2.frag"],"refused_passes":[{"pass":"present","fragment":"project://shaders/present2","reason":"pass 'present' input 1 ('vi_green') ...","declarations":[{"binding":0,"type":"sampled_image","name":"in_a","variadic":false}]}]})");
        CHECK(refused.kind == ShaderReloadOutcome::Kind::refused);
        CHECK(refused.refused_passes == QStringList{QStringLiteral("present")});
        CHECK(refused.message.contains(QStringLiteral("vi_green")));
        // An empty list is a commit, and so is a reply that carries none.
        CHECK(reload(R"({"committed":true,"changed":[],"refused_passes":[]})")
                  .kind == ShaderReloadOutcome::Kind::committed);
        CHECK(reload(R"({"committed":false,"attempted":false})").kind ==
              ShaderReloadOutcome::Kind::unchanged);
    }

    SECTION("the canvas marks the binding and offers the way out") {
        EditingFixture fixture;
        auto pass_inputs = vi3aFixedInputs({"in_a", "in_b"});
        pass_inputs["reload_refused"] = {
            {"reason", "the new shader no longer declares binding 1"},
            {"shader_input_mismatch",
             {{"pass", Vi3aPass}, {"direction", "pass_input_undeclared"},
              {"binding", 1}, {"resource", "t_b"}}},
            {"declarations", Json::array({vi3aDeclaration(0, "in_a")})}};
        setVi3aPassInputs(fixture.wire, pass_inputs);
        fixture.tweak_context = [](Json &context) {
            setVi3aInputs(context, {"t_a", "t_b"});
        };
        fixture.open();
        auto &logical = fixture.logical();
        QGraphicsItem *kept = declaredSocket(
            logical, QString::fromLatin1(Vi3aPass), QStringLiteral("in@0"));
        QGraphicsItem *lost = declaredSocket(
            logical, QString::fromLatin1(Vi3aPass), QStringLiteral("in@1"));
        REQUIRE(kept != nullptr);
        REQUIRE(lost != nullptr);
        CHECK(lost->data(FramePlanSocketRemovedRole).toBool());
        CHECK(static_cast<QGraphicsPathItem *>(lost)->brush().color() ==
              QColor{QStringLiteral("#d94c3d")});
        // The control: the binding the new shader still declares.
        CHECK_FALSE(kept->data(FramePlanSocketRemovedRole).toBool());

        auto *disconnect = fixture.widget.findChild<QPushButton *>(
            QStringLiteral("pelican.framePlan.reloadRefused.disconnect"));
        REQUIRE(disconnect != nullptr);
        fixture.widget.receiveShaderReloadOutcome(ShaderReloadOutcome{
            .kind = ShaderReloadOutcome::Kind::refused,
            .message = QStringLiteral("refused"),
            .refused_passes = {QString::fromLatin1(Vi3aPass)}});
        QApplication::processEvents();
        CHECK(statusText(fixture.widget) ==
              QStringLiteral("wp343_mixed_a keeps drawing with its previous "
                             "shader: the new one no longer declares binding "
                             "1 (t_b)."));
        CHECK_FALSE(disconnect->isHidden());
        disconnect->click();
        REQUIRE(fixture.recorder.submits == 1);
        CHECK(Json::parse(QJsonDocument(fixture.recorder.params)
                              .toJson()
                              .toStdString())
                  .at("input") == Json::array({"t_a"}));
        CHECK(disconnect->isHidden());
    }
}

// Q-10. MG-2b: the root of the rendering document holds no tasks. A task the
// catalog still attributes to it -- a stale reading, or a fixture -- is
// refused by name before anything is sent, on the canvas and in the Task
// Editor, and a new task still goes out as add_compute_task.
TEST_CASE("Q-10 a task at the rendering document's root is refused by name; a new task is still added",
          "[devstudio][frame-plan][q10][canvas-edit]") {
    using PelicanStudio::ComputeTaskEditorSubmit;
    using PelicanStudio::computeTaskEditorRequest;
    using PelicanStudio::framePlanRootTaskRefusal;
    using PelicanStudio::framePlanTaskAtRoot;
    const std::string root = "project://rendering.json";
    const std::string line =
        "authored_consumer: rendering root holds no tasks; use a feature or a "
        "render.compute node.";
    const std::string sentence =
        line + " The engine refuses an edit of it (compute_task_not_editable).";
    CHECK(framePlanRootTaskRefusal("authored_consumer") == sentence);

    // The rule: the root document, and no feature providing the task.
    CHECK(framePlanTaskAtRoot(root, "", root));
    CHECK_FALSE(framePlanTaskAtRoot(root, "gpu_particles", root));
    CHECK_FALSE(framePlanTaskAtRoot("project://passes/task.json", "", root));
    CHECK_FALSE(framePlanTaskAtRoot("engine://pipelines/forward.json", "", root));
    // Not told where the root is: nothing claimed.
    CHECK_FALSE(framePlanTaskAtRoot(root, "", ""));

    SECTION("the canvas: a drag onto the root task sends nothing and says why") {
        application();
        EditingFixture fixture;
        fixture.catalog = authoredTaskCatalog();
        // A stale catalog that still calls the root task editable.
        fixture.catalog.at("tasks").at(0)["source_reference"] = root;
        fixture.root_reference = root;
        fixture.open();
        auto &logical = fixture.logical();
        fixture.dragSocket(QStringLiteral("wp343_mixed_a"),
                           QStringLiteral("wp343_mixed_a_output"),
                           QStringLiteral("output"),
                           nodeItem(logical, "wp343_mixed_b"));
        CHECK(fixture.recorder.submits == 0);
        // The status line is the first sentence; the code is in its tooltip.
        CHECK(statusText(fixture.widget) == QString::fromStdString(line));
        auto *status = fixture.widget.findChild<QLabel *>(
            QStringLiteral("pelican.framePlanStatus"));
        REQUIRE(status != nullptr);
        CHECK(status->toolTip() == QString::fromStdString(sentence));
        // Nor does it offer its dispatch for retyping.
        CHECK(bandItem(logical, QStringLiteral("wp343_mixed_b"),
                       QStringLiteral("dispatch")) == nullptr);
    }

    SECTION("the Task Editor: an edit is refused, a new task is added as before") {
        const Json task{{"name", "authored_consumer"},
                        {"shader", "project://shaders/consume.comp"},
                        {"dispatch", {{"groups", Json::array({1, 1, 1})}}}};
        ComputeTaskEditorSubmit edit;
        edit.base_source_digest = RootDigest;
        edit.task_name = "authored_consumer";
        edit.task_source_digest = TaskDigest;
        edit.source_reference = root;
        edit.editable = true;
        edit.root_reference = root;
        const auto refused = computeTaskEditorRequest(edit, task);
        CHECK_FALSE(refused.sendable());
        CHECK(refused.refusal == sentence);

        // The control: the same edit of a task a feature declares goes out.
        edit.source_reference = "project://passes/task.json";
        edit.provider_feature = "task_feature";
        const auto sent = computeTaskEditorRequest(edit, task);
        REQUIRE(sent.sendable());
        CHECK(sent.method == "edit_compute_task");
        CHECK(sent.params == Json{{"base_source_digest", RootDigest},
                                  {"base_task_source_digest", TaskDigest},
                                  {"task_name", "authored_consumer"},
                                  {"task", task}});

        // A new task: add_compute_task into the project:// feature it names,
        // whatever the root holds.
        ComputeTaskEditorSubmit create;
        create.creating = true;
        create.base_source_digest = RootDigest;
        create.destination_reference = "project://passes/new_compute.json";
        create.root_reference = root;
        const Json fresh{{"name", "new_compute"}};
        const auto added = computeTaskEditorRequest(create, fresh);
        REQUIRE(added.sendable());
        CHECK(added.method == "add_compute_task");
        CHECK(added.params == Json{{"base_source_digest", RootDigest},
                                   {"destination_reference", "project://passes/new_compute.json"},
                                   {"task", fresh}});

        // A renamed task is refused as it always was.
        auto renamed = task;
        renamed["name"] = "other";
        CHECK(computeTaskEditorRequest(edit, renamed).refusal ==
              "The task name cannot be changed.");
    }
}

namespace {

// Q-12 (MG-3b). The authored task, as a render.compute node's task: listed
// against the rendering document even when the root names a preset (so the
// Q-10 test alone would call it a root task), with the node that made it.
Json graphNodeTaskCatalog(const std::string &root) {
    auto catalog = authoredTaskCatalog();
    auto &row = catalog.at("tasks").at(0);
    row["origin"] = "graph_node";
    row["source_reference"] = root;
    row["editable"] = false;
    row["graph_node"] = {{"scope", "smooth"}, {"node", "step"}, {"instance", "left"}};
    row["read_only_reason"] = std::string{PelicanStudio::FramePlanGraphNodeTaskReason};
    return catalog;
}

// The entries a right-click over `item` offers, read off the popup and closed.
QStringList contextMenuTexts(FramePlanGraphicsScene &scene, QGraphicsItem &item) {
    QStringList texts;
    QTimer reader;
    reader.setInterval(0);
    QObject::connect(&reader, &QTimer::timeout, &scene, [&] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *menu = qobject_cast<QMenu *>(widget);
            if (menu == nullptr || !menu->isVisible()) continue;
            for (QAction *action : menu->actions())
                if (!action->isSeparator()) texts.push_back(action->text());
            menu->close();
        }
    });
    reader.start();
    QGraphicsSceneContextMenuEvent event{QEvent::GraphicsSceneContextMenu};
    event.setScenePos(item.sceneBoundingRect().center());
    event.setScreenPos(QPoint{100, 100});
    QApplication::sendEvent(&scene, &event);
    reader.stop();
    QApplication::processEvents();
    return texts;
}

} // namespace

// Q-12. MG-3b says where a task came from. A task a render.compute node made
// is refused with the engine's own sentence -- never Q-10's root sentence,
// even when it is listed against the root -- and the root sentence is kept
// for origin "root" and for a catalog that carries no origin.
TEST_CASE("Q-12 the origin is read before the root test: a graph node's task says so",
          "[devstudio][frame-plan][q12][canvas-edit]") {
    using PelicanStudio::ComputeTaskEditorSubmit;
    using PelicanStudio::computeTaskEditorRequest;
    using PelicanStudio::FramePlanGraphNodeRef;
    using PelicanStudio::FramePlanGraphNodeTaskReason;
    using PelicanStudio::FramePlanTaskLock;
    using PelicanStudio::framePlanGraphNodeTaskReason;
    using PelicanStudio::framePlanRootTaskRefusal;
    using PelicanStudio::framePlanTaskAuthoringFromCatalog;
    using PelicanStudio::framePlanTaskGraphNode;
    using PelicanStudio::framePlanTaskLock;
    using PelicanStudio::framePlanTaskOrigin;
    const std::string root = "project://rendering.json";
    const std::string reason{FramePlanGraphNodeTaskReason};
    CHECK(reason ==
          "This task is made by a render.compute node in the graph. Edit the "
          "node in the Graph tab.");

    // The four values, and a row from an engine that does not say.
    for (const auto *origin : {"feature", "graph_node", "preset", "root"})
        CHECK(framePlanTaskOrigin(Json{{"name", "t"}, {"origin", origin}}) == origin);
    CHECK(framePlanTaskOrigin(Json{{"name", "t"}}).empty());

    // The decision, origin first.
    CHECK(framePlanTaskLock("feature", true, root, "gpu_particles", root) ==
          FramePlanTaskLock::none);
    CHECK(framePlanTaskLock("feature", false, "project://features/a.json", "a", root) ==
          FramePlanTaskLock::read_only);
    // A node's task under a preset root: listed against the root, no feature.
    CHECK(framePlanTaskLock("graph_node", false, root, "", root) ==
          FramePlanTaskLock::graph_node);
    CHECK(framePlanTaskLock("graph_node", true, root, "", root) ==
          FramePlanTaskLock::graph_node);
    CHECK(framePlanTaskLock("preset", false, "engine://render_pipelines/hybrid_v1.json",
                            "", root) == FramePlanTaskLock::read_only);
    CHECK(framePlanTaskLock("preset", true, "project://pipeline.json", "", root) ==
          FramePlanTaskLock::none);
    CHECK(framePlanTaskLock("root", false, root, "", root) == FramePlanTaskLock::root);
    // The engine says root even where Studio has not been told the reference.
    CHECK(framePlanTaskLock("root", false, root, "", "") == FramePlanTaskLock::root);
    // No origin: Q-10's test, unchanged.
    CHECK(framePlanTaskLock("", true, root, "", root) == FramePlanTaskLock::root);
    CHECK(framePlanTaskLock("", true, "project://passes/task.json", "", root) ==
          FramePlanTaskLock::none);
    CHECK(framePlanTaskLock("", false, "project://passes/task.json", "", root) ==
          FramePlanTaskLock::read_only);
    CHECK(framePlanTaskLock("", true, root, "", "") == FramePlanTaskLock::none);

    // Where the node is: only for a graph_node row that names scope and node.
    const auto catalog = graphNodeTaskCatalog(root);
    const auto &row = catalog.at("tasks").at(0);
    CHECK(framePlanTaskGraphNode(row) == FramePlanGraphNodeRef{"smooth", "step", "left"});
    auto no_instance = row;
    no_instance["graph_node"].erase("instance");
    CHECK(framePlanTaskGraphNode(no_instance) == FramePlanGraphNodeRef{"smooth", "step", ""});
    auto no_node = row;
    no_node["graph_node"].erase("node");
    CHECK_FALSE(framePlanTaskGraphNode(no_node).has_value());
    auto feature = row;
    feature["origin"] = "feature";
    CHECK_FALSE(framePlanTaskGraphNode(feature).has_value());
    CHECK(framePlanGraphNodeTaskReason("") == reason);
    CHECK(framePlanGraphNodeTaskReason("Edit it elsewhere.") == "Edit it elsewhere.");

    // The canvas, from the catalog as it arrives: the node's sentence, not
    // the root's, for every gesture that edits the task.
    auto authoring = framePlanTaskAuthoringFromCatalog(catalog);
    authoring.root_reference = root;
    const auto *task = authoring.find("authored_consumer");
    REQUIRE(task != nullptr);
    CHECK(task->origin == "graph_node");
    CHECK(task->graph_node == std::optional{FramePlanGraphNodeRef{"smooth", "step", "left"}});
    const std::string refusal = "authored_consumer: " + reason +
                                " Right-click the task and choose Open in Graph.";
    for (const auto &request :
         {PelicanStudio::framePlanTaskResourceRequest(
              authoring, "authored_consumer", {"wp343_mixed_a_output"}, true),
          PelicanStudio::framePlanTaskDispatchRequest(
              authoring, "authored_consumer", R"({"groups":[2,1,1]})"),
          PelicanStudio::framePlanTaskRemoveRequest(authoring, "authored_consumer")}) {
        CHECK_FALSE(request.sendable());
        CHECK(request.line == reason);
        CHECK(request.refusal == refusal);
        CHECK(request.refusal != framePlanRootTaskRefusal("authored_consumer"));
    }
    // The control: the same row from an engine that does not say where the
    // task came from gets Q-10's sentence.
    auto old_catalog = catalog;
    old_catalog.at("tasks").at(0).erase("origin");
    old_catalog.at("tasks").at(0).erase("graph_node");
    auto old_authoring = framePlanTaskAuthoringFromCatalog(old_catalog);
    old_authoring.root_reference = root;
    CHECK(PelicanStudio::framePlanTaskRemoveRequest(old_authoring, "authored_consumer")
              .refusal == framePlanRootTaskRefusal("authored_consumer"));

    // The Task Editor: the engine's sentence, before the root test.
    const Json declaration{{"name", "authored_consumer"}};
    ComputeTaskEditorSubmit edit;
    edit.base_source_digest = RootDigest;
    edit.task_name = "authored_consumer";
    edit.task_source_digest = TaskDigest;
    edit.source_reference = root;
    edit.root_reference = root;
    edit.origin = "graph_node";
    edit.read_only_reason = reason;
    const auto refused = computeTaskEditorRequest(edit, declaration);
    CHECK_FALSE(refused.sendable());
    CHECK(refused.refusal == reason);
    edit.origin.clear();
    CHECK(computeTaskEditorRequest(edit, declaration).refusal ==
          framePlanRootTaskRefusal("authored_consumer"));
}

// Q-12. On the canvas: the gestures say the node's sentence, and a
// right-click on the task offers Open in Graph, which hands the node MG-3b
// named to whoever opens the Graph tab.
TEST_CASE("Q-12 a graph node's task on the canvas: its sentence, and Open in Graph",
          "[devstudio][frame-plan][q12][canvas-edit]") {
    application();
    const std::string root = "project://rendering.json";
    const QString reason =
        QString::fromUtf8(PelicanStudio::FramePlanGraphNodeTaskReason.data(),
                          static_cast<qsizetype>(
                              PelicanStudio::FramePlanGraphNodeTaskReason.size()));
    EditingFixture fixture;
    fixture.catalog = graphNodeTaskCatalog(root);
    fixture.root_reference = root;
    fixture.open();
    auto &logical = fixture.logical();
    auto *status = fixture.widget.findChild<QLabel *>(
        QStringLiteral("pelican.framePlanStatus"));
    REQUIRE(status != nullptr);

    // A drag onto it sends nothing; the line is the engine's, whole.
    fixture.dragSocket(QStringLiteral("wp343_mixed_a"),
                       QStringLiteral("wp343_mixed_a_output"),
                       QStringLiteral("output"),
                       nodeItem(logical, "wp343_mixed_b"));
    CHECK(fixture.recorder.submits == 0);
    CHECK(statusText(fixture.widget) == reason);
    CHECK(status->toolTip() ==
          QStringLiteral("authored_consumer: ") + reason +
              QStringLiteral(" Right-click the task and choose Open in Graph."));
    // No band to retype.
    CHECK(bandItem(logical, QStringLiteral("wp343_mixed_b"),
                   QStringLiteral("dispatch")) == nullptr);

    QGraphicsItem *task = nodeItem(logical, "wp343_mixed_b");
    QGraphicsItem *pass = nodeItem(logical, "wp343_mixed_a");
    REQUIRE(task != nullptr);
    REQUIRE(pass != nullptr);

    // Delete on it: the same sentence, nothing sent.
    status->clear();
    task->setSelected(true);
    QApplication::processEvents();
    REQUIRE(logical.selectedNode().has_value());
    auto &view = logicalView(fixture.widget);
    view.setFocus();
    QTest::keyClick(&view, Qt::Key_Delete);
    QApplication::processEvents();
    CHECK(fixture.recorder.submits == 0);
    CHECK(statusText(fixture.widget) == reason);

    // Offered on the task -- and, GL-1 decision 6, on every other pass too,
    // whose node on the Graph canvas is a folded one.
    CHECK(contextMenuTexts(logical, *task).contains(QStringLiteral("Open in Graph")));
    CHECK(contextMenuTexts(logical, *pass).contains(QStringLiteral("Open in Graph")));

    // Chosen: the node MG-3b named goes to the Graph tab's reveal.
    QStringList opened;
    bool shown = true;
    fixture.widget.setGraphNodeOpenAction(
        [&](const QString &scope, const QString &node, const QString &instance) {
            opened = {scope, node, instance};
            return shown;
        });
    triggerContextAction(logical, *task, QStringLiteral("Open in Graph"));
    CHECK(opened == QStringList{QStringLiteral("smooth"), QStringLiteral("step"),
                                QStringLiteral("left")});

    // A Graph tab that cannot show it: the status line says which node.
    opened.clear();
    shown = false;
    triggerContextAction(logical, *task, QStringLiteral("Open in Graph"));
    CHECK(opened.size() == 3);
    CHECK(statusText(fixture.widget) ==
          QStringLiteral("The Graph tab has no node step in smooth yet: refresh it."));
}

TEST_CASE("N-8 Delete on a picked edge cuts every resource it carries in one RPC",
          "[devstudio][frame-plan][n8][canvas-edit]") {
    application();
    EditingFixture fixture;
    // The task already reads the resource, so Delete has something to cut.
    fixture.catalog = authoredTaskCatalog(Json::array({"wp343_mixed_a_output"}));
    fixture.open();
    auto &logical = fixture.logical();
    logical.selectEdge(FramePlanEdgeSelection{
        fixture.graph, "wp343_mixed_b",
        std::vector<std::string>{"wp343_mixed_a_output"}});
    REQUIRE(logical.selectedEdge().has_value());
    auto &view = logicalView(fixture.widget);
    view.setFocus();
    QTest::keyClick(&view, Qt::Key_Delete);
    QApplication::processEvents();
    REQUIRE(fixture.recorder.submits == 1);
    CHECK(fixture.recorder.method == QStringLiteral("edit_compute_task"));
    const auto sent = Json::parse(
        QJsonDocument(fixture.recorder.params).toJson().toStdString());
    CHECK(sent.at("task_name") == "authored_consumer");
    CHECK(sent.at("task").at("reads") == Json::array());
}

TEST_CASE("N-8 the in-place band replaces one shader stem and nothing else",
          "[devstudio][frame-plan][n8][canvas-edit]") {
    application();
    EditingFixture fixture;
    fixture.open();
    auto &logical = fixture.logical();
    // Only a pass that can be edited offers bands.
    CHECK(bandItem(logical, QStringLiteral("wp343_mixed_d"),
                   QStringLiteral("shader.vertex")) == nullptr);
    QGraphicsItem *band = bandItem(logical, QStringLiteral("wp343_mixed_a"),
                                   QStringLiteral("shader.fragment"));
    REQUIRE(band != nullptr);
    CHECK(band->data(FramePlanInlineValueRole).toString() ==
          QStringLiteral("engine://fullscreen"));

    auto &view = logicalView(fixture.widget);
    view.fitInView(band->sceneBoundingRect().adjusted(-200, -200, 200, 200),
                   Qt::KeepAspectRatio);
    QApplication::processEvents();
    const QPoint at = view.mapFromScene(band->sceneBoundingRect().center());
    REQUIRE(view.viewport()->rect().contains(at));
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, at);
    QApplication::processEvents();
    REQUIRE(logical.editingInline());
    logical.setInlineEditText(QStringLiteral("project://shaders/tint.frag"));
    REQUIRE(logical.commitInlineEdit());
    QApplication::processEvents();

    REQUIRE(fixture.recorder.submits == 1);
    CHECK(fixture.recorder.method == QStringLiteral("edit_preset_pass"));
    const auto sent = Json::parse(
        QJsonDocument(fixture.recorder.params).toJson().toStdString());
    const auto declaration = mixedPassDeclaration(0);
    CHECK(sent.at("shader").at("fragment") == "project://shaders/tint.frag");
    // The control: the stem that was not typed, and every other field, are the
    // declaration's own.
    CHECK(sent.at("shader").at("vertex") ==
          declaration.at("shader").at("vertex"));
    CHECK(sent.at("output") == declaration.at("output"));
    CHECK(sent.at("input") == Json::array());
}

namespace {

// The editor reports its decision from the event loop, and the commit it asks
// for is queued again from there: two turns, not one.
void settle() {
    QApplication::processEvents();
    QApplication::processEvents();
}

// The line edit that is open over a band, reached the way the scene holds it:
// through the proxy item, not through the widget tree.
QLineEdit *inlineEditorWidget(QGraphicsScene &value) {
    for (QGraphicsItem *item : value.items()) {
        auto *proxy = dynamic_cast<QGraphicsProxyWidget *>(item);
        if (proxy == nullptr) continue;
        if (auto *edit = qobject_cast<QLineEdit *>(proxy->widget()))
            return edit;
    }
    return nullptr;
}

// A key, sent to the widget rather than to the window: the editor is inside a
// graphics proxy, which is not something the test keyboard can aim at.
void typeKey(QWidget *widget, int key) {
    REQUIRE(widget != nullptr);
    QKeyEvent event{QEvent::KeyPress, key, Qt::NoModifier};
    QApplication::sendEvent(widget, &event);
    settle();
}

// Open the band editor the way a user does.
void clickBand(EditingFixture &fixture, const QString &node,
               const QString &field) {
    QGraphicsItem *band = bandItem(fixture.logical(), node, field);
    REQUIRE(band != nullptr);
    auto &view = logicalView(fixture.widget);
    view.fitInView(band->sceneBoundingRect().adjusted(-200, -200, 200, 200),
                   Qt::KeepAspectRatio);
    QApplication::processEvents();
    const QPoint at = view.mapFromScene(band->sceneBoundingRect().center());
    REQUIRE(view.viewport()->rect().contains(at));
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, at);
    QApplication::processEvents();
}

} // namespace

// Q-4. The raw reply is laid out for reading only while somebody is reading
// it. What must not change is what the tab shows when it is opened.
TEST_CASE("The Frame Plan raw JSON tab shows the reply it is opened on",
          "[devstudio][frame-plan][q4]") {
    application();
    EditingFixture fixture;
    fixture.open();
    auto *raw = fixture.widget.findChild<QPlainTextEdit *>(
        QStringLiteral("pelican.framePlanRawJson"));
    auto *tabs = fixture.widget.findChild<QTabWidget *>(
        QStringLiteral("pelican.framePlanTabs"));
    REQUIRE(raw != nullptr);
    REQUIRE(tabs != nullptr);
    // A plan arrived, and nobody has opened this tab: nothing was laid out.
    REQUIRE(raw->toPlainText().isEmpty());

    int index = -1;
    for (int tab = 0; tab < tabs->count(); ++tab)
        if (tabs->widget(tab) == raw) index = tab;
    REQUIRE(index >= 0);
    tabs->setCurrentIndex(index);
    settle();
    const QString shown = raw->toPlainText();
    REQUIRE_FALSE(shown.isEmpty());
    REQUIRE(Json::parse(shown.toStdString()) == fixture.wire);

    // And a reply arriving while it is the tab in front reaches it.
    auto next = fixture.wire;
    next["runtime_generation"] = 4242;
    fixture.widget.receiveResult(QByteArray::fromStdString(next.dump()));
    settle();
    REQUIRE(Json::parse(raw->toPlainText().toStdString()) == next);
}

// Q-4, the mirror of Q-3's defect on the Frame Plan side. The band editor used
// to end on Enter and on nothing else -- no Escape, and no focus rule at all --
// while on Windows the Studio blinks inactive for one trip through the event
// loop whenever the keyboard moves between the embedded engine viewport's
// native window and the main window. Qt reports that blink as
// FocusOut(ActiveWindowFocusReason).
TEST_CASE("T-1 a shader band's editor completes to the shaders it is given",
          "[devstudio][frame-plan][t1][canvas-edit]") {
    application();
    EditingFixture fixture;
    std::vector<QString> asked;
    fixture.widget.setInlineFieldChoices([&](const QString &field) {
        asked.push_back(field);
        return field == QLatin1String{"shader.fragment"}
                   ? QStringList{QStringLiteral("project://shaders/tint"),
                                 QStringLiteral("engine://fullscreen")}
                   : QStringList{};
    });
    fixture.open();
    auto &logical = fixture.logical();
    clickBand(fixture, QStringLiteral("wp343_mixed_a"), QStringLiteral("shader.fragment"));
    REQUIRE(logical.editingInline());
    QLineEdit *editing = inlineEditorWidget(logical);
    REQUIRE(editing != nullptr);
    CHECK(asked == std::vector<QString>{QStringLiteral("shader.fragment")});
    REQUIRE(editing->completer() != nullptr);
    auto *model = editing->completer()->model();
    REQUIRE(model != nullptr);
    REQUIRE(model->rowCount() == 2);
    CHECK(model->index(0, 0).data().toString() == QStringLiteral("project://shaders/tint"));
    // Still a text field: what is typed is what is committed.
    CHECK(editing->text() == logical.inlineEditText());
}

TEST_CASE("A Frame Plan band editor survives the window going inactive, and "
          "still ends when the focus really leaves it",
          "[devstudio][frame-plan][q4][canvas-edit]") {
    application();
    EditingFixture fixture;
    fixture.open();
    auto &logical = fixture.logical();
    clickBand(fixture, QStringLiteral("wp343_mixed_a"),
              QStringLiteral("shader.fragment"));
    REQUIRE(logical.editingInline());
    logical.setInlineEditText(QStringLiteral("project://shaders/tint.frag"));
    QLineEdit *editing = inlineEditorWidget(logical);
    REQUIRE(editing != nullptr);

    // The application went to the back and came forward again. Nothing about
    // the edit changed.
    QFocusEvent away{QEvent::FocusOut, Qt::ActiveWindowFocusReason};
    QApplication::sendEvent(editing, &away);
    settle();
    REQUIRE(logical.editingInline());
    REQUIRE(logical.inlineEditText() ==
            QStringLiteral("project://shaders/tint.frag"));
    REQUIRE(inlineEditorWidget(logical) == editing);
    REQUIRE(fixture.recorder.submits == 0);

    // A popup taking the keyboard is the same kind of loan.
    QFocusEvent popup{QEvent::FocusOut, Qt::PopupFocusReason};
    QApplication::sendEvent(editing, &popup);
    settle();
    REQUIRE(logical.editingInline());
    REQUIRE(fixture.recorder.submits == 0);

    SECTION("Escape abandons what was typed") {
        // Escape reaches this panel as its own shortcut -- the one that leaves
        // a group -- so that is where it has to be answered while a band is
        // open. Sending the key to the line edit would not even reach it: the
        // shortcut map takes it first.
        auto *escape = fixture.widget.findChild<QAction *>(
            QStringLiteral("pelican.framePlanLeaveGroup"));
        REQUIRE(escape != nullptr);
        REQUIRE(escape->shortcut() == QKeySequence{Qt::Key_Escape});
        escape->trigger();
        settle();
        REQUIRE_FALSE(logical.editingInline());
        REQUIRE(fixture.recorder.submits == 0);
        // And with nothing open it is the group key again.
        REQUIRE(logical.property("pelicanCurrentGroupScope").toString().isEmpty());
        escape->trigger();
        settle();
        REQUIRE(fixture.recorder.submits == 0);
    }

    SECTION("the control: the focus really leaving is a person saying done") {
        QFocusEvent elsewhere{QEvent::FocusOut, Qt::MouseFocusReason};
        QApplication::sendEvent(editing, &elsewhere);
        settle();
        REQUIRE_FALSE(logical.editingInline());
        REQUIRE(fixture.recorder.submits == 1);
        const auto sent = Json::parse(
            QJsonDocument(fixture.recorder.params).toJson().toStdString());
        CHECK(sent.at("shader").at("fragment") ==
              "project://shaders/tint.frag");
    }
}

// The other half: `renderCurrentGraph` used to call closeInlineEditor()
// unconditionally, so any redraw under an open editor threw away what was
// being typed -- and the Frame Plan is redrawn by a plan reply, a theme
// switch, and an authoring-context reply, all of which can arrive mid-edit.
TEST_CASE("A redraw under an open Frame Plan band editor re-opens it with "
          "what was typed",
          "[devstudio][frame-plan][q4][canvas-edit]") {
    application();
    EditingFixture fixture;
    fixture.open();
    auto &logical = fixture.logical();
    clickBand(fixture, QStringLiteral("wp343_mixed_a"),
              QStringLiteral("shader.fragment"));
    REQUIRE(logical.editingInline());
    logical.setInlineEditText(QStringLiteral("project://shaders/carry.frag"));

    // A frame-plan reply arriving while somebody is typing.
    fixture.widget.receiveResult(
        QByteArray::fromStdString(fixture.wire.dump()));
    settle();
    REQUIRE(logical.editingInline());
    REQUIRE(logical.inlineEditText() ==
            QStringLiteral("project://shaders/carry.frag"));
    // The editor is over the band this draw made, not a dangling proxy from
    // the old one, and Enter still reaches it.
    QLineEdit *reopened = inlineEditorWidget(logical);
    REQUIRE(reopened != nullptr);
    REQUIRE(fixture.recorder.submits == 0);
    typeKey(reopened, Qt::Key_Return);
    REQUIRE_FALSE(logical.editingInline());
    REQUIRE(fixture.recorder.submits == 1);
    const auto sent = Json::parse(
        QJsonDocument(fixture.recorder.params).toJson().toStdString());
    CHECK(sent.at("shader").at("fragment") == "project://shaders/carry.frag");

    // The control: a draw that no longer offers that field has nothing to open
    // over, and the edit ends there rather than hanging on a band that is gone.
    REQUIRE(fixture.recorder.result);
    fixture.recorder.result(
        fixture.recorder.context_id,
        QByteArray::fromStdString(mixedAuthoringContext(fixture.graph).dump()));
    QApplication::processEvents();
    clickBand(fixture, QStringLiteral("wp343_mixed_a"),
              QStringLiteral("shader.fragment"));
    REQUIRE(logical.editingInline());
    auto without = mixedAuthoringContext(fixture.graph);
    for (auto &pass : without.at("graphs").at(0).at("passes")) {
        pass["editable_preset_pass"] = false;
        pass.erase("preset_declaration");
    }
    fixture.widget.receiveResult(
        QByteArray::fromStdString(fixture.wire.dump()));
    QApplication::processEvents();
    fixture.recorder.result(fixture.recorder.context_id,
                            QByteArray::fromStdString(without.dump()));
    QApplication::processEvents();
    REQUIRE(bandItem(logical, QStringLiteral("wp343_mixed_a"),
                     QStringLiteral("shader.fragment")) == nullptr);
    REQUIRE_FALSE(logical.editingInline());
}

TEST_CASE("N-8 add and delete reach the right form and the right RPC",
          "[devstudio][frame-plan][n8][canvas-edit]") {
    application();
    EditingFixture fixture;
    fixture.open();
    auto &logical = fixture.logical();

    SECTION("Add fullscreen pass names the node the new pass follows") {
        QString graph;
        QString after;
        fixture.widget.setPassInsertAction(
            [&](const QString &g, const QString &a) { graph = g; after = a; });
        QGraphicsItem *anchor = nodeItem(logical, "wp343_mixed_c");
        REQUIRE(anchor != nullptr);
        anchor->setSelected(true);
        QApplication::processEvents();
        auto *action = fixture.widget.findChild<QAction *>(
            QStringLiteral("pelican.framePlan.addFullscreenPass"));
        REQUIRE(action != nullptr);
        action->trigger();
        QApplication::processEvents();
        CHECK(graph.toStdString() == fixture.graph);
        CHECK(after == QStringLiteral("wp343_mixed_c"));
    }

    SECTION("Delete removes a managed fragment and refuses a preset pass by name") {
        QGraphicsItem *managed = nodeItem(logical, "wp343_mixed_c");
        REQUIRE(managed != nullptr);
        managed->setSelected(true);
        QApplication::processEvents();
        auto &view = logicalView(fixture.widget);
        view.setFocus();
        QTest::keyClick(&view, Qt::Key_Delete);
        QApplication::processEvents();
        REQUIRE(fixture.recorder.submits == 1);
        CHECK(fixture.recorder.method == QStringLiteral("remove_authored_pass"));
        const auto sent = Json::parse(
            QJsonDocument(fixture.recorder.params).toJson().toStdString());
        CHECK(sent.at("fragment_reference") == "project://passes/mixed.json");
        CHECK(sent.at("base_source_digest") == RootDigest);

        QGraphicsItem *preset = nodeItem(logical, "wp343_mixed_a");
        REQUIRE(preset != nullptr);
        managed->setSelected(false);
        preset->setSelected(true);
        QApplication::processEvents();
        QTest::keyClick(&view, Qt::Key_Delete);
        QApplication::processEvents();
        CHECK(fixture.recorder.submits == 1);
        const auto notice = statusText(fixture.widget);
        CHECK(notice.contains(QStringLiteral("wp343_mixed_a")));
    }
}

TEST_CASE("GL-5c decisions 1, 2 and 5: on a root that selects no preset the Adds hand the "
          "Graph the palette's op and say so; on a preset's root they keep their forms",
          "[devstudio][frame-plan][gl5c]") {
    application();
    EditingFixture fixture;
    QStringList placed;
    const auto action = [&](const char *name) {
        auto *found = fixture.widget.findChild<QAction *>(QString::fromLatin1(name));
        REQUIRE(found != nullptr);
        return found;
    };
    const auto materialForm = [&] {
        auto *form = fixture.widget.findChild<QDialog *>(QStringLiteral("pelican.materialPassForm"));
        return form != nullptr && form->isVisible();
    };

    SECTION("a graph root (config_kind direct): the Graph places them") {
        fixture.tweak_context = [](Json &context) {
            context["config_kind"] = "direct";
            context.erase("pipeline_preset");
        };
        fixture.widget.setGraphPlaceAction([&](const QString &op) {
            placed << op;
            return true;
        });
        bool own_pass = false;
        bool own_task = false;
        fixture.widget.setPassInsertAction([&](const QString &, const QString &) { own_pass = true; });
        fixture.widget.setTaskInsertAction([&] { own_task = true; });
        fixture.open();
        // Nothing picked: a palette node needs no anchor.
        action("pelican.framePlan.addMaterialPass")->trigger();
        action("pelican.framePlan.addFullscreenPass")->trigger();
        action("pelican.framePlan.addComputeTask")->trigger();
        auto *button = fixture.widget.findChild<QPushButton *>(
            QStringLiteral("pelican.framePlanAddMaterialPass"));
        REQUIRE(button != nullptr);
        button->click();
        QApplication::processEvents();
        CHECK(placed == QStringList{QStringLiteral("render.material_pass"),
                                    QStringLiteral("render.fullscreen"),
                                    QStringLiteral("render.compute"),
                                    QStringLiteral("render.material_pass")});
        CHECK_FALSE(materialForm());
        CHECK_FALSE(own_pass);
        CHECK_FALSE(own_task);
        CHECK(fixture.recorder.submits == 0);
        // Decision 5: the entries say where they open.
        CHECK(button->toolTip().startsWith(QStringLiteral("Opens in the Graph")));
        CHECK(action("pelican.framePlan.addFullscreenPass")
                  ->toolTip()
                  .contains(QStringLiteral("render.fullscreen")));
        CHECK(action("pelican.framePlan.addComputeTask")
                  ->toolTip()
                  .startsWith(QStringLiteral("Opens in the Graph")));
    }

    SECTION("a root the Graph tab cannot place on: the Frame Plan's own form") {
        fixture.tweak_context = [](Json &context) {
            context["config_kind"] = "direct";
            context.erase("pipeline_preset");
        };
        fixture.widget.setGraphPlaceAction([&](const QString &op) {
            placed << op;
            return false;
        });
        fixture.open();
        action("pelican.framePlan.addMaterialPass")->trigger();
        QApplication::processEvents();
        CHECK(placed == QStringList{QStringLiteral("render.material_pass")});
        CHECK(materialForm());
    }

    SECTION("a root that selects a preset: the forms, as before") {
        fixture.widget.setGraphPlaceAction([&](const QString &op) {
            placed << op;
            return true;
        });
        QString after;
        bool own_task = false;
        fixture.widget.setPassInsertAction([&](const QString &, const QString &a) { after = a; });
        fixture.widget.setTaskInsertAction([&] { own_task = true; });
        fixture.open();
        action("pelican.framePlan.addMaterialPass")->trigger();
        QApplication::processEvents();
        CHECK(materialForm());
        QGraphicsItem *anchor = nodeItem(fixture.logical(), "wp343_mixed_c");
        REQUIRE(anchor != nullptr);
        anchor->setSelected(true);
        QApplication::processEvents();
        action("pelican.framePlan.addFullscreenPass")->trigger();
        action("pelican.framePlan.addComputeTask")->trigger();
        QApplication::processEvents();
        CHECK(placed.isEmpty());
        CHECK(after == QStringLiteral("wp343_mixed_c"));
        CHECK(own_task);
        CHECK_FALSE(action("pelican.framePlan.addMaterialPass")
                        ->toolTip()
                        .contains(QStringLiteral("Opens in the Graph")));
    }
}

TEST_CASE("GL-5c decision 3: Edit pass and a double-click open a pass the Graph edits there; a "
          "pass it reads only opens in the Pass Editor",
          "[devstudio][frame-plan][gl5c]") {
    application();
    EditingFixture fixture;
    QStringList in_graph;
    QStringList in_pass_editor;
    fixture.widget.setGraphPassOpenAction([&](const QString &pass) {
        in_graph << pass;
        return true;
    });
    fixture.widget.setPresetPassEditAction(
        [&](const QString &, const QString &pass) { in_pass_editor << pass; });
    fixture.open();
    auto &logical = fixture.logical();
    REQUIRE(logical.nodeEditRequested);
    const FramePlanNodeKey edited{fixture.graph, "wp343_mixed_c"};
    const FramePlanNodeKey read_only{fixture.graph, "wp343_mixed_a"};

    // No Graph to ask (as before GL-5c): the Pass Editor for both.
    logical.nodeEditRequested(edited);
    CHECK(in_graph.isEmpty());
    CHECK(in_pass_editor == QStringList{QStringLiteral("wp343_mixed_c")});
    in_pass_editor.clear();

    // The Graph says which passes it edits.
    fixture.widget.setGraphEditsPassQuery(
        [](const QString &pass) { return pass == QStringLiteral("wp343_mixed_c"); });
    logical.nodeEditRequested(edited);
    logical.nodeEditRequested(read_only);
    CHECK(in_graph == QStringList{QStringLiteral("wp343_mixed_c")});
    CHECK(in_pass_editor == QStringList{QStringLiteral("wp343_mixed_a")});

    // Edit pass on the toolbar, by the same rule.
    auto *edit_pass = fixture.widget.findChild<QPushButton *>(
        QStringLiteral("pelican.framePlanEditPresetPass"));
    REQUIRE(edit_pass != nullptr);
    QGraphicsItem *node = nodeItem(logical, "wp343_mixed_c");
    REQUIRE(node != nullptr);
    node->setSelected(true);
    QApplication::processEvents();
    edit_pass->click();
    QApplication::processEvents();
    CHECK(in_graph == QStringList{QStringLiteral("wp343_mixed_c"), QStringLiteral("wp343_mixed_c")});
    // Decision 5: it says where it opens.
    CHECK(edit_pass->toolTip().startsWith(QStringLiteral("Opens in the Graph")));
    CHECK(in_pass_editor == QStringList{QStringLiteral("wp343_mixed_a")});
}

TEST_CASE("N-8 a node drag writes one frame_plan placement, and a legacy root writes none",
          "[devstudio][frame-plan][n8][placement]") {
    application();
    EditingFixture fixture;
    Json stored = Json::object();
    int writes = 0;
    bool allowed = true;
    fixture.widget.setLayoutCapability(FramePlanLayoutCapability{
        .read = [&] { return stored; },
        .write = [&](const nlohmann::json &frame_plan, QString *reason) {
            if (!allowed) {
                if (reason != nullptr)
                    *reason = QStringLiteral(
                        "Node positions are not saved: passes/main.json is a "
                        "legacy rendering document.");
                return false;
            }
            stored = frame_plan;
            ++writes;
            return true;
        }});
    fixture.open();
    auto &logical = fixture.logical();
    QGraphicsItem *node = nodeItem(logical, "wp343_mixed_a");
    REQUIRE(node != nullptr);
    node->setPos(QPointF{321.0, -47.5});
    QApplication::processEvents();

    REQUIRE(writes >= 1);
    REQUIRE(stored.contains(fixture.graph));
    const auto &placed = stored.at(fixture.graph).at("nodes");
    REQUIRE(placed.contains("wp343_mixed_a"));
    CHECK(placed.at("wp343_mixed_a").at("x") == 321.0);
    CHECK(placed.at("wp343_mixed_a").at("y") == -47.5);

    // What the document holds is what the canvas draws next time.
    logical.setPlacement(fixture.graph,
                         {{"wp343_mixed_a", QPointF{12.0, 34.0}}});
    QApplication::processEvents();
    CHECK(nodeItem(logical, "wp343_mixed_a")->pos() == QPointF{12.0, 34.0});

    // A legacy root: nothing is written, and the notice says which file and
    // why -- a refusal a user can tell from a failed write.
    allowed = false;
    const int before = writes;
    nodeItem(logical, "wp343_mixed_a")->setPos(QPointF{99.0, 99.0});
    QApplication::processEvents();
    CHECK(writes == before);
    const auto notice = statusText(fixture.widget);
    CHECK(notice.contains(QStringLiteral("legacy")));
    CHECK(notice.contains(QStringLiteral("main.json")));
}
// Q-5, the Frame Plan half. Its placement rides in the Graph tab's one
// `set_layout`, and the reply to that write does not redraw this canvas at
// all; the one redraw a placement causes here is the placement being read
// back (the tab shown again, the capability handed over). That redraw keeps
// what the person has picked.
TEST_CASE("Q-5 a Frame Plan placement read back keeps the picked nodes",
          "[devstudio][frame-plan][n8][placement][q5]") {
    application();
    EditingFixture fixture;
    Json stored = Json::object();
    fixture.widget.setLayoutCapability(FramePlanLayoutCapability{
        .read = [&] { return stored; },
        .write = [&](const nlohmann::json &frame_plan, QString *) {
            stored = frame_plan;
            return true;
        }});
    fixture.open();
    auto &logical = fixture.logical();
    QGraphicsItem *moved = nodeItem(logical, "wp343_mixed_a");
    QGraphicsItem *other = nodeItem(logical, "wp343_mixed_c");
    REQUIRE(moved != nullptr);
    REQUIRE(other != nullptr);
    moved->setPos(QPointF{321.0, -47.5});
    QApplication::processEvents();
    REQUIRE(stored.contains(fixture.graph));

    moved->setSelected(true);
    other->setSelected(true);
    QApplication::processEvents();
    REQUIRE(logical.selectedNodes().size() == 2);
    // Items made by this draw carry a mark; a redraw makes new ones without it.
    constexpr int Marked = Qt::UserRole + 905;
    moved->setData(Marked, true);

    fixture.widget.refreshAuthoringContext();
    QApplication::processEvents();
    // It really was drawn again, from what the document holds.
    REQUIRE_FALSE(nodeItem(logical, "wp343_mixed_a")->data(Marked).toBool());
    CHECK(nodeItem(logical, "wp343_mixed_a")->pos() == QPointF{321.0, -47.5});
    CHECK(logical.selectedNodes().size() == 2);
    CHECK(nodeItem(logical, "wp343_mixed_a")->isSelected());
    CHECK(nodeItem(logical, "wp343_mixed_c")->isSelected());
}
// F-3. A shader that failed to reload is marked on the node whose shader it
// is, by the path the engine names; a failure no node reads marks nothing and
// is said in one line; the mark goes when the error does.
TEST_CASE("F-3 a shader reload failure marks the node that reads the file, "
          "and only while it fails",
          "[devstudio][frame-plan][f3][shader-reload]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = std::filesystem::path{temporary.path().toStdWString()};
    std::filesystem::create_directories(root / "shaders");
    {
        std::ofstream project{root / "project.json", std::ios::binary};
        project << Json{{"schema", "pelican.project"},
                        {"version", 1},
                        {"name", "f3-reload"}}
                       .dump();
        for (const char *file : {"tint.frag", "other.frag", "unread.frag"}) {
            std::ofstream shader{root / "shaders" / file, std::ios::binary};
            shader << "#version 450\nvoid main(){}\n";
        }
    }
    const auto fragment = [](const std::string &stem) {
        return Json{
            {"state", "resolved"},
            {"stages",
             Json::array(
                 {{{"stage", "vertex"},
                   {"effective_ref", "engine://fullscreen"},
                   {"origin", "engine_default"},
                   {"source_open_ref", nullptr},
                   {"source_open_reason", "embedded_engine_resource"}},
                  {{"stage", "fragment"},
                   {"declared_ref", "project://shaders/" + stem},
                   {"effective_ref", "project://shaders/" + stem},
                   {"origin", "authored"},
                   {"source_open_ref",
                    "project://shaders/" + stem + ".frag"}}})}};
    };

    EditingFixture fixture;
    for (auto &node : fixture.wire["nodes"]) {
        if (node.at("name") == "wp343_mixed_a")
            node["shader_resolution"] = fragment("tint");
        if (node.at("name") == "wp343_mixed_c")
            node["shader_resolution"] = fragment("other");
    }
    fixture.widget.setProjectRoot(root);
    fixture.open();
    auto &logical = fixture.logical();
    REQUIRE(nodeItem(logical, "wp343_mixed_a") != nullptr);
    REQUIRE(nodeItem(logical, "wp343_mixed_c") != nullptr);

    const auto marked = [&] {
        StringSet names;
        for (QGraphicsItem *item :
             itemsOfKind(logical, FramePlanNodeShaderErrorItem))
            names.insert(item->data(FramePlanNameRole).toString().toStdString());
        return names;
    };
    const auto status = [](const Json &last_error) {
        return QByteArray::fromStdString(
            Json{{"reload",
                  {{"state", "watching"},
                   {"runtime",
                    {{"pelican.shaders",
                      {{"attempted", 2},
                       {"applied", 1},
                       {"failed", 1},
                       {"last_error", last_error},
                       {"details", {{"enabled", true}}}}}}}}}}
                .dump());
    };
    // The engine writes the path it compiled, in its own spelling.
    const auto engine_path = [&](const char *file) {
        return std::filesystem::canonical(root / "shaders" / file).string();
    };
    const auto failure = [&](const char *file) {
        const auto path = engine_path(file);
        return "Shader compile failed: " + path + "\n" + path +
               ":3: error: 'vaule0' : undeclared identifier\n"
               "1 error generated.";
    };
    CHECK(marked().empty());

    SECTION("the path names a node's fragment: that node, and only it") {
        fixture.widget.receiveShaderReloadStatus(status(failure("tint.frag")));
        CHECK(marked() == StringSet{"wp343_mixed_a"});
        const auto marks = itemsOfKind(logical, FramePlanNodeShaderErrorItem);
        REQUIRE(marks.size() == 1);
        CHECK(marks.front()->toolTip() ==
              QString::fromStdString(engine_path("tint.frag") +
                                     ":3: error: 'vaule0' : undeclared "
                                     "identifier"));
        CHECK(marks.front()->parentItem() == nodeItem(logical, "wp343_mixed_a"));

        // A redraw of the canvas keeps the mark on its node.
        subtreeDepth(fixture.widget).setValue(63);
        QApplication::processEvents();
        CHECK(marked() == StringSet{"wp343_mixed_a"});

        // The same shader fixed: the next reading has no error, the mark goes.
        fixture.widget.receiveShaderReloadStatus(status(nullptr));
        CHECK(marked().empty());
    }

    SECTION("two failures joined by the reload service mark both nodes") {
        fixture.widget.receiveShaderReloadStatus(
            status(failure("tint.frag") + "; " + failure("other.frag")));
        CHECK(marked() == StringSet{"wp343_mixed_a", "wp343_mixed_c"});
    }

    SECTION("a file no drawn node reads marks nothing and is said in a line") {
        fixture.widget.receiveShaderReloadStatus(status(failure("unread.frag")));
        CHECK(marked().empty());
        CHECK(statusText(fixture.widget)
                  .startsWith(QStringLiteral("Shader reload failed: ")));
    }
}
// F-3 / Q-2. The reload status is read because a shader was written, never
// because time passed: one read when the watched files change, a bounded run
// after a write that stops as soon as the engine's reading moves (or can never
// move), and nothing at all while nobody writes.
TEST_CASE("F-3 the shader reload status is read after a write, a bounded "
          "number of times, and never on a clock",
          "[devstudio][frame-plan][f3][shader-reload][q2]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto shader =
        std::filesystem::path{temporary.path().toStdWString()} / "a.frag";
    {
        std::ofstream file{shader, std::ios::binary};
        file << "#version 450\nvoid main(){}\n";
    }
    std::vector<qint64> requests;
    std::vector<bool> applied_after_change;
    ShaderReloadFollower follower{ShaderReloadFollower::Hooks{
        .request_status =
            [&](QString *) {
                requests.push_back(static_cast<qint64>(requests.size()) + 9000);
                return requests.back();
            },
        .apply =
            [&](const ShaderReloadMarks &, const ShaderReloadReading &,
                bool after_change) {
                applied_after_change.push_back(after_change);
            },
    }};
    const auto reading = [](std::uint64_t attempted, bool enabled) {
        return QByteArray::fromStdString(
            Json{{"reload",
                  {{"runtime",
                    {{"pelican.shaders",
                      {{"attempted", attempted},
                       {"applied", attempted},
                       {"failed", 0},
                       {"last_error", nullptr},
                       {"details", {{"enabled", enabled}}}}}}}}}}
                .dump());
    };
    const auto answer = [&](const QByteArray &status) {
        REQUIRE(follower.pendingRequest() != 0);
        REQUIRE(follower.receive(follower.pendingRequest(), status));
    };
    const auto wait = [](int ms) { QTest::qWait(ms); };

    // Nothing watched: nothing read.
    wait(100);
    CHECK(requests.empty());

    // A new set of files is read once, so marks start from what is known.
    follower.setSources(NodeShaderSources{{"pass", {shader}}});
    wait(50);
    REQUIRE(requests.size() == 1);
    answer(reading(1, true));
    CHECK(applied_after_change == std::vector<bool>{false});
    // The same set again is not a reason to ask.
    follower.setSources(NodeShaderSources{{"pass", {shader}}});
    // And idle is idle: no clock asks again.
    wait(1500);
    CHECK(requests.size() == 1);

    SECTION("a write is followed until the engine's reading moves") {
        follower.sourceChanged();
        wait(ShaderReloadFollower::FirstReadDelayMs + 100);
        REQUIRE(requests.size() == 2);
        answer(reading(1, true)); // the engine has not got to it yet
        wait(ShaderReloadFollower::FollowReadIntervalMs + 100);
        REQUIRE(requests.size() == 3);
        answer(reading(2, true)); // now it has
        wait(1000);
        CHECK(requests.size() == 3);
        CHECK(applied_after_change.back());
    }

    SECTION("an engine that does not reload shaders is asked once") {
        follower.sourceChanged();
        wait(ShaderReloadFollower::FirstReadDelayMs + 100);
        REQUIRE(requests.size() == 2);
        answer(reading(1, false));
        wait(1000);
        CHECK(requests.size() == 2);
    }

    SECTION("an engine whose reading never moves is asked a bounded number "
            "of times") {
        follower.sourceChanged();
        for (int read = 0; read < ShaderReloadFollower::MaxFollowReads + 4;
             ++read) {
            wait(ShaderReloadFollower::FirstReadDelayMs + 50);
            if (follower.pendingRequest() == 0) break;
            answer(reading(1, true));
        }
        wait(1000);
        CHECK(requests.size() ==
              1 + static_cast<std::size_t>(ShaderReloadFollower::MaxFollowReads));
    }

    SECTION("the write itself, on disk, is what starts the reads") {
        {
            std::ofstream file{shader, std::ios::binary | std::ios::trunc};
            file << "#version 450\nvoid main(){ }\n";
        }
        for (int tick = 0; tick < 60 && requests.size() < 2; ++tick) wait(50);
        CHECK(requests.size() == 2);
    }
}

// F-5. A write to a watched shader asks the engine to recompile: once per
// burst of writes, one request at a time, and its answer -- not a reading of
// the file watch's gate -- is what the panel shows.
TEST_CASE("F-5 a shader write sends one reload_shaders, one at a time, and "
          "reads the status once after it",
          "[devstudio][frame-plan][f5][shader-reload][q2]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto shader =
        std::filesystem::path{temporary.path().toStdWString()} / "a.frag";
    {
        std::ofstream file{shader, std::ios::binary};
        file << "#version 450\nvoid main(){}\n";
    }
    std::vector<qint64> reads;
    std::vector<qint64> reloads;
    std::vector<ShaderReloadOutcome> outcomes;
    ShaderReloadFollower follower{ShaderReloadFollower::Hooks{
        .request_status =
            [&](QString *) {
                reads.push_back(static_cast<qint64>(reads.size()) + 9000);
                return reads.back();
            },
        .apply = [](const ShaderReloadMarks &, const ShaderReloadReading &,
                    bool) {},
        .request_reload =
            [&](QString *) {
                reloads.push_back(static_cast<qint64>(reloads.size()) + 7000);
                return reloads.back();
            },
        .reloaded =
            [&](const ShaderReloadOutcome &outcome) {
                outcomes.push_back(outcome);
            },
    }};
    const auto status = QByteArray::fromStdString(
        Json{{"reload",
              {{"runtime",
                {{"pelican.shaders",
                  {{"attempted", 1},
                   {"applied", 1},
                   {"failed", 0},
                   {"last_error", nullptr},
                   {"details", {{"enabled", false},
                                {"gate_reason", "rpc driver"}}}}}}}}}}
            .dump());
    const auto wait = [](int ms) { QTest::qWait(ms); };
    const auto settle = ShaderReloadFollower::ReloadDebounceMs + 150;

    follower.setSources(NodeShaderSources{{"pass", {shader}}});
    wait(50);
    REQUIRE(reads.size() == 1);
    REQUIRE(follower.receive(reads.back(), status));
    CHECK(reloads.empty());

    SECTION("a burst of writes is one request") {
        follower.sourceChanged();
        wait(50);
        follower.sourceChanged();
        wait(50);
        follower.sourceChanged();
        CHECK(reloads.empty());
        wait(settle);
        REQUIRE(reloads.size() == 1);
        // No status read before the engine has answered.
        CHECK(reads.size() == 1);

        // A write while it is in flight waits for it.
        follower.sourceChanged();
        wait(settle);
        CHECK(reloads.size() == 1);

        REQUIRE(follower.receive(
            reloads.front(),
            QByteArray{R"({"committed":true,"changed":["project://shaders/a.frag"],"cache_hits":0,"cache_misses":1})"}));
        REQUIRE(outcomes.size() == 1);
        CHECK(outcomes.front().kind == ShaderReloadOutcome::Kind::committed);
        CHECK(outcomes.front().changed ==
              QStringList{QStringLiteral("project://shaders/a.frag")});
        // Then the one it held back, and one status read for the marks.
        CHECK(reloads.size() == 2);
        wait(50);
        CHECK(reads.size() == 2);
        CHECK(follower.receive(reads.back(), status));
        CHECK(follower.receive(reloads.back(),
                               QByteArray{R"({"committed":false,"attempted":false})"}));
        CHECK(outcomes.back().kind == ShaderReloadOutcome::Kind::unchanged);
        // Nothing moved: no read follows, and idle stays idle.
        wait(1000);
        CHECK(reads.size() == 2);
        CHECK(reloads.size() == 2);
    }

    SECTION("a refusal is read by its error_code") {
        follower.sourceChanged();
        wait(settle);
        REQUIRE(reloads.size() == 1);
        // Someone else's id is not ours.
        CHECK_FALSE(follower.reject(reloads.back() + 1,
                                    QStringLiteral("shader_reload_failed@1"),
                                    QStringLiteral("x")));
        const QString log = QStringLiteral(
            "Shader compile failed: a.frag\na.frag:2: error: 'vaule0' : "
            "undeclared identifier\n");
        REQUIRE(follower.reject(reloads.back(),
                                QStringLiteral("shader_reload_failed@1"), log));
        REQUIRE(outcomes.size() == 1);
        CHECK(outcomes.back().kind == ShaderReloadOutcome::Kind::failed);
        CHECK(outcomes.back().message == log);
        // The failure the engine keeps is read once for the marks.
        wait(50);
        CHECK(reads.size() == 2);
        // The one-line failure that follows the rejection is already handled.
        CHECK_FALSE(follower.fail(reloads.back(), QStringLiteral("RPC -32000: x")));

        follower.sourceChanged();
        wait(settle);
        REQUIRE(reloads.size() == 2);
        REQUIRE(follower.reject(
            reloads.back(), QStringLiteral("shader_reload_unavailable@1"),
            QStringLiteral("reload_shaders: this engine does not reload "
                           "shaders (replay)")));
        CHECK(outcomes.back().kind == ShaderReloadOutcome::Kind::unavailable);

        follower.sourceChanged();
        wait(settle);
        REQUIRE(reloads.size() == 3);
        REQUIRE(follower.fail(reloads.back(),
                              QStringLiteral("RPC -32601: Method not found")));
        CHECK(outcomes.back().kind == ShaderReloadOutcome::Kind::error);
    }

    SECTION("the write itself, on disk, is what sends it") {
        {
            std::ofstream file{shader, std::ios::binary | std::ios::trunc};
            file << "#version 450\nvoid main(){ }\n";
        }
        for (int tick = 0; tick < 60 && reloads.empty(); ++tick) wait(50);
        CHECK(reloads.size() == 1);
    }
}

// F-5. What the panel does with reload_shaders' answer: a compile failure
// marks the node that reads the file and hands its log on; a commit takes
// both down; and only the engine's refusal says the engine does not reload
// shaders -- a shut file watch in get_status says nothing now.
TEST_CASE("F-5 the reload answer marks, clears, and names a refusal",
          "[devstudio][frame-plan][f5][shader-reload]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = std::filesystem::path{temporary.path().toStdWString()};
    std::filesystem::create_directories(root / "shaders");
    {
        std::ofstream project{root / "project.json", std::ios::binary};
        project << Json{{"schema", "pelican.project"},
                        {"version", 1},
                        {"name", "f5-reload"}}
                       .dump();
        std::ofstream shader{root / "shaders" / "tint.frag", std::ios::binary};
        shader << "#version 450\nvoid main(){}\n";
    }
    EditingFixture fixture;
    for (auto &node : fixture.wire["nodes"]) {
        if (node.at("name") != "wp343_mixed_a") continue;
        node["shader_resolution"] = Json{
            {"state", "resolved"},
            {"stages",
             Json::array(
                 {{{"stage", "fragment"},
                   {"declared_ref", "project://shaders/tint"},
                   {"effective_ref", "project://shaders/tint"},
                   {"origin", "authored"},
                   {"source_open_ref", "project://shaders/tint.frag"}}})}};
    }
    std::vector<QString> logs;
    fixture.widget.setShaderReloadLogAction(
        [&](const QString &message) { logs.push_back(message); });
    fixture.widget.setProjectRoot(root);
    fixture.open();
    auto &logical = fixture.logical();
    REQUIRE(nodeItem(logical, "wp343_mixed_a") != nullptr);
    const auto marked = [&] {
        StringSet names;
        for (QGraphicsItem *item :
             itemsOfKind(logical, FramePlanNodeShaderErrorItem))
            names.insert(item->data(FramePlanNameRole).toString().toStdString());
        return names;
    };
    const auto path =
        std::filesystem::canonical(root / "shaders" / "tint.frag").string();
    const QString log = QString::fromStdString(
        "Shader compile failed: " + path + "\n" + path +
        ":3: error: 'vaule0' : undeclared identifier\n");

    // A shut file watch, read from get_status, is not news any more.
    fixture.widget.receiveShaderReloadStatus(QByteArray::fromStdString(
        Json{{"reload",
              {{"runtime",
                {{"pelican.shaders",
                  {{"attempted", 0},
                   {"applied", 0},
                   {"failed", 0},
                   {"last_error", nullptr},
                   {"details", {{"enabled", false},
                                {"gate_reason", "rpc driver"}}}}}}}}}}
            .dump()));
    CHECK_FALSE(statusText(fixture.widget)
                    .contains(QStringLiteral("does not reload shaders")));

    fixture.widget.receiveShaderReloadOutcome(ShaderReloadOutcome{
        .kind = ShaderReloadOutcome::Kind::failed, .message = log});
    CHECK(marked() == StringSet{"wp343_mixed_a"});
    REQUIRE(logs.size() == 1);
    CHECK(logs.back() == log);

    fixture.widget.receiveShaderReloadOutcome(ShaderReloadOutcome{
        .kind = ShaderReloadOutcome::Kind::committed,
        .changed = {QStringLiteral("project://shaders/tint.frag")}});
    CHECK(marked().empty());
    REQUIRE(logs.size() == 2);
    CHECK(logs.back().isEmpty());

    fixture.widget.receiveShaderReloadOutcome(ShaderReloadOutcome{
        .kind = ShaderReloadOutcome::Kind::unavailable,
        .message = QStringLiteral(
            "reload_shaders: this engine does not reload shaders (replay)")});
    CHECK(statusText(fixture.widget) ==
          QStringLiteral("A shader file changed; this engine does not reload "
                         "shaders (replay)."));
    // A reload that commits later takes that line down again.
    fixture.widget.receiveShaderReloadOutcome(ShaderReloadOutcome{
        .kind = ShaderReloadOutcome::Kind::committed});
    CHECK_FALSE(statusText(fixture.widget)
                    .contains(QStringLiteral("does not reload shaders")));
}

// --- F-1. Render targets declared from the Frame Plan.

namespace {

Json f1OwnedDeclaration() {
    return Json{{"name", "f1_owned"},
                {"extent_scale", 1.0},
                {"format", "R16G16B16A16_SFLOAT"},
                {"format_class", "scene"},
                {"mip_levels", "full"},
                {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
}

// The render target half of get_render_authoring_context: one target the
// editor owns, the screen output, and a preset's target.
Json withRenderTargets(Json context, const std::string &owned = "f1_owned") {
    auto owned_declaration = f1OwnedDeclaration();
    owned_declaration["name"] = owned;
    context["pipeline_preset"]["name"] = "hybrid_v1";
    context["render_targets"] = Json::array(
        {Json{{"name", owned},
              {"declaration", owned_declaration},
              {"authored_declaration", owned_declaration},
              {"editable", true},
              {"source", "authoring"}},
         Json{{"name", "display"},
              {"declaration", {{"name", "display"}, {"format_class", "display"}}},
              {"editable", false},
              {"source", "engine"}},
         Json{{"name", "gbuffer_albedo"},
              {"declaration", {{"name", "gbuffer_albedo"}}},
              {"editable", false},
              {"source", "preset"}}});
    context["render_target_vocabulary"] = {
        {"formats", Json::array({"R8G8B8A8_UNORM", "R16G16B16A16_SFLOAT"})},
        {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED", "STORAGE"})}};
    return context;
}

} // namespace

TEST_CASE("F-4 a pass a direct root declares is refused as the root's, not a preset's",
          "[devstudio][frame-plan][f4][canvas-edit]") {
    // The shape a Blank pipeline project's context has: no pipeline_preset,
    // config_kind "direct", and `present` declared by the root itself.
    const Json present{
        {"name", "present"},
        {"type", "fullscreen"},
        {"input", Json::array()},
        {"output", {{"color", Json::array({"display"})}, {"depth", nullptr}}},
        {"shader", {{"vertex", "engine://fullscreen"},
                    {"fragment", "project://shaders/present"}}}};
    Json context{
        {"source_reference", "project://passes/main_rendering_config.json"},
        {"source_digest", {{"hex", RootDigest}}},
        {"config_kind", "direct"},
        {"graphs",
         Json::array({Json{
             {"name", "main_render"},
             {"passes",
              Json::array(
                  {Json{{"name", "present"},
                        {"declaration_index", 0},
                        {"declaration", present},
                        {"editable_preset_pass", false},
                        {"editable_feature_pass", false},
                        {"provenance", {{"source", "project"},
                                        {"provider_feature", ""},
                                        {"provider_reference", ""}}}},
                   Json{{"name", "gizmo_pass"},
                        {"declaration_index", 1},
                        {"declaration", {{"name", "gizmo_pass"},
                                         {"type", "gizmo"}}},
                        {"editable_preset_pass", false},
                        {"editable_feature_pass", false},
                        {"provenance", {{"source", "engine"},
                                        {"provider_feature", ""},
                                        {"provider_reference", ""}}}}})},
             {"anchor_candidates",
              Json::array({Json{{"position", 0}, {"insert", "before:present"}},
                           Json{{"position", 1}, {"insert", "after:present"}}})}}})}};

    const auto direct = framePlanAuthoringFromContext(context);
    REQUIRE(direct.received);
    CHECK(direct.direct);
    const auto refused = framePlanPassResourceRequest(
        direct, "main_render", "present", {"scene_color"}, true);
    CHECK_FALSE(refused.sendable());
    CHECK(refused.refusal.find("present") != std::string::npos);
    CHECK(refused.refusal.find("project://passes/main_rendering_config.json") !=
          std::string::npos);
    CHECK(refused.refusal.find("selects no preset") != std::string::npos);
    // There is no preset in this project, so it cannot be told to fork one.
    CHECK(refused.refusal.find("fork") == std::string::npos);
    // A pass the engine adds to the same root is the engine's, not the file's.
    const auto engine_pass = framePlanPassShaderRequest(
        direct, "main_render", "gizmo_pass", "fragment", "project://shaders/x");
    CHECK_FALSE(engine_pass.sendable());
    CHECK(engine_pass.refusal.find("added by the engine") != std::string::npos);
    CHECK(engine_pass.refusal.find("fork") == std::string::npos);

    // The control: the same pass under an engine preset the project has not
    // forked is still told to fork it.
    context["config_kind"] = "preset";
    context["pipeline_preset"] = {
        {"reference", "engine://render_pipelines/hybrid_v1.json"},
        {"source_digest", {{"hex", PresetDigest}}}};
    const auto preset = framePlanAuthoringFromContext(context);
    CHECK_FALSE(preset.direct);
    const auto told_to_fork = framePlanPassResourceRequest(
        preset, "main_render", "present", {"scene_color"}, true);
    CHECK_FALSE(told_to_fork.sendable());
    CHECK(told_to_fork.refusal.find("fork") != std::string::npos);
}

TEST_CASE("F-6 the canvas edits a pass the direct root declares, in the root",
          "[devstudio][frame-plan][f6][canvas-edit]") {
    const Json present{
        {"name", "present"},
        {"type", "fullscreen"},
        {"input", Json::array()},
        {"output", {{"color", Json::array({"display"})}, {"depth", nullptr}}},
        {"shader", {{"vertex", "engine://fullscreen"},
                    {"fragment", "project://shaders/present"}}}};
    const Json context{
        {"source_reference", "project://passes/main_rendering_config.json"},
        {"source_digest", {{"hex", RootDigest}}},
        {"config_kind", "direct"},
        {"graphs",
         Json::array({Json{
             {"name", "main_render"},
             {"passes",
              Json::array(
                  {Json{{"name", "present"},
                        {"declaration_index", 0},
                        {"declaration", present},
                        {"editable_preset_pass", false},
                        {"editable_feature_pass", false},
                        {"editable_root_pass", true},
                        {"root_declaration", present},
                        {"provenance", {{"source", "project"},
                                        {"provider_feature", ""},
                                        {"provider_reference", ""}}}},
                   Json{{"name", "gizmo_pass"},
                        {"declaration_index", 1},
                        {"declaration", {{"name", "gizmo_pass"},
                                         {"type", "gizmo"}}},
                        {"editable_preset_pass", false},
                        {"editable_feature_pass", false},
                        {"editable_root_pass", false},
                        {"provenance", {{"source", "engine"},
                                        {"provider_feature", ""},
                                        {"provider_reference", ""}}}}})},
             {"anchor_candidates",
              Json::array({Json{{"position", 0}, {"insert", "before:present"}},
                           Json{{"position", 1}, {"insert", "after:present"}}})}}})}};

    const auto authoring = framePlanAuthoringFromContext(context);
    REQUIRE(authoring.received);
    // A drag connection: the root is named by base_source_digest alone.
    const auto connect = framePlanPassResourceRequest(
        authoring, "main_render", "present", {"scene_color"}, true);
    INFO(connect.refusal);
    REQUIRE(connect.sendable());
    CHECK(connect.method == "edit_preset_pass");
    CHECK(connect.params.at("base_source_digest") == RootDigest);
    CHECK_FALSE(connect.params.contains("base_preset_digest"));
    CHECK_FALSE(connect.params.contains("base_feature_digest"));
    CHECK(connect.params.at("input") == Json::array({"scene_color"}));
    CHECK(connect.params.at("output") == present.at("output"));
    CHECK(connect.params.at("shader") == present.at("shader"));
    // The in-place band, on the same rule.
    CHECK_FALSE(framePlanInlineFields(authoring, {}, "main_render", "present", "").empty());
    const auto band = framePlanPassShaderRequest(
        authoring, "main_render", "present", "fragment", "project://shaders/stripes");
    REQUIRE(band.sendable());
    CHECK_FALSE(band.params.contains("base_preset_digest"));
    CHECK(band.params.at("shader").at("fragment") == "project://shaders/stripes");
    // What the engine adds to the same root is still the engine's.
    const auto engine_pass = framePlanPassShaderRequest(
        authoring, "main_render", "gizmo_pass", "fragment", "project://shaders/x");
    CHECK_FALSE(engine_pass.sendable());
    CHECK(engine_pass.refusal.find("added by the engine") != std::string::npos);
    // Deleting a root pass is not part of F-6.
    CHECK_FALSE(framePlanPassRemoveRequest(authoring, "main_render", "present").sendable());
}

TEST_CASE("F-1 the render target form becomes the declaration the engine takes",
          "[devstudio][frame-plan][f1][render-targets]") {
    const auto authoring = framePlanAuthoringFromContext(
        withRenderTargets(mixedAuthoringContext("main_render")));
    REQUIRE(authoring.received);
    REQUIRE(authoring.findRenderTarget("f1_owned") != nullptr);
    CHECK(authoring.render_target_usages ==
          std::vector<std::string>{"COLOR_ATTACHMENT", "SAMPLED", "STORAGE"});
    CHECK(authoring.preset_name == "hybrid_v1");

    SECTION("a name alone is a scene color target the size of the screen") {
        RenderTargetFormValues values;
        values.name = "f1_new";
        const auto request = framePlanRenderTargetAddRequest(authoring, values);
        REQUIRE(request.sendable());
        CHECK(request.method == "edit_render_targets");
        CHECK(request.params.at("base_source_digest") == RootDigest);
        REQUIRE(request.params.at("ops").size() == 1);
        const auto &op = request.params.at("ops").at(0);
        CHECK(op.at("op") == "add");
        CHECK(op.at("target") ==
              Json{{"name", "f1_new"},
                   {"extent_scale", 1.0},
                   {"format", "R16G16B16A16_SFLOAT"},
                   {"format_class", "scene"},
                   {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}});
    }

    SECTION("a fixed size sends width and height, and the ratio does not travel") {
        RenderTargetFormValues values;
        values.name = "f1_fixed";
        values.kind = "explicit";
        values.format = "R8G8B8A8_UNORM";
        values.fixed_size = true;
        values.extent_scale = 0.5;
        values.width = 512;
        values.height = 256;
        const auto target = framePlanRenderTargetAddRequest(authoring, values)
                                .params.at("ops")
                                .at(0)
                                .at("target");
        CHECK(target.at("width") == 512);
        CHECK(target.at("height") == 256);
        // The parser requires extent_scale even when the fixed extent wins,
        // so the form's ratio (0.5) is replaced by the neutral 1.
        CHECK(target.at("extent_scale") == 1.0);
        CHECK(target.at("format_class") == "explicit(R8G8B8A8_UNORM)");
    }

    SECTION("a replace starts from the fragment's declaration") {
        auto values = renderTargetFormValues(f1OwnedDeclaration());
        CHECK(values.kind == "scene");
        CHECK_FALSE(values.fixed_size);
        values.extent_scale = 0.5;
        const auto request =
            framePlanRenderTargetReplaceRequest(authoring, "f1_owned", values);
        REQUIRE(request.sendable());
        const auto &op = request.params.at("ops").at(0);
        CHECK(op.at("op") == "replace");
        CHECK(op.at("name") == "f1_owned");
        CHECK(op.at("target").at("extent_scale") == 0.5);
        // A field the form does not show travels unchanged.
        CHECK(op.at("target").at("mip_levels") == "full");
        CHECK(framePlanRenderTargetReplaceRequest(
                  authoring, "f1_owned",
                  renderTargetFormValues(f1OwnedDeclaration()))
                  .refusal.find("already") != std::string::npos);
    }

    SECTION("a target the editor does not own makes no RPC") {
        const auto display = framePlanRenderTargetRemoveRequest(authoring, "display");
        CHECK_FALSE(display.sendable());
        CHECK(display.method.empty());
        CHECK(display.refusal.find("screen output the engine provides") !=
              std::string::npos);
        const auto preset =
            framePlanRenderTargetRemoveRequest(authoring, "gbuffer_albedo");
        CHECK_FALSE(preset.sendable());
        CHECK(preset.refusal.find("preset hybrid_v1") != std::string::npos);
        const auto owned = framePlanRenderTargetRemoveRequest(authoring, "f1_owned");
        REQUIRE(owned.sendable());
        CHECK(owned.params.at("ops") ==
              Json::array({Json{{"op", "remove"}, {"name", "f1_owned"}}}));
    }

    SECTION("an empty name is refused in the form") {
        const auto request =
            framePlanRenderTargetAddRequest(authoring, RenderTargetFormValues{});
        CHECK_FALSE(request.sendable());
        CHECK_FALSE(request.refusal.empty());
    }

    SECTION("a rejection is one sentence chosen by its code") {
        const auto referenced = framePlanRenderTargetRejection(
            Json{{"code", "render_target_referenced"},
                 {"name", "f1_owned"},
                 {"users", Json::array({"f1_writer", "f1_reader"})}});
        CHECK(referenced.find("f1_owned") != std::string::npos);
        CHECK(referenced.find("f1_writer, f1_reader") != std::string::npos);
        const auto not_editable = framePlanRenderTargetRejection(
            Json{{"code", "render_target_not_editable"},
                 {"name", "gbuffer_albedo"},
                 {"source", "preset"}},
            "hybrid_v1");
        CHECK(not_editable.find("preset hybrid_v1") != std::string::npos);
        // G-6: a graph variant that refused the candidate is named with the
        // code; without one the sentence stays as it was.
        const auto variant = framePlanRenderTargetRejection(
            Json{{"code", "render_pipeline_preflight_failed"},
                 {"variant", "preview"},
                 {"message", "preview graph 'main_render' rejects authored "
                             "pass 'present': history read"}});
        CHECK(variant.starts_with("The preview graph refused it: "
                                  "render_pipeline_preflight_failed: "));
        const auto flat = framePlanRenderTargetRejection(
            Json{{"code", "render_pipeline_preflight_failed"},
                 {"message", "bad target"}});
        CHECK(flat == "The engine refused the render target: bad target");
    }
}

TEST_CASE("F-1 New render target sends one edit_render_targets from the form, "
          "and the target's node takes Delete",
          "[devstudio][frame-plan][f1][render-targets][canvas-edit]") {
    application();
    EditingFixture fixture;
    fixture.widget.resize(1400, 900);
    fixture.widget.show();
    fixture.widget.setEditCapability(fixture.recorder.capability());
    fixture.widget.receiveResult(QByteArray::fromStdString(fixture.wire.dump()));
    fixture.graph = fixture.wire.at("graph").get<std::string>();
    targetSelector(fixture.widget).setCurrentText("wp343_mixed_d_output");
    QApplication::processEvents();
    REQUIRE(fixture.recorder.result);
    fixture.recorder.result(
        fixture.recorder.context_id,
        QByteArray::fromStdString(
            withRenderTargets(mixedAuthoringContext(fixture.graph),
                              "wp343_mixed_d_output")
                .dump()));
    QApplication::processEvents();

    SECTION("the form's OK is one request with the typed name") {
        auto *action = fixture.widget.findChild<QAction *>(
            QStringLiteral("pelican.framePlan.newRenderTarget"));
        REQUIRE(action != nullptr);
        action->trigger();
        QApplication::processEvents();
        auto *form = fixture.widget.findChild<QWidget *>(
            QStringLiteral("pelican.renderTargetForm"));
        REQUIRE(form != nullptr);
        CHECK(form->isVisible());
        auto *name = form->findChild<QLineEdit *>(
            QStringLiteral("pelican.renderTargetForm.name"));
        REQUIRE(name != nullptr);
        // GL-5b, decision 5: it opens on a name the declared targets leave
        // free (presentNewRenderTarget, which the Graph opens it with too).
        CHECK(name->text() == QStringLiteral("render_target"));
        CHECK(form->findChild<QWidget *>(
                  QStringLiteral("pelican.renderTargetForm.usage.STORAGE")) !=
              nullptr);
        name->setText(QStringLiteral("f1_from_form"));
        auto *ok = form->findChild<QPushButton *>(
            QStringLiteral("pelican.renderTargetForm.ok"));
        REQUIRE(ok != nullptr);
        QTest::mouseClick(ok, Qt::LeftButton);
        QApplication::processEvents();
        REQUIRE(fixture.recorder.submits == 1);
        CHECK(fixture.recorder.method == QStringLiteral("edit_render_targets"));
        const auto sent = Json::parse(
            QJsonDocument(fixture.recorder.params).toJson().toStdString());
        CHECK(sent.at("ops").at(0).at("op") == "add");
        CHECK(sent.at("ops").at(0).at("target").at("name") == "f1_from_form");
        CHECK(sent.at("ops").at(0).at("target").at("extent_scale") == 1.0);
        CHECK_FALSE(form->isVisible());
    }

    SECTION("the selected target's node picks, and Delete removes it") {
        auto &logical = fixture.logical();
        const auto nodes = itemsOfKind(logical, FramePlanResourceNodeItem);
        REQUIRE(nodes.size() == 1);
        CHECK(nodes.front()->data(FramePlanNameRole).toString() ==
              QStringLiteral("wp343_mixed_d_output"));
        auto &view = logicalView(fixture.widget);
        view.centerOn(nodes.front());
        QApplication::processEvents();
        const QPoint at =
            view.mapFromScene(nodes.front()->sceneBoundingRect().center());
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QApplication::processEvents();
        REQUIRE(logical.pickedResource().has_value());
        view.setFocus();
        QTest::keyClick(&view, Qt::Key_Delete);
        QApplication::processEvents();
        REQUIRE(fixture.recorder.submits == 1);
        CHECK(fixture.recorder.method == QStringLiteral("edit_render_targets"));
        const auto sent = Json::parse(
            QJsonDocument(fixture.recorder.params).toJson().toStdString());
        CHECK(sent.at("ops") ==
              Json::array({Json{{"op", "remove"},
                                {"name", "wp343_mixed_d_output"}}}));
    }
}

namespace {

// F-7: the editing wire, with one more target the project declares and no
// pass reads or writes yet -- the state a person is in right after "New
// render target...".
Json declaredOnlyEditingWire() {
    static const Json resolved = resolvedMixedMembershipFramePlan(
        false, MixedFeatureNodes.size(),
        Json::array({Json{
            {"name", "f7_declared_only"},
            {"extent_scale", 1.0},
            {"format", "R16G16B16A16_SFLOAT"},
            {"format_class", "data"},
            {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
        }}));
    Json wire = resolved;
    for (auto &node : wire["nodes"]) {
        if (node.at("name") == "wp343_mixed_b") {
            node["kind"] = "compute";
            node["authored_name"] = "authored_consumer";
        }
    }
    return wire;
}

} // namespace

TEST_CASE("F-7 Whole frame is the first view of a graph, draws a target "
          "nothing reads, and a drag from it asks a pass to read it once",
          "[devstudio][frame-plan][f7][canvas-edit]") {
    application();
    EditingFixture fixture;
    fixture.wire = declaredOnlyEditingWire();
    fixture.widget.resize(1400, 900);
    fixture.widget.show();
    fixture.widget.setEditCapability(fixture.recorder.capability());
    fixture.widget.receiveResult(QByteArray::fromStdString(fixture.wire.dump()));
    fixture.graph = fixture.wire.at("graph").get<std::string>();
    QApplication::processEvents();
    REQUIRE(fixture.recorder.result);
    fixture.recorder.result(
        fixture.recorder.context_id,
        QByteArray::fromStdString(mixedAuthoringContext(fixture.graph).dump()));
    fixture.recorder.result(
        fixture.recorder.task_id,
        QByteArray::fromStdString(fixture.catalog.dump()));
    QApplication::processEvents();

    // A graph the panel has not shown before opens on the whole frame.
    QComboBox &selector = targetSelector(fixture.widget);
    CHECK(selector.currentIndex() == 0);
    CHECK(selector.currentText() == QStringLiteral("Whole frame"));
    auto &logical = fixture.logical();
    REQUIRE(logical.wholeFrame());
    CHECK(logical.property("pelicanWholeFrame").toBool());

    // Every pass is on the canvas, whatever it touches.
    for (const auto &node : fixture.wire.at("nodes"))
        CHECK(nodeItem(logical, node.at("name").get<std::string>()) != nullptr);

    // The declared-only target is a node of its own; a target a pass writes
    // is not drawn a second time -- the pass's output socket is its handle.
    QGraphicsItem *declared = nullptr;
    for (QGraphicsItem *item : itemsOfKind(logical, FramePlanResourceNodeItem)) {
        CHECK(item->data(FramePlanNameRole).toString() !=
              QStringLiteral("wp343_mixed_d_output"));
        if (item->data(FramePlanNameRole).toString() ==
            QStringLiteral("f7_declared_only"))
            declared = item;
    }
    REQUIRE(declared != nullptr);
    CHECK(socketItem(logical, QStringLiteral("f7_declared_only"),
                     QStringLiteral("f7_declared_only"),
                     QStringLiteral("output")) != nullptr);

    SECTION("a drag from its socket onto a pass is one request to read it") {
        fixture.dragSocket(QStringLiteral("f7_declared_only"),
                           QStringLiteral("f7_declared_only"),
                           QStringLiteral("output"),
                           nodeItem(logical, "wp343_mixed_a"));
        REQUIRE(fixture.recorder.submits == 1);
        CHECK(fixture.recorder.method == QStringLiteral("edit_preset_pass"));
        const auto sent = Json::parse(
            QJsonDocument(fixture.recorder.params).toJson().toStdString());
        CHECK(sent.at("pass_name") == "wp343_mixed_a");
        CHECK(sent.at("input") == Json::array({"f7_declared_only"}));
        // The press on the socket started a connection; it did not pick the
        // node it sits on.
        CHECK_FALSE(logical.pickedResource().has_value());
    }

    SECTION("a click on the node's body still picks it") {
        auto &view = logicalView(fixture.widget);
        view.centerOn(declared);
        QApplication::processEvents();
        const QPoint at =
            view.mapFromScene(declared->sceneBoundingRect().center());
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QApplication::processEvents();
        REQUIRE(logical.pickedResource().has_value());
        CHECK(logical.pickedResource()->name == "f7_declared_only");
    }

    SECTION("a refresh of the same graph stays on the whole frame, and a "
            "chosen target is kept") {
        fixture.widget.receiveResult(
            QByteArray::fromStdString(fixture.wire.dump()));
        QApplication::processEvents();
        CHECK(selector.currentIndex() == 0);
        CHECK(logical.wholeFrame());
        selector.setCurrentText(QStringLiteral("wp343_mixed_d_output"));
        QApplication::processEvents();
        CHECK_FALSE(logical.wholeFrame());
        fixture.widget.receiveResult(
            QByteArray::fromStdString(fixture.wire.dump()));
        QApplication::processEvents();
        CHECK(selector.currentText() == QStringLiteral("wp343_mixed_d_output"));
        CHECK(logical.property("pelicanTarget").toString() ==
              QStringLiteral("wp343_mixed_d_output"));
    }
}
// --- G-1: Add material pass...

namespace {

// The G-buffer formats of materialpassattachments.hpp, spelled the way a
// render target declaration spells them, and the depth format after them.
const std::vector<std::string> G1GbufferHdr{
    "R16G16B16A16_SFLOAT", "R16G16B16A16_SFLOAT", "R8G8B8A8_UNORM",
    "R16G16B16A16_SFLOAT", "R16G16B16A16_SFLOAT", "D32_SFLOAT"};
const std::vector<std::string> G1GbufferSdr{
    "B8G8R8A8_SRGB", "R16G16B16A16_SFLOAT", "R8G8B8A8_UNORM",
    "R16G16B16A16_SFLOAT", "B8G8R8A8_SRGB", "D32_SFLOAT"};

std::vector<std::string> g1Formats(const std::vector<MaterialPassSlot> &outputs) {
    std::vector<std::string> formats;
    for (const auto &slot : outputs) formats.push_back(slot.format);
    return formats;
}

Json g1Context(std::vector<std::string> features = {"hdr"}) {
    auto context = withRenderTargets(mixedAuthoringContext("main_render"));
    context["enabled_feature_names"] = features;
    return context;
}

} // namespace

TEST_CASE("T-1 Add material pass... opens with its outputs filled, reusing a "
          "declared target of the role's name whose format fits",
          "[devstudio][frame-plan][t1][material-pass]") {
    FramePlanAuthoring authoring;
    authoring.received = true;
    authoring.enabled_features_known = true;
    authoring.enabled_features = {"hdr"};
    // Nothing declared: every output is a new target of its role's name.
    CHECK(materialPassInitialTargets(authoring, "forward_opaque_v1") ==
          std::vector<std::string>{"scene_color", "scene_depth"});
    CHECK(materialPassInitialTargets(authoring, "deferred_geometry_v1") ==
          std::vector<std::string>{"gbuffer_albedo", "gbuffer_normal", "gbuffer_material",
                                   "gbuffer_worldpos", "gbuffer_emissive", "scene_depth"});

    const auto declare = [&](const std::string &name, Json declaration) {
        declaration["name"] = name;
        authoring.render_targets[name] =
            FramePlanRenderTarget{.name = name, .declaration = std::move(declaration)};
    };
    declare("scene_color", {{"format", "R16G16B16A16_SFLOAT"}});
    declare("scene_depth", {{"format", "D16_UNORM"}});
    declare("gbuffer_albedo", {{"format_class", "scene"}});
    declare("gbuffer_normal", {{"format_class", "display"}});
    declare("gbuffer_material", Json::object());
    // scene_color fits and is reused; scene_depth's format does not, so the
    // depth is a new scene_depth_2.
    CHECK(materialPassInitialTargets(authoring, "forward_opaque_v1") ==
          std::vector<std::string>{"scene_color", "scene_depth_2"});
    // By class when the declaration names no format; a target that says
    // neither is not reused.
    CHECK(materialPassInitialTargets(authoring, "deferred_geometry_v1") ==
          std::vector<std::string>{"gbuffer_albedo", "gbuffer_normal_2", "gbuffer_material_2",
                                   "gbuffer_worldpos", "gbuffer_emissive", "scene_depth_2"});
}

TEST_CASE("GL-5c decision 4: New render target for a material pass's colour output opens on "
          "the kind the contract's table gives it, not on the Scene color default",
          "[devstudio][frame-plan][gl5c][material-pass]") {
    application();
    FramePlanAuthoring authoring;
    authoring.received = true;
    authoring.enabled_features_known = true;
    authoring.render_target_formats = {"R8G8B8A8_UNORM", "R16G16B16A16_SFLOAT"};
    authoring.render_target_usages = {"COLOR_ATTACHMENT", "SAMPLED", "STORAGE"};
    // A forward pass's colour: R16G16B16A16_SFLOAT by its class, the declaration
    // Add material pass's Create missing targets makes for the same slot.
    const auto forward = materialPassColorTargetValues(authoring, "forward_opaque_v1", 0);
    REQUIRE(forward);
    CHECK(forward->name.empty());
    CHECK(forward->kind == "explicit");
    CHECK(forward->format == "R16G16B16A16_SFLOAT");
    const auto slot = materialPassSlots("forward_opaque_v1", false).front();
    CHECK(renderTargetDeclaration(*forward).at("format_class") == slot.format_class);
    CHECK(forward->usage == slot.usage);
    // The default the form had is the one the engine refused for it.
    CHECK(renderTargetDeclaration(RenderTargetFormValues{}).at("format_class") != slot.format_class);
    // Only colour outputs; a G-buffer's by its own slot (SDR: albedo is a
    // scene-class target, material eight bits of data).
    CHECK_FALSE(materialPassColorTargetValues(authoring, "forward_opaque_v1", 1));
    const auto albedo = materialPassColorTargetValues(authoring, "deferred_geometry_v1", 0);
    REQUIRE(albedo);
    CHECK(albedo->kind == "scene");
    const auto material = materialPassColorTargetValues(authoring, "deferred_geometry_v1", 2);
    REQUIRE(material);
    CHECK(material->kind == "data");
    CHECK(material->format == "R8G8B8A8_UNORM");
    CHECK_FALSE(materialPassColorTargetValues(authoring, "no_such_contract", 0));

    // The form opens on it, under the name it was asked for.
    RenderTargetForm form;
    presentNewRenderTarget(form, authoring, "forward_color", forward);
    QApplication::processEvents();
    auto *name = form.findChild<QLineEdit *>(QStringLiteral("pelican.renderTargetForm.name"));
    auto *kind = form.findChild<QComboBox *>(QStringLiteral("pelican.renderTargetForm.kind"));
    auto *format = form.findChild<QComboBox *>(QStringLiteral("pelican.renderTargetForm.format"));
    REQUIRE(name != nullptr);
    REQUIRE(kind != nullptr);
    REQUIRE(format != nullptr);
    CHECK(name->text() == QStringLiteral("forward_color"));
    CHECK(kind->currentData().toString() == QStringLiteral("explicit"));
    CHECK(format->currentText() == QStringLiteral("R16G16B16A16_SFLOAT"));
    // Without one: the defaults, as before.
    presentNewRenderTarget(form, authoring);
    QApplication::processEvents();
    CHECK(kind->currentData().toString() == QStringLiteral("scene"));
    form.close();
}

TEST_CASE("G-1 a material contract lays out its outputs, and the missing "
          "targets are made with the formats it requires",
          "[devstudio][frame-plan][g1][material-pass]") {
    SECTION("forward is one scene-linear color and depth") {
        for (const bool hdr : {true, false}) {
            const auto outputs = materialPassSlots("forward_opaque_v1", hdr);
            REQUIRE(outputs.size() == 2);
            CHECK(outputs[0].id == "color");
            CHECK(outputs[0].default_name == "scene_color");
            CHECK(outputs[0].format == "R16G16B16A16_SFLOAT");
            CHECK(outputs[0].format_class == "explicit(R16G16B16A16_SFLOAT)");
            CHECK(outputs[1].depth);
            CHECK(outputs[1].default_name == "scene_depth");
            CHECK(outputs[1].format == "D32_SFLOAT");
        }
        CHECK(materialPassSlots("forward_transparent_v1", true).size() == 2);
    }

    SECTION("a G-buffer is five named colors in the contract's order") {
        for (const auto *contract : {"deferred_geometry_v1", "legacy_gbuffer_v1"}) {
            const auto outputs = materialPassSlots(contract, true);
            REQUIRE(outputs.size() == 6);
            std::vector<std::string> ids;
            for (const auto &slot : outputs) ids.push_back(slot.id);
            CHECK(ids == std::vector<std::string>{"albedo", "normal", "material",
                                                  "world_position", "emissive",
                                                  "depth"});
            CHECK(g1Formats(outputs) == G1GbufferHdr);
            CHECK(g1Formats(materialPassSlots(contract, false)) == G1GbufferSdr);
            // Albedo and emissive follow the pipeline's HDR switch as targets,
            // the way the contract's table does.
            CHECK(outputs[0].format_class == "scene");
            CHECK(outputs[4].format_class == "scene");
            CHECK(outputs[1].format_class == "data");
        }
        CHECK(materialPassSlots("no_such_contract_v1", true).empty());
    }

    SECTION("Create missing targets fills the empty outputs and only them") {
        const std::vector<std::string> declared{"gbuffer_albedo", "display"};
        const auto filled = materialPassFillMissing(
            declared, "deferred_geometry_v1",
            {"", "my_normal", "", "", "", ""});
        CHECK(filled == std::vector<std::string>{
                            "gbuffer_albedo_2", "my_normal", "gbuffer_material",
                            "gbuffer_worldpos", "gbuffer_emissive", "scene_depth"});
    }

    const auto authoring = framePlanAuthoringFromContext(g1Context());
    REQUIRE(authoring.received);
    CHECK(materialPassHdr(authoring));
    MaterialPassFormValues values;
    values.graph = "main_render";
    values.insert = "before:wp343_mixed_a";
    values.name = "g1_gbuffer";
    values.contract = "deferred_geometry_v1";
    values.targets = materialPassFillMissing({"f1_owned", "display", "gbuffer_albedo"},
                                             values.contract, {});

    SECTION("Save's first request makes every new target, with the contract's format") {
        const auto request = materialPassTargetsRequest(authoring, values);
        REQUIRE(request.sendable());
        CHECK(request.method == "edit_render_targets");
        CHECK(request.params.at("base_source_digest") == RootDigest);
        const auto &ops = request.params.at("ops");
        REQUIRE(ops.size() == 6);
        std::vector<std::string> names;
        std::vector<std::string> formats;
        for (const auto &op : ops) {
            CHECK(op.at("op") == "add");
            names.push_back(op.at("target").at("name").get<std::string>());
            formats.push_back(op.at("target").at("format").get<std::string>());
        }
        CHECK(names == values.targets);
        CHECK(formats == G1GbufferHdr);
        CHECK(ops.at(5).at("target").at("usage") ==
              Json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"}));

        // A pipeline without "hdr" makes the SDR column.
        const auto sdr = framePlanAuthoringFromContext(g1Context({"bloom"}));
        CHECK_FALSE(materialPassHdr(sdr));
        const auto sdr_request = materialPassTargetsRequest(sdr, values);
        std::vector<std::string> sdr_formats;
        for (const auto &op : sdr_request.params.at("ops"))
            sdr_formats.push_back(op.at("target").at("format").get<std::string>());
        CHECK(sdr_formats == G1GbufferSdr);
    }

    SECTION("the pass waits for its targets, then is one add_authored_pass") {
        const auto early = materialPassAddRequest(authoring, values);
        CHECK_FALSE(early.sendable());
        CHECK(early.refusal.find("not declared yet") != std::string::npos);

        auto context = g1Context();
        for (const auto &name : values.targets)
            context["render_targets"].push_back(
                Json{{"name", name}, {"declaration", {{"name", name}}},
                     {"editable", true}, {"source", "authoring"}});
        const auto after = framePlanAuthoringFromContext(context);
        CHECK_FALSE(materialPassTargetsRequest(after, values).sendable());
        CHECK(materialPassTargetsRequest(after, values).refusal.empty());
        const auto request = materialPassAddRequest(after, values);
        REQUIRE(request.sendable());
        CHECK(request.method == "add_authored_pass");
        CHECK(request.params ==
              Json{{"base_source_digest", RootDigest},
                   {"graph", "main_render"},
                   {"insert", "before:wp343_mixed_a"},
                   {"pass",
                    {{"name", "g1_gbuffer"},
                     {"type", "material"},
                     {"material_contract", "deferred_geometry_v1"},
                     {"output",
                      {{"color", Json::array({values.targets[0], values.targets[1],
                                              values.targets[2], values.targets[3],
                                              values.targets[4]})},
                       {"depth", values.targets[5]}}}}}});
    }

    SECTION("the form refuses what the engine would, in one sentence") {
        auto empty = values;
        empty.targets[1].clear();
        CHECK(materialPassFormRefusal(authoring, empty).find("normal") !=
              std::string::npos);
        auto taken = values;
        taken.name = "wp343_mixed_a";
        CHECK(materialPassFormRefusal(authoring, taken).find("already has") !=
              std::string::npos);
        auto nowhere = values;
        nowhere.insert = "after:nobody";
        CHECK_FALSE(materialPassFormRefusal(authoring, nowhere).empty());
        CHECK(materialPassDefaultName(authoring, "main_render",
                                      "deferred_geometry_v1") ==
              "deferred_geometry");
    }
}

TEST_CASE("G-1 Add material pass... makes the missing targets, then adds the "
          "pass, in that order",
          "[devstudio][frame-plan][g1][material-pass][canvas-edit]") {
    application();
    EditingFixture fixture;
    fixture.widget.resize(1400, 900);
    fixture.widget.show();
    fixture.widget.setEditCapability(fixture.recorder.capability());
    fixture.widget.receiveResult(QByteArray::fromStdString(fixture.wire.dump()));
    fixture.graph = fixture.wire.at("graph").get<std::string>();
    QApplication::processEvents();
    REQUIRE(fixture.recorder.result);
    auto context = withRenderTargets(mixedAuthoringContext(fixture.graph));
    fixture.recorder.result(fixture.recorder.context_id,
                            QByteArray::fromStdString(context.dump()));
    QApplication::processEvents();

    auto *action = fixture.widget.findChild<QAction *>(
        QStringLiteral("pelican.framePlan.addMaterialPass"));
    REQUIRE(action != nullptr);
    CHECK(fixture.widget.findChild<QPushButton *>(
              QStringLiteral("pelican.framePlanAddMaterialPass")) != nullptr);
    action->trigger();
    QApplication::processEvents();
    auto *form = fixture.widget.findChild<QWidget *>(
        QStringLiteral("pelican.materialPassForm"));
    REQUIRE(form != nullptr);
    CHECK(form->isVisible());

    // Deferred geometry lays out five colors and a depth.
    auto *contract = form->findChild<QComboBox *>(
        QStringLiteral("pelican.materialPassForm.contract"));
    REQUIRE(contract != nullptr);
    contract->setCurrentIndex(
        contract->findData(QStringLiteral("deferred_geometry_v1")));
    QApplication::processEvents();
    auto *name = form->findChild<QLineEdit *>(
        QStringLiteral("pelican.materialPassForm.name"));
    REQUIRE(name != nullptr);
    CHECK(name->text() == QStringLiteral("deferred_geometry"));
    for (const auto *slot : {"albedo", "normal", "material", "world_position",
                             "emissive", "depth"})
        CHECK(form->findChild<QComboBox *>(
                  QStringLiteral("pelican.materialPassForm.target.%1")
                      .arg(QLatin1String{slot})) != nullptr);

    // T-1: the outputs are filled as the contract lays them out. The
    // preset's gbuffer_albedo declares no format, so it is not reused.
    const auto slot_text = [form](const char *slot) {
        auto *target = form->findChild<QComboBox *>(
            QStringLiteral("pelican.materialPassForm.target.%1").arg(QLatin1String{slot}));
        REQUIRE(target != nullptr);
        return target->currentText();
    };
    CHECK(slot_text("albedo") == QStringLiteral("gbuffer_albedo_2"));
    CHECK(slot_text("normal") == QStringLiteral("gbuffer_normal"));
    CHECK(slot_text("depth") == QStringLiteral("scene_depth"));

    // Save with an output emptied is refused inside the form: nothing is sent.
    auto *normal = form->findChild<QComboBox *>(
        QStringLiteral("pelican.materialPassForm.target.normal"));
    REQUIRE(normal != nullptr);
    normal->setEditText(QString{});
    auto *ok = form->findChild<QPushButton *>(
        QStringLiteral("pelican.materialPassForm.ok"));
    REQUIRE(ok != nullptr);
    QTest::mouseClick(ok, Qt::LeftButton);
    QApplication::processEvents();
    CHECK(fixture.recorder.submits == 0);
    CHECK(form->isVisible());

    auto *missing = form->findChild<QPushButton *>(
        QStringLiteral("pelican.materialPassForm.createMissing"));
    REQUIRE(missing != nullptr);
    QTest::mouseClick(missing, Qt::LeftButton);
    QApplication::processEvents();
    CHECK(slot_text("normal") == QStringLiteral("gbuffer_normal"));
    auto *albedo_state = form->findChild<QLabel *>(
        QStringLiteral("pelican.materialPassForm.state.albedo"));
    REQUIRE(albedo_state != nullptr);
    CHECK(albedo_state->text().startsWith(QStringLiteral("new")));
    auto *notice = form->findChild<QLabel *>(
        QStringLiteral("pelican.materialPassForm.notice"));
    REQUIRE(notice != nullptr);
    CHECK(notice->text().contains(QStringLiteral("stay declared")));

    QTest::mouseClick(ok, Qt::LeftButton);
    QApplication::processEvents();
    CHECK_FALSE(form->isVisible());
    REQUIRE(fixture.recorder.methods ==
            std::vector<QString>{QStringLiteral("edit_render_targets")});
    const auto targets = Json::parse(
        QJsonDocument(fixture.recorder.params).toJson().toStdString());
    REQUIRE(targets.at("ops").size() == 6);
    std::vector<std::string> made;
    for (const auto &op : targets.at("ops"))
        made.push_back(op.at("target").at("name").get<std::string>());
    // gbuffer_albedo is the preset's; the new one takes the next name.
    CHECK(made == std::vector<std::string>{"gbuffer_albedo_2", "gbuffer_normal",
                                           "gbuffer_material", "gbuffer_worldpos",
                                           "gbuffer_emissive", "scene_depth"});

    // The targets commit; the pass waits for the context that names them.
    fixture.recorder.result(
        fixture.recorder.submit_id,
        QByteArray::fromStdString(
            Json{{"status", "accepted"}, {"ticket", "g1-targets"}}.dump()));
    QApplication::processEvents();
    fixture.recorder.result(
        fixture.recorder.ticket_id,
        QByteArray::fromStdString(
            Json{{"status", "committed"}, {"committed", true}}.dump()));
    QApplication::processEvents();
    CHECK(fixture.recorder.methods.size() == 1);

    constexpr auto after_digest =
        "4444444444444444444444444444444444444444444444444444444444444444";
    context["source_digest"]["hex"] = after_digest;
    for (const auto &target : made)
        context["render_targets"].push_back(
            Json{{"name", target}, {"declaration", {{"name", target}}},
                 {"editable", true}, {"source", "authoring"}});
    fixture.recorder.result(fixture.recorder.context_id,
                            QByteArray::fromStdString(context.dump()));
    QApplication::processEvents();

    REQUIRE(fixture.recorder.methods ==
            std::vector<QString>{QStringLiteral("edit_render_targets"),
                                 QStringLiteral("add_authored_pass")});
    const auto pass = Json::parse(
        QJsonDocument(fixture.recorder.params).toJson().toStdString());
    CHECK(pass.at("base_source_digest") == after_digest);
    CHECK(pass.at("graph") == fixture.graph);
    CHECK(pass.at("insert") == "before:wp343_mixed_a");
    CHECK(pass.at("pass").at("name") == "deferred_geometry");
    CHECK(pass.at("pass").at("type") == "material");
    CHECK(pass.at("pass").at("material_contract") == "deferred_geometry_v1");
    CHECK(pass.at("pass").at("output").at("color") ==
          Json::array({"gbuffer_albedo_2", "gbuffer_normal", "gbuffer_material",
                       "gbuffer_worldpos", "gbuffer_emissive"}));
    CHECK(pass.at("pass").at("output").at("depth") == "scene_depth");

    SECTION("a refused pass says the targets it made stay") {
        fixture.recorder.result(
            fixture.recorder.submit_id,
            QByteArray::fromStdString(
                Json{{"status", "rejected"},
                     {"error",
                      {{"code", "render_pipeline_preflight_failed"},
                       {"message", "Material pass requires depth output: x"}}}}
                    .dump()));
        QApplication::processEvents();
        auto *status = fixture.widget.findChild<QLabel *>(
            QStringLiteral("pelican.framePlanStatus"));
        REQUIRE(status != nullptr);
        const QString shown = status->toolTip().isEmpty() ? status->text()
                                                          : status->toolTip();
        CHECK(shown.contains(QStringLiteral("requires depth output")));
        CHECK(shown.contains(QStringLiteral("stay declared")));
        CHECK(shown.contains(QStringLiteral("gbuffer_albedo_2")));
    }
}
// --- G-4: a Forward opaque pass in a pipeline with no deferred pass.

namespace {

// A direct root whose only pass is `present`, with a forward pass's two
// targets declared: what the G-1 fixture is once its probes are gone.
FramePlanAuthoring g4PresentOnly() {
    FramePlanAuthoring authoring;
    authoring.received = true;
    authoring.direct = true;
    authoring.source_digest = RootDigest;
    authoring.passes[{"main_render", "present"}].declaration =
        Json{{"name", "present"}, {"type", "fullscreen"}};
    authoring.order["main_render"] = {"present"};
    authoring.anchors["main_render"] = {{0, "before:present"}, {1, "after:present"}};
    authoring.render_targets["scene_color"].name = "scene_color";
    authoring.render_targets["scene_depth"].name = "scene_depth";
    return authoring;
}

MaterialPassFormValues g4ForwardValues(bool forward_default) {
    MaterialPassFormValues values;
    values.graph = "main_render";
    values.insert = "before:present";
    values.name = "forward_opaque";
    values.contract = "forward_opaque_v1";
    values.targets = {"scene_color", "scene_depth"};
    values.forward_default = forward_default;
    return values;
}

} // namespace

TEST_CASE("G-4 Forward opaque with no deferred pass offers the forward default, and Save "
          "carries it in the pass's own request",
          "[devstudio][frame-plan][g4][material-pass]") {
    auto authoring = g4PresentOnly();
    CHECK_FALSE(materialPassPipelineHasDeferredPass(authoring));
    CHECK(materialPassOffersForwardDefault(authoring, "forward_opaque_v1"));
    CHECK_FALSE(materialPassOffersForwardDefault(authoring, "forward_transparent_v1"));
    CHECK_FALSE(materialPassOffersForwardDefault(authoring, "deferred_geometry_v1"));

    const auto request = materialPassAddRequest(authoring, g4ForwardValues(true));
    INFO(request.refusal);
    REQUIRE(request.sendable());
    CHECK(request.method == "add_authored_pass");
    CHECK(request.params.at("material_route_default") == "forward");
    CHECK(request.params.at("pass").at("material_contract") == "forward_opaque_v1");
    CHECK_FALSE(materialPassAddRequest(authoring, g4ForwardValues(false))
                    .params.contains("material_route_default"));

    // A pipeline that draws deferred materials already does not need it; a
    // pass without a contract is a legacy G-buffer pass.
    for (const auto &declaration :
         {Json{{"type", "material"}, {"material_contract", "deferred_geometry_v1"}},
          Json{{"type", "material"}, {"material_contract", "legacy_gbuffer_v1"}},
          Json{{"type", "material"}}}) {
        CAPTURE(declaration.dump());
        auto with_gbuffer = authoring;
        with_gbuffer.passes[{"main_render", "gbuffer"}].declaration = declaration;
        CHECK(materialPassPipelineHasDeferredPass(with_gbuffer));
        CHECK_FALSE(materialPassOffersForwardDefault(with_gbuffer, "forward_opaque_v1"));
        CHECK_FALSE(materialPassAddRequest(with_gbuffer, g4ForwardValues(true))
                        .params.contains("material_route_default"));
    }
}

TEST_CASE("G-4 the form shows the forward default checked for Forward opaque only",
          "[devstudio][frame-plan][g4][material-pass]") {
    application();
    MaterialPassForm form;
    form.present(g4PresentOnly(), "main_render", "before:present", QStringLiteral("before present"));
    QApplication::processEvents();
    auto *checkbox = form.findChild<QCheckBox *>(
        QStringLiteral("pelican.materialPassForm.forwardDefault"));
    REQUIRE(checkbox != nullptr);
    CHECK(checkbox->isVisible());
    CHECK(checkbox->isChecked());
    CHECK(form.values().forward_default);
    auto *notice = form.findChild<QLabel *>(QStringLiteral("pelican.materialPassForm.notice"));
    REQUIRE(notice != nullptr);
    CHECK(notice->text().contains(QStringLiteral("restarts")));

    checkbox->setChecked(false);
    CHECK_FALSE(form.values().forward_default);
    CHECK_FALSE(notice->text().contains(QStringLiteral("restarts")));

    auto *contract = form.findChild<QComboBox *>(QStringLiteral("pelican.materialPassForm.contract"));
    REQUIRE(contract != nullptr);
    contract->setCurrentIndex(contract->findData(QStringLiteral("deferred_geometry_v1")));
    QApplication::processEvents();
    CHECK_FALSE(checkbox->isVisible());
    CHECK_FALSE(form.values().forward_default);

    // Presenting again starts from checked.
    form.present(g4PresentOnly(), "main_render", "before:present", QStringLiteral("before present"));
    QApplication::processEvents();
    CHECK(checkbox->isVisible());
    CHECK(checkbox->isChecked());
    form.close();
}

TEST_CASE("G-4 a material_route_unserved@1 refusal names the materials and the fix",
          "[devstudio][frame-plan][g4][material-pass]") {
    const Json error{
        {"code", "material_route_unserved@1"},
        {"message", "material_route_unserved@1: ..."},
        {"unserved",
         Json::array({{{"route", "deferred_geometry"},
                       {"shader_contract", "gbuffer_v1"},
                       {"materials",
                        Json::array({{{"name", "pelican.builtin.transparent"}, {"origin", "builtin"}},
                                     {{"name", "box"}, {"origin", "asset"}, {"source", "project://box.glb"}}})},
                       {"hint", "set material_route_default to \"forward\" on the rendering config "
                                "root, or add a deferred_geometry_v1 material pass"}}})},
    };
    const auto notice = materialRouteUnservedNotice(error);
    CHECK(notice.find("pelican.builtin.transparent (engine built-in), box") != std::string::npos);
    CHECK(notice.find("deferred_geometry route") != std::string::npos);
    CHECK(notice.find("To fix: set material_route_default") != std::string::npos);
    CHECK(materialRouteUnservedNotice(Json{{"code", "render_pipeline_preflight_failed"}}).empty());
}

// --- Q-13. A commit in another panel -- a node's placement included -- moves
// the rendering document's digest; the Frame Plan's next edit carries the new
// one, and a refusal for a moved document is rebuilt once.

namespace {

constexpr auto MovedDigest =
    "5555555555555555555555555555555555555555555555555555555555555555";

Json movedContext(const std::string &graph, const char *digest) {
    auto context = withRenderTargets(mixedAuthoringContext(graph));
    context["source_digest"]["hex"] = digest;
    return context;
}

Json movedCatalog(const char *digest) {
    auto catalog = authoredTaskCatalog();
    catalog["source_digest"]["hex"] = digest;
    return catalog;
}

Json sentParams(const FramePlanEditRecorder &recorder) {
    return Json::parse(QJsonDocument(recorder.params).toJson().toStdString());
}

// Opens Add material pass... and presses OK on what it offers.
void saveMaterialPass(FramePlanWidget &widget) {
    auto *action = widget.findChild<QAction *>(
        QStringLiteral("pelican.framePlan.addMaterialPass"));
    REQUIRE(action != nullptr);
    action->trigger();
    QApplication::processEvents();
    auto *form = widget.findChild<QWidget *>(
        QStringLiteral("pelican.materialPassForm"));
    REQUIRE(form != nullptr);
    REQUIRE(form->isVisible());
    auto *ok = form->findChild<QPushButton *>(
        QStringLiteral("pelican.materialPassForm.ok"));
    REQUIRE(ok != nullptr);
    QTest::mouseClick(ok, Qt::LeftButton);
    QApplication::processEvents();
}

QByteArray movedRefusal() {
    return QByteArray::fromStdString(
        Json{{"status", "rejected"},
             {"committed", false},
             {"error",
              {{"code", "external_modification"},
               {"message", "render config base digest no longer matches the "
                           "engine-owned document"}}}}
            .dump());
}

} // namespace

TEST_CASE("Q-13 a placement another panel committed reaches the Frame Plan "
          "before its next edit",
          "[devstudio][frame-plan][q13][canvas-edit]") {
    application();
    EditingFixture fixture;
    const auto digest = std::make_shared<RenderConfigDigest>();
    fixture.widget.setRenderConfigDigest(digest);
    fixture.tweak_context = [](Json &context) { context = withRenderTargets(context); };
    fixture.open();
    // What the Frame Plan read is what the others are told.
    CHECK(digest->current() == RootDigest);

    // Mark the existing items: a digest-only refresh must keep them, even
    // though the next actual edit must use the new digest below.
    auto &canvas = fixture.logical();
    constexpr int Marked = Qt::UserRole + 910;
    REQUIRE_FALSE(canvas.items().empty());
    for (auto *item : canvas.items()) item->setData(Marked, true);
    // The Graph tab's set_layout committed: a new revision of the document.
    const auto context_before = fixture.recorder.context_id;
    const auto task_before = fixture.recorder.task_id;
    digest->publish(MovedDigest);
    QApplication::processEvents();
    REQUIRE(fixture.recorder.context_id != context_before);
    REQUIRE(fixture.recorder.task_id != task_before);
    fixture.recorder.result(
        fixture.recorder.context_id,
        QByteArray::fromStdString(movedContext(fixture.graph, MovedDigest).dump()));
    fixture.recorder.result(
        fixture.recorder.task_id,
        QByteArray::fromStdString(movedCatalog(MovedDigest).dump()));
    QApplication::processEvents();

    for (auto *item : canvas.items()) CHECK(item->data(Marked).toBool());
    saveMaterialPass(fixture.widget);
    REQUIRE(fixture.recorder.submits == 1);
    CHECK(fixture.recorder.method == QStringLiteral("edit_render_targets"));
    CHECK(sentParams(fixture.recorder).at("base_source_digest") == MovedDigest);

    // A panel's own read is not news to itself: nothing more is asked.
    const auto context_after = fixture.recorder.context_id;
    digest->publish(MovedDigest, &fixture.widget);
    QApplication::processEvents();
    CHECK(fixture.recorder.context_id == context_after);
}

TEST_CASE("Q-13 an edit refused for a moved document is built again once from "
          "a fresh context, and the second refusal offers Reload",
          "[devstudio][frame-plan][q13][canvas-edit]") {
    application();
    EditingFixture fixture;
    fixture.widget.resize(1400, 900);
    fixture.widget.show();
    fixture.widget.setEditCapability(fixture.recorder.capability());
    fixture.widget.receiveResult(QByteArray::fromStdString(fixture.wire.dump()));
    fixture.graph = fixture.wire.at("graph").get<std::string>();
    QApplication::processEvents();
    REQUIRE(fixture.recorder.result);
    fixture.recorder.result(
        fixture.recorder.context_id,
        QByteArray::fromStdString(movedContext(fixture.graph, RootDigest).dump()));
    fixture.recorder.result(
        fixture.recorder.task_id,
        QByteArray::fromStdString(movedCatalog(RootDigest).dump()));
    QApplication::processEvents();

    auto *reload = fixture.widget.findChild<QPushButton *>(
        QStringLiteral("pelican.framePlan.reloadAuthoring"));
    REQUIRE(reload != nullptr);
    CHECK_FALSE(reload->isVisible());

    saveMaterialPass(fixture.widget);
    REQUIRE(fixture.recorder.submits == 1);
    CHECK(sentParams(fixture.recorder).at("base_source_digest") == RootDigest);

    // Refused: the context is read again, and nothing is sent until it is here.
    const auto context_before = fixture.recorder.context_id;
    const auto task_before = fixture.recorder.task_id;
    fixture.recorder.result(fixture.recorder.submit_id, movedRefusal());
    QApplication::processEvents();
    REQUIRE(fixture.recorder.context_id != context_before);
    CHECK(fixture.recorder.task_id != task_before);
    CHECK(fixture.recorder.submits == 1);
    fixture.recorder.result(
        fixture.recorder.context_id,
        QByteArray::fromStdString(movedContext(fixture.graph, MovedDigest).dump()));
    QApplication::processEvents();
    CHECK(fixture.recorder.submits == 1);
    fixture.recorder.result(
        fixture.recorder.task_id,
        QByteArray::fromStdString(movedCatalog(MovedDigest).dump()));
    QApplication::processEvents();

    // Built again from the new context, not the refused bytes sent again.
    REQUIRE(fixture.recorder.submits == 2);
    CHECK(fixture.recorder.method == QStringLiteral("edit_render_targets"));
    CHECK(sentParams(fixture.recorder).at("base_source_digest") == MovedDigest);
    CHECK_FALSE(reload->isVisible());

    // Refused again, after the engine took it: once is all it is rebuilt.
    const auto context_second = fixture.recorder.context_id;
    fixture.recorder.result(
        fixture.recorder.submit_id,
        QByteArray::fromStdString(
            Json{{"status", "accepted"}, {"ticket", "q13-targets"}}.dump()));
    QApplication::processEvents();
    fixture.recorder.result(fixture.recorder.ticket_id, movedRefusal());
    QApplication::processEvents();
    CHECK(fixture.recorder.submits == 2);
    CHECK(fixture.recorder.context_id == context_second);
    CHECK(statusText(fixture.widget).contains(
        QStringLiteral("changed outside this panel")));
    CHECK(reload->isVisible());

    // Reload reads the context again.
    QTest::mouseClick(reload, Qt::LeftButton);
    QApplication::processEvents();
    CHECK(fixture.recorder.context_id != context_second);
    CHECK_FALSE(reload->isVisible());
    CHECK(fixture.recorder.submits == 2);
}

// --- Q-14. A resource dragged onto a pass whose shader does not read it: the
// refusal says what to do, and Use engine://passthrough (VI-3b) sends the
// inputs and that fragment in one edit_preset_pass.

TEST_CASE("Q-14 the inputs and the fragment of a pass travel in one request",
          "[devstudio][frame-plan][q14][canvas-edit]") {
    const auto authoring =
        framePlanAuthoringFromContext(mixedAuthoringContext("g"));
    const auto request = framePlanPassInputsAndShaderRequest(
        authoring, "g", "wp343_mixed_a", {"wp343_mixed_d_output"},
        "project://shaders/wp343_mixed_a_pass_through");
    REQUIRE(request.sendable());
    CHECK(request.method == "edit_preset_pass");
    const auto declaration = mixedPassDeclaration(0);
    CHECK(request.params.at("input") == Json::array({"wp343_mixed_d_output"}));
    CHECK(request.params.at("shader").at("fragment") ==
          "project://shaders/wp343_mixed_a_pass_through");
    CHECK(request.params.at("shader").at("vertex") ==
          declaration.at("shader").at("vertex"));
    CHECK(request.params.at("output") == declaration.at("output"));
    CHECK(request.params.at("base_source_digest") == RootDigest);
    CHECK(request.params.at("base_preset_digest") == PresetDigest);
    // A pass the canvas cannot edit is refused by name, as every gesture is.
    const auto refused = framePlanPassInputsAndShaderRequest(
        authoring, "g", "wp343_mixed_d", {"x"}, "project://shaders/x");
    CHECK_FALSE(refused.sendable());
    CHECK(refused.refusal.find("wp343_mixed_d") != std::string::npos);
}

TEST_CASE("Q-14 a drag its shader cannot read is refused with a line that says "
          "what to do, and Use engine://passthrough sends inputs and shader "
          "together",
          "[devstudio][frame-plan][q14][canvas-edit]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = std::filesystem::path{temporary.path().toStdWString()};
    {
        std::ofstream project{root / "project.json", std::ios::binary};
        project << Json{{"schema", "pelican.project"},
                        {"version", 1},
                        {"name", "q14-contract"}}
                       .dump();
    }
    EditingFixture fixture;
    fixture.widget.setProjectRoot(root);
    fixture.open();
    auto &logical = fixture.logical();
    auto *pass_through = fixture.widget.findChild<QPushButton *>(
        QStringLiteral("pelican.framePlan.shaderContract.usePassthrough"));
    auto *open_shader = fixture.widget.findChild<QPushButton *>(
        QStringLiteral("pelican.framePlan.shaderContract.openShader"));
    REQUIRE(pass_through != nullptr);
    REQUIRE(open_shader != nullptr);
    CHECK(pass_through->isHidden());
    CHECK(open_shader->isHidden());

    fixture.dragSocket(QStringLiteral("wp343_mixed_d"),
                       QStringLiteral("wp343_mixed_d_output"),
                       QStringLiteral("output"),
                       nodeItem(logical, "wp343_mixed_a"));
    REQUIRE(fixture.recorder.submits == 1);
    const auto refusal = [](std::string message) {
        return QByteArray::fromStdString(
            Json{{"status", "rejected"},
                 {"committed", false},
                 {"error",
                  {{"code", "render_pipeline_preflight_failed"},
                   {"message", std::move(message)}}}}
                .dump());
    };

    SECTION("the contract refusal: the line, and the two buttons") {
        fixture.recorder.result(
            fixture.recorder.submit_id,
            refusal("Fullscreen pass input texture/local-read contract does "
                    "not match shader reflection"));
        QApplication::processEvents();
        CHECK(statusText(fixture.widget) ==
              QStringLiteral("wp343_mixed_a's shader (fullscreen) does not read "
                             "the inputs the pass lists. To change its inputs, "
                             "change the shader too."));
        CHECK_FALSE(pass_through->isHidden());
        CHECK_FALSE(open_shader->isHidden());
        // An engine shader has no project file to open.
        CHECK_FALSE(open_shader->isEnabled());

        QTest::mouseClick(pass_through, Qt::LeftButton);
        QApplication::processEvents();
        REQUIRE(fixture.recorder.submits == 2);
        CHECK(fixture.recorder.method == QStringLiteral("edit_preset_pass"));
        const auto sent = sentParams(fixture.recorder);
        CHECK(sent.at("pass_name") == "wp343_mixed_a");
        CHECK(sent.at("input") == Json::array({"wp343_mixed_d_output"}));
        CHECK(sent.at("shader").at("fragment") == "engine://passthrough");
        CHECK(sent.at("shader").at("vertex") == "engine://fullscreen");
        CHECK(sent.at("output") == mixedPassDeclaration(0).at("output"));
        CHECK(sent.at("base_source_digest") == RootDigest);
        // No file is written.
        CHECK_FALSE(std::filesystem::exists(root / "shaders"));
        CHECK(pass_through->isHidden());
        CHECK(open_shader->isHidden());
    }

    SECTION("control: any other refusal keeps its line and offers nothing") {
        fixture.recorder.result(
            fixture.recorder.submit_id,
            refusal("render feature pass insert anchor was not found: x"));
        QApplication::processEvents();
        CHECK(statusText(fixture.widget).contains(
            QStringLiteral("render_pipeline_preflight_failed")));
        CHECK(pass_through->isHidden());
        CHECK(open_shader->isHidden());
        CHECK_FALSE(std::filesystem::exists(root / "shaders"));
    }
}


// --- VI-3b: engine://passthrough shows the input whose socket is clicked; a
// shader that declares no input is not given one; Fix sockets and Make
// sockets variadic rewrite the shader.

namespace {

// engine://passthrough as get_frame_plan publishes it: every input variadic.
Json vi3bPassthroughInputs(const std::string &fragment = "engine://passthrough") {
    auto declarations = Json::array();
    for (std::size_t slot = 0; slot < 8; ++slot)
        declarations.push_back(vi3aDeclaration(
            slot, "pelican_variadic_input_" + std::to_string(slot), true));
    return Json{{"state", "declared"},
                {"fragment", fragment},
                {"declarations", std::move(declarations)},
                {"variadic_range", {{"first", 0}, {"max", 8}}},
                {"pass_texture_inputs", 0},
                {"pass_buffer_inputs", 0},
                {"buffer_binding_base", 8}};
}

// `a`'s shader object, in the composed declaration and the preset's own.
void setVi3bShader(Json &context, const Json &shader) {
    for (auto &graph : context["graphs"])
        for (auto &pass : graph["passes"])
            if (pass.at("name") == Vi3aPass) {
                pass["declaration"]["shader"] = shader;
                pass["preset_declaration"]["shader"] = shader;
            }
}

Json vi3bPassthroughShader(Json defines = nullptr) {
    Json shader{{"vertex", "engine://fullscreen"},
                {"fragment", "engine://passthrough"}};
    if (!defines.is_null()) shader["defines"] = std::move(defines);
    return shader;
}

FramePlanAuthoring vi3bAuthoring(const std::string &graph,
                                 const std::vector<std::string> &inputs,
                                 const Json &shader) {
    auto context = mixedAuthoringContext(graph);
    setVi3aInputs(context, inputs);
    setVi3bShader(context, shader);
    return framePlanAuthoringFromContext(context);
}

std::filesystem::path vi3bProjectRoot(const QTemporaryDir &temporary) {
    const auto root = std::filesystem::path{temporary.path().toStdWString()};
    std::ofstream project{root / "project.json", std::ios::binary};
    project << Json{{"schema", "pelican.project"},
                    {"version", 1},
                    {"name", "vi3b-sockets"}}
                   .dump();
    return root;
}

std::string vi3bRead(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input);
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

void vi3bCommit(EditingFixture &fixture) {
    fixture.recorder.result(
        fixture.recorder.submit_id,
        QByteArray::fromStdString(
            Json{{"status", "accepted"}, {"ticket", "t-1"}}.dump()));
    QApplication::processEvents();
    fixture.recorder.result(
        fixture.recorder.ticket_id,
        QByteArray::fromStdString(
            Json{{"status", "committed"}, {"committed", true}}.dump()));
    QApplication::processEvents();
}

} // namespace

TEST_CASE("VI-3b a click on a passthrough socket writes PELICAN_PASSTHROUGH_INPUT "
          "and keeps the other defines",
          "[devstudio][frame-plan][vi3b][passthrough]") {
    const auto model = vi3aModel(vi3bPassthroughInputs());
    const std::string graph = model.graph;
    const std::vector<std::string> inputs{"t_a", "t_b", "t_c"};
    const auto request = [&](const Json &shader, std::size_t binding) {
        return framePlanPassthroughInputRequest(vi3bAuthoring(graph, inputs, shader),
                                                model, graph, Vi3aPass, binding);
    };

    SECTION("input 1: the define is written, the others kept, nothing else moves") {
        const auto made =
            request(vi3bPassthroughShader(Json::array({"TINT=2"})), 1);
        REQUIRE(made.sendable());
        CHECK(made.method == "edit_preset_pass");
        CHECK(made.params.at("shader").at("defines") ==
              Json::array({"TINT=2", "PELICAN_PASSTHROUGH_INPUT=1"}));
        CHECK(made.params.at("shader").at("fragment") == "engine://passthrough");
        CHECK(made.params.at("input") == Json::array({"t_a", "t_b", "t_c"}));
        CHECK(made.params.at("output") == mixedPassDeclaration(0).at("output"));
        CHECK(made.notice == "wp343_mixed_a shows t_b.");
    }
    SECTION("an existing value is replaced, not repeated") {
        const auto made = request(
            vi3bPassthroughShader(Json::array({"PELICAN_PASSTHROUGH_INPUT=2", "TINT=2"})), 1);
        REQUIRE(made.sendable());
        CHECK(made.params.at("shader").at("defines") ==
              Json::array({"TINT=2", "PELICAN_PASSTHROUGH_INPUT=1"}));
    }
    SECTION("input 0: the define goes (0 is its default), the others stay") {
        auto made = request(
            vi3bPassthroughShader(Json::array({"PELICAN_PASSTHROUGH_INPUT=2", "TINT=2"})), 0);
        REQUIRE(made.sendable());
        CHECK(made.params.at("shader").at("defines") == Json::array({"TINT=2"}));
        // With nothing else defined, `defines` goes with it.
        made = request(vi3bPassthroughShader(Json::array({"PELICAN_PASSTHROUGH_INPUT=2"})), 0);
        REQUIRE(made.sendable());
        CHECK_FALSE(made.params.at("shader").contains("defines"));
    }
    SECTION("refusals: the input already shown, a binding nothing feeds, another shader") {
        CHECK(request(vi3bPassthroughShader(), 0).refusal ==
              "wp343_mixed_a already shows t_a.");
        CHECK(request(vi3bPassthroughShader(), 5).refusal ==
              "wp343_mixed_a has no input connected at binding 5.");
        CHECK_FALSE(request(Json{{"vertex", "engine://fullscreen"},
                                 {"fragment", "project://shaders/vi"}},
                            1)
                        .sendable());
    }
    SECTION("the shown member is read from the defines, and marked") {
        CHECK(framePlanPassthroughShown(
                  vi3bAuthoring(graph, inputs, vi3bPassthroughShader()), graph,
                  Vi3aPass) == 0u);
        const auto authoring = vi3bAuthoring(
            graph, inputs,
            vi3bPassthroughShader(Json::array({"PELICAN_PASSTHROUGH_INPUT=2"})));
        const auto shown = framePlanPassthroughShown(authoring, graph, Vi3aPass);
        CHECK(shown == 2u);
        CHECK_FALSE(framePlanPassthroughShown(
            vi3bAuthoring(graph, inputs,
                          Json{{"vertex", "engine://fullscreen"},
                               {"fragment", "project://shaders/vi"}}),
            graph, Vi3aPass));
        const auto sockets = framePlanInputSockets(model, vi3aNode(model), inputs, shown);
        REQUIRE(sockets.sockets.size() == 4);
        CHECK(sockets.sockets[2].shown);
        CHECK(sockets.sockets[2].label == "← t_c (shown)");
        CHECK_FALSE(sockets.sockets[0].shown);
        CHECK(sockets.sockets[0].label == "← t_a");
    }
    SECTION("the Pass Editor's helper drops the define alone") {
        CHECK(framePlanShaderWithoutPassthroughInput(vi3bPassthroughShader(
                  Json::array({"PELICAN_PASSTHROUGH_INPUT", "A=1"})))
                  .at("defines") == Json::array({"A=1"}));
        CHECK_FALSE(framePlanShaderWithoutPassthroughInput(vi3bPassthroughShader(
                        Json::array({"PELICAN_PASSTHROUGH_INPUT=3"})))
                        .contains("defines"));
    }
}

TEST_CASE("VI-3b on the canvas a click on a passthrough socket sends the define "
          "and the shown socket is marked",
          "[devstudio][frame-plan][vi3b][passthrough][canvas-edit]") {
    application();
    EditingFixture fixture;
    setVi3aPassInputs(fixture.wire, vi3bPassthroughInputs());
    fixture.tweak_context = [](Json &context) {
        setVi3aInputs(context, {"t_a", "t_b"});
        setVi3bShader(context, vi3bPassthroughShader());
    };
    fixture.open();
    auto &logical = fixture.logical();
    QGraphicsItem *first = declaredSocket(logical, QString::fromLatin1(Vi3aPass),
                                          QStringLiteral("in@0"));
    QGraphicsItem *second = declaredSocket(logical, QString::fromLatin1(Vi3aPass),
                                           QStringLiteral("in@1"));
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first->data(FramePlanSocketShownRole).toBool());
    CHECK_FALSE(second->data(FramePlanSocketShownRole).toBool());

    auto &view = logicalView(fixture.widget);
    view.fitInView(nodeItem(logical, Vi3aPass)->sceneBoundingRect().adjusted(-40, -40, 40, 40),
                   Qt::KeepAspectRatio);
    QApplication::processEvents();
    const QPoint at = view.mapFromScene(second->sceneBoundingRect().center());
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, at);
    QApplication::processEvents();
    REQUIRE(fixture.recorder.submits == 1);
    CHECK(sentParams(fixture.recorder).at("shader").at("defines") ==
          Json::array({"PELICAN_PASSTHROUGH_INPUT=1"}));
    CHECK(sentParams(fixture.recorder).at("input") == Json::array({"t_a", "t_b"}));
}

TEST_CASE("VI-3b a drop on a pass whose shader declares no input is not sent; "
          "Use engine://passthrough sends the input and that fragment",
          "[devstudio][frame-plan][vi3b][no-inputs][canvas-edit]") {
    application();
    SECTION("the request: a line and the inputs to offer") {
        const auto model = vi3aModel(vi3aFixedInputs({}));
        CHECK(framePlanNodeDeclaresNoInputs(vi3aNode(model)));
        CHECK_FALSE(framePlanNodeDeclaresNoInputs(vi3aNode(vi3aModel())));
        const auto made = framePlanSocketConnectRequest(
            vi3aAuthoring(model.graph, {"t_a"}), model, model.graph, Vi3aPass,
            "t_b", FramePlanSocketDrop{});
        CHECK_FALSE(made.sendable());
        CHECK(made.line == "wp343_mixed_a's shader declares no inputs.");
        REQUIRE(made.passthrough_inputs);
        CHECK(*made.passthrough_inputs == std::vector<std::string>{"t_a", "t_b"});
        CHECK(made.fragment == "project://shaders/vi");
    }
    SECTION("the canvas: nothing sent, the line and the two buttons") {
        QTemporaryDir temporary;
        REQUIRE(temporary.isValid());
        EditingFixture fixture;
        fixture.widget.setProjectRoot(vi3bProjectRoot(temporary));
        setVi3aPassInputs(fixture.wire, vi3aFixedInputs({}));
        fixture.open();
        auto &logical = fixture.logical();
        auto *use = fixture.widget.findChild<QPushButton *>(
            QStringLiteral("pelican.framePlan.shaderContract.usePassthrough"));
        auto *open_shader = fixture.widget.findChild<QPushButton *>(
            QStringLiteral("pelican.framePlan.shaderContract.openShader"));
        REQUIRE(use != nullptr);
        REQUIRE(open_shader != nullptr);
        fixture.dragSocket(QStringLiteral("wp343_mixed_d"),
                           QStringLiteral("wp343_mixed_d_output"),
                           QStringLiteral("output"), nodeItem(logical, Vi3aPass));
        CHECK(fixture.recorder.submits == 0);
        CHECK(statusText(fixture.widget) ==
              QStringLiteral("wp343_mixed_a's shader declares no inputs."));
        CHECK_FALSE(use->isHidden());
        CHECK_FALSE(open_shader->isHidden());
        CHECK(open_shader->isEnabled());
        // No socket is drawn for a shader that declares nothing.
        CHECK(declaredSocket(logical, QString::fromLatin1(Vi3aPass),
                             QStringLiteral("in:variadic")) == nullptr);
        QTest::mouseClick(use, Qt::LeftButton);
        QApplication::processEvents();
        REQUIRE(fixture.recorder.submits == 1);
        const auto sent = sentParams(fixture.recorder);
        CHECK(sent.at("pass_name") == "wp343_mixed_a");
        CHECK(sent.at("input") == Json::array({"wp343_mixed_d_output"}));
        CHECK(sent.at("shader").at("fragment") == "engine://passthrough");
        CHECK(use->isHidden());
    }
}

TEST_CASE("VI-3b Fix sockets: why it cannot, and what it rewrites",
          "[devstudio][frame-plan][vi3b][fix-sockets]") {
    application();
    SECTION("the plan: the group's connected members, or why not") {
        const auto model = vi3aModel();
        const std::string graph = model.graph;
        auto fix = framePlanSocketFix(vi3aAuthoring(graph, {"t_a", "t_b", "t_c"}),
                                      model, graph, Vi3aPass);
        // A buffer after the group would move once the group is fixed.
        CHECK(fix.refusal.find("buffer") != std::string::npos);
        auto no_buffer = vi3aPassInputs();
        no_buffer["declarations"].erase(11);
        const auto plain = vi3aModel(no_buffer);
        fix = framePlanSocketFix(vi3aAuthoring(graph, {"t_a", "t_b", "t_c"}),
                                 plain, graph, Vi3aPass);
        CHECK(fix.refusal == "Connect at least one input before fixing the sockets.");
        fix = framePlanSocketFix(
            vi3aAuthoring(graph, {"t_a", "t_b", "t_c", "t_v@history", "t_w"}),
            plain, graph, Vi3aPass);
        CHECK(fix.refusal.empty());
        CHECK(fix.first == 3);
        CHECK(fix.inputs == std::vector<std::string>{"t_v@history", "t_w"});
        CHECK(fix.taken == std::vector<std::string>{"in_a", "in_b", "in_c"});
        CHECK(fix.fragment == "project://shaders/vi");
        // An engine shader other than engine://passthrough is not copied.
        const auto other = vi3aModel(vi3bPassthroughInputs("engine://fullscreen"));
        CHECK(framePlanSocketFix(vi3aAuthoring(graph, {"t_a"}), other, graph,
                                 Vi3aPass)
                  .refusal.find("cannot open or copy") != std::string::npos);
    }

    SECTION("engine://passthrough: copied fixed, and the pass moved onto the copy") {
        QTemporaryDir temporary;
        REQUIRE(temporary.isValid());
        const auto root = vi3bProjectRoot(temporary);
        EditingFixture fixture;
        fixture.widget.setProjectRoot(root);
        setVi3aPassInputs(fixture.wire, vi3bPassthroughInputs());
        fixture.tweak_context = [](Json &context) {
            setVi3aInputs(context, {"t_a", "t_b"});
            setVi3bShader(context, vi3bPassthroughShader(
                                       Json::array({"PELICAN_PASSTHROUGH_INPUT=1"})));
        };
        fixture.open();
        // A file already under the pass's name is never overwritten.
        std::filesystem::create_directories(root / "shaders");
        std::ofstream{root / "shaders" / "wp343_mixed_a.frag", std::ios::binary}
            << "// mine\n";
        fixture.widget.fixSockets(QString::fromStdString(fixture.graph),
                                  QString::fromLatin1(Vi3aPass));
        QApplication::processEvents();
        REQUIRE(fixture.recorder.submits == 1);
        const auto sent = sentParams(fixture.recorder);
        CHECK(sent.at("shader").at("fragment") ==
              "project://shaders/wp343_mixed_a_2");
        // The pass keeps showing what it showed.
        CHECK(sent.at("shader").at("defines") ==
              Json::array({"PELICAN_PASSTHROUGH_INPUT=1"}));
        CHECK(sent.at("input") == Json::array({"t_a", "t_b"}));
        CHECK(vi3bRead(root / "shaders" / "wp343_mixed_a.frag") == "// mine\n");
        const auto copy = vi3bRead(root / "shaders" / "wp343_mixed_a_2.frag");
        const auto expected = fixShaderSockets(
            *engineFragmentSource(EnginePassthroughFragment), 0, {"t_a", "t_b"});
        REQUIRE(expected.ok());
        CHECK(copy == expected.source);
        CHECK(copy.find("uniform sampler2D in_t_b;") != std::string::npos);
    }

    SECTION("a project shader: rewritten in place, and made variadic again") {
        QTemporaryDir temporary;
        REQUIRE(temporary.isValid());
        const auto root = vi3bProjectRoot(temporary);
        std::filesystem::create_directories(root / "shaders");
        const std::string original =
            "#version 460\n"
            "PELICAN_DECLARE_VARIADIC_INPUTS(0, 8);\n"
            "void main() {}\n";
        std::ofstream{root / "shaders" / "vi.frag", std::ios::binary} << original;
        EditingFixture fixture;
        fixture.widget.setProjectRoot(root);
        setVi3aPassInputs(fixture.wire, vi3bPassthroughInputs("project://shaders/vi"));
        fixture.tweak_context = [](Json &context) {
            setVi3aInputs(context, {"t_a", "t_b"});
        };
        fixture.open();
        auto *fix_action = fixture.widget.findChild<QAction *>(
            QStringLiteral("pelican.framePlan.fixSockets"));
        auto *variadic_action = fixture.widget.findChild<QAction *>(
            QStringLiteral("pelican.framePlan.makeSocketsVariadic"));
        REQUIRE(fix_action != nullptr);
        REQUIRE(variadic_action != nullptr);
        auto &logical = fixture.logical();
        // The Fix button stands beside the band's end.
        bool button = false;
        for (QGraphicsItem *item : itemsOfKind(logical, FramePlanSocketFixItem))
            button = button || item->data(FramePlanNameRole).toString() ==
                                   QLatin1String{Vi3aPass};
        CHECK(button);
        // Picked by its heading, as a person does before the menu entry.
        logical.clearSelection();
        nodeItem(logical, Vi3aPass)->setSelected(true);
        QApplication::processEvents();
        fix_action->trigger();
        QApplication::processEvents();
        CHECK(fixture.recorder.submits == 0);
        const auto fixed = vi3bRead(root / "shaders" / "vi.frag");
        CHECK(fixed == fixShaderSockets(original, 0, {"t_a", "t_b"}).source);
        CHECK(statusText(fixture.widget).contains(QStringLiteral("Fixed")));

        variadic_action->trigger();
        QApplication::processEvents();
        CHECK(vi3bRead(root / "shaders" / "vi.frag") == original);
        // A second time: nothing Fix sockets wrote is left, and it says so.
        variadic_action->trigger();
        QApplication::processEvents();
        CHECK(statusText(fixture.widget) ==
              QStringLiteral("wp343_mixed_a's shader has no sockets Studio fixed."));
        CHECK(vi3bRead(root / "shaders" / "vi.frag") == original);
    }
}

// --- LP-4a: view_select (LP-Va) on the canvas, in the details, and in the
// refusal lines.

namespace {

// The WP343 editing fixture with view_select written the way get_frame_plan
// writes it: `a` runs for cascade 2, the task `b` for face 3 of light 0, `c`
// for cascade 1 but no view of this frame matched it, and `d` for every view
// (the control).
Json lp4aWire() {
    Json wire = mixedEditingWire();
    for (auto &node : wire["nodes"]) {
        const auto name = node.at("name").get<std::string>();
        if (name == "wp343_mixed_a") {
            node["view_family"] = "$shadow/directional";
            node["view_select"] = {{"cascade", 2}};
        } else if (name == "wp343_mixed_b") {
            node["view_family"] = "$shadow/point";
            node["view_select"] = {{"light", 0}, {"face", 3}};
        } else if (name == "wp343_mixed_c") {
            node["view_family"] = "$shadow/directional";
            node["view_select"] = {{"cascade", 1}};
            node["no_selected_view"] = true;
        }
    }
    for (auto &scope : wire["physical_target_plan"]["scopes"])
        for (const auto &member : scope.at("nodes"))
            if (member == "wp343_mixed_a") scope["view_select"] = {{"cascade", 2}};
    return wire;
}

std::map<std::string, QString> lp4aViewLines(QGraphicsScene &canvas) {
    std::map<std::string, QString> lines;
    for (QGraphicsItem *item : itemsOfKind(canvas, FramePlanViewSelectItem))
        lines.emplace(item->data(FramePlanNameRole).toString().toStdString(),
                      static_cast<QGraphicsSimpleTextItem *>(item)->text());
    return lines;
}

QGraphicsItem *lp4aHeader(QGraphicsScene &canvas, const char *node) {
    for (QGraphicsItem *item : itemsOfKind(canvas, FramePlanNodeHeaderItem))
        if (item->data(FramePlanNameRole).toString() == QLatin1String{node})
            return item;
    return nullptr;
}

// What the engine's edit service answers when the render pipeline refused
// the candidate for view_select: the code, its fields beside it, and a
// message that is the code with the fields as JSON (carriedRefusalMessage).
Json lp4aRefusal(const char *code, Json fields) {
    Json error = fields;
    error["code"] = code;
    error["message"] = std::string{code} + ": " + fields.dump();
    error["ticket"] = "lp4a-1";
    error["participant"] = "pelican.render_pipeline";
    return error;
}

} // namespace

TEST_CASE("LP-4a view_select is one line under the heading, and a node that "
          "ran no view this frame is dimmed and says why",
          "[devstudio][frame-plan][lp4a][view-select]") {
    application();
    const Json wire = lp4aWire();
    const auto model = buildFramePlanModel(wire.dump());

    // The model reads both, and the scope's.
    const auto &a = vi3aNode(model, "wp343_mixed_a");
    const auto &b = vi3aNode(model, "wp343_mixed_b");
    const auto &c = vi3aNode(model, "wp343_mixed_c");
    const auto &d = vi3aNode(model, "wp343_mixed_d");
    CHECK(a.view_select ==
          std::vector<FramePlanViewSelectAxis>{{"cascade", 2}});
    CHECK_FALSE(a.no_selected_view);
    CHECK(c.no_selected_view);
    CHECK(d.view_select.empty());
    CHECK_FALSE(d.no_selected_view);
    CHECK(framePlanViewSelectLine(a.view_select) == "view: cascade 2");
    // Several axes, in the order the plan lists them.
    CHECK(framePlanViewSelectLine(b.view_select) == "view: face 3, light 0");
    CHECK(framePlanViewSelectLine(d.view_select).empty());
    const auto scope = std::ranges::find_if(
        model.physical_plan.scopes, [](const FramePlanPhysicalScope &value) {
            return std::ranges::find(value.nodes, "wp343_mixed_a") !=
                   value.nodes.end();
        });
    REQUIRE(scope != model.physical_plan.scopes.end());
    CHECK(scope->view_select ==
          std::vector<FramePlanViewSelectAxis>{{"cascade", 2}});
    CHECK(framePlanNoSelectedViewNotice(c) ==
          "No view of $shadow/directional matches view_select this frame.");
    CHECK(framePlanNoSelectedViewNotice(a).empty());

    SECTION("the canvas draws the line on the nodes that have one") {
        FramePlanGraphicsScene canvas;
        canvas.populateWholeFrame(model);
        const auto lines = lp4aViewLines(canvas);
        CHECK(lines == std::map<std::string, QString>{
                           {"wp343_mixed_a", QStringLiteral("view: cascade 2")},
                           {"wp343_mixed_b",
                            QStringLiteral("view: face 3, light 0")},
                           {"wp343_mixed_c", QStringLiteral("view: cascade 1")},
                       });
        // The control: a node without view_select has no line.
        CHECK_FALSE(lines.contains("wp343_mixed_d"));
        // Under the heading, above the node's sockets.
        QGraphicsItem *line = nullptr;
        for (QGraphicsItem *item : itemsOfKind(canvas, FramePlanViewSelectItem))
            if (item->data(FramePlanNameRole).toString() ==
                QLatin1String{"wp343_mixed_a"})
                line = item;
        REQUIRE(line != nullptr);
        QGraphicsItem *heading = lp4aHeader(canvas, "wp343_mixed_a");
        REQUIRE(heading != nullptr);
        CHECK(line->sceneBoundingRect().top() >=
              heading->sceneBoundingRect().bottom());
        std::size_t sockets = 0;
        for (QGraphicsItem *socket : itemsOfKind(canvas, FramePlanSocketItem)) {
            if (socket->data(FramePlanNameRole).toString() !=
                QLatin1String{"wp343_mixed_a"})
                continue;
            ++sockets;
            CHECK(line->sceneBoundingRect().bottom() <=
                  socket->sceneBoundingRect().center().y());
        }
        CHECK(sockets > 0);

        // The node that ran no view: its heading is dimmed and says why.
        QGraphicsItem *idle = lp4aHeader(canvas, "wp343_mixed_c");
        REQUIRE(idle != nullptr);
        CHECK(idle->opacity() < 1.0);
        CHECK(idle->toolTip() ==
              QStringLiteral("No view of $shadow/directional matches "
                             "view_select this frame."));
        CHECK(nodeItem(canvas, "wp343_mixed_c")
                  ->toolTip()
                  .contains(QStringLiteral("matches view_select this frame")));
        // The control: a node that ran keeps its heading as it was.
        CHECK(heading->opacity() == 1.0);
        CHECK(heading->toolTip().isEmpty());
        CHECK_FALSE(nodeItem(canvas, "wp343_mixed_a")
                        ->toolTip()
                        .contains(QStringLiteral("matches view_select")));
    }

    SECTION("the details list it for the node and for its scope") {
        EmbeddedViewport viewport;
        FramePlanWidget widget{&viewport};
        widget.receiveResult(QByteArray::fromStdString(wire.dump()));
        QApplication::processEvents();
        QStringList selects;
        QStringList frames;
        QStringList scopes;
        for (auto *tree : widget.findChildren<QTreeWidget *>()) {
            for (const auto *item : tree->findItems(
                     QStringLiteral("View select"),
                     Qt::MatchExactly | Qt::MatchRecursive, 1))
                selects.push_back(item->text(2));
            for (const auto *item : tree->findItems(
                     QStringLiteral("This frame"),
                     Qt::MatchExactly | Qt::MatchRecursive, 1))
                frames.push_back(item->text(2));
            for (const auto *item : tree->findItems(
                     QStringLiteral("view: cascade 2"),
                     Qt::MatchContains | Qt::MatchRecursive, 3))
                scopes.push_back(item->text(0));
        }
        selects.sort();
        CHECK(selects == QStringList{QStringLiteral("cascade 1"),
                                     QStringLiteral("cascade 2"),
                                     QStringLiteral("face 3, light 0")});
        CHECK(frames == QStringList{QStringLiteral(
                            "No view of $shadow/directional matches "
                            "view_select this frame.")});
        CHECK(scopes == QStringList{QString::fromStdString(scope->id)});
    }
}

TEST_CASE("LP-4a the four view_select refusals read as one sentence each, "
          "the code last",
          "[devstudio][frame-plan][lp4a][refusal]") {
    application();
    SECTION("the sentences") {
        CHECK(renderAuthoringRefusalLine(lp4aRefusal(
                  "graph.view_select_axis_unknown@1",
                  {{"pass", "cube_depth"},
                   {"family", "$capture/cube"},
                   {"axis", "cascade"}})) ==
              "cube_depth: $capture/cube has no view axis named cascade "
              "(graph.view_select_axis_unknown@1).");
        CHECK(renderAuthoringRefusalLine(lp4aRefusal(
                  "graph.view_select_axis_unknown@1",
                  {{"pass", "sun_depth"},
                   {"family", "$shadow/directional"},
                   {"axis", "cascade"},
                   {"value", 4},
                   {"length", 4}})) ==
              "sun_depth: cascade 4 is outside $shadow/directional's 4 views "
              "(0..3) (graph.view_select_axis_unknown@1).");
        CHECK(renderAuthoringRefusalLine(lp4aRefusal(
                  "graph.view_select_runtime_axis@1",
                  {{"pass", "sun_depth"},
                   {"family", "$shadow/directional"},
                   {"axis", "light"}})) ==
              "sun_depth: light of $shadow/directional is decided each frame "
              "and cannot be selected (graph.view_select_runtime_axis@1).");
        CHECK(renderAuthoringRefusalLine(lp4aRefusal(
                  "graph.view_select_primary_family@1",
                  {{"pass", "tint"}, {"family", "$main"}})) ==
              "tint draws the main view ($main), which has no axes to select "
              "(graph.view_select_primary_family@1).");
        CHECK(renderAuthoringRefusalLine(lp4aRefusal(
                  "graph.view_select_needs_target_plan@1",
                  {{"pass", "tint"}})) ==
              "tint: view_select needs the physical target plan, which this "
              "configuration does not build "
              "(graph.view_select_needs_target_plan@1).");
    }

    SECTION("carried inside a broader refusal, and from a graph variant") {
        const Json wrapped{
            {"code", "render_pipeline_preflight_failed"},
            {"message",
             "render pipeline candidate was rejected: "
             "graph.view_select_runtime_axis@1: "
             "{\"axis\":\"light\",\"family\":\"$shadow/spot\","
             "\"pass\":\"spot_depth\"}"}};
        CHECK(renderAuthoringRefusalLine(wrapped) ==
              "spot_depth: light of $shadow/spot is decided each frame and "
              "cannot be selected (graph.view_select_runtime_axis@1).");
        // The graph canvas reads the same table.
        CHECK(knownRefusalDescription(wrapped.at("code").get<std::string>(),
                                      wrapped.at("message").get<std::string>()) ==
              renderAuthoringRefusalLine(wrapped));
        auto variant = lp4aRefusal("graph.view_select_primary_family@1",
                                   {{"pass", "tint"}, {"family", "$main"}});
        variant["variant"] = "preview";
        CHECK(renderAuthoringRefusalLine(variant) ==
              "The preview graph refused it: tint draws the main view "
              "($main), which has no axes to select "
              "(graph.view_select_primary_family@1).");
    }

    SECTION("the control: any other refusal keeps its code and message") {
        CHECK(renderAuthoringRefusalLine(
                  Json{{"code", "gate_closed"},
                       {"message", "The editor gate is closed"}}) ==
              "gate_closed: The editor gate is closed");
        CHECK(viewSelectRefusalDescription(
                  Json{{"code", "gate_closed"},
                       {"message", "The editor gate is closed"}})
                  .empty());
    }

    SECTION("the Frame Plan's status line says it after a refused drag") {
        EditingFixture fixture;
        fixture.open();
        auto &logical = fixture.logical();
        fixture.dragSocket(QStringLiteral("wp343_mixed_d"),
                           QStringLiteral("wp343_mixed_d_output"),
                           QStringLiteral("output"),
                           nodeItem(logical, "wp343_mixed_a"));
        REQUIRE(fixture.recorder.submits == 1);
        fixture.recorder.result(
            fixture.recorder.submit_id,
            QByteArray::fromStdString(
                Json{{"status", "accepted"}, {"ticket", "lp4a-1"}}.dump()));
        QApplication::processEvents();
        REQUIRE(fixture.recorder.ticket_id != 0);
        fixture.recorder.result(
            fixture.recorder.ticket_id,
            QByteArray::fromStdString(
                Json{{"status", "rejected"},
                     {"committed", false},
                     {"error", lp4aRefusal("graph.view_select_axis_unknown@1",
                                           {{"pass", "wp343_mixed_a"},
                                            {"family", "$capture/cube"},
                                            {"axis", "cascade"}})}}
                    .dump()));
        QApplication::processEvents();
        const QString sentence = QStringLiteral(
            "wp343_mixed_a: $capture/cube has no view axis named cascade "
            "(graph.view_select_axis_unknown@1).");
        auto *status = fixture.widget.findChild<QLabel *>(
            QStringLiteral("pelican.framePlanStatus"));
        REQUIRE(status != nullptr);
        CHECK(status->text() == oneLine(sentence));
        CHECK(status->toolTip().startsWith(sentence));
        CHECK_FALSE(status->text().contains(QStringLiteral("{")));
    }
}

TEST_CASE("UX-5a an engine refusal whose message carries JSON reads as its sentence; "
          "the whole is the tooltip's",
          "[devstudio][frame-plan][ux5a][refusal]") {
    application();
    const std::string reason =
        "Pass input target is not produced as an earlier output: lit in pass: present";
    SECTION("the words of a message") {
        // A JSON object with its own message: that sentence, its code first.
        CHECK(engineRefusalSentence("{\"code\":\"pipeline.order\",\"pass\":\"present\","
                                    "\"message\":\"" + reason + "\"}") ==
              "pipeline.order: " + reason);
        // A code and its fields: the fields, briefly.
        CHECK(engineRefusalSentence("graph.loop_output_empty@1 {\"node\":\"loop\",\"port\":\"color\"}") ==
              "graph.loop_output_empty@1: node loop, port color");
        CHECK(engineRefusalSentence("render pipeline candidate was rejected: x.y: {\"reason\":\"" +
                                    reason + "\"}") ==
              "render pipeline candidate was rejected: x.y: " + reason);
        // No object: as it is (a shader's log has braces that are no JSON).
        CHECK(engineRefusalSentence(reason) == reason);
        CHECK(engineRefusalSentence("0:12: error: '{' : syntax error") ==
              "0:12: error: '{' : syntax error");
        CHECK(engineRefusalLine(Json{{"code", "render_pipeline_preflight_failed"},
                                     {"message", "{\"message\":\"" + reason + "\"}"}}) ==
              "render_pipeline_preflight_failed: " + reason);
        CHECK(engineRefusalLine(Json{{"code", "gate_closed"},
                                     {"message", "The editor gate is closed"}}) ==
              "gate_closed: The editor gate is closed");
    }

    SECTION("the Frame Plan's status line, after a refused drag") {
        EditingFixture fixture;
        fixture.open();
        auto &logical = fixture.logical();
        fixture.dragSocket(QStringLiteral("wp343_mixed_d"),
                           QStringLiteral("wp343_mixed_d_output"),
                           QStringLiteral("output"),
                           nodeItem(logical, "wp343_mixed_a"));
        REQUIRE(fixture.recorder.submits == 1);
        fixture.recorder.result(
            fixture.recorder.submit_id,
            QByteArray::fromStdString(
                Json{{"status", "accepted"}, {"ticket", "ux5a-1"}}.dump()));
        QApplication::processEvents();
        REQUIRE(fixture.recorder.ticket_id != 0);
        const std::string message = "{\"pass\":\"present\",\"message\":\"" + reason + "\"}";
        fixture.recorder.result(
            fixture.recorder.ticket_id,
            QByteArray::fromStdString(
                Json{{"status", "rejected"},
                     {"committed", false},
                     {"error", {{"code", "render_pipeline_preflight_failed"},
                                {"message", message}}}}
                    .dump()));
        QApplication::processEvents();
        auto *status = fixture.widget.findChild<QLabel *>(
            QStringLiteral("pelican.framePlanStatus"));
        REQUIRE(status != nullptr);
        CHECK(status->text() ==
              oneLine(QString::fromStdString("render_pipeline_preflight_failed: " + reason)));
        CHECK_FALSE(status->text().contains(QStringLiteral("{")));
        CHECK(status->toolTip().contains(QString::fromStdString(message)));
    }
}

TEST_CASE("LP-4a an edit on the canvas keeps a pass's and a task's view_select",
          "[devstudio][frame-plan][lp4a][view-select][canvas-edit]") {
    application();
    EditingFixture fixture;
    // `a` and the task both run for cascade 1 of the directional shadow.
    fixture.tweak_context = [](Json &context) {
        for (auto &graph : context["graphs"])
            for (auto &pass : graph["passes"])
                if (pass.at("name") == "wp343_mixed_a")
                    for (const char *key : {"declaration", "preset_declaration"}) {
                        pass[key]["view_family"] = "$shadow/directional";
                        pass[key]["view_select"] = {{"cascade", 1}};
                    }
    };
    fixture.catalog["tasks"][0]["declaration"]["view_family"] =
        "$shadow/directional";
    fixture.catalog["tasks"][0]["declaration"]["view_select"] = {
        {"cascade", 1}};
    fixture.open();
    auto &logical = fixture.logical();

    SECTION("a pass: edit_preset_pass keeps every member it is not given, so "
            "view_select stays by not being sent") {
        fixture.dragSocket(QStringLiteral("wp343_mixed_d"),
                           QStringLiteral("wp343_mixed_d_output"),
                           QStringLiteral("output"),
                           nodeItem(logical, "wp343_mixed_a"));
        REQUIRE(fixture.recorder.submits == 1);
        REQUIRE(fixture.recorder.method == QStringLiteral("edit_preset_pass"));
        const auto sent = Json::parse(
            QJsonDocument(fixture.recorder.params).toJson().toStdString());
        CHECK(sent.at("input") == Json::array({"wp343_mixed_d_output"}));
        CHECK_FALSE(sent.contains("view_select"));
        CHECK_FALSE(sent.contains("view_family"));
        // Only members the engine's fullscreen edit takes: anything else
        // would be refused, and a null would erase the member.
        const std::set<std::string> accepted{
            "base_source_digest", "base_preset_digest", "base_feature_digest",
            "graph", "pass_name", "input", "output", "shader", "ray_query",
            "uses_light_data", "push_constants", "clear_colors"};
        for (const auto &[key, value] : sent.items()) {
            INFO(key);
            CHECK(accepted.contains(key));
        }
    }

    SECTION("a task: the whole declaration goes back, view_select with it") {
        fixture.dragSocket(QStringLiteral("wp343_mixed_a"),
                           QStringLiteral("wp343_mixed_a_output"),
                           QStringLiteral("output"),
                           nodeItem(logical, "wp343_mixed_b"));
        REQUIRE(fixture.recorder.submits == 1);
        REQUIRE(fixture.recorder.method == QStringLiteral("edit_compute_task"));
        const auto sent = Json::parse(
            QJsonDocument(fixture.recorder.params).toJson().toStdString());
        CHECK(sent.at("task").at("reads") ==
              Json::array({"wp343_mixed_a_output"}));
        CHECK(sent.at("task").at("view_select") == Json{{"cascade", 1}});
        CHECK(sent.at("task").at("view_family") == "$shadow/directional");
    }
}

// --- LP-4b: the View field and the canvas's View menu, from the engine's
// view_axes (LP-Va2).

namespace {

const Json Lp4bCubeAxes = Json::array(
    {{{"axis", "face"},
      {"length", 6},
      {"runtime", false},
      {"labels", Json::array({"+X", "-X", "+Y", "-Y", "+Z", "-Z"})}}});
const Json Lp4bDirectionalAxes = Json::array(
    {{{"axis", "light"}, {"length", nullptr}, {"runtime", true}},
     {{"axis", "cascade"}, {"length", 3}, {"runtime", false}}});

// The WP343 editing fixture with view_axes the way get_frame_plan writes them:
// `a` draws a cube capture and runs for face 2, the task `b` runs for every
// view of a directional shadow (light decided each frame, 3 cascades), `c`
// draws the main view ([]), and `d` predates view_axes (no member).
Json lp4bWire() {
    Json wire = mixedEditingWire();
    for (auto &node : wire["nodes"]) {
        const auto name = node.at("name").get<std::string>();
        if (name == "wp343_mixed_a") {
            node["view_family"] = "$capture/cube";
            node["view_select"] = {{"face", 2}};
            node["view_axes"] = Lp4bCubeAxes;
        } else if (name == "wp343_mixed_b") {
            node["view_family"] = "$shadow/directional";
            node["view_axes"] = Lp4bDirectionalAxes;
        } else if (name == "wp343_mixed_c") {
            node["view_family"] = "$main";
            node["view_axes"] = Json::array();
        }
    }
    return wire;
}

// A menu's entries as "text", "[x] text" for a checked radio, "(off) text"
// for a disabled one; sections and separators left out.
QStringList lp4bMenuEntries(const QMenu &menu) {
    QStringList entries;
    for (QAction *action : menu.actions()) {
        if (action->isSeparator()) continue;
        QString entry = action->text();
        if (!action->isEnabled()) entry = QStringLiteral("(off) ") + entry;
        else if (action->isChecked()) entry = QStringLiteral("[x] ") + entry;
        entries.push_back(entry);
    }
    return entries;
}

QAction *lp4bMenuAction(const QMenu &menu, const QString &text) {
    for (QAction *action : menu.actions())
        if (!action->isSeparator() && action->text() == text) return action;
    return nullptr;
}

} // namespace

TEST_CASE("LP-4b the View field's rows come from the engine's view_axes",
          "[devstudio][frame-plan][lp4b][view-select]") {
    const auto model = buildFramePlanModel(lp4bWire().dump());
    const auto &a = vi3aNode(model, "wp343_mixed_a");
    const auto &b = vi3aNode(model, "wp343_mixed_b");
    const auto &c = vi3aNode(model, "wp343_mixed_c");
    const auto &d = vi3aNode(model, "wp343_mixed_d");

    // The model reads them.
    REQUIRE(a.view_axes.size() == 1);
    CHECK(a.view_axes[0] ==
          FramePlanViewAxis{"face", 6, false, {"+X", "-X", "+Y", "-Y", "+Z", "-Z"}});
    REQUIRE(b.view_axes.size() == 2);
    CHECK(b.view_axes[0] == FramePlanViewAxis{"light", std::nullopt, true, {}});
    CHECK(b.view_axes[1] == FramePlanViewAxis{"cascade", 3, false, {}});
    CHECK(c.view_axes.empty());
    CHECK(d.view_axes.empty());

    using Kind = FramePlanViewFieldRow::Kind;
    SECTION("a cube capture: one combo, named by the engine's labels") {
        const auto rows =
            framePlanViewFieldRows(a.view_axes, a.view_select, a.view_family);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].kind == Kind::choice);
        CHECK(rows[0].axis == "face");
        CHECK(rows[0].values ==
              std::vector<std::string>{"0 (+X)", "1 (-X)", "2 (+Y)", "3 (-Y)",
                                       "4 (+Z)", "5 (-Z)"});
        CHECK(rows[0].selected == 2u);
        CHECK(rows[0].text.empty());
        // The canvas line and the details name the value too.
        CHECK(framePlanViewSelectLine(a.view_select, a.view_axes) ==
              "view: face 2 (+Y)");
    }

    SECTION("a directional shadow: light is not selectable, cascade is") {
        const auto rows =
            framePlanViewFieldRows(b.view_axes, b.view_select, b.view_family);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].kind == Kind::runtime);
        CHECK(rows[0].axis == "light");
        CHECK(rows[0].text == "light: every view, decided each frame");
        CHECK(rows[0].values.empty());
        CHECK(rows[1].kind == Kind::choice);
        CHECK(rows[1].values == std::vector<std::string>{"0", "1", "2"});
        CHECK_FALSE(rows[1].selected.has_value());
    }

    SECTION("no axes: no field") {
        CHECK(framePlanViewFieldRows(c.view_axes, c.view_select, c.view_family)
                  .empty());
        CHECK(framePlanViewFieldRows(d.view_axes, d.view_select, d.view_family)
                  .empty());
        // Even when view_select names an axis: $main has none to show.
        CHECK(framePlanViewFieldRows({}, {{"cascade", 1}}, "$main").empty());
    }

    SECTION("an axis view_select names but the family lacks: a warning row") {
        const auto rows = framePlanViewFieldRows(
            a.view_axes, {{"cascade", 1}, {"face", 4}}, a.view_family);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].axis == "face");
        CHECK(rows[0].selected == 4u);
        CHECK(rows[1].kind == Kind::unknown);
        CHECK(rows[1].axis == "cascade");
        CHECK(rows[1].selected == 1u);
        CHECK(rows[1].text ==
              "$capture/cube has no view axis named cascade: the engine will "
              "refuse it. Remove it.");
        // A value past the axis is kept, and warned about.
        const auto past =
            framePlanViewFieldRows(b.view_axes, {{"cascade", 5}}, b.view_family);
        REQUIRE(past.size() == 2);
        CHECK(past[1].selected == 5u);
        CHECK(past[1].text ==
              "cascade 5 is outside its 3 views (0..2): the engine will refuse "
              "it.");
    }

    SECTION("what an edit carries") {
        CHECK(framePlanViewSelectJson({}).is_null());
        CHECK(framePlanViewSelectJson({{"cascade", 1}}) == Json{{"cascade", 1}});
        CHECK(framePlanViewSelectWith({{"light", 0}}, "cascade", 1) ==
              std::vector<FramePlanViewSelectAxis>{{"light", 0}, {"cascade", 1}});
        CHECK(framePlanViewSelectWith({{"cascade", 2}}, "cascade", std::nullopt)
                  .empty());
        CHECK(framePlanDeclaredViewSelect(
                  Json{{"view_select", {{"cascade", 1}}}}) ==
              std::vector<FramePlanViewSelectAxis>{{"cascade", 1}});
        CHECK(framePlanDeclaredViewSelect(Json::object()).empty());
    }
}

TEST_CASE("LP-4b the canvas: labels on the view line, and a View menu that "
          "sends one edit",
          "[devstudio][frame-plan][lp4b][view-select][canvas-edit]") {
    application();
    EditingFixture fixture;
    fixture.wire = lp4bWire();
    // The declarations say what the plan says: `a` runs for face 2.
    fixture.tweak_context = [](Json &context) {
        for (auto &graph : context["graphs"])
            for (auto &pass : graph["passes"])
                if (pass.at("name") == "wp343_mixed_a")
                    for (const char *key : {"declaration", "preset_declaration"}) {
                        pass[key]["view_family"] = "$capture/cube";
                        pass[key]["view_select"] = {{"face", 2}};
                    }
    };
    fixture.catalog["tasks"][0]["declaration"]["view_family"] =
        "$shadow/directional";
    fixture.open();
    auto &logical = fixture.logical();

    SECTION("the line and the details name the face") {
        CHECK(lp4aViewLines(logical) ==
              std::map<std::string, QString>{
                  {"wp343_mixed_a", QStringLiteral("view: face 2 (+Y)")}});
        QStringList selects;
        for (auto *tree : fixture.widget.findChildren<QTreeWidget *>())
            for (const auto *item : tree->findItems(
                     QStringLiteral("View select"),
                     Qt::MatchExactly | Qt::MatchRecursive, 1))
                selects.push_back(item->text(2));
        CHECK(selects == QStringList{QStringLiteral("face 2 (+Y)")});
        // The Task Editor's View field asks the panel for its task's axes.
        const auto task_axes =
            fixture.widget.taskViewAxes(QStringLiteral("authored_consumer"));
        REQUIRE(task_axes.size() == 2);
        CHECK(task_axes[0].runtime);
        CHECK(task_axes[1] == FramePlanViewAxis{"cascade", 3, false, {}});
        CHECK(fixture.widget.taskViewAxes(QStringLiteral("no_such_task")).empty());
    }

    SECTION("View is offered on a node with axes, and nowhere else") {
        CHECK(contextMenuTexts(logical, *nodeItem(logical, "wp343_mixed_a"))
                  .contains(QStringLiteral("View")));
        CHECK(contextMenuTexts(logical, *nodeItem(logical, "wp343_mixed_b"))
                  .contains(QStringLiteral("View")));
        // The control: [] and a node without view_axes get no View.
        CHECK_FALSE(contextMenuTexts(logical, *nodeItem(logical, "wp343_mixed_c"))
                        .contains(QStringLiteral("View")));
        CHECK_FALSE(contextMenuTexts(logical, *nodeItem(logical, "wp343_mixed_d"))
                        .contains(QStringLiteral("View")));
        QMenu none;
        CHECK_FALSE(logical.fillViewMenu(
            none, FramePlanNodeKey{fixture.graph, "wp343_mixed_c"}));
        CHECK(none.actions().isEmpty());
    }

    SECTION("the entries: All views and each value, the current one checked") {
        QMenu cube;
        REQUIRE(logical.fillViewMenu(
            cube, FramePlanNodeKey{fixture.graph, "wp343_mixed_a"}));
        CHECK(lp4bMenuEntries(cube) ==
              QStringList{QStringLiteral("All views"), QStringLiteral("0 (+X)"),
                          QStringLiteral("1 (-X)"), QStringLiteral("[x] 2 (+Y)"),
                          QStringLiteral("3 (-Y)"), QStringLiteral("4 (+Z)"),
                          QStringLiteral("5 (-Z)")});
        QMenu directional;
        REQUIRE(logical.fillViewMenu(
            directional, FramePlanNodeKey{fixture.graph, "wp343_mixed_b"}));
        CHECK(lp4bMenuEntries(directional) ==
              QStringList{QStringLiteral("(off) light: every view, decided each "
                                         "frame"),
                          QStringLiteral("[x] All views"), QStringLiteral("0"),
                          QStringLiteral("1"), QStringLiteral("2")});
    }

    const auto pick = [&](const char *node, const char *entry) {
        QMenu menu;
        REQUIRE(logical.fillViewMenu(menu,
                                     FramePlanNodeKey{fixture.graph, node}));
        QAction *action = lp4bMenuAction(menu, QString::fromLatin1(entry));
        REQUIRE(action != nullptr);
        action->trigger();
        QApplication::processEvents();
    };
    const auto sent = [&] {
        return Json::parse(
            QJsonDocument(fixture.recorder.params).toJson().toStdString());
    };

    SECTION("a pass: a face goes out as edit_preset_pass with view_select") {
        pick("wp343_mixed_a", "1 (-X)");
        REQUIRE(fixture.recorder.submits == 1);
        REQUIRE(fixture.recorder.method == QStringLiteral("edit_preset_pass"));
        const auto params = sent();
        INFO(params.dump());
        CHECK(params.at("pass_name") == "wp343_mixed_a");
        CHECK(params.at("view_select") == Json{{"face", 1}});
        // A fullscreen edit carries what the engine requires with it, as
        // the declaration holds it.
        const auto declaration = mixedPassDeclaration(0);
        CHECK(params.at("output") == declaration.at("output"));
        CHECK(params.at("shader") == declaration.at("shader"));
        CHECK(params.at("input") == Json::array());
        const std::set<std::string> accepted{
            "base_source_digest", "base_preset_digest", "base_feature_digest",
            "graph", "pass_name", "input", "output", "shader", "ray_query",
            "uses_light_data", "push_constants", "clear_colors", "view_select"};
        for (const auto &[key, value] : params.items()) {
            INFO(key);
            CHECK(accepted.contains(key));
        }
    }

    SECTION("a pass: All views sends view_select null") {
        pick("wp343_mixed_a", "All views");
        REQUIRE(fixture.recorder.submits == 1);
        const auto params = sent();
        INFO(params.dump());
        REQUIRE(params.contains("view_select"));
        CHECK(params.at("view_select").is_null());
    }

    SECTION("the value it already has is not sent") {
        pick("wp343_mixed_a", "2 (+Y)");
        CHECK(fixture.recorder.submits == 0);
        CHECK(statusText(fixture.widget) ==
              QStringLiteral("wp343_mixed_a already runs for face 2."));
    }

    SECTION("a task: the whole declaration goes back with view_select") {
        pick("wp343_mixed_b", "1");
        REQUIRE(fixture.recorder.submits == 1);
        REQUIRE(fixture.recorder.method == QStringLiteral("edit_compute_task"));
        const auto task = sent().at("task");
        INFO(task.dump());
        CHECK(task.at("view_select") == Json{{"cascade", 1}});
        CHECK(task.at("view_family") == "$shadow/directional");
        CHECK(task.at("dispatch") ==
              fixture.catalog["tasks"][0]["declaration"].at("dispatch"));
        CHECK(task.at("name") == "authored_consumer");
    }
}

TEST_CASE("LP-4b a task's View menu back to All views drops view_select from "
          "the declaration",
          "[devstudio][frame-plan][lp4b][view-select][canvas-edit]") {
    application();
    EditingFixture fixture;
    fixture.wire = lp4bWire();
    for (auto &node : fixture.wire["nodes"])
        if (node.at("name") == "wp343_mixed_b")
            node["view_select"] = {{"cascade", 1}};
    fixture.catalog["tasks"][0]["declaration"]["view_family"] =
        "$shadow/directional";
    fixture.catalog["tasks"][0]["declaration"]["view_select"] = {{"cascade", 1}};
    fixture.open();
    auto &logical = fixture.logical();
    QMenu menu;
    REQUIRE(logical.fillViewMenu(
        menu, FramePlanNodeKey{fixture.graph, "wp343_mixed_b"}));
    CHECK(lp4bMenuEntries(menu).contains(QStringLiteral("[x] 1")));
    QAction *all = lp4bMenuAction(menu, QStringLiteral("All views"));
    REQUIRE(all != nullptr);
    all->trigger();
    QApplication::processEvents();
    REQUIRE(fixture.recorder.submits == 1);
    REQUIRE(fixture.recorder.method == QStringLiteral("edit_compute_task"));
    const auto task =
        Json::parse(QJsonDocument(fixture.recorder.params).toJson().toStdString())
            .at("task");
    INFO(task.dump());
    CHECK_FALSE(task.contains("view_select"));
    CHECK(task.at("view_family") == "$shadow/directional");
}
TEST_CASE("LP-4c a loop's copies are one folded group named with the loop's count, "
          "and their details name the body node",
          "[devstudio][frame-plan][grouping][lp4c]") {
    (void)application();
    // A fake get_frame_plan: the bloom pair of the real wire, told it is the
    // two elements of one loop.
    Json wire = regionGroupingBaselineFramePlan();
    const StringSet members{"HorizontalBlur_0", "VerticalBlur_0"};
    std::int64_t element = 0;
    for (auto &node : wire.at("nodes"))
        if (members.contains(node.at("name").get<std::string>()))
            node["loop_origin"] = {{"loop", "bloom_loop"}, {"element", element++},
                                   {"node", "blur"}};
    REQUIRE(element == 2);
    wire["loops"] = Json::array({{{"node", "bloom_loop"}, {"tier", "expand"}, {"count", 2},
                                  {"elements", Json::array({
                                      {{"element", 0}, {"index", 0}, {"branch", 0}},
                                      {{"element", 1}, {"index", 1}, {"branch", 0}}})},
                                  {"unmatched_branches", Json::array()}}});
    const FramePlanModel model = buildFramePlanModel(wire.dump());
    REQUIRE(model.loops.size() == 1);
    CHECK(model.loops[0] == FramePlanLoop{"bloom_loop", "expand", 2,
                                          {{0, 0, 0}, {1, 1, 0}}, {}});
    const auto vertical = std::ranges::find(model.nodes, "VerticalBlur_0", &FramePlanNode::name);
    REQUIRE(vertical != model.nodes.end());
    REQUIRE(vertical->loop_origin);
    CHECK(*vertical->loop_origin == FramePlanLoopOrigin{"bloom_loop", 1, "blur"});
    // The details line.
    CHECK(framePlanLoopOriginLine(*vertical->loop_origin) ==
          "loop bloom_loop · element 1 · body node blur");

    FramePlanGraphicsScene scene;
    scene.populate(model, FramePlanNodeKey{model.graph, "swapchain"}, 64);
    // Folded on first sight: one group box in place of the copies.
    QGraphicsItem *group = singleGroupItem(scene);
    CHECK(strings(group->data(FramePlanMembersRole)) == members);
    CHECK(visibleSceneTextContains(scene, QStringLiteral("loop bloom_loop ×2")));
    for (const auto &member : members) CHECK(nodeItem(scene, member) == nullptr);
    // And it opens like any group.
    const QString group_id = group->data(FramePlanGroupIdRole).toString();
    REQUIRE(scene.expandGroup(group_id));
    CHECK(itemsOfKind(scene, FramePlanGroupItem).empty());
    for (const auto &member : members) CHECK(nodeItem(scene, member) != nullptr);
    // Expanded once, it stays expanded when the plan is read again.
    scene.populate(model, FramePlanNodeKey{model.graph, "swapchain"}, 64);
    CHECK(itemsOfKind(scene, FramePlanGroupItem).empty());

    // The plan says where each copy came from, so the group does not wait
    // for the lowering graph the region and feature groups need.
    Json unplanned = wire;
    unplanned.erase("physical_target_plan");
    const FramePlanModel without_lowering = buildFramePlanModel(unplanned.dump());
    REQUIRE_FALSE(without_lowering.physical_plan.loweringGraphAvailable());
    FramePlanGraphicsScene bare;
    bare.populate(without_lowering, FramePlanNodeKey{without_lowering.graph, "swapchain"}, 64);
    CHECK(strings(singleGroupItem(bare)->data(FramePlanMembersRole)) == members);
}

TEST_CASE("LP-4d2 Show in Frame Plan opens a loop's group with its copies picked",
          "[devstudio][frame-plan][grouping][lp4d2]") {
    (void)application();
    // The same fake plan as LP-4c's: the bloom pair told it is the two
    // elements of one loop.
    Json wire = regionGroupingBaselineFramePlan();
    const StringSet members{"HorizontalBlur_0", "VerticalBlur_0"};
    std::int64_t element = 0;
    for (auto &node : wire.at("nodes"))
        if (members.contains(node.at("name").get<std::string>()))
            node["loop_origin"] = {{"loop", "bloom_loop"}, {"element", element++},
                                   {"node", "blur"}};
    REQUIRE(element == 2);
    wire["loops"] = Json::array({{{"node", "bloom_loop"}, {"tier", "expand"}, {"count", 2},
                                  {"elements", Json::array({
                                      {{"element", 0}, {"index", 0}, {"branch", 0}},
                                      {{"element", 1}, {"index", 1}, {"branch", 0}}})},
                                  {"unmatched_branches", Json::array()}}});
    const FramePlanModel model = buildFramePlanModel(wire.dump());

    FramePlanGraphicsScene scene;
    scene.populate(model, FramePlanNodeKey{model.graph, "swapchain"}, 64);
    // Folded on first sight, as LP-4c draws it.
    REQUIRE(singleGroupItem(scene) != nullptr);
    for (const auto &member : members) REQUIRE(nodeItem(scene, member) == nullptr);

    // The group opens and both copies are picked, drawn as picked.
    REQUIRE(scene.selectLoopGroup("bloom_loop"));
    CHECK(itemsOfKind(scene, FramePlanGroupItem).empty());
    std::set<std::string> picked;
    for (const auto &node : scene.selectedNodes()) picked.insert(node.name);
    CHECK(picked == std::set<std::string>{members.begin(), members.end()});
    for (const auto &member : members) {
        QGraphicsItem *copy = nodeItem(scene, member);
        REQUIRE(copy != nullptr);
        CHECK(copy->isSelected());
    }
    // A read of the same plan keeps it open and picked.
    scene.populate(model, FramePlanNodeKey{model.graph, "swapchain"}, 64);
    CHECK(itemsOfKind(scene, FramePlanGroupItem).empty());
    CHECK(scene.selectedNodes().size() == 2);

    // A loop the plan has no copies of: nothing moves.
    CHECK_FALSE(scene.selectLoopGroup("other_loop"));
    CHECK(scene.selectedNodes().size() == 2);
}

TEST_CASE("UX-12b decision 6: the line Show in Frame Plan puts up outlasts the reads every "
          "plan read asks for, while the copies stay picked",
          "[devstudio][frame-plan][lp4d2][ux12b]") {
    application();
    Json wire = regionGroupingBaselineFramePlan();
    const StringSet members{"HorizontalBlur_0", "VerticalBlur_0"};
    std::int64_t element = 0;
    for (auto &node : wire.at("nodes"))
        if (members.contains(node.at("name").get<std::string>()))
            node["loop_origin"] = {{"loop", "bloom_loop"}, {"element", element++},
                                   {"node", "blur"}};
    wire["loops"] = Json::array({{{"node", "bloom_loop"}, {"tier", "expand"}, {"count", 2},
                                  {"elements", Json::array({
                                      {{"element", 0}, {"index", 0}, {"branch", 0}},
                                      {{"element", 1}, {"index", 1}, {"branch", 0}}})},
                                  {"unmatched_branches", Json::array()}}});
    EmbeddedViewport viewport;
    FramePlanEditRecorder recorder;
    FramePlanWidget widget{&viewport};
    std::vector<qint64> reads;
    qint64 next = 9100;
    widget.setPlanRequest([&](QString *) {
        reads.push_back(next);
        return next++;
    });
    widget.resize(1200, 800);
    widget.show();
    widget.setEditCapability(recorder.capability());
    QApplication::processEvents();

    // Show in Frame Plan: the plan is read, and the copies picked.
    widget.showLoopGroup(QStringLiteral("bloom_loop"));
    REQUIRE(reads.size() == 1);
    Q_EMIT viewport.inspectorRpcSucceeded(reads[0], QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();
    const QString picked = QStringLiteral("Loop bloom_loop: its copies are picked.");
    CHECK(statusText(widget) == picked);
    // The plan read asked for the authoring context and the task catalog;
    // each lands after it and draws the plan again. The line stays.
    REQUIRE(recorder.result);
    REQUIRE(recorder.context_id != 0);
    REQUIRE(recorder.task_id != 0);
    recorder.result(recorder.context_id, QByteArrayLiteral("{}"));
    QApplication::processEvents();
    CHECK(statusText(widget) == picked);
    recorder.result(recorder.task_id, QByteArrayLiteral("{}"));
    QApplication::processEvents();
    CHECK(statusText(widget) == picked);
    // A plan read again (a commit elsewhere) keeps the copies picked, and so
    // the line.
    widget.planChangedByCommit();
    QApplication::processEvents();
    REQUIRE(reads.size() == 2);
    Q_EMIT viewport.inspectorRpcSucceeded(reads[1], QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();
    CHECK(statusText(widget) == picked);
}

TEST_CASE("LP-4c a loop's or a definition's refusal reads as its sentence, "
          "and a graph variant is named whatever the code",
          "[devstudio][frame-plan][lp4c][refusal]") {
    application();
    // LP-0d: the graph's own code, `variant` beside the fields, no message.
    const Json variant{{"code", "graph.loop_expression_invalid@1"},
                       {"variant", "preview"},
                       {"node", "steps"},
                       {"pointer", "/nodes/1/parameters/over/range/count"},
                       {"reason", "not_static"}};
    CHECK(renderAuthoringRefusalLine(variant) ==
          "The preview graph refused it: An expression of loop steps is not valid: "
          "not_static at /nodes/1/parameters/over/range/count "
          "(graph.loop_expression_invalid@1).");
    // The render target form says the same for it.
    CHECK(framePlanRenderTargetRejection(variant) == renderAuthoringRefusalLine(variant));
    // A view_select refusal with a variant, handed on the same way.
    const Json view_select{{"code", "graph.view_select_primary_family@1"},
                           {"variant", "xr"},
                           {"pass", "tint"},
                           {"family", "$main"}};
    CHECK(framePlanRenderTargetRejection(view_select) ==
          "The xr graph refused it: tint draws the main view ($main), which has "
          "no axes to select (graph.view_select_primary_family@1).");
    // Without a variant a definition refusal is its sentence alone.
    CHECK(renderAuthoringRefusalLine({{"code", "graph.definition_duplicate@1"},
                                      {"definition", "loop_body"}}) ==
          "A definition named loop_body already exists (graph.definition_duplicate@1).");
    // LP-2: a draw-loop refusal comes back with its own code and fields, the
    // message still "<code>: <fields>". LP-4d2: graph.loop_variant_missing@1
    // names the material's variant material_variant (engine 5e246063); in the
    // preview graph's preflight the graph variant takes `variant`.
    const Json fields{{"node", "draws"}, {"material", "box"}, {"material_variant", "outline"}};
    Json main_graph = fields;
    main_graph["participant"] = "pelican.render_pipeline";
    main_graph["code"] = "graph.loop_variant_missing@1";
    main_graph["message"] = "graph.loop_variant_missing@1: " + fields.dump();
    CHECK(renderAuthoringRefusalLine(main_graph) ==
          "Loop draws: material box has no outline variant (graph.loop_variant_missing@1).");
    CHECK(framePlanRenderTargetRejection(main_graph) == renderAuthoringRefusalLine(main_graph));
    Json preview = main_graph;
    preview["variant"] = "preview";
    CHECK(renderAuthoringRefusalLine(preview) ==
          "The preview graph refused it: Loop draws: material box has no outline variant "
          "(graph.loop_variant_missing@1).");
    // A render target edit that hit one reads as the loop's sentence too.
    const Json limit_fields{{"node", "draws"}, {"count", 17}, {"limit", 16}};
    Json limit = limit_fields;
    limit["code"] = "graph.loop_override_compile_limit@1";
    limit["message"] = "graph.loop_override_compile_limit@1: " + limit_fields.dump();
    CHECK(framePlanRenderTargetRejection(limit) ==
          "Loop draws needs 17 shader compiles; the limit is 16 "
          "(graph.loop_override_compile_limit@1).");
}

// --- LP-4e: Decompose into loop, and a draw_loop on its material pass.

namespace {

// The WP343 context with `wp343_mixed_a` declared as a material pass. `how`:
// "forked" (a pass of the project's preset), "direct" (the root declares it)
// or "engine" (a pass of an engine preset nobody forked).
void lp4eMaterialPassContext(Json &context, const std::string &how) {
    auto &pass = context["graphs"][0]["passes"][0];
    REQUIRE(pass.at("name") == "wp343_mixed_a");
    pass["declaration"]["type"] = "material";
    pass["declaration"]["material_contract"] = "forward_opaque_v1";
    if (how == "direct") {
        context.erase("pipeline_preset");
        context["config_kind"] = "direct";
        pass["editable_preset_pass"] = false;
        pass["editable_root_pass"] = true;
        pass["root_declaration"] = pass.at("declaration");
        pass["provenance"]["source"] = "project";
    } else if (how == "engine") {
        context["pipeline_preset"]["reference"] = "engine://presets/hybrid_v1.json";
        context["pipeline_preset"]["name"] = "hybrid_v1";
        pass["editable_preset_pass"] = false;
    }
}

Json lp4eDecomposeSent(const FramePlanEditRecorder &recorder) {
    return Json::parse(QJsonDocument(recorder.params).toJson().toStdString());
}

} // namespace

TEST_CASE("LP-4e Decompose into loop confirms, sends the digests its pass lives under, and "
          "shows the loop it made",
          "[devstudio][frame-plan][lp4e][canvas-edit]") {
    application();
    EditingFixture fixture;
    // A pass of the project's preset goes with the preset's digest too; one
    // the direct root declares, with the root's digest alone.
    const std::string how = GENERATE(std::string{"forked"}, std::string{"direct"});
    CAPTURE(how);
    fixture.tweak_context = [&](Json &context) { lp4eMaterialPassContext(context, how); };
    fixture.open();
    QStringList decomposed;
    fixture.widget.setLoopDecomposedAction(
        [&](const QString &loop) { decomposed << loop; });
    const FramePlanNodeKey pass{fixture.graph, "wp343_mixed_a"};
    // The entry is offered on the material pass and on nothing else.
    auto &scene = fixture.logical();
    REQUIRE(scene.decomposeOffered);
    CHECK(scene.decomposeOffered(pass) == std::optional<std::string>{std::string{}});
    CHECK_FALSE(scene.decomposeOffered({fixture.graph, "wp343_mixed_d"}).has_value());

    fixture.widget.decomposeMaterialPass(QString::fromStdString(fixture.graph),
                                         QStringLiteral("wp343_mixed_a"));
    QApplication::processEvents();
    // Nothing goes before the question is answered, and it says Undo takes
    // it back (RU-1).
    CHECK(fixture.recorder.submits == 0);
    auto *prompt = fixture.widget.findChild<QMessageBox *>(
        QStringLiteral("pelican.framePlan.decomposeConfirm"));
    REQUIRE(prompt != nullptr);
    CHECK(prompt->text().contains(QStringLiteral("Undo takes it back")));
    CHECK_FALSE(prompt->text().contains(QStringLiteral("cannot be undone")));
    auto *accept = prompt->findChild<QPushButton *>(
        QStringLiteral("pelican.framePlan.decomposeConfirm.ok"));
    REQUIRE(accept != nullptr);
    accept->click();
    QApplication::processEvents();
    REQUIRE(fixture.recorder.submits == 1);
    CHECK(fixture.recorder.method == QStringLiteral("decompose_material_pass"));
    Json expected{{"base_source_digest", RootDigest}, {"pass", "wp343_mixed_a"}};
    if (how == "forked") expected["base_preset_digest"] = PresetDigest;
    CHECK(lp4eDecomposeSent(fixture.recorder) == expected);

    // Accepted with what it made, then committed: the loop is handed on.
    fixture.recorder.result(
        fixture.recorder.submit_id,
        QByteArray::fromStdString(
            Json{{"status", "accepted"}, {"ticket", "t-1"},
                 {"decomposed", {{"pass", "wp343_mixed_a"}, {"loop", "wp343_mixed_a_draws"},
                                 {"definition", "wp343_mixed_a_draw"}}}}
                .dump()));
    QApplication::processEvents();
    CHECK(decomposed.isEmpty());
    REQUIRE(fixture.recorder.ticket_id != 0);
    fixture.recorder.result(
        fixture.recorder.ticket_id,
        QByteArray::fromStdString(Json{{"status", "committed"}, {"committed", true}}.dump()));
    QApplication::processEvents();
    CHECK(decomposed == QStringList{QStringLiteral("wp343_mixed_a_draws")});
}

TEST_CASE("LP-4e Decompose into loop says why it cannot, before and after it is sent",
          "[devstudio][frame-plan][lp4e][refusal]") {
    application();
    SECTION("Cancel sends nothing") {
        EditingFixture fixture;
        fixture.tweak_context = [](Json &context) { lp4eMaterialPassContext(context, "forked"); };
        fixture.open();
        fixture.widget.decomposeMaterialPass(QString::fromStdString(fixture.graph),
                                             QStringLiteral("wp343_mixed_a"));
        QApplication::processEvents();
        auto *cancel = fixture.widget.findChild<QPushButton *>(
            QStringLiteral("pelican.framePlan.decomposeConfirm.cancel"));
        REQUIRE(cancel != nullptr);
        cancel->click();
        QApplication::processEvents();
        CHECK(fixture.recorder.submits == 0);
    }
    SECTION("an engine preset's pass: fork first, nothing sent") {
        EditingFixture fixture;
        fixture.tweak_context = [](Json &context) { lp4eMaterialPassContext(context, "engine"); };
        fixture.open();
        const FramePlanNodeKey pass{fixture.graph, "wp343_mixed_a"};
        const auto reason = fixture.logical().decomposeOffered(pass);
        REQUIRE(reason.has_value());
        CHECK(*reason ==
              "wp343_mixed_a is a pass of the engine preset hybrid_v1: fork the preset into the "
              "project first (Features tab, Fork), then decompose the pass there.");
        fixture.widget.decomposeMaterialPass(QString::fromStdString(fixture.graph),
                                             QStringLiteral("wp343_mixed_a"));
        QApplication::processEvents();
        CHECK(fixture.widget.findChild<QMessageBox *>(
                  QStringLiteral("pelican.framePlan.decomposeConfirm")) == nullptr);
        CHECK(fixture.recorder.submits == 0);
        CHECK(statusText(fixture.widget).contains(QStringLiteral("fork the preset")));
    }
    SECTION("a feature's pass is not decomposed; its reason is the entry's tooltip") {
        EditingFixture fixture;
        fixture.tweak_context = [](Json &context) {
            auto &pass = context["graphs"][0]["passes"][2];
            REQUIRE(pass.at("name") == "wp343_mixed_c");
            pass["declaration"]["type"] = "material";
        };
        fixture.open();
        const auto reason = fixture.logical().decomposeOffered({fixture.graph, "wp343_mixed_c"});
        REQUIRE(reason.has_value());
        CHECK(*reason == "wp343_mixed_c is a pass of a feature (project://passes/mixed.json): a "
                         "feature's pass is replaced through its pass_replacements, not decomposed.");
    }
    SECTION("the engine's refusals, one sentence per reason") {
        const auto line = [](Json fields) {
            fields["code"] = "material_pass_not_decomposable";
            fields["pass"] = "forward_opaque";
            return framePlanDecomposeRejection(fields);
        };
        CHECK(line({{"reason", "not_found"}}) ==
              "Neither the rendering config nor its preset declares a pass named forward_opaque "
              "(material_pass_not_decomposable).");
        CHECK(line({{"reason", "graph_pass"}}) ==
              "forward_opaque is already a render.material_pass of the graph: edit its loop in "
              "the Graph tab (material_pass_not_decomposable).");
        CHECK(line({{"reason", "feature_pass"}, {"feature", "sky"}}) ==
              "forward_opaque is a pass of the feature sky: a feature's pass is replaced through "
              "its pass_replacements, not decomposed (material_pass_not_decomposable).");
        CHECK(line({{"reason", "not_material"}, {"type", "fullscreen"}}) ==
              "forward_opaque is a fullscreen pass; only a material pass decomposes into a loop "
              "(material_pass_not_decomposable).");
        CHECK(line({{"reason", "contract_unsupported"}, {"contract", "legacy_gbuffer_v1"}}) ==
              "forward_opaque draws with legacy_gbuffer_v1, which no draw_loop runs over "
              "(material_pass_not_decomposable).");
        CHECK(line({{"reason", "field_unsupported"}, {"field", "draw_order"}}) ==
              "forward_opaque sets draw_order, which a render.material_pass cannot carry: remove "
              "it first (material_pass_not_decomposable).");
        CHECK(line({{"reason", "attachment_unsupported"}, {"socket", "depth"}}) ==
              "The graph cannot name forward_opaque's depth attachment (a subresource, no depth, "
              "or more colours than the contract's sockets) (material_pass_not_decomposable).");
        CHECK(framePlanDecomposeRejection({{"code", "project_preset_required"},
                                           {"pass", "forward_opaque"},
                                           {"preset", "engine://presets/hybrid_v1.json"}}) ==
              "forward_opaque is a pass of the engine preset engine://presets/hybrid_v1.json: "
              "fork the preset into the project first (Features tab, Fork), then decompose the "
              "pass there (project_preset_required).");
        CHECK(framePlanDecomposeRejection({{"code", "graph.node_duplicate@1"},
                                           {"node", "forward_opaque_draws"}, {"scope", "root"}}) ==
              "The graph already has a node named forward_opaque_draws, which decomposing would "
              "add: rename it first (graph.node_duplicate@1).");
        CHECK(framePlanDecomposeRejection({{"code", "graph.definition_duplicate@1"},
                                           {"definition", "forward_opaque_draw"}}) ==
              "A definition named forward_opaque_draw already exists (graph.definition_duplicate@1).");
    }
    SECTION("a refusal that arrives reads as its sentence on the status line") {
        EditingFixture fixture;
        fixture.tweak_context = [](Json &context) { lp4eMaterialPassContext(context, "direct"); };
        fixture.open();
        fixture.widget.decomposeMaterialPass(QString::fromStdString(fixture.graph),
                                             QStringLiteral("wp343_mixed_a"));
        QApplication::processEvents();
        auto *accept = fixture.widget.findChild<QPushButton *>(
            QStringLiteral("pelican.framePlan.decomposeConfirm.ok"));
        REQUIRE(accept != nullptr);
        accept->click();
        QApplication::processEvents();
        REQUIRE(fixture.recorder.submits == 1);
        fixture.recorder.result(
            fixture.recorder.submit_id,
            QByteArray::fromStdString(
                Json{{"status", "rejected"},
                     {"error", {{"code", "material_pass_not_decomposable"},
                                {"message", "Material pass wp343_mixed_a cannot be decomposed"},
                                {"pass", "wp343_mixed_a"}, {"reason", "field_unsupported"},
                                {"field", "draw_order"}}}}
                    .dump()));
        QApplication::processEvents();
        CHECK(statusText(fixture.widget).contains(
            QStringLiteral("wp343_mixed_a sets draw_order, which a render.material_pass cannot "
                           "carry")));
    }
}

TEST_CASE("LP-4e a material pass a draw_loop draws in says so under its heading, and lists its "
          "materials by branch",
          "[devstudio][frame-plan][lp4e]") {
    application();
    EditingFixture fixture;
    fixture.tweak_context = [](Json &context) { lp4eMaterialPassContext(context, "direct"); };
    // Two materials on branch 0 of 300; the plan's page holds these two.
    fixture.wire["loops"] = Json::array({{{"node", "wp343_mixed_a_draws"}, {"tier", "draw_loop"},
                                          {"pass", "wp343_mixed_a"}, {"count", 300},
                                          {"elements", Json::array({
                                              {{"element", "project://m.material.json#metal"},
                                               {"index", 0}, {"branch", 0}},
                                              {{"element", "asset://box.glb#material[0]"},
                                               {"index", 1}, {"branch", 1}}})},
                                          {"unmatched_branches", Json::array({2})},
                                          {"total", 300}, {"offset", 0}, {"limit", 256}}});
    const FramePlanModel model = buildFramePlanModel(fixture.wire.dump());
    REQUIRE(model.loops.size() == 1);
    CHECK(model.loops[0].pass == "wp343_mixed_a");
    CHECK(model.loops[0].elements[0].key == "project://m.material.json#metal");
    CHECK(model.loops[0].elementCount() == 300);
    CHECK(framePlanDrawLoopLine(model, "wp343_mixed_a") ==
          "loop: wp343_mixed_a_draws (300 materials)");
    CHECK(framePlanDrawLoopLine(model, "wp343_mixed_b").empty());
    fixture.open();
    auto &scene = fixture.logical();
    const auto lines = itemsOfKind(scene, FramePlanDrawLoopItem);
    REQUIRE(lines.size() == 1);
    CHECK(lines.front()->data(FramePlanNameRole).toString() == QStringLiteral("wp343_mixed_a"));
    // It is already a graph pass: the entry says so instead of sending.
    const auto reason = scene.decomposeOffered({fixture.graph, "wp343_mixed_a"});
    REQUIRE(reason.has_value());
    CHECK(*reason == "wp343_mixed_a is already a render.material_pass of the graph; its loop "
                     "wp343_mixed_a_draws is edited in the Graph tab.");
    // The details list the materials by branch, the empty branch named, and
    // say how many more there are.
    QGraphicsItem *selected = nodeItem(scene, "wp343_mixed_a");
    REQUIRE(selected != nullptr);
    selected->setSelected(true);
    QApplication::processEvents();
    auto *details = fixture.widget.findChild<QTreeWidget *>(
        QStringLiteral("pelican.framePlanLogicalDetails"));
    REQUIRE(details != nullptr);
    QStringList texts;
    for (QTreeWidgetItemIterator it{details}; *it; ++it)
        texts << (*it)->text(1) + QStringLiteral(" | ") + (*it)->text(2);
    const auto joined = texts.join(QLatin1Char{'\n'});
    CHECK(joined.contains(QStringLiteral("Draw loop wp343_mixed_a_draws (300)")));
    CHECK(joined.contains(QStringLiteral("Material | project://m.material.json#metal")));
    CHECK(joined.contains(QStringLiteral("Material | asset://box.glb#material[0]")));
    CHECK(joined.contains(QStringLiteral("none: no material takes this branch")));
    CHECK(joined.contains(QStringLiteral("2 of 300 shown")));
    CHECK(fixture.widget.findChild<QPushButton *>(
              QStringLiteral("pelican.framePlan.loopMaterialsMore")) != nullptr);
}

// --- LP-4f2: a shipped decomposed graph in place of a feature (GT-1), and
// VF-1's refusal.

namespace {

// get_graph_templates as GT-1 answers it, cut down to what Studio reads:
// bloom removes its feature and adds two defines; the cascade replaces one
// pass of the directional shadow.
Json lp4f2Templates() {
    const auto document = [](const Json &decomposes, const Json &nodes, const Json &definitions) {
        return Json{{"schema", "pelican.graph"},
                    {"version", 1},
                    {"decomposes", decomposes},
                    {"definitions", definitions},
                    {"nodes", nodes}};
    };
    const Json bloom_decomposes{
        {"feature", "engine://features/bloom.json"},
        {"remove_feature", true},
        {"shader_defines", Json::array({"PELICAN_FEATURE_BLOOM_THRESHOLD=0.4",
                                        "PELICAN_FEATURE_BLOOM_KNEE=0.7"})}};
    const Json cascade_decomposes{{"feature", "engine://features/shadow_directional.json"},
                                  {"pass_replacements", {{"shadow_depth", "shadow_cascades"}}}};
    return Json{
        {"templates",
         Json::array(
             {{{"reference", "engine://graphs/bloom_loop.json"},
               {"title", "Bloom loop"},
               {"decomposes", bloom_decomposes},
               {"document",
                document(bloom_decomposes,
                         Json::array({{{"id", "bloom_lit"}, {"op", "render.target_ref"}},
                                      {{"id", "bloom_down"}, {"op", "graph.loop"}},
                                      {{"id", "bloom_up"}, {"op", "graph.loop"}}}),
                         {{"bloom_down_step", Json::object()}, {"bloom_up_step", Json::object()}})}},
              {{"reference", "engine://graphs/shadow_cascade_loop.json"},
               {"title", "Shadow cascade loop"},
               {"description", "One depth pass a cascade."},
               {"decomposes", cascade_decomposes},
               {"document", document(cascade_decomposes,
                                     Json::array({{{"id", "shadow_cascades"}, {"op", "graph.loop"}}}),
                                     {{"shadow_cascade", Json::object()}})}},
              // No decomposes.feature: nothing a feature row could offer.
              {{"reference", "engine://graphs/loose.json"},
               {"title", "Loose"},
               {"decomposes", Json::object()},
               {"document", Json::object()}}})}};
}

} // namespace

TEST_CASE("LP-4f2 the templates name the features that have a loop version, and what taking "
          "one in does",
          "[devstudio][frame-plan][lp4f2]") {
    const auto templates = graphTemplatesFromResponse(lp4f2Templates());
    REQUIRE(templates.size() == 2);
    const auto *bloom = graphTemplateForFeature(templates, "engine://features/bloom.json");
    REQUIRE(bloom != nullptr);
    CHECK(bloom->reference == "engine://graphs/bloom_loop.json");
    CHECK(bloom->loops == std::vector<std::string>{"bloom_down", "bloom_up"});
    CHECK(graphTemplateForFeature(templates, "engine://features/ssao.json") == nullptr);
    const auto *cascade =
        graphTemplateForFeature(templates, "engine://features/shadow_directional.json");
    REQUIRE(cascade != nullptr);

    // The confirmation says what happens, from decomposes.
    const auto bloom_text = graphTemplateConfirmation(*bloom);
    CHECK(bloom_text.starts_with("Bloom loop\n\n"));
    CHECK(bloom_text.find("The feature bloom is removed") != std::string::npos);
    CHECK(bloom_text.find("gains 3 nodes and 2 definitions") != std::string::npos);
    CHECK(bloom_text.find("PELICAN_FEATURE_BLOOM_THRESHOLD=0.4, PELICAN_FEATURE_BLOOM_KNEE=0.7") !=
          std::string::npos);
    CHECK(bloom_text.find("Undo takes it back") != std::string::npos);
    const auto cascade_text = graphTemplateConfirmation(*cascade);
    CHECK(cascade_text.starts_with("Shadow cascade loop: One depth pass a cascade.\n\n"));
    CHECK(cascade_text.find("The feature shadow_directional stays; 1 of its passes is replaced "
                            "by shadow_cascades.") != std::string::npos);
    CHECK(cascade_text.find("Shader defines") == std::string::npos);

    // The digests: the root's alone for a direct root's feature, the
    // preset's too for one the project's preset lists.
    CHECK(graphTemplateApplyParams(*bloom, std::string(64, 'a'), {}) ==
          Json{{"base_source_digest", std::string(64, 'a')},
               {"template", "engine://graphs/bloom_loop.json"}});
    CHECK(graphTemplateApplyParams(*bloom, std::string(64, 'a'), std::string(64, 'b')) ==
          Json{{"base_source_digest", std::string(64, 'a')},
               {"base_preset_digest", std::string(64, 'b')},
               {"template", "engine://graphs/bloom_loop.json"}});
    // An engine preset's feature: fork first, nothing sent.
    CHECK(graphTemplateRefusal("engine://features/bloom.json", "preset",
                               "engine://render_pipelines/hybrid_v1.json") ==
          "bloom is a feature of the engine preset engine://render_pipelines/hybrid_v1.json: fork "
          "the preset into the project first (Features tab, Fork), then replace the feature there.");
    CHECK(graphTemplateRefusal("engine://features/bloom.json", "preset",
                               "project://passes/local-pipeline.json")
              .empty());
    CHECK(graphTemplateRefusal("engine://features/bloom.json", "root", "passes/main.json").empty());

    // What the Graph tab selects: the first loop the engine says it added.
    CHECK(graphTemplateAppliedLoop(*bloom, Json{{"applied",
                                                 {{"nodes", Json::array({"bloom_lit", "bloom_down",
                                                                         "bloom_up"})}}}}) ==
          "bloom_down");
    CHECK(graphTemplateAppliedLoop(*bloom, Json{{"status", "accepted"}}).empty());
    CHECK(graphTemplateAppliedLine(*bloom) == "Replaced bloom with Bloom loop. Undo takes it back.");
}

TEST_CASE("LP-4f2 each refusal of apply_graph_template reads as its sentence",
          "[devstudio][frame-plan][lp4f2][refusal]") {
    const auto line = [](Json error) { return graphTemplateRejection(error); };
    const std::string bloom = "engine://graphs/bloom_loop.json";
    CHECK(line({{"code", "graph_template_unknown"}, {"template", "engine://graphs/nope.json"}}) ==
          "The engine does not ship the graph engine://graphs/nope.json (graph_template_unknown).");
    CHECK(line({{"code", "graph_template_invalid"}, {"template", bloom}, {"reason", "no decomposes"}}) ==
          "The graph engine://graphs/bloom_loop.json cannot be taken in: no decomposes "
          "(graph_template_invalid).");
    CHECK(line({{"code", "graph_template_feature_absent"},
                {"template", bloom},
                {"feature", "engine://features/bloom.json"}}) ==
          "Neither the rendering config nor its preset lists bloom, which the graph replaces: add "
          "the feature, or refresh the list (graph_template_feature_absent).");
    CHECK(line({{"code", "graph_template_pass_replacement_conflict"},
                {"feature", "engine://features/shadow_directional.json"},
                {"pass", "shadow_depth"},
                {"current", "my_shadows"},
                {"requested", "shadow_cascades"}}) ==
          "The feature shadow_directional already replaces its pass shadow_depth with my_shadows, "
          "and the graph would replace it with shadow_cascades: remove that replacement first "
          "(graph_template_pass_replacement_conflict).");
    CHECK(line({{"code", "project_preset_required"},
                {"feature", "engine://features/bloom.json"},
                {"preset", "engine://render_pipelines/hybrid_v1.json"}}) ==
          "bloom is a feature of the engine preset engine://render_pipelines/hybrid_v1.json: fork "
          "the preset into the project first (Features tab, Fork), then replace the feature there "
          "(project_preset_required).");
    CHECK(line({{"code", "graph.node_duplicate@1"}, {"node", "bloom_lit"}, {"scope", "root"}}) ==
          "The graph already has a node named bloom_lit, which the loop version adds -- perhaps it "
          "is in already (Undo takes an earlier one back), or rename that node first "
          "(graph.node_duplicate@1).");
    CHECK(line({{"code", "graph.definition_duplicate@1"}, {"definition", "shadow_cascade"}}) ==
          "The graph already has a definition named shadow_cascade, which the loop version adds -- "
          "perhaps it is in already (Undo takes an earlier one back), or rename that definition "
          "first (graph.definition_duplicate@1).");
    // Relayed under error_fields, the same.
    CHECK(line({{"code", "graph.definition_duplicate@1"},
                {"error_fields", {{"definition", "shadow_cascade"}}}})
              .starts_with("The graph already has a definition named shadow_cascade,"));
    // Anything else reads as any render edit's refusal.
    CHECK(line({{"code", "external_modification"}, {"message", "The document changed"}}) ==
          "external_modification: The document changed");
}

TEST_CASE("LP-4f2 VF-1's refusal names the pass, the sizes and the family's first pass",
          "[devstudio][frame-plan][lp4f2][vf1][refusal]") {
    const Json fields{{"family", "$capture/cube"},
                      {"pass", "loop/0/face_pass"},
                      {"target", "loop/0/face_pass__color"},
                      {"extent", {{"output_scale", 1.0}}},
                      {"expected_pass", "cube_capture_geometry"},
                      {"expected_target", "cube_capture_albedo"},
                      {"expected_extent", Json::array({256, 256})},
                      {"loop_origin", {{"loop", "loop"}, {"element", 0}, {"node", "face_pass"}}}};
    const std::string sentence =
        "loop/0/face_pass (loop loop, body node face_pass) writes loop/0/face_pass__color at "
        "screen ×1.0, but $capture/cube draws at 256×256 (its first pass "
        "cube_capture_geometry writes cube_capture_albedo): the passes of one view family draw at "
        "one size (render.view_family_extent_mismatch@1).";
    // Beside the code (an edit's refusal, isCarriedRefusalCode) ...
    auto beside = fields;
    beside["code"] = "render.view_family_extent_mismatch@1";
    CHECK(viewFamilyExtentRefusalDescription(beside) == sentence);
    // ... relayed under error_fields ...
    CHECK(viewFamilyExtentRefusalDescription(
              {{"code", "render.view_family_extent_mismatch@1"}, {"error_fields", fields}}) ==
          sentence);
    // ... or only in a message (a load that stopped on it).
    CHECK(viewFamilyExtentRefusalDescription(
              {{"code", "render_pipeline_preflight_failed"},
               {"message", "render.view_family_extent_mismatch@1: " + fields.dump()}}) == sentence);
    // Every render refusal line of the panels reads it.
    CHECK(renderAuthoringRefusalLine(beside) == sentence);
    CHECK(viewFamilyExtentText({{"output_scale", 0.25}}) == "screen ×0.25");
    CHECK(viewFamilyExtentText(Json::array({512, 128})) == "512×128");
    // Nothing else is it.
    CHECK(viewFamilyExtentRefusalDescription({{"code", "graph.loop_nested@1"}}).empty());
}

TEST_CASE("LP-4f2 a feature's group offers its loop version when the Features tab has one",
          "[devstudio][frame-plan][lp4f2][canvas-edit]") {
    application();
    EditingFixture fixture;
    fixture.open();
    auto &scene = fixture.logical();
    QGraphicsItem *fallback = nodeItem(scene, "wp343_mixed_c");
    QGraphicsItem *region = nodeItem(scene, "wp343_mixed_a");
    REQUIRE(fallback != nullptr);
    REQUIRE(region != nullptr);
    const auto feature_group = fallback->data(FramePlanGroupIdRole).toString();
    const auto region_group = region->data(FramePlanGroupIdRole).toString();
    // The feature's group names the feature its passes came from; a
    // region's names none.
    const auto feature = scene.groupFeatureReference(feature_group);
    CHECK_FALSE(feature.empty());
    CHECK(scene.groupFeatureReference(region_group).empty());

    std::vector<std::string> asked;
    QStringList replaced;
    std::optional<QString> answer = QString{};
    fixture.widget.setLoopVersionActions(
        [&](const QString &reference) {
            asked.push_back(reference.toStdString());
            return answer;
        },
        [&](const QString &reference) { replaced << reference; });
    auto *entry = fixture.widget.findChild<QAction *>(
        QStringLiteral("pelican.framePlan.replaceWithLoopVersion"));
    REQUIRE(entry != nullptr);

    // A feature that has one: offered, and it hands the feature on.
    {
        QMenu menu;
        REQUIRE(scene.addLoopVersionEntry(menu, feature_group));
        CHECK(asked == std::vector<std::string>{feature});
        CHECK(menu.actions().contains(entry));
        CHECK(entry->isEnabled());
        entry->trigger();
        CHECK(replaced == QStringList{QString::fromStdString(feature)});
    }
    // A region's group: not a feature's, nothing asked, nothing offered.
    {
        QMenu menu;
        CHECK_FALSE(scene.addLoopVersionEntry(menu, region_group));
        CHECK(menu.actions().isEmpty());
        CHECK(asked.size() == 1);
    }
    // A feature the Features tab has no loop version for: no entry.
    answer = std::nullopt;
    {
        QMenu menu;
        CHECK_FALSE(scene.addLoopVersionEntry(menu, feature_group));
        CHECK(menu.actions().isEmpty());
    }
    // One it cannot take in now: offered disabled, the reason its tooltip.
    answer = QStringLiteral("Apply the pending edit first.");
    {
        QMenu menu;
        REQUIRE(scene.addLoopVersionEntry(menu, feature_group));
        CHECK_FALSE(entry->isEnabled());
        CHECK(entry->toolTip() == QStringLiteral("Apply the pending edit first."));
    }
}

// --- LP-4f3: the size a secondary view family draws at, as get_frame_plan
// says it (VF-2's family_extent, family_layers, family_extent_pass).

TEST_CASE("LP-4f3 the model reads family_extent, and the details say the family's size",
          "[devstudio][frame-plan][lp4f3][vf2]") {
    (void)application();
    Json wire = lp4bWire();
    for (auto &node : wire["nodes"]) {
        const auto name = node.at("name").get<std::string>();
        if (name == "wp343_mixed_a") {
            node["family_extent"] = Json::array({256, 256});
            node["family_layers"] = 6;
            node["family_extent_pass"] = "cube_capture_geometry";
        } else if (name == "wp343_mixed_b") {
            node["family_extent"] = {{"output_scale", 1.0}};
            node["family_layers"] = 1;
            node["family_extent_pass"] = "shadow_depth";
        }
    }
    const auto model = buildFramePlanModel(wire.dump());
    const auto &a = vi3aNode(model, "wp343_mixed_a");
    const auto &b = vi3aNode(model, "wp343_mixed_b");
    const auto &c = vi3aNode(model, "wp343_mixed_c");

    // The model reads them: a fixed size with its layers, one relative to
    // the output, and none on $main.
    REQUIRE(a.family_extent.has_value());
    CHECK(a.family_extent->size == std::array<std::uint64_t, 2>{256, 256});
    CHECK(a.family_extent->layers == 6);
    CHECK(a.family_extent->pass == "cube_capture_geometry");
    REQUIRE(b.family_extent.has_value());
    CHECK_FALSE(b.family_extent->size.has_value());
    CHECK(b.family_extent->output_scale == 1.0);
    CHECK(b.family_extent->layers == 1);
    CHECK_FALSE(c.family_extent.has_value());

    // The details line, written as VF-1's refusal writes an extent.
    CHECK(framePlanFamilyExtentLine(a) ==
          "Family size: 256×256 ×6 layers (from cube_capture_geometry)");
    CHECK(framePlanFamilyExtentLine(b) == "Family size: screen ×1.0 (from shadow_depth)");
    CHECK(framePlanFamilyExtentLine(c).empty());

    SECTION("the node's details carry it") {
        EmbeddedViewport viewport;
        FramePlanWidget widget{&viewport};
        widget.receiveResult(QByteArray::fromStdString(wire.dump()));
        QApplication::processEvents();
        QStringList sizes;
        for (auto *tree : widget.findChildren<QTreeWidget *>())
            for (const auto *item : tree->findItems(QStringLiteral("Family size"),
                                                    Qt::MatchExactly | Qt::MatchRecursive, 1))
                sizes.push_back(item->text(2));
        sizes.sort();
        CHECK(sizes == QStringList{QStringLiteral("256×256 ×6 layers (from cube_capture_geometry)"),
                                   QStringLiteral("screen ×1.0 (from shadow_depth)")});
    }

    SECTION("a family_extent Studio cannot read is refused with the node named") {
        for (auto &node : wire["nodes"])
            if (node.at("name") == "wp343_mixed_a") node["family_extent"] = Json::array({256});
        std::string refusal;
        try {
            (void)buildFramePlanModel(wire.dump());
        } catch (const std::exception &error) {
            refusal = error.what();
        }
        INFO(refusal);
        CHECK(refusal.find("family_extent") != std::string::npos);
    }
}

// --- Q-18. A Graph tab edit that committed (anything but a placement) can
// change the plan -- a loop's count, a branch, a view axis. The window tells
// the Frame Plan, which reads get_frame_plan again: at once when it is in
// view, after a read already in flight (which may predate the commit), and
// otherwise when it is next shown. Refresh is not needed.
TEST_CASE("Q-18 a commit elsewhere reads the plan again: now in view, after the read in flight, "
          "or when next shown",
          "[devstudio][frame-plan][q18]") {
    application();
    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    std::vector<qint64> reads;
    qint64 next = 9000;
    widget.setPlanRequest([&](QString *) {
        reads.push_back(next);
        return next++;
    });
    widget.resize(900, 600);
    widget.show();
    QApplication::processEvents();
    // Shown, with nothing committed anywhere: nothing is read.
    REQUIRE(reads.empty());
    const auto answer = [&](qint64 id) {
        Q_EMIT viewport.inspectorRpcSucceeded(id, QByteArray::fromStdString(mixedEditingWire().dump()));
        QApplication::processEvents();
    };

    // In view: at once.
    widget.planChangedByCommit();
    QApplication::processEvents();
    REQUIRE(reads.size() == 1);
    // Another commit while that read is out: not a second read beside it...
    widget.planChangedByCommit();
    QApplication::processEvents();
    CHECK(reads.size() == 1);
    // ...but one after it, since the read out may have been answered before
    // the commit.
    answer(reads[0]);
    REQUIRE(reads.size() == 2);
    answer(reads[1]);
    CHECK(reads.size() == 2);

    // Out of view: nothing now, once when shown again.
    widget.hide();
    QApplication::processEvents();
    widget.planChangedByCommit();
    QApplication::processEvents();
    CHECK(reads.size() == 2);
    widget.show();
    QApplication::processEvents();
    REQUIRE(reads.size() == 3);
    answer(reads[2]);
    // Shown again with nothing new: nothing more.
    widget.hide();
    QApplication::processEvents();
    widget.show();
    QApplication::processEvents();
    CHECK(reads.size() == 3);
}

TEST_CASE("UX-6a decisions 2 and 4: the Frame Plan tells the Graph after a reload that moved "
          "declarations, and its Physical plan offers Remove TRANSFER_SRC on an image kept out "
          "of aliasing for it",
          "[devstudio][frame-plan][ux6a]") {
    (void)application();
    Json wire = capturedRuntimeFramePlan();
    // Four physical resources no alias group holds:
    //   kept   -- unaliased, a reason with no causes (an engine before PO-1),
    //             TRANSFER_SRC in its usage: the usage decides;
    //   po1    -- unaliased, PO-1's cause names TRANSFER_SRC: the sentence
    //             decides, whatever the usage says;
    //   other  -- unaliased for another usage (PO-1), TRANSFER_SRC as well:
    //             removing it would not alias it, so no button;
    //   aliased -- aliasable, with TRANSFER_SRC: nothing to remove.
    auto &physical = wire["physical_target_plan"]["resources"];
    REQUIRE(physical.size() > 4);
    const auto name_of = [&](std::size_t index) {
        return physical[index].at("logical_resource").get<std::string>();
    };
    const auto kept = name_of(0);
    const auto po1 = name_of(1);
    const auto other = name_of(2);
    const auto aliased = name_of(3);
    for (std::size_t index = 0; index < 3; ++index) physical[index]["aliasable"] = false;
    physical[3]["aliasable"] = true;
    physical[0]["reason"] = "arbitrary read requires a materialized resource";
    physical[1]["reason"] = "logical materialization requirement is required: usage TRANSFER_SRC "
                            "is outside the alias-safe set {COLOR_ATTACHMENT, SAMPLED}";
    physical[2]["reason"] = "logical materialization requirement is required: usage STORAGE is "
                            "outside the alias-safe set {COLOR_ATTACHMENT, SAMPLED}";
    for (auto &declaration : wire["resources"]) {
        const auto name = declaration.at("name").get<std::string>();
        if (name == kept || name == other || name == aliased)
            declaration["usage"] = Json::array({"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"});
        else if (declaration.contains("usage") && declaration.at("usage").is_array()) {
            auto &usage = declaration["usage"];
            usage.erase(std::remove(usage.begin(), usage.end(), Json("TRANSFER_SRC")),
                        usage.end());
        }
    }

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    std::vector<std::string> dropped;
    widget.setDropTransferSourceAction(
        [&](const QString &resource, const std::optional<FramePlanLoopOrigin> &) {
            dropped.push_back(resource.toStdString());
            return true;
        });
    int reloaded = 0;
    widget.setShadersReloadedAction([&] { ++reloaded; });
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();

    // Decision 4: the two TRANSFER_SRC keeps out, and only they.
    const auto button = [&](const std::string &name) {
        return widget.findChild<QPushButton *>(QStringLiteral("pelican.framePlan.dropTransferSrc.") +
                                               QString::fromStdString(name));
    };
    auto *drop = button(kept);
    REQUIRE(drop != nullptr);
    CHECK(drop->isEnabled());
    REQUIRE(button(po1) != nullptr);
    CHECK(button(other) == nullptr);
    CHECK(button(aliased) == nullptr);
    drop->click();
    button(po1)->click();
    CHECK(dropped == std::vector<std::string>{kept, po1});

    // Decision 2: a reload that committed, or was refused for a pass, tells
    // the Graph; one that changed nothing does not.
    widget.receiveShaderReloadOutcome(ShaderReloadOutcome{.kind = ShaderReloadOutcome::Kind::unchanged});
    CHECK(reloaded == 0);
    widget.receiveShaderReloadOutcome(ShaderReloadOutcome{.kind = ShaderReloadOutcome::Kind::committed});
    CHECK(reloaded == 1);
}

TEST_CASE("UX-12c decision 2: a material pass's row says how many materials it drew and their "
          "draws, and the Materials tab lists each, drawn by the pass",
          "[devstudio][frame-plan][ux12c]") {
    application();
    EditingFixture fixture;
    fixture.wire["material_routing"] = {
        {"policy", "hybrid_auto_v1"},
        {"routes", {{"deferred_geometry",
                     {{"pass", "wp343_mixed_a"}, {"contract", "deferred_geometry_v1"},
                      {"phase", "opaque"}}}}}};
    bool marked = false;
    for (auto &node : fixture.wire["nodes"])
        if (node.value("name", std::string{}) == "wp343_mixed_a") {
            node["drawn_materials"] = Json::array(
                {{{"material", "pelican.primitive.default"}, {"id", 1}, {"draws", 1}},
                 {{"material", "pelican.primitive.default"}, {"id", 2}, {"draws", 3}},
                 {{"id", "asset://floor.glb#material[0]"}, {"draws", 1}}});
            node["drawn_materials_frame"] = 7;
            marked = true;
        }
    REQUIRE(marked);
    const FramePlanModel model = buildFramePlanModel(fixture.wire.dump());
    const auto drawn = std::ranges::find(model.nodes, std::string{"wp343_mixed_a"}, &FramePlanNode::name);
    REQUIRE(drawn != model.nodes.end());
    REQUIRE(drawn->drawn_materials);
    CHECK(drawn->drawn_materials->frame == 7);
    CHECK(drawn->drawn_materials->draws() == 5);
    // The engine's runtime id is a number.
    CHECK(drawn->drawn_materials->materials[0].id == "1");
    CHECK(drawn->drawn_materials->materials[2].name.empty());
    fixture.open();

    // The Passes row: the count and the draws, then each material.
    auto *passes = fixture.widget.findChild<QTreeWidget *>(QStringLiteral("pelican.framePlanPasses"));
    REQUIRE(passes != nullptr);
    QStringList facts;
    for (QTreeWidgetItemIterator it{passes}; *it; ++it)
        if ((*it)->parent() != nullptr && (*it)->parent()->text(1) == QStringLiteral("Materials drawn"))
            facts << (*it)->text(1) + QStringLiteral(" | ") + (*it)->text(2);
        else if ((*it)->text(1) == QStringLiteral("Materials drawn"))
            facts << QStringLiteral("group | ") + (*it)->text(2);
    // Two materials of one name are told apart by their ids.
    CHECK(facts == QStringList({QStringLiteral("group | 3 material(s), 5 draw(s) (frame 7)"),
                                QStringLiteral("pelican.primitive.default (material 1) | 1 draw(s)"),
                                QStringLiteral("pelican.primitive.default (material 2) | 3 draw(s)"),
                                QStringLiteral("asset://floor.glb#material[0] | 1 draw(s)")}));

    // The Materials tab: the route says what its pass drew, and each material
    // is a row of the pass.
    auto *materials = fixture.widget.findChild<QTreeWidget *>(QStringLiteral("pelican.framePlanMaterials"));
    REQUIRE(materials != nullptr);
    REQUIRE(materials->columnCount() == 6);
    QStringList rows;
    for (int index = 0; index < materials->topLevelItemCount(); ++index) {
        const auto *item = materials->topLevelItem(index);
        rows << item->text(0) + QStringLiteral(" | ") + item->text(1) + QStringLiteral(" | ") +
                    item->text(5);
    }
    CHECK(rows == QStringList({QStringLiteral("deferred_geometry | wp343_mixed_a | 3 material(s), "
                                              "5 draw(s) (frame 7)"),
                               QStringLiteral("pelican.primitive.default (material 1) | wp343_mixed_a | "
                                              "drawn by wp343_mixed_a, 1 draw(s)"),
                               QStringLiteral("pelican.primitive.default (material 2) | wp343_mixed_a | "
                                              "drawn by wp343_mixed_a, 3 draw(s)"),
                               QStringLiteral("asset://floor.glb#material[0] | wp343_mixed_a | drawn "
                                              "by wp343_mixed_a, 1 draw(s)")}));
    // The Graph's Materials drawn picks the route and the materials.
    fixture.widget.showMaterialRoutes(QStringLiteral("wp343_mixed_a"));
    CHECK(materials->selectedItems().size() == 4);
}

// --- UX-15b: the lower layer's decisions from the Frame Plan (engine EL-2).

namespace {

// Accepted, then committed: what the engine answers an edit it took.
void commitSubmitted(EditingFixture &fixture) {
    fixture.recorder.result(
        fixture.recorder.submit_id,
        QByteArray::fromStdString(Json{{"status", "accepted"}, {"ticket", "t-ux15b"}}.dump()));
    QApplication::processEvents();
    REQUIRE(fixture.recorder.ticket_id != 0);
    fixture.recorder.result(
        fixture.recorder.ticket_id,
        QByteArray::fromStdString(Json{{"status", "committed"}, {"committed", true}}.dump()));
    QApplication::processEvents();
}

QString bandText(FramePlanWidget &widget, const char *name) {
    auto *label = widget.findChild<QLabel *>(QString::fromLatin1(name));
    return label == nullptr ? QString{} : label->text();
}

} // namespace

TEST_CASE("UX-15b decision 1: Pin this candidate sends the ejected pin with that candidate; a pin "
          "the plan applies is a band with Unpin",
          "[devstudio][frame-plan][ux15b][canvas-edit]") {
    application();
    EditingFixture fixture;
    fixture.open();
    const auto &physical = fixture.wire.at("physical_target_plan");
    const Json ejected = physical.at("ejectable_pin_package");
    REQUIRE(ejected.is_object());
    std::string pinnable;
    std::string rejected;
    for (const auto &candidate : physical.at("backend_selection").at("candidates")) {
        const auto name = candidate.at("candidate").get<std::string>();
        if (candidate.value("feasible", false) && pinnable.empty()) pinnable = name;
        if (!candidate.value("feasible", false) && !candidate.value("selected", false))
            rejected = name;
    }
    REQUIRE_FALSE(pinnable.empty());
    CHECK(bandText(fixture.widget, "pelican.framePlan.backend.state")
              .startsWith(QStringLiteral("Not pinned")));

    // The candidate's row, picked; the named entry acts on it.
    auto *backends =
        fixture.widget.findChild<QTreeWidget *>(QStringLiteral("pelican.framePlanBackends"));
    REQUIRE(backends != nullptr);
    const auto rows = backends->findItems(QString::fromStdString(pinnable), Qt::MatchExactly, 0);
    REQUIRE(rows.size() == 1);
    backends->setCurrentItem(rows.front());
    auto *pin = fixture.widget.findChild<QAction *>(QStringLiteral("pelican.framePlan.backend.pin"));
    REQUIRE(pin != nullptr);
    pin->trigger();
    QApplication::processEvents();
    REQUIRE(fixture.recorder.submits == 1);
    CHECK(fixture.recorder.method == QStringLiteral("edit_vulkan_plan_pins"));
    Json expected_pin = ejected;
    expected_pin["pins"]["backend_candidate"] = pinnable;
    CHECK(sentParams(fixture.recorder) ==
          Json{{"base_source_digest", RootDigest}, {"family", "flat"}, {"pin", expected_pin}});
    commitSubmitted(fixture);
    CHECK(statusText(fixture.widget) == QString::fromStdString("Pinned " + pinnable + "."));

    // The plan read again applies it: the band says so and offers Unpin.
    Json pinned = fixture.wire;
    pinned["physical_target_plan"]["applied_pin_package"] = expected_pin;
    fixture.widget.receiveResult(QByteArray::fromStdString(pinned.dump()));
    QApplication::processEvents();
    CHECK(bandText(fixture.widget, "pelican.framePlan.backend.state") ==
          QString::fromStdString("Pinned: " + pinnable));
    auto *unpin = fixture.widget.findChild<QPushButton *>(
        QStringLiteral("pelican.framePlan.backend.unpinButton"));
    REQUIRE(unpin != nullptr);
    unpin->click();
    QApplication::processEvents();
    REQUIRE(fixture.recorder.submits == 2);
    CHECK(sentParams(fixture.recorder) == Json{{"base_source_digest", RootDigest},
                                               {"family", "flat"},
                                               {"pin", nullptr},
                                               {"graph", fixture.graph}});

    // What is not sent says why.
    const auto model = buildFramePlanModel(pinned.dump());
    CHECK(framePlanBackendPinRequest(FramePlanAuthoring{}, model, pinnable).refusal ==
          "The render authoring context has not arrived yet.");
    FramePlanAuthoring received;
    received.received = true;
    CHECK(framePlanBackendPinRequest(received, model, pinnable).refusal ==
          pinnable + " is pinned already.");
    if (!rejected.empty())
        CHECK(framePlanBackendPinRequest(received, model, rejected).refusal ==
              rejected + " was rejected by the planner: only a Selected or Feasible candidate "
                         "can be pinned.");
    CHECK(framePlanPackageFamily("main_render#xr") == "xr");
}

TEST_CASE("UX-15b decision 2: Freeze sends the ejected fragment as it is; Frozen offers Remove; a "
          "stale refusal is one sentence beside Eject again",
          "[devstudio][frame-plan][ux15b][canvas-edit]") {
    application();
    EditingFixture fixture;
    fixture.open();
    const Json fragment = fixture.wire.at("physical_target_plan").at("ejectable_physical_fragment");
    REQUIRE(fragment.is_object());
    CHECK(bandText(fixture.widget, "pelican.framePlan.physical.state")
              .startsWith(QStringLiteral("Not frozen")));
    auto *freeze = fixture.widget.findChild<QPushButton *>(
        QStringLiteral("pelican.framePlan.physical.freezeButton"));
    REQUIRE(freeze != nullptr);
    freeze->click();
    QApplication::processEvents();
    REQUIRE(fixture.recorder.submits == 1);
    CHECK(fixture.recorder.method == QStringLiteral("put_vulkan_physical_fragment"));
    const Json frozen{{"base_source_digest", RootDigest}, {"family", "flat"}, {"fragment", fragment}};
    CHECK(sentParams(fixture.recorder) == frozen);

    // The runtime's linker finds it stale when the change commits.
    fixture.recorder.result(
        fixture.recorder.submit_id,
        QByteArray::fromStdString(Json{{"status", "accepted"}, {"ticket", "t-stale"}}.dump()));
    QApplication::processEvents();
    fixture.recorder.result(
        fixture.recorder.ticket_id,
        QByteArray::fromStdString(
            Json{{"status", "rejected"},
                 {"error",
                  {{"code", "physical_fragment_stale"},
                   {"message", "Vulkan physical fragment automatic plan fingerprint is stale"},
                   {"family", "flat"},
                   {"graph", fixture.graph},
                   {"fingerprint", "automatic_plan"},
                   {"expected", "fnv1a64:0000000000000001"},
                   {"actual", "fnv1a64:0000000000000002"}}}}
                .dump()));
    QApplication::processEvents();
    auto *status = fixture.widget.findChild<QLabel *>(QStringLiteral("pelican.framePlanStatus"));
    REQUIRE(status != nullptr);
    CHECK(status->toolTip().contains(QStringLiteral(
        "The frozen plan no longer matches the graph (automatic_plan); Eject again "
        "(physical_fragment_stale).")));
    auto *eject = fixture.widget.findChild<QPushButton *>(
        QStringLiteral("pelican.framePlan.physical.ejectAgainButton"));
    REQUIRE(eject != nullptr);
    CHECK_FALSE(eject->isHidden());
    // Eject again: the plan on screen's fragment, in one edit.
    eject->click();
    QApplication::processEvents();
    REQUIRE(fixture.recorder.submits == 2);
    CHECK(sentParams(fixture.recorder) == frozen);
    CHECK(eject->isHidden());
    commitSubmitted(fixture);

    // The plan read again applies it: Frozen, and Remove.
    Json applied = fixture.wire;
    applied["physical_target_plan"]["applied_physical_fragment"] = fragment;
    fixture.widget.receiveResult(QByteArray::fromStdString(applied.dump()));
    QApplication::processEvents();
    CHECK(bandText(fixture.widget, "pelican.framePlan.physical.state")
              .startsWith(QStringLiteral("Frozen")));
    auto *remove = fixture.widget.findChild<QPushButton *>(
        QStringLiteral("pelican.framePlan.physical.removeButton"));
    REQUIRE(remove != nullptr);
    remove->click();
    QApplication::processEvents();
    REQUIRE(fixture.recorder.submits == 3);
    CHECK(sentParams(fixture.recorder) == Json{{"base_source_digest", RootDigest},
                                               {"family", "flat"},
                                               {"fragment", nullptr},
                                               {"graph", fixture.graph}});
}

TEST_CASE("UX-15b decisions 4 and 5: the resident BLAS only for a ray-query scene, and the rewrite "
          "that put a pass in the plan",
          "[devstudio][frame-plan][ux15b]") {
    application();
    Json wire = capturedRuntimeFramePlan();
    const auto rayQueryRow = [](FramePlanWidget &widget) -> QTreeWidgetItem * {
        auto *physical =
            widget.findChild<QTreeWidget *>(QStringLiteral("pelican.framePlanPhysical"));
        if (physical == nullptr) return nullptr;
        const auto rows = physical->findItems(QStringLiteral("Ray query acceleration structures"),
                                              Qt::MatchExactly, 0);
        return rows.isEmpty() ? nullptr : rows.front();
    };
    {
        // The captured project has no ray tracing: no section.
        REQUIRE_FALSE(wire.at("ray_query_acceleration_structures").value("requested", true));
        EmbeddedViewport viewport;
        FramePlanWidget widget{&viewport};
        widget.receiveResult(QByteArray::fromStdString(wire.dump()));
        QApplication::processEvents();
        CHECK(rayQueryRow(widget) == nullptr);
    }

    auto &scene = wire["ray_query_acceleration_structures"];
    scene["requested"] = true;
    scene["tlas_instance_count"] = 3;
    scene["named_tlas_instance_counts"] = {{"scene", 3}};
    scene["resident_blas_keys"] = Json::array(
        {{{"geometry_kind", "triangles"}, {"geometry_allocation_id", 7}, {"geometry_flags", 1},
          {"geometry_count", 2}, {"index_device_address", 0}, {"vertex_device_address", 0}},
         {{"geometry_kind", "aabbs"}, {"geometry_allocation_id", 9}, {"geometry_flags", 0},
          {"geometry_count", 1}, {"index_device_address", 0}, {"vertex_device_address", 0}}});
    const auto replaced = wire.at("nodes").at(1).at("name").get<std::string>();
    const auto untouched = wire.at("nodes").at(0).at("name").get<std::string>();
    wire["physical_target_plan"]["subgraph_replacements"] = Json::array(
        {{{"region", "bloom_region"},
          {"provider", "pelican.test.bloom"},
          {"implementation", "fused"},
          {"explicitly_selected", true},
          {"source_nodes", Json::array({"bloom_a", "bloom_b"})},
          {"replacement_nodes", Json::array({replaced})}}});
    wire["physical_target_plan"]["render_strategy"] = {{"name", "forward"},
                                                       {"provider", "pelican.strategy.forward"},
                                                       {"implementation", "default"},
                                                       {"graph_variant", "flat"}};

    const auto model = buildFramePlanModel(wire.dump());
    REQUIRE(model.ray_query.has_value());
    CHECK(model.ray_query->shown());
    CHECK(model.ray_query->resident_blas_keys ==
          std::vector<FramePlanBlasKey>{{"triangles", 7, 1, 2}, {"aabbs", 9, 0, 1}});
    CHECK(framePlanReplacedByLines(model, replaced) ==
          std::vector<std::string>{"subgraph replacement bloom_region by pelican.test.bloom (fused)"});
    // The strategy rewrote the whole graph: said once, not on each pass.
    CHECK(framePlanReplacedByLines(model, untouched).empty());
    REQUIRE(model.physical_plan.rewrites.size() == 2);
    CHECK(framePlanRewriteLine(model.physical_plan.rewrites[1]) ==
          "render strategy forward by pelican.strategy.forward (default), graph variant flat");

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();
    auto *section = rayQueryRow(widget);
    REQUIRE(section != nullptr);
    CHECK(section->text(2) == QStringLiteral("2 resident BLAS, 3 TLAS instances"));
    QStringList children;
    for (int index = 0; index < section->childCount(); ++index)
        children << section->child(index)->text(1);
    CHECK(children.contains(QStringLiteral("Resident BLAS (2)")));
    CHECK(children.contains(QStringLiteral("Named TLAS (1)")));

    auto *passes = widget.findChild<QTreeWidget *>(QStringLiteral("pelican.framePlanPasses"));
    REQUIRE(passes != nullptr);
    const auto rows = passes->findItems(QString::fromStdString(replaced), Qt::MatchExactly, 1);
    REQUIRE(rows.size() == 1);
    CHECK(rows.front()->toolTip(1) ==
          QStringLiteral("Replaced by subgraph replacement bloom_region by pelican.test.bloom (fused)"));
}
TEST_CASE("GL-5d the Graph's Order... and Fix sockets are the Frame Plan's own, run out of view once "
          "the plan and the context are read again; a graph node's pass takes the copy through "
          "the Graph",
          "[devstudio][frame-plan][gl5d]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = vi3bProjectRoot(temporary);
    EditingFixture fixture;
    fixture.widget.setProjectRoot(root);
    setVi3aPassInputs(fixture.wire, vi3bPassthroughInputs());
    fixture.wire["runtime_generation"] = 5;
    fixture.tweak_context = [](Json &context) { setVi3aInputs(context, {"t_a", "t_b"}); };
    std::vector<qint64> reads;
    qint64 next = 9300;
    fixture.widget.setPlanRequest([&](QString *) {
        reads.push_back(next);
        return next++;
    });
    QString ordered_graph;
    QStringList ordered;
    quint64 generation = 0;
    fixture.widget.setNodeOrderAction(
        [&](const QString &graph, const QStringList &names, quint64 published) {
            ordered_graph = graph;
            ordered = names;
            generation = published;
        });
    fixture.open();
    // Behind the Graph tab: the panel reads nothing on its own.
    fixture.widget.hide();
    QApplication::processEvents();
    QStringList said;
    const auto hear = [&](const QString &line) { said << line; };
    // The plan, then the context, as the engine answers them.
    const auto answer = [&] {
        REQUIRE_FALSE(reads.empty());
        Q_EMIT fixture.viewport.inspectorRpcSucceeded(reads.back(),
                                                      QByteArray::fromStdString(fixture.wire.dump()));
        QApplication::processEvents();
        auto context = mixedAuthoringContext(fixture.graph);
        fixture.tweak_context(context);
        fixture.recorder.result(fixture.recorder.context_id,
                                QByteArray::fromStdString(context.dump()));
        QApplication::processEvents();
    };

    // Decision 1: Order... waits for both reads, then opens on the plan.
    const auto plan_reads = reads.size();
    const auto contexts = fixture.recorder.context_id;
    fixture.widget.whenRead([&] { fixture.widget.orderNodes(); }, hear);
    QApplication::processEvents();
    CHECK(reads.size() == plan_reads + 1);
    CHECK(fixture.recorder.context_id != contexts);
    CHECK(ordered.isEmpty());
    answer();
    CHECK(ordered_graph == QString::fromStdString(fixture.graph));
    CHECK(generation == 5);
    QStringList names;
    for (const auto &node : fixture.wire.at("nodes"))
        names << QString::fromStdString(node.at("name").get<std::string>());
    CHECK(ordered == names);

    // Decision 2: Fix sockets on a pass a graph node draws: the copy is
    // written, and the Graph -- not edit_preset_pass -- moves the node onto it.
    QStringList moved;
    fixture.widget.whenRead(
        [&] {
            fixture.widget.fixSockets(QString::fromStdString(fixture.graph),
                                      QString::fromLatin1(Vi3aPass),
                                      [&](const QString &reference) {
                                          moved << reference;
                                          return true;
                                      });
        },
        hear);
    QApplication::processEvents();
    CHECK(moved.isEmpty());
    answer();
    CHECK(moved == QStringList{QStringLiteral("project://shaders/wp343_mixed_a")});
    CHECK(fixture.recorder.submits == 0);
    CHECK(vi3bRead(root / "shaders" / "wp343_mixed_a.frag").find("uniform sampler2D in_t_b;") !=
          std::string::npos);
    // What it said is the Graph's line too.
    REQUIRE_FALSE(said.isEmpty());
    CHECK(said.back().startsWith(QStringLiteral("Copied engine://passthrough to shaders/wp343_mixed_a.frag")));

    // Decision 6: the Frame Plan's entries say the Graph has the same.
    auto *order = fixture.widget.findChild<QPushButton *>(QStringLiteral("pelican.framePlanOrderNodes"));
    REQUIRE(order != nullptr);
    CHECK(order->toolTip().contains(QStringLiteral("in the Graph")));
    auto *fix = fixture.widget.findChild<QAction *>(QStringLiteral("pelican.framePlan.fixSockets"));
    REQUIRE(fix != nullptr);
    CHECK(fix->toolTip().contains(QStringLiteral("in the Graph")));
}
} // namespace PelicanStudio


namespace PelicanStudio {
TEST_CASE("Frame Plan drag stays attached across a crowded scene and rebuild",
          "[devstudio][frame-plan][drag][navigation-fix]") {
    (void)application();
    Json wire = capturedRuntimeFramePlan();
    wire["runtime_resolution"] = exampleFramePlan(true).at("runtime_resolution");
    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    targetSelector(widget).setCurrentText(QStringLiteral("Bloom_Threshold_RT"));
    subtreeDepth(widget).setValue(2);
    QApplication::processEvents();
    for (int generation = 0; generation < 2; ++generation) {
        auto &logical = scene(widget);
        auto *node = nodeItem(logical, "HorizontalBlur_0");
        auto *edge = dynamic_cast<QGraphicsPathItem *>(
            edgeItem(logical, "HighLuminanceExtraction", "HorizontalBlur_0"));
        REQUIRE(node != nullptr);
        REQUIRE(edge != nullptr);
        // Labels/decorations make repeated full-scene endpoint searches costly.
        for (int i = 0; i < 1500; ++i)
            logical.addRect(i % 100, i / 100, 1, 1);
        const auto start = node->pos();
        QElapsedTimer timer;
        timer.start();
        for (int step = 1; step <= 80; ++step) {
            const auto previous = edge->path();
            node->setPos(start + QPointF{step * 2.0, step * 1.0});
            REQUIRE(edge->path() != previous);
        }
        if (qEnvironmentVariableIsSet("PELICAN_STUDIO_DRAG_BENCHMARK"))
            WARN("Frame Plan: 80 drag updates " << timer.nsecsElapsed() / 1e6 << " ms");
        QApplication::processEvents();
        // Rebuild replaces all items, including identically named endpoints.
        subtreeDepth(widget).setValue(0);
        subtreeDepth(widget).setValue(2);
        QApplication::processEvents();
    }
}

} // namespace PelicanStudio

namespace PelicanStudio {
TEST_CASE("Frame Plan mouse drag preserves the grabbed screen point after event delivery",
          "[devstudio][frame-plan][drag][input-latency]") {
    (void)application();
    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.resize(1200, 800);
    widget.show();
    Json wire = capturedRuntimeFramePlan();
    wire["runtime_resolution"] = exampleFramePlan(true).at("runtime_resolution");
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    targetSelector(widget).setCurrentText(QStringLiteral("Bloom_Threshold_RT"));
    subtreeDepth(widget).setValue(2);
    QApplication::processEvents();
    auto &logical = scene(widget);
    auto &view = logicalView(widget);
    const auto nodes = itemsOfKind(logical, FramePlanNodeItem);
    REQUIRE_FALSE(nodes.empty());
    auto *node = *std::ranges::max_element(nodes, {}, [](const QGraphicsItem *item) {
        return item->sceneBoundingRect().bottom();
    });
    REQUIRE(node != nullptr);
    // A fitted canvas is narrower than its viewport on one axis. Growing
    // its bounds must not recenter it underneath the held mouse button.
    view.setSceneRect(QRectF{});
    view.fitInView(logical.itemsBoundingRect().adjusted(-30, -30, 30, 30), Qt::KeepAspectRatio);
    QApplication::processEvents();
    auto *port = view.viewport();
    const QPointF grab{60, 15};
    const QPoint press = view.mapFromScene(node->mapToScene(grab));
    REQUIRE(port->rect().contains(press));
    const auto send = [&](QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent event{type, QPointF{at}, QPointF{at}, port->mapToGlobal(QPointF{at}), button, held, Qt::NoModifier};
        QApplication::sendEvent(port, &event);
        QApplication::processEvents();
    };
    send(QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
    const QPointF offset = QPointF{press} - QPointF{view.mapFromScene(node->mapToScene(grab))};
    double worst = 0;
    for (int step = 1; step <= 20; ++step) {
        const QPoint at = press + QPoint{step * 2, step * 4};
        send(QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton);
        const QPointF shown = QPointF{view.mapFromScene(node->mapToScene(grab))} + offset;
        worst = std::max(worst, QLineF{shown, QPointF{at}}.length());
    }
    send(QEvent::MouseButtonRelease, press + QPoint{40, 80}, Qt::LeftButton, Qt::NoButton);
    CHECK(worst <= 2.0);
}
} // namespace PelicanStudio


namespace PelicanStudio {

TEST_CASE("FX-5 Render Undo and Redo notices survive a late frame plan reply",
          "[devstudio][frame-plan][fx5]") {
    application();
    const bool reply_first = GENERATE(false, true);
    const bool hidden = GENERATE(false, true);
    const auto line = GENERATE(QStringLiteral("Undid \"Set shader_defines\"."),
                               QStringLiteral("Redid \"Set shader_defines\"."));
    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    qint64 request = 9000;
    widget.setPlanRequest([&](QString *) { return ++request; });
    widget.resize(900, 600);
    if (!hidden) widget.show();
    QApplication::processEvents();
    auto *status = widget.findChild<QLabel *>(QStringLiteral("pelican.framePlanStatus"));
    REQUIRE(status != nullptr);
    const auto answer = [&] {
        Q_EMIT viewport.inspectorRpcSucceeded(
            request, QByteArray::fromStdString(mixedEditingWire().dump()));
        QApplication::processEvents();
    };

    // MainWindow::renderDocumentsRestored starts this read; the history's
    // changed notification then names the completed Undo / Redo. Exercise
    // both delivery orders without sleeps or a running engine.
    widget.refresh();
    if (reply_first) answer();
    widget.showRenderHistoryLine(line);
    if (!reply_first) answer();
    CHECK(status->text() == line);
    widget.show();
    QApplication::processEvents();
    CHECK(status->isVisible());
    CHECK(status->text() == line);

    // A second history operation replaces the first, including a read
    // already in flight when another commit queues one more snapshot.
    widget.refresh();
    widget.planChangedByCommit();
    const auto next_line = QStringLiteral("Undid \"Set graph_transforms\".");
    widget.showRenderHistoryLine(next_line);
    const auto previous_request = request;
    answer();
    REQUIRE(request == previous_request + 1);
    answer();
    CHECK(status->text() == next_line);

    // A failed refresh still reports its error, not an old success.
    widget.refresh();
    Q_EMIT viewport.inspectorRpcFailed(request, QStringLiteral("fixture read failure"));
    CHECK(status->text().contains(QStringLiteral("fixture read failure")));

    // Disconnection / switching projects must not resurrect the old notice.
    widget.setPlanRequest({});
    widget.refresh();
    widget.receiveResult(QByteArray::fromStdString(mixedEditingWire().dump()));
    CHECK_FALSE(status->text().contains(QStringLiteral("Undid")));
}

} // namespace PelicanStudio
