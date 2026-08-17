#include "../src/core/renderingpass/previewgraph.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/project/featurecompose.hpp"
#include "../src/project/renderpipeline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;

Json featureEnvelope(std::string name) {
    return {
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", std::move(name)},
    };
}

Json baseConfig(std::vector<std::string> features = {}) {
    return {
        {"features", std::move(features)},
        {"render_targets", Json::array()},
        {"rendering_passes",
         Json::array({
             {
                 {"name", "main"},
                 {"passes",
                  Json::array({
                      {
                          {"name", "scene"},
                          {"type", "material"},
                          {"output",
                           {{"color", Json::array({"swapchain"})},
                            {"depth", nullptr}}},
                      },
                  })},
             },
         })},
    };
}

std::string loadEngineResource(std::string_view reference) {
    constexpr std::string_view prefix = "engine://";
    if (!reference.starts_with(prefix)) {
        throw std::runtime_error("expected engine resource reference");
    }
    return engineResourceOrThrow(reference.substr(prefix.size()));
}

Json hybridRouting() {
    return {
        {"policy", "hybrid_auto_v1"},
        {"routes",
         {
             {"deferred_geometry",
              {{"pass", "deferred_geometry"},
               {"contract", "deferred_geometry_v1"},
               {"shader_contract", "gbuffer_v1"},
               {"phase", "opaque"}}},
             {"forward_opaque",
              {{"pass", "forward_opaque"},
               {"contract", "forward_opaque_v1"},
               {"shader_contract", "forward_scene_color_v1"},
               {"phase", "opaque"}}},
             {"forward_transparent",
              {{"pass", "forward_transparent"},
               {"contract", "forward_transparent_v1"},
               {"shader_contract", "forward_scene_color_v1"},
               {"phase", "transparent"}}},
         }},
    };
}

Json extendedGbufferSchema(std::string name =
                               "project.extended_gbuffer") {
    return {
        {"schema", "pelican.material_outputs"},
        {"version", 1},
        {"name", std::move(name)},
        {"outputs",
         {
             {{"name", "base_color"},
              {"type", "vec4"},
              {"source", "surface.base_color"}},
             {{"name", "normal"},
              {"type", "vec2"},
              {"source", "custom"}},
             {{"name", "object_id"},
              {"type", "uint"},
              {"source", "custom"}},
         }},
    };
}

Json hybridPassSet(std::string name,
                   const Json &output_schema) {
    return {
        {"name", std::move(name)},
        {"passes",
         Json::array({
             {
                 {"name", "deferred_geometry"},
                 {"type", "material"},
                 {"material_contract",
                  "deferred_geometry_v1"},
                 {"material_outputs", output_schema},
             },
             {
                 {"name", "forward_opaque"},
                 {"type", "material"},
                 {"material_contract",
                  "forward_opaque_v1"},
             },
             {
                 {"name", "forward_transparent"},
                 {"type", "material"},
                 {"material_contract",
                  "forward_transparent_v1"},
             },
         })},
    };
}

Json weightedOutputStates() {
    return {
        {"base_color",
         {
             {"blend",
              {
                  {"color",
                   {{"src", "one"},
                    {"dst", "one"},
                    {"op", "add"}}},
                  {"alpha",
                   {{"src", "one"},
                    {"dst", "one"},
                    {"op", "add"}}},
              }},
             {"write_mask", "rgba"},
         }},
        {"object_id",
         {
             {"blend", "opaque"},
             {"write_mask", "r"},
         }},
    };
}

void publishGenerationProbe(
    FrameGraphRuntimeContainer &runtime,
    std::shared_ptr<const CompiledRenderPipeline> pipeline) {
    CompiledRenderingPass rendering_pass;
    rendering_pass.name = "wp311_publication_probe";
    FramePlan frame_plan;
    frame_plan.name = rendering_pass.name;
    runtime.registerExecutionPlan(
        RenderingPassId{0}, rendering_pass,
        std::move(frame_plan), std::move(pipeline));
}

} // namespace

TEST_CASE(
    "WP218 material routing carries one output schema across graph variants",
    "[wp218][render-pipeline][material-output]") {
    auto config = Json{
        {"material_routing",
         {
             {"policy", "hybrid_auto_v1"},
             {"routes",
              {
                  {"deferred_geometry",
                   "deferred_geometry"},
                  {"forward_opaque",
                   "forward_opaque"},
                  {"forward_transparent",
                   "forward_transparent"},
              }},
         }},
        {"rendering_passes",
         Json::array({
             hybridPassSet(
                 "flat",
                 extendedGbufferSchema()),
             hybridPassSet(
                 "xr",
                 extendedGbufferSchema()),
         })},
    };
    const auto routing =
        resolveMaterialRoutingTable(config);
    const auto &deferred =
        routing.at("routes").at(
            "deferred_geometry");
    REQUIRE(
        deferred.at("output_schema").at("outputs").size() ==
        3);
    REQUIRE(
        deferred.at("output_schema_fingerprint")
            .get<std::string>()
            .starts_with(
                "pelican.material_outputs@1:"
                "project.extended_gbuffer"));

    ResolvedRenderPipeline resolved;
    resolved.material_routing = routing;
    const auto compiled =
        compileRenderPipeline(resolved);
    REQUIRE(compiled.material_routing);
    REQUIRE(
        compiled.material_routing->routes.front()
            .output_schema);
    const auto dumped =
        serializeCompiledRenderPipelineMetadata(
            compiled);
    REQUIRE(
        dumped.at("material_routing")
            .at("routes")
            .at("deferred_geometry")
            .at("output_schema") ==
        extendedGbufferSchema());

    config["rendering_passes"][1]["passes"][0]
          ["material_outputs"]["name"] =
        "project.xr_only_schema";
    REQUIRE_THROWS_WITH(
        resolveMaterialRoutingTable(config),
        Catch::Matchers::ContainsSubstring(
            "different material_outputs schemas"));
}

