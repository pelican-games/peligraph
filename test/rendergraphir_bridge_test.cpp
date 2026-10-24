// RC-3c1a: the bridge between the planning path core runs today
// (JSON -> parseRenderTargetDefinitionsFromJson + parseFrameGraphDefinitionsFromConfigJson
// -> planFrameGraph / compileRenderingLogicalGraphs) and the new one
// (JSON -> parseRenderGraph -> RenderGraphIR -> planRenderGraph / toLogicalGraph).
// Both read the same input, normalizeRenderingJson(X) (RC-3b), and every
// value is compared field by field; a difference names its first path.
// RC-3c1b switches production to the new path and removes this file with the
// core planning parse.
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/renderingpass/computetaskdefinitionjson.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/graphtransformregistry.hpp"
#include "../src/core/renderingpass/materialpassinfojsonparser.hpp"
#include "../src/core/renderingpass/passfieldownershipcapabilities.hpp"
#include "../src/core/renderingpass/renderingpassconfigjsonparser.hpp"
#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/rendertargetmetadata.hpp"
#include "../src/core/renderingpass/rendertargetmetadataresolver.hpp"
#include "../src/core/renderingpass/rendertargetnameresolver.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerprogramdetail.hpp"
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

// ---------------------------------------------------------------------------
// Field-by-field comparison: the first path where two values differ ("" when
// they agree). Objects by key, arrays by index and size, scalars by value.

template <class J>
std::string firstDifference(const J &left, const J &right, const std::string &path = "") {
    if (left.type() != right.type()) return path + " (type)";
    if (left.is_object()) {
        std::set<std::string> keys;
        for (auto it = left.begin(); it != left.end(); ++it) keys.insert(it.key());
        for (auto it = right.begin(); it != right.end(); ++it) keys.insert(it.key());
        for (const auto &key : keys) {
            if (!left.contains(key) || !right.contains(key)) return path + "/" + key + " (missing)";
            if (auto found = firstDifference(left.at(key), right.at(key), path + "/" + key);
                !found.empty()) {
                return found;
            }
        }
        return {};
    }
    if (left.is_array()) {
        const auto count = std::min(left.size(), right.size());
        for (std::size_t index = 0; index < count; ++index) {
            if (auto found = firstDifference(left.at(index), right.at(index),
                                             path + "/" + std::to_string(index));
                !found.empty()) {
                return found;
            }
        }
        if (left.size() != right.size()) {
            return path + " (size " + std::to_string(left.size()) + " vs " +
                   std::to_string(right.size()) + ")";
        }
        return {};
    }
    return left == right ? std::string{} : path;
}

Json subresourceJson(const std::optional<ImageSubresourceRange> &range) {
    if (!range) return nullptr;
    return {{"base_mip_level", range->base_mip_level},
            {"level_count", range->level_count},
            {"base_array_layer", range->base_array_layer},
            {"layer_count", range->layer_count},
            {"mip_count_mode", static_cast<int>(range->mip_count_mode)}};
}

Json footprintJson(const LogicalReadFootprint &footprint) {
    return {{"kind", std::string{logicalReadFootprintKindName(footprint.kind)}},
            {"radius", footprint.radius ? Json(*footprint.radius) : Json(nullptr)}};
}

// The 24 fields of FrameGraphNodeDefinition. read_footprints is one entry per
// resource whose order no reader depends on (the logical graph keys it by
// resource, target planning finds it by resource): compared by resource.
Json nodeJson(const FrameGraphNodeDefinition &node) {
    auto footprints = Json::object();
    for (const auto &entry : node.read_footprints) {
        footprints[entry.resource] = footprintJson(entry.footprint);
    }
    auto accesses = Json::array();
    for (const auto &access : node.resource_accesses) {
        accesses.push_back({{"resource", access.resource},
                            {"intent", std::string{logicalAccessIntentName(access.intent)}}});
    }
    auto attachments = Json::array();
    for (const auto &attachment : node.attachments) {
        attachments.push_back(
            {{"resource", attachment.resource},
             {"subresource", subresourceJson(attachment.subresource)},
             {"aspect", std::string{attachmentAspectName(attachment.aspect)}},
             {"load_op", std::string{attachmentLoadOpName(attachment.operations.load_op)}},
             {"store_op", std::string{attachmentStoreOpName(attachment.operations.store_op)}}});
    }
    Json filter = nullptr;
    if (node.material_filter) {
        filter = {{"include", node.material_filter->include},
                  {"exclude", node.material_filter->exclude},
                  {"id", node.material_filter->id.value}};
    }
    auto view_select = Json::object();
    for (const auto &[axis, value] : node.view_select) view_select[axis] = value;
    return {
        {"name", node.name},
        {"kind", framePlanNodeKindName(node.kind)},
        {"declaration_index", node.declaration_index},
        {"reads", node.reads},
        {"reads_history", node.reads_history},
        {"read_footprints", footprints},
        {"local_read_shader_inputs", node.local_read_shader_inputs},
        {"resource_accesses", accesses},
        {"writes", node.writes},
        {"after", node.after},
        {"before", node.before},
        {"region_tags", node.region_tags},
        {"attachments", attachments},
        {"snapshot_after", node.snapshot_after},
        {"byte_size", node.byte_size},
        {"raster_geometry", node.raster_geometry},
        {"ray_query", node.ray_query ? Json(*node.ray_query) : Json(nullptr)},
        {"ray_query_acceleration_structure", node.ray_query_acceleration_structure},
        {"resolution_domain", std::string{renderResolutionDomainName(node.resolution_domain)}},
        {"material_filter", filter},
        {"material_variant", node.material_variant ? Json(*node.material_variant) : Json(nullptr)},
        {"view_family", node.view_family},
        {"view_select", view_select},
        {"once", node.once},
    };
}

Json definitionJson(const FrameGraphDefinition &definition) {
    auto nodes = Json::array();
    for (const auto &node : definition.nodes) nodes.push_back(nodeJson(node));
    return {{"name", definition.name},
            {"declared_resources", definition.declared_resources},
            {"history_resources", definition.history_resources},
            {"graph_transforms", definition.graph_transforms.size()},
            {"subgraph_replacements", definition.subgraph_replacements.size()},
            {"render_strategy", definition.render_strategy.has_value()},
            {"nodes", nodes}};
}

