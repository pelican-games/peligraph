#include "registered_render_targets_test_support.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/passfieldownershipcapabilities.hpp"
#include "../src/project/passfusion.hpp"
#include "../src/project/rendergraphparse.hpp"
#include "../src/project/renderpipeline.hpp"
#include "render_graph_root_test_support.hpp"
#include "render_graph_ir_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;

std::filesystem::path sourceRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
}

Json readJson(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error("cannot read " + path.generic_string());
    }
    return Json::parse(input);
}

std::string loadEngineJson(std::string_view reference) {
    constexpr std::string_view prefix = "engine://";
    if (!reference.starts_with(prefix)) {
        throw std::runtime_error("expected an engine reference: " +
                                 std::string{reference});
    }
    return engineResourceOrThrow(reference.substr(prefix.size()));
}

// The runtime package's composition (vulkanrendercompilerprogram.cpp):
// features and presets from the engine resources, format classes resolved
// against the output. A 100x100 B8G8R8A8_SRGB output stands in for the
// swapchain; the planner compares extents as rules, not as sizes.
ResolvedRenderPipeline compose(const Json &authored, std::string source) {
    return resolveRenderPipeline(
        RenderPipelineRequest{authored, std::move(source)},
        RenderEnvironmentCapabilities{
            .runtime_shader_compiler_enabled = true},
        RenderPipelineResolveDependencies{
            .load_feature_json = loadEngineJson,
            .load_pipeline_json = loadEngineJson,
            .normalize_config =
                [](const Json &config,
                   const std::vector<std::string> &features) {
                    return resolveRenderTargetFormatClassesV2(
                        config, vk::Format::eB8G8R8A8Srgb,
                        vk::Extent2D{100, 100},
                        std::find(features.begin(), features.end(),
                                  "hdr") != features.end());
                },
        });
}

using PassKey = std::pair<std::string, std::string>; // graph, pass

// The composed config's generic raster passes (`"type": "raster"`). Since
// RC-2 the vk-free FrameGraphDefinition no longer marks them (the marker
// only fed the deleted execution plan), so the pass's authored type is read.
std::set<PassKey> genericRasterPasses(const Json &composed) {
    std::set<PassKey> passes;
    for (const auto &pass_set : composed.at("rendering_passes")) {
        for (const auto &pass : pass_set.at("passes")) {
            if (pass.value("type", std::string{}) == "raster") {
                passes.insert({pass_set.at("name").get<std::string>(),
                               pass.at("name").get<std::string>()});
            }
        }
    }
    return passes;
}

// Fullscreen, read from the vk-free FrameGraphDefinition: a render node
// without geometry, not a generic raster pass, whose shader samples an
// authored input. debug_draw / debug_text / gizmo / ui / imgui passes sample
// none. A fullscreen pass without any input is not seen as one
// (requireFullscreenMatchesPassType pins that no composed pass here is such
// a pass).
bool isFullscreenNode(const FrameGraphNodeDefinition &node,
                      bool generic_raster) {
    return node.kind == FramePlanNodeKind::render &&
           !node.raster_geometry && !generic_raster &&
           !node.local_read_shader_inputs.empty();
}

ResourceExtentPlan extentPlan(const RenderTargetDefinition &target) {
    if (target.format_class == "display") return {};
    if (target.fixed_extent) {
        return {.kind = ResourceExtentKind::fixed,
                .width = target.fixed_extent->width,
                .height = target.fixed_extent->height};
    }
    return {.kind = ResourceExtentKind::output_relative,
            .scale_x = target.extent_scale,
            .scale_y = target.extent_scale};
}

struct FusionInput {
    std::vector<FrameGraphDefinition> definitions;
    std::vector<RenderTargetDefinition> targets;
    std::vector<CompiledLogicalRenderGraph> graphs;
    std::vector<PassFusionGraphFacts> facts;
};

