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
            {ResourceFormatCandidate{
                display
                    ? "B8G8R8A8_UNORM"
                : depth
                    ? "D32_SFLOAT"
                    : "R16G16B16A16_SFLOAT",
                {std::string{
                    display ? kDisplayFormatCapability
                    : depth ? kDepthFormatCapability
                            : kHdrFormatCapability}},
            }},
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
        fragment_package = std::nullopt) {
    return compileVulkanTargetPlan(
        types, graph, target, providers(),
        VulkanTargetPlanRequest{
            .endpoint = "device:0",
            .provider = std::string{kProvider},
            .pattern_bindings = std::move(bindings),
            .sample_count = std::move(sample_count),
            .view_execution = std::move(view_execution),
            .external_depth_export =
                std::move(external_depth_export),
            .pin_package =
                std::move(pin_package),
            .fragment_package =
                std::move(fragment_package),
        });
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
    const auto automatic = compile(
        types, graph, topology(true),
        bindingsFor(types, graph));

    const auto ejected =
        ejectVulkanPhysicalFragmentPackage(
            automatic);
    const auto document =
        vulkanPhysicalFragmentPackageToJson(
            ejected);
    REQUIRE(document.at("schema") ==
            "pelican.vulkan_physical_fragment");
    REQUIRE(document.at("version") == 1);
    REQUIRE(document.at("automatic_plan_fingerprint")
                .get<std::string>()
                .starts_with("fnv1a64:"));
    REQUIRE(
        vulkanPhysicalFragmentPackageFromJson(
            document) == ejected);

    const auto linked = compile(
        types, graph, topology(true),
        bindingsFor(types, graph), std::nullopt,
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
    REQUIRE(encoded.at(
                "ejectable_physical_fragment") ==
            document);
    REQUIRE(encoded.at(
                "applied_physical_fragment") ==
            document);
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
                   "pelican.plan.physical_scope_fragment@1";
        }));
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
        "keeps the automatic format");

    auto fused = ejectVulkanPhysicalFragmentPackage(
        desktop);
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
