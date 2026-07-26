#include "../src/project/targetrenderplanning.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
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
                      {"B8G8R8A8_UNORM",
                       {std::string{
                           kDisplayFormatCapability}}},
                  }
            : depth
                ? std::vector<ResourceFormatCandidate>{
                      {"D32_SFLOAT",
                       {std::string{
                           kDepthFormatCapability}}},
                  }
                : std::vector<ResourceFormatCandidate>{
                      {"R16G16B16A16_SFLOAT",
                       {std::string{
                           kHdrFormatCapability}}},
                      {"R8G8B8A8_UNORM",
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

VulkanTargetPlan compile(
    const LogicalTypeRegistry &types,
    const CompiledLogicalRenderGraph &graph,
    TargetTopologySnapshot target,
    std::vector<ResourcePatternBinding> bindings,
    std::optional<VulkanSampleCountPlanRequest> sample_count =
        std::nullopt,
    std::optional<VulkanViewExecutionPlanRequest> view_execution =
        std::nullopt,
    std::optional<VulkanExternalDepthExportRequest>
        external_depth_export = std::nullopt,
    std::optional<VulkanTargetPlanPinPackage>
        pin_package = std::nullopt,
    std::optional<VulkanPhysicalFragmentPackage>
        fragment_package = std::nullopt,
    std::vector<VulkanPhysicalResourceFormatCapability>
        format_capabilities = {},
    std::vector<VulkanPhysicalAttachmentPlan>
        attachments = {},
    PlanningProfile profile = {}) {
    return compileVulkanTargetPlan(
        types, graph, target, providers(),
        VulkanTargetPlanRequest{
            .endpoint = "device:0",
            .provider = std::string{kProvider},
            .pattern_bindings = std::move(bindings),
            .profile = profile,
            .sample_count = std::move(sample_count),
            .view_execution = std::move(view_execution),
            .external_depth_export =
                std::move(external_depth_export),
            .pin_package =
                std::move(pin_package),
            .fragment_package =
                std::move(fragment_package),
            .fragment_format_capabilities =
                std::move(format_capabilities),
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
                    ? "D32_SFLOAT"
                    : "R16G16B16A16_SFLOAT",
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
        compile(types, graph, topology(false), bindings);
    REQUIRE(desktop.backend_selection.selected_candidate ==
            "pelican.vulkan.materialized_plan@1");
    for (std::size_t index = 0; index < 6; ++index) {
        const auto name =
            index == 5 ? "gbuffer_custom_attribute"
                       : "gbuffer_" + std::to_string(index);
        REQUIRE(physicalResource(desktop, name).representation ==
                VulkanResourceRepresentation::materialized_image);
    }

    const auto tile = compile(types, graph, topology(true), bindings);
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
        compile(types, graph, topology(true), std::move(bindings));
    REQUIRE(vulkanTargetPlanToJson(tile).dump() ==
            vulkanTargetPlanToJson(reordered).dump());
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
                VulkanPhysicalAttachmentAspect::color,
            .load_op =
                VulkanPhysicalAttachmentLoadOp::clear,
            .store_op =
                VulkanPhysicalAttachmentStoreOp::store,
        },
    };

    const auto automatic = compile(
        types, graph, transient_target,
        bindingsFor(types, graph),
        std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, {},
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
        automatic.attachments.front().store_op ==
        VulkanPhysicalAttachmentStoreOp::discard);
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

    auto materialized =
        ejectVulkanPhysicalFragmentPackage(
            automatic);
    const auto scratch_resource =
        std::find_if(
            materialized.resources.begin(),
            materialized.resources.end(),
            [](const auto &resource) {
                return resource.logical_resource ==
                       "write_only_scratch";
            });
    REQUIRE(
        scratch_resource !=
        materialized.resources.end());
    scratch_resource->representation =
        VulkanResourceRepresentation::
            materialized_image;
    REQUIRE(materialized.attachments.has_value());
    materialized.attachments->front().store_op =
        VulkanPhysicalAttachmentStoreOp::store;
    const auto overridden = compile(
        types, graph, transient_target,
        bindingsFor(types, graph),
        std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, materialized, {},
        attachments);
    REQUIRE(
        physicalResource(
            overridden, "write_only_scratch")
            .representation ==
        VulkanResourceRepresentation::
            materialized_image);
    REQUIRE(
        overridden.attachments.front().store_op ==
        VulkanPhysicalAttachmentStoreOp::store);

    const auto conservative = compile(
        types, graph, transient_target,
        bindingsFor(types, graph),
        std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, {},
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
        conservative.attachments.front().store_op ==
        VulkanPhysicalAttachmentStoreOp::store);
}

TEST_CASE("Vulkan target plan pins round-trip and bind a logical graph",
          "[target-render-planning][pin][eject]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 4, false, true);
    const auto automatic = compile(
        types, graph, topology(true),
        bindingsFor(types, graph));
    REQUIRE(automatic.backend_selection.selected_candidate ==
            "pelican.vulkan.tile_local_plan@1");
    REQUIRE(automatic.logical_graph_fingerprint ==
            vulkanTargetPlanLogicalGraphFingerprint(graph));

    const auto ejected =
        ejectVulkanTargetPlanPinPackage(automatic);
    const auto document =
        vulkanTargetPlanPinPackageToJson(ejected);
    REQUIRE(
        vulkanTargetPlanPinPackageFromJson(document) ==
        ejected);
    REQUIRE(document.at("schema") ==
            "pelican.vulkan_target_plan_pins");
    REQUIRE(document.at("version") == 1);
    REQUIRE(document.at("logical_graph_fingerprint")
                .get<std::string>()
                .starts_with("fnv1a64:"));

    auto materialized_pin = ejected;
    materialized_pin.backend_candidate =
        "pelican.vulkan.materialized_plan@1";
    const auto pinned = compile(
        types, graph, topology(true),
        bindingsFor(types, graph), std::nullopt,
        std::nullopt, std::nullopt,
        materialized_pin);
    REQUIRE(pinned.backend_selection.selected_candidate ==
            "pelican.vulkan.materialized_plan@1");
    REQUIRE(pinned.applied_pin_package ==
            materialized_pin);
    const auto pinned_json =
        vulkanTargetPlanToJson(pinned);
    REQUIRE(pinned_json.at("applied_pin_package") ==
            vulkanTargetPlanPinPackageToJson(
                materialized_pin));
    REQUIRE(pinned_json.at("ejectable_pin_package")
                .at("pins")
                .at("backend_candidate") ==
            "pelican.vulkan.materialized_plan@1");

    auto stale = materialized_pin;
    stale.logical_graph_fingerprint ^= 1;
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, topology(true),
                bindingsFor(types, graph), std::nullopt,
                std::nullopt, std::nullopt, stale);
        },
        "pin package is stale");

    auto wrong_graph = materialized_pin;
    wrong_graph.graph = "other";
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, topology(true),
                bindingsFor(types, graph), std::nullopt,
                std::nullopt, std::nullopt,
                wrong_graph);
        },
        "pin package graph mismatch");

    auto malformed = document;
    malformed["pins"]["representation"] =
        "tile_local_attachment";
    requireThrowsContaining(
        [&] {
            (void)vulkanTargetPlanPinPackageFromJson(
                malformed);
        },
        "unknown key 'representation'");
}