// The production path from a composed config to the planner's input:
// parseRenderGraph, then toLogicalGraph without a device (RC-3c1b: the
// target-plan compile adds only the device's tile-local formats, which change
// a resource's materialization, not the graph).
FusionInput fusionInput(const Json &composed) {
    FusionInput input;
    input.targets = Pelican::test_support::renderTargetDefinitionsOf(composed);
    input.definitions = TestSupport::frameGraphsOf(composed);
    input.graphs = TestSupport::logicalGraphsOf(composed);
    REQUIRE(input.graphs.size() == input.definitions.size());
    std::map<std::string, const RenderTargetDefinition *, std::less<>>
        targets;
    for (const auto &target : input.targets) {
        targets.emplace(target.name, &target);
    }
    const auto raster = genericRasterPasses(composed);
    for (std::size_t index = 0; index < input.definitions.size(); ++index) {
        const auto &definition = input.definitions[index];
        PassFusionGraphFacts facts{.graph = definition.name};
        for (const auto &node : definition.nodes) {
            facts.nodes.push_back(
                {node.name,
                 isFullscreenNode(node,
                                  raster.contains({definition.name, node.name})),
                 node.once});
        }
        for (const auto &resource : input.graphs[index].resources) {
            if (resource.name == "swapchain") {
                facts.resources.push_back(
                    {resource.name, ResourceExtentPlan{}, "display"});
                continue;
            }
            const auto target = targets.find(resource.name);
            if (target == targets.end()) continue; // a buffer
            facts.resources.push_back({resource.name,
                                       extentPlan(*target->second),
                                       target->second->format_class});
        }
        input.facts.push_back(std::move(facts));
    }
    return input;
}

void requireFullscreenMatchesPassType(const Json &composed,
                                      const FusionInput &input) {
    std::map<std::pair<std::string, std::string>, std::string> types;
    for (const auto &pass_set : composed.at("rendering_passes")) {
        for (const auto &pass : pass_set.at("passes")) {
            types[{pass_set.at("name").get<std::string>(),
                   pass.at("name").get<std::string>()}] =
                pass.value("type", std::string{});
        }
    }
    for (const auto &definition : input.definitions) {
        for (const auto &node : definition.nodes) {
            const auto found = types.find({definition.name, node.name});
            if (found == types.end()) continue; // a compute task
            INFO(definition.name << "." << node.name << " is "
                                 << found->second);
            CHECK((found->second == "fullscreen") ==
                  isFullscreenNode(node, found->second == "raster"));
        }
    }
}

PassFusionPlan planFor(const ResolvedRenderPipeline &resolved,
                       const FusionInput &input) {
    return planPassFusion(input.graphs, input.facts,
                          resolved.target_planning);
}

std::vector<std::vector<std::string>> chainMembers(
    const PassFusionPlan &plan) {
    std::vector<std::vector<std::string>> result;
    for (const auto &chain : plan.chains) result.push_back(chain.members);
    return result;
}

std::string boundaryLine(const PassFusionBoundary &boundary) {
    return boundary.producer + "->" + boundary.consumer + " " +
           logicalValueIdName(boundary.value) + " " +
           std::string{passFusionBoundaryReasonName(boundary.reason)};
}

std::vector<std::string> boundaryLines(const PassFusionPlan &plan) {
    std::vector<std::string> result;
    for (const auto &boundary : plan.boundaries) {
        result.push_back(boundaryLine(boundary));
    }
    return result;
}

std::vector<std::string> chainResourceLines(const PassFusionChain &chain) {
    std::vector<std::string> result;
    for (const auto &resource : chain.chain_resources) {
        result.push_back(resource.producer + "->" + resource.consumer + " " +
                         logicalValueIdName(resource.value));
    }
    return result;
}