// Every field of the plan: the nodes in order and the barriers.
Json planJson(const FramePlan &plan) {
    auto nodes = Json::array();
    for (const auto &node : plan.nodes) {
        auto view_select = Json::object();
        for (const auto &[axis, value] : node.view_select) view_select[axis] = value;
        nodes.push_back({
            {"name", node.name},
            {"kind", framePlanNodeKindName(node.kind)},
            {"declaration_index", node.declaration_index},
            {"order", node.order},
            {"reads", node.reads},
            {"reads_history", node.reads_history},
            {"writes", node.writes},
            {"snapshot_after", node.snapshot_after},
            {"byte_size", node.byte_size},
            {"material_filter", node.material_filter
                                    ? Json{{"include", node.material_filter->include},
                                           {"exclude", node.material_filter->exclude}}
                                    : Json(nullptr)},
            {"material_variant", node.material_variant ? Json(*node.material_variant) : Json(nullptr)},
            {"view_family", node.view_family},
            {"view_select", view_select},
            {"once", node.once},
        });
    }
    auto barriers = Json::array();
    for (const auto &barrier : plan.barriers) {
        barriers.push_back({{"kind", barrier.kind},
                            {"resource", barrier.resource},
                            {"from", barrier.from},
                            {"to", barrier.to}});
    }
    return {{"name", plan.name}, {"nodes", nodes}, {"barriers", barriers}};
}

Json logicalJson(const CompiledLogicalRenderGraph &graph) {
    return Json::parse(compiledLogicalRenderGraphToJson(graph).dump());
}

// ---------------------------------------------------------------------------
// The two paths.

struct Outcome {
    std::string stage; // "" accepted; "parse", "plan", "logical"
    std::string refusal;
    std::vector<FrameGraphDefinition> definitions;
    std::vector<FramePlan> plans;
    std::vector<Json> logical;
};

// Device facts both paths are given: tile-local attachments with local read
// for the listed formats.
struct DeviceFacts {
    bool tile_local = false;
    std::vector<ImageFormat> local_read_formats;
};

RenderingTargetPlanDeviceFacts oldDeviceFacts(const DeviceFacts &facts) {
    RenderingTargetPlanDeviceFacts result;
    result.transient_attachments = facts.tile_local;
    result.dynamic_rendering_local_read = facts.tile_local;
    result.query_image_format_capability = [formats = facts.local_read_formats](
                                               const RenderTargetDefinition &target) {
        RenderingImageFormatCapability capability;
        capability.image_usage_supported = true;
        capability.supported_samples = {1};
        capability.local_read_attachment_supported =
            std::find(formats.begin(), formats.end(), target.format) != formats.end();
        return capability;
    };
    return result;
}

// What runs before planning, in the order the compiler runs it, or nothing.
struct Finalize {
    std::string suffix;
    std::vector<SceneAccelerationStructureDefinition> named;
    bool ray_tracing_project = false;
    bool apply_frame_node_order = false;
};

Outcome runOld(const Json &config, const DeviceFacts &device,
               const std::optional<Finalize> &finalize = std::nullopt) {
    Outcome outcome;
    std::vector<RenderTargetDefinition> targets;
    try {
        targets = parseRenderTargetDefinitionsFromJson(config);
        outcome.definitions = parseFrameGraphDefinitionsFromConfigJson(config);
        if (finalize) {
            std::vector<ComputeTaskDefinition> tasks;
            if (const auto found = config.find("compute_tasks"); found != config.end()) {
                for (const auto &task : *found) {
                    ComputeTaskDefinition definition;
                    definition.name = task.at("name").get<std::string>();
                    if (task.contains("ray_tracing")) {
                        definition.ray_tracing.emplace();
                        definition.ray_tracing->acceleration_structure =
                            task.at("ray_tracing").value("acceleration_structure", "scene");
                    }
                    if (task.contains("ray_query")) {
                        definition.ray_query_acceleration_structure =
                            task.at("ray_query").at("acceleration_structure").get<std::string>();
                    }
                    tasks.push_back(std::move(definition));
                }
            }
            // The compute task parser's override of a task's structure.
            if (const auto found = config.find("ray_task_acceleration_structures");
                found != config.end()) {
                for (const auto &[name, selected] : found->items()) {
                    for (auto &task : tasks) {
                        if (task.name != name) continue;
                        if (task.ray_tracing) {
                            task.ray_tracing->acceleration_structure = selected.get<std::string>();
                        } else if (task.ray_query_acceleration_structure) {
                            task.ray_query_acceleration_structure = selected.get<std::string>();
                        }
                    }
                }
            }
            CompiledRenderPipeline pipeline;
            pipeline.graph_variant_policy.rendering_pass_name_suffix = finalize->suffix;
            pipeline.scene_acceleration_structures = finalize->named;
            detail::namespaceComputeTasks(tasks, outcome.definitions, config, pipeline);
            detail::insertSceneAccelerationStructureNodes(
                outcome.definitions, pipeline.target_planning, tasks,
                finalize->ray_tracing_project, finalize->named);
            if (finalize->apply_frame_node_order && config.contains("frame_node_order")) {
                detail::applyFrameNodeOrder(outcome.definitions, config.at("frame_node_order"),
                                            false);
            }
        }
    } catch (const std::exception &error) {
        outcome.stage = "parse";
        outcome.refusal = error.what();
        return outcome;
    }
    try {
        for (const auto &definition : outcome.definitions) {
            outcome.plans.push_back(planFrameGraph(definition));
        }
    } catch (const std::exception &error) {
        outcome.stage = "plan";
        outcome.refusal = error.what();
        return outcome;
    }
    try {
        for (const auto &graph :
             compileRenderingLogicalGraphs(outcome.definitions, targets, oldDeviceFacts(device))) {
            outcome.logical.push_back(logicalJson(graph));
        }
    } catch (const std::exception &error) {
        outcome.stage = "logical";
        outcome.refusal = error.what();
    }
    return outcome;
}

struct NewOutcome : Outcome {
    std::optional<RenderGraphIR> ir;
    std::string code;
};

std::string codeOf(const std::exception &error) {
    if (const auto *named = dynamic_cast<const GraphRegistryError *>(&error)) return named->code();
    return {};
}

