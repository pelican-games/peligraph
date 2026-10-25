// RC-3c1a / RC-3c1b: what the IR holds and what the new path refuses,
// checked on the IR itself (parseRenderGraph -> RenderGraphIR -> finalize ->
// planRenderGraph / toLogicalGraph). RC-3c1b removed the bridge test that
// compared this path with core's planning parse; the direct checks it held
// are here, each against an expectation written for it.
#include "registered_render_targets_test_support.hpp"
#include "render_graph_ir_test_support.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "compute_task_test_support.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/materialpassinfojsonparser.hpp"
#include "../src/core/renderingpass/passfieldownershipcapabilities.hpp"
#include "../src/core/renderingpass/renderingpassconfigjsonparser.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/rendertargetmetadata.hpp"
#include "../src/core/renderingpass/rendertargetmetadataresolver.hpp"
#include "../src/core/renderingpass/rendertargetnameresolver.hpp"
#include "../src/project/featurecompose.hpp"
#include "../src/project/renderconfigsection.hpp"
#include "../src/project/rendergraphparse.hpp"
#include "../src/project/renderingjsonnormalize.hpp"
#include "../src/project/renderpipeline.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

fs::path sourceRoot() { return fs::path{PELICAN_TEST_SOURCE_DIR}; }

Json readJson(const fs::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) throw std::runtime_error("cannot read " + path.generic_string());
    return Json::parse(file);
}

std::string readText(const fs::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) throw std::runtime_error("cannot read " + path.generic_string());
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

Json logicalJson(const CompiledLogicalRenderGraph &graph) {
    return Json::parse(compiledLogicalRenderGraphToJson(graph).dump());
}

// A fake device: whether an image of this format, usage and kind (cube or
// not) is supported as a local-read attachment.
struct DeviceFacts {
    bool tile_local = false;
    std::function<bool(ImageFormat, ImageUsage, bool cube)> supports;
};

RenderGraphDeviceFacts deviceFactsOf(const DeviceFacts &facts) {
    RenderGraphDeviceFacts result;
    result.tile_local_attachments = facts.tile_local;
    if (facts.supports) {
        result.supports_local_read_attachment =
            [supports = facts.supports](const RenderGraphImageCapabilityQuery &query) {
                return supports(query.format, query.usage, query.cube_compatible);
            };
    }
    return result;
}

// What the compiler runs on the final revision, or nothing.
struct Finalize {
    TargetPlanningPolicy planning;
    std::string suffix;
    std::vector<SceneAccelerationStructureDefinition> named;
    bool ray_tracing_project = false;
};

// The path's outcome: the stage that refused ("" accepted; "parse",
// "finalize", "plan", "logical") with its code and message, or the IR, the
// definitions, the plans and the logical graphs.
struct Outcome {
    std::string stage;
    std::string refusal;
    std::string code;
    std::optional<RenderGraphIR> ir;
    std::vector<FrameGraphDefinition> definitions;
    std::vector<FramePlan> plans;
    std::vector<Json> logical;
    std::vector<PlanningGraphConstraints> planning;
};

std::string codeOf(const std::exception &error) {
    if (const auto *named = dynamic_cast<const GraphRegistryError *>(&error)) return named->code();
    return {};
}

Outcome run(const Json &config, const DeviceFacts &device = {},
            const std::optional<Finalize> &finalize = std::nullopt) {
    Outcome outcome;
    const auto refused = [&](std::string stage, const std::exception &error) {
        outcome.stage = std::move(stage);
        outcome.refusal = error.what();
        outcome.code = codeOf(error);
    };
    RenderGraphIR ir;
    try {
        ir = parseRenderGraph(config, buildPassFieldOwnershipCapabilities());
    } catch (const std::exception &error) {
        refused("parse", error);
        return outcome;
    }
    if (finalize) {
        try {
            auto planning = finalize->planning;
            ir = finalizeRenderGraph(std::move(ir),
                                     FinalizeRenderGraphRequest{
                                         .named = finalize->named,
                                         .ray_tracing_project = finalize->ray_tracing_project,
                                         .compute_task_suffix = finalize->suffix,
                                         // RC-3c2a: a snapshot copy is sized at the output.
                                         .output_extent = {1920, 1080},
                                     },
                                     planning);
            outcome.planning = planning.graphs;
        } catch (const std::exception &error) {
            refused("finalize", error);
            return outcome;
        }
    }
    for (const auto &graph : ir.graphs) outcome.definitions.push_back(frameGraphDefinitionOf(ir, graph));
    outcome.ir = std::move(ir);
    try {
        for (const auto &graph : outcome.ir->graphs) {
            outcome.plans.push_back(planRenderGraph(*outcome.ir, graph));
        }
    } catch (const std::exception &error) {
        refused("plan", error);
        return outcome;
    }
    try {
        const ToLogicalGraphOptions options{.device = deviceFactsOf(device)};
        for (const auto &graph : outcome.ir->graphs) {
            outcome.logical.push_back(logicalJson(toLogicalGraph(*outcome.ir, graph, options)));
        }
    } catch (const std::exception &error) {
        refused("logical", error);
    }
    return outcome;
}

// The rendering section of a document, in the normal form, with its format
// classes resolved as the runtime package resolves them (B8G8R8A8_SRGB,
// 100x100) when it is a resolver_version 2 config.
Json prepared(const Json &document) {
    auto normalized = normalizeRenderingJson(document);
    Json section = isRenderGraphRoot(normalized) ? renderingSectionOrEmpty(normalized) : normalized;
    const auto every_target_has_a_class = [&] {
        const auto targets = section.find("render_targets");
        return targets != section.end() && targets->is_array() &&
               std::all_of(targets->begin(), targets->end(), [](const Json &target) {
                   return target.is_object() && target.contains("format_class") &&
                          target.at("format_class").is_string();
               });
    };
    if (section.value("resolver_version", 0) == 2 && every_target_has_a_class()) {
        section = resolveRenderTargetFormatClassesV2(section, vk::Format::eB8G8R8A8Srgb,
                                                     vk::Extent2D{100, 100}, false);
    }
    return section;
}

std::string loadEngineJson(std::string_view reference) {
    constexpr std::string_view prefix = "engine://";
    if (!reference.starts_with(prefix)) {
        throw std::runtime_error("expected an engine reference: " + std::string{reference});
    }
    return engineResourceOrThrow(reference.substr(prefix.size()));
}

ResolvedRenderPipeline composeProject(const Json &authored, const fs::path &project_root,
                                      const std::string &source) {
    const auto load = [project_root](std::string_view reference) -> std::string {
        constexpr std::string_view project = "project://";
        if (reference.starts_with(project)) {
            return readText(project_root / std::string{reference.substr(project.size())});
        }
        return loadEngineJson(reference);
    };
    return resolveRenderPipeline(
        RenderPipelineRequest{authored, source},
        RenderEnvironmentCapabilities{.runtime_shader_compiler_enabled = true},
        RenderPipelineResolveDependencies{
            .load_feature_json = load,
            .load_pipeline_json = load,
            .normalize_config =
                [](const Json &config, const std::vector<std::string> &features) {
                    return resolveRenderTargetFormatClassesV2(
                        config, vk::Format::eB8G8R8A8Srgb, vk::Extent2D{100, 100},
                        std::find(features.begin(), features.end(), "hdr") != features.end());
                },
        });
}

const NodeIR &irNode(const RenderGraphIR &ir, std::string_view name) {
    for (const auto &graph : ir.graphs) {
        for (const auto &node : graph.nodes) {
            if (node.name == name) return node;
        }
    }
    throw std::runtime_error("no node " + std::string{name});
}

ResolvedRenderPipeline composeWithFeature(const std::string &base, const Json &feature) {
    if (base == "example") {
        auto authored = readJson(sourceRoot() / "projects/example/passes/main_rendering_config.json");
        authored["rendering"]["features"].push_back(feature);
        return composeProject(authored, sourceRoot() / "projects/example", "example+feature");
    }
    auto authored = readJson(sourceRoot() / "projects/animgraph_demo/passes/main.json");
    authored["rendering"]["features"] = Json::array({feature});
    return composeProject(authored, sourceRoot() / "projects/animgraph_demo", "hybrid_v1+feature");
}