// The post-processing graph of A1: a three-stage same-pixel chain
// (tonemap, grade, vignette), a blur that reads its neighbourhood and heads
// a second chain, and a probe after both that samples the first chain's
// middle value (an exposure meter writing a fixed-size image). The composed
// config chains the passes in this order, so each chain is consecutive.
Json postChainSection() {
    return Json::parse(R"json({
      "render_targets": [
        {"name": "hdr_color", "extent_scale": 1.0,
         "format": "R16G16B16A16_SFLOAT",
         "format_class": "explicit(R16G16B16A16_SFLOAT)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "scene_depth", "extent_scale": 1.0, "format": "D32_SFLOAT",
         "format_class": "data", "usage": ["DEPTH_STENCIL_ATTACHMENT"]},
        {"name": "ldr_a", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
         "format_class": "explicit(R8G8B8A8_UNORM)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "ldr_b", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
         "format_class": "explicit(R8G8B8A8_UNORM)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "ldr_c", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
         "format_class": "explicit(R8G8B8A8_UNORM)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "luma", "extent_scale": 1.0, "width": 64, "height": 64,
         "format": "R16_SFLOAT", "format_class": "data",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "blurred", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
         "format_class": "explicit(R8G8B8A8_UNORM)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "composited", "extent_scale": 1.0,
         "format": "R8G8B8A8_UNORM",
         "format_class": "explicit(R8G8B8A8_UNORM)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "final_color", "extent_scale": 1.0,
         "format": "R8G8B8A8_UNORM",
         "format_class": "explicit(R8G8B8A8_UNORM)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}
      ],
      "rendering_passes": [{
        "name": "main",
        "passes": [
          {"name": "scene", "type": "material",
           "output": {"color": ["hdr_color"], "depth": "scene_depth"}},
          {"name": "tonemap", "type": "fullscreen", "input": ["hdr_color"],
           "output": {"color": ["ldr_a"], "depth": null},
           "shader": {"vertex": "engine://fullscreen",
                      "fragment": "engine://passthrough"}},
          {"name": "grade", "type": "fullscreen", "input": ["ldr_a"],
           "input_footprints": {"ldr_a": "same_pixel"},
           "output": {"color": ["ldr_b"], "depth": null},
           "shader": {"vertex": "engine://fullscreen",
                      "fragment": "engine://passthrough"}},
          {"name": "vignette", "type": "fullscreen", "input": ["ldr_b"],
           "input_footprints": {"ldr_b": "same_pixel"},
           "output": {"color": ["ldr_c"], "depth": null},
           "shader": {"vertex": "engine://fullscreen",
                      "fragment": "engine://passthrough"}},
          {"name": "blur", "type": "fullscreen", "input": ["ldr_c"],
           "input_footprints": {"ldr_c": {"kind": "neighborhood",
                                          "radius": 4}},
           "output": {"color": ["blurred"], "depth": null},
           "shader": {"vertex": "engine://fullscreen",
                      "fragment": "engine://passthrough"}},
          {"name": "composite", "type": "fullscreen", "input": ["blurred"],
           "input_footprints": {"blurred": "same_pixel"},
           "output": {"color": ["composited"], "depth": null},
           "shader": {"vertex": "engine://fullscreen",
                      "fragment": "engine://passthrough"}},
          {"name": "grain", "type": "fullscreen", "input": ["composited"],
           "input_footprints": {"composited": "same_pixel"},
           "output": {"color": ["final_color"], "depth": null},
           "shader": {"vertex": "engine://fullscreen",
                      "fragment": "engine://passthrough"}},
          {"name": "luma_probe", "type": "fullscreen", "input": ["ldr_b"],
           "output": {"color": ["luma"], "depth": null},
           "shader": {"vertex": "engine://fullscreen",
                      "fragment": "engine://passthrough"}}
        ]
      }]
    })json");
}

const std::vector<std::string> kPostChainBoundaries{
    "scene->tonemap hdr_color#1 not_fullscreen",
    "grade->luma_probe ldr_b#1 read_arbitrary",
    "vignette->blur ldr_c#1 read_neighborhood",
};

// The footprints the bundled fragment shaders actually have (read from
// src/core/resources/*.frag): the bundled passes declare no
// input_footprints, so the shadow graph lowers each of these sampled reads
// to arbitrary. Applied only by the counting below, to show what declaring
// them would open up; nothing authored changes.
struct ShaderFootprint {
    std::string_view pass;
    std::size_t input = 0;
    std::string_view footprint;
};

const std::vector<ShaderFootprint> kShaderFootprints{
    {"scene_present", 0, "same_pixel"},
    {"hdr_tonemap", 0, "same_pixel"},
    {"HighLuminanceExtraction", 0, "same_pixel"},
    {"FinalBloomComposite", 0, "same_pixel"},
    {"taa_composite", 0, "same_pixel"},
    {"taa_resolve", 0, "neighborhood:1"},
    {"taa_resolve", 1, "same_pixel"},
    {"lighting_pass", 0, "same_pixel"},
    {"lighting_pass", 1, "same_pixel"},
    {"lighting_pass", 2, "same_pixel"},
    {"lighting_pass", 3, "same_pixel"},
    {"lighting_pass", 4, "same_pixel"},
    {"lighting_pass", 5, "same_pixel"},
    {"deferred_lighting", 0, "same_pixel"},
    {"deferred_lighting", 1, "same_pixel"},
    {"deferred_lighting", 2, "same_pixel"},
    {"deferred_lighting", 3, "same_pixel"},
    {"deferred_lighting", 4, "same_pixel"},
    {"deferred_lighting", 5, "same_pixel"},
    {"ssao_blur_pass", 0, "neighborhood:2"},
    {"sky_background", 0, "same_pixel"},
    {"rt_shadow_mask", 0, "same_pixel"},
    {"rt_shadow_mask", 1, "same_pixel"},
    {"HorizontalBlur_0", 0, "neighborhood:4"},
    {"HorizontalBlur_1", 0, "neighborhood:4"},
    {"HorizontalBlur_2", 0, "neighborhood:4"},
    {"HorizontalBlur_3", 0, "neighborhood:4"},
    {"VerticalBlur_0", 0, "neighborhood:4"},
    {"VerticalBlur_1", 0, "neighborhood:4"},
    {"VerticalBlur_2", 0, "neighborhood:4"},
    {"VerticalBlur_3", 0, "neighborhood:4"},
};