NewOutcome runNew(const Json &config, const DeviceFacts &device,
                  const std::optional<Finalize> &finalize = std::nullopt) {
    NewOutcome outcome;
    try {
        auto ir = parseRenderGraph(config, buildPassFieldOwnershipCapabilities());
        if (finalize) {
            ir = namespaceSharedComputeTasks(std::move(ir), finalize->suffix);
            ir = insertAccelerationStructureNodes(
                std::move(ir), AccelerationStructureNodeRequest{
                                   .planning_graphs = {},
                                   .ray_tracing_project = finalize->ray_tracing_project,
                                   .named = finalize->named});
            if (finalize->apply_frame_node_order) ir = applyFrameNodeOrder(std::move(ir), false);
        }
        for (const auto &graph : ir.graphs) {
            outcome.definitions.push_back(frameGraphDefinitionOf(ir, graph));
        }
        outcome.ir = std::move(ir);
    } catch (const std::exception &error) {
        outcome.stage = "parse";
        outcome.refusal = error.what();
        outcome.code = codeOf(error);
        return outcome;
    }
    try {
        for (const auto &graph : outcome.ir->graphs) {
            outcome.plans.push_back(planRenderGraph(*outcome.ir, graph));
        }
    } catch (const std::exception &error) {
        outcome.stage = "plan";
        outcome.refusal = error.what();
        outcome.code = codeOf(error);
        return outcome;
    }
    try {
        const ToLogicalGraphOptions options{
            .device = {.tile_local_attachments = device.tile_local,
                       .local_read_attachment_formats = device.local_read_formats}};
        for (const auto &graph : outcome.ir->graphs) {
            outcome.logical.push_back(logicalJson(toLogicalGraph(*outcome.ir, graph, options)));
        }
    } catch (const std::exception &error) {
        outcome.stage = "logical";
        outcome.refusal = error.what();
        outcome.code = codeOf(error);
    }
    return outcome;
}

struct Counts {
    std::size_t graphs = 0;
    std::size_t nodes = 0;
};

// Both paths agree: the same stage refuses, or every definition, plan and
// logical graph is the same field by field.
Counts requireSameOutcome(const std::string &name, const Outcome &old_outcome,
                          const NewOutcome &new_outcome) {
    INFO("input: " << name);
    INFO("old refusal (" << old_outcome.stage << "): " << old_outcome.refusal);
    INFO("new refusal (" << new_outcome.stage << "): " << new_outcome.refusal);
    REQUIRE(old_outcome.stage == new_outcome.stage);
    Counts counts;
    if (old_outcome.stage == "parse") return counts;
    REQUIRE(old_outcome.definitions.size() == new_outcome.definitions.size());
    for (std::size_t index = 0; index < old_outcome.definitions.size(); ++index) {
        const auto difference = firstDifference(definitionJson(old_outcome.definitions[index]),
                                                definitionJson(new_outcome.definitions[index]));
        INFO("graph " << old_outcome.definitions[index].name << " definition differs at "
                      << difference);
        REQUIRE(difference.empty());
        ++counts.graphs;
        counts.nodes += old_outcome.definitions[index].nodes.size();
    }
    if (old_outcome.stage == "plan") return counts;
    REQUIRE(old_outcome.plans.size() == new_outcome.plans.size());
    for (std::size_t index = 0; index < old_outcome.plans.size(); ++index) {
        const auto difference =
            firstDifference(planJson(old_outcome.plans[index]), planJson(new_outcome.plans[index]));
        INFO("plan " << old_outcome.plans[index].name << " differs at " << difference);
        REQUIRE(difference.empty());
    }
    if (old_outcome.stage == "logical") return counts;
    REQUIRE(old_outcome.logical.size() == new_outcome.logical.size());
    for (std::size_t index = 0; index < old_outcome.logical.size(); ++index) {
        const auto difference =
            firstDifference(old_outcome.logical[index], new_outcome.logical[index]);
        INFO("logical graph " << index << " differs at " << difference);
        REQUIRE(difference.empty());
    }
    return counts;
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

// The test-only single-graph form (top-level passes / compute_tasks) as the
// normal form: the config becomes the one graph of rendering_passes. Its
// declarations become the graph's own, as the planner read them.
Json wrapSingleGraph(const Json &config) {
    if (config.contains("rendering_passes") ||
        !(config.contains("passes") || config.contains("compute_tasks"))) {
        return config;
    }
    return Json{{"rendering_passes", Json::array({config})}};
}

Counts bridge(const std::string &name, const Json &config, const DeviceFacts &device = {},
              const std::optional<Finalize> &finalize = std::nullopt) {
    const auto old_outcome = runOld(config, device, finalize);
    const auto new_outcome = runNew(config, device, finalize);
    return requireSameOutcome(name, old_outcome, new_outcome);
}

// ---------------------------------------------------------------------------
// Inputs.

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

struct ComposedCase {
    std::string name;
    std::string how;
    Json config;
    std::optional<Finalize> finalize;
};

// The shipping projects (their rendering document composed with its presets
// and features) and the example with each engine feature added.
std::vector<ComposedCase> composedCases(std::vector<std::string> &skipped) {
    std::vector<ComposedCase> result;
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
        const auto root = sourceRoot() / "projects" / project;
        try {
            const auto authored = readJson(root / document);
            auto resolved = composeProject(authored, root, project);
            const auto compiled = compileRenderPipeline(resolved);
            const bool ray = project == "raytracing_demo" || project == "pathtrace_demo";
            result.push_back({project, "resolveRenderPipeline (flat): presets and features expanded, "
                                       "graph lowered, format classes resolved",
                              resolved.normalized_config,
                              Finalize{.suffix = {},
                                       .named = compiled.scene_acceleration_structures,
                                       .ray_tracing_project = ray,
                                       .apply_frame_node_order = true}});
        } catch (const std::exception &error) {
            skipped.push_back(project + ": " + error.what());
        }
    }
    // Each engine feature on two bases: the example (forward + deferred
    // presets) and animgraph_demo's hybrid_v1 preset without its features.
    auto hybrid = readJson(sourceRoot() / "projects/animgraph_demo/passes/main.json");
    hybrid["rendering"]["features"] = Json::array();
    const std::vector<std::tuple<std::string, fs::path, Json>> bases{
        {"example", sourceRoot() / "projects/example",
         readJson(sourceRoot() / "projects/example/passes/main_rendering_config.json")},
        {"hybrid_v1", sourceRoot() / "projects/animgraph_demo", hybrid},
    };
    for (const auto &[base_name, base_root, base] : bases) {
        for (const auto &entry :
             fs::directory_iterator(sourceRoot() / "src/core/resources/features")) {
            if (entry.path().extension() != ".json") continue;
            const auto feature = "engine://features/" + entry.path().filename().string();
            auto authored = base;
            auto &features = authored["rendering"]["features"];
            if (!features.is_array()) features = Json::array();
            const auto present =
                std::any_of(features.begin(), features.end(), [&](const Json &value) {
                    return value == feature ||
                           (value.is_object() && value.value("ref", "") == feature);
                });
            if (present) continue;
            features.push_back(feature);
            const auto name = base_name + "+" + entry.path().stem().string();
            try {
                auto resolved = composeProject(authored, base_root, name);
                result.push_back({name,
                                  base_name + " with the feature appended, resolveRenderPipeline (flat)",
                                  resolved.normalized_config, std::nullopt});
            } catch (const std::exception &error) {
                skipped.push_back(name + ": " + error.what());
            }
        }
    }
    return result;
}