Json *composedTask(Json &config, std::string_view name) {
    if (config.contains("compute_tasks")) {
        for (auto &task : config["compute_tasks"]) {
            if (task.value("name", std::string{}) == name) return &task;
        }
    }
    for (auto &graph : config["rendering_passes"]) {
        if (!graph.contains("compute_tasks")) continue;
        for (auto &task : graph["compute_tasks"]) {
            if (task.value("name", std::string{}) == name) return &task;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("RC-3c1b IR: the shipping projects and the engine features plan on the new path",
          "[rendergraphir][rc3c1b]") {
    // The shipping projects, composed (presets and features expanded, graph
    // lowered, format classes resolved), and each engine feature on two
    // bases. Which features compose and which are refused (and why) is fixed:
    // a new refusal, a refusal that now composes or a feature added to the
    // engine fails.
    std::size_t shipping = 0;
    const std::vector<std::pair<std::string, std::string>> projects{
        {"example", "passes/main_rendering_config.json"},
        {"animgraph_demo", "passes/main.json"},
        {"posegraph_demo", "passes/main.json"},
        {"sprite_demo", "passes/main.json"},
        {"vrm_xr_demo", "passes/main.json"},
        {"raytracing_demo", "rendering.json"},
        {"pathtrace_demo", "rendering.json"},
    };
    for (const auto &[project, document] : projects) {
        DYNAMIC_SECTION(project) {
            const auto root = sourceRoot() / "projects" / project;
            const auto resolved = composeProject(readJson(root / document), root, project);
            const auto compiled = compileRenderPipeline(resolved);
            const bool ray = project == "raytracing_demo" || project == "pathtrace_demo";
            const auto outcome =
                run(prepared(resolved.composed_config), {},
                    Finalize{.planning = compiled.target_planning,
                             .named = compiled.scene_acceleration_structures,
                             .ray_tracing_project = ray});
            INFO(outcome.refusal);
            REQUIRE(outcome.stage.empty());
            std::size_t nodes = 0;
            for (const auto &definition : outcome.definitions) {
                nodes += static_cast<std::size_t>(std::count_if(
                    definition.nodes.begin(), definition.nodes.end(), [](const auto &node) {
                        return node.kind == FramePlanNodeKind::render ||
                               node.kind == FramePlanNodeKind::compute;
                    }));
            }
            CHECK(nodes > 0);
            // The ray projects plan the scene's build first.
            if (ray) CHECK(outcome.plans.at(0).nodes.at(0).name == "__scene_acceleration_structure");
        }
        ++shipping;
    }
    CHECK(shipping == 7);

    auto hybrid = readJson(sourceRoot() / "projects/animgraph_demo/passes/main.json");
    hybrid["rendering"]["features"] = Json::array();
    const std::vector<std::tuple<std::string, fs::path, Json>> bases{
        {"example", sourceRoot() / "projects/example",
         readJson(sourceRoot() / "projects/example/passes/main_rendering_config.json")},
        {"hybrid_v1", sourceRoot() / "projects/animgraph_demo", hybrid},
    };
    const std::map<std::string, std::string> known_refusals{
        {"example+clustered_lighting", "references unknown pass: deferred_lighting"},
        {"example+cube_capture", "references unknown pass 'deferred_lighting'"},
        {"example+editor", "render feature schema is not supported"},
        {"example+hdr", "HDR canonical pipeline requires a scene pass"},
        {"example+planar_reflection", "references unknown pass 'deferred_lighting'"},
        {"example+rt_shadow_mask", "feature.insertion_anchor_missing@1"},
        {"example+shadow_point", "references unknown pass: deferred_lighting"},
        {"example+shadow_spot", "references unknown pass: deferred_lighting"},
        {"example+sky_ambient", "feature.required_target_missing@1"},
        {"example+taa", "format_class is incompatible"},
        {"hybrid_v1+editor", "render feature schema is not supported"},
        {"hybrid_v1+taa", "role is incompatible"},
    };
    const std::set<std::string> composes{
        "example+debug_draw", "example+debug_text", "example+gizmo", "example+gpu_timing",
        "example+picking", "example+rt_shadow_mask_pipeline", "example+shadow_directional",
        "example+sprite", "example+velocity",
        "hybrid_v1+bloom", "hybrid_v1+clustered_lighting", "hybrid_v1+cube_capture",
        "hybrid_v1+debug_draw", "hybrid_v1+debug_text", "hybrid_v1+gizmo",
        "hybrid_v1+gpu_timing", "hybrid_v1+hdr", "hybrid_v1+picking",
        "hybrid_v1+planar_reflection", "hybrid_v1+rt_shadow_mask",
        "hybrid_v1+rt_shadow_mask_pipeline", "hybrid_v1+shadow_directional",
        "hybrid_v1+shadow_point", "hybrid_v1+shadow_spot", "hybrid_v1+sky_ambient",
        "hybrid_v1+sprite", "hybrid_v1+ui", "hybrid_v1+velocity",
    };
    std::set<std::string> seen;
    for (const auto &[base_name, base_root, base] : bases) {
        for (const auto &entry : fs::directory_iterator(sourceRoot() / "src/core/resources/features")) {
            if (entry.path().extension() != ".json") continue;
            const auto feature = "engine://features/" + entry.path().filename().string();
            auto authored = base;
            auto &features = authored["rendering"]["features"];
            if (!features.is_array()) features = Json::array();
            const auto present = std::any_of(features.begin(), features.end(), [&](const Json &value) {
                return value == feature || (value.is_object() && value.value("ref", "") == feature);
            });
            if (present) continue;
            features.push_back(feature);
            const auto name = base_name + "+" + entry.path().stem().string();
            seen.insert(name);
            INFO("feature case " << name);
            std::optional<ResolvedRenderPipeline> resolved;
            std::string refusal;
            try {
                resolved = composeProject(authored, base_root, name);
            } catch (const std::exception &error) {
                refusal = error.what();
            }
            if (const auto known = known_refusals.find(name); known != known_refusals.end()) {
                INFO("refusal: " << refusal);
                REQUIRE_FALSE(resolved);
                REQUIRE(refusal.find(known->second) != std::string::npos);
                continue;
            }
            INFO("refusal: " << refusal);
            REQUIRE(composes.contains(name));
            REQUIRE(resolved);
            const auto compiled = compileRenderPipeline(*resolved);
            const auto outcome = run(prepared(resolved->composed_config), {},
                                     Finalize{.planning = compiled.target_planning,
                                              .named = compiled.scene_acceleration_structures});
            INFO(outcome.refusal);
            REQUIRE(outcome.stage.empty());
            // The feature's own passes and tasks are in the graph; a feature
            // without any is named as such.
            std::set<std::string> nodes;
            for (const auto &definition : outcome.definitions) {
                for (const auto &node : definition.nodes) nodes.insert(node.name);
            }
            const auto feature_json = readJson(entry.path());
            std::vector<std::string> declared;
            for (const auto &item : feature_json.value("passes", Json::array())) {
                declared.push_back(item.contains("pass") ? item.at("pass").at("name").get<std::string>()
                                                         : item.at("name").get<std::string>());
            }
            for (const auto &task : feature_json.value("compute_tasks", Json::array())) {
                declared.push_back(task.at("name").get<std::string>());
            }
            static const std::set<std::string> without_nodes{"sprite", "gpu_timing"};
            CHECK(declared.empty() == without_nodes.contains(feature_json.at("name").get<std::string>()));
            for (const auto &declared_name : declared) {
                INFO("feature node " << declared_name);
                CHECK(nodes.contains(declared_name));
            }
        }
    }
    std::set<std::string> expected(composes.begin(), composes.end());
    for (const auto &[name, reason] : known_refusals) expected.insert(name);
    REQUIRE(seen == expected);
}

TEST_CASE("RC-3c2a a render target definition is made from the IR, and needs a resolved format",
          "[rendergraphir][rc3c2a][refusal]") {
    // The one way a definition is made (renderTargetDefinitionOf): the IR's
    // declaration, single-sample, materialized, unaliased; a fixed image's
    // extent_scale is 1. An image with a format_class and no format yet (the
    // editor's composition, the preview) is refused by name where a format is
    // needed.
    const auto config = Json::parse(R"json({"render_targets": [
      {"name": "fixed", "width": 32, "height": 16, "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
       "format_candidates": ["R8G8B8A8_UNORM"], "usage": ["COLOR_ATTACHMENT", "SAMPLED"], "history": true,
       "clear_color": [0.25, 0.5, 0.75, 1.0], "lifetime": "persistent"},
      {"name": "half", "extent_scale": 0.5, "format": "D32_SFLOAT", "usage": ["DEPTH_STENCIL_ATTACHMENT"],
       "layers": 3, "mip_levels": 2},
      {"name": "unresolved", "extent_scale": 1.0, "format_class": "scene", "usage": ["COLOR_ATTACHMENT"]}]})json");
    const auto ir = parseRenderGraph(config, buildPassFieldOwnershipCapabilities());
    const auto fixed = renderTargetDefinitionOf(ir.images.at(0));
    CHECK(fixed.fixed_extent == std::optional{ResolvedResourceExtent{32, 16}});
    CHECK(fixed.extent_scale == 1.0f);
    CHECK(fixed.format == ImageFormat::r16g16b16a16_sfloat);
    CHECK(fixed.format_candidates ==
          std::vector<ImageFormat>{ImageFormat::r16g16b16a16_sfloat, ImageFormat::r8g8b8a8_unorm});
    CHECK(fixed.history);
    CHECK(fixed.persistent);
    CHECK(fixed.history_clear_color == std::array<double, 4>{0.25, 0.5, 0.75, 1.0});
    CHECK(fixed.samples == 1);
    CHECK(fixed.storage_mode == RenderTargetStorageMode::materialized);
    CHECK_FALSE(fixed.alias_group);
    const auto half = renderTargetDefinitionOf(ir.images.at(1));
    CHECK_FALSE(half.fixed_extent);
    CHECK(half.extent_scale == 0.5f);
    CHECK(half.array_layers == 3);
    CHECK(half.mip_levels == ImageMipLevelCount{ImageMipLevelMode::fixed, 2});
    CHECK(half.usage == ImageUsage{ImageUsageBit::depth_stencil_attachment});
    TestSupport::requireNamedRefusal([&] { (void)renderTargetDefinitionsOf(ir); },
                                     "render_graph.image_format_unresolved@1", {{"resource", "unresolved"}});
}

TEST_CASE("RC-3c2a IR: a graph declares no images or buffers of its own", "[rendergraphir][rc3c2a]") {
    // Nothing composes a graph's own render_targets or buffers (a graph
    // document's images and a loop's copies are the config's) and nothing
    // would register them: the parse refuses them by name, each key in turn.
    // RC-3c2b1: nor compute_tasks (compose, a loop and Studio write the
    // config's).
    const auto local = Json::parse(R"json({
      "render_targets": [{"name": "color", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
                          "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}],
      "compute_tasks": [{"name": "own_task", "writes": ["x"]},
                        {"name": "shared_task", "reads": ["x"], "writes": ["y"]}],
      "rendering_passes": [
        {"name": "a",
         "passes": [{"name": "draw", "type": "fullscreen",
                     "output": {"color": [{"target": "color"}], "depth": null}}]},
        {"name": "b",
         "passes": [{"name": "present", "type": "fullscreen", "input": ["x"],
                     "output": {"color": [{"target": "swapchain"}], "depth": null}}]}]})json");
    for (const auto *key : {"buffers", "render_targets", "compute_tasks"}) {
        INFO(key);
        auto config = local;
        config["buffers"] = Json::array({Json{{"name", "x"}}, Json{{"name", "y"}}});
        config["rendering_passes"][1][key] = Json::array({Json{{"name", "unused_b"}}});
        const auto outcome = run(config);
        REQUIRE(outcome.stage == "parse");
        CHECK(outcome.refusal ==
              std::string{R"(render_graph.graph_declaration_unsupported@1 {"field":")"} + key +
                  R"(","graph":"b"})");
    }
    SECTION("the same declaration in the config is everyone's") {
        auto config = local;
        config["buffers"] = Json::array({Json{{"name", "x"}}, Json{{"name", "y"}},
                                         Json{{"name", "unused_top"}}});
        const auto outcome = run(config);
        REQUIRE(outcome.stage.empty());
        // The graph's passes, then the config's compute tasks.
        const auto &a = outcome.ir->graphs.at(0);
        REQUIRE(a.nodes.size() == 3);
        CHECK(a.nodes[0].scope == NodeDeclarationScope::graph);
        CHECK(a.nodes[1].name == "own_task");
        CHECK(a.nodes[1].scope == NodeDeclarationScope::shared);
        CHECK(a.nodes[2].name == "shared_task");
        CHECK(a.nodes[2].scope == NodeDeclarationScope::shared);
        // An unused declaration is still a resource of the logical graph.
        const auto &resources = outcome.logical.at(1).at("resources");
        CHECK(std::any_of(resources.begin(), resources.end(), [](const Json &resource) {
            return resource.at("name") == "unused_top";
        }));
        CHECK(outcome.definitions.at(0).declared_resources ==
              std::vector<std::string>{"color", "x", "y", "unused_top"});
    }
}

TEST_CASE("RC-3c1b IR: a loading attachment is a same-pixel use", "[rendergraphir][rc3c1b]") {
    const auto config = Json::parse(R"json({
      "render_targets": [{"name": "color", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
                          "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}],
      "rendering_passes": [{"name": "main", "passes": [
        {"name": "base", "type": "fullscreen", "output": {"color": [{"target": "color"}], "depth": null}},
        {"name": "overlay", "type": "fullscreen", "color_load_op": "load",
         "output": {"color": [{"target": "color"}], "depth": null}},
        {"name": "present", "type": "fullscreen", "input": ["color"],
         "output": {"color": [{"target": "swapchain"}], "depth": null}}]}]})json");
    auto ir = parseRenderGraph(config, buildPassFieldOwnershipCapabilities());
    auto &overlay = ir.graphs.at(0).nodes.at(1);
    REQUIRE(overlay.uses.size() == 1);
    const auto &use = overlay.uses.front();
    CHECK(use.role == ResourceUseRole::color_attachment);
    CHECK(use.access == ResourceUseAccess::read_write);
    REQUIRE(use.operations);
    CHECK(use.operations->load_op == AttachmentLoadOp::load);
    REQUIRE(use.footprint);
    CHECK(use.footprint->kind == LogicalReadFootprintKind::same_pixel);
    // The planner's view reads it from the use: a read of color, same_pixel.
    const auto planned = frameGraphDefinitionOf(ir, ir.graphs.at(0)).nodes.at(1);
    CHECK(planned.reads == std::vector<std::string>{"color"});
    REQUIRE(planned.read_footprints.size() == 1);
    CHECK(planned.read_footprints.front().footprint.kind == LogicalReadFootprintKind::same_pixel);
    // Control: without the use's footprint the planner's view has none.
    overlay.uses.front().footprint.reset();
    CHECK(frameGraphDefinitionOf(ir, ir.graphs.at(0)).nodes.at(1).read_footprints.empty());
}

TEST_CASE("RC-3c1b IR: tile-local eligibility from the IR and the device facts",
          "[rendergraphir][rc3c1b][tile-local]") {
    // Two depth images of one format, read pixel for pixel by a local read
    // and written as depth attachments: scene_depth (2D) and probe_depth (a
    // cube). DEPTH_STENCIL_ATTACHMENT | SAMPLED is neither transient nor an
    // alias candidate, so only tile-local makes either virtual. The device
    // supports D32_SFLOAT as a sampled depth attachment, but not as a cube
    // nor with another usage. gbuffer_depth is a third candidate that
    // another pass samples (arbitrary): never eligible.
    const auto config = Json::parse(R"json({
      "render_targets": [
        {"name": "scene_color", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "scene_depth", "extent_scale": 1.0, "format": "D32_SFLOAT",
         "usage": ["DEPTH_STENCIL_ATTACHMENT", "SAMPLED"]},
        {"name": "lit", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "probe_color", "extent_scale": 1.0, "width": 16, "height": 16,
         "format": "R8G8B8A8_UNORM", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "probe_depth", "extent_scale": 1.0, "width": 16, "height": 16, "dimension": "cube",
         "format": "D32_SFLOAT", "usage": ["DEPTH_STENCIL_ATTACHMENT", "SAMPLED"]},
        {"name": "probe_lit", "extent_scale": 1.0, "width": 16, "height": 16,
         "format": "R8G8B8A8_UNORM", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "shadow_depth", "extent_scale": 1.0, "format": "D32_SFLOAT",
         "usage": ["DEPTH_STENCIL_ATTACHMENT"]},
        {"name": "gbuffer_color", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "gbuffer_depth", "extent_scale": 1.0, "format": "D32_SFLOAT",
         "usage": ["DEPTH_STENCIL_ATTACHMENT", "SAMPLED"]}],
      "rendering_passes": [{"name": "main", "passes": [
        {"name": "geometry", "type": "raster",
         "output": {"color": [{"target": "scene_color"}], "depth": {"target": "scene_depth"}}},
        {"name": "resolve", "type": "fullscreen", "input": ["scene_depth"],
         "input_footprints": {"scene_depth": "same_pixel"},
         "output": {"color": [{"target": "lit"}], "depth": null}},
        {"name": "probe_geometry", "type": "raster",
         "output": {"color": [{"target": "probe_color"}], "depth": {"target": "probe_depth"}}},
        {"name": "probe_resolve", "type": "fullscreen", "input": ["probe_depth"],
         "input_footprints": {"probe_depth": "same_pixel"},
         "output": {"color": [{"target": "probe_lit"}], "depth": null}},
        {"name": "gbuffer", "type": "raster",
         "output": {"color": [{"target": "gbuffer_color"}], "depth": {"target": "gbuffer_depth"}}},
        {"name": "shade", "type": "fullscreen", "input": ["gbuffer_depth", "gbuffer_color", "lit", "probe_lit"],
         "output": {"color": [{"target": "swapchain"}], "depth": null}}]}]})json");
    const DeviceFacts device{.tile_local = true,
                             .supports = [](ImageFormat format, ImageUsage usage, bool cube) {
                                 return format == ImageFormat::d32_sfloat && !cube &&
                                        usage == (ImageUsageBit::depth_stencil_attachment |
                                                  ImageUsageBit::sampled);
                             }};
    const auto materialization = [](const Outcome &outcome, std::string_view name) {
        for (const auto &resource : outcome.logical.at(0).at("resources")) {
            if (resource.at("name").get<std::string>() == name) {
                return resource.at("materialization").get<std::string>();
            }
        }
        return std::string{};
    };
    const auto with_device = run(config, device);
    REQUIRE(with_device.stage.empty());
    const auto &ir = *with_device.ir;
    const auto device_formats = renderGraphDeviceTileLocalFormats(ir, deviceFactsOf(device));
    CHECK(device_formats == RenderGraphTileLocalFormats{{"gbuffer_depth", ImageFormat::d32_sfloat},
                                                        {"scene_depth", ImageFormat::d32_sfloat}});
    const auto eligible = renderGraphTileLocalFormats(ir, ir.graphs.at(0), device_formats);
    CHECK(eligible == RenderGraphTileLocalFormats{{"scene_depth", ImageFormat::d32_sfloat}});
    CHECK(materialization(with_device, "scene_depth") == "virtual");
    CHECK(materialization(with_device, "probe_depth") == "required");
    CHECK(materialization(with_device, "gbuffer_depth") == "required");
    // Without the device's support the same image is materialized.
    const auto without_device = run(config);
    CHECK(materialization(without_device, "scene_depth") == "required");
}

TEST_CASE("RC-3c1b IR: multiview candidates follow the authored implementation",
          "[rendergraphir][rc3c1b][multiview]") {
    for (const bool implementation : {false, true}) {
        auto config = Json::parse(R"json({
          "render_targets": [],
          "rendering_passes": [{"name": "main", "passes": [
            {"name": "post", "type": "fullscreen",
             "shader": {"vertex": "engine://fullscreen", "fragment": "engine://scene_present"},
             "output": {"color": [{"target": "swapchain"}], "depth": null}}]}]})json");
        if (implementation) {
            config["rendering_passes"][0]["passes"][0]["implementation"] = {{"provider", "fixture.post@1"}};
        }
        const auto ir = parseRenderGraph(config, buildPassFieldOwnershipCapabilities());
        const auto &info = std::get<FullscreenNodeInfo>(ir.graphs.at(0).nodes.at(0).info);
        CHECK(info.implementation_provider.has_value() == implementation);
        if (implementation) CHECK(*info.implementation_provider == "fixture.post@1");
        for (const bool enable : {false, true}) {
            INFO("implementation=" << implementation << " enable=" << enable);
            CHECK(renderGraphMultiviewCapableNodes(ir, enable).contains("post") ==
                  (enable && !implementation));
        }
    }
}