TEST_CASE("Vulkan physical fragments round-trip against an automatic target environment",
          "[target-render-planning][physical-fragment][eject]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
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
    scene_color->mip_levels = {
        ImageMipLevelMode::full_chain, 1};
    scene_color->array_layers = 3;
    const auto automatic = compile(
        types, graph, topology(true),
        bindings);
    REQUIRE(
        physicalResource(
            automatic, "scene_color")
            .mip_levels ==
        scene_color->mip_levels);
    REQUIRE(
        physicalResource(
            automatic, "scene_color")
            .array_layers == 3);

    const auto ejected =
        ejectVulkanPhysicalFragmentPackage(
            automatic);
    const auto document =
        vulkanPhysicalFragmentPackageToJson(
            ejected);
    REQUIRE(document.at("schema") ==
            "pelican.vulkan_physical_fragment");
    REQUIRE(document.at("version") == 3);
    REQUIRE(
        document.at("scope_edit_mode") ==
        "dependency_safe");
    REQUIRE(document.at("automatic_plan_fingerprint")
                .get<std::string>()
                .starts_with("fnv1a64:"));
    REQUIRE(
        vulkanPhysicalFragmentPackageFromJson(
            document) == ejected);

    const auto linked = compile(
        types, graph, topology(true),
        bindings, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt,
        ejected);
    REQUIRE(linked.resources == automatic.resources);
    REQUIRE(linked.scopes == automatic.scopes);
    REQUIRE(linked.alias_groups ==
            automatic.alias_groups);
    REQUIRE(linked.required_physical_features ==
            automatic.required_physical_features);
    REQUIRE(linked.applied_fragment_package ==
            ejected);
    REQUIRE(
        ejectVulkanPhysicalFragmentPackage(
            linked) == ejected);

    const auto encoded =
        vulkanTargetPlanToJson(linked);
    const auto encoded_scene_color =
        std::find_if(
            encoded.at("resources").begin(),
            encoded.at("resources").end(),
            [](const auto &resource_json) {
                return resource_json.at(
                           "logical_resource") ==
                       "scene_color";
            });
    REQUIRE(
        encoded_scene_color !=
        encoded.at("resources").end());
    REQUIRE(
        encoded_scene_color->at("mip_levels")
            .at("mode") == "full");
    REQUIRE(
        encoded_scene_color->at("array_layers") ==
        3);
    REQUIRE(encoded.at(
                "ejectable_physical_fragment") ==
            document);
    REQUIRE(encoded.at(
                "applied_physical_fragment") ==
            document);
}