// test/fixtures and projects/example configs with rendering_passes, as the
// frame planner's compatibility cases read them (a section, not composed).
std::vector<std::pair<std::string, Json>> staticCases() {
    std::vector<std::pair<std::string, Json>> cases;
    for (const auto &root : {sourceRoot() / "projects" / "example", sourceRoot() / "test" / "fixtures"}) {
        for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
            if (it->is_directory() && (it->path().filename() == "project_corpus" ||
                                       it->path().filename() == "studio_gui_graph_folded_edits")) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file() || it->path().extension() != ".json") continue;
            Json json;
            try {
                json = readJson(it->path());
            } catch (const std::exception &) {
                continue;
            }
            if (!json.is_object()) continue;
            if (isRenderGraphRoot(json) && json.contains("nodes") && json.at("nodes").is_array() &&
                std::any_of(json.at("nodes").begin(), json.at("nodes").end(), [](const Json &node) {
                    return node.is_object() && node.value("op", std::string{}).starts_with("render.");
                })) {
                continue;
            }
            const auto &section = isRenderGraphRoot(json) ? renderingSectionOrEmpty(json) : json;
            if (!section.is_object() || !section.contains("rendering_passes")) continue;
            cases.emplace_back(fs::relative(it->path(), sourceRoot()).generic_string(), json);
        }
    }
    return cases;
}

Json composedWith(const Json &config) {
    return composeRenderFeatureConfig(
               config, RenderFeatureComposeDependencies{
                           [](std::string_view reference) { return loadEngineJson(reference); }, true})
        .config;
}

// The inputs of frameplanner_test's plan fixtures, built as that test builds
// them.
std::vector<std::pair<std::string, Json>> planFixtureInputs() {
    std::vector<std::pair<std::string, Json>> result;
    result.emplace_back("plans/example_main_render (the example's section)",
                        readJson(sourceRoot() / "projects/example/passes/main_rendering_config.json"));
    result.emplace_back("plans/history_read (single-graph form)", Json::parse(R"json({
      "name":"history_accumulation",
      "render_targets":[{"name":"current_color"},{"name":"temporal_accum","history":true}],
      "passes":[
        {"name":"produce_current","type":"fullscreen","output":{"color":"current_color","depth":null}},
        {"name":"accumulate","type":"fullscreen","input":["current_color","temporal_accum@history"],
         "output":{"color":"temporal_accum","depth":null}}]})json"));
    result.emplace_back("plans/explicit_edges_base (single-graph form)", Json::parse(R"json({
      "name": "explicit_edges_base",
      "render_targets": [{"name": "rt_a"}, {"name": "rt_b"}, {"name": "rt_c"}],
      "passes": [
        {"name": "clear_a", "type": "fullscreen", "output": {"color": "rt_a", "depth": null}},
        {"name": "clear_b", "type": "fullscreen", "output": {"color": "rt_b", "depth": null}},
        {"name": "clear_c", "type": "fullscreen", "output": {"color": "rt_c", "depth": null}},
        {"name": "present", "type": "fullscreen", "input": ["rt_a", "rt_b", "rt_c"],
         "output": {"color": "swapchain", "depth": null}}]})json"));
    result.emplace_back("plans/explicit_edges_after_before (single-graph form)", Json::parse(R"json({
  "name": "explicit_edges_after_before",
  "render_targets": [
    {"name": "rt_a"},
    {"name": "rt_b"},
    {"name": "rt_c"}
  ],
  "passes": [
    {
      "name": "clear_a",
      "type": "fullscreen",
      "after": ["clear_b"],
      "output": {"color": "rt_a", "depth": null}
    },
    {
      "name": "clear_b",
      "type": "fullscreen",
      "before": ["clear_c"],
      "output": {"color": "rt_b", "depth": null}
    },
    {"name": "clear_c", "type": "fullscreen", "output": {"color": "rt_c", "depth": null}},
    {
      "name": "present",
      "type": "fullscreen",
      "input": ["rt_a", "rt_b", "rt_c"],
      "output": {"color": "swapchain", "depth": null}
    }
  ]
})json"));
    result.emplace_back("plans/snapshot_screen_input (composeRenderFeatureConfig, 64x32)", [] {
        auto composed = composedWith(Json::parse(R"json({
          "resolver_version": 2,
          "snapshots": [{"name": "opaque_color", "after": "opaque"}],
          "render_targets": [],
          "rendering_passes": [{"name": "snapshot_demo", "passes": [
            {"name": "opaque", "type": "fullscreen", "output": {"color": "swapchain", "depth": null}},
            {"name": "refract", "type": "fullscreen", "canonical_anchor": "post_ldr",
             "input": ["opaque_color"], "output": {"color": "swapchain", "depth": null}}]}]})json"));
        return resolveRenderTargetFormatClassesV2(composed, vk::Format::eB8G8R8A8Srgb,
                                                  vk::Extent2D{64, 32}, false);
    }());
    for (const auto &feature : {"debug_draw", "debug_text"}) {
        auto config = Json::parse(R"json({"render_targets": [], "rendering_passes": [{"name": "main",
          "passes": [{"name": "present", "type": "fullscreen",
                      "output": {"color": "swapchain", "depth": null}}]}]})json");
        config["features"] = Json::array({std::string{"engine://features/"} + feature + ".json"});
        result.emplace_back(std::string{"plans/"} + feature + "_feature_main (composeRenderFeatureConfig)",
                            composedWith(config));
    }
    result.emplace_back("plans/shadow_directional_feature_main (composeRenderFeatureConfig)", [] {
        auto config = Json::parse(R"json({
  "features": ["engine://features/shadow_directional.json"],
  "render_targets": [
    {"name": "gbuffer_albedo", "extent_scale": 1.0, "format": "B8G8R8A8_UNORM", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
    {"name": "gbuffer_normal", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
    {"name": "gbuffer_material", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
    {"name": "gbuffer_worldpos", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
    {"name": "g_emissive", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
    {"name": "offscreen_depth", "extent_scale": 1.0, "format": "D32_SFLOAT", "usage": ["DEPTH_STENCIL_ATTACHMENT"]},
    {"name": "ssao_blur", "extent_scale": 1.0, "format": "R8_UNORM", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
    {"name": "lit_color", "extent_scale": 1.0, "format": "B8G8R8A8_UNORM", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}
  ],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "gbuffer_pass",
          "type": "material",
          "output": {
            "color": ["gbuffer_albedo", "gbuffer_normal", "gbuffer_material", "gbuffer_worldpos", "g_emissive"],
            "depth": "offscreen_depth"
          }
        },
        {
          "name": "lighting_pass",
          "type": "fullscreen",
          "output": {"color": "lit_color", "depth": null},
          "input": ["gbuffer_albedo", "gbuffer_normal", "gbuffer_material", "gbuffer_worldpos", "g_emissive", "ssao_blur"],
          "shader": {"vertex": "engine://fullscreen", "fragment": "engine://fullscreen"},
          "uses_light_data": true
        }
      ]
    }
  ]
})json");
        return composedWith(config);
    }());
    result.emplace_back("plans/compute_kinds (single-graph form)", Json::parse(R"json({
      "name": "mixed", "buffers": ["particles", {"name": "visible_particles"}],
      "passes": [{"name": "present", "type": "fullscreen", "output": {"color": "swapchain", "depth": null}}],
      "compute_tasks": [
        {"name": "simulate", "reads": ["particles"], "writes": ["particles"]},
        {"name": "cull", "reads": ["particles"], "writes": ["visible_particles"]}]})json"));
    return result;
}