TEST_CASE(
    "WP219 material routing carries attachment state across graph variants",
    "[wp219][render-pipeline][material-output-state]") {
    auto flat =
        hybridPassSet(
            "flat", extendedGbufferSchema());
    auto xr =
        hybridPassSet(
            "xr", extendedGbufferSchema());
    flat["passes"][0]["material_output_states"] =
        weightedOutputStates();
    xr["passes"][0]["material_output_states"] =
        weightedOutputStates();
    auto config = Json{
        {"material_routing",
         {
             {"policy", "hybrid_auto_v1"},
             {"routes",
              {
                  {"deferred_geometry",
                   "deferred_geometry"},
                  {"forward_opaque",
                   "forward_opaque"},
                  {"forward_transparent",
                   "forward_transparent"},
              }},
         }},
        {"rendering_passes",
         Json::array({flat, xr})},
    };

    const auto routing =
        resolveMaterialRoutingTable(config);
    const auto &deferred =
        routing.at("routes")
            .at("deferred_geometry");
    REQUIRE(
        deferred.at("output_states")
            .contains("base_color"));
    REQUIRE(
        deferred.at("output_states_fingerprint")
            .get<std::string>()
            .starts_with(
                "pelican.material_output_states@1:"));

    ResolvedRenderPipeline resolved;
    resolved.material_routing = routing;
    const auto compiled =
        compileRenderPipeline(resolved);
    REQUIRE(compiled.material_routing);
    REQUIRE(
        compiled.material_routing->routes.front()
            .output_states.size() == 2);
    const auto dumped =
        serializeCompiledRenderPipelineMetadata(
            compiled);
    REQUIRE(
        dumped.at("material_routing")
            .at("routes")
            .at("deferred_geometry")
            .at("output_states")
            .contains("object_id"));

    config["rendering_passes"][1]["passes"][0]
          ["material_output_states"]["base_color"]
          ["write_mask"] = "rgb";
    REQUIRE_THROWS_WITH(
        resolveMaterialRoutingTable(config),
        Catch::Matchers::ContainsSubstring(
            "different material_output_states"));

    auto tampered = routing;
    tampered["routes"]["deferred_geometry"]
            ["output_states_fingerprint"] =
        "tampered";
    ResolvedRenderPipeline tampered_resolved;
    tampered_resolved.material_routing =
        std::move(tampered);
    REQUIRE_THROWS_WITH(
        compileRenderPipeline(tampered_resolved),
        Catch::Matchers::ContainsSubstring(
            "output_states_fingerprint does not match"));
}

TEST_CASE("WP180 flat resolver preserves FeatureCompose bytes without modules",
          "[wp180][render-pipeline][resolve][flat]") {
    const auto preset = Json{
        {"schema", "pelican.render_pipeline"},
        {"version", 1},
        {"name", "fixture_v1"},
        {"config", baseConfig({"fixture://safe"})},
    };
    const auto authored = Json{
        {"pipeline", {{"preset", "fixture://pipeline"}}},
        {"shader_defines", Json::array({"PELICAN_FIXTURE"})},
    };
    const auto load_feature = [](std::string_view ref) {
        if (ref != "fixture://safe") {
            throw std::runtime_error("unknown feature");
        }
        return featureEnvelope("safe").dump();
    };
    const auto load_pipeline = [&preset](std::string_view ref) {
        if (ref != "fixture://pipeline") {
            throw std::runtime_error("unknown pipeline");
        }
        return preset.dump();
    };

    const auto legacy = composeRenderFeatureConfig(
        authored,
        RenderFeatureComposeDependencies{load_feature, true, {},
                                         load_pipeline});
    const auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{authored, "flat fixture"},
        RenderEnvironmentCapabilities{true,
                                      RenderPipelineGraphVariant::flat},
        RenderPipelineResolveDependencies{load_feature, load_pipeline});

    REQUIRE(resolved.normalized_config.dump() == legacy.config.dump());
    REQUIRE(resolved.shader_defines == legacy.shader_defines);
    REQUIRE(resolved.feature_names == legacy.feature_names);
    REQUIRE(resolved.excluded_feature_names ==
            legacy.excluded_feature_names);
    REQUIRE(resolved.material_routing == legacy.material_routing);
    REQUIRE(resolved.pipeline_preset.has_value());
    REQUIRE(resolved.pipeline_preset->reference == "fixture://pipeline");
    REQUIRE(resolved.pipeline_preset->name == "fixture_v1");
    REQUIRE(resolved.pipeline_preset->version == 1);

    const auto compiled = compileRenderPipeline(resolved);
    const auto metadata =
        serializeCompiledRenderPipelineMetadata(compiled);
    REQUIRE(metadata == Json{
                            {"pipeline_preset",
                             {{"ref", "fixture://pipeline"},
                              {"name", "fixture_v1"},
                              {"version", 1}}},
                        });
    REQUIRE(resolved.diagnostics.front().kind ==
            RenderPipelineDiagnosticKind::graph_variant_selected);
    REQUIRE(resolved.diagnostics.front().subject == "flat");
}

