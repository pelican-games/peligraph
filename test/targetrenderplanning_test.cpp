#include "rc7c_test_facts.hpp"
#include "../src/devstudio/model/frameplanpolicy.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/generationresources.hpp"
#include "../src/project/renderpipeline.hpp"
#include <type_traits>
#include "../src/project/targetrenderplanning.hpp"
#include "../src/project/targetrendermemory.hpp"
#include "../src/project/physicaltargetplanwire.hpp"
#include "../src/project/vulkancompletephysicalplan.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

constexpr std::string_view kProvider =
    "pelican.test.vulkan_target_lowering@1";
constexpr std::string_view kHdrFormatCapability =
    "pelican.vulkan.format_rgba16_sfloat@1";
constexpr std::string_view kDisplayFormatCapability =
    "pelican.vulkan.format_bgra8_unorm@1";
constexpr std::string_view kDepthFormatCapability =
    "pelican.vulkan.format_d32_sfloat@1";
constexpr std::string_view kLdrFormatCapability =
    "pelican.vulkan.format_rgba8_unorm@1";

void requireThrowsContaining(const std::function<void()> &operation,
                             std::string_view expected) {
    try {
        operation();
        FAIL("operation did not throw");
    } catch (const std::runtime_error &error) {
        REQUIRE(std::string_view{error.what()}.find(expected) !=
                std::string_view::npos);
    }
}

CompilerProviderRegistrySnapshot providers() {
    static CompilerProviderRegistry registry;
    static const bool registered = [] {
        registry.registerProvider(CompilerProviderDescriptor{
            std::string{kProvider},
            CompilerProviderKind::target_lowering,
            0,
            0,
            "builtin:test-target-lowering-v1",
        });
        return true;
    }();
    (void)registered;
    return registry.snapshot();
}

TargetTopologySnapshot topology(
    bool tile, std::uint32_t budget = 8,
    std::uint32_t max_multiview_view_count = 0,
    const XrMultiviewDeviceIdentity *device = nullptr) {
    std::vector<std::string> capabilities{
        "pelican.vulkan.graphics@1",
        "pelican.vulkan.sampled_image@1",
        "pelican.vulkan.storage_buffer@1",
        "pelican.vulkan.transfer_copy@1",
        std::string{kHdrFormatCapability},
        std::string{kDisplayFormatCapability},
        std::string{kDepthFormatCapability},
        std::string{kLdrFormatCapability},
    };
    if (tile) {
        capabilities.insert(
            capabilities.end(),
            {"pelican.vulkan.tile_based@1",
             "pelican.vulkan.dynamic_rendering_local_read@1",
             "pelican.vulkan.transient_attachment@1"});
    }
    std::vector<TargetFact> facts{
        {"pelican.vulkan.max_color_attachments@1",
         std::to_string(budget)},
        {"pelican.vulkan.profile@1",
         tile ? "tile" : "desktop"},
    };
    if (max_multiview_view_count != 0) {
        capabilities.push_back(
            "pelican.vulkan.multiview@1");
        facts.push_back(
            {"pelican.vulkan.max_multiview_view_count@1",
             std::to_string(max_multiview_view_count)});
    }
    if (device != nullptr) {
        facts.insert(
            facts.end(),
            {
                {std::string{vulkanVendorIdFact},
                 std::to_string(device->vendor_id)},
                {std::string{vulkanDeviceIdFact},
                 std::to_string(device->device_id)},
                {std::string{vulkanDriverVersionFact},
                 std::to_string(
                     device->driver_version)},
                {std::string{vulkanDeviceNameFact},
                 device->device_name},
            });
    }
    return TargetTopologySnapshot{
        tile ? "mock_tile" : "mock_desktop",
        {TargetEndpoint{
            "device:0",
            TargetEndpointKind::vulkan_device,
            std::move(capabilities),
            std::move(facts),
        }},
        {},
    };
}

LogicalPortContract port(const LogicalTypeRegistry &types,
                         std::string name,
                         LogicalPortDirection direction,
                         const LogicalType &type) {
    return LogicalPortContract{
        std::move(name), direction,
        exactLogicalTypePattern(types, type), {}};
}

void addWrite(const LogicalTypeRegistry &types, LogicalGraphNode &node,
              const LogicalResourceDesc &resource,
              std::uint32_t version, std::string port_name,
              LogicalAccessIntent intent =
                  LogicalAccessIntent::attachment) {
    node.ports.push_back(port(types, port_name,
                              LogicalPortDirection::output,
                              resource.type));
    node.uses.push_back(makeLogicalWriteUse(
        std::move(port_name),
        LogicalValueId{resource.name, version}, intent));
}

void addRead(const LogicalTypeRegistry &types, LogicalGraphNode &node,
             const LogicalResourceDesc &resource,
             std::uint32_t version, std::string port_name,
             LogicalReadFootprintKind footprint,
             LogicalAccessIntent intent =
                 LogicalAccessIntent::sampled) {
    node.ports.push_back(port(types, port_name,
                              LogicalPortDirection::input,
                              resource.type));
    node.uses.push_back(makeLogicalReadUse(
        std::move(port_name),
        LogicalValueId{resource.name, version},
        LogicalReadFootprint{footprint, std::nullopt}, intent));
}

void addReadWrite(const LogicalTypeRegistry &types,
                  LogicalGraphNode &node,
                  const LogicalResourceDesc &resource,
                  std::uint32_t input_version,
                  std::uint32_t output_version,
                  std::string port_name) {
    node.ports.push_back(port(types, port_name,
                              LogicalPortDirection::input_output,
                              resource.type));
    node.uses.push_back(makeLogicalReadWriteUse(
        std::move(port_name),
        LogicalValueId{resource.name, input_version},
        LogicalValueId{resource.name, output_version},
        LogicalReadFootprint{
            LogicalReadFootprintKind::same_pixel, std::nullopt},
        LogicalAccessIntent::attachment));
}

const LogicalResourceDesc &resource(
    const CompiledLogicalRenderGraph &graph, std::string_view name) {
    const auto found = std::find_if(
        graph.resources.begin(), graph.resources.end(),
        [&](const LogicalResourceDesc &entry) {
            return entry.name == name;
        });
    if (found == graph.resources.end()) {
        throw std::runtime_error("test resource missing");
    }
    return *found;
}

CompiledLogicalRenderGraph hybridGraph(
    const LogicalTypeRegistry &types, std::size_t gbuffer_count,
    bool refraction, bool shared_depth = false) {
    CompiledLogicalRenderGraph graph;
    graph.name = refraction ? "hybrid_refraction"
                            : "hybrid_base";
    const auto scene = sceneLinearHdrV1(types);
    const auto display = displayEncodedV1(types);
    for (std::size_t index = 0; index < gbuffer_count; ++index) {
        const auto name =
            index + 1 == gbuffer_count && gbuffer_count > 5
                ? "gbuffer_custom_attribute"
                : "gbuffer_" + std::to_string(index);
        graph.resources.push_back(
            LogicalResourceDesc{.name = name, .type = scene});
    }
    if (shared_depth) {
        graph.resources.push_back(LogicalResourceDesc{
            .name = "scene_depth",
            .type = deviceDepthV1(types),
        });
    }
    graph.resources.push_back(
        LogicalResourceDesc{.name = "scene_color", .type = scene});
    if (refraction) {
        graph.resources.push_back(LogicalResourceDesc{
            .name = "opaque_snapshot", .type = scene});
    }
    graph.resources.push_back(LogicalResourceDesc{
        .name = "display_output",
        .type = display,
        .materialization =
            LogicalMaterializationRequirement::required,
    });
    graph.resources.push_back(LogicalResourceDesc{
        .name = "quality_intent",
        .type = legacyOpaqueResourceV1(types),
    });

    LogicalGraphNode gbuffer;
    gbuffer.name = "GBuffer";
    gbuffer.kind = LogicalGraphNodeKind::render;
    gbuffer.region_tags = {"region.opaque"};
    for (std::size_t index = 0; index < gbuffer_count; ++index) {
        const auto &entry = graph.resources[index];
        addWrite(types, gbuffer, entry, 1,
                 "out_" + std::to_string(index));
    }
    if (shared_depth) {
        addWrite(types, gbuffer, resource(graph, "scene_depth"), 1,
                 "depth");
    }

    LogicalGraphNode lighting;
    lighting.name = "Lighting";
    lighting.kind = LogicalGraphNodeKind::render;
    lighting.region_tags = {"region.deferred_lighting"};
    for (std::size_t index = 0; index < gbuffer_count; ++index) {
        const auto &entry = graph.resources[index];
        addRead(types, lighting, entry, 1,
                "in_" + std::to_string(index),
                LogicalReadFootprintKind::same_pixel);
    }
    addWrite(types, lighting, resource(graph, "scene_color"), 1,
             "scene_out");

    LogicalGraphNode forward;
    forward.name = "Forward";
    forward.kind = LogicalGraphNodeKind::render;
    forward.region_tags = {"region.forward_opaque"};
    addReadWrite(types, forward, resource(graph, "scene_color"), 1, 2,
                 "scene");
    if (shared_depth) {
        addReadWrite(types, forward, resource(graph, "scene_depth"), 1, 2,
                     "depth");
    }

    graph.nodes = {std::move(forward), std::move(lighting),
                   std::move(gbuffer)};

    std::uint32_t final_scene_version = 2;
    if (refraction) {
        LogicalGraphNode snapshot;
        snapshot.name = "OpaqueSnapshot";
        snapshot.kind = LogicalGraphNodeKind::snapshot_copy;
        snapshot.region_tags = {"region.transparency"};
        addRead(types, snapshot, resource(graph, "scene_color"), 2,
                "source", LogicalReadFootprintKind::arbitrary,
                LogicalAccessIntent::transfer);
        addWrite(types, snapshot, resource(graph, "opaque_snapshot"), 1,
                 "snapshot", LogicalAccessIntent::transfer);

        LogicalGraphNode refract;
        refract.name = "Refraction";
        refract.kind = LogicalGraphNodeKind::render;
        refract.region_tags = {"region.transparency.water"};
        addRead(types, refract, resource(graph, "opaque_snapshot"), 1,
                "opaque",
                LogicalReadFootprintKind::neighborhood);
        addReadWrite(types, refract, resource(graph, "scene_color"), 2, 3,
                     "scene");
        graph.nodes.push_back(std::move(refract));
        graph.nodes.push_back(std::move(snapshot));
        final_scene_version = 3;
    }

    LogicalGraphNode tone_map;
    tone_map.name = "ToneMap";
    tone_map.kind = LogicalGraphNodeKind::output_transform;
    tone_map.region_tags = {"region.post.tonemap"};
    addRead(types, tone_map, resource(graph, "scene_color"),
            final_scene_version, "scene",
            LogicalReadFootprintKind::same_pixel);
    addWrite(types, tone_map, resource(graph, "display_output"), 1,
             "display");
    graph.nodes.push_back(std::move(tone_map));
    validateCompiledLogicalRenderGraph(types, graph);
    return graph;
}

CompiledLogicalRenderGraph hybridGraphWithSharedShadow(
    const LogicalTypeRegistry &types) {
    auto graph = hybridGraph(types, 3, false);
    graph.name = "hybrid_shared_shadow";
    graph.resources.push_back(LogicalResourceDesc{
        .name = "directional_shadow",
        .type = deviceDepthV1(types),
    });

    LogicalGraphNode shadow;
    shadow.name = "ShadowDepth";
    shadow.kind = LogicalGraphNodeKind::render;
    shadow.region_tags = {"region.shadow"};
    shadow.view_family =
        std::string{
            directionalShadowRenderViewFamilyId};
    addWrite(
        types, shadow,
        resource(graph, "directional_shadow"), 1,
        "shadow_depth");

    for (auto &node : graph.nodes) {
        if (node.name != "Lighting" &&
            node.name != "Forward") {
            continue;
        }
        addRead(
            types, node,
            resource(graph, "directional_shadow"), 1,
            "directional_shadow",
            LogicalReadFootprintKind::arbitrary);
    }
    graph.nodes.push_back(std::move(shadow));
    validateCompiledLogicalRenderGraph(types, graph);
    return graph;
}

CompiledLogicalRenderGraph
hybridGraphWithSecondaryInternalResource(
    const LogicalTypeRegistry &types) {
    auto graph = hybridGraph(types, 3, false);
    graph.name =
        "hybrid_secondary_internal";
    const auto scene =
        sceneLinearHdrV1(types);
    graph.resources.push_back(
        LogicalResourceDesc{
            .name =
                "capture_intermediate",
            .type = scene,
        });
    graph.resources.push_back(
        LogicalResourceDesc{
            .name = "capture_output",
            .type = scene,
        });

    LogicalGraphNode prepare;
    prepare.name = "CapturePrepare";
    prepare.kind =
        LogicalGraphNodeKind::render;
    prepare.view_family =
        "$capture/test";
    addWrite(
        types, prepare,
        resource(
            graph,
            "capture_intermediate"),
        1, "intermediate");

    LogicalGraphNode resolve;
    resolve.name = "CaptureResolve";
    resolve.kind =
        LogicalGraphNodeKind::render;
    resolve.view_family =
        "$capture/test";
    addRead(
        types, resolve,
        resource(
            graph,
            "capture_intermediate"),
        1, "intermediate",
        LogicalReadFootprintKind::
            same_pixel);
    addWrite(
        types, resolve,
        resource(
            graph, "capture_output"),
        1, "capture");

    for (auto &node : graph.nodes) {
        if (node.name != "Lighting") {
            continue;
        }
        addRead(
            types, node,
            resource(
                graph,
                "capture_output"),
            1, "capture",
            LogicalReadFootprintKind::
                arbitrary);
    }
    graph.nodes.push_back(
        std::move(resolve));
    graph.nodes.push_back(
        std::move(prepare));
    validateCompiledLogicalRenderGraph(
        types, graph);
    return graph;
}