Json shaderFootprintJson(std::string_view footprint) {
    constexpr std::string_view neighborhood = "neighborhood:";
    if (footprint.starts_with(neighborhood)) {
        return Json{{"kind", "neighborhood"},
                    {"radius",
                     std::stoul(std::string{
                         footprint.substr(neighborhood.size())})}};
    }
    return std::string{footprint};
}

Json withShaderFootprints(Json composed) {
    for (auto &pass_set : composed.at("rendering_passes")) {
        for (auto &pass : pass_set.at("passes")) {
            const auto name = pass.value("name", std::string{});
            for (const auto &entry : kShaderFootprints) {
                if (entry.pass != name || !pass.contains("input")) continue;
                const auto &inputs = pass.at("input");
                if (entry.input >= inputs.size()) continue;
                const auto resource = inputs.at(entry.input).get<std::string>();
                if (resource.find('@') != std::string::npos) continue;
                pass["input_footprints"][resource] =
                    shaderFootprintJson(entry.footprint);
            }
        }
    }
    return composed;
}

std::string summaryLine(const PassFusionSummary &summary) {
    std::ostringstream out;
    out << "chains=" << summary.chains
        << " fused_passes=" << summary.fused_passes
        << " eliminated_passes=" << summary.eliminated_passes
        << " eliminated_reads=" << summary.eliminated_reads
        << " eliminated_writes=" << summary.eliminated_writes
        << " boundaries=" << summary.boundaries << " reasons={";
    for (std::size_t index = 0; index < summary.boundary_reasons.size();
         ++index) {
        const auto &[reason, count] = summary.boundary_reasons[index];
        out << (index == 0 ? "" : ", ")
            << passFusionBoundaryReasonName(reason) << ": " << count;
    }
    out << "}";
    return out.str();
}

void requireNamedReasons(const PassFusionPlan &plan) {
    const auto reasons = passFusionBoundaryReasons();
    for (const auto &boundary : plan.boundaries) {
        INFO(boundaryLine(boundary));
        REQUIRE(std::find(reasons.begin(), reasons.end(), boundary.reason) !=
                reasons.end());
        REQUIRE_FALSE(passFusionBoundaryReasonName(boundary.reason).empty());
        REQUIRE_FALSE(boundary.detail.empty());
    }
}

struct BundledCase {
    std::string name;
    Json authored;
};

std::vector<BundledCase> bundledCases() {
    const auto example = readJson(
        sourceRoot() / "projects/example/passes/main_rendering_config.json");
    std::vector<BundledCase> result{
        {"example", example},
        {"animgraph_demo (hybrid_v1)",
         readJson(sourceRoot() / "projects/animgraph_demo/passes/main.json")},
    };
    // hybrid_v1 alone, with bloom, with hdr and with both. TAA binds a
    // color-role scene target, which hybrid_v1's lit_color is not, so it is
    // counted on the example as featurecompose_test's bundled TAA fixture
    // composes it (lit_color in the scene format class, velocity, taa bound
    // to the example's targets). hdr does not compose on the example (no
    // scene pass writes the display), so taa and hdr never meet here.
    auto base = result.back().authored;
    base["rendering"]["features"] = Json::array();
    const auto with = [&](std::string name, Json features) {
        auto authored = base;
        authored["rendering"]["features"] = std::move(features);
        result.push_back({std::move(name), std::move(authored)});
    };
    with("hybrid_v1", Json::array());
    with("hybrid_v1+bloom", Json::array({"engine://features/bloom.json"}));
    with("hybrid_v1+hdr", Json::array({"engine://features/hdr.json"}));
    with("hybrid_v1+bloom+hdr",
         Json::array({"engine://features/bloom.json",
                      "engine://features/hdr.json"}));
    auto taa = example;
    for (auto &target : taa["rendering"]["render_targets"]) {
        if (target.value("name", std::string{}) == "lit_color") {
            target["format_class"] = "scene";
        }
    }
    auto &features = taa["rendering"]["features"];
    features.push_back("engine://features/velocity.json");
    features.push_back(Json{{"ref", "engine://features/taa.json"},
                            {"parameters",
                             {{"scene_color", "lit_color"},
                              {"velocity", "velocity"},
                              {"depth", "offscreen_depth"},
                              {"downstream_color", "lit_color"}}}});
    result.push_back({"example+taa (bloom, ui, velocity, taa)", taa});
    return result;
}

} // namespace