#if PELICAN_WITH_OPENXR
TEST_CASE("WP180 XR resolution preserves policy order suffix and metadata",
          "[wp180][render-pipeline][resolve][xr]") {
    const std::unordered_map<std::string, Json> features{
        {"fixture://safe", featureEnvelope("safe")},
        {"fixture://ui", featureEnvelope("ui")},
    };
    const auto authored = baseConfig({"fixture://safe", "fixture://ui"});
    const auto load_feature = [&features](std::string_view ref) {
        return features.at(std::string{ref}).dump();
    };
    const auto include_feature =
        [](std::string_view name, const Json &) { return name != "ui"; };

    auto legacy = composeRenderFeatureConfig(
        authored,
        RenderFeatureComposeDependencies{load_feature, true,
                                         include_feature});
    legacy.config["normalization_probe"] = "normalized";
    legacy.config["transform_probe"] = "transformed";
    for (auto &graph : legacy.config.at("rendering_passes")) {
        graph["name"] = graph.at("name").get<std::string>() + "#xr";
    }

    std::vector<std::string> resolution_order;
    const auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{authored, "xr fixture"},
        RenderEnvironmentCapabilities{true,
                                      RenderPipelineGraphVariant::xr},
        RenderPipelineResolveDependencies{
            .load_feature_json = load_feature,
            .normalize_config =
                [&resolution_order](
                    const Json &config,
                    const std::vector<std::string> &) {
                resolution_order.push_back("normalize");
                auto normalized = config;
                normalized["normalization_probe"] = "normalized";
                return normalized;
            },
            .transform_config = [&resolution_order](Json &config) {
                resolution_order.push_back("transform");
                REQUIRE(config.at("normalization_probe") == "normalized");
                config["transform_probe"] = "transformed";
            },
            .validate_config =
                [&resolution_order](const Json &config) {
                resolution_order.push_back("validate");
                REQUIRE(config.at("normalization_probe") == "normalized");
                REQUIRE(config.at("transform_probe") == "transformed");
                REQUIRE(config.at("rendering_passes").at(0).at("name") ==
                        "main");
            },
        });

    REQUIRE(resolution_order ==
            std::vector<std::string>{"normalize", "transform", "validate"});
    REQUIRE(resolved.normalized_config.dump() == legacy.config.dump());
    REQUIRE(resolved.feature_names == std::vector<std::string>{"safe"});
    REQUIRE(resolved.excluded_feature_names ==
            std::vector<std::string>{"ui"});
    const auto compiled = compileRenderPipeline(resolved);
    REQUIRE(serializeCompiledRenderPipelineMetadata(compiled) ==
            Json{{"graph_variant", "xr"},
                 {"excluded_features", Json::array({"ui"})}});
    REQUIRE(std::any_of(
        resolved.diagnostics.begin(), resolved.diagnostics.end(),
        [](const auto &diagnostic) {
            return diagnostic.kind ==
                       RenderPipelineDiagnosticKind::feature_excluded &&
                   diagnostic.subject == "ui" &&
                   diagnostic.detail ==
                       "xr:known_incompatible";
        }));
}
#endif