ResourcePattern patternFor(const LogicalTypeRegistry &types,
                           const LogicalResourceDesc &resource) {
    const auto display =
        resource.type == displayEncodedV1(types);
    const auto depth =
        resource.type == deviceDepthV1(types);
    return ResourcePattern{
        .id = display
                  ? "pelican.render.display_output_pattern@1"
              : depth
                  ? "pelican.render.device_depth_pattern@1"
                  : "pelican.render.frame_color_pattern@1",
        .applicable_type =
            exactLogicalTypePattern(types, resource.type),
        .format_candidates =
            display
                ? std::vector<ResourceFormatCandidate>{
                      {ImageFormat::b8g8r8a8_unorm,
                       {std::string{
                           kDisplayFormatCapability}}},
                  }
            : depth
                ? std::vector<ResourceFormatCandidate>{
                      {ImageFormat::d32_sfloat,
                       {std::string{
                           kDepthFormatCapability}}},
                  }
                : std::vector<ResourceFormatCandidate>{
                      {ImageFormat::r16g16b16a16_sfloat,
                       {std::string{
                           kHdrFormatCapability}}},
                      {ImageFormat::r8g8b8a8_unorm,
                       {std::string{
                           kLdrFormatCapability}}},
                  },
        .prefer_transient = !display,
        .allow_tile_local = !display,
        .allow_alias = !display,
        .require_store = display,
        .local_read_fallback =
            ResourcePatternFallback::materialize,
        .estimated_bytes =
            display ? 4ULL * 1024ULL * 1024ULL
            : depth ? 4ULL * 1024ULL * 1024ULL
                    : 8ULL * 1024ULL * 1024ULL,
        .provenance =
            display ? "builtin:display-v1"
            : depth ? "builtin:device-depth-v1"
                    : "builtin:frame-color-v1",
    };
}

std::vector<ResourcePatternBinding> bindingsFor(
    const LogicalTypeRegistry &types,
    const CompiledLogicalRenderGraph &graph) {
    std::vector<ResourcePatternBinding> result;
    for (const auto &entry : graph.resources) {
        if (entry.type.constructor != LogicalTypeConstructor::image &&
            entry.type.constructor != LogicalTypeConstructor::buffer) {
            continue;
        }
        result.push_back(
            {entry.name, patternFor(types, entry)});
    }
    return result;
}

// These fixtures deliberately store versioned logical nodes out of execution
// order. Their legal orders are explicit; no conversion to unversioned IR.
FramePlanNodeOrder fixtureOrder(const CompiledLogicalRenderGraph &graph) {
    if (graph.name == "alias_lifetimes")
        return {graph, {"WriteA", "ReadA", "WriteB", "ReadB"}};
    if (graph.name.starts_with("tile_link_materialized_"))
        return {graph, {"Producer", "Consumer", "Output"}};
    std::vector<std::string> order;
    for (const auto *name : {"CapturePrepare", "CaptureResolve", "GBuffer", "ShadowDepth",
                             "Lighting", "Forward", "OpaqueSnapshot", "Refraction", "Scratch", "ToneMap"}) {
        if (std::any_of(graph.nodes.begin(), graph.nodes.end(),
                        [&](const auto &node) { return node.name == name; }))
            order.emplace_back(name);
    }
    return {graph, std::move(order)};
}

VulkanTargetPlan compile(
    const LogicalTypeRegistry &types,
    const CompiledLogicalRenderGraph &graph,
    const FramePlanNodeOrder &node_order,
    TargetTopologySnapshot target,
    std::vector<ResourcePatternBinding> bindings,
    std::optional<VulkanSampleCountPlanRequest> sample_count =
        std::nullopt,
    std::optional<VulkanViewExecutionPlanRequest> view_execution =
        std::nullopt,
    std::optional<VulkanExternalDepthExportRequest>
        external_depth_export = std::nullopt,
    std::vector<VulkanPhysicalAttachmentPlan>
        attachments = {},
    PlanningProfile profile = {}) {
    std::map<std::string, ImageUsage, std::less<>> usage;
    if (graph.name == "alias_lifetimes") {
        for (const auto &r : graph.resources) usage[r.name] = ImageUsageBit::color_attachment | ImageUsageBit::sampled;
        if (attachments.empty()) for (const auto &node : graph.nodes) for (const auto &u : node.uses)
            if (u.output_value && u.intent == LogicalAccessIntent::attachment)
                attachments.push_back({.node = node.name, .logical_resource = u.output_value->resource,
                    .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::store}});
    }
    return compileVulkanTargetPlan(
        types, graph, target, providers(),
        VulkanTargetPlanRequest{
            .node_order = node_order,
            .endpoint = "device:0",
            .provider = std::string{kProvider},
            .pattern_bindings = std::move(bindings),
            .effective_image_usage = std::move(usage),
            .memory_requirements = TestSupport::rc7cMemoryRequirements,
            .profile = profile,
            .sample_count = std::move(sample_count),
            .view_execution = std::move(view_execution),
            .external_depth_export =
                std::move(external_depth_export),
            .automatic_attachments =
                std::move(attachments),
        });
}

CompiledLogicalRenderGraph graphWithWriteOnlyAttachment(
    const LogicalTypeRegistry &types) {
    auto graph = hybridGraph(types, 3, false);
    graph.name = "transient_attachment";
    graph.resources.push_back(
        LogicalResourceDesc{
            .name = "write_only_scratch",
            .type = sceneLinearHdrV1(types),
        });
    LogicalGraphNode scratch;
    scratch.name = "Scratch";
    scratch.kind = LogicalGraphNodeKind::render;
    addWrite(
        types, scratch,
        resource(graph, "write_only_scratch"),
        1, "scratch");
    graph.nodes.push_back(std::move(scratch));
    validateCompiledLogicalRenderGraph(types, graph);
    return graph;
}

CompiledLogicalRenderGraph
graphWithTileLinkAndMaterializedOverwrite(
    const LogicalTypeRegistry &types,
    bool preserve_materialized_contents) {
    CompiledLogicalRenderGraph graph;
    graph.name =
        preserve_materialized_contents
            ? "tile_link_materialized_load"
            : "tile_link_materialized_clear";
    const auto scene = sceneLinearHdrV1(types);
    graph.resources = {
        LogicalResourceDesc{
            .name = "tile_link",
            .type = scene,
        },
        LogicalResourceDesc{
            .name = "persistent_color",
            .type = scene,
            .materialization =
                LogicalMaterializationRequirement::required,
        },
        LogicalResourceDesc{
            .name = "display_output",
            .type = displayEncodedV1(types),
            .materialization =
                LogicalMaterializationRequirement::required,
        },
    };

    LogicalGraphNode producer;
    producer.name = "Producer";
    producer.kind = LogicalGraphNodeKind::render;
    addWrite(types, producer, resource(graph, "tile_link"),
             1, "tile");
    addWrite(types, producer,
             resource(graph, "persistent_color"),
             1, "color");

    LogicalGraphNode consumer;
    consumer.name = "Consumer";
    consumer.kind = LogicalGraphNodeKind::render;
    addRead(types, consumer, resource(graph, "tile_link"),
            1, "tile", LogicalReadFootprintKind::same_pixel,
            LogicalAccessIntent::attachment);
    if (preserve_materialized_contents) {
        addReadWrite(
            types, consumer,
            resource(graph, "persistent_color"),
            1, 2, "color");
    } else {
        addWrite(
            types, consumer,
            resource(graph, "persistent_color"),
            2, "color");
    }

    LogicalGraphNode output;
    output.name = "Output";
    output.kind =
        LogicalGraphNodeKind::output_transform;
    addRead(types, output,
            resource(graph, "persistent_color"),
            2, "color",
            LogicalReadFootprintKind::same_pixel);
    addWrite(types, output,
             resource(graph, "display_output"),
             1, "display");

    graph.nodes = {
        std::move(consumer),
        std::move(output),
        std::move(producer),
    };
    validateCompiledLogicalRenderGraph(types, graph);
    return graph;
}

std::vector<VulkanPhysicalAttachmentPlan>
tileLinkAttachmentOperations(
    bool preserve_materialized_contents) {
    return {
        VulkanPhysicalAttachmentPlan{
            .node = "Producer",
            .logical_resource = "tile_link",
            .aspect =
                AttachmentAspect::color,
            .operations =
                PassAttachmentOperations{
                    AttachmentLoadOp::clear,
                    AttachmentStoreOp::store},
        },
        VulkanPhysicalAttachmentPlan{
            .node = "Producer",
            .logical_resource = "persistent_color",
            .aspect =
                AttachmentAspect::color,
            .operations =
                PassAttachmentOperations{
                    AttachmentLoadOp::clear,
                    preserve_materialized_contents ? AttachmentStoreOp::store : AttachmentStoreOp::dont_care},
        },
        VulkanPhysicalAttachmentPlan{
            .node = "Consumer",
            .logical_resource = "persistent_color",
            .aspect =
                AttachmentAspect::color,
            .operations =
                PassAttachmentOperations{
                    preserve_materialized_contents ? AttachmentLoadOp::load : AttachmentLoadOp::clear,
                    AttachmentStoreOp::store},
        },
        VulkanPhysicalAttachmentPlan{
            .node = "Output",
            .logical_resource = "display_output",
            .aspect =
                AttachmentAspect::color,
            .operations =
                PassAttachmentOperations{
                    AttachmentLoadOp::clear,
                    AttachmentStoreOp::store},
        },
    };
}

VulkanSampleCountPlanRequest sampleCountRequest(
    const CompiledLogicalRenderGraph &graph,
    SampleCountPolicy policy,
    std::string_view limited_resource = {},
    std::vector<std::string> geometry_nodes =
        {"GBuffer", "Forward"}) {
    std::vector<SampleCountResourceCapability> capabilities;
    for (const auto &entry : graph.resources) {
        if (entry.type.constructor != LogicalTypeConstructor::image ||
            entry.name == "display_output" ||
            entry.name == "opaque_snapshot") {
            continue;
        }
        capabilities.push_back({
            .resource = entry.name,
            .format =
                semanticTypeIdName(
                    entry.type.semantic) ==
                        "pelican.render.depth@1"
                    ? ImageFormat::d32_sfloat
                    : ImageFormat::r16g16b16a16_sfloat,
            .supported_samples =
                entry.name == limited_resource
                    ? std::vector<std::uint32_t>{1, 2}
                    : std::vector<std::uint32_t>{1, 2, 4},
        });
    }
    return {
        .policy = std::move(policy),
        .capabilities = std::move(capabilities),
        .geometry_nodes = std::move(geometry_nodes),
    };
}

const VulkanPhysicalResourcePlan &physicalResource(
    const VulkanTargetPlan &plan, std::string_view name) {
    const auto found = std::find_if(
        plan.resources.begin(), plan.resources.end(),
        [&](const VulkanPhysicalResourcePlan &entry) {
            return entry.logical_resource == name;
        });
    if (found == plan.resources.end()) {
        throw std::runtime_error("test physical resource missing");
    }
    return *found;
}

bool oneScopeContains(const VulkanTargetPlan &plan,
                      std::string_view first,
                      std::string_view second) {
    return std::any_of(
        plan.scopes.begin(), plan.scopes.end(),
        [&](const VulkanPhysicalScopePlan &scope) {
            return std::find(scope.nodes.begin(), scope.nodes.end(),
                             first) != scope.nodes.end() &&
                   std::find(scope.nodes.begin(), scope.nodes.end(),
                             second) != scope.nodes.end();
        });
}

const VulkanPhysicalScopePlan &scopeForNode(
    const VulkanTargetPlan &plan, std::string_view node) {
    const auto found = std::find_if(
        plan.scopes.begin(), plan.scopes.end(),
        [&](const VulkanPhysicalScopePlan &scope) {
            return std::find(scope.nodes.begin(),
                             scope.nodes.end(),
                             node) != scope.nodes.end();
        });
    if (found == plan.scopes.end()) {
        throw std::runtime_error("test physical scope missing");
    }
    return *found;
}

} // namespace

TEST_CASE("desktop materializes arbitrary G-buffer attachments while tile keeps same-pixel data local",
          "[target-render-planning][desktop][tile][gbuffer]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 6, false, true);
    const auto canonical_before =
        compiledLogicalRenderGraphToJson(graph).dump();
    auto bindings = bindingsFor(types, graph);

    const auto desktop =
        compile(types, graph, fixtureOrder(graph), topology(false), bindings);
    REQUIRE(desktop.backend_selection.selected_candidate ==
            "pelican.vulkan.materialized_plan@1");
    for (std::size_t index = 0; index < 6; ++index) {
        const auto name =
            index == 5 ? "gbuffer_custom_attribute"
                       : "gbuffer_" + std::to_string(index);
        REQUIRE(physicalResource(desktop, name).representation ==
                VulkanResourceRepresentation::materialized_image);
    }

    const auto tile = compile(types, graph, fixtureOrder(graph), topology(true), bindings);
    REQUIRE(tile.backend_selection.selected_candidate ==
            "pelican.vulkan.tile_local_plan@1");
    for (std::size_t index = 0; index < 6; ++index) {
        const auto name =
            index == 5 ? "gbuffer_custom_attribute"
                       : "gbuffer_" + std::to_string(index);
        REQUIRE(physicalResource(tile, name).representation ==
                VulkanResourceRepresentation::tile_local_attachment);
    }
    REQUIRE(oneScopeContains(tile, "GBuffer", "Lighting"));
    REQUIRE(oneScopeContains(tile, "Lighting", "Forward"));
    REQUIRE(std::none_of(
        tile.resources.begin(), tile.resources.end(),
        [](const VulkanPhysicalResourcePlan &entry) {
            return entry.logical_resource == "quality_intent";
        }));
    REQUIRE(compiledLogicalRenderGraphToJson(graph).dump() ==
            canonical_before);

    std::reverse(bindings.begin(), bindings.end());
    const auto reordered =
        compile(types, graph, fixtureOrder(graph), topology(true), std::move(bindings));
    REQUIRE(vulkanTargetPlanToJson(tile).dump() ==
            vulkanTargetPlanToJson(reordered).dump());
}