TEST_CASE("pass fusion finds the same-pixel chains of a post-processing graph",
          "[passfusion][pf1]") {
    const auto resolved = compose(
        TestSupport::renderGraphRoot(postChainSection()), "pf1 post chain");
    const auto input = fusionInput(resolved.composed_config);
    requireFullscreenMatchesPassType(resolved.composed_config, input);
    const auto plan = planFor(resolved, input);

    REQUIRE(chainMembers(plan) ==
            std::vector<std::vector<std::string>>{
                {"tonemap", "grade", "vignette"},
                {"blur", "composite", "grain"}});
    REQUIRE(boundaryLines(plan) == kPostChainBoundaries);

    const auto &first = plan.chains.at(0);
    REQUIRE(first.graph == "main");
    REQUIRE(first.stage_count == 3);
    REQUIRE(chainResourceLines(first) ==
            std::vector<std::string>{"tonemap->grade ldr_a#1",
                                     "grade->vignette ldr_b#1"});
    // The probe reads grade's output: it stays a second color output of
    // the fused pass, materialized because the probe samples it anywhere.
    REQUIRE(first.kept_outputs.size() == 1);
    const auto &kept = first.kept_outputs.front();
    REQUIRE(kept.value == LogicalValueId{"ldr_b", 1});
    REQUIRE(kept.producer == "grade");
    REQUIRE(kept.readers == std::vector<std::string>{"luma_probe"});
    REQUIRE_FALSE(kept.live_out);
    REQUIRE(kept.materialized);
    REQUIRE(first.eliminated_reads == 2);
    REQUIRE(first.eliminated_writes == 1);

    const auto &second = plan.chains.at(1);
    REQUIRE(chainResourceLines(second) ==
            std::vector<std::string>{"blur->composite blurred#1",
                                     "composite->grain composited#1"});
    REQUIRE(second.kept_outputs.empty());
    REQUIRE(second.eliminated_reads == 2);
    REQUIRE(second.eliminated_writes == 2);

    using Ordered = nlohmann::ordered_json;
    const auto json = passFusionPlanToJson(plan);
    REQUIRE(json.at("schema") == "pelican.pass_fusion_plan");
    REQUIRE(json.at("stage") == "structural");
    REQUIRE(json.at("fused_nodes").size() == 2);
    REQUIRE(json.at("fused_nodes").at(0).at("members") ==
            Ordered::array({"tonemap", "grade", "vignette"}));
    REQUIRE(json.at("fused_nodes").at(0).at("kept_outputs").at(0) ==
            Ordered{{"resource", "ldr_b"},
                    {"version", 1},
                    {"producer", "grade"},
                    {"readers", Ordered::array({"luma_probe"})},
                    {"live_out", false},
                    {"materialized", true}});
    REQUIRE(json.at("boundaries").at(2).at("reason") == "read_neighborhood");
    REQUIRE(json.at("boundaries").at(2).at("detail") ==
            "blur reads ldr_c#1 as neighborhood radius 4");
    REQUIRE(json.at("summary") ==
            Ordered{
                {"chains", 2},
                {"fused_passes", 6},
                {"eliminated_passes", 4},
                {"eliminated_reads", 4},
                {"eliminated_writes", 3},
                {"boundaries", 3},
                {"boundary_reasons",
                 {{"not_fullscreen", 1},
                  {"read_neighborhood", 1},
                  {"read_arbitrary", 1}}}});
    // Planning is a pure function of its input.
    REQUIRE(passFusionPlanToJson(planFor(resolved, input)) == json);
}

TEST_CASE("pass fusion stops at a node whose fuse constraint is false",
          "[passfusion][pf1]") {
    auto section = postChainSection();
    section["target_planning"] = {
        {"graphs", {{"main", {{"nodes", {{"grade", {{"fuse", false}}}}}}}}}};
    const auto resolved = compose(TestSupport::renderGraphRoot(section),
                                  "pf1 fuse false");
    REQUIRE(resolved.target_planning.graphs.size() == 1);
    REQUIRE((resolved.target_planning.graphs.front().nodes ==
             std::vector<PlanningNodeConstraint>{
                 {.node = "grade", .fuse = false}}));
    const auto input = fusionInput(resolved.composed_config);
    const auto plan = planFor(resolved, input);

    REQUIRE(chainMembers(plan) ==
            std::vector<std::vector<std::string>>{
                {"blur", "composite", "grain"}});
    REQUIRE(boundaryLines(plan) ==
            std::vector<std::string>{
                "scene->tonemap hdr_color#1 not_fullscreen",
                "tonemap->grade ldr_a#1 fuse_disabled",
                "grade->vignette ldr_b#1 fuse_disabled",
                "grade->luma_probe ldr_b#1 fuse_disabled",
                "vignette->blur ldr_c#1 read_neighborhood",
            });
    REQUIRE(plan.boundaries.at(1).detail ==
            "target_planning.graphs.main.nodes.grade.fuse is false");

    // A constraint must name a node of the graph it is written for.
    auto unknown = resolved.target_planning;
    unknown.graphs.front().nodes.front().node = "no_such_pass";
    REQUIRE_THROWS_WITH(
        planPassFusion(input.graphs, input.facts, unknown),
        Catch::Matchers::ContainsSubstring(
            "pass fusion constraint references unknown node: "
            "main.no_such_pass"));
}