TEST_CASE("WP180 hybrid resolver preserves semantic material routing",
          "[wp180][render-pipeline][resolve][hybrid]") {
    const auto authored = Json{
        {"pipeline",
         {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
        {"features", Json::array({"engine://features/ui.json"})},
    };
    const auto legacy = composeRenderFeatureConfig(
        authored,
        RenderFeatureComposeDependencies{loadEngineResource, true, {},
                                         loadEngineResource});
    const auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{authored, "hybrid fixture"},
        RenderEnvironmentCapabilities{true,
                                      RenderPipelineGraphVariant::flat},
        RenderPipelineResolveDependencies{loadEngineResource,
                                          loadEngineResource});

    REQUIRE(resolved.normalized_config.dump() == legacy.config.dump());
    REQUIRE(resolved.material_routing == legacy.material_routing);
    REQUIRE(resolved.material_routing.at("routes")
                .at("deferred_geometry")
                .at("pass") == "deferred_geometry");
    REQUIRE(resolved.material_routing.at("routes")
                .at("forward_transparent")
                .at("phase") == "transparent");
    const auto compiled = compileRenderPipeline(resolved);
    const auto metadata =
        serializeCompiledRenderPipelineMetadata(compiled);
    REQUIRE(metadata.at("material_routing") == legacy.material_routing);
    REQUIRE(compiled.draw_sorting.opaque.provider == "state_batched_v1");
    REQUIRE(compiled.draw_sorting.transparent.provider ==
            "back_to_front_v1");
    REQUIRE(compiled.draw_sorting.xr_view_policy ==
            DrawSortXrViewPolicy::logical_view_center);
    REQUIRE(metadata.at("draw_sort") == legacy.draw_sort);
    REQUIRE(metadata.at("pipeline_preset").at("name") == "hybrid_v1");
    REQUIRE_FALSE(metadata.contains("graph_variant"));
}

TEST_CASE("target planning authoring compiles to typed graph-scoped controls",
          "[render-pipeline][target-planning][typed]") {
#if PELICAN_WITH_OPENXR
    constexpr auto selected_variant = RenderPipelineGraphVariant::xr;
    const auto selected_graph = std::string{"main#xr"};
#else
    constexpr auto selected_variant = RenderPipelineGraphVariant::flat;
    const auto selected_graph = std::string{"main"};
#endif
    auto authored = baseConfig();
    authored["target_planning"] = {
        {"profile",
         {{"kind", "hazard_stress"}, {"seed", 17}}},
        {"graphs",
          {{"main",
           {{"required_capabilities",
             Json::array(
                 {"pelican.vulkan.ray_query@1"})},
            {"nodes",
             {{"scene",
               {{"serial", true}, {"isolate", true}}}}},
            {"resources",
             {{"swapchain", {{"no_alias", true}}}}}}}}},
        {"diagnostics",
         {{"strict_warnings",
           Json::array({"fixture.warning.b", "fixture.warning.a"})}}},
    };

    const auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{authored, "target planning fixture"},
        RenderEnvironmentCapabilities{
            true, selected_variant});
    REQUIRE(resolved.target_planning.authored);
    REQUIRE((resolved.target_planning.profile ==
             PlanningProfile{
                 PlanningProfileKind::hazard_stress, 17}));
    REQUIRE(resolved.target_planning.graphs.size() == 1);
    REQUIRE(resolved.target_planning.graphs.front().graph ==
            selected_graph);
    REQUIRE((resolved.target_planning.graphs.front()
                 .required_capabilities ==
             std::vector<std::string>{
                 "pelican.vulkan.ray_query@1"}));
    REQUIRE((resolved.target_planning.graphs.front().nodes ==
             std::vector<PlanningNodeConstraint>{
                 {"scene", true, true}}));
    REQUIRE((resolved.target_planning.graphs.front().resources ==
             std::vector<PlanningResourceConstraint>{
                 {"swapchain", true}}));
    REQUIRE((
        resolved.target_planning.diagnostic_policy
            .strict_warning_ids ==
        std::vector<std::string>{
            "fixture.warning.a", "fixture.warning.b"}));

    const auto compiled = compileRenderPipeline(resolved);
    REQUIRE(compiled.target_planning ==
            resolved.target_planning);
    auto expected_target_planning = Json{
        {"profile",
         {{"kind", "hazard_stress"}, {"seed", 17}}},
        {"graphs", Json::object()},
        {"diagnostics",
         {{"strict_warnings",
           Json::array(
               {"fixture.warning.a",
                "fixture.warning.b"})}}},
    };
    expected_target_planning["graphs"][selected_graph] = {
        {"required_capabilities",
         Json::array(
             {"pelican.vulkan.ray_query@1"})},
        {"nodes",
         {{"scene",
           {{"serial", true},
            {"isolate", true}}}}},
        {"resources",
         {{"swapchain",
           {{"no_alias", true}}}}},
    };
    REQUIRE(
        serializeCompiledRenderPipelineMetadata(compiled)
            .at("target_planning") ==
        expected_target_planning);
}

TEST_CASE("target planning authoring rejects ambiguous controls",
          "[render-pipeline][target-planning][validation]") {
    auto authored = baseConfig();

    SECTION("seed belongs only to hazard stress") {
        authored["target_planning"] = {
            {"profile",
             {{"kind", "optimized"}, {"seed", 1}}},
        };
        REQUIRE_THROWS_WITH(
            resolveRenderPipeline(
                RenderPipelineRequest{
                    authored, "invalid target planning"},
                RenderEnvironmentCapabilities{}),
            Catch::Matchers::ContainsSubstring(
                "seed is only valid for hazard_stress"));
    }

    SECTION("constraint flags are typed") {
        authored["target_planning"] = {
            {"graphs",
             {{"main",
               {{"nodes",
                 {{"scene", {{"serial", "yes"}}}}}}}}},
        };
        REQUIRE_THROWS_WITH(
            resolveRenderPipeline(
                RenderPipelineRequest{
                    authored, "invalid target planning"},
                RenderEnvironmentCapabilities{}),
            Catch::Matchers::ContainsSubstring(
                "serial must be a boolean"));
    }

    SECTION("strict warning ids are unique") {
        authored["target_planning"] = {
            {"diagnostics",
             {{"strict_warnings",
               Json::array({"same", "same"})}}},
        };
        REQUIRE_THROWS_WITH(
            resolveRenderPipeline(
                RenderPipelineRequest{
                    authored, "invalid target planning"},
                RenderEnvironmentCapabilities{}),
            Catch::Matchers::ContainsSubstring(
                "must not contain duplicates"));
    }

    SECTION("required capabilities are unique") {
        authored["target_planning"] = {
            {"graphs",
             {{"main",
               {{"required_capabilities",
                 Json::array({
                     "pelican.vulkan.ray_query@1",
                     "pelican.vulkan.ray_query@1",
                 })}}}}},
        };
        REQUIRE_THROWS_WITH(
            resolveRenderPipeline(
                RenderPipelineRequest{
                    authored, "invalid target planning"},
                RenderEnvironmentCapabilities{}),
            Catch::Matchers::ContainsSubstring(
                "required_capabilities must not contain duplicates"));
    }
}