TEST_CASE("RC-3c1b IR: the acceleration-structure builds are inserted as written down",
          "[rendergraphir][rc3c1b][acceleration-structure]") {
    const auto config = Json::parse(R"json({
      "render_targets": [{"name": "color", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
                          "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}],
      "buffers": [{"name": "hits"}],
      "compute_tasks": [{"name": "trace", "ray_query": {"acceleration_structure": "characters"},
                         "writes": ["hits"]}],
      "rendering_passes": [{"name": "main", "passes": [
        {"name": "shade", "type": "fullscreen", "ray_query": {"acceleration_structure": "characters"},
         "output": {"color": [{"target": "color"}], "depth": null}},
        {"name": "present", "type": "fullscreen", "input": ["color"],
         "output": {"color": [{"target": "swapchain"}], "depth": null}}]}]})json");
    const Finalize finalize{.named = {{.name = "characters"}}};
    const auto outcome = run(config, {}, finalize);
    INFO(outcome.refusal);
    REQUIRE(outcome.stage.empty());
    // Independently written expectation: the scene's build, the named one
    // after it, at the head; the passes and the task wait on them.
    const auto &nodes = outcome.ir->graphs.at(0).nodes;
    REQUIRE(nodes.size() == 5);
    CHECK(nodes[0].name == "__scene_acceleration_structure");
    CHECK(nodes[0].shape == RenderNodeShape::acceleration_structure);
    CHECK(nodes[0].after.empty());
    CHECK(nodes[1].name == "__scene_acceleration_structure:characters");
    CHECK(nodes[1].after == std::vector<std::string>{"__scene_acceleration_structure"});
    CHECK(std::get<AccelerationStructureNodeInfo>(nodes[1].info).named == "characters");
    CHECK(nodes[2].name == "shade");
    CHECK(nodes[2].after == std::vector<std::string>{"__scene_acceleration_structure",
                                                     "__scene_acceleration_structure:characters"});
    CHECK(nodes[3].name == "present");
    CHECK(nodes[3].after.empty()); // no ray query
    CHECK(nodes[4].name == "trace");
    CHECK(nodes[4].after == std::vector<std::string>{"__scene_acceleration_structure",
                                                     "__scene_acceleration_structure:characters"});
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        CHECK(nodes[index].declaration_index == index);
    }
    CHECK(framePlanOrder(outcome.plans.at(0)) ==
          std::vector<std::string>{"__scene_acceleration_structure",
                                   "__scene_acceleration_structure:characters", "shade", "present",
                                   "trace"});
    // The capability every graph's target planning then requires.
    REQUIRE(outcome.planning.size() == 1);
    CHECK(outcome.planning.front().required_capabilities ==
          std::vector<std::string>{"pelican.vulkan.ray_query@1"});
    // An unknown named structure is refused by name.
    auto unknown = config;
    unknown["rendering_passes"][0]["passes"][0]["ray_query"]["acceleration_structure"] = "props";
    unknown["compute_tasks"][0].erase("ray_query");
    const auto refused = run(unknown, {}, finalize);
    CHECK(refused.stage == "finalize");
    CHECK(refused.code == "render_graph.acceleration_structure_unknown@1");
    CHECK(refused.refusal.find("\"node\":\"shade\"") != std::string::npos);
}