TEST_CASE(
    "tile-local fusion preserves materialized attachment operations",
    "[target-render-planning][tile][attachment][synchronization]") {
    const auto types = makeBuiltinLogicalTypeRegistry();

    const auto preserving_graph =
        graphWithTileLinkAndMaterializedOverwrite(
            types, true);
    const auto preserving = compile(
        types, preserving_graph, fixtureOrder(preserving_graph), topology(true),
        bindingsFor(types, preserving_graph),
        std::nullopt, std::nullopt, std::nullopt,
        tileLinkAttachmentOperations(true));
    REQUIRE(
        preserving.backend_selection
            .selected_candidate ==
        "pelican.vulkan.tile_local_plan@1");
    REQUIRE(oneScopeContains(
        preserving, "Producer", "Consumer"));
    REQUIRE(
        physicalResource(
            preserving, "tile_link")
            .representation ==
        VulkanResourceRepresentation::
            tile_local_attachment);

    const auto clearing_graph =
        graphWithTileLinkAndMaterializedOverwrite(
            types, false);
    const auto clearing = compile(
        types, clearing_graph, fixtureOrder(clearing_graph), topology(true),
        bindingsFor(types, clearing_graph),
        std::nullopt, std::nullopt, std::nullopt,
        tileLinkAttachmentOperations(false));
    REQUIRE(
        clearing.backend_selection
            .selected_candidate ==
        "pelican.vulkan.materialized_plan@1");
    REQUIRE_FALSE(oneScopeContains(
        clearing, "Producer", "Consumer"));
    REQUIRE(
        physicalResource(
            clearing, "tile_link")
            .representation ==
        VulkanResourceRepresentation::
            materialized_image);
}

// LP-Va: scope fusion is the only place two logical nodes share one physical
// scope, and the scheduler runs a scope on a single view set. A tile target
// fuses GBuffer and Lighting; a different view_select on Lighting must keep
// them apart, and the selection must reach the scope, its JSON, and the
// logical fingerprint.
TEST_CASE("tile fusion keeps nodes with different view_select apart",
          "[target-render-planning][tile][view-select][lpva]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);
    const auto baseline = compile(
        types, graph, fixtureOrder(graph), topology(true), bindingsFor(types, graph));
    REQUIRE(oneScopeContains(baseline, "GBuffer", "Lighting"));

    auto selected_graph = graph;
    for (auto &node : selected_graph.nodes) {
        if (node.name == "Lighting") {
            node.view_select = {{"face", 1}};
        }
    }
    const auto selected = compile(
        types, selected_graph, fixtureOrder(selected_graph), topology(true),
        bindingsFor(types, selected_graph));
    CHECK_FALSE(oneScopeContains(selected, "GBuffer", "Lighting"));
    CHECK(scopeForNode(selected, "Lighting").view_select ==
          RenderViewSelect{{"face", 1}});
    CHECK(scopeForNode(selected, "GBuffer").view_select.empty());
    CHECK(selected.logical_graph_fingerprint !=
          baseline.logical_graph_fingerprint);

    const auto selected_json = vulkanTargetPlanToJson(selected);
    const auto baseline_json = vulkanTargetPlanToJson(baseline);
    bool reported = false;
    for (const auto &scope : selected_json.at("scopes")) {
        if (scope.contains("view_select")) {
            CHECK(scope.at("view_select") ==
                  nlohmann::ordered_json{{"face", 1}});
            reported = true;
        }
    }
    CHECK(reported);
    for (const auto &scope : baseline_json.at("scopes")) {
        CHECK_FALSE(scope.contains("view_select"));
    }
}

TEST_CASE(
    "transient candidate elides write-only attachment storage without requiring local reads",
    "[target-render-planning][transient][attachment]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph =
        graphWithWriteOnlyAttachment(types);
    auto transient_target = topology(false);
    transient_target.endpoints.front()
        .capabilities.push_back(
            "pelican.vulkan.transient_attachment@1");
    const std::vector attachments{
        VulkanPhysicalAttachmentPlan{
            .node = "Scratch",
            .logical_resource =
                "write_only_scratch",
            .aspect =
                AttachmentAspect::color,
            .operations =
                PassAttachmentOperations{
                    AttachmentLoadOp::clear,
                    AttachmentStoreOp::store},
        },
    };

    const auto automatic = compile(
        types, graph, fixtureOrder(graph), transient_target,
        bindingsFor(types, graph),
        std::nullopt, std::nullopt, std::nullopt,
        attachments);
    REQUIRE(
        automatic.backend_selection
            .selected_candidate ==
        "pelican.vulkan.transient_plan@1");
    REQUIRE(
        physicalResource(
            automatic, "write_only_scratch")
            .representation ==
        VulkanResourceRepresentation::
            transient_attachment);
    REQUIRE(
        automatic.attachments.front().operations.store_op ==
        AttachmentStoreOp::dont_care);
    REQUIRE(std::any_of(
        automatic.decisions.begin(),
        automatic.decisions.end(),
        [](const auto &decision) {
            return decision.id ==
                   "pelican.plan.transient_attachment_store_elided@1";
        }));
    REQUIRE(std::none_of(
        automatic.backend_selection.candidates.begin(),
        automatic.backend_selection.candidates.end(),
        [](const auto &candidate) {
            return candidate.candidate ==
                       "pelican.vulkan.tile_local_plan@1" &&
                   candidate.feasible;
        }));

    const auto conservative = compile(
        types, graph, fixtureOrder(graph), transient_target,
        bindingsFor(types, graph),
        std::nullopt, std::nullopt, std::nullopt,
        attachments,
        PlanningProfile{
            PlanningProfileKind::
                conservative_debug,
            0});
    REQUIRE(
        conservative.backend_selection
            .selected_candidate ==
        "pelican.vulkan.materialized_plan@1");
    REQUIRE(
        physicalResource(
            conservative, "write_only_scratch")
            .representation ==
        VulkanResourceRepresentation::
            materialized_image);
    REQUIRE(
        conservative.attachments.front().operations.store_op ==
        AttachmentStoreOp::store);
}

TEST_CASE(
    "physical attachment plans validate and serialize raster subresources",
    "[target-render-planning][attachment-subresource][wp235]") {
    const auto types =
        makeBuiltinLogicalTypeRegistry();
    const auto graph =
        graphWithWriteOnlyAttachment(types);
    std::vector resources{
        VulkanPhysicalResourcePlan{
            .logical_resource =
                "write_only_scratch",
            .format = ImageFormat::r8g8b8a8_unorm,
            .mip_levels =
                ImageMipLevelCount{
                    .mode =
                        ImageMipLevelMode::fixed,
                    .count = 3,
                },
            .array_layers = 4,
        },
    };
    const auto range =
        ImageSubresourceRange{
            .base_mip_level = 1,
            .base_array_layer = 1,
            .layer_count = 2,
        };
    std::vector attachments{
        VulkanPhysicalAttachmentPlan{
            .node = "Scratch",
            .logical_resource =
                "write_only_scratch",
            .subresource = range,
            .aspect =
                AttachmentAspect::color,
            .operations =
                PassAttachmentOperations{
                    AttachmentLoadOp::clear,
                    AttachmentStoreOp::store},
        },
    };
    REQUIRE_NOTHROW(
        validateVulkanPhysicalAttachmentPlans(
            graph, resources, attachments));

    auto plan = compile(
        types, graph, fixtureOrder(graph), topology(false),
        bindingsFor(types, graph));
    plan.attachments = attachments;
    const auto encoded =
        vulkanTargetPlanToJson(plan);
    REQUIRE(
        encoded.at("attachments").at(0)
            .at("subresource").at("mip") == 1);
    REQUIRE(
        encoded.at("attachments").at(0)
            .at("subresource")
            .at("layer_count") == 2);

    attachments.front()
        .subresource->base_array_layer = 3;
    requireThrowsContaining(
        [&] {
            validateVulkanPhysicalAttachmentPlans(
                graph, resources, attachments);
        },
        "unsupported image subresource");

    attachments.front().subresource = range;
    resources.front().rasterization_samples = 4;
    requireThrowsContaining(
        [&] {
            validateVulkanPhysicalAttachmentPlans(
                graph, resources, attachments);
        },
        "unsupported image subresource");
}


TEST_CASE(
    "target plan fingerprints are deterministic and distinguish structural inputs",
    "[target-render-planning][fingerprint]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 4, false, true);
    const auto logical_fingerprint =
        vulkanTargetPlanLogicalGraphFingerprint(graph);
    REQUIRE(
        vulkanTargetPlanLogicalGraphFingerprint(graph) ==
        logical_fingerprint);

    auto reordered_imports = graph;
    reordered_imports.imports = {
        LogicalValueImport{
            LogicalValueId{"external_b", 2},
            LogicalValueImportKind::external},
        LogicalValueImport{
            LogicalValueId{"external_a", 1},
            LogicalValueImportKind::graph_input},
    };
    const auto ordered_import_fingerprint =
        vulkanTargetPlanLogicalGraphFingerprint(
            reordered_imports);
    std::reverse(
        reordered_imports.imports.begin(),
        reordered_imports.imports.end());
    REQUIRE(
        vulkanTargetPlanLogicalGraphFingerprint(
            reordered_imports) ==
        ordered_import_fingerprint);

    auto different_graph = graph;
    different_graph.nodes.front().uses.front().port +=
        "_changed";
    REQUIRE(
        vulkanTargetPlanLogicalGraphFingerprint(
            different_graph) != logical_fingerprint);

    const auto target = topology(true);
    const auto automatic = compile(
        types, graph, fixtureOrder(graph), target,
        bindingsFor(types, graph));
    const auto automatic_fingerprint =
        vulkanAutomaticTargetPlanFingerprint(
            target, automatic);
    REQUIRE(
        vulkanAutomaticTargetPlanFingerprint(
            target, automatic) ==
        automatic_fingerprint);

    auto reordered_topology = target;
    std::reverse(
        reordered_topology.endpoints.front()
            .capabilities.begin(),
        reordered_topology.endpoints.front()
            .capabilities.end());
    std::reverse(
        reordered_topology.endpoints.front().facts.begin(),
        reordered_topology.endpoints.front().facts.end());
    REQUIRE(
        vulkanAutomaticTargetPlanFingerprint(
            reordered_topology, automatic) ==
        automatic_fingerprint);

    auto explanatory_change = automatic;
    explanatory_change.backend_selection.decisions.push_back(
        PlanningDecision{
            "pelican.test.explanation@1", "candidate",
            "unchanged", "diagnostic-only decision"});
    REQUIRE(
        vulkanAutomaticTargetPlanFingerprint(
            target, explanatory_change) ==
        automatic_fingerprint);

    auto different_plan = automatic;
    different_plan.resources.front().reason +=
        " (changed)";
    REQUIRE(
        vulkanAutomaticTargetPlanFingerprint(
            target, different_plan) !=
        automatic_fingerprint);

    auto different_topology = target;
    different_topology.endpoints.front().facts.front().value +=
        "_changed";
    REQUIRE(
        vulkanAutomaticTargetPlanFingerprint(
            different_topology, automatic) !=
        automatic_fingerprint);
}


TEST_CASE(
    "complete Vulkan physical plans round-trip and verify as an "
    "environment-bound data package",
    "[target-render-planning][complete-physical-plan][eject]") {
    const auto types =
        makeBuiltinLogicalTypeRegistry();
    const auto graph =
        hybridGraph(types, 4, false, true);
    const auto target = topology(false);
    const auto automatic =
        compile(
            types, graph, fixtureOrder(graph), target,
            bindingsFor(types, graph));

    const auto ejected =
        ejectVulkanCompletePhysicalPlanPackage(
            automatic);
    const auto document =
        vulkanCompletePhysicalPlanPackageToJson(
            ejected);
    REQUIRE(
        document.at("schema") ==
        "pelican.vulkan_complete_physical_plan");
    REQUIRE(document.at("version") == 1);
    REQUIRE(
        document.at("resources").size() ==
        automatic.resources.size());
    REQUIRE(
        document.at("scopes").size() ==
        automatic.scopes.size());

    const auto parsed =
        vulkanCompletePhysicalPlanPackageFromJson(
            document);
    REQUIRE(parsed == ejected);
    const auto verified =
        verifyVulkanCompletePhysicalPlanPackage(
            graph, target, automatic, fixtureOrder(graph), parsed);
    REQUIRE(verified.package == ejected);
    REQUIRE(
        verified.package_fingerprint ==
        vulkanCompletePhysicalPlanPackageFingerprint(
            ejected));
    REQUIRE(verified.diagnostics.empty());

    const auto dump =
        vulkanTargetPlanToJson(automatic);
    for (const auto *field : {"ejectable_pin_package", "ejectable_physical_fragment",
                             "applied_pin_package", "applied_physical_fragment",
                             "ejectable_complete_physical_plan"})
        CHECK_FALSE(dump.contains(field));

    auto malformed = document;
    malformed["raw_commands"] =
        nlohmann::json::array();
    requireThrowsContaining(
        [&] {
            (void)vulkanCompletePhysicalPlanPackageFromJson(
                malformed);
        },
        "unknown key 'raw_commands'");
}