TEST_CASE("Vulkan plan pins select the current immutable graph variant",
          "[render-pipeline][vulkan-plan-pins][typed]") {
    const VulkanTargetPlanPinPackage flat_pin{
        .graph = "main",
        .logical_graph_fingerprint = 1,
        .backend_candidate =
            "pelican.vulkan.materialized_plan@1",
    };
    const VulkanTargetPlanPinPackage xr_pin{
        .graph = "main#xr",
        .logical_graph_fingerprint = 2,
        .backend_candidate =
            "pelican.vulkan.materialized_plan@1",
    };
#if PELICAN_WITH_OPENXR
    constexpr auto selected_variant = RenderPipelineGraphVariant::xr;
    constexpr auto selected_variant_name = "xr";
    const auto &selected_pin = xr_pin;
#else
    constexpr auto selected_variant = RenderPipelineGraphVariant::flat;
    constexpr auto selected_variant_name = "flat";
    const auto &selected_pin = flat_pin;
#endif
    auto authored = baseConfig();
    authored["vulkan_plan_pins"] = {
        {"flat",
         Json::array({
             vulkanTargetPlanPinPackageToJson(flat_pin)})},
        {"xr",
         Json::array({
             vulkanTargetPlanPinPackageToJson(xr_pin)})},
    };

    const auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{
            authored, "Vulkan plan pin fixture"},
        RenderEnvironmentCapabilities{
            true, selected_variant});
    REQUIRE(resolved.vulkan_plan_pins ==
            std::vector<VulkanTargetPlanPinPackage>{
                selected_pin});
    const auto compiled =
        compileRenderPipeline(resolved);
    REQUIRE(compiled.vulkan_plan_pins ==
            resolved.vulkan_plan_pins);
    REQUIRE(
        serializeCompiledRenderPipelineMetadata(compiled)
            .at("vulkan_plan_pins") ==
        Json::array({
            vulkanTargetPlanPinPackageToJson(selected_pin)}));

    authored["vulkan_plan_pins"][selected_variant_name].push_back(
        vulkanTargetPlanPinPackageToJson(selected_pin));
    REQUIRE_THROWS_WITH(
        resolveRenderPipeline(
            RenderPipelineRequest{
                authored, "duplicate Vulkan plan pins"},
            RenderEnvironmentCapabilities{
                true, selected_variant}),
        Catch::Matchers::ContainsSubstring(
            "duplicate packages for a graph"));
}

TEST_CASE("Vulkan physical fragments select the current immutable graph variant",
          "[render-pipeline][vulkan-physical-fragments][typed]") {
    const VulkanPhysicalFragmentPackage flat_fragment{
        .graph = "main",
        .logical_graph_fingerprint = 1,
        .automatic_plan_fingerprint = 11,
        .backend_candidate =
            "pelican.vulkan.materialized_plan@1",
        .resources = {
            VulkanPhysicalResourceFragment{
                .logical_resource = "swapchain",
                .representation =
                    VulkanResourceRepresentation::external,
            },
        },
    };
    const VulkanPhysicalFragmentPackage xr_fragment{
        .graph = "main#xr",
        .logical_graph_fingerprint = 2,
        .automatic_plan_fingerprint = 22,
        .backend_candidate =
            "pelican.vulkan.materialized_plan@1",
        .resources = {
            VulkanPhysicalResourceFragment{
                .logical_resource = "swapchain",
                .representation =
                    VulkanResourceRepresentation::external,
            },
        },
    };
#if PELICAN_WITH_OPENXR
    constexpr auto selected_variant = RenderPipelineGraphVariant::xr;
    constexpr auto selected_variant_name = "xr";
    const auto &selected_fragment = xr_fragment;
#else
    constexpr auto selected_variant = RenderPipelineGraphVariant::flat;
    constexpr auto selected_variant_name = "flat";
    const auto &selected_fragment = flat_fragment;
#endif
    auto authored = baseConfig();
    authored["vulkan_physical_fragments"] = {
        {"flat",
         Json::array({
             vulkanPhysicalFragmentPackageToJson(
                 flat_fragment)})},
        {"xr",
         Json::array({
             vulkanPhysicalFragmentPackageToJson(
                 xr_fragment)})},
    };

    const auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{
            authored, "Vulkan physical fragment fixture"},
        RenderEnvironmentCapabilities{
            true, selected_variant});
    REQUIRE(
        resolved.vulkan_physical_fragments ==
        std::vector<VulkanPhysicalFragmentPackage>{
            selected_fragment});
    const auto compiled =
        compileRenderPipeline(resolved);
    REQUIRE(
        compiled.vulkan_physical_fragments ==
        resolved.vulkan_physical_fragments);
    REQUIRE(
        serializeCompiledRenderPipelineMetadata(compiled)
            .at("vulkan_physical_fragments") ==
        Json::array({
            vulkanPhysicalFragmentPackageToJson(
                selected_fragment)}));

    authored["vulkan_physical_fragments"][selected_variant_name]
        .push_back(
            vulkanPhysicalFragmentPackageToJson(
                selected_fragment));
    REQUIRE_THROWS_WITH(
        resolveRenderPipeline(
            RenderPipelineRequest{
                authored,
                "duplicate Vulkan physical fragments"},
            RenderEnvironmentCapabilities{
                true, selected_variant}),
        Catch::Matchers::ContainsSubstring(
            "duplicate packages for a graph"));
}