TEST_CASE("Vulkan physical attachment fragments preserve logical dependencies and resolve semantics",
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
                    VulkanPhysicalAttachmentAspect::color,
                .load_op =
                    VulkanPhysicalAttachmentLoadOp::clear,
                .store_op =
                    VulkanPhysicalAttachmentStoreOp::store,
            },
            {
                .node = "Forward",
                .logical_resource = "scene_color",
                .aspect =
                    VulkanPhysicalAttachmentAspect::color,
                .load_op =
                    VulkanPhysicalAttachmentLoadOp::load,
                .store_op =
                    VulkanPhysicalAttachmentStoreOp::store,
            },
            {
                .node = "Lighting",
                .logical_resource = "scene_color",
                .aspect =
                    VulkanPhysicalAttachmentAspect::color,
                .load_op =
                    VulkanPhysicalAttachmentLoadOp::clear,
                .store_op =
                    VulkanPhysicalAttachmentStoreOp::store,
            },
            {
                .node = "ToneMap",
                .logical_resource = "display_output",
                .aspect =
                    VulkanPhysicalAttachmentAspect::color,
                .load_op =
                    VulkanPhysicalAttachmentLoadOp::clear,
                .store_op =
                    VulkanPhysicalAttachmentStoreOp::store,
            },
        };
    };
    const auto automatic = compile(
        types, graph, topology(false),
        bindingsFor(types, graph),
        std::nullopt, std::nullopt,
        std::nullopt, std::nullopt,
        std::nullopt, {},
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
            types, temporal_self_read,
            topology(false),
            bindingsFor(
                types, temporal_self_read),
            std::nullopt, std::nullopt,
            std::nullopt, std::nullopt,
            std::nullopt, {},
            attachment_contract()));

    const auto ejected =
        ejectVulkanPhysicalFragmentPackage(
            automatic);
    REQUIRE(ejected.schema_version == 3);
    REQUIRE(
        ejected.scope_edit_mode ==
        VulkanPhysicalScopeEditMode::
            dependency_safe);
    REQUIRE(ejected.attachments.has_value());
    REQUIRE(ejected.attachments->size() == 4);
    const auto document =
        vulkanPhysicalFragmentPackageToJson(
            ejected);
    REQUIRE(document.at("version") == 3);
    REQUIRE(document.at("attachments").size() == 4);
    REQUIRE(
        vulkanPhysicalFragmentPackageFromJson(
            document) == ejected);

    const auto linked = compile(
        types, graph, topology(false),
        bindingsFor(types, graph),
        std::nullopt, std::nullopt,
        std::nullopt, std::nullopt,
        ejected, {}, attachment_contract());
    REQUIRE(linked.attachments ==
            automatic.attachments);

    auto discard_clear = ejected;
    const auto gbuffer = std::find_if(
        discard_clear.attachments->begin(),
        discard_clear.attachments->end(),
        [](const auto &attachment) {
            return attachment.node == "GBuffer";
        });
    REQUIRE(gbuffer !=
            discard_clear.attachments->end());
    gbuffer->load_op =
        VulkanPhysicalAttachmentLoadOp::discard;
    const auto discard_clear_linked = compile(
        types, graph, topology(false),
        bindingsFor(types, graph),
        std::nullopt, std::nullopt,
        std::nullopt, std::nullopt,
        discard_clear, {},
        attachment_contract());
    REQUIRE(
        discard_clear_linked.attachments.front().load_op ==
        VulkanPhysicalAttachmentLoadOp::discard);

    auto erase_load = ejected;
    const auto forward = std::find_if(
        erase_load.attachments->begin(),
        erase_load.attachments->end(),
        [](const auto &attachment) {
            return attachment.node == "Forward";
        });
    REQUIRE(forward != erase_load.attachments->end());
    forward->load_op =
        VulkanPhysicalAttachmentLoadOp::clear;
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, topology(false),
                bindingsFor(types, graph),
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                erase_load, {},
                attachment_contract());
        },
        "cannot discard or clear a logical read dependency");

    auto invent_load = ejected;
    std::find_if(
        invent_load.attachments->begin(),
        invent_load.attachments->end(),
        [](const auto &attachment) {
            return attachment.node == "GBuffer";
        })->load_op =
        VulkanPhysicalAttachmentLoadOp::load;
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, topology(false),
                bindingsFor(types, graph),
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                invent_load, {},
                attachment_contract());
        },
        "Load has no logical read dependency");

    auto discard_single_sample_store = ejected;
    std::find_if(
        discard_single_sample_store
            .attachments->begin(),
        discard_single_sample_store
            .attachments->end(),
        [](const auto &attachment) {
            return attachment.node == "GBuffer";
        })->store_op =
        VulkanPhysicalAttachmentStoreOp::discard;
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, topology(false),
                bindingsFor(types, graph),
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                discard_single_sample_store, {},
                attachment_contract());
        },
        "separate multisample resolve");

    const SampleCountPolicy msaa_policy{
        .request =
            {SampleCountRequestMode::exact, 4},
        .scope = SampleCountScope::geometry,
        .authored = true,
    };
    const auto msaa = compile(
        types, graph, topology(false),
        bindingsFor(types, graph),
        sampleCountRequest(graph, msaa_policy),
        std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, {},
        attachment_contract());
    REQUIRE(
        physicalResource(msaa, "gbuffer_0")
            .resolve_required);
    REQUIRE(
        physicalResource(msaa, "scene_color")
            .resolve_required);
    auto discard_resolved_store =
        ejectVulkanPhysicalFragmentPackage(msaa);
    std::find_if(
        discard_resolved_store
            .attachments->begin(),
        discard_resolved_store
            .attachments->end(),
        [](const auto &attachment) {
            return attachment.node == "GBuffer";
        })->store_op =
        VulkanPhysicalAttachmentStoreOp::discard;
    const auto resolved = compile(
        types, graph, topology(false),
        bindingsFor(types, graph),
        sampleCountRequest(graph, msaa_policy),
        std::nullopt, std::nullopt,
        std::nullopt,
        discard_resolved_store, {},
        attachment_contract());
    REQUIRE(
        resolved.attachments.front().store_op ==
        VulkanPhysicalAttachmentStoreOp::discard);

    auto discard_before_attachment_load =
        ejectVulkanPhysicalFragmentPackage(msaa);
    std::find_if(
        discard_before_attachment_load
            .attachments->begin(),
        discard_before_attachment_load
            .attachments->end(),
        [](const auto &attachment) {
            return attachment.node == "Lighting";
        })->store_op =
        VulkanPhysicalAttachmentStoreOp::discard;
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, topology(false),
                bindingsFor(types, graph),
                sampleCountRequest(
                    graph, msaa_policy),
                std::nullopt, std::nullopt,
                std::nullopt,
                discard_before_attachment_load,
                {}, attachment_contract());
        },
        "downstream attachment loads");

    auto v1_with_attachments = document;
    v1_with_attachments["version"] = 1;
    v1_with_attachments.erase(
        "scope_edit_mode");
    requireThrowsContaining(
        [&] {
            (void)vulkanPhysicalFragmentPackageFromJson(
                v1_with_attachments);
        },
        "unknown key 'attachments'");
}