// The configs graph transform and subgraph replacement tests resolve, after
// the builtin (identity) provider.
std::vector<std::pair<std::string, Json>> providerInputs() {
    const auto transform_config = Json::parse(R"json({
      "resolver_version": 2,
      "graph_transforms": [{"name": "fixture.global_transform", "parameters": {"quality": "fixture"}}],
      "render_targets": [{"name": "scene", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
                          "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}],
      "rendering_passes": [{"name": "main", "passes": [
        {"name": "scene_source", "type": "fullscreen", "output": {"color": "scene", "depth": null}},
        {"name": "present", "type": "fullscreen", "input": ["scene"], "after": "scene_source",
         "output": {"color": "swapchain", "depth": null}}]}]})json");
    const auto region_config = Json::parse(R"json({
      "render_targets": [
        {"name": "scene_in", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "scratch", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "scene_out", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}],
      "rendering_passes": [{"name": "main", "region_replacements": [{"region": "region.post.fixture"}],
        "passes": [
          {"name": "pre", "type": "fullscreen", "before": "tone", "output": {"color": "scene_in", "depth": null},
           "shader": {"vertex": "engine://fullscreen", "fragment": "engine://scene_present"}},
          {"name": "tone", "type": "fullscreen", "regions": ["region.post.fixture", "region.post"],
           "input": ["scene_in"], "after": "pre", "before": "present",
           "output": {"color": "scene_out", "depth": null},
           "shader": {"vertex": "engine://fullscreen", "fragment": "engine://scene_present"}},
          {"name": "present", "type": "fullscreen", "input": ["scene_out"], "after": "tone",
           "output": {"color": "swapchain", "depth": null},
           "shader": {"vertex": "engine://fullscreen", "fragment": "engine://scene_present"}}]}]})json");
    std::vector<std::pair<std::string, Json>> result;
    {
        GraphTransformRegistry registry;
        const auto providers = registry.snapshot();
        result.emplace_back("graph_transform identity (graphtransformregistry_test's config)",
                            resolveLogicalGraphTransforms(transform_config, providers).config);
    }
    {
        SubgraphReplacementRegistry registry;
        const auto providers = registry.snapshot();
        result.emplace_back("subgraph identity (subgraphreplacementregistry_test's config)",
                            resolveTaggedSubgraphReplacements(region_config, providers).config);
    }
    result.emplace_back("graph_transform config as authored", transform_config);
    result.emplace_back("tagged region config as authored", region_config);
    return result;
}

std::size_t nonAnchorNodes(const std::vector<FrameGraphDefinition> &definitions) {
    std::size_t count = 0;
    for (const auto &definition : definitions) {
        count += static_cast<std::size_t>(std::count_if(
            definition.nodes.begin(), definition.nodes.end(), [](const auto &node) {
                return node.kind == FramePlanNodeKind::render ||
                       node.kind == FramePlanNodeKind::compute;
            }));
    }
    return count;
}

} // namespace

// ---------------------------------------------------------------------------
// F1: the two paths give the same definitions, plans and logical graphs.

TEST_CASE("RC-3c1a bridge: the shipping projects and the engine features plan alike on both paths",
          "[rendergraphir][rc3c1a][bridge]") {
    std::vector<std::string> skipped;
    const auto cases = composedCases(skipped);
    for (const auto &line : skipped) std::cout << "RC3C1A skipped " << line << '\n';
    std::size_t shipping = 0;
    for (const auto &composed : cases) {
        DYNAMIC_SECTION(composed.name) {
            const auto config = prepared(composed.config);
            const auto old_outcome = runOld(config, {});
            REQUIRE(old_outcome.stage.empty());
            // Not a vacuous agreement: the composed graph has passes.
            REQUIRE(nonAnchorNodes(old_outcome.definitions) > 0);
            const auto counts = requireSameOutcome(composed.name, old_outcome, runNew(config, {}));
            std::size_t finalized_nodes = 0;
            if (composed.finalize) {
                const auto finalized_old = runOld(config, {}, composed.finalize);
                const auto finalized_new = runNew(config, {}, composed.finalize);
                requireSameOutcome(composed.name + " (finalized)", finalized_old, finalized_new);
                for (const auto &definition : finalized_old.definitions) {
                    finalized_nodes += definition.nodes.size();
                }
            }
            std::cout << "RC3C1A input=" << composed.name << " how=\"" << composed.how
                      << "\" graphs=" << counts.graphs << " nodes=" << counts.nodes
                      << " finalized_nodes=" << finalized_nodes << '\n';
        }
        if (composed.name.find('+') == std::string::npos) ++shipping;
    }
    REQUIRE(shipping == 7);
}