TEST_CASE("WP184 compiles draw sort providers and XR view policy for every graph variant",
          "[wp184][render-pipeline][draw-sort][typed]") {
    ResolvedRenderPipeline resolved;
    resolved.draw_sort = {
        {"opaque", {{"provider", "fixture.opaque"}}},
        {"transparent", {{"provider", "fixture.transparent"}}},
        {"xr_view_policy", "per_view"},
    };

    auto variants = std::vector{
        RenderPipelineGraphVariant::flat,
        RenderPipelineGraphVariant::preview,
    };
#if PELICAN_WITH_OPENXR
    variants.push_back(RenderPipelineGraphVariant::xr);
#endif
    for (const auto variant : variants) {
        const auto variant_name =
            std::string{renderPipelineGraphVariantName(variant)};
        CAPTURE(variant_name);
        resolved.graph_variant_policy =
            compileGraphVariantPolicy(
                GraphVariantPolicyRequest{variant});
        const auto compiled = compileRenderPipeline(resolved);
        REQUIRE(compiled.draw_sorting.authored);
        REQUIRE(compiled.draw_sorting.opaque.provider == "fixture.opaque");
        REQUIRE(compiled.draw_sorting.transparent.provider ==
                "fixture.transparent");
        REQUIRE(compiled.draw_sorting.xr_view_policy ==
                DrawSortXrViewPolicy::per_view);
        const auto metadata =
            serializeCompiledRenderPipelineMetadata(compiled);
        REQUIRE(metadata.at("draw_sort") == resolved.draw_sort);
        REQUIRE(metadata.contains("graph_variant") ==
                (variant == RenderPipelineGraphVariant::xr));
    }

    ResolvedRenderPipeline defaults;
    const auto compiled_defaults = compileRenderPipeline(defaults);
    REQUIRE_FALSE(compiled_defaults.draw_sorting.authored);
    REQUIRE(compiled_defaults.draw_sorting.opaque.provider ==
            "state_batched_v1");
    REQUIRE(compiled_defaults.draw_sorting.transparent.provider ==
            "back_to_front_v1");
    REQUIRE(compiled_defaults.draw_sorting.xr_view_policy ==
            DrawSortXrViewPolicy::logical_view_center);
    REQUIRE_FALSE(
        serializeCompiledRenderPipelineMetadata(compiled_defaults)
            .contains("draw_sort"));
}

TEST_CASE("WP184 draw sort declarations reject malformed policy with named errors",
          "[wp184][render-pipeline][draw-sort][validation]") {
    ResolvedRenderPipeline resolved;

    SECTION("unknown root key") {
        resolved.draw_sort = {{"opaque", {{"provider", "valid"}}},
                              {"surprise", true}};
        REQUIRE_THROWS_WITH(
            compileRenderPipeline(resolved),
            Catch::Matchers::ContainsSubstring(
                "resolved draw_sort has unknown key 'surprise'"));
    }

    SECTION("unknown phase key") {
        resolved.draw_sort = {
            {"transparent",
             {{"provider", "valid"}, {"direction", "front_to_back"}}},
        };
        REQUIRE_THROWS_WITH(
            compileRenderPipeline(resolved),
            Catch::Matchers::ContainsSubstring(
                "resolved draw_sort transparent has unknown key 'direction'"));
    }

    SECTION("empty provider") {
        resolved.draw_sort = {{"opaque", {{"provider", ""}}}};
        REQUIRE_THROWS_WITH(
            compileRenderPipeline(resolved),
            Catch::Matchers::ContainsSubstring(
                "resolved draw_sort opaque requires non-empty string provider"));
    }

    SECTION("unknown XR policy") {
        resolved.draw_sort = {{"xr_view_policy", "both_at_once"}};
        REQUIRE_THROWS_WITH(
            compileRenderPipeline(resolved),
            Catch::Matchers::ContainsSubstring(
                "resolved draw_sort has unknown xr_view_policy: both_at_once"));
    }
}

TEST_CASE("WP180 resolve failure cannot publish partial registration state",
          "[wp180][render-pipeline][resolve][atomic]") {
    struct RegistrationProbe {
        int render_targets = 3;
        int passes = 5;
        void publish(const ResolvedRenderPipeline &) {
            ++render_targets;
            ++passes;
        }
    } registration;
    const auto before_targets = registration.render_targets;
    const auto before_passes = registration.passes;
    const auto authored = baseConfig();
    const auto authored_before = authored;

    const auto resolve_then_publish = [&] {
        const auto resolved = resolveRenderPipeline(
            RenderPipelineRequest{authored, "invalid fixture"},
            RenderEnvironmentCapabilities{},
            RenderPipelineResolveDependencies{
                .transform_config = [](Json &config) {
                    config["partial_transform"] = true;
                },
                .validate_config = [](const Json &config) {
                    REQUIRE(config.at("partial_transform") == true);
                    throw std::runtime_error("fixture validation failed");
                },
            });
        registration.publish(resolved);
    };

    REQUIRE_THROWS_WITH(resolve_then_publish(), "fixture validation failed");
    REQUIRE(authored == authored_before);
    REQUIRE(registration.render_targets == before_targets);
    REQUIRE(registration.passes == before_passes);
}