TEST_CASE("physical fragments conservatively materialize tile data before splitting scopes",
          "[target-render-planning][physical-fragment][materialize][scope]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph =
        hybridGraph(types, 4, false, true);
    const auto automatic = compile(
        types, graph, topology(true),
        bindingsFor(types, graph));
    REQUIRE(oneScopeContains(
        automatic, "GBuffer", "Lighting"));

    auto fragment =
        ejectVulkanPhysicalFragmentPackage(
            automatic);
    for (auto &resource_fragment :
         fragment.resources) {
        if (physicalResource(
                automatic,
                resource_fragment
                    .logical_resource)
                .representation ==
            VulkanResourceRepresentation::
                tile_local_attachment) {
            resource_fragment.representation =
                VulkanResourceRepresentation::
                    materialized_image;
        }
    }
    fragment.scopes =
        std::vector<VulkanPhysicalScopeFragment>{};
    for (const auto &scope : automatic.scopes) {
        for (const auto &node : scope.nodes) {
            fragment.scopes->push_back(
                VulkanPhysicalScopeFragment{
                    .id = "manual:" + node,
                    .nodes = {node},
                });
        }
    }
    fragment.alias_groups =
        std::vector<
            VulkanPhysicalAliasGroupFragment>{};

    const auto linked = compile(
        types, graph, topology(true),
        bindingsFor(types, graph), std::nullopt,
        std::nullopt, std::nullopt, std::nullopt,
        fragment);
    REQUIRE_FALSE(oneScopeContains(
        linked, "GBuffer", "Lighting"));
    REQUIRE(std::all_of(
        linked.resources.begin(),
        linked.resources.end(),
        [](const VulkanPhysicalResourcePlan &resource) {
            return resource.representation !=
                   VulkanResourceRepresentation::
                       tile_local_attachment;
        }));
    REQUIRE(linked.scopes.size() ==
            linked.lowering_graph.nodes.size());
    REQUIRE(std::any_of(
        linked.decisions.begin(),
        linked.decisions.end(),
        [](const PlanningDecision &decision) {
            return decision.id ==
                   "pelican.plan.physical_scope_dependency_safe@1";
    }));
}

TEST_CASE(
    "version 3 physical fragments fuse compatible materialized rendering scopes",
    "[target-render-planning][physical-fragment][scope][fusion]") {
    const auto types =
        makeBuiltinLogicalTypeRegistry();
    const auto graph =
        hybridGraph(types, 3, false);
    const std::vector<VulkanPhysicalAttachmentPlan>
        attachments{
            {
                .node = "Lighting",
                .logical_resource =
                    "scene_color",
                .aspect =
                    VulkanPhysicalAttachmentAspect::
                        color,
                .load_op =
                    VulkanPhysicalAttachmentLoadOp::
                        clear,
                .store_op =
                    VulkanPhysicalAttachmentStoreOp::
                        store,
            },
            {
                .node = "Forward",
                .logical_resource =
                    "scene_color",
                .aspect =
                    VulkanPhysicalAttachmentAspect::
                        color,
                .load_op =
                    VulkanPhysicalAttachmentLoadOp::
                        load,
                .store_op =
                    VulkanPhysicalAttachmentStoreOp::
                        store,
            },
        };
    const auto automatic = compile(
        types, graph, topology(false),
        bindingsFor(types, graph),
        std::nullopt, std::nullopt,
        std::nullopt, std::nullopt,
        std::nullopt, {}, attachments);
    auto fragment =
        ejectVulkanPhysicalFragmentPackage(
            automatic);
    REQUIRE(fragment.schema_version == 3);
    REQUIRE(
        fragment.scope_edit_mode ==
        VulkanPhysicalScopeEditMode::
            dependency_safe);

    std::vector<VulkanPhysicalScopeFragment>
        fused_scopes;
    for (const auto &scope :
         *fragment.scopes) {
        if (std::find(
                scope.nodes.begin(),
                scope.nodes.end(),
                "Lighting") !=
            scope.nodes.end()) {
            fused_scopes.push_back({
                .id = "manual:lighting_forward",
                .nodes =
                    {"Lighting", "Forward"},
            });
        } else if (std::find(
                       scope.nodes.begin(),
                       scope.nodes.end(),
                       "Forward") ==
                   scope.nodes.end()) {
            fused_scopes.push_back(scope);
        }
    }
    fragment.scopes =
        std::move(fused_scopes);
    fragment.alias_groups =
        std::vector<
            VulkanPhysicalAliasGroupFragment>{};

    const auto linked = compile(
        types, graph, topology(false),
        bindingsFor(types, graph),
        std::nullopt, std::nullopt,
        std::nullopt, std::nullopt,
        fragment, {}, attachments);
    const auto &scope =
        scopeForNode(linked, "Lighting");
    REQUIRE(
        scope.nodes ==
        std::vector<std::string>{
            "Lighting", "Forward"});
    REQUIRE(scope.single_rendering_instance);
    REQUIRE(scope.local_reads.empty());
    REQUIRE(std::any_of(
        linked.decisions.begin(),
        linked.decisions.end(),
        [](const PlanningDecision &decision) {
            return decision.id ==
                   "pelican.plan.physical_scope_dependency_safe@1";
        }));

    auto non_final_discard = fragment;
    const auto lighting =
        std::find_if(
            non_final_discard
                .attachments->begin(),
            non_final_discard
                .attachments->end(),
            [](const auto &attachment) {
                return attachment.node ==
                       "Lighting";
            });
    REQUIRE(
        lighting !=
        non_final_discard
            .attachments->end());
    lighting->store_op =
        VulkanPhysicalAttachmentStoreOp::
            discard;
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph,
                topology(false),
                bindingsFor(types, graph),
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                non_final_discard, {},
                attachments);
        },
        "separate multisample resolve");
}