TEST_CASE("RC-3c1a bridge: frame planner fixtures, static configs and provider configs plan alike",
          "[rendergraphir][rc3c1a][bridge]") {
    std::size_t accepted = 0;
    std::size_t refused = 0;
    const auto run = [&](const std::string &name, const Json &input, const std::string &how) {
        const auto config = prepared(input);
        const auto old_outcome = runOld(config, {});
        const auto counts = requireSameOutcome(name, old_outcome, runNew(config, {}));
        (old_outcome.stage.empty() ? accepted : refused) += 1;
        std::cout << "RC3C1A input=" << name << " how=\"" << how << "\" graphs=" << counts.graphs
                  << " nodes=" << counts.nodes << " outcome="
                  << (old_outcome.stage.empty() ? "accepted" : "refused at " + old_outcome.stage)
                  << '\n';
    };
    SECTION("frame planner plan fixtures") {
        for (const auto &[name, input] : planFixtureInputs()) {
            // The single-graph form is wrapped mechanically; the planner
            // (the core planning parse and planFrameGraph, which is all the
            // form ever met) plans the same before and after the wrap.
            if (!input.contains("rendering_passes")) {
                const auto unwrapped = parseFrameGraphDefinitionsFromConfigJson(input);
                const auto wrapped = parseFrameGraphDefinitionsFromConfigJson(wrapSingleGraph(input));
                REQUIRE(unwrapped.size() == 1);
                REQUIRE(wrapped.size() == 1);
                REQUIRE(firstDifference(definitionJson(unwrapped.front()),
                                        definitionJson(wrapped.front())) == "");
                REQUIRE(firstDifference(planJson(planFrameGraph(unwrapped.front())),
                                        planJson(planFrameGraph(wrapped.front()))) == "");
            }
            run(name, wrapSingleGraph(input), "as frameplanner_test builds it, wrapped when single-graph");
        }
        REQUIRE(accepted >= 8);
    }
    SECTION("frame planner error fixtures") {
        for (const auto *file : {"ambiguous_writes.json", "cycle_after.json", "unknown_resource.json"}) {
            const auto input = readJson(sourceRoot() / "test/fixtures/frameplanner/errors" / file);
            run(std::string{"errors/"} + file, wrapSingleGraph(input), "wrapped single-graph form");
        }
        REQUIRE(refused == 3);
    }
    SECTION("the static rendering configs") {
        for (const auto &[name, input] : staticCases()) {
            run(name, input, "the section as authored, normalized, format classes resolved");
        }
        REQUIRE(accepted > 0);
    }
    SECTION("graph transform and subgraph replacement configs") {
        for (const auto &[name, input] : providerInputs()) {
            run(name, input, "the builtin provider's response, or the config the provider reads");
        }
        REQUIRE(accepted == 4);
    }
}

TEST_CASE("RC-3c1a bridge: graph-local declarations stay the graph's own",
          "[rendergraphir][rc3c1a][bridge]") {
    const auto local = Json::parse(R"json({
      "render_targets": [{"name": "color", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
                          "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}],
      "compute_tasks": [{"name": "shared_task", "reads": ["x"], "writes": ["y"]}],
      "rendering_passes": [
        {"name": "a", "buffers": [{"name": "x"}, {"name": "unused_a"}],
         "compute_tasks": [{"name": "own_task", "writes": ["x"]}],
         "passes": [{"name": "draw", "type": "fullscreen",
                     "output": {"color": [{"target": "color"}], "depth": null}}]},
        {"name": "b",
         "passes": [{"name": "present", "type": "fullscreen", "input": ["x"],
                     "output": {"color": [{"target": "swapchain"}], "depth": null}}]}]})json");
    SECTION("a buffer only graph a declares is unknown to graph b") {
        auto config = local;
        config["buffers"] = Json::array({Json{{"name", "y"}}});
        const auto old_outcome = runOld(config, {});
        const auto new_outcome = runNew(config, {});
        REQUIRE(old_outcome.stage == "plan");
        REQUIRE(new_outcome.code == "render_graph.unknown_resource@1");
        REQUIRE(new_outcome.refusal.find("\"graph\":\"b\"") != std::string::npos);
        REQUIRE(new_outcome.refusal.find("\"resource\":\"x\"") != std::string::npos);
        requireSameOutcome("graph-local buffer read by another graph", old_outcome, new_outcome);
    }
    SECTION("the same declaration in the config is everyone's") {
        auto config = local;
        config["rendering_passes"][0].erase("buffers");
        config["buffers"] = Json::array({Json{{"name", "x"}}, Json{{"name", "y"}},
                                         Json{{"name", "unused_top"}}});
        const auto new_outcome = runNew(config, {});
        REQUIRE(new_outcome.stage.empty());
        requireSameOutcome("config buffer read by both graphs", runOld(config, {}), new_outcome);
        // The graph's own compute tasks come first, the config's after them.
        const auto &a = new_outcome.ir->graphs.at(0);
        REQUIRE(a.nodes.size() == 3);
        CHECK(a.nodes[1].name == "own_task");
        CHECK(a.nodes[1].scope == NodeDeclarationScope::graph);
        CHECK(a.nodes[2].name == "shared_task");
        CHECK(a.nodes[2].scope == NodeDeclarationScope::shared);
        // An unused declaration is still a resource of the logical graph.
        const auto &resources = new_outcome.logical.at(1).at("resources");
        CHECK(std::any_of(resources.begin(), resources.end(), [](const Json &resource) {
            return resource.at("name") == "unused_top";
        }));
    }
    SECTION("graph-local declarations are looked up before the config's") {
        auto config = local;
        config["buffers"] = Json::array({Json{{"name", "x"}}, Json{{"name", "y"}}});
        const auto new_outcome = runNew(config, {});
        REQUIRE(new_outcome.stage.empty());
        requireSameOutcome("graph-local and config buffers", runOld(config, {}), new_outcome);
        CHECK(new_outcome.definitions.at(0).declared_resources ==
              std::vector<std::string>{"x", "unused_a", "color", "y"});
    }
}

TEST_CASE("RC-3c1a bridge: a loading attachment is a same-pixel use in the IR",
          "[rendergraphir][rc3c1a][bridge]") {
    const auto config = Json::parse(R"json({
      "render_targets": [{"name": "color", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
                          "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}],
      "rendering_passes": [{"name": "main", "passes": [
        {"name": "base", "type": "fullscreen", "output": {"color": [{"target": "color"}], "depth": null}},
        {"name": "overlay", "type": "fullscreen", "color_load_op": "load",
         "output": {"color": [{"target": "color"}], "depth": null}},
        {"name": "present", "type": "fullscreen", "input": ["color"],
         "output": {"color": [{"target": "swapchain"}], "depth": null}}]}]})json");
    const auto old_graphs = parseFrameGraphDefinitionsFromConfigJson(config);
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
    CHECK(firstDifference(definitionJson(old_graphs.at(0)),
                          definitionJson(frameGraphDefinitionOf(ir, ir.graphs.at(0)))) == "");
    // Control: without the derived footprint the bridge names where it went.
    overlay.uses.front().footprint.reset();
    CHECK(firstDifference(definitionJson(old_graphs.at(0)),
                          definitionJson(frameGraphDefinitionOf(ir, ir.graphs.at(0)))) ==
          "/nodes/1/read_footprints/color (missing)");
}

TEST_CASE("RC-3c1a bridge: tile-local eligibility is decided by each path from its own graph",
          "[rendergraphir][rc3c1a][bridge][tile-local]") {
    // gbuffer_color is read same-pixel by a local read: eligible. scene_depth
    // is a tile-local candidate the device reads locally, but another pass
    // samples it (arbitrary): not eligible.
    const auto config = Json::parse(R"json({
      "render_targets": [
        {"name": "gbuffer_color", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "scene_depth", "extent_scale": 1.0, "format": "D32_SFLOAT",
         "usage": ["DEPTH_STENCIL_ATTACHMENT", "SAMPLED"]},
        {"name": "lit", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}],
      "rendering_passes": [{"name": "main", "passes": [
        {"name": "gbuffer", "type": "raster",
         "output": {"color": [{"target": "gbuffer_color"}], "depth": {"target": "scene_depth"}}},
        {"name": "resolve", "type": "fullscreen", "input": ["gbuffer_color", "scene_depth"],
         "input_footprints": {"gbuffer_color": "same_pixel"},
         "output": {"color": [{"target": "lit"}], "depth": null}},
        {"name": "present", "type": "fullscreen", "input": ["lit"],
         "output": {"color": [{"target": "swapchain"}], "depth": null}}]}]})json");
    const DeviceFacts device{.tile_local = true,
                             .local_read_formats = {ImageFormat::r8g8b8a8_unorm,
                                                    ImageFormat::d32_sfloat}};
    const auto new_outcome = runNew(config, device);
    REQUIRE(new_outcome.stage.empty());
    const auto &ir = *new_outcome.ir;
    const auto device_formats = renderGraphDeviceTileLocalFormats(
        ir, {.tile_local_attachments = true,
             .local_read_attachment_formats = device.local_read_formats});
    CHECK(device_formats.contains({"scene_depth", ImageFormat::d32_sfloat}));
    CHECK(device_formats.contains({"gbuffer_color", ImageFormat::r8g8b8a8_unorm}));
    const auto eligible = renderGraphTileLocalFormats(ir, ir.graphs.at(0), device_formats);
    CHECK(eligible == RenderGraphTileLocalFormats{{"gbuffer_color", ImageFormat::r8g8b8a8_unorm}});
    requireSameOutcome("tile-local depth read arbitrarily", runOld(config, device), new_outcome);
    const auto materialization = [&](std::string_view name) {
        for (const auto &resource : new_outcome.logical.at(0).at("resources")) {
            if (resource.at("name").get<std::string>() == name) {
                return resource.at("materialization").get<std::string>();
            }
        }
        return std::string{};
    };
    CHECK(materialization("gbuffer_color") == "virtual");
    CHECK(materialization("scene_depth") == "required");
    // Without the device facts neither path keeps gbuffer_color tile-local.
    requireSameOutcome("tile-local without device facts", runOld(config, {}), runNew(config, {}));
}

TEST_CASE("RC-3c1a bridge: multiview candidates follow the authored implementation",
          "[rendergraphir][rc3c1a][bridge][multiview]") {
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
            CHECK(renderGraphMultiviewCapableNodes(ir, enable) ==
                  detail::multiviewCapableFullscreenPasses(config, enable));
            CHECK(renderGraphMultiviewCapableNodes(ir, enable).contains("post") ==
                  (enable && !implementation));
        }
    }
}