TEST_CASE(
    "complete Vulkan physical verifier rejects stale order, "
    "lifetime, and alias metadata",
    "[target-render-planning][complete-physical-plan][reject]") {
    const auto types =
        makeBuiltinLogicalTypeRegistry();
    const auto graph =
        hybridGraph(types, 4, false, true);
    const auto target = topology(false);
    std::vector<VulkanPhysicalAttachmentPlan> alias_evidence;
    std::map<std::string, ImageUsage, std::less<>> alias_usage;
    for (const auto &r : graph.resources) if (r.name.starts_with("gbuffer_")) {
        alias_usage[r.name] = ImageUsageBit::color_attachment | ImageUsageBit::sampled;
        alias_evidence.push_back({.node = "GBuffer", .logical_resource = r.name,
            .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::store}});
    }
    const auto automatic =
        compileVulkanTargetPlan(types, graph, target, providers(), {
            .node_order = fixtureOrder(graph), .endpoint = "device:0", .provider = std::string{kProvider},
            .pattern_bindings = bindingsFor(types, graph), .effective_image_usage = alias_usage,
            .automatic_attachments = alias_evidence});
    const auto ejected =
        ejectVulkanCompletePhysicalPlanPackage(
            automatic);

    auto stale = ejected;
    ++stale.logical_graph_fingerprint;
    requireThrowsContaining(
        [&] {
            (void)verifyVulkanCompletePhysicalPlanPackage(
                graph, target, automatic, fixtureOrder(graph),
                stale);
        },
        "stale for the logical graph");

    auto wrong_lifetime = ejected;
    const auto used =
        std::find_if(
            wrong_lifetime.resources.begin(),
            wrong_lifetime.resources.end(),
            [](const auto &resource) {
                return resource.lifetime.used;
            });
    REQUIRE(
        used != wrong_lifetime.resources.end());
    ++used->lifetime.last_use;
    requireThrowsContaining(
        [&] {
            (void)verifyVulkanCompletePhysicalPlanPackage(
                graph, target, automatic, fixtureOrder(graph),
                wrong_lifetime);
        },
        "stale lifetime");

    auto wrong_order = ejected;
    REQUIRE(wrong_order.scopes.size() >= 2);
    std::reverse(
        wrong_order.scopes.begin(),
        wrong_order.scopes.end());
    requireThrowsContaining(
        [&] {
            (void)verifyVulkanCompletePhysicalPlanPackage(
                graph, target, automatic, fixtureOrder(graph),
                wrong_order);
        },
        "render_compiler.physical_order_mismatches_frame_plan@1");

    auto missing_feature = ejected;
    std::erase(
        missing_feature.required_physical_features,
        "pelican.vulkan.graphics@1");
    requireThrowsContaining(
        [&] {
            (void)verifyVulkanCompletePhysicalPlanPackage(
                graph, target, automatic, fixtureOrder(graph),
                missing_feature);
        },
        "omits required feature");

    auto missing_resource_feature = ejected;
    const auto materialized =
        std::find_if(
            missing_resource_feature.resources.begin(),
            missing_resource_feature.resources.end(),
            [](const auto &resource) {
                return resource.representation ==
                       VulkanResourceRepresentation::
                           materialized_image;
            });
    REQUIRE(
        materialized !=
        missing_resource_feature.resources.end());
    std::erase(
        materialized->required_physical_features,
        "pelican.vulkan.sampled_image@1");
    requireThrowsContaining(
        [&] {
            (void)verifyVulkanCompletePhysicalPlanPackage(
                graph, target, automatic, fixtureOrder(graph),
                missing_resource_feature);
        },
        "physical resource omits required feature");

    auto wrong_scope_kind = ejected;
    REQUIRE_FALSE(wrong_scope_kind.scopes.empty());
    wrong_scope_kind.scopes.front().kind =
        VulkanPhysicalScopeKind::compute;
    requireThrowsContaining(
        [&] {
            (void)verifyVulkanCompletePhysicalPlanPackage(
                graph, target, automatic, fixtureOrder(graph),
                wrong_scope_kind);
        },
        "scope kind does not match logical node");

    auto overlapping_alias = ejected;
    const auto first =
        std::find_if(
            overlapping_alias.resources.begin(),
            overlapping_alias.resources.end(),
            [](const auto &resource) {
                return resource.aliasable;
            });
    REQUIRE(
        first != overlapping_alias.resources.end());
    const auto second =
        std::find_if(
            std::next(first),
            overlapping_alias.resources.end(),
            [&](const auto &resource) {
                return resource.aliasable &&
                       resource.representation ==
                           first->representation &&
                       resource.format ==
                           first->format &&
                       resource.rasterization_samples ==
                           first->rasterization_samples &&
                       resource.view_layout ==
                           first->view_layout &&
                       resource.mip_levels ==
                           first->mip_levels &&
                       resource.array_layers ==
                           first->array_layers &&
                       resource.dimension ==
                           first->dimension &&
                       resource.extent ==
                           first->extent &&
                       resource.lifetime.used &&
                       first->lifetime.used &&
                       !(resource.lifetime.last_use <
                             first->lifetime.first_use ||
                         first->lifetime.last_use <
                             resource.lifetime.first_use);
            });
    REQUIRE(
        second != overlapping_alias.resources.end());
    overlapping_alias.alias_groups = {
        VulkanAliasGroupPlan{
            .id = "overlap",
            .resources = {
                first->logical_resource,
                second->logical_resource,
            },
        },
    };
    requireThrowsContaining(
        [&] {
            (void)verifyVulkanCompletePhysicalPlanPackage(
                graph, target, automatic, fixtureOrder(graph),
                overlapping_alias);
        },
        "overlapping lifetimes");
}

TEST_CASE(
    "NativeScope closes typed effects and keeps unsafe "
    "synchronization explicit",
    "[target-render-planning][complete-physical-plan][native-scope]") {
    const auto types =
        makeBuiltinLogicalTypeRegistry();
    const auto graph =
        hybridGraph(types, 3, false, false);
    const auto target = topology(false);
    const auto automatic =
        compile(
            types, graph, fixtureOrder(graph), target,
            bindingsFor(types, graph));
    auto package =
        ejectVulkanCompletePhysicalPlanPackage(
            automatic);
    const auto &physical_scope =
        scopeForNode(automatic, "ToneMap");

    std::map<std::string, LogicalAccessMode, std::less<>>
        accesses;
    const auto merge =
        [&](std::string_view resource,
            LogicalAccessMode access) {
            const auto [found, inserted] =
                accesses.emplace(
                    std::string{resource}, access);
            if (!inserted &&
                found->second != access) {
                found->second =
                    LogicalAccessMode::read_write;
            }
        };
    for (const auto &node_name :
         physical_scope.nodes) {
        const auto node =
            std::find_if(
                graph.nodes.begin(),
                graph.nodes.end(),
                [&](const auto &candidate) {
                    return candidate.name ==
                           node_name;
                });
        REQUIRE(node != graph.nodes.end());
        for (const auto &use : node->uses) {
            if (use.input_value) {
                merge(
                    use.input_value->resource,
                    use.access);
            }
            if (use.output_value &&
                (!use.input_value ||
                 use.input_value->resource !=
                     use.output_value->resource)) {
                merge(
                    use.output_value->resource,
                    use.access);
            }
        }
    }

    VulkanNativeScopeDeclaration native{
        .scope = physical_scope.id,
        .implementation =
            "pelican.test.native_tonemap@1",
        .queue_capability =
            "pelican.vulkan.graphics@1",
        .synchronization =
            VulkanNativeScopeSynchronizationMode::
                automatic,
        .capture_compatible = true,
        .device_loss_recoverable = true,
        .hot_reloadable = true,
        .implementation_config =
            nlohmann::ordered_json{
                {"shader", "tonemap_native.spv"},
            },
    };
    for (const auto &[name, access] : accesses) {
        const auto logical =
            std::find_if(
                graph.resources.begin(),
                graph.resources.end(),
                [&](const auto &resource) {
                    return resource.name == name;
                });
        REQUIRE(logical != graph.resources.end());
        native.resources.push_back(
            VulkanNativeScopeResourceBoundary{
                .logical_resource = name,
                .semantic_type =
                    semanticTypeIdName(
                        logical->type.semantic),
                .access = access,
            });
    }
    package.native_scopes = {native};

    const auto verified =
        verifyVulkanCompletePhysicalPlanPackage(
            graph, target, automatic, fixtureOrder(graph), package);
    REQUIRE(verified.diagnostics.empty());

    auto extension_bound = package;
    extension_bound.native_scopes.front()
        .required_extensions = {"VK_EXT_test_scope"};
    requireThrowsContaining(
        [&] {
            (void)verifyVulkanCompletePhysicalPlanPackage(
                graph, target, automatic, fixtureOrder(graph),
                extension_bound);
        },
        "not enabled");
    const std::vector<std::string>
        enabled_extensions{"VK_EXT_test_scope"};
    REQUIRE_NOTHROW(
        verifyVulkanCompletePhysicalPlanPackage(
            graph, target, automatic, fixtureOrder(graph),
            extension_bound, {},
            enabled_extensions));

    const auto document =
        vulkanCompletePhysicalPlanPackageToJson(
            package);
    REQUIRE(
        vulkanCompletePhysicalPlanPackageFromJson(
            document) ==
        verified.package);

    auto incomplete = package;
    incomplete.native_scopes.front()
        .resources.pop_back();
    requireThrowsContaining(
        [&] {
            (void)verifyVulkanCompletePhysicalPlanPackage(
                graph, target, automatic, fixtureOrder(graph),
                incomplete);
        },
        "declare every accessed logical resource");

    auto manual = package;
    manual.native_scopes.front()
        .synchronization =
        VulkanNativeScopeSynchronizationMode::manual;
    requireThrowsContaining(
        [&] {
            (void)verifyVulkanCompletePhysicalPlanPackage(
                graph, target, automatic, fixtureOrder(graph),
                manual);
        },
        "manual NativeScope synchronization requires");
    for (auto &resource :
         manual.native_scopes.front().resources) {
        resource.stages = {
            "VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT"};
        resource.accesses = {
            "VK_ACCESS_2_MEMORY_READ_BIT",
            "VK_ACCESS_2_MEMORY_WRITE_BIT"};
    }
    REQUIRE_NOTHROW(
        verifyVulkanCompletePhysicalPlanPackage(
            graph, target, automatic, fixtureOrder(graph), manual));

    auto unchecked = package;
    unchecked.native_scopes.front()
        .synchronization =
        VulkanNativeScopeSynchronizationMode::
            unchecked;
    const auto unchecked_verified =
        verifyVulkanCompletePhysicalPlanPackage(
            graph, target, automatic, fixtureOrder(graph), unchecked);
    REQUIRE(
        std::any_of(
            unchecked_verified.diagnostics.begin(),
            unchecked_verified.diagnostics.end(),
            [](const auto &diagnostic) {
                return diagnostic.id ==
                       "pelican.plan.native_scope_unchecked_sync@1";
            }));
}

TEST_CASE(
    "Vulkan target planning preserves cube resource shape independently "
    "from view-family layout",
    "[target-render-planning][cube][wp236]") {
    const auto types =
        makeBuiltinLogicalTypeRegistry();
    const auto graph =
        hybridGraph(types, 4, false, true);
    auto bindings =
        bindingsFor(types, graph);
    const auto scene_color =
        std::find_if(
            bindings.begin(), bindings.end(),
            [](const auto &binding) {
                return binding.resource ==
                       "scene_color";
            });
    REQUIRE(scene_color != bindings.end());
    scene_color->dimension =
        ImageResourceDimension::cube;
    scene_color->array_layers = 6;
    scene_color->extent =
        ResourceExtentPlan{
            .kind = ResourceExtentKind::fixed,
            .width = 256,
            .height = 256,
        };

    const auto plan =
        compile(
            types, graph, fixtureOrder(graph), topology(false),
            bindings);
    const auto &physical =
        physicalResource(plan, "scene_color");
    REQUIRE(
        physical.dimension ==
        ImageResourceDimension::cube);
    REQUIRE(physical.array_layers == 6);
    REQUIRE(
        physical.view_layout ==
        VulkanResourceViewLayout::shared_2d);
    const auto encoded =
        vulkanTargetPlanToJson(plan);
    const auto encoded_resource =
        std::find_if(
            encoded.at("resources").begin(),
            encoded.at("resources").end(),
            [](const auto &resource) {
                return resource.at(
                           "logical_resource") ==
                       "scene_color";
            });
    REQUIRE(
        encoded_resource !=
        encoded.at("resources").end());
    REQUIRE(
        encoded_resource->at("dimension") ==
        "cube");

    auto invalid_layers = bindings;
    std::find_if(
        invalid_layers.begin(),
        invalid_layers.end(),
        [](const auto &binding) {
            return binding.resource ==
                   "scene_color";
        })->array_layers = 5;
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, fixtureOrder(graph), topology(false),
                invalid_layers);
        },
        "exactly six array layers");

    auto invalid_extent = bindings;
    std::find_if(
        invalid_extent.begin(),
        invalid_extent.end(),
        [](const auto &binding) {
            return binding.resource ==
                   "scene_color";
        })->extent =
        ResourceExtentPlan{
            .kind = ResourceExtentKind::fixed,
            .width = 256,
            .height = 128,
        };
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, fixtureOrder(graph), topology(false),
                invalid_extent);
        },
        "square fixed extent");
}

TEST_CASE("Vulkan automatic attachments preserve logical dependencies and resolve semantics",
          "[target-render-planning][physical-fragment][attachment]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph =
        hybridGraph(types, 3, false, true);
    const auto attachment_contract = [] {
        return std::vector<VulkanPhysicalAttachmentPlan>{
            {
                .node = "GBuffer",
                .logical_resource = "gbuffer_0",
                .aspect =
                    AttachmentAspect::color,
                .operations =
                    PassAttachmentOperations{
                        AttachmentLoadOp::clear,
                        AttachmentStoreOp::store},
            },
            {
                .node = "Forward",
                .logical_resource = "scene_color",
                .aspect =
                    AttachmentAspect::color,
                .operations =
                    PassAttachmentOperations{
                        AttachmentLoadOp::load,
                        AttachmentStoreOp::store},
            },
            {
                .node = "Lighting",
                .logical_resource = "scene_color",
                .aspect =
                    AttachmentAspect::color,
                .operations =
                    PassAttachmentOperations{
                        AttachmentLoadOp::clear,
                        AttachmentStoreOp::store},
            },
            {
                .node = "ToneMap",
                .logical_resource = "display_output",
                .aspect =
                    AttachmentAspect::color,
                .operations =
                    PassAttachmentOperations{
                        AttachmentLoadOp::clear,
                        AttachmentStoreOp::store},
            },
        };
    };
    const auto automatic = compile(
        types, graph, fixtureOrder(graph), topology(false),
        bindingsFor(types, graph),
        std::nullopt, std::nullopt,
        std::nullopt,
        attachment_contract());

    auto temporal_self_read = graph;
    const auto temporal_node = std::find_if(
        temporal_self_read.nodes.begin(),
        temporal_self_read.nodes.end(),
        [](const auto &node) {
            return node.name == "GBuffer";
        });
    REQUIRE(
        temporal_node !=
        temporal_self_read.nodes.end());
    const auto &temporal_resource =
        resource(
            temporal_self_read,
            "gbuffer_0");
    temporal_node->ports.push_back(
        port(
            types,
            "history_gbuffer_0",
            LogicalPortDirection::input,
            temporal_resource.type));
    temporal_node->uses.push_back(
        makeLogicalReadUse(
            "history_gbuffer_0",
            LogicalValueId{
                temporal_resource.name, 0},
            LogicalReadFootprint{
                LogicalReadFootprintKind::temporal,
                std::nullopt},
            LogicalAccessIntent::sampled));
    temporal_self_read.imports.push_back(
        LogicalValueImport{
            LogicalValueId{
                temporal_resource.name, 0},
            LogicalValueImportKind::previous_epoch});
    validateCompiledLogicalRenderGraph(
        types, temporal_self_read);
    REQUIRE_NOTHROW(
        compile(
            types, temporal_self_read, fixtureOrder(temporal_self_read),
            topology(false),
            bindingsFor(
                types, temporal_self_read),
            std::nullopt, std::nullopt,
            std::nullopt,
            attachment_contract()));

    CHECK(automatic.attachments == attachment_contract());
    auto bad = automatic.attachments;
    bad.at(0).operations.load_op = AttachmentLoadOp::load;
    requireThrowsContaining([&] { validateVulkanPhysicalAttachmentPlans(graph, automatic.resources, bad); },
                            "Load has no logical read dependency");
    bad = automatic.attachments;
    bad.at(1).operations.load_op = AttachmentLoadOp::clear;
    requireThrowsContaining([&] { validateVulkanPhysicalAttachmentPlans(graph, automatic.resources, bad); },
                            "cannot discard or clear a logical read dependency");
    const SampleCountPolicy msaa_policy{
        .request = {SampleCountRequestMode::exact, 4},
        .scope = SampleCountScope::geometry, .authored = true};
    const auto msaa = compile(types, graph, fixtureOrder(graph), topology(false), bindingsFor(types, graph),
        sampleCountRequest(graph, msaa_policy), std::nullopt, std::nullopt,
        attachment_contract());
    CHECK(physicalResource(msaa, "gbuffer_0").resolve_required);
    CHECK(physicalResource(msaa, "scene_color").resolve_required);

}