TEST_CASE(
    "dependency-safe physical scopes reorder independent work and recompute lifetimes",
    "[target-render-planning][physical-fragment][scope][reorder][lifetime]") {
    const auto types =
        makeBuiltinLogicalTypeRegistry();
    const auto graph =
        graphWithWriteOnlyAttachment(types);
    const auto automatic = compile(
        types, graph, topology(false),
        bindingsFor(types, graph));
    auto fragment =
        ejectVulkanPhysicalFragmentPackage(
            automatic);
    fragment.alias_groups =
        std::vector<
            VulkanPhysicalAliasGroupFragment>{};

    const auto scratch =
        std::find_if(
            fragment.scopes->begin(),
            fragment.scopes->end(),
            [](const auto &scope) {
                return std::find(
                           scope.nodes.begin(),
                           scope.nodes.end(),
                           "Scratch") !=
                       scope.nodes.end();
            });
    REQUIRE(scratch != fragment.scopes->end());
    auto scratch_scope = *scratch;
    const auto scratch_was_first =
        scratch == fragment.scopes->begin();
    fragment.scopes->erase(scratch);
    if (scratch_was_first) {
        fragment.scopes->push_back(
            std::move(scratch_scope));
    } else {
        fragment.scopes->insert(
            fragment.scopes->begin(),
            std::move(scratch_scope));
    }

    const auto linked = compile(
        types, graph, topology(false),
        bindingsFor(types, graph),
        std::nullopt, std::nullopt,
        std::nullopt, std::nullopt,
        fragment);
    std::vector<std::string> authored_order;
    for (const auto &scope :
         *fragment.scopes) {
        authored_order.insert(
            authored_order.end(),
            scope.nodes.begin(),
            scope.nodes.end());
    }
    std::vector<std::string> linked_order;
    for (const auto &node :
         linked.lowering_graph.nodes) {
        linked_order.push_back(
            node.logical.name);
    }
    REQUIRE(linked_order == authored_order);
    const auto scratch_position =
        static_cast<std::size_t>(
            std::find(
                authored_order.begin(),
                authored_order.end(),
                "Scratch") -
            authored_order.begin());
    const TargetResourceLifetime
        expected_scratch_lifetime{
            true,
            scratch_position,
            scratch_position};
    REQUIRE(
        physicalResource(
            linked,
            "write_only_scratch")
            .lifetime ==
        expected_scratch_lifetime);

    auto reversed = ejectVulkanPhysicalFragmentPackage(
        automatic);
    reversed.alias_groups =
        std::vector<
            VulkanPhysicalAliasGroupFragment>{};
    const auto scope_index =
        [&](std::string_view node) {
            const auto found =
                std::find_if(
                    reversed.scopes->begin(),
                    reversed.scopes->end(),
                    [&](const auto &scope) {
                        return std::find(
                                   scope.nodes.begin(),
                                   scope.nodes.end(),
                                   node) !=
                               scope.nodes.end();
                    });
            REQUIRE(
                found !=
                reversed.scopes->end());
            return static_cast<std::size_t>(
                found -
                reversed.scopes->begin());
        };
    const auto gbuffer_scope =
        scope_index("GBuffer");
    const auto lighting_scope =
        scope_index("Lighting");
    std::iter_swap(
        reversed.scopes->begin() +
            static_cast<std::ptrdiff_t>(
                gbuffer_scope),
        reversed.scopes->begin() +
            static_cast<std::ptrdiff_t>(
                lighting_scope));
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph,
                topology(false),
                bindingsFor(types, graph),
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                reversed);
        },
        "reverses data dependency: GBuffer -> Lighting");
}