TEST_CASE("RC-3c1a bridge: the acceleration-structure builds are inserted as written down",
          "[rendergraphir][rc3c1a][bridge][acceleration-structure]") {
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
    auto ir = parseRenderGraph(config, buildPassFieldOwnershipCapabilities());
    ir = insertAccelerationStructureNodes(
        std::move(ir), {.planning_graphs = {}, .ray_tracing_project = false, .named = finalize.named});
    // Independently written expectation: the scene's build, the named one
    // after it, at the head; the passes and the task wait on them.
    const auto &nodes = ir.graphs.at(0).nodes;
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
    const auto plan = planRenderGraph(ir, ir.graphs.at(0));
    CHECK(framePlanOrder(plan) == std::vector<std::string>{
                                      "__scene_acceleration_structure",
                                      "__scene_acceleration_structure:characters", "shade",
                                      "present", "trace"});
    // And the core path inserts the same.
    requireSameOutcome("ray query with a named acceleration structure",
                       runOld(config, {}, finalize), runNew(config, {}, finalize));
    // An unknown named structure is refused by name.
    auto unknown = config;
    unknown["rendering_passes"][0]["passes"][0]["ray_query"]["acceleration_structure"] = "props";
    unknown["compute_tasks"][0].erase("ray_query");
    const auto refused = runNew(unknown, {}, finalize);
    CHECK(refused.code == "render_graph.acceleration_structure_unknown@1");
    CHECK(refused.refusal.find("\"node\":\"shade\"") != std::string::npos);
    CHECK_FALSE(runOld(unknown, {}, finalize).refusal.empty());
}