TEST_CASE("pass fusion target_planning fuse is a boolean written only when false",
          "[passfusion][pf1][target-planning]") {
    const auto metadata = [](const Json &target_planning) {
        auto section = postChainSection();
        section["target_planning"] = target_planning;
        const auto resolved = compose(TestSupport::renderGraphRoot(section),
                                      "pf1 fuse metadata");
        return serializeCompiledRenderPipelineMetadata(
                   compileRenderPipeline(resolved))
            .at("target_planning");
    };
    const auto nodes = [](Json entries) {
        return Json{{"graphs", {{"main", {{"nodes", std::move(entries)}}}}}};
    };

    // No fuse written: the dump is the one before PF-1.
    REQUIRE(metadata(nodes({{"grade", {{"serial", true}}}}))
                .at("graphs")
                .at("main")
                .at("nodes")
                .at("grade") == Json{{"serial", true}, {"isolate", false}});
    // fuse true is the default and is not written back.
    REQUIRE(metadata(nodes({{"grade", {{"fuse", true}}}}))
                .at("graphs")
                .at("main")
                .at("nodes")
                .at("grade") == Json{{"serial", false}, {"isolate", false}});
    REQUIRE(metadata(nodes({{"grade", {{"fuse", false}}}}))
                .at("graphs")
                .at("main")
                .at("nodes")
                .at("grade") == Json{{"serial", false},
                                     {"isolate", false},
                                     {"fuse", false}});

    auto section = postChainSection();
    section["target_planning"] = nodes({{"grade", {{"fuse", "no"}}}});
    REQUIRE_THROWS_WITH(
        compose(TestSupport::renderGraphRoot(section), "pf1 fuse typed"),
        Catch::Matchers::ContainsSubstring(
            "target_planning graph 'main' node 'grade' fuse must be a "
            "boolean"));
    section["target_planning"] = nodes({{"grade", {{"fused", false}}}});
    REQUIRE_THROWS(
        compose(TestSupport::renderGraphRoot(section), "pf1 fuse key"));
}