TEST_CASE("RC-3d2 explicit physical orders reject stale complete plans and accept fresh complete plans",
          "[target-render-planning][rc3d2][node-order][stale]") {
    static_assert(!std::is_default_constructible_v<FramePlanNodeOrder>);
    static_assert(!std::is_default_constructible_v<VulkanTargetPlanRequest>);
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = graphWithWriteOnlyAttachment(types);
    const auto first = fixtureOrder(graph);
    const FramePlanNodeOrder second{graph, {"Scratch", "GBuffer", "Lighting", "Forward", "ToneMap"}};
    const auto a = compile(types, graph, first, topology(false), bindingsFor(types, graph));
    const auto b = compile(types, graph, second, topology(false), bindingsFor(types, graph));
    for (const auto *plan : {&a, &b}) {
        const auto &expected = plan == &a ? first.nodes() : second.nodes();
        CHECK(physicalNodeOrderOf(plan->scopes).nodes == expected);
        CHECK(plan->opportunities.node_order == expected);
        CHECK(vulkanTargetPlanToJson(*plan).at("planning_opportunities").at("node_order") ==
              nlohmann::ordered_json(expected));
        std::vector<std::string> lowering;
        for (const auto &node : plan->lowering_graph.nodes) lowering.push_back(node.logical.name);
        CHECK(lowering == expected);
    }
    CHECK(a.logical_graph_fingerprint == b.logical_graph_fingerprint);
    REQUIRE(a.automatic_plan_fingerprint != b.automatic_plan_fingerprint);
    CHECK(physicalResource(a, "write_only_scratch").lifetime.first_use == 3);
    CHECK(physicalResource(b, "write_only_scratch").lifetime.first_use == 0);
    const auto old_complete = vulkanCompletePhysicalPlanPackageFromJson(
        vulkanCompletePhysicalPlanPackageToJson(ejectVulkanCompletePhysicalPlanPackage(a)));
    requireThrowsContaining([&] {
        (void)verifyVulkanCompletePhysicalPlanPackage(graph, topology(false), b, second, old_complete);
    }, "complete_physical_plan.stale_automatic_plan@1");
    const auto fresh_complete = vulkanCompletePhysicalPlanPackageFromJson(
        vulkanCompletePhysicalPlanPackageToJson(ejectVulkanCompletePhysicalPlanPackage(b)));
    const auto verified = verifyVulkanCompletePhysicalPlanPackage(graph, topology(false), b, second, fresh_complete);
    CHECK(physicalNodeOrderOf(applyVerifiedVulkanCompletePhysicalPlanPackage(b, verified).scopes).nodes == second.nodes());
    const auto tile = compile(types, graph, first, topology(true), bindingsFor(types, graph));
    REQUIRE(tile.backend_selection.selected_candidate == "pelican.vulkan.tile_local_plan@1");
    const FramePlanNodeOrder interrupted{graph, {"GBuffer", "Scratch", "Lighting", "Forward", "ToneMap"}};
    const auto materialized = compile(types, graph, interrupted, topology(true), bindingsFor(types, graph));
    REQUIRE(materialized.backend_selection.selected_candidate == "pelican.vulkan.transient_plan@1");
    // Saving through JSON exercises persisted packages across a change that
    // changes both the automatic fingerprint and the selected backend.
    const auto saved_complete = vulkanCompletePhysicalPlanPackageFromJson(
        vulkanCompletePhysicalPlanPackageToJson(ejectVulkanCompletePhysicalPlanPackage(tile)));
    try {
        (void)verifyVulkanCompletePhysicalPlanPackage(graph, topology(true), materialized, interrupted, saved_complete);
        FAIL("saved tile-local complete plan must be stale after backend change");
    } catch (const GraphRegistryError &error) {
        CHECK(error.code() == "complete_physical_plan.stale_automatic_plan@1");
        CHECK(error.detail().at("graph") == graph.name);
        CHECK(error.detail().at("message").get<std::string>().find("eject it again") != std::string::npos);
    }
    const auto new_complete = vulkanCompletePhysicalPlanPackageFromJson(
        vulkanCompletePhysicalPlanPackageToJson(ejectVulkanCompletePhysicalPlanPackage(materialized)));
    const auto new_verified = verifyVulkanCompletePhysicalPlanPackage(graph, topology(true), materialized, interrupted, new_complete);
    CHECK(physicalNodeOrderOf(applyVerifiedVulkanCompletePhysicalPlanPackage(materialized, new_verified).scopes).nodes == interrupted.nodes());

}

TEST_CASE("RC-3d2 standalone complete verification requires the expected order beyond dependency legality",
          "[target-render-planning][rc3d2][complete-physical-plan]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = graphWithWriteOnlyAttachment(types);
    const auto first = fixtureOrder(graph);
    const FramePlanNodeOrder second{graph, {"Scratch", "GBuffer", "Lighting", "Forward", "ToneMap"}};
    const auto automatic = compile(types, graph, first, topology(false), bindingsFor(types, graph));
    const auto reordered = compile(types, graph, second, topology(false), bindingsFor(types, graph));
    auto package = ejectVulkanCompletePhysicalPlanPackage(reordered);
    package.automatic_plan_fingerprint = automatic.automatic_plan_fingerprint;
    // Prove dependency, lifetime, alias and freshness checks accept this
    // package. Only the expected sequence differs in the refusal below.
    REQUIRE_NOTHROW(verifyVulkanCompletePhysicalPlanPackage(graph, topology(false), automatic, second, package));
    try {
        (void)verifyVulkanCompletePhysicalPlanPackage(graph, topology(false), automatic, first, package);
        FAIL("complete plan must match its expected FramePlan");
    } catch (const GraphRegistryError &error) {
        CHECK(error.code() == "render_compiler.physical_order_mismatches_frame_plan@1");
        CHECK(error.detail().at("graph") == graph.name);
        CHECK(error.detail().at("position") == 0);
        CHECK(error.detail().at("expected") == "GBuffer");
        CHECK(error.detail().at("actual") == "Scratch");
    }
    REQUIRE_NOTHROW(verifyVulkanCompletePhysicalPlanPackage(
        graph, topology(false), automatic, first, ejectVulkanCompletePhysicalPlanPackage(automatic)));
}

TEST_CASE("RC-3d2 order inputs distinguish empty graphs from missing and invalid orders",
          "[target-render-planning][rc3d2][node-order]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const CompiledLogicalRenderGraph empty{.name = "empty"};
    const FramePlanNodeOrder empty_order{empty, {}};
    const auto empty_plan = compile(types, empty, empty_order, topology(false), {});
    CHECK(empty_plan.scopes.empty());
    const auto graph = hybridGraph(types, 3, false);
    for (const auto &names : std::vector<std::vector<std::string>>{
            {}, {"GBuffer", "Lighting", "Forward"}, {"GBuffer", "Lighting", "Forward", "extra"},
            {"GBuffer", "Lighting", "Forward", "Forward", "ToneMap"}}) {
        requireThrowsContaining([&] { (void)FramePlanNodeOrder{graph, names}; }, "target_plan.node_order_mismatch@1");
    }
    requireThrowsContaining([&] {
        (void)FramePlanNodeOrder{graph, {"Lighting", "GBuffer", "Forward", "ToneMap"}};
    }, "target_plan.node_order_conflicts_dependency@1");
}


TEST_CASE("view execution planning keeps mono and sequential stereo as explicit physical contracts",
          "[target-render-planning][view-execution][sequential]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);

    const auto mono = compile(
        types, graph, fixtureOrder(graph), topology(false),
        bindingsFor(types, graph));
    REQUIRE(mono.view_execution_plan.view_count == 1);
    REQUIRE_FALSE(mono.view_execution_plan.uses_multiview);
    REQUIRE(std::all_of(
        mono.scopes.begin(), mono.scopes.end(),
        [](const VulkanPhysicalScopePlan &scope) {
            return scope.view_execution ==
                       VulkanScopeViewExecution::single_view &&
                   scope.view_count == 1 &&
                   scope.execution_count == 1 &&
                   scope.view_mask == 0;
        }));
    REQUIRE(std::all_of(
        mono.resources.begin(), mono.resources.end(),
        [](const VulkanPhysicalResourcePlan &resource) {
            return resource.view_layout ==
                       VulkanResourceViewLayout::shared_2d &&
                   resource.array_layers == 1;
        }));

    const auto sequential = compile(
        types, graph, fixtureOrder(graph), topology(false),
        bindingsFor(types, graph), std::nullopt,
        VulkanViewExecutionPlanRequest{
            .view_count = 2,
            .preference =
                XrViewExecutionPreference::automatic,
            .multiview_capable_nodes =
                {"GBuffer", "Lighting", "Forward", "ToneMap"},
        });
    REQUIRE_FALSE(
        sequential.view_execution_plan.uses_multiview);
    REQUIRE(sequential.view_execution_plan.reason.find(
                "does not advertise") != std::string::npos);
    REQUIRE(std::all_of(
        sequential.scopes.begin(),
        sequential.scopes.end(),
        [](const VulkanPhysicalScopePlan &scope) {
            return scope.view_execution ==
                       VulkanScopeViewExecution::sequential &&
                   scope.view_count == 2 &&
                   scope.execution_count == 2 &&
                   scope.view_mask == 0;
        }));
    REQUIRE(physicalResource(sequential, "scene_color")
                .view_layout ==
            VulkanResourceViewLayout::sequential_2d);
    REQUIRE(std::find(
                sequential.required_physical_features.begin(),
                sequential.required_physical_features.end(),
                "pelican.vulkan.multiview@1") ==
                sequential.required_physical_features.end());
}

TEST_CASE("directional shadow remains one producer-family array for flat preview sequential XR and multiview",
          "[target-render-planning][view-execution][shadow][wp205]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph =
        hybridGraphWithSharedShadow(types);
    const auto independent =
        std::vector<std::string>{"ShadowDepth"};
    const auto multiview_capable =
        std::vector<std::string>{
            "GBuffer", "Lighting", "Forward",
            "ToneMap"};

    const auto flat = compile(
        types, graph, fixtureOrder(graph), topology(false),
        bindingsFor(types, graph), std::nullopt,
        VulkanViewExecutionPlanRequest{
            .view_count = 1,
            .view_independent_nodes = independent,
        });
    auto preview_graph = graph;
    preview_graph.name = "preview";
    const auto preview = compile(
        types, preview_graph, fixtureOrder(preview_graph), topology(false),
        bindingsFor(types, preview_graph), std::nullopt,
        VulkanViewExecutionPlanRequest{
            .view_count = 1,
            .view_independent_nodes = independent,
        });
    for (const auto *plan : {&flat, &preview}) {
        REQUIRE(scopeForNode(*plan, "ShadowDepth")
                    .view_execution ==
                VulkanScopeViewExecution::single_view);
        REQUIRE(scopeForNode(*plan, "Lighting")
                    .view_execution ==
                VulkanScopeViewExecution::single_view);
        REQUIRE(physicalResource(
                    *plan, "directional_shadow")
                    .view_layout ==
                VulkanResourceViewLayout::
                    family_2d_array);
        REQUIRE(physicalResource(
                    *plan, "directional_shadow")
                    .array_layers == 1);
    }

    const auto sequential = compile(
        types, graph, fixtureOrder(graph), topology(false),
        bindingsFor(types, graph), std::nullopt,
        VulkanViewExecutionPlanRequest{
            .view_count = 2,
            .preference =
                XrViewExecutionPreference::sequential,
            .view_independent_nodes = independent,
            .multiview_capable_nodes =
                multiview_capable,
        });
    const auto &sequential_shadow =
        scopeForNode(sequential, "ShadowDepth");
    REQUIRE(sequential_shadow.view_execution ==
            VulkanScopeViewExecution::single_view);
    REQUIRE(sequential_shadow.execution_count == 1);
    REQUIRE(scopeForNode(sequential, "Lighting")
                .view_execution ==
            VulkanScopeViewExecution::sequential);
    REQUIRE(scopeForNode(sequential, "Forward")
                .execution_count == 2);
    REQUIRE(physicalResource(
                sequential, "directional_shadow")
                .view_layout ==
            VulkanResourceViewLayout::
                family_2d_array);
    REQUIRE(physicalResource(
                sequential, "directional_shadow")
                .array_layers == 1);

    const auto multiview = compile(
        types, graph, fixtureOrder(graph), topology(false, 8, 2),
        bindingsFor(types, graph), std::nullopt,
        VulkanViewExecutionPlanRequest{
            .view_count = 2,
            .preference =
                XrViewExecutionPreference::automatic,
            .view_independent_nodes = independent,
            .multiview_capable_nodes =
                multiview_capable,
        });
    REQUIRE(multiview.view_execution_plan.uses_multiview);
    REQUIRE(scopeForNode(multiview, "ShadowDepth")
                .view_execution ==
            VulkanScopeViewExecution::single_view);
    REQUIRE(scopeForNode(multiview, "Lighting")
                .view_execution ==
            VulkanScopeViewExecution::multiview);
    REQUIRE(scopeForNode(multiview, "Forward")
                .view_mask == 0b11u);
    REQUIRE(physicalResource(
                multiview, "directional_shadow")
                .view_layout ==
            VulkanResourceViewLayout::
                family_2d_array);
    REQUIRE(physicalResource(
                multiview, "directional_shadow")
                .array_layers == 1);
}

