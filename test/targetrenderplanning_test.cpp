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

TargetTopologySnapshot topology(bool tile, std::uint32_t budget = 8) {
    std::vector<std::string> capabilities{
        "pelican.vulkan.graphics@1",
        "pelican.vulkan.sampled_image@1",
        "pelican.vulkan.transfer_copy@1",
        std::string{kHdrFormatCapability},
        std::string{kDisplayFormatCapability},
    };
    if (tile) {
        capabilities.insert(
            capabilities.end(),
            {"pelican.vulkan.tile_based@1",
             "pelican.vulkan.dynamic_rendering_local_read@1",
             "pelican.vulkan.transient_attachment@1"});
    }
    return TargetTopologySnapshot{
        tile ? "mock_tile" : "mock_desktop",
        {TargetEndpoint{
            "device:0",
            TargetEndpointKind::vulkan_device,
            std::move(capabilities),
            {{"pelican.vulkan.max_color_attachments@1",
              std::to_string(budget)},
             {"pelican.vulkan.profile@1",
              tile ? "tile" : "desktop"}},
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
    return ResourcePattern{
        .id = display
                  ? "pelican.render.display_output_pattern@1"
                  : "pelican.render.frame_color_pattern@1",
        .applicable_type =
            exactLogicalTypePattern(types, resource.type),
        .format_candidates =
            {ResourceFormatCandidate{
                display ? "B8G8R8A8_UNORM"
                        : "R16G16B16A16_SFLOAT",
                {std::string{
                    display ? kDisplayFormatCapability
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
                    : 8ULL * 1024ULL * 1024ULL,
        .provenance = display ? "builtin:display-v1"
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
        std::nullopt) {
    return compileVulkanTargetPlan(
        types, graph, target, providers(),
        VulkanTargetPlanRequest{
            .endpoint = "device:0",
            .provider = std::string{kProvider},
            .pattern_bindings = std::move(bindings),
            .sample_count = std::move(sample_count),
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
            .format = "R16G16B16A16_SFLOAT",
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