TEST_CASE(
    "WP311 resolve and standalone preview reject pass ownership before providers and publication",
    "[wp311][render-pipeline][preview][pass-field-ownership]") {
    auto authored = baseConfig();
    auto &pass = authored["rendering_passes"][0]["passes"][0];
    pass["type"] = "fullscreen";
    pass["gpu_draw_source"] = {
        {"commands", "visible_draws"},
        {"count", "visible_draw_count"},
        {"max_draw_count", 2},
    };
    std::size_t provider_calls = 0;
    FrameGraphRuntimeContainer runtime;
    FrameGraphRuntimeContainer publication_control;
    publishGenerationProbe(
        publication_control,
        std::make_shared<CompiledRenderPipeline>());
    REQUIRE(publication_control.activeGeneration() == 1);
    const auto resolve_then_publish = [&] {
        const auto resolved = resolveRenderPipeline(
            RenderPipelineRequest{
                authored, "WP311 preview ownership fixture"},
            RenderEnvironmentCapabilities{},
            RenderPipelineResolveDependencies{
                .resolve_render_strategy =
                    [&](const Json &config,
                        const CompiledGraphVariantPolicy &) {
                        ++provider_calls;
                        return config;
                    },
            });
        publishGenerationProbe(
            runtime,
            std::make_shared<CompiledRenderPipeline>(
                compileRenderPipeline(resolved)));
    };

    REQUIRE_THROWS_WITH(
        resolve_then_publish(),
        Catch::Matchers::ContainsSubstring(
            "Pass 'scene' type 'fullscreen' does not own field "
            "'gpu_draw_source'"));
    CHECK(provider_calls == 0);
    CHECK(runtime.activeGeneration() == 0);

    REQUIRE_THROWS_WITH(
        precompilePreviewGraph(
            authored.dump(),
            [](std::string_view) -> std::string {
                throw std::runtime_error(
                    "standalone preview loader must not be called");
            },
            false),
        Catch::Matchers::ContainsSubstring(
            "Pass 'scene' type 'fullscreen' does not own field "
            "'gpu_draw_source'"));
}

TEST_CASE("WP180 preview precompile resolves presets through the shared boundary",
          "[wp180][render-pipeline][resolve][preview]") {
    auto preview_config = baseConfig();
    preview_config["rendering_passes"][0]
                  ["passes"][0]
                  ["material_filter"] = {
        {"include", {"preview_selected"}},
        {"exclude", {"preview_hidden"}},
    };
    const auto preset = Json{
        {"schema", "pelican.render_pipeline"},
        {"version", 1},
        {"name", "preview_fixture_v1"},
        {"config", std::move(preview_config)},
    };
    const auto load_pipeline = [&preset](std::string_view ref) {
        if (ref != "fixture://preview_pipeline") {
            throw std::runtime_error("unexpected preview resource");
        }
        return preset.dump();
    };
    const auto authored =
        Json{{"pipeline", {{"preset", "fixture://preview_pipeline"}}}};
    const auto program = precompilePreviewGraph(
        authored.dump(), load_pipeline,
        true);

    REQUIRE(program.generation != 0);
    REQUIRE(program.composed_config.dump().find("swapchain") ==
            std::string::npos);
    REQUIRE(program.composed_config.dump().find("preview_capture") !=
            std::string::npos);
    REQUIRE(program.pass_names ==
            std::vector<std::string>{"scene", "output_transform"});
    const auto &preview_passes =
        program.composed_config
            .at("rendering_passes")
            .at(0)
            .at("passes");
    const auto preview_material =
        std::find_if(
            preview_passes.begin(),
            preview_passes.end(),
            [](const Json &pass) {
                return pass.value(
                           "name",
                           std::string{}) ==
                       "scene";
            });
    REQUIRE(
        preview_material !=
        preview_passes.end());
    REQUIRE(
        preview_material->at("material_filter")
            .at("include") ==
        Json{"preview_selected"});
    REQUIRE(
        preview_material->at("material_filter")
            .at("exclude") ==
        Json{"preview_hidden"});

    const auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{authored, "preview typed fixture"},
        RenderEnvironmentCapabilities{true,
                                      RenderPipelineGraphVariant::preview},
        RenderPipelineResolveDependencies{
            .load_pipeline_json = load_pipeline,
        });
    const auto compiled = compileRenderPipeline(resolved);
    REQUIRE(compiled.graph_variant_policy.variant ==
            RenderPipelineGraphVariant::preview);
    REQUIRE(serializeCompiledRenderPipelineMetadata(compiled) ==
            Json{{"pipeline_preset",
                  {{"ref", "fixture://preview_pipeline"},
                   {"name", "preview_fixture_v1"},
                   {"version", 1}}}});
}