TEST_CASE(
    "secondary-family internal images retain runtime per-view layers",
    "[target-render-planning][view-execution][secondary]") {
    const auto types =
        makeBuiltinLogicalTypeRegistry();
    const auto graph =
        hybridGraphWithSecondaryInternalResource(
            types);
    const auto plan =
        compile(
            types, graph, fixtureOrder(graph), topology(false),
            bindingsFor(types, graph),
            std::nullopt,
            VulkanViewExecutionPlanRequest{
                .view_count = 1,
                .view_independent_nodes = {
                    "CapturePrepare",
                    "CaptureResolve",
                },
            });

    REQUIRE(
        scopeForNode(
            plan, "CapturePrepare")
            .view_execution ==
        VulkanScopeViewExecution::
            single_view);
    REQUIRE(
        scopeForNode(
            plan, "CaptureResolve")
            .view_execution ==
        VulkanScopeViewExecution::
            single_view);
    REQUIRE(
        physicalResource(
            plan,
            "capture_intermediate")
            .view_layout ==
        VulkanResourceViewLayout::
            sequential_2d);
    REQUIRE(
        physicalResource(
            plan, "capture_output")
            .view_layout ==
        VulkanResourceViewLayout::
            family_2d_array);
}

TEST_CASE("view execution planning selects layered multiview and exposes its Vulkan scope contract",
          "[target-render-planning][view-execution][multiview]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);
    const auto canonical_before =
        compiledLogicalRenderGraphToJson(graph).dump();
    const auto plan = compile(
        types, graph, fixtureOrder(graph), topology(false, 8, 2),
        bindingsFor(types, graph), std::nullopt,
        VulkanViewExecutionPlanRequest{
            .view_count = 2,
            .preference =
                XrViewExecutionPreference::automatic,
            .multiview_capable_nodes =
                {"GBuffer", "Lighting", "Forward", "ToneMap"},
        });

    REQUIRE(plan.view_execution_plan.uses_multiview);
    REQUIRE_FALSE(plan.view_execution_plan.mixed_execution);
    REQUIRE(plan.view_execution_plan
                .endpoint_supports_multiview);
    REQUIRE(plan.view_execution_plan
                .max_multiview_view_count == 2);
    REQUIRE(std::all_of(
        plan.scopes.begin(), plan.scopes.end(),
        [](const VulkanPhysicalScopePlan &scope) {
            return scope.view_execution ==
                       VulkanScopeViewExecution::multiview &&
                   scope.view_count == 2 &&
                   scope.execution_count == 1 &&
                   scope.view_mask == 0b11;
        }));
    REQUIRE(physicalResource(plan, "gbuffer_0")
                .view_layout ==
            VulkanResourceViewLayout::layered_2d_array);
    REQUIRE(physicalResource(plan, "display_output")
                .array_layers == 2);
    REQUIRE(std::find(
                plan.required_physical_features.begin(),
                plan.required_physical_features.end(),
                "pelican.vulkan.multiview@1") !=
            plan.required_physical_features.end());
    const auto encoded = vulkanTargetPlanToJson(plan);
    REQUIRE(encoded.at("view_execution_plan")
                .at("uses_multiview") == true);
    REQUIRE(encoded.at("scopes").at(0).contains("view_mask"));
    REQUIRE(compiledLogicalRenderGraphToJson(graph).dump() ==
            canonical_before);
}

TEST_CASE("automatic multiview honors measured device profiles while required mode overrides them",
          "[target-render-planning][view-execution][device-profile][wp203c]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);
    const XrMultiviewDeviceIdentity device{
        .vendor_id = 4318,
        .device_id = 9860,
        .driver_version = 77,
        .device_name = "Mock GPU",
    };
    const XrMultiviewAutoPolicy measured_regression{
        .minimum_gain_percent = 0.0,
        .profiles =
            {XrMultiviewDeviceProfile{
                .id = "mock_regression",
                .vendor_id = 4318,
                .device_id = 9860,
                .measurement = {
                    .sequential_gpu_ms = 5.0,
                    .multiview_gpu_ms = 5.5,
                    .sample_count = 300,
                    .source =
                        "RenderTiming/gpu_timestamp",
                },
            }},
    };
    const auto capable_nodes =
        std::vector<std::string>{
            "GBuffer", "Lighting", "Forward",
            "ToneMap"};

    const auto automatic = compile(
        types, graph, fixtureOrder(graph),
        topology(false, 8, 2, &device),
        bindingsFor(types, graph), std::nullopt,
        VulkanViewExecutionPlanRequest{
            .view_count = 2,
            .preference =
                XrViewExecutionPreference::automatic,
            .automatic_policy =
                measured_regression,
            .multiview_capable_nodes =
                capable_nodes,
        });
    REQUIRE_FALSE(
        automatic.view_execution_plan
            .uses_multiview);
    REQUIRE(automatic.view_execution_plan
                .automatic_policy
                .matched_profile);
    REQUIRE(automatic.view_execution_plan
                .automatic_policy
                .profile_id ==
            "mock_regression");
    REQUIRE(automatic.view_execution_plan.reason.find(
                "device performance profile") !=
            std::string::npos);
    REQUIRE(std::all_of(
        automatic.scopes.begin(),
        automatic.scopes.end(),
        [](const auto &scope) {
            return scope.view_execution ==
                   VulkanScopeViewExecution::sequential;
        }));
    const auto encoded =
        vulkanTargetPlanToJson(automatic);
    REQUIRE(encoded.at("view_execution_plan")
                .at("auto_gate")
                .at("selection") ==
            "sequential");
    REQUIRE(encoded.at("view_execution_plan")
                .at("auto_gate")
                .at("measurement")
                .at("sample_count") == 300);
    REQUIRE(encoded.at("view_execution_plan")
                .at("auto_gate")
                .at("device")
                .at("device_id") == 9860);
    REQUIRE(encoded.at("view_execution_plan")
                .at("auto_gate")
                .at("graph") == graph.name);

    const auto required = compile(
        types, graph, fixtureOrder(graph),
        topology(false, 8, 2, &device),
        bindingsFor(types, graph), std::nullopt,
        VulkanViewExecutionPlanRequest{
            .view_count = 2,
            .preference =
                XrViewExecutionPreference::
                    require_multiview,
            .automatic_policy =
                measured_regression,
            .multiview_capable_nodes =
                capable_nodes,
        });
    REQUIRE(required.view_execution_plan
                .uses_multiview);
    REQUIRE(required.view_execution_plan
                .automatic_policy.selection ==
            XrMultiviewAutoSelection::sequential);
}

TEST_CASE("view execution planning supports mixed scopes and rejects an unsatisfied required policy",
          "[target-render-planning][view-execution][mixed]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);
    const auto mixed = compile(
        types, graph, fixtureOrder(graph), topology(false, 8, 2),
        bindingsFor(types, graph), std::nullopt,
        VulkanViewExecutionPlanRequest{
            .view_count = 2,
            .preference =
                XrViewExecutionPreference::automatic,
            .multiview_capable_nodes =
                {"GBuffer", "Lighting", "Forward"},
        });

    REQUIRE(mixed.view_execution_plan.uses_multiview);
    REQUIRE(mixed.view_execution_plan.mixed_execution);
    REQUIRE(scopeForNode(mixed, "GBuffer").view_execution ==
            VulkanScopeViewExecution::multiview);
    REQUIRE(scopeForNode(mixed, "ToneMap").view_execution ==
            VulkanScopeViewExecution::sequential);
    REQUIRE(physicalResource(mixed, "scene_color")
                .view_layout ==
            VulkanResourceViewLayout::layered_2d_array);
    REQUIRE(physicalResource(mixed, "display_output")
                .view_layout ==
            VulkanResourceViewLayout::sequential_2d);

    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, fixtureOrder(graph), topology(false, 8, 2),
                bindingsFor(types, graph), std::nullopt,
                VulkanViewExecutionPlanRequest{
                    .view_count = 2,
                    .preference =
                        XrViewExecutionPreference::
                            require_multiview,
                    .multiview_capable_nodes =
                        {"GBuffer", "Lighting", "Forward"},
                });
        },
        "ToneMap");
}

TEST_CASE("external depth export infers camera depth and keeps it materialized across tile lowering",
          "[target-render-planning][external-depth][tile][multiview]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    auto graph = hybridGraph(types, 3, false, true);
    graph.resources.push_back(LogicalResourceDesc{
        .name = "shadow_depth",
        .type = deviceDepthV1(types),
    });
    LogicalGraphNode shadow;
    shadow.name = "ShadowDepth";
    shadow.kind = LogicalGraphNodeKind::render;
    shadow.declaration_index = 100;
    shadow.after = {"ToneMap"};
    addWrite(types, shadow,
             resource(graph, "shadow_depth"), 1,
             "depth");
    graph.nodes.push_back(std::move(shadow));
    validateCompiledLogicalRenderGraph(types, graph);

    const auto plan = compile(
        types, graph, FramePlanNodeOrder{graph, {"GBuffer", "Lighting", "Forward", "ToneMap", "ShadowDepth"}}, topology(true, 8, 2),
        bindingsFor(types, graph), std::nullopt,
        VulkanViewExecutionPlanRequest{
            .view_count = 2,
            .preference =
                XrViewExecutionPreference::automatic,
            .multiview_capable_nodes =
                {"GBuffer", "Lighting", "Forward",
                 "ToneMap", "ShadowDepth"},
        },
        VulkanExternalDepthExportRequest{});

    REQUIRE(plan.external_depth_export.has_value());
    REQUIRE(plan.external_depth_export->source_resource ==
            "scene_depth");
    REQUIRE(plan.external_depth_export->format ==
            ImageFormat::d32_sfloat);
    REQUIRE(plan.external_depth_export->view_layout ==
            VulkanResourceViewLayout::layered_2d_array);
    REQUIRE(plan.external_depth_export->array_layers == 2);
    const auto &depth =
        physicalResource(plan, "scene_depth");
    CHECK(depth.lifetime.retain == std::vector<std::string>{"external_depth_export"});
    CHECK(depth.lifetime.last_use == 4);
    const auto without_export = compile(types, graph,
        FramePlanNodeOrder{graph, {"GBuffer", "Lighting", "Forward", "ToneMap", "ShadowDepth"}},
        topology(false), bindingsFor(types, graph));
    CHECK(physicalResource(without_export, "scene_depth").lifetime.last_use == 2);
    CHECK(physicalResource(without_export, "scene_depth").lifetime.retain.empty());
    REQUIRE(depth.representation ==
            VulkanResourceRepresentation::materialized_image);
    REQUIRE(depth.stored);
    REQUIRE_FALSE(depth.aliasable);
    REQUIRE(std::find(
                depth.required_physical_features.begin(),
                depth.required_physical_features.end(),
                "pelican.vulkan.transfer_copy@1") !=
            depth.required_physical_features.end());
    const auto encoded = vulkanTargetPlanToJson(plan);
    REQUIRE(encoded.at("external_depth_export")
                .at("source_resource") == "scene_depth");
    REQUIRE(encoded.at("external_depth_export")
                .at("array_layers") == 2);
    const auto complete =
        ejectVulkanCompletePhysicalPlanPackage(plan);
    REQUIRE(
        complete.external_depth_export ==
        plan.external_depth_export);
    const auto complete_document =
        vulkanCompletePhysicalPlanPackageToJson(
            complete);
    REQUIRE(
        complete_document.at("external_depth_export")
            .at("source_resource") ==
        "scene_depth");
    REQUIRE_NOTHROW(
        verifyVulkanCompletePhysicalPlanPackage(
            graph, topology(true, 8, 2), plan, FramePlanNodeOrder{graph, {"GBuffer", "Lighting", "Forward", "ToneMap", "ShadowDepth"}},
            vulkanCompletePhysicalPlanPackageFromJson(
                complete_document)));

    const auto mixed = compile(
        types, graph, FramePlanNodeOrder{graph, {"GBuffer", "Lighting", "Forward", "ToneMap", "ShadowDepth"}}, topology(false, 8, 2),
        bindingsFor(types, graph), std::nullopt,
        VulkanViewExecutionPlanRequest{
            .view_count = 2,
            .preference =
                XrViewExecutionPreference::automatic,
            .multiview_capable_nodes =
                {"Lighting", "ToneMap", "ShadowDepth"},
        },
        VulkanExternalDepthExportRequest{});
    REQUIRE(mixed.view_execution_plan.uses_multiview);
    REQUIRE(mixed.view_execution_plan.mixed_execution);
    REQUIRE(scopeForNode(mixed, "Forward")
                .view_execution ==
            VulkanScopeViewExecution::sequential);
    REQUIRE(mixed.external_depth_export
                ->view_layout ==
            VulkanResourceViewLayout::layered_2d_array);
    REQUIRE(mixed.external_depth_export
                ->array_layers == 2);

    const auto explicit_shadow = compile(
        types, graph, FramePlanNodeOrder{graph, {"GBuffer", "Lighting", "Forward", "ToneMap", "ShadowDepth"}}, topology(false),
        bindingsFor(types, graph), std::nullopt,
        std::nullopt,
        VulkanExternalDepthExportRequest{
            .source_resource = "shadow_depth",
        });
    REQUIRE(explicit_shadow.external_depth_export
                ->source_resource == "shadow_depth");
}