TEST_CASE("RC-3c1b IR: compute namespacing and frame_node_order", "[rendergraphir][rc3c1b]") {
    const auto config = Json::parse(R"json({
      "render_targets": [{"name": "color", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
                          "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}],
      "buffers": [{"name": "particles"}],
      "compute_tasks": [{"name": "simulate", "writes": ["particles"], "before": ["present"]},
                        {"name": "late", "after": ["simulate"]}],
      "frame_node_order": {"main#xr": ["draw", "late#xr"]},
      "rendering_passes": [{"name": "main", "passes": [
        {"name": "draw", "type": "fullscreen", "output": {"color": [{"target": "color"}], "depth": null}},
        {"name": "present", "type": "fullscreen", "input": ["color", "particles"],
         "output": {"color": [{"target": "swapchain"}], "depth": null}}]}]})json");
    const auto namespaced = run(config, {}, Finalize{.suffix = "#xr"});
    INFO(namespaced.refusal);
    REQUIRE(namespaced.stage.empty());
    const auto &nodes = namespaced.ir->graphs.at(0).nodes;
    CHECK(nodes.at(2).name == "simulate#xr");
    CHECK(nodes.at(3).after == std::vector<std::string>{"simulate#xr"});
    CHECK(namespaced.ir->shared_compute_tasks == std::vector<std::string>{"simulate#xr", "late#xr"});

    auto plain = config;
    plain["frame_node_order"] = {{"main", {"draw", "late"}}};
    const auto applied = run(plain, {}, Finalize{});
    REQUIRE(applied.stage.empty());
    CHECK(applied.ir->graphs.at(0).nodes.at(3).after == std::vector<std::string>{"simulate", "draw"});
    auto conflicting = plain;
    conflicting["frame_node_order"]["main"] = Json::array({"present", "simulate"});
    const auto refused = run(conflicting, {}, Finalize{});
    CHECK(refused.stage == "finalize");
    CHECK(refused.code == "frame_node_order.conflicts_dependency@1");
}

TEST_CASE("RC-3c1b IR: a compute task's ray query alone brings the acceleration-structure build",
          "[rendergraphir][rc3c1b][acceleration-structure]") {
    const auto resolved =
        composeWithFeature("hybrid_v1", Json("engine://features/planar_reflection.json"));
    const auto compiled = compileRenderPipeline(resolved);
    REQUIRE(compiled.scene_acceleration_structures.empty());
    const Finalize finalize{.planning = compiled.target_planning};
    const std::string build = "__scene_acceleration_structure";
    for (const bool query : {false, true}) {
        DYNAMIC_SECTION("compute ray_query " << query) {
            auto config = prepared(resolved.composed_config);
            auto *task = composedTask(config, "planar_reflection_filter_mip_1");
            REQUIRE(task != nullptr);
            if (query) (*task)["ray_query"] = {{"acceleration_structure", "scene"}};
            const auto outcome = run(config, {}, finalize);
            INFO(outcome.refusal);
            REQUIRE(outcome.stage.empty());
            // No raster pass queries: the task is the only reason.
            for (const auto &graph : outcome.ir->graphs) {
                for (const auto &node : graph.nodes) CHECK_FALSE(node.ray_query.has_value());
            }
            CHECK((outcome.ir->graphs.at(0).nodes.front().name == build) == query);
            const auto &filter = irNode(*outcome.ir, "planar_reflection_filter_mip_1");
            CHECK((std::find(filter.after.begin(), filter.after.end(), build) != filter.after.end()) ==
                  query);
            const auto capability =
                std::any_of(outcome.planning.begin(), outcome.planning.end(), [](const auto &graph) {
                    return std::find(graph.required_capabilities.begin(),
                                     graph.required_capabilities.end(),
                                     "pelican.vulkan.ray_query@1") != graph.required_capabilities.end();
                });
            CHECK(capability == query);
            // The build is found from the task even without the capability.
            auto ir = parseRenderGraph(config, buildPassFieldOwnershipCapabilities());
            ir = insertAccelerationStructureNodes(std::move(ir), {});
            CHECK((ir.graphs.at(0).nodes.front().name == build) == query);
        }
    }
}

TEST_CASE("RC-3c1b IR: a history read is a temporal use", "[rendergraphir][rc3c1b]") {
    // The shipping TAA feature, bound to the example as passfusion_test binds
    // it: taa_resolve reads taa_accum@history and the current scene colour.
    auto authored = readJson(sourceRoot() / "projects/example/passes/main_rendering_config.json");
    for (auto &target : authored["rendering"]["render_targets"]) {
        if (target.value("name", std::string{}) == "lit_color") target["format_class"] = "scene";
    }
    auto &features = authored["rendering"]["features"];
    features.push_back("engine://features/velocity.json");
    features.push_back(Json{{"ref", "engine://features/taa.json"},
                            {"parameters",
                             {{"scene_color", "lit_color"},
                              {"velocity", "velocity"},
                              {"depth", "offscreen_depth"},
                              {"downstream_color", "lit_color"}}}});
    const auto resolved = composeProject(authored, sourceRoot() / "projects/example", "example+taa");
    const auto ir = parseRenderGraph(prepared(resolved.composed_config),
                                     buildPassFieldOwnershipCapabilities());
    const auto &resolve = irNode(ir, "taa_resolve");
    std::size_t history = 0;
    std::size_t current = 0;
    for (const auto &use : resolve.uses) {
        if (use.role != ResourceUseRole::input) continue;
        INFO("use " << use.resource << (use.history ? "@history" : ""));
        if (use.history) {
            ++history;
            CHECK(use.resource == "taa_accum");
            REQUIRE(use.footprint);
            CHECK(use.footprint->kind == LogicalReadFootprintKind::temporal);
        } else {
            ++current;
            CHECK((!use.footprint || use.footprint->kind != LogicalReadFootprintKind::temporal));
        }
    }
    CHECK(history == 1);
    CHECK(current > 0);
}

TEST_CASE("RC-3c1b IR: a resource port's direction is its own", "[rendergraphir][rc3c1b]") {
    // planar_reflection_filter_mip_1 samples mip 0 of planar_reflection_color
    // and stores mip 1 of it.
    const auto resolved =
        composeWithFeature("hybrid_v1", Json("engine://features/planar_reflection.json"));
    const auto ir = parseRenderGraph(prepared(resolved.composed_config),
                                     buildPassFieldOwnershipCapabilities());
    const auto &filter = irNode(ir, "planar_reflection_filter_mip_1");
    std::map<std::string, const ResourceUse *> ports;
    for (const auto &use : filter.uses) {
        if (use.role == ResourceUseRole::resource_port) ports[use.binding.port] = &use;
    }
    REQUIRE(ports.contains("source_color"));
    REQUIRE(ports.contains("filtered_color"));
    const auto &source = *ports.at("source_color");
    const auto &destination = *ports.at("filtered_color");
    CHECK(source.resource == "planar_reflection_color");
    CHECK(source.access == ResourceUseAccess::read);
    CHECK(source.intent == LogicalAccessIntent::sampled);
    REQUIRE(source.subresource);
    CHECK(source.subresource->base_mip_level == 0);
    CHECK(destination.resource == "planar_reflection_color");
    CHECK(destination.access == ResourceUseAccess::write);
    CHECK(destination.intent == LogicalAccessIntent::storage);
    REQUIRE(destination.subresource);
    CHECK(destination.subresource->base_mip_level == 1);
}

TEST_CASE("RC-3c1b IR: every way a history read enters is a temporal use", "[rendergraphir][rc3c1b]") {
    // A current and a history read through each entry: a pass's input, a
    // resource port, a material resource, a compute task's reads.
    const auto config = Json::parse(R"json({
      "render_targets": [
        {"name": "color", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "accum", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"], "history": true},
        {"name": "depth", "extent_scale": 1.0, "format": "D32_SFLOAT", "usage": ["DEPTH_STENCIL_ATTACHMENT"]},
        {"name": "shaded", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}],
      "buffers": [{"name": "stats"}],
      "compute_tasks": [{"name": "measure", "reads": ["color", "accum@history"], "writes": ["stats"]}],
      "rendering_passes": [{"name": "main", "passes": [
        {"name": "draw", "type": "fullscreen", "output": {"color": [{"target": "color"}], "depth": null}},
        {"name": "blend", "type": "fullscreen", "input": ["color", "accum@history"],
         "resource_ports": {"now": {"resource": "color"}, "before": {"resource": "accum@history"}},
         "output": {"color": [{"target": "accum"}], "depth": null}},
        {"name": "shade", "type": "material", "material_contract": "forward_opaque_v1",
         "material_resources": {"now": "color", "before": "accum@history"},
         "output": {"color": [{"target": "shaded"}], "depth": {"target": "depth"}}}]}]})json");
    const auto ir = parseRenderGraph(config, buildPassFieldOwnershipCapabilities());
    const auto expect = [&](std::string_view node, ResourceUseRole role, std::size_t count) {
        std::size_t seen = 0;
        for (const auto &use : irNode(ir, node).uses) {
            if (use.role != role) continue;
            ++seen;
            INFO(node << " " << resourceUseRoleName(role) << " " << use.resource
                      << (use.history ? "@history" : ""));
            // CHECK, not REQUIRE: every entry is judged on its own.
            if (use.history) {
                CHECK((use.footprint && use.footprint->kind == LogicalReadFootprintKind::temporal));
            } else if (role == ResourceUseRole::material_resource) {
                CHECK((use.footprint && use.footprint->kind == LogicalReadFootprintKind::arbitrary));
            } else {
                CHECK_FALSE(use.footprint);
            }
        }
        CHECK(seen == count);
    };
    expect("blend", ResourceUseRole::input, 2);
    expect("blend", ResourceUseRole::resource_port, 2);
    expect("shade", ResourceUseRole::material_resource, 2);
    expect("measure", ResourceUseRole::input, 2);
}

TEST_CASE("RC-3c2a refusals: an image's format_class is a string, sized or not",
          "[rendergraphir][rc3c2a][refusal]") {
    // RC-3c1b read a graph's own image's format_class only where the planner
    // read it (an image with a width and a height, up to the first display
    // one). A graph declares no images now (render_graph.graph_declaration_
    // unsupported@1); the config's image is the render target parser's
    // contract, whatever its extent.
    const auto config = [](Json format_class, bool sized) {
        Json image{{"name", "t"},
                   {"extent_scale", 1.0},
                   {"format", "R8G8B8A8_UNORM"},
                   {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
                   {"format_class", std::move(format_class)}};
        if (sized) {
            image["width"] = 1;
            image["height"] = 1;
        }
        Json pass{{"name", "p"},
                  {"type", "fullscreen"},
                  {"output", {{"color", Json::array({Json{{"target", "swapchain"}}})}, {"depth", nullptr}}}};
        Json graph{{"name", "g"}, {"passes", Json::array({pass})}};
        return Json{{"render_targets", Json::array({image})}, {"rendering_passes", Json::array({graph})}};
    };
    for (const bool sized : {true, false}) {
        const auto refused = run(config(42, sized));
        INFO(refused.refusal);
        CHECK(refused.code == "render_graph.image_invalid@1");
        const auto detail = Json::parse(refused.refusal.substr(refused.refusal.find('{')));
        CHECK(detail.at("resource") == "t");
        CHECK(detail.at("field") == "format_class");
        CHECK(detail.at("reason") == "requires_string");
    }
    CHECK(run(config("data", true)).stage.empty());
}

namespace {

Json refusalBase() {
    return Json::parse(R"json({
      "render_targets": [
        {"name": "color", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "depth", "extent_scale": 1.0, "format": "D32_SFLOAT", "usage": ["DEPTH_STENCIL_ATTACHMENT", "SAMPLED"]},
        {"name": "accum", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT", "usage": ["COLOR_ATTACHMENT", "SAMPLED"], "history": true},
        {"name": "cube", "extent_scale": 1.0, "width": 16, "height": 16, "dimension": "cube", "format": "R8G8B8A8_UNORM", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}],
      "buffers": [{"name": "draws"}, {"name": "draw_count"}, {"name": "shared"}],
      "rendering_passes": [{"name": "main", "passes": [
        {"name": "draw", "type": "fullscreen", "output": {"color": [{"target": "color"}], "depth": null}},
        {"name": "geometry", "type": "material", "material_contract": "legacy_gbuffer_v1",
         "output": {"color": [{"target": "accum"}], "depth": {"target": "depth"}}},
        {"name": "present", "type": "fullscreen", "input": ["color"],
         "output": {"color": [{"target": "swapchain"}], "depth": null}}]}]})json");
}

Json &pass(Json &config, std::size_t index) { return config["rendering_passes"][0]["passes"][index]; }

struct RefusalCase {
    std::string name;
    std::function<void(Json &)> edit;
    std::string code;
    // A field the refusal's detail names, and its value.
    std::string subject_field;
    std::string subject;
};

void requireNamedRefusal(const RefusalCase &refusal, const Outcome &outcome) {
    INFO(refusal.name << ": " << outcome.refusal);
    REQUIRE_FALSE(outcome.stage.empty());
    CHECK(outcome.code == refusal.code);
    // GraphRegistryError's message: "<code> <detail>".
    const auto brace = outcome.refusal.find('{');
    REQUIRE(brace != std::string::npos);
    const auto detail = Json::parse(outcome.refusal.substr(brace));
    REQUIRE(detail.contains(refusal.subject_field));
    CHECK(detail.at(refusal.subject_field) == refusal.subject);
}

} // namespace

TEST_CASE("RC-3c1b refusals: what a revision must pass, parseRenderGraph refuses by name",
          "[rendergraphir][rc3c1b][refusal]") {
    const std::vector<RefusalCase> cases{
        {"a pass is not an object", [](Json &c) { pass(c, 2) = 5; }, "render_graph.not_object@1", "graph", "main"},
        {"a pass has no name", [](Json &c) { pass(c, 2).erase("name"); }, "render_graph.node_name_invalid@1", "graph", "main"},
        {"a clear colour without a color output",
         [](Json &c) { pass(c, 2)["clear_color"] = {0, 0, 0, 1}; pass(c, 2)["output"]["color"] = Json::array(); },
         "render_graph.attachment_option_without_output@1", "node", "present"},
        {"view_family is not a string", [](Json &c) { pass(c, 2)["view_family"] = 3; }, "render_graph.node_field_invalid@1", "field", "view_family"},
        {"view_select is empty", [](Json &c) { pass(c, 2)["view_select"] = Json::object(); }, "render_graph.node_field_invalid@1", "field", "view_select"},
        {"regions is not an array", [](Json &c) { pass(c, 2)["regions"] = "post"; }, "render_graph.node_field_invalid@1", "field", "regions"},
        {"a region repeats", [](Json &c) { pass(c, 2)["regions"] = {"post", "post"}; }, "render_graph.node_field_invalid@1", "node", "present"},
        {"a fullscreen schedule per_view", [](Json &c) { pass(c, 2)["schedule"] = "per_view"; }, "render_graph.node_field_invalid@1", "field", "schedule"},
        {"an unknown pass type", [](Json &c) { pass(c, 2)["type"] = "bogus"; }, "render_graph.pass_field_ownership@1", "node", "present"},
        {"a field the type does not own", [](Json &c) { pass(c, 2)["material_filter"] = Json::object(); }, "render_graph.pass_field_ownership@1", "node", "present"},
        {"a fullscreen pass without a color output", [](Json &c) { pass(c, 2)["output"]["color"] = Json::array(); }, "render_graph.pass_shape@1", "node", "present"},
        {"material_filter is not an object", [](Json &c) { pass(c, 1)["material_filter"] = 1; }, "render_graph.node_field_invalid@1", "field", "material_filter"},
        {"material_variant without a filter include", [](Json &c) { pass(c, 1)["material_variant"] = "shadow"; }, "render_graph.node_field_invalid@1", "field", "material_variant"},
        {"a snapshot copy without snapshot_after",
         [](Json &c) { pass(c, 2) = {{"name", "copy"}, {"type", "snapshot_copy"}, {"source", "color"}, {"destination", "accum"}}; },
         "render_graph.node_field_invalid@1", "field", "snapshot_after"},
        {"an input qualifier other than @history", [](Json &c) { pass(c, 2)["input"] = {"color@previous"}; }, "render_graph.pass_shape@1", "node", "present"},
        {"a compute read qualifier other than @history",
         [](Json &c) { c["compute_tasks"] = Json::array({{{"name", "task"}, {"reads", {"shared@previous"}}}}); },
         "render_graph.resource_reference_invalid@1", "resource", "shared@previous"},
        {"a resource port names a resource nothing reads",
         [](Json &c) { pass(c, 2)["resource_ports"] = {{"p", {{"resource", "depth"}}}}; },
         "render_graph.node_field_invalid@1", "field", "resource_ports"},
        {"input_footprints names a resource not read", [](Json &c) { pass(c, 2)["input_footprints"] = {{"depth", "same_pixel"}}; }, "render_graph.footprint_invalid@1", "resource", "depth"},
        {"a temporal footprint", [](Json &c) { pass(c, 2)["input_footprints"] = {{"color", "temporal"}}; }, "render_graph.footprint_invalid@1", "reason", "temporal_is_history"},
        {"a screen input that is not a name", [](Json &c) { pass(c, 1)["screen_inputs"] = {{"scene_depth", 4}}; }, "render_graph.node_field_invalid@1", "field", "screen_inputs"},
        {"a history material resource with a footprint",
         [](Json &c) { pass(c, 1)["material_resources"] = {{"prev", {{"resource", "accum@history"}, {"footprint", "same_pixel"}}}}; },
         "render_graph.footprint_invalid@1", "reason", "history_derives_temporal"},
        {"gpu_draw_source with one buffer for both",
         [](Json &c) { pass(c, 1)["gpu_draw_source"] = {{"commands", "draws"}, {"count", "draws"}, {"max_draw_count", 1}}; },
         "render_graph.node_field_invalid@1", "field", "gpu_draw_source"},
        {"ray_query is an empty object", [](Json &c) { pass(c, 2)["ray_query"] = Json::object(); }, "render_graph.node_field_invalid@1", "field", "ray_query"},
        {"an unknown resolution_domain", [](Json &c) { pass(c, 2)["resolution_domain"] = "galaxy"; }, "render_graph.node_field_invalid@1", "field", "resolution_domain"},
        {"an attachment with an unknown field", [](Json &c) { pass(c, 0)["output"]["color"][0]["blend"] = true; }, "render_graph.attachment_invalid@1", "reason", "unknown_field"},
        {"an attachment of two mips",
         [](Json &c) { pass(c, 0)["output"]["color"][0]["subresource"] = {{"mip", 0}, {"mip_count", 2}}; },
         "render_graph.attachment_invalid@1", "reason", "subresource_not_one_mip"},
        {"a swapchain attachment with a subresource",
         [](Json &c) { pass(c, 2)["output"]["color"][0]["subresource"] = {{"mip", 0}}; },
         "render_graph.attachment_invalid@1", "reason", "swapchain_subresource"},
        {"an unknown attachment load op", [](Json &c) { pass(c, 0)["output"]["color"][0]["load_op"] = "bogus"; }, "render_graph.attachment_invalid@1", "reason", "unknown_op"},
        {"an unknown pass color_load_op", [](Json &c) { pass(c, 0)["color_load_op"] = "bogus"; }, "render_graph.node_field_invalid@1", "field", "color_load_op"},
        {"dispatch.indirect is not an object",
         [](Json &c) { c["compute_tasks"] = Json::array({{{"name", "task"}, {"dispatch", {{"indirect", 3}}}}}); },
         "render_graph.node_field_invalid@1", "field", "dispatch.indirect"},
        {"a render target without a name", [](Json &c) { c["render_targets"][0].erase("name"); }, "render_graph.image_invalid@1", "field", "name"},
        {"a render target's history is not a boolean", [](Json &c) { c["render_targets"][2]["history"] = "yes"; }, "render_graph.image_invalid@1", "resource", "accum"},
        {"a render target named swapchain", [](Json &c) { c["render_targets"][0]["name"] = "swapchain"; }, "render_graph.image_invalid@1", "reason", "reserved"},
        {"a render target without extent_scale", [](Json &c) { c["render_targets"][0].erase("extent_scale"); }, "render_graph.image_invalid@1", "resource", "color"},
        {"two render targets share a name", [](Json &c) { c["render_targets"][1]["name"] = "color"; }, "render_graph.image_invalid@1", "reason", "duplicate"},
        {"a graph's own render_targets",
         [](Json &c) {
             c["rendering_passes"][0]["render_targets"] = Json::array(
                 {{{"name", "t"}, {"width", 1}, {"height", 1}, {"format", "R8G8B8A8_UNORM"}, {"format_class", 42}}});
         },
         "render_graph.graph_declaration_unsupported@1", "graph", "main"},
        {"a graph's own buffers",
         [](Json &c) { c["rendering_passes"][0]["buffers"] = Json::array({{{"name", "local"}}}); },
         "render_graph.graph_declaration_unsupported@1", "field", "buffers"},
        {"a buffer named swapchain", [](Json &c) { c["buffers"][0]["name"] = "swapchain"; }, "render_graph.buffer_invalid@1", "resource", "swapchain"},
        // RC-3c2a: in every revision (a preview is never finalized); the
        // copy's size waits on the output (finalizeRenderGraph).
        {"a snapshot copy of a format outside the copy contract",
         [](Json &c) {
             c["render_targets"].push_back({{"name", "ids"}, {"extent_scale", 1.0}, {"width", 8}, {"height", 8},
                                            {"format", "R32_UINT"}, {"usage", {"COLOR_ATTACHMENT", "TRANSFER_DST"}}});
             pass(c, 2) = {{"name", "copy"}, {"type", "snapshot_copy"}, {"source", "color"},
                           {"destination", "ids"}, {"snapshot_after", "draw"}};
         },
         "render_graph.snapshot_size_unavailable@1", "resource", "ids"},
        {"frame_node_order is not an object", [](Json &c) { c["frame_node_order"] = 1; }, "render_graph.frame_node_order_invalid@1", "reason", "not_an_object"},
        {"a graph with an empty name", [](Json &c) { c["rendering_passes"][0]["name"] = ""; }, "render_graph.graph_name_invalid@1", "reason", "empty"},
        {"two graphs share a name",
         [](Json &c) { c["rendering_passes"].push_back(c["rendering_passes"][0]); },
         "render_graph.graph_name_invalid@1", "graph", "main"},
        {"a material resource reading a cube view same-pixel",
         [](Json &c) {
             pass(c, 1)["material_resources"] = {{"env", {{"resource", "cube"}, {"view", "cube"}, {"footprint", "same_pixel"}}}};
         },
         "render_graph.footprint_invalid@1", "reason", "same_pixel_cube_view"},
        {"a material resource reading a subresource view same-pixel",
         [](Json &c) {
             pass(c, 1)["material_resources"] = {{"env", {{"resource", "color"},
                                                          {"subresource", {{"mip", 0}}},
                                                          {"footprint", "same_pixel"}}}};
         },
         "render_graph.footprint_invalid@1", "reason", "same_pixel_subresource"},
        {"a compute task with an unknown schedule",
         [](Json &c) { c["compute_tasks"] = Json::array({{{"name", "task"}, {"schedule", "sometimes"}}}); },
         "render_graph.compute_task_invalid@1", "field", "schedule"},
        {"a compute ray_query without its acceleration structure",
         [](Json &c) { c["compute_tasks"] = Json::array({{{"name", "task"}, {"ray_query", Json::object()}}}); },
         "render_graph.compute_task_invalid@1", "field", "ray_query"},
        {"a compute ray_query with an empty acceleration structure",
         [](Json &c) { c["compute_tasks"] = Json::array({{{"name", "task"}, {"ray_query", {{"acceleration_structure", ""}}}}}); },
         "render_graph.compute_task_invalid@1", "reason", "empty_acceleration_structure"},
        {"ray_task_acceleration_structures is not an object",
         [](Json &c) { c["ray_task_acceleration_structures"] = 1; },
         "render_graph.compute_task_invalid@1", "reason", "not_an_object"},
        {"ray_task_acceleration_structures names no structure",
         [](Json &c) { c["ray_task_acceleration_structures"] = {{"task", ""}}; },
         "render_graph.compute_task_invalid@1", "reason", "requires_task_and_acceleration_structure"},
    };
    for (const auto &refusal : cases) {
        DYNAMIC_SECTION(refusal.name) {
            auto config = refusalBase();
            REQUIRE(run(prepared(config)).stage.empty());
            refusal.edit(config);
            const auto outcome = run(prepared(config));
            CHECK(outcome.stage == "parse");
            requireNamedRefusal(refusal, outcome);
        }
    }
}

TEST_CASE("RC-3c1b refusals: what only the final revision must pass, finalizeRenderGraph refuses by name",
          "[rendergraphir][rc3c1b][refusal]") {
    // A provider may add the task the table names (or give it ray work)
    // after the revision that holds the table: a revision is not refused
    // for it, the final one is.
    const auto with_task = [](Json extra) {
        Json task{{"name", "task"}, {"writes", {"shared"}}};
        for (auto it = extra.begin(); it != extra.end(); ++it) task[it.key()] = it.value();
        return Json::array({task});
    };
    const std::vector<RefusalCase> cases{
        {"ray_task_acceleration_structures naming no task",
         [&](Json &c) {
             c["compute_tasks"] = with_task({});
             c["ray_task_acceleration_structures"] = {{"ghost", "scene"}};
         },
         "render_graph.compute_task_invalid@1", "node", "ghost"},
        {"ray_task_acceleration_structures naming a task that traces nothing",
         [&](Json &c) {
             c["compute_tasks"] = with_task({});
             c["ray_task_acceleration_structures"] = {{"task", "scene"}};
         },
         "render_graph.compute_task_invalid@1", "reason", "task_declares_no_ray_work"},
        {"ray_task_acceleration_structures naming an unknown structure",
         [&](Json &c) {
             c["compute_tasks"] = with_task({{"ray_query", {{"acceleration_structure", "scene"}}}});
             c["ray_task_acceleration_structures"] = {{"task", "props"}};
         },
         "render_graph.acceleration_structure_unknown@1", "acceleration_structure", "props"},
        {"a format class without its format",
         [](Json &c) {
             c["render_targets"][0].erase("format");
             c["render_targets"][0]["format_class"] = "scene";
         },
         "render_graph.image_format_unresolved@1", "resource", "color"},
        {"frame_node_order naming a node the graph has not",
         [](Json &c) { c["frame_node_order"] = {{"main", {"draw", "ghost"}}}; },
         "render_graph.frame_node_order_unknown@1", "node", "ghost"},
    };
    for (const auto &refusal : cases) {
        DYNAMIC_SECTION(refusal.name) {
            auto config = refusalBase();
            refusal.edit(config);
            const auto input = normalizeRenderingJson(config);
            // Each revision may hold it ...
            REQUIRE(run(input).stage.empty());
            // ... the final one may not.
            const auto outcome = run(input, {}, Finalize{});
            CHECK(outcome.stage == "finalize");
            requireNamedRefusal(refusal, outcome);
        }
    }
}

TEST_CASE("RC-3c1b refusals: what the planner refused, planRenderGraph refuses by name",
          "[rendergraphir][rc3c1b][refusal]") {
    const std::vector<RefusalCase> cases{
        {"two nodes share a name", [](Json &c) { pass(c, 0)["name"] = "present"; }, "render_graph.duplicate_node@1", "node", "present"},
        {"a node with an empty name", [](Json &c) { pass(c, 0)["name"] = ""; }, "render_graph.node_name_empty@1", "graph", "main"},
        {"a read of an undeclared resource", [](Json &c) { pass(c, 2)["input"] = {"ghost"}; }, "render_graph.unknown_resource@1", "resource", "ghost"},
        {"a history read of an image without history", [](Json &c) { pass(c, 2)["input"] = {"color", "depth@history"}; },
         "render_graph.unknown_history_resource@1", "resource", "depth"},
        {"after names no node", [](Json &c) { pass(c, 2)["after"] = {"nowhere"}; }, "render_graph.order_reference_unknown@1", "reference", "nowhere"},
        {"a node after itself", [](Json &c) { pass(c, 2)["after"] = {"present"}; }, "render_graph.self_dependency@1", "node", "present"},
        {"two unordered writers",
         [](Json &c) {
             c["compute_tasks"] = Json::array({{{"name", "write_a"}, {"writes", {"shared"}}},
                                               {{"name", "write_b"}, {"writes", {"shared"}}}});
         },
         "render_graph.ambiguous_writes@1", "resource", "shared"},
        {"a cycle", [](Json &c) { pass(c, 0)["after"] = {"present"}; }, "render_graph.cycle@1", "graph", "main"},
    };
    for (const auto &refusal : cases) {
        DYNAMIC_SECTION(refusal.name) {
            auto config = refusalBase();
            refusal.edit(config);
            const auto outcome = run(prepared(config));
            CHECK(outcome.stage == "plan");
            requireNamedRefusal(refusal, outcome);
            // The logical graph refuses the same graph by the planner's name
            // first (its structure), except two unordered writers, which the
            // final revision's frame_node_order may order.
            const auto ir = parseRenderGraph(prepared(config), buildPassFieldOwnershipCapabilities());
            if (refusal.code == "render_graph.ambiguous_writes@1") {
                CHECK_NOTHROW(toLogicalGraph(ir, ir.graphs.at(0)));
            } else {
                try {
                    (void)toLogicalGraph(ir, ir.graphs.at(0));
                    FAIL("toLogicalGraph accepted it");
                } catch (const GraphRegistryError &error) {
                    CHECK(error.code() == refusal.code);
                }
            }
        }
    }
}


TEST_CASE("RC-3c1b IR: two unordered writers wait for the final frame_node_order",
          "[rendergraphir][rc3c1b][frame-node-order]") {
    // A revision (a provider's, the editor's) may write one buffer from two
    // tasks without an order; the final revision's frame_node_order orders
    // them, and only the final plan judges the writes.
    auto config = refusalBase();
    config["compute_tasks"] = Json::array({{{"name", "write_a"}, {"writes", {"shared"}}},
                                           {{"name", "write_b"}, {"writes", {"shared"}}}});
    const auto revision = parseRenderGraph(prepared(config), buildPassFieldOwnershipCapabilities());
    CHECK_NOTHROW(toLogicalGraph(revision, revision.graphs.at(0)));
    // Without an order the final plan refuses them by name ...
    const auto unordered = run(prepared(config), {}, Finalize{});
    CHECK(unordered.stage == "plan");
    CHECK(unordered.code == "render_graph.ambiguous_writes@1");
    // ... with one it plans them in that order.
    config["frame_node_order"] = {{"main", {"write_b", "write_a"}}};
    const auto ordered = run(prepared(config), {}, Finalize{});
    INFO(ordered.refusal);
    REQUIRE(ordered.stage.empty());
    const auto order = framePlanOrder(ordered.plans.at(0));
    CHECK(std::find(order.begin(), order.end(), "write_b") < std::find(order.begin(), order.end(), "write_a"));
}

TEST_CASE("RC-3c1b IR: a fixed-size image with a class and no format waits for the format",
          "[rendergraphir][rc3c1b][format-class]") {
    // The same 64x64 image with its format, and with its class only: the
    // second waits on the format (it is not refused before the final
    // revision; RC-3c2a: a snapshot is sized by finalize).
    auto config = refusalBase();
    config["render_targets"][0]["width"] = 64;
    config["render_targets"][0]["height"] = 64;
    const auto with_format = run(normalizeRenderingJson(config));
    INFO(with_format.refusal);
    REQUIRE(with_format.stage.empty());
    CHECK(snapshotCopyBytes(with_format.ir->images.at(0), {1920, 1080}) == std::size_t{64 * 64 * 4});
    config["render_targets"][0].erase("format");
    config["render_targets"][0]["format_class"] = "scene";
    const auto class_only = run(normalizeRenderingJson(config));
    INFO(class_only.refusal);
    REQUIRE(class_only.stage.empty());
    CHECK(class_only.ir->images.at(0).format_unresolved);
    CHECK(snapshotCopyBytes(class_only.ir->images.at(0), {1920, 1080}) == 0);
    // The final revision needs the format.
    CHECK(run(normalizeRenderingJson(config), {}, Finalize{}).code == "render_graph.image_format_unresolved@1");
}

TEST_CASE("RC-3c1b IR: ray_task_acceleration_structures selects a declared structure",
          "[rendergraphir][rc3c1b][acceleration-structure]") {
    // A task that queries, and one that traces, both on "scene"; the table
    // selects the declared `actors` for each. With the table the final IR
    // carries `actors` and the task waits on its build; without it neither.
    const std::vector<SceneAccelerationStructureDefinition> named{{.name = "actors"}};
    for (const bool tracing : {false, true}) {
        DYNAMIC_SECTION((tracing ? "ray tracing" : "ray query")) {
            auto config = refusalBase();
            Json task{{"name", "trace"}, {"writes", {"shared"}}};
            if (tracing) {
                task["ray_tracing"] = Json::object();
            } else {
                task["ray_query"] = {{"acceleration_structure", "scene"}};
            }
            config["compute_tasks"] = Json::array({task});
            const auto selected = [&](bool table) {
                auto input = config;
                if (table) input["ray_task_acceleration_structures"] = {{"trace", "actors"}};
                const auto outcome = run(normalizeRenderingJson(input), {}, Finalize{.named = named});
                INFO(outcome.refusal);
                REQUIRE(outcome.stage.empty());
                const auto &node = irNode(*outcome.ir, "trace");
                const auto &info = std::get<ComputeNodeInfo>(node.info);
                const auto value = tracing ? info.ray_tracing->acceleration_structure
                                           : info.ray_query_acceleration_structure.value_or("");
                const bool waits = std::find(node.after.begin(), node.after.end(),
                                             "__scene_acceleration_structure:actors") != node.after.end();
                return std::pair{value, waits};
            };
            CHECK(selected(false) == std::pair{std::string{"scene"}, false});
            CHECK(selected(true) == std::pair{std::string{"actors"}, true});
        }
    }
}

namespace {

// The execution parsers that refused the (c) group before: the material
// pass's material_resources parser (with the registered targets' names) and
// the compute task parser.
std::string executionRefusal(const Json &config) {
    try {
        const auto definitions = Pelican::test_support::renderTargetDefinitionsOf(
            Json{{"render_targets", config.value("render_targets", Json::array())}});
        std::unordered_map<std::string, GlobalRenderTargetId> ids;
        for (std::size_t index = 0; index < definitions.size(); ++index) {
            ids.emplace(definitions[index].name, GlobalRenderTargetId{static_cast<int>(index)});
        }
        std::unordered_set<std::string> buffers;
        for (const auto &buffer : config.value("buffers", Json::array())) {
            buffers.insert(buffer.at("name").get<std::string>());
        }
        const RenderTargetNameResolver names{[&](const std::string &name) {
            const auto found = ids.find(name);
            return found == ids.end() ? noRenderTargetId() : found->second;
        }};
        for (const auto &pass_json : config.at("rendering_passes").at(0).at("passes")) {
            if (pass_json.value("type", std::string{}) != "material") continue;
            PassDefinition pass_definition;
            pass_definition.name = pass_json.at("name").get<std::string>();
            pass_definition.pass_info = MaterialPassInfo{};
            parseMaterialPassResourcesFromJson(pass_definition, pass_json, names, buffers);
        }
        (void)Pelican::TestSupport::computeTaskDefinitionsOf(config, nullptr);
    } catch (const std::exception &error) {
        return error.what();
    }
    return {};
}

} // namespace

TEST_CASE("RC-3c1b refusals: the execution parsers refuse what the IR refuses first",
          "[rendergraphir][rc3c1b][refusal]") {
    // The (c) group RC-3c1a moved ahead of the execution parsers: the IR
    // refuses them by name; the execution parser still refuses the same
    // input (the IR's refusal comes first on the production path).
    const auto task = [](Json extra) {
        Json result{{"name", "task"},
                    {"shader", "engine://shaders/compute/fixture"},
                    {"writes", {"shared"}},
                    {"dispatch", {{"groups", {1, 1, 1}}}}};
        for (auto it = extra.begin(); it != extra.end(); ++it) result[it.key()] = it.value();
        return result;
    };
    const std::vector<std::pair<std::string, std::function<void(Json &)>>> cases{
        {"cube view same-pixel",
         [](Json &c) {
             pass(c, 1)["material_resources"] = {{"env", {{"resource", "cube"}, {"view", "cube"}, {"footprint", "same_pixel"}}}};
         }},
        {"unknown schedule", [&](Json &c) { c["compute_tasks"] = Json::array({task({{"schedule", "sometimes"}})}); }},
        {"table naming no task",
         [&](Json &c) {
             c["compute_tasks"] = Json::array({task({})});
             c["ray_task_acceleration_structures"] = {{"ghost", "scene"}};
         }},
    };
    for (const auto &[name, edit] : cases) {
        DYNAMIC_SECTION(name) {
            auto config = refusalBase();
            config["compute_tasks"] = Json::array({task({})});
            REQUIRE(executionRefusal(prepared(config)).empty());
            edit(config);
            const auto input = prepared(config);
            CHECK_FALSE(executionRefusal(input).empty());
            const auto outcome = run(input, {}, Finalize{});
            CHECK_FALSE(outcome.stage.empty());
            CHECK_FALSE(outcome.code.empty());
        }
    }
}

TEST_CASE("RC-3c1b IR: a format class without a format is planned unresolved",
          "[rendergraphir][rc3c1b]") {
    // The editor's composition and the preview read the config before the
    // runtime package resolves the classes: the image is accepted, planned
    // and given a logical graph; what needs its format refuses by name.
    auto config = refusalBase();
    config["render_targets"][0].erase("format");
    config["render_targets"][0]["format_class"] = "scene";
    const auto outcome = run(normalizeRenderingJson(config));
    INFO(outcome.refusal);
    REQUIRE(outcome.stage.empty());
    CHECK(outcome.ir->images.at(0).format_unresolved);
    CHECK_FALSE(outcome.ir->images.at(1).format_unresolved);
    // A device asked about its format.
    const auto refused = run(normalizeRenderingJson(config),
                             DeviceFacts{.tile_local = true, .supports = [](ImageFormat, ImageUsage, bool) {
                                             return true;
                                         }});
    CHECK(refused.code == "render_graph.image_format_unresolved@1");
    // A config image with neither is refused at the parse, as before.
    config["render_targets"][0].erase("format_class");
    CHECK(run(normalizeRenderingJson(config)).code == "render_graph.image_invalid@1");
}

TEST_CASE("RC-3c1b IR: a pass's inputs are written before it, compute reads keep their import",
          "[rendergraphir][rc3c1b][input-not-produced]") {
    // A compute task reads and writes a persistent image (its previous
    // contents): accepted. A pass reading an image nothing writes before it:
    // refused by name. A history read, a buffer, an image with an initial
    // asset are held before the frame.
    const auto config = Json::parse(R"json({
      "render_targets": [
        {"name": "state", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
         "usage": ["STORAGE", "SAMPLED"], "lifetime": "persistent"},
        {"name": "color", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "accum", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"], "history": true},
        {"name": "late", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}],
      "buffers": [{"name": "counts"}],
      "compute_tasks": [{"name": "evolve", "reads": ["state", "counts"], "writes": ["state"]}],
      "rendering_passes": [{"name": "main", "passes": [
        {"name": "draw", "type": "fullscreen", "input": ["accum@history"],
         "output": {"color": [{"target": "color"}], "depth": null}},
        {"name": "present", "type": "fullscreen", "input": ["color"],
         "output": {"color": [{"target": "swapchain"}], "depth": null}}]}]})json");
    const auto check = [](const Json &input) {
        const auto ir = parseRenderGraph(input, buildPassFieldOwnershipCapabilities());
        const auto &graph = ir.graphs.at(0);
        requireRenderGraphInputsProduced(ir, graph, planRenderGraph(ir, graph));
    };
    CHECK_NOTHROW(check(config));
    auto unproduced = config;
    unproduced["rendering_passes"][0]["passes"][1]["input"] = {"color", "late"};
    try {
        check(unproduced);
        FAIL("an input nothing writes was accepted");
    } catch (const GraphRegistryError &error) {
        CHECK(error.code() == "render_graph.input_not_produced@1");
        CHECK(error.detail().at("node") == "present");
        CHECK(error.detail().at("resource") == "late");
        CHECK(error.detail().at("graph") == "main");
    }
    // An image holding an asset before the frame may be read so.
    auto initial = unproduced;
    initial["render_targets"][3]["initial"] = "project://late.png";
    CHECK_NOTHROW(check(initial));
}

} // namespace Pelican