TEST_CASE(
    "dependency-safe physical scopes preserve explicit before and after dependencies",
    "[target-render-planning][physical-fragment][scope][reorder][explicit-dependency]") {
    const auto types =
        makeBuiltinLogicalTypeRegistry();
    const auto move_scope =
        [](VulkanPhysicalFragmentPackage &fragment,
           std::string_view node,
           bool to_front) {
        const auto found =
            std::find_if(
                fragment.scopes->begin(),
                fragment.scopes->end(),
                [&](const auto &scope) {
                    return std::find(
                               scope.nodes.begin(),
                               scope.nodes.end(),
                               node) !=
                           scope.nodes.end();
                });
        REQUIRE(
            found != fragment.scopes->end());
        auto scope = *found;
        fragment.scopes->erase(found);
        if (to_front) {
            fragment.scopes->insert(
                fragment.scopes->begin(),
                std::move(scope));
        } else {
            fragment.scopes->push_back(
                std::move(scope));
        }
        fragment.alias_groups =
            std::vector<
                VulkanPhysicalAliasGroupFragment>{};
    };

    auto after_graph =
        graphWithWriteOnlyAttachment(types);
    const auto after_node =
        std::find_if(
            after_graph.nodes.begin(),
            after_graph.nodes.end(),
            [](const auto &node) {
                return node.name == "Scratch";
            });
    REQUIRE(
        after_node != after_graph.nodes.end());
    after_node->after = {"ToneMap"};
    validateCompiledLogicalRenderGraph(
        types, after_graph);
    const auto after_automatic = compile(
        types, after_graph, topology(false),
        bindingsFor(types, after_graph));
    auto after_fragment =
        ejectVulkanPhysicalFragmentPackage(
            after_automatic);
    move_scope(
        after_fragment, "Scratch", true);
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, after_graph,
                topology(false),
                bindingsFor(
                    types, after_graph),
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                after_fragment);
        },
        "reverses after dependency: ToneMap -> Scratch");

    auto before_graph =
        graphWithWriteOnlyAttachment(types);
    const auto before_node =
        std::find_if(
            before_graph.nodes.begin(),
            before_graph.nodes.end(),
            [](const auto &node) {
                return node.name == "Scratch";
            });
    REQUIRE(
        before_node != before_graph.nodes.end());
    before_node->before = {"GBuffer"};
    validateCompiledLogicalRenderGraph(
        types, before_graph);
    const auto before_automatic = compile(
        types, before_graph,
        topology(false),
        bindingsFor(types, before_graph));
    auto before_fragment =
        ejectVulkanPhysicalFragmentPackage(
            before_automatic);
    move_scope(
        before_fragment, "Scratch", false);
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, before_graph,
                topology(false),
                bindingsFor(
                    types, before_graph),
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                before_fragment);
        },
        "reverses before dependency: Scratch -> GBuffer");
}

TEST_CASE(
    "dependency-safe physical fusion rejects mismatched attachment contracts",
    "[target-render-planning][physical-fragment][scope][fusion][reject]") {
    const auto types =
        makeBuiltinLogicalTypeRegistry();
    const auto graph =
        hybridGraph(types, 3, false);
    const std::vector<VulkanPhysicalAttachmentPlan>
        attachments{
            {
                .node = "GBuffer",
                .logical_resource = "gbuffer_0",
                .aspect =
                    VulkanPhysicalAttachmentAspect::
                        color,
            },
            {
                .node = "Lighting",
                .logical_resource =
                    "scene_color",
                .aspect =
                    VulkanPhysicalAttachmentAspect::
                        color,
            },
        };
    const auto automatic = compile(
        types, graph, topology(false),
        bindingsFor(types, graph),
        std::nullopt, std::nullopt,
        std::nullopt, std::nullopt,
        std::nullopt, {}, attachments);
    auto fragment =
        ejectVulkanPhysicalFragmentPackage(
            automatic);
    std::vector<VulkanPhysicalScopeFragment>
        scopes;
    scopes.push_back({
        .id = "invalid:attachments",
        .nodes = {"GBuffer", "Lighting"},
    });
    for (const auto &scope :
         *fragment.scopes) {
        if (std::find(
                scope.nodes.begin(),
                scope.nodes.end(),
                "GBuffer") ==
                scope.nodes.end() &&
            std::find(
                scope.nodes.begin(),
                scope.nodes.end(),
                "Lighting") ==
                scope.nodes.end()) {
            scopes.push_back(scope);
        }
    }
    fragment.scopes = std::move(scopes);
    fragment.alias_groups =
        std::vector<
            VulkanPhysicalAliasGroupFragment>{};
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph,
                topology(false),
                bindingsFor(types, graph),
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                fragment, {}, attachments);
        },
        "requires identical ordered attachments");
}

TEST_CASE("physical fragments select declared alternate formats with target capability evidence",
          "[target-render-planning][physical-fragment][format]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph =
        hybridGraph(types, 3, false, true);
    const SampleCountPolicy policy{
        .request =
            {SampleCountRequestMode::exact, 4},
        .scope = SampleCountScope::geometry,
        .authored = true,
    };
    const auto automatic = compile(
        types, graph, topology(false),
        bindingsFor(types, graph),
        sampleCountRequest(graph, policy));
    REQUIRE(
        physicalResource(automatic, "gbuffer_0")
            .format ==
        "R16G16B16A16_SFLOAT");
    REQUIRE(
        physicalResource(automatic, "gbuffer_0")
            .rasterization_samples == 4);

    auto fragment =
        ejectVulkanPhysicalFragmentPackage(
            automatic);
    const auto edited = std::find_if(
        fragment.resources.begin(),
        fragment.resources.end(),
        [](const auto &resource) {
            return resource.logical_resource ==
                   "gbuffer_0";
        });
    REQUIRE(edited != fragment.resources.end());
    edited->format = "R8G8B8A8_UNORM";

    const auto linked = compile(
        types, graph, topology(false),
        bindingsFor(types, graph),
        sampleCountRequest(graph, policy),
        std::nullopt, std::nullopt,
        std::nullopt, fragment,
        {VulkanPhysicalResourceFormatCapability{
            .logical_resource = "gbuffer_0",
            .format = "R8G8B8A8_UNORM",
            .image_usage_supported = true,
            .supported_samples = {1, 2, 4},
            .max_array_layers = 1,
        }});
    REQUIRE(
        physicalResource(linked, "gbuffer_0")
            .format ==
        "R8G8B8A8_UNORM");
    REQUIRE(
        physicalResource(linked, "gbuffer_0")
            .rasterization_samples == 4);
    REQUIRE(linked.sample_count_plan.has_value());
    const auto resolved = std::find_if(
        linked.sample_count_plan->resources.begin(),
        linked.sample_count_plan->resources.end(),
        [](const auto &resource) {
            return resource.resource == "gbuffer_0";
        });
    REQUIRE(
        resolved !=
        linked.sample_count_plan->resources.end());
    REQUIRE(resolved->format == "R8G8B8A8_UNORM");
    const auto lowering = std::find_if(
        linked.lowering_graph.resources.begin(),
        linked.lowering_graph.resources.end(),
        [](const auto &resource) {
            return resource.logical.name ==
                   "gbuffer_0";
        });
    REQUIRE(
        lowering !=
        linked.lowering_graph.resources.end());
    REQUIRE(
        std::find(
            lowering->required_physical_features.begin(),
            lowering->required_physical_features.end(),
            std::string{kLdrFormatCapability}) !=
        lowering->required_physical_features.end());
}