TEST_CASE("external depth export remains optional but diagnoses an unsatisfied required request",
          "[target-render-planning][external-depth][diagnostic]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);

    const auto optional = compile(
        types, graph, fixtureOrder(graph), topology(false),
        bindingsFor(types, graph), std::nullopt,
        std::nullopt,
        VulkanExternalDepthExportRequest{});
    REQUIRE_FALSE(optional.external_depth_export);

    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, fixtureOrder(graph), topology(false),
                bindingsFor(types, graph), std::nullopt,
                std::nullopt,
                VulkanExternalDepthExportRequest{
                    .required = true,
                });
        },
        "found no written compatible device/projection depth");
}

TEST_CASE("neighborhood refraction materializes an opaque snapshot without spilling G-buffer attachments",
          "[target-render-planning][tile][refraction][snapshot]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 5, true);
    const auto plan = compile(types, graph, fixtureOrder(graph), topology(true),
                              bindingsFor(types, graph));

    REQUIRE(plan.backend_selection.selected_candidate ==
            "pelican.vulkan.tile_local_plan@1");
    REQUIRE(physicalResource(plan, "gbuffer_0").representation ==
            VulkanResourceRepresentation::tile_local_attachment);
    const auto &snapshot =
        physicalResource(plan, "opaque_snapshot");
    REQUIRE(snapshot.representation ==
            VulkanResourceRepresentation::materialized_image);
    REQUIRE(snapshot.widest_read ==
            LogicalReadFootprintKind::neighborhood);
    REQUIRE(snapshot.reason.find("snapshot") != std::string::npos);
    REQUIRE(std::any_of(
        plan.scopes.begin(), plan.scopes.end(),
        [](const VulkanPhysicalScopePlan &scope) {
            return scope.kind == VulkanPhysicalScopeKind::transfer &&
                   scope.nodes ==
                       std::vector<std::string>{"OpaqueSnapshot"};
        }));
}

TEST_CASE("target lowering enforces dialect legality and one-way probe capability closure",
          "[target-render-planning][dialect][capability]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);
    auto workspace = makeTargetLoweringGraph(
        types, graph, bindingsFor(types, graph), fixtureOrder(graph));
    REQUIRE_NOTHROW(validateTargetLoweringGraphDialect(
        workspace, TargetLoweringStage::canonical_workspace));

    workspace.nodes.front().dialect =
        TargetIrDialect::physical_vulkan;
    requireThrowsContaining(
        [&] {
            validateTargetLoweringGraphDialect(
                workspace,
                TargetLoweringStage::canonical_workspace);
        },
        "dialect is illegal");

    const auto plan = compile(types, graph, fixtureOrder(graph), topology(true),
                              bindingsFor(types, graph));
    const auto selected = std::find_if(
        plan.backend_selection.candidates.begin(),
        plan.backend_selection.candidates.end(),
        [&](const BackendProbeResult &candidate) {
            return candidate.candidate ==
                   plan.backend_selection.selected_candidate;
        });
    REQUIRE(selected != plan.backend_selection.candidates.end());
    auto grown = plan.required_physical_features;
    grown.push_back("game.experimental.unknown_feature@1");
    requireThrowsContaining(
        [&] {
            validateVulkanPhysicalFeatureClosure(*selected, grown);
        },
        "pelican.plan.lowering_capability_growth@1");
}

TEST_CASE("attachment budget is a target fact rather than a planner G-buffer constant",
          "[target-render-planning][gbuffer][budget]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto within_budget = hybridGraph(types, 7, false);
    REQUIRE_NOTHROW(compile(
        types, within_budget, fixtureOrder(within_budget), topology(true, 7),
        bindingsFor(types, within_budget)));

    const auto over_budget = hybridGraph(types, 9, false);
    requireThrowsContaining(
        [&] {
            (void)compile(types, over_budget, fixtureOrder(over_budget), topology(true, 8),
                          bindingsFor(types, over_budget));
        },
        "pelican.plan.color_attachment_budget_exceeded@1");
}

TEST_CASE("non-overlapping materialized resource lifetimes become legal alias groups",
          "[target-render-planning][lifetime][alias]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto image = sceneLinearHdrV1(types);
    CompiledLogicalRenderGraph graph;
    graph.name = "alias_lifetimes";
    graph.resources = {
        {.name = "temporary_a", .type = image},
        {.name = "temporary_b", .type = image},
    };

    LogicalGraphNode write_a;
    write_a.name = "WriteA";
    addWrite(types, write_a, graph.resources[0], 1, "out");
    LogicalGraphNode read_a;
    read_a.name = "ReadA";
    addRead(types, read_a, graph.resources[0], 1, "in",
            LogicalReadFootprintKind::same_pixel);
    LogicalGraphNode write_b;
    write_b.name = "WriteB";
    write_b.after = {"ReadA"};
    addWrite(types, write_b, graph.resources[1], 1, "out");
    LogicalGraphNode read_b;
    read_b.name = "ReadB";
    addRead(types, read_b, graph.resources[1], 1, "in",
            LogicalReadFootprintKind::same_pixel);
    graph.nodes = {std::move(read_b), std::move(write_b),
                   std::move(read_a), std::move(write_a)};
    validateCompiledLogicalRenderGraph(types, graph);

    const auto plan = compile(types, graph, fixtureOrder(graph), topology(false),
                              bindingsFor(types, graph));
    REQUIRE(std::find(
                plan.opportunities.alias_candidates.begin(),
                plan.opportunities.alias_candidates.end(),
                PlanningNamePair{"temporary_a", "temporary_b"}) !=
            plan.opportunities.alias_candidates.end());
    REQUIRE(plan.alias_groups.size() == 1);
    REQUIRE(plan.alias_groups.front().resources ==
            std::vector<std::string>{"temporary_a", "temporary_b"});

    auto mixed_dimensions =
        bindingsFor(types, graph);
    for (auto &binding : mixed_dimensions) {
        binding.array_layers = 6;
        binding.extent =
            ResourceExtentPlan{
                .kind =
                    ResourceExtentKind::fixed,
                .width = 128,
                .height = 128,
            };
        if (binding.resource == "temporary_a") {
            binding.dimension =
                ImageResourceDimension::cube;
        }
    }
    const auto mixed_plan =
        compile(types, graph, fixtureOrder(graph), topology(false),
                std::move(mixed_dimensions));
    REQUIRE(std::find(
                mixed_plan.opportunities
                    .alias_candidates.begin(),
                mixed_plan.opportunities
                    .alias_candidates.end(),
                PlanningNamePair{
                    "temporary_a",
                    "temporary_b"}) ==
            mixed_plan.opportunities
                .alias_candidates.end());
    REQUIRE(mixed_plan.alias_groups.empty());
}

TEST_CASE("RC-6c E1 reasons retain only used resources beyond their final graph use",
          "[target-render-planning][rc6c][rc6c-e1]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    auto graph = hybridGraph(types, 3, false);
    graph.resources.push_back({.name = "unused", .type = sceneLinearHdrV1(types)});
    const auto order = fixtureOrder(graph);
    const auto plain = calculateTargetResourceLifetimes(graph, order, {});
    REQUIRE(plain.at("gbuffer_0").last_use < order.nodes().size() - 1);
    for (const auto *reason : {"present", "history", "persistent", "external_depth_export", "xr_mirror", "picking", "capture"}) {
        CAPTURE(reason);
        const auto retained = calculateTargetResourceLifetimes(graph, order,
            {{"gbuffer_0", {reason}}, {"unused", {reason}}});
        CHECK(retained.at("gbuffer_0").retain == std::vector<std::string>{reason});
        CHECK(retained.at("gbuffer_0").first_use == plain.at("gbuffer_0").first_use);
        CHECK(retained.at("gbuffer_0").last_use == order.nodes().size() - 1);
        CHECK_FALSE(retained.at("unused").used);
        CHECK(retained.at("unused").retain.empty());
        CHECK(calculateTargetResourceLifetimes(graph, order, {}).at("gbuffer_0") == plain.at("gbuffer_0"));
    }
    const auto overlap = calculateTargetResourceLifetimes(graph, order,
        {{"gbuffer_0", {"persistent", "history", "history"}}});
    CHECK(overlap.at("gbuffer_0").retain == std::vector<std::string>{"history", "persistent"});
}

TEST_CASE("RC-6c E2 complete independently verifies shortened lifetimes and retain reasons",
          "[target-render-planning][rc6c][rc6c-e2][complete]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    auto graph = hybridGraph(types, 3, false);
    // This required + stored resource used to be extended without a reader.
    graph.resources.front().materialization = LogicalMaterializationRequirement::required;
    auto bindings = bindingsFor(types, graph);
    bindings.front().pattern.require_store = true;
    const auto order = fixtureOrder(graph);
    const auto target = topology(false);
    const auto plan = compileVulkanTargetPlan(types, graph, target, providers(), {
        .node_order = order, .endpoint = "device:0", .provider = std::string{kProvider},
        .pattern_bindings = bindings, .retain_reasons = {{"gbuffer_1", {"picking"}}}});
    REQUIRE(physicalResource(plan, "gbuffer_0").lifetime.last_use == 1);
    REQUIRE(physicalResource(plan, "gbuffer_1").lifetime.last_use == order.nodes().size() - 1);
    const auto package = ejectVulkanCompletePhysicalPlanPackage(plan);
    const auto json = vulkanCompletePhysicalPlanPackageToJson(package);
    auto parsed = vulkanCompletePhysicalPlanPackageFromJson(json);
    CHECK(vulkanCompletePhysicalPlanPackageToJson(parsed) == json);
    REQUIRE_NOTHROW(verifyVulkanCompletePhysicalPlanPackage(graph, target, plan, order, parsed));
    for (const bool mutate_reason : {false, true}) {
        CAPTURE(mutate_reason);
        auto changed = parsed;
        for (auto &r : changed.resources) {
            if (r.logical_resource != "gbuffer_1") continue;
            if (mutate_reason) r.lifetime.retain = {"history"};
            else r.lifetime.last_use = 1;
        }
        CHECK_THROWS_WITH(verifyVulkanCompletePhysicalPlanPackage(graph, target, plan, order, changed),
            Catch::Matchers::ContainsSubstring("stale lifetime"));
    }
    // Absence is preserved in both dump dialects.
    const auto dump = vulkanTargetPlanToJson(plan);
    for (const auto &r : dump.at("resources"))
        CHECK(r.at("lifetime").contains("retain") == (r.at("logical_resource") == "gbuffer_1"));
    for (const auto &bad : {nlohmann::json::array(), nlohmann::json::array({"unknown"}),
                            nlohmann::json::array({"picking", "picking"}), nlohmann::json{false}}) {
        auto malformed = json;
        malformed["resources"][0]["lifetime"]["retain"] = bad;
        CHECK_THROWS(vulkanCompletePhysicalPlanPackageFromJson(malformed));
    }
}

TEST_CASE("RC-6c E3 required aliasable images without store gain only nonoverlapping aliases",
          "[target-render-planning][rc6c][rc6c-e3]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    CompiledLogicalRenderGraph graph;
    graph.name = "alias_lifetimes";
    graph.resources = {{.name = "a", .type = sceneLinearHdrV1(types), .materialization = LogicalMaterializationRequirement::required},
                       {.name = "b", .type = sceneLinearHdrV1(types), .materialization = LogicalMaterializationRequirement::required}};
    LogicalGraphNode wa, ra, wb, rb;
    wa.name = "WriteA"; ra.name = "ReadA"; wb.name = "WriteB"; rb.name = "ReadB";
    wb.after = {"ReadA"};
    addWrite(types, wa, graph.resources[0], 1, "out");
    addRead(types, ra, graph.resources[0], 1, "in", LogicalReadFootprintKind::same_pixel);
    addWrite(types, wb, graph.resources[1], 1, "out");
    addRead(types, rb, graph.resources[1], 1, "in", LogicalReadFootprintKind::same_pixel);
    graph.nodes = {wa, ra, wb, rb};
    auto bindings = bindingsFor(types, graph);
    for (auto &b : bindings) {
        b.pattern.require_store = false;
        b.pattern.allow_alias = true;
    }
    const auto order = fixtureOrder(graph);
    const auto plan = compile(types, graph, order, topology(false), bindings);
    REQUIRE_FALSE(plan.alias_groups.empty());
    for (const auto &group : plan.alias_groups)
        for (std::size_t i = 0; i < group.resources.size(); ++i)
            for (std::size_t j = i + 1; j < group.resources.size(); ++j) {
                const auto &a = physicalResource(plan, group.resources[i]).lifetime;
                const auto &b = physicalResource(plan, group.resources[j]).lifetime;
                CHECK((a.last_use < b.first_use || b.last_use < a.first_use));
            }
    TargetResourceRetainReasons old_tail;
    for (const auto &r : graph.resources) old_tail[r.name] = {"persistent"};
    const auto previous = compileVulkanTargetPlan(types, graph, topology(false), providers(), {
        .node_order = order, .endpoint = "device:0", .provider = std::string{kProvider},
        .pattern_bindings = bindings, .retain_reasons = old_tail});
    CHECK(previous.alias_groups.empty());
}

TEST_CASE("target lowering resolves sample counts on physical resources and scopes",
          "[target-render-planning][sample-count][gbuffer]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 6, false, true);
    SampleCountPolicy policy{
        .request = {SampleCountRequestMode::prefer, 4},
        .scope = SampleCountScope::geometry,
        .authored = true,
    };
    const auto plan = compile(
        types, graph, fixtureOrder(graph), topology(false), bindingsFor(types, graph),
        sampleCountRequest(graph, policy,
                           "gbuffer_custom_attribute"));

    REQUIRE(plan.sample_count_plan.has_value());
    REQUIRE(physicalResource(
                plan, "gbuffer_custom_attribute")
                .rasterization_samples == 2);
    REQUIRE(physicalResource(plan, "scene_color")
                .rasterization_samples == 2);
    REQUIRE(physicalResource(plan, "scene_color")
                .resolve_required);
    REQUIRE(std::all_of(
        plan.scopes.begin(), plan.scopes.end(),
        [](const VulkanPhysicalScopePlan &scope) {
            return scope.kind !=
                       VulkanPhysicalScopeKind::rendering ||
                   scope.rasterization_samples == 2;
        }));
    REQUIRE(plan.sample_count_plan->groups.at(0)
                .limiting_resources ==
            std::vector<std::string>{
                "gbuffer_custom_attribute "
                "(R16G16B16A16_SFLOAT)"});
    const auto json = vulkanTargetPlanToJson(plan);
    REQUIRE(json.at("sample_count_plan")
                .at("resources")
                .is_array());
    REQUIRE(json.at("resources").at(0).contains(
        "rasterization_samples"));
}