TEST_CASE("RC-3c1a bridge: compute namespacing and frame_node_order agree with core",
          "[rendergraphir][rc3c1a][bridge]") {
    const auto config = Json::parse(R"json({
      "render_targets": [{"name": "color", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
                          "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}],
      "buffers": [{"name": "particles"}],
      "compute_tasks": [{"name": "simulate", "writes": ["particles"], "before": ["present"]},
                        {"name": "late", "after": ["simulate"]}],
      "frame_node_order": {"main": ["draw", "late"]},
      "rendering_passes": [{"name": "main", "passes": [
        {"name": "draw", "type": "fullscreen", "output": {"color": [{"target": "color"}], "depth": null}},
        {"name": "present", "type": "fullscreen", "input": ["color", "particles"],
         "output": {"color": [{"target": "swapchain"}], "depth": null}}]}]})json");
    const Finalize finalize{.suffix = "#xr", .apply_frame_node_order = false};
    const auto new_outcome = runNew(config, {}, finalize);
    REQUIRE(new_outcome.stage.empty());
    requireSameOutcome("compute tasks namespaced #xr", runOld(config, {}, finalize), new_outcome);
    const auto &nodes = new_outcome.ir->graphs.at(0).nodes;
    CHECK(nodes.at(2).name == "simulate#xr");
    CHECK(nodes.at(3).after == std::vector<std::string>{"simulate#xr"});
    CHECK(new_outcome.ir->shared_compute_tasks == std::vector<std::string>{"simulate#xr", "late#xr"});

    const Finalize ordered{.apply_frame_node_order = true};
    requireSameOutcome("frame_node_order applied", runOld(config, {}, ordered),
                       runNew(config, {}, ordered));
    const auto applied = runNew(config, {}, ordered);
    CHECK(applied.ir->graphs.at(0).nodes.at(3).after ==
          std::vector<std::string>{"simulate", "draw"});
    auto conflicting = config;
    conflicting["frame_node_order"]["main"] = Json::array({"present", "simulate"});
    const auto refused = runNew(conflicting, {}, ordered);
    CHECK(refused.code == "frame_node_order.conflicts_dependency@1");
    CHECK_FALSE(runOld(conflicting, {}, ordered).refusal.empty());
}

// ---------------------------------------------------------------------------
// F2: refusals, by group, each with its code and subject.

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

void requireNamedRefusal(const RefusalCase &refusal, const NewOutcome &outcome) {
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

TEST_CASE("RC-3c1a refusals: what the planning parse refused, parseRenderGraph refuses by name",
          "[rendergraphir][rc3c1a][refusal]") {
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
        {"a buffer named swapchain", [](Json &c) { c["buffers"][0]["name"] = "swapchain"; }, "render_graph.buffer_invalid@1", "resource", "swapchain"},
        {"a snapshot copy of a format outside the copy contract",
         [](Json &c) {
             c["render_targets"].push_back({{"name", "ids"}, {"extent_scale", 1.0}, {"width", 8}, {"height", 8},
                                            {"format", "R32_UINT"}, {"usage", {"COLOR_ATTACHMENT", "TRANSFER_DST"}}});
             pass(c, 2) = {{"name", "copy"}, {"type", "snapshot_copy"}, {"source", "color"},
                           {"destination", "ids"}, {"snapshot_after", "draw"}};
         },
         "render_graph.snapshot_size_unavailable@1", "resource", "ids"},
        {"frame_node_order is not an object", [](Json &c) { c["frame_node_order"] = 1; }, "render_graph.frame_node_order_invalid@1", "reason", "not_an_object"},
    };
    std::size_t checked = 0;
    for (const auto &refusal : cases) {
        DYNAMIC_SECTION(refusal.name) {
            auto config = refusalBase();
            requireSameOutcome("base", runOld(prepared(config), {}), runNew(prepared(config), {}));
            refusal.edit(config);
            const auto input = prepared(config);
            // frame_node_order is read where the compiler applies it.
            const auto finalize = input.contains("frame_node_order")
                                      ? std::optional{Finalize{.apply_frame_node_order = true}}
                                      : std::nullopt;
            const auto old_outcome = runOld(input, {}, finalize);
            const auto new_outcome = runNew(input, {}, finalize);
            INFO("old: " << old_outcome.refusal);
            CHECK(old_outcome.stage == "parse");
            CHECK(new_outcome.stage == "parse");
            requireNamedRefusal(refusal, new_outcome);
        }
        ++checked;
    }
    std::cout << "RC3C1A refusals group=a cases=" << checked << '\n';
}

TEST_CASE("RC-3c1a refusals: what the planner refused, planRenderGraph refuses by name",
          "[rendergraphir][rc3c1a][refusal]") {
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
    std::size_t checked = 0;
    for (const auto &refusal : cases) {
        DYNAMIC_SECTION(refusal.name) {
            auto config = refusalBase();
            refusal.edit(config);
            const auto input = prepared(config);
            const auto old_outcome = runOld(input, {});
            const auto new_outcome = runNew(input, {});
            INFO("old: " << old_outcome.refusal);
            CHECK(old_outcome.stage == "plan");
            CHECK(new_outcome.stage == "plan");
            requireNamedRefusal(refusal, new_outcome);
        }
        ++checked;
    }
    std::cout << "RC3C1A refusals group=b cases=" << checked << '\n';
}

namespace {

// The execution parsers that refused these before: the material pass's
// material_resources parser (with the registered targets' names) and the
// compute task parser.
std::string executionRefusal(const Json &config) {
    try {
        const auto definitions = parseRenderTargetDefinitionsFromJson(config);
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
        (void)parseComputeTaskDefinitionsFromConfigJson(config, nullptr);
    } catch (const std::exception &error) {
        return error.what();
    }
    return {};
}

} // namespace

TEST_CASE("RC-3c1a refusals: moved ahead of the execution parsers, refused by name",
          "[rendergraphir][rc3c1a][refusal]") {
    const auto task = [](Json extra) {
        Json result{{"name", "task"},
                    {"shader", "engine://shaders/compute/fixture"},
                    {"writes", {"shared"}},
                    {"dispatch", {{"groups", {1, 1, 1}}}}};
        for (auto it = extra.begin(); it != extra.end(); ++it) result[it.key()] = it.value();
        return result;
    };
    const std::vector<RefusalCase> cases{
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
         [&](Json &c) { c["compute_tasks"] = Json::array({task({{"schedule", "sometimes"}})}); },
         "render_graph.compute_task_invalid@1", "field", "schedule"},
        {"a compute ray_query without its acceleration structure",
         [&](Json &c) { c["compute_tasks"] = Json::array({task({{"ray_query", Json::object()}})}); },
         "render_graph.compute_task_invalid@1", "field", "ray_query"},
        {"a compute ray_query with an empty acceleration structure",
         [&](Json &c) { c["compute_tasks"] = Json::array({task({{"ray_query", {{"acceleration_structure", ""}}}})}); },
         "render_graph.compute_task_invalid@1", "reason", "empty_acceleration_structure"},
        {"ray_task_acceleration_structures naming no task",
         [&](Json &c) {
             c["compute_tasks"] = Json::array({task({})});
             c["ray_task_acceleration_structures"] = {{"ghost", "scene"}};
         },
         "render_graph.compute_task_invalid@1", "node", "ghost"},
        {"ray_task_acceleration_structures naming a task that traces nothing",
         [&](Json &c) {
             c["compute_tasks"] = Json::array({task({})});
             c["ray_task_acceleration_structures"] = {{"task", "scene"}};
         },
         "render_graph.compute_task_invalid@1", "reason", "task_declares_no_ray_work"},
    };
    std::size_t checked = 0;
    for (const auto &refusal : cases) {
        DYNAMIC_SECTION(refusal.name) {
            auto config = refusalBase();
            config["compute_tasks"] = Json::array({task({})});
            REQUIRE(executionRefusal(prepared(config)).empty());
            refusal.edit(config);
            const auto input = prepared(config);
            // The planning path accepted it; an execution parser refused it.
            const auto old_outcome = runOld(input, {});
            INFO("old: " << old_outcome.refusal);
            CHECK(old_outcome.stage.empty());
            CHECK_FALSE(executionRefusal(input).empty());
            requireNamedRefusal(refusal, runNew(input, {}));
        }
        ++checked;
    }
    std::cout << "RC3C1A refusals group=c cases=" << checked << '\n';
}

} // namespace Pelican