TEST_CASE("alternate physical format evidence is fail-closed for usage and samples",
          "[target-render-planning][physical-fragment][format][reject]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph =
        hybridGraph(types, 3, false, true);
    const SampleCountPolicy policy{
        .request =
            {SampleCountRequestMode::exact, 4},
        .scope = SampleCountScope::geometry,
        .authored = true,
    };
    const auto automatic = compile(
        types, graph, topology(false),
        bindingsFor(types, graph),
        sampleCountRequest(graph, policy));
    auto fragment =
        ejectVulkanPhysicalFragmentPackage(
            automatic);
    const auto edited = std::find_if(
        fragment.resources.begin(),
        fragment.resources.end(),
        [](const auto &resource) {
            return resource.logical_resource ==
                   "gbuffer_0";
        });
    REQUIRE(edited != fragment.resources.end());
    edited->format = "R8G8B8A8_UNORM";

    const auto compile_with_capability =
        [&](VulkanPhysicalResourceFormatCapability capability) {
            return compile(
                types, graph, topology(false),
                bindingsFor(types, graph),
                sampleCountRequest(graph, policy),
                std::nullopt, std::nullopt,
                std::nullopt, fragment,
                {std::move(capability)});
        };
    requireThrowsContaining(
        [&] {
            (void)compile_with_capability({
                .logical_resource = "gbuffer_0",
                .format = "R8G8B8A8_UNORM",
                .image_usage_supported = false,
                .supported_samples = {1, 2, 4},
            });
        },
        "does not support the required image usage");
    requireThrowsContaining(
        [&] {
            (void)compile_with_capability({
                .logical_resource = "gbuffer_0",
                .format = "R8G8B8A8_UNORM",
                .image_usage_supported = true,
                .supported_samples = {1, 2},
            });
        },
        "does not support the selected rasterization sample count");
}

TEST_CASE("physical fragment verifier rejects stale, aggressive, open-scope, and overlapping edits",
          "[target-render-planning][physical-fragment][reject]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph =
        hybridGraph(types, 3, false, true);
    const auto tile = compile(
        types, graph, topology(true),
        bindingsFor(types, graph));
    auto tile_fragment =
        ejectVulkanPhysicalFragmentPackage(tile);

    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, topology(true, 7),
                bindingsFor(types, graph),
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                tile_fragment);
        },
        "stale for the current target facts/provider generation");

    auto split_without_materialization =
        tile_fragment;
    split_without_materialization.scopes =
        std::vector<VulkanPhysicalScopeFragment>{};
    for (const auto &scope : tile.scopes) {
        for (const auto &node : scope.nodes) {
            split_without_materialization
                .scopes->push_back(
                    VulkanPhysicalScopeFragment{
                        .id = "split:" + node,
                        .nodes = {node},
                    });
        }
    }
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, topology(true),
                bindingsFor(types, graph),
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                split_without_materialization);
        },
        "exposes scope-local resource across scopes");

    const auto desktop = compile(
        types, graph, topology(false),
        bindingsFor(types, graph));
    auto aggressive =
        ejectVulkanPhysicalFragmentPackage(
            desktop);
    const auto gbuffer =
        std::find_if(
            aggressive.resources.begin(),
            aggressive.resources.end(),
            [](const auto &resource) {
                return resource.logical_resource ==
                       "gbuffer_0";
            });
    REQUIRE(gbuffer != aggressive.resources.end());
    gbuffer->representation =
        VulkanResourceRepresentation::
            tile_local_attachment;
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, topology(false),
                bindingsFor(types, graph),
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                aggressive);
        },
        "conservatively materialize");

    auto alternate_format =
        ejectVulkanPhysicalFragmentPackage(
            desktop);
    const auto format_resource =
        std::find_if(
            alternate_format.resources.begin(),
            alternate_format.resources.end(),
            [](const auto &resource) {
                return resource.logical_resource ==
                       "gbuffer_0";
            });
    REQUIRE(
        format_resource !=
        alternate_format.resources.end());
    format_resource->format =
        "R8G8B8A8_UNORM";
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, topology(false),
                bindingsFor(types, graph),
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                alternate_format);
        },
        "no target format capability");

    auto fused = ejectVulkanPhysicalFragmentPackage(
        desktop);
    fused.schema_version = 2;
    fused.scope_edit_mode =
        VulkanPhysicalScopeEditMode::split_only;
    REQUIRE(fused.scopes->size() >= 2);
    auto fused_scopes =
        std::vector<VulkanPhysicalScopeFragment>{};
    fused_scopes.push_back(
        VulkanPhysicalScopeFragment{
            .id = "illegal:fused",
            .nodes = {
                fused.scopes->at(0).nodes.front(),
                fused.scopes->at(1).nodes.front(),
            },
        });
    for (std::size_t index = 2;
         index < fused.scopes->size(); ++index) {
        fused_scopes.push_back(
            fused.scopes->at(index));
    }
    fused.scopes = std::move(fused_scopes);
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, topology(false),
                bindingsFor(types, graph),
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                fused);
        },
        "cannot fuse nodes from different automatic scopes");

    auto overlapping =
        ejectVulkanPhysicalFragmentPackage(
            desktop);
    overlapping.alias_groups =
        std::vector<
            VulkanPhysicalAliasGroupFragment>{
            {
                .id = "manual:overlap",
                .resources =
                    {"gbuffer_0", "gbuffer_1"},
            },
        };
    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, topology(false),
                bindingsFor(types, graph),
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                overlapping);
        },
        "overlapping resource lifetimes");

    auto malformed =
        vulkanPhysicalFragmentPackageToJson(
            tile_fragment);
    malformed["resources"][0]["load_op"] =
        "dont_care";
    requireThrowsContaining(
        [&] {
            (void)vulkanPhysicalFragmentPackageFromJson(
                malformed);
        },
        "unknown key 'load_op'");
}