TEST_CASE("exact target sample planning reports a user-added attachment",
          "[target-render-planning][sample-count][diagnostic]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 6, false);
    SampleCountPolicy policy{
        .request = {SampleCountRequestMode::exact, 4},
        .scope = SampleCountScope::geometry,
        .authored = true,
    };
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, fixtureOrder(graph), topology(false),
                bindingsFor(types, graph),
                sampleCountRequest(
                    graph, policy,
                    "gbuffer_custom_attribute"));
        },
        "gbuffer_custom_attribute (R16G16B16A16_SFLOAT)");
}

TEST_CASE("explicit sample target selects its attachment component and blocks incompatible scope fusion",
          "[target-render-planning][sample-count][tile]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 5, false);
    SampleCountPolicy policy{
        .request = {SampleCountRequestMode::exact, 4},
        .scope = SampleCountScope::none,
        .targets = {"gbuffer_0"},
        .authored = true,
    };
    const auto plan = compile(
        types, graph, fixtureOrder(graph), topology(true), bindingsFor(types, graph),
        sampleCountRequest(graph, policy));

    REQUIRE(physicalResource(plan, "gbuffer_0")
                .rasterization_samples == 4);
    REQUIRE(physicalResource(plan, "gbuffer_4")
                .rasterization_samples == 4);
    REQUIRE(physicalResource(plan, "scene_color")
                .rasterization_samples == 1);
    REQUIRE_FALSE(oneScopeContains(plan, "GBuffer", "Lighting"));
}

TEST_CASE("RC-1 image bytes count every mip, layer and history surface and the multisampled image",
          "[target-render-planning][rc1][memory]") {
    // RC-3a1: the format table's sizes (RC-1's values).
    CHECK(imageFormatEstimatedTexelBytes(ImageFormat::r8_unorm) == 1);
    CHECK(imageFormatEstimatedTexelBytes(ImageFormat::r16_sfloat) == 2);
    CHECK(imageFormatEstimatedTexelBytes(ImageFormat::b8g8r8a8_srgb) == 4);
    CHECK(imageFormatEstimatedTexelBytes(ImageFormat::d32_sfloat) == 4);
    CHECK(imageFormatEstimatedTexelBytes(ImageFormat::r16g16b16a16_sfloat) == 8);
    CHECK(imageFormatEstimatedTexelBytes(ImageFormat::d32_sfloat_s8_uint) == 8);
    CHECK(imageFormatEstimatedTexelBytes(ImageFormat::r32g32b32a32_sfloat) == 16);

    CHECK(imageResourceBytes({.format = ImageFormat::r8g8b8a8_unorm,
                              .extent = {1920, 1080}}) == 8'294'400);
    // 8x4, 4x2, 2x1, 1x1: 43 texels.
    CHECK(imageResourceBytes({.format = ImageFormat::r8_unorm,
                              .extent = {8, 4},
                              .mip_levels = 4}) == 43);
    CHECK(imageResourceBytes({.format = ImageFormat::r8_unorm,
                              .extent = {8, 4},
                              .mip_levels = 4,
                              .array_layers = 6}) == 6 * 43);
    CHECK(imageResourceBytes({.format = ImageFormat::r16g16b16a16_sfloat,
                              .extent = {4, 4},
                              .surfaces = 2}) == 2 * 8 * 16);
    // The sampled image with its two mips (8x4 + 4x2 texels) and the 4x
    // image the passes render into (8x4 texels, four samples each), for
    // each of the two history surfaces.
    CHECK(imageResourceBytes({.format = ImageFormat::r8g8b8a8_unorm,
                              .extent = {8, 4},
                              .mip_levels = 2,
                              .samples = 4,
                              .surfaces = 2}) ==
          2 * (4 * (32 + 8) + 4 * 32 * 4));
    CHECK(imageResourceBytes({.format = ImageFormat::r8_unorm,
                              .extent = {0, 8}}) == 0);

    VulkanPhysicalResourcePlan bloom{
        .logical_resource = "bloom",
        .format = ImageFormat::b8g8r8a8_srgb,
        .mip_levels = {ImageMipLevelMode::full_chain, 1},
    };
    // A full chain at 16x8: 128 + 32 + 8 + 2 + 1 texels.
    CHECK(vulkanPhysicalResourceBytes(bloom, {{16, 8}, 1}) == 4 * 171);
    bloom.array_layers = 2;
    bloom.rasterization_samples = 2;
    CHECK(vulkanPhysicalResourceBytes(bloom, {{16, 8}, 1}) ==
          2 * 4 * 171 + 2 * 2 * 4 * 128);
    VulkanPhysicalResourcePlan swapchain{
        .logical_resource = "swapchain",
        .format = ImageFormat::b8g8r8a8_srgb,
        .representation = VulkanResourceRepresentation::external,
    };
    CHECK_FALSE(vulkanPhysicalResourceIsPlannedImage(swapchain));
    CHECK(vulkanPhysicalResourceBytes(swapchain, {{16, 8}, 1}) == 0);
}

TEST_CASE("RC-1 memory summary counts an alias group once and finds the node most bytes are alive at",
          "[target-render-planning][rc1][memory]") {
    const auto node = [](std::string name) {
        return TargetLoweringNode{
            .logical = LogicalGraphNode{.name = std::move(name)}};
    };
    const auto image = [](std::string name, ImageFormat format,
                          std::size_t first, std::size_t last) {
        return VulkanPhysicalResourcePlan{
            .logical_resource = std::move(name),
            .format = format,
            .lifetime = {true, first, last},
        };
    };
    VulkanTargetPlan plan;
    plan.lowering_graph.nodes = {node("geometry"), node("blur"),
                                 node("composite"), node("present")};
    plan.resources = {
        image("depth", ImageFormat::d32_sfloat, 0, 3),
        image("first", ImageFormat::r8g8b8a8_unorm, 0, 1),
        image("second", ImageFormat::r8g8b8a8_unorm, 2, 3),
        image("small", ImageFormat::r8_unorm, 1, 2),
        image("swapchain", ImageFormat::b8g8r8a8_srgb, 3, 3),
        image("unknown", ImageFormat::r8g8b8a8_unorm, 0, 3),
    };
    plan.resources[4].representation =
        VulkanResourceRepresentation::external;
    plan.alias_groups = {{"alias:0", {"first", "second"}}};
    plan.graph = "main";

    std::vector<std::string> asked;
    const auto summary = summarizeVulkanTargetPlanMemory(
        plan, [&](const VulkanPhysicalResourcePlan &resource) {
            asked.push_back(resource.logical_resource);
            return vulkanPhysicalResourceBytes(resource, {{10, 10}, 1});
        });
    // Never asked for the swapchain: the plan does not allocate it.
    CHECK(asked == std::vector<std::string>{"depth", "first", "second",
                                            "small", "unknown"});
    REQUIRE(summary.resource_bytes.size() == 6);
    CHECK(summary.resource_bytes[0] == 400);
    CHECK(summary.resource_bytes[1] == 400);
    CHECK(summary.resource_bytes[2] == 400);
    CHECK(summary.resource_bytes[3] == 100);
    CHECK_FALSE(summary.resource_bytes[4].has_value());
    CHECK(summary.resource_bytes[5] == 400);
    // geometry 1200, blur 1300, composite 1300, present 1200: the first 1300.
    CHECK(summary.peak_live_bytes == 1300);
    CHECK(summary.peak_order == 1);
    CHECK(summary.peak_node == "blur");

    // RC-1b: a caller that cannot say how large an image is says so by
    // name; the summary does not leave the image out.
    CHECK_THROWS_WITH(
        summarizeVulkanTargetPlanMemory(
            plan,
            [](const VulkanPhysicalResourcePlan &resource) -> std::uint64_t {
                if (resource.logical_resource == "unknown") {
                    throw std::runtime_error("cannot count unknown");
                }
                return 1;
            }),
        "cannot count unknown");

    // What registration made of the plan's targets: first and second share
    // one allocation, as the plan's alias group says.
    const auto registered = [](std::string name, std::uint64_t bytes,
                               std::optional<std::uint64_t> alias_group) {
        return RegisteredTargetMemory{
            .name = std::move(name),
            .bytes = bytes,
            .alias_shared_bytes = bytes,
            .alias_group = alias_group,
        };
    };
    const std::vector<RegisteredTargetMemory> allocation{
        registered("depth", 400, std::nullopt),
        registered("first", 400, 1),
        registered("second", 400, 1),
        registered("small", 100, std::nullopt),
        registered("unknown", 400, std::nullopt),
    };
    nlohmann::json frame_plan{
        {"physical_target_plan",
         {{"resources",
           nlohmann::json::array({
               {{"logical_resource", "depth"}},
               {{"logical_resource", "first"}},
               {{"logical_resource", "second"}},
               {{"logical_resource", "small"}},
               {{"logical_resource", "swapchain"}},
               {{"logical_resource", "unknown"}},
           })}}},
    };
    appendVulkanTargetPlanMemoryJson(
        frame_plan, plan, summary, totalRegisteredTargetMemory(allocation));
    // depth + one allocation for first/second + small + unknown.
    CHECK(frame_plan.at("memory_summary") ==
          nlohmann::json{{"total_bytes", 1300},
                         {"unused_bytes", 0},
                         {"unused_resources", nlohmann::json::array()},
                         {"unallocated_resources", nlohmann::json::array()},
                         {"peak_live_bytes", 1300},
                         {"peak_node", "blur"},
                         {"peak_graph", "main"}, {"payload_bytes",1700}, {"planned_region_bytes",0}, {"planned_region_count",0}, {"actual_placement",nullptr}});
    const auto &resources =
        frame_plan.at("physical_target_plan").at("resources");
    CHECK(resources[0].at("bytes") == 400);
    CHECK(resources[2].at("bytes") == 400);
    CHECK(resources[3].at("bytes") == 100);
    CHECK_FALSE(resources[4].contains("bytes"));
    CHECK(resources[5].at("bytes") == 400);

    // Nothing counted: no peak, and no node named; still the graph's.
    const auto empty = summarizeVulkanTargetPlanMemory(
        plan, [](const VulkanPhysicalResourcePlan &) {
            return std::uint64_t{0};
        });
    CHECK(empty.peak_live_bytes == 0);
    CHECK_FALSE(empty.peak_order.has_value());
    nlohmann::json empty_plan = nlohmann::json::object();
    appendVulkanTargetPlanMemoryJson(empty_plan, plan, empty, {});
    CHECK(empty_plan.at("memory_summary").at("peak_node").is_null());
    CHECK(empty_plan.at("memory_summary").at("peak_graph") == "main");
    CHECK(empty_plan.at("memory_summary").at("total_bytes") == 0);
}

TEST_CASE("RC-1 the memory total counts each allocation registration made once and names "
          "the targets no graph uses",
          "[target-render-planning][rc1][memory]") {
    const auto target = [](std::string name, std::uint64_t bytes,
                           std::uint64_t shared,
                           std::optional<std::uint64_t> alias_group,
                           bool used = true) {
        return RegisteredTargetMemory{
            .name = std::move(name),
            .bytes = bytes,
            .alias_shared_bytes = shared,
            .alias_group = alias_group,
            .used = used,
        };
    };
    // Two targets the runtime kept apart are two allocations, whatever a
    // plan's own alias group said.
    CHECK(totalRegisteredTargetMemory(std::vector{
                                          target("a", 16'384, 16'384, std::nullopt),
                                          target("b", 16'384, 16'384, std::nullopt),
                                      })
              .total_bytes == 32'768);
    // Members of one alias group share their single-sample images, once at
    // the largest; each keeps the multisampled image it renders into
    // (4x: 4 x 1000 of its own besides the 1000 it shares).
    const auto aliased = totalRegisteredTargetMemory(std::vector{
        target("owner", 5'000, 1'000, 7),
        target("member", 5'000, 1'000, 7),
        target("other", 300, 300, std::nullopt),
    });
    CHECK(aliased.total_bytes == 1'000 + 2 * 4'000 + 300);
    CHECK(aliased.unused_bytes == 0);
    CHECK(aliased.unused_resources.empty());

    // A target no graph uses is still allocated: it counts, and it is named.
    // An alias group's shared images are unused only when no member is used.
    const auto unused = totalRegisteredTargetMemory(std::vector{
        target("zeta", 4'096, 4'096, std::nullopt, false),
        target("used_member", 600, 600, 3),
        target("unused_member", 600, 600, 3, false),
        target("alpha", 100, 100, std::nullopt, false),
        target("lonely_a", 200, 200, 4, false),
        target("lonely_b", 200, 200, 4, false),
    });
    CHECK(unused.total_bytes == 4'096 + 600 + 100 + 200);
    CHECK(unused.unused_bytes == 4'096 + 100 + 200);
    CHECK(unused.unused_resources ==
          std::vector<std::string>{"alpha", "lonely_a", "lonely_b",
                                   "unused_member", "zeta"});

    CHECK_THROWS_AS(totalRegisteredTargetMemory(std::vector{
                        target("broken", 10, 20, 1)}),
                    std::invalid_argument);
}


TEST_CASE("RC-7c measured totals count each region allocation once", "[rc7c][memory]") {
    const std::vector<RegisteredTargetMemory> measured{
        {.name="A",.bytes=100,.used=true,.allocations={{1000,1024}}},
        {.name="B",.bytes=200,.used=false,.allocations={{1000,1024}}},
        {.name="C",.bytes=50,.used=false,.allocations={{2000,256}}},
    };
    const auto total=totalRegisteredTargetMemory(measured);
    CHECK(total.total_bytes==1280); CHECK(total.unused_bytes==256);
    CHECK(total.unused_resources==std::vector<std::string>{"B","C"});
}

#include "rc6a2_target_policy_test.inc"
#include "rc6b1_target_policy_test.inc"
#include "rc7a_target_policy_test.inc"
#include "rc7b_target_policy_test.inc"
#include "rc7c_target_policy_test.inc"
#include "rc7d_target_test.inc"
} // namespace Pelican