TEST_CASE("pass fusion names extent, format, in-place and outside-path boundaries",
          "[passfusion][pf1]") {
    // Each case is consecutive in the composed order, so only the named
    // condition decides it. t1 is the last scene pass and t2 the first of
    // post_main: only the phase anchors lie between them.
    const auto section = Json::parse(R"json({
      "render_targets": [
        {"name": "hdr", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
         "format_class": "explicit(R16G16B16A16_SFLOAT)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "x", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
         "format_class": "explicit(R16G16B16A16_SFLOAT)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "half", "extent_scale": 0.5, "format": "R16G16B16A16_SFLOAT",
         "format_class": "explicit(R16G16B16A16_SFLOAT)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "y", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
         "format_class": "explicit(R16G16B16A16_SFLOAT)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "m", "extent_scale": 1.0, "format": "R8_UNORM",
         "format_class": "data", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "n", "extent_scale": 1.0, "format": "R8_UNORM",
         "format_class": "data", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "p", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
         "format_class": "explicit(R16G16B16A16_SFLOAT)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "f_out", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
         "format_class": "explicit(R16G16B16A16_SFLOAT)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "q", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
         "format_class": "explicit(R16G16B16A16_SFLOAT)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "r", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
         "format_class": "explicit(R16G16B16A16_SFLOAT)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "s", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
         "format_class": "explicit(R16G16B16A16_SFLOAT)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "u", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
         "format_class": "explicit(R16G16B16A16_SFLOAT)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "w", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
         "format_class": "explicit(R16G16B16A16_SFLOAT)",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}
      ],
      "rendering_passes": [{
        "name": "main",
        "passes": [
          {"name": "scene", "type": "material",
           "output": {"color": ["hdr"], "depth": null}},
          {"name": "a", "type": "fullscreen", "input": ["hdr"],
           "output": {"color": ["x"], "depth": null}},
          {"name": "b", "type": "fullscreen", "input": ["x"],
           "input_footprints": {"x": "same_pixel"},
           "output": {"color": ["half"], "depth": null}},
          {"name": "c", "type": "fullscreen", "input": ["hdr"],
           "output": {"color": ["y"], "depth": null}},
          {"name": "c1", "type": "fullscreen", "input": ["y"],
           "input_footprints": {"y": "same_pixel"},
           "output": {"color": ["m"], "depth": null}},
          {"name": "c2", "type": "fullscreen", "input": ["m"],
           "input_footprints": {"m": "same_pixel"},
           "output": {"color": ["n"], "depth": null}},
          {"name": "d", "type": "fullscreen", "input": ["hdr"],
           "output": {"color": ["p"], "depth": null}},
          {"name": "f", "type": "fullscreen", "input": ["p"],
           "output": {"color": ["f_out"], "depth": null}},
          {"name": "e", "type": "fullscreen", "input": ["hdr"],
           "output": {"color": [{"target": "p", "load_op": "load"}],
                      "depth": null}},
          {"name": "g", "type": "fullscreen", "input": ["hdr"],
           "output": {"color": ["q"], "depth": null}},
          {"name": "h", "type": "fullscreen", "input": ["q"],
           "input_footprints": {"q": {"kind": "neighborhood", "radius": 1}},
           "output": {"color": ["r"], "depth": null}},
          {"name": "i", "type": "fullscreen", "input": ["q", "r"],
           "input_footprints": {"q": "same_pixel"},
           "output": {"color": ["s"], "depth": null}},
          {"name": "t1", "type": "fullscreen", "input": ["hdr"],
           "output": {"color": ["u"], "depth": null}},
          {"name": "t2", "type": "fullscreen", "canonical_anchor": "post_main",
           "input": ["u"], "input_footprints": {"u": "same_pixel"},
           "output": {"color": ["w"], "depth": null}}
        ]
      }]
    })json");
    const auto resolved = compose(TestSupport::renderGraphRoot(section),
                                  "pf1 boundaries");
    const auto input = fusionInput(resolved.composed_config);
    requireFullscreenMatchesPassType(resolved.composed_config, input);
    const auto plan = planFor(resolved, input);

    // c hands y to c1; c1 would hand a data mask on, another class. t1 and
    // t2 fuse across the phase boundary.
    REQUIRE(chainMembers(plan) ==
            std::vector<std::vector<std::string>>{{"c", "c1"}, {"t1", "t2"}});
    const auto lines = boundaryLines(plan);
    const auto has = [&](std::string_view line) {
        INFO(line);
        return std::find(lines.begin(), lines.end(), line) != lines.end();
    };
    CHECK(has("a->b x#1 extent_mismatch"));
    CHECK(has("c1->c2 m#1 format_class_mismatch"));
    CHECK(has("d->f p#1 read_arbitrary"));
    CHECK(has("d->e p#1 in_place_read_outside"));
    CHECK(has("g->h q#1 read_neighborhood"));
    CHECK(has("g->i q#1 outside_dependency"));
    CHECK(has("h->i r#1 read_arbitrary"));
    const auto detail = [&](std::string_view line) {
        for (const auto &boundary : plan.boundaries) {
            if (boundaryLine(boundary) == line) return boundary.detail;
        }
        return std::string{};
    };
    CHECK(detail("a->b x#1 extent_mismatch") ==
          "x#1 is output*1 but b writes half at output*0.5");
    CHECK(detail("c1->c2 m#1 format_class_mismatch") ==
          "m#1 is data but the chain hands y#1 as "
          "explicit(R16G16B16A16_SFLOAT)");
    CHECK(detail("d->e p#1 in_place_read_outside") ==
          "e loads p#1 in place and f reads it too");
    CHECK(detail("g->i q#1 outside_dependency") ==
          "h runs after g and before i");
    requireNamedReasons(plan);
}