TEST_CASE("view execution planning keeps mono and sequential stereo as explicit physical contracts",
          "[target-render-planning][view-execution][sequential]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);

    const auto mono = compile(
        types, graph, topology(false),
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
        types, graph, topology(false),
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

TEST_CASE("directional shadow remains one shared image for flat preview sequential XR and multiview",
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
        types, graph, topology(false),
        bindingsFor(types, graph), std::nullopt,
        VulkanViewExecutionPlanRequest{
            .view_count = 1,
            .view_independent_nodes = independent,
        });
    auto preview_graph = graph;
    preview_graph.name = "preview";
    const auto preview = compile(
        types, preview_graph, topology(false),
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
                VulkanResourceViewLayout::shared_2d);
        REQUIRE(physicalResource(
                    *plan, "directional_shadow")
                    .array_layers == 1);
    }

    const auto sequential = compile(
        types, graph, topology(false),
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
            VulkanResourceViewLayout::shared_2d);
    REQUIRE(physicalResource(
                sequential, "directional_shadow")
                .array_layers == 1);

    const auto multiview = compile(
        types, graph, topology(false, 8, 2),
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
            VulkanResourceViewLayout::shared_2d);
    REQUIRE(physicalResource(
                multiview, "directional_shadow")
                .array_layers == 1);
}

TEST_CASE("view execution planning selects layered multiview and exposes its Vulkan scope contract",
          "[target-render-planning][view-execution][multiview]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);
    const auto canonical_before =
        compiledLogicalRenderGraphToJson(graph).dump();
    const auto plan = compile(
        types, graph, topology(false, 8, 2),
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
        types, graph,
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
        types, graph,
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
        types, graph, topology(false, 8, 2),
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
                types, graph, topology(false, 8, 2),
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
        types, graph, topology(true, 8, 2),
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
            "D32_SFLOAT");
    REQUIRE(plan.external_depth_export->view_layout ==
            VulkanResourceViewLayout::layered_2d_array);
    REQUIRE(plan.external_depth_export->array_layers == 2);
    const auto &depth =
        physicalResource(plan, "scene_depth");
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

    const auto mixed = compile(
        types, graph, topology(false, 8, 2),
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
        types, graph, topology(false),
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
        types, graph, topology(false),
        bindingsFor(types, graph), std::nullopt,
        std::nullopt,
        VulkanExternalDepthExportRequest{});
    REQUIRE_FALSE(optional.external_depth_export);

    requireThrowsContaining(
        [&] {
            (void)compile(
                types, graph, topology(false),
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
    const auto plan = compile(types, graph, topology(true),
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
        types, graph, bindingsFor(types, graph));
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

    const auto plan = compile(types, graph, topology(true),
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
        types, within_budget, topology(true, 7),
        bindingsFor(types, within_budget)));

    const auto over_budget = hybridGraph(types, 9, false);
    requireThrowsContaining(
        [&] {
            (void)compile(types, over_budget, topology(true, 8),
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

    const auto plan = compile(types, graph, topology(false),
                              bindingsFor(types, graph));
    REQUIRE(std::find(
                plan.opportunities.alias_candidates.begin(),
                plan.opportunities.alias_candidates.end(),
                PlanningNamePair{"temporary_a", "temporary_b"}) !=
            plan.opportunities.alias_candidates.end());
    REQUIRE(plan.alias_groups.size() == 1);
    REQUIRE(plan.alias_groups.front().resources ==
            std::vector<std::string>{"temporary_a", "temporary_b"});
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
        types, graph, topology(false), bindingsFor(types, graph),
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
                types, graph, topology(false),
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
        types, graph, topology(true), bindingsFor(types, graph),
        sampleCountRequest(graph, policy));

    REQUIRE(physicalResource(plan, "gbuffer_0")
                .rasterization_samples == 4);
    REQUIRE(physicalResource(plan, "gbuffer_4")
                .rasterization_samples == 4);
    REQUIRE(physicalResource(plan, "scene_color")
                .rasterization_samples == 1);
    REQUIRE_FALSE(oneScopeContains(plan, "GBuffer", "Lighting"));
}

} // namespace Pelican