TEST_CASE("WP181 compiles dump metadata into a typed immutable runtime contract",
          "[wp181][render-pipeline][compile][typed]") {
#if PELICAN_WITH_OPENXR
    constexpr auto selected_variant = RenderPipelineGraphVariant::xr;
    const auto selected_variant_name = std::string{"xr"};
    const auto selected_suffix = std::string{"#xr"};
#else
    constexpr auto selected_variant = RenderPipelineGraphVariant::flat;
    const auto selected_variant_name = std::string{"flat"};
    const auto selected_suffix = std::string{};
#endif
    ResolvedRenderPipeline resolved;
    resolved.normalized_config = {{"authoring_only", true}};
    resolved.shader_defines = {"PELICAN_TYPED_FIXTURE"};
    resolved.feature_names = {"typed_feature"};
    resolved.excluded_feature_names = {"xr_excluded"};
    resolved.projection_jitter = {
        {"provider", "typed_jitter"},
        {"pattern", "table"},
        {"phases", 2},
        {"offsets_px", Json::array({Json::array({0, 0.0}),
                                     Json::array({0.25, -0.125})})},
    };
    resolved.feature_instances = Json::array({
        {
            {"feature", "typed_feature"},
            {"parameters",
             {
                 {"bool_value", false},
                 {"floating_value", 0.125},
                 {"signed_value", -7},
                 {"string_value", "history_a"},
                 {"unsigned_value", Json(Json::number_unsigned_t{9})},
             }},
            {"ref", "fixture://typed_feature"},
        },
    });
    resolved.material_routing = hybridRouting();
    resolved.pipeline_preset = RenderPipelinePresetInfo{
        "fixture://typed_pipeline", "typed_pipeline_v1", 1};
    resolved.graph_variant_policy =
        compileGraphVariantPolicy(
            GraphVariantPolicyRequest{
                selected_variant});
    resolved.diagnostics = {
        {RenderPipelineDiagnosticKind::graph_variant_selected,
         selected_variant_name,
         "selected_by_environment"},
        {RenderPipelineDiagnosticKind::feature_excluded, "xr_excluded",
         "xr"},
    };
    resolved.used_features = true;

    const auto pipeline = std::make_shared<const CompiledRenderPipeline>(
        compileRenderPipeline(resolved));
    static_assert(std::is_const_v<
                  std::remove_reference_t<decltype(*pipeline)>>);

    REQUIRE(pipeline->shader_defines ==
            std::vector<std::string>{"PELICAN_TYPED_FIXTURE"});
    REQUIRE(pipeline->graph_variant_policy.variant ==
            selected_variant);
    REQUIRE(pipeline->graph_variant_policy
                .rendering_pass_name_suffix == selected_suffix);
    REQUIRE(pipeline->projection_jitter.has_value());
    REQUIRE(pipeline->projection_jitter->pattern ==
            ProjectionJitterPattern::table);
    REQUIRE(pipeline->projection_jitter->offsets_px.size() == 2);
    REQUIRE(std::holds_alternative<std::int64_t>(
        pipeline->projection_jitter->offsets_px.at(0).at(0)));
    REQUIRE(std::holds_alternative<double>(
        pipeline->projection_jitter->offsets_px.at(0).at(1)));
    REQUIRE(pipeline->material_routing.has_value());
    REQUIRE(pipeline->material_routing->routes.size() == 3);
    REQUIRE(pipeline->material_routing->routes.at(2).route ==
            MaterialRouteClass::forward_transparent);
    REQUIRE(pipeline->material_routing->routes.at(2).pass_contract ==
            MaterialPassContract::forward_transparent_v1);
    REQUIRE(pipeline->diagnostics == resolved.diagnostics);

    REQUIRE(pipeline->feature_instances.size() == 1);
    const auto &parameters = pipeline->feature_instances.front().parameters;
    const auto parameter = [&parameters](std::string_view name)
        -> const CompiledRenderFeatureParameterValue & {
        const auto found = std::find_if(
            parameters.begin(), parameters.end(),
            [name](const auto &candidate) { return candidate.name == name; });
        REQUIRE(found != parameters.end());
        return found->value;
    };
    REQUIRE(std::get<bool>(parameter("bool_value")) == false);
    REQUIRE(std::get<double>(parameter("floating_value")) == 0.125);
    REQUIRE(std::get<std::int64_t>(parameter("signed_value")) == -7);
    REQUIRE(std::get<std::string>(parameter("string_value")) ==
            "history_a");
    REQUIRE(std::get<std::uint64_t>(parameter("unsigned_value")) == 9);

    const auto metadata = serializeCompiledRenderPipelineMetadata(*pipeline);
    REQUIRE_FALSE(metadata.contains("authoring_only"));
    auto expected_metadata = Json{
        {"projection_jitter", *resolved.projection_jitter},
        {"feature_instances", resolved.feature_instances},
        {"material_routing", resolved.material_routing},
        {"pipeline_preset",
         {{"ref", "fixture://typed_pipeline"},
          {"name", "typed_pipeline_v1"},
          {"version", 1}}},
    };
#if PELICAN_WITH_OPENXR
    expected_metadata["graph_variant"] = "xr";
    expected_metadata["excluded_features"] =
        Json::array({"xr_excluded"});
#endif
    REQUIRE(metadata == expected_metadata);
    REQUIRE(metadata.dump() == expected_metadata.dump());
}

TEST_CASE("WP181 compile and graph-plan failures precede publication",
          "[wp181][render-pipeline][compile][atomic]") {
    struct PublicationProbe {
        int render_targets = 3;
        int passes = 5;
        void publish() {
            ++render_targets;
            ++passes;
        }
    } publication;

    SECTION("typed compile failure") {
        ResolvedRenderPipeline malformed;
        malformed.projection_jitter = {
            {"provider", "broken_table"},
            {"pattern", "table"},
            {"phases", 2},
            {"offsets_px", Json::array({Json::array({0.0, 0.0})})},
        };
        const auto compile_then_publish = [&] {
            (void)compileRenderPipeline(malformed);
            publication.publish();
        };

        REQUIRE_THROWS_WITH(
            compile_then_publish(),
            "resolved projection_jitter table offsets_px length must match phases");
    }

    SECTION("frame graph planning failure") {
        FrameGraphDefinition cyclic;
        cyclic.name = "cyclic";
        FrameGraphNodeDefinition first;
        first.name = "first";
        first.after = {"second"};
        FrameGraphNodeDefinition second;
        second.name = "second";
        second.after = {"first"};
        cyclic.nodes = {std::move(first), std::move(second)};
        const auto plan_then_publish = [&] {
            (void)planFrameGraph(cyclic);
            publication.publish();
        };

        REQUIRE_THROWS(plan_then_publish());
    }

    REQUIRE(publication.render_targets == 3);
    REQUIRE(publication.passes == 5);
}

} // namespace Pelican