TEST_CASE("pass fusion counts the bundled configurations",
          "[passfusion][pf1][bundled]") {
    for (const auto &bundled : bundledCases()) {
        DYNAMIC_SECTION(bundled.name) {
            const auto resolved = compose(bundled.authored, bundled.name);
            const auto composed = resolved.composed_config;
            for (const auto &[mode, config] :
                 {std::pair<std::string, Json>{"authored", composed},
                  std::pair<std::string, Json>{
                      "shader_footprints", withShaderFootprints(composed)}}) {
                const auto input = fusionInput(config);
                requireFullscreenMatchesPassType(config, input);
                const auto plan = planFor(resolved, input);
                requireNamedReasons(plan);
                // Every chain is two or more fullscreen passes.
                for (const auto &chain : plan.chains) {
                    REQUIRE(chain.members.size() >= 2);
                    REQUIRE(chain.stage_count == chain.members.size());
                    REQUIRE(chain.chain_resources.size() ==
                            chain.members.size() - 1);
                }
                std::size_t nodes = 0;
                std::size_t fullscreen = 0;
                for (const auto &facts : input.facts) {
                    nodes += facts.nodes.size();
                    fullscreen += static_cast<std::size_t>(std::count_if(
                        facts.nodes.begin(), facts.nodes.end(),
                        [](const auto &node) { return node.fullscreen; }));
                }
                std::cout << "PF1 config=" << bundled.name
                          << " footprints=" << mode
                          << " graphs=" << input.graphs.size()
                          << " nodes=" << nodes
                          << " fullscreen=" << fullscreen << " "
                          << summaryLine(summarizePassFusionPlan(plan))
                          << '\n';
                for (const auto &chain : plan.chains) {
                    std::cout << "PF1   chain";
                    for (const auto &member : chain.members) {
                        std::cout << ' ' << member;
                    }
                    std::cout << " kept=" << chain.kept_outputs.size()
                              << '\n';
                }
                for (const auto &boundary : plan.boundaries) {
                    std::cout << "PF1   boundary " << boundaryLine(boundary)
                              << " | " << boundary.detail << '\n';
                }
            }
        }
    }
}

namespace {

// RC-3c1a: the same facts from RenderGraphIR -- the node's shape, the
// logical graph toLogicalGraph makes, the config's images -- instead of the
// core parse, the authored type and the render target parser.
FusionInput fusionInputFromRenderGraph(const Json &composed) {
    FusionInput input;
    const auto ir = parseRenderGraph(composed, buildPassFieldOwnershipCapabilities());
    for (const auto &graph : ir.graphs) {
        input.definitions.push_back(frameGraphDefinitionOf(ir, graph));
        input.graphs.push_back(toLogicalGraph(ir, graph));
        PassFusionGraphFacts facts{.graph = graph.name};
        for (const auto &node : graph.nodes) {
            facts.nodes.push_back({node.name, node.shape == RenderNodeShape::fullscreen,
                                   node.schedule == NodeSchedule::once});
        }
        for (const auto &resource : input.graphs.back().resources) {
            if (resource.name == "swapchain") {
                facts.resources.push_back({resource.name, ResourceExtentPlan{}, "display"});
                continue;
            }
            const auto image = std::find_if(ir.images.begin(), ir.images.end(),
                                            [&](const auto &entry) { return entry.name == resource.name; });
            if (image == ir.images.end()) continue; // a buffer
            facts.resources.push_back(
                {resource.name,
                 image->format_class == "display" ? ResourceExtentPlan{} : image->extent,
                 image->format_class});
        }
        input.facts.push_back(std::move(facts));
    }
    return input;
}

std::vector<std::string> keptOutputLines(const PassFusionPlan &plan) {
    std::vector<std::string> result;
    for (const auto &chain : plan.chains) {
        for (const auto &kept : chain.kept_outputs) {
            std::string line = kept.producer + " " + logicalValueIdName(kept.value) +
                               (kept.live_out ? " live_out" : "");
            for (const auto &reader : kept.readers) line += " " + reader;
            result.push_back(std::move(line));
        }
    }
    return result;
}

} // namespace

TEST_CASE("RC-3c1a pass fusion plans the same from RenderGraphIR's facts",
          "[passfusion][rc3c1a][bridge]") {
    for (const auto &bundled : bundledCases()) {
        DYNAMIC_SECTION(bundled.name) {
            const auto resolved = compose(bundled.authored, bundled.name);
            for (const auto &config :
                 {resolved.composed_config, withShaderFootprints(resolved.composed_config)}) {
                const auto old_plan = planFor(resolved, fusionInput(config));
                const auto new_plan = planFor(resolved, fusionInputFromRenderGraph(config));
                CHECK(chainMembers(new_plan) == chainMembers(old_plan));
                CHECK(keptOutputLines(new_plan) == keptOutputLines(old_plan));
                CHECK(boundaryLines(new_plan) == boundaryLines(old_plan));
                std::size_t chains = old_plan.chains.size();
                std::cout << "RC3C1A passfusion config=" << bundled.name
                          << " chains=" << chains
                          << " boundaries=" << old_plan.boundaries.size() << '\n';
            }
        }
    }
}

} // namespace Pelican
