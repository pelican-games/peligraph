#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/logicalframegraphadapter.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/project/featurecompose.hpp"
#include "../src/core/renderingpass/renderingpassconfigjsonparser.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/rendertargetmetadataresolver.hpp"
#include "../src/core/renderingpass/rendertargetnameresolver.hpp"
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace Pelican {

namespace {

struct ConfigCase {
    std::string name;
    nlohmann::json config;
};

std::filesystem::path sourceRoot() { return std::filesystem::path{PELICAN_TEST_SOURCE_DIR}; }
std::filesystem::path fixtureRoot() { return sourceRoot() / "test" / "fixtures" / "frameplanner"; }

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open json fixture: " + path.string());
    }
    return nlohmann::json::parse(file);
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open text fixture: " + path.string());
    }
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

void requirePlanFixture(const nlohmann::json &actual, const std::filesystem::path &path) {
    const char *update = std::getenv("PELICAN_UPDATE_FRAMEPLAN_FIXTURES");
    if (update != nullptr && std::string{update} == "1") {
        std::ofstream file{path, std::ios_base::binary};
        file << actual.dump(2) << '\n';
        return;
    }
    REQUIRE(actual == readJson(path));
}

std::vector<std::string> passNames(const RenderingPassDefinition &definition) {
    std::vector<std::string> names;
    names.reserve(definition.passes.size());
    for (const auto &pass : definition.passes) {
        names.push_back(pass.name);
    }
    return names;
}

std::vector<std::string> graphNodeNames(const FrameGraphDefinition &definition) {
    std::vector<std::string> names;
    names.reserve(definition.nodes.size());
    for (const auto &node : definition.nodes) {
        names.push_back(node.name);
    }
    return names;
}

void normalizeDisplayAlias(nlohmann::json &value) {
    if (value.is_string()) {
        if (value.get<std::string>() == "display") {
            value = "swapchain";
        }
        return;
    }
    if (value.is_array() || value.is_object()) {
        for (auto &entry : value) {
            normalizeDisplayAlias(entry);
        }
    }
}

nlohmann::json legacyPassProjection(const nlohmann::json &config) {
    auto projected = nlohmann::json::array();
    for (const auto &pass : config.at("rendering_passes").at(0).at("passes")) {
        const auto type = pass.value("type", std::string{});
        if (type == "canonical_anchor" || type == "output_transform") {
            continue;
        }
        auto legacy = pass;
        legacy.erase("after");
        legacy.erase("before");
        legacy.erase("canonical_anchor");
        normalizeDisplayAlias(legacy);
        projected.push_back(std::move(legacy));
    }
    return projected;
}

nlohmann::json legacyTargetProjection(const nlohmann::json &config) {
    auto projected = nlohmann::json::array();
    for (const auto &target : config.at("render_targets")) {
        if (target.value("name", std::string{}) == "display") {
            continue;
        }
        auto legacy = target;
        legacy.erase("format_class");
        legacy.erase("role");
        projected.push_back(std::move(legacy));
    }
    return projected;
}

nlohmann::json stemGoldenRenderingConfig() {
    return nlohmann::json::parse(R"json({
  "render_targets": [],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "solid",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "shaders/solid",
            "fragment": "shaders/solid"
          }
        }
      ]
    }
  ]
})json");
}

std::vector<ConfigCase> staticRenderingConfigCases() {
    std::vector<ConfigCase> cases;
    const std::vector<std::filesystem::path> roots{
        sourceRoot() / "projects" / "example",
        sourceRoot() / "test" / "fixtures",
    };
    for (const auto &root : roots) {
        if (!std::filesystem::exists(root)) {
            continue;
        }
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }
            const auto json = readJson(entry.path());
            if (json.is_object() && json.contains("rendering_passes")) {
                cases.push_back(ConfigCase{
                    std::filesystem::relative(entry.path(), sourceRoot()).generic_string(),
                    json,
                });
            }
        }
    }
    return cases;
}

std::vector<ConfigCase> goldenRenderingConfigCases() {
    std::vector<ConfigCase> cases;
    const auto golden_root = sourceRoot() / "test" / "golden";
    for (const auto &entry : std::filesystem::directory_iterator(golden_root)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto case_json = entry.path() / "case.json";
        if (!std::filesystem::exists(case_json)) {
            continue;
        }
        const auto mode = readJson(case_json).at("mode").get<std::string>();
        if (mode == "stem_fullscreen") {
            cases.push_back(ConfigCase{"test/golden/" + entry.path().filename().string(), stemGoldenRenderingConfig()});
        } else if (mode == "vat_playback") {
            cases.push_back(ConfigCase{
                "test/golden/" + entry.path().filename().string(),
                readJson(sourceRoot() / "projects" / "example" / "passes" / "main_rendering_config.json"),
            });
        }
    }
    return cases;
}

std::vector<ConfigCase> allShadowConfigCases() {
    auto cases = staticRenderingConfigCases();
    auto golden_cases = goldenRenderingConfigCases();
    cases.insert(cases.end(), golden_cases.begin(), golden_cases.end());
    return cases;
}

struct ParsedRenderTargetResolvers {
    std::unordered_map<std::string, GlobalRenderTargetId> ids_by_name;
    std::vector<RenderTargetMetadata> metadata;

    explicit ParsedRenderTargetResolvers(const nlohmann::json &config) {
        auto base = config;
        base.erase("features"); // base-definition helper; feature composition has dedicated gates
        const auto composed = composeRenderFeatureConfig(base);
        const auto resolved = resolveRenderTargetFormatClassesV2(
            composed.config, vk::Format::eB8G8R8A8Srgb, vk::Extent2D{100, 100}, false);
        const auto definitions = parseRenderTargetDefinitionsFromJson(resolved);
        metadata.reserve(definitions.size());
        for (size_t i = 0; i < definitions.size(); ++i) {
            const auto id = GlobalRenderTargetId{static_cast<int>(i)};
            ids_by_name.emplace(definitions[i].name, id);
            const auto scaled_extent = static_cast<uint32_t>(definitions[i].extent_scale * 100.0f);
            const auto extent = definitions[i].fixed_extent.value_or(vk::Extent2D{scaled_extent, scaled_extent});
            metadata.push_back(RenderTargetMetadata{
                definitions[i].name,
                definitions[i].usage,
                definitions[i].format,
                extent,
                definitions[i].history,
            });
        }
    }

    RenderTargetNameResolver nameResolver() const {
        return RenderTargetNameResolver{[this](const std::string &name) {
            const auto found = ids_by_name.find(name);
            if (found == ids_by_name.end()) {
                return noRenderTargetId();
            }
            return found->second;
        }};
    }

    RenderTargetMetadataResolver metadataResolver() const {
        return RenderTargetMetadataResolver{[this](GlobalRenderTargetId id) {
            if (!isConcreteRenderTarget(id) || static_cast<size_t>(id.value) >= metadata.size()) {
                throw std::runtime_error("test render target metadata not found");
            }
            return metadata[static_cast<size_t>(id.value)];
        }};
    }
};

nlohmann::json wp352FiveColorConfig() {
    return nlohmann::json::parse(R"json({
      "render_targets": [
        {"name":"gbuffer_albedo","extent_scale":1.0,"format":"B8G8R8A8_UNORM","usage":["COLOR_ATTACHMENT","SAMPLED"]},
        {"name":"gbuffer_normal","extent_scale":1.0,"format":"R16G16B16A16_SFLOAT","usage":["COLOR_ATTACHMENT","SAMPLED"]},
        {"name":"gbuffer_material","extent_scale":1.0,"format":"R8G8B8A8_UNORM","usage":["COLOR_ATTACHMENT","SAMPLED"]},
        {"name":"gbuffer_worldpos","extent_scale":1.0,"format":"R16G16B16A16_SFLOAT","usage":["COLOR_ATTACHMENT","SAMPLED"]},
        {"name":"g_emissive","extent_scale":1.0,"format":"R8G8B8A8_UNORM","usage":["COLOR_ATTACHMENT","SAMPLED"]},
        {"name":"offscreen_depth","extent_scale":1.0,"format":"D32_SFLOAT","usage":["DEPTH_STENCIL_ATTACHMENT"]}
      ],
      "rendering_passes": [{
        "name": "main",
        "passes": [{
          "name": "gbuffer_pass",
          "type": "material",
          "output": {
            "color": [
              "gbuffer_albedo",
              "gbuffer_normal",
              "gbuffer_material",
              "gbuffer_worldpos",
              "g_emissive"
            ],
            "depth": "offscreen_depth"
          }
        }]
      }]
    })json");
}

std::vector<FrameGraphNodeDefinition> wp352NodesFromBothPlannerInputs(
    const nlohmann::json &config) {
    std::vector<FrameGraphNodeDefinition> result;

    const auto raw_graphs =
        parseFrameGraphDefinitionsFromConfigJson(config);
    if (raw_graphs.size() != 1 ||
        raw_graphs.front().nodes.size() != 1) {
        throw std::runtime_error(
            "WP352 raw planner fixture must contain one node");
    }
    result.push_back(raw_graphs.front().nodes.front());

    const ParsedRenderTargetResolvers resolvers{config};
    const auto definitions =
        parseRenderingPassDefinitionsFromConfigJson(
            config, resolvers.nameResolver(),
            resolvers.metadataResolver());
    if (definitions.size() != 1) {
        throw std::runtime_error(
            "WP352 typed planner fixture must contain one pass set");
    }
    const auto typed_graph =
        makeFrameGraphDefinition(definitions.front());
    if (typed_graph.nodes.size() != 1) {
        throw std::runtime_error(
            "WP352 typed planner fixture must contain one node");
    }
    result.push_back(typed_graph.nodes.front());
    return result;
}

std::string loadEngineJsonForWp352(std::string_view reference) {
    constexpr std::string_view prefix = "engine://";
    if (!reference.starts_with(prefix)) {
        throw std::runtime_error(
            "WP352 shipping config expected engine reference: " +
            std::string{reference});
    }
    return engineResourceOrThrow(
        reference.substr(prefix.size()));
}

void materializeAttachmentOperations(
    nlohmann::json &encoded,
    const std::string &load_op,
    const std::string &store_op) {
    if (encoded.is_null()) return;
    if (encoded.is_array()) {
        for (auto &entry : encoded) {
            materializeAttachmentOperations(
                entry, load_op, store_op);
        }
        return;
    }
    if (encoded.is_string()) {
        const auto target = encoded.get<std::string>();
        encoded = {
            {"target", target},
            {"load_op", load_op},
            {"store_op", store_op},
        };
        return;
    }
    if (!encoded.is_object()) {
        throw std::runtime_error(
            "WP352 shipping output is not an attachment");
    }
    if (!encoded.contains("load_op")) {
        encoded["load_op"] = load_op;
    }
    if (!encoded.contains("store_op")) {
        encoded["store_op"] = store_op;
    }
}

nlohmann::json materializeShippingAttachmentOperations(
    nlohmann::json config) {
    for (auto &pass_set : config.at("rendering_passes")) {
        for (auto &pass : pass_set.at("passes")) {
            if (!pass.contains("output")) continue;
            auto &output = pass.at("output");
            const auto type =
                pass.value("type", std::string{});
            const auto color_load =
                pass.value(
                    "color_load_op",
                    type == "ui" || type == "imgui"
                        ? std::string{"Load"}
                        : std::string{"Clear"});
            const auto color_store =
                pass.value(
                    "color_store_op",
                    std::string{"Store"});
            const auto depth_load =
                pass.value(
                    "depth_load_op",
                    std::string{"Clear"});
            const auto depth_store =
                pass.value(
                    "depth_store_op",
                    std::string{"DontCare"});
            materializeAttachmentOperations(
                output.at("color"),
                color_load, color_store);
            materializeAttachmentOperations(
                output.at("depth"),
                depth_load, depth_store);
        }
    }
    return config;
}

} // namespace

TEST_CASE("frame planner preserves existing rendering config order", "[frameplanner]") {
    const auto cases = allShadowConfigCases();
    REQUIRE_FALSE(cases.empty());

    for (const auto &config_case : cases) {
        DYNAMIC_SECTION(config_case.name) {
            const auto raw_graphs = parseFrameGraphDefinitionsFromConfigJson(config_case.config);
            REQUIRE_FALSE(raw_graphs.empty());
            for (const auto &graph : raw_graphs) {
                const auto plan = planFrameGraph(graph);
                REQUIRE(framePlanOrder(plan) == graphNodeNames(graph));
            }

            const ParsedRenderTargetResolvers resolvers{config_case.config};
            const auto parsed_definitions = parseRenderingPassDefinitionsFromConfigJson(
                config_case.config, resolvers.nameResolver(), resolvers.metadataResolver());
            REQUIRE(parsed_definitions.size() == raw_graphs.size());
            for (const auto &definition : parsed_definitions) {
                auto graph = makeFrameGraphDefinition(definition);
                // The legacy PassDefinition adapter has no snapshot-copy
                // node. Its screen-input targets therefore enter this shadow
                // graph as declared resources instead of snapshot writes.
                for (std::size_t index = 0;
                     index < resolvers.metadata.size(); ++index) {
                    graph.declared_resources.push_back(
                        "rt:" + std::to_string(index));
                }
                const auto plan = planFrameGraph(graph);
                REQUIRE(framePlanOrder(plan) == passNames(definition));
            }
        }
    }
}

TEST_CASE("logical shadow compilation preserves every existing frame plan",
          "[frameplanner][logical-render-graph][compatibility]") {
    const auto registry = makeBuiltinLogicalTypeRegistry();
    const auto cases = allShadowConfigCases();
    REQUIRE_FALSE(cases.empty());

    for (const auto &config_case : cases) {
        DYNAMIC_SECTION(config_case.name) {
            const auto graphs =
                parseFrameGraphDefinitionsFromConfigJson(config_case.config);
            REQUIRE_FALSE(graphs.empty());
            for (const auto &graph : graphs) {
                const auto before = framePlanToJson(planFrameGraph(graph));
                const auto first = compileLogicalFrameGraphShadow(graph, registry);
                const auto second = compileLogicalFrameGraphShadow(graph, registry);
                const auto after = framePlanToJson(planFrameGraph(graph));

                REQUIRE(before == after);
                REQUIRE(compiledLogicalRenderGraphToJson(first) ==
                        compiledLogicalRenderGraphToJson(second));
            }
        }
    }
}

TEST_CASE("hybrid preset resolves ordered deferred and forward writes",
          "[frameplanner][hybrid][pipeline-preset]") {
    const auto authored = nlohmann::json{
        {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
    };
    const auto load_engine = [](std::string_view ref) {
        constexpr std::string_view prefix = "engine://";
        if (ref.rfind(prefix, 0) != 0) throw std::runtime_error("expected engine ref");
        return engineResourceOrThrow(ref.substr(prefix.size()));
    };
    const auto composed = composeRenderFeatureConfig(
        authored, RenderFeatureComposeDependencies{{}, false, {}, load_engine});
    const auto resolved = resolveRenderTargetFormatClassesV2(
        composed.config, vk::Format::eB8G8R8A8Srgb, vk::Extent2D{1280, 720}, false);

    const auto target_definitions = parseRenderTargetDefinitionsFromJson(resolved);
    std::unordered_map<std::string, GlobalRenderTargetId> ids;
    std::vector<RenderTargetMetadata> metadata;
    for (std::size_t index = 0; index < target_definitions.size(); ++index) {
        const auto &target = target_definitions[index];
        const auto id = GlobalRenderTargetId{static_cast<int>(index)};
        ids.emplace(target.name, id);
        metadata.push_back(RenderTargetMetadata{
            target.name, target.usage, target.format, vk::Extent2D{1280, 720},
            target.history});
    }
    const RenderTargetNameResolver names{[&](const std::string &name) {
        const auto found = ids.find(name);
        return found == ids.end() ? noRenderTargetId() : found->second;
    }};
    const RenderTargetMetadataResolver target_metadata{
        [&](GlobalRenderTargetId id) { return metadata.at(static_cast<std::size_t>(id.value)); }};
    const auto passes = parseRenderingPassDefinitionsFromConfigJson(
        resolved, names, target_metadata);
    REQUIRE(passes.size() == 1);
    REQUIRE(passes.front().passes.at(0).materialInfo().contract ==
            MaterialPassContract::deferred_geometry_v1);

    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(resolved);
    REQUIRE(graphs.size() == 1);
    const auto plan = planFrameGraph(graphs.front());
    const auto order = framePlanOrder(plan);
    const auto position = [&](std::string_view name) {
        const auto found = std::find(order.begin(), order.end(), name);
        REQUIRE(found != order.end());
        return std::distance(order.begin(), found);
    };
    REQUIRE(position("deferred_geometry") < position("deferred_lighting"));
    REQUIRE(position("deferred_lighting") < position("forward_opaque"));
    REQUIRE(position("forward_opaque") < position("__snapshot_opaque_color"));
    REQUIRE(position("forward_opaque") < position("__snapshot_opaque_depth"));
    REQUIRE(position("__snapshot_opaque_color") <
            position("forward_transparent"));
    REQUIRE(position("__snapshot_opaque_depth") <
            position("forward_transparent"));
    REQUIRE(position("forward_transparent") < position("scene_present"));
    REQUIRE(position("scene_present") < position("output_transform"));
    const auto transparent = std::find_if(
        plan.nodes.begin(), plan.nodes.end(), [](const auto &node) {
            return node.name == "forward_transparent";
        });
    REQUIRE(transparent != plan.nodes.end());
    REQUIRE(std::find(transparent->reads.begin(), transparent->reads.end(),
                      "opaque_color") != transparent->reads.end());
    REQUIRE(std::find(transparent->reads.begin(), transparent->reads.end(),
                      "opaque_depth") != transparent->reads.end());
}

TEST_CASE("frame planner accepts compute tasks and serializes node kinds", "[frameplanner]") {
    const auto graph = parseFrameGraphDefinitionFromJson(nlohmann::json::parse(R"json({
  "name": "mixed",
  "buffers": ["particles", {"name": "visible_particles"}],
  "passes": [
    {
      "name": "present",
      "type": "fullscreen",
      "output": {"color": "swapchain", "depth": null}
    }
  ],
  "compute_tasks": [
    {"name": "simulate", "reads": ["particles"], "writes": ["particles"]},
    {"name": "cull", "reads": ["particles"], "writes": ["visible_particles"]}
  ]
})json"));

    const auto plan = planFrameGraph(graph);
    const auto plan_json = framePlanToJson(plan);

    REQUIRE(plan_json.at("schema") == "pelican.frame_plan");
    REQUIRE(plan_json.at("version") == 1);
    REQUIRE(plan_json.at("nodes").at(0).at("kind") == "render");
    REQUIRE(plan_json.at("nodes").at(1).at("kind") == "compute");
    REQUIRE(plan_json.at("nodes").at(2).at("kind") == "compute");
}

TEST_CASE("history reads are serialized without an intra-frame dependency",
          "[frameplanner][temporal]") {
    const auto graph = parseFrameGraphDefinitionFromJson(nlohmann::json::parse(R"json({
      "name":"history_accumulation",
      "render_targets":[
        {"name":"current_color"},
        {"name":"temporal_accum","history":true}
      ],
      "passes":[
        {"name":"produce_current","type":"fullscreen",
         "output":{"color":"current_color","depth":null}},
        {"name":"accumulate","type":"fullscreen",
         "input":["current_color","temporal_accum@history"],
         "output":{"color":"temporal_accum","depth":null}}
      ]
    })json"));
    const auto plan = planFrameGraph(graph);
    REQUIRE(framePlanOrder(plan) == std::vector<std::string>{"produce_current", "accumulate"});
    REQUIRE(plan.nodes.back().reads == std::vector<std::string>{"current_color"});
    REQUIRE(plan.nodes.back().reads_history == std::vector<std::string>{"temporal_accum"});
    REQUIRE(plan.barriers.size() == 1);
    REQUIRE(plan.barriers.front().resource == "current_color");
    REQUIRE(std::none_of(plan.barriers.begin(), plan.barriers.end(), [](const auto &barrier) {
        return barrier.resource == "temporal_accum";
    }));
    requirePlanFixture(framePlanToJson(plan), fixtureRoot() / "plans" / "history_read.json");
}

TEST_CASE("frame graph parses typed current-read footprints",
          "[frameplanner][logical]") {
    const auto graph =
        parseFrameGraphDefinitionFromJson(
            nlohmann::json::parse(R"json({
              "name": "typed_reads",
              "render_targets": [
                {"name": "gbuffer"},
                {"name": "ao"}
              ],
              "passes": [
                {
                  "name": "lighting",
                  "type": "fullscreen",
                  "input": ["gbuffer", "ao"],
                  "input_footprints": {
                    "gbuffer": "same_pixel",
                    "ao": {
                      "kind": "neighborhood",
                      "radius": 2
                    }
                  },
                  "output": {
                    "color": "swapchain",
                    "depth": null
                  }
                }
              ]
            })json"));

    REQUIRE(graph.nodes.size() == 1);
    const auto &footprints =
        graph.nodes.front().read_footprints;
    REQUIRE(footprints.size() == 2);
    const auto gbuffer = std::find_if(
        footprints.begin(), footprints.end(),
        [](const auto &entry) {
            return entry.resource == "gbuffer";
        });
    const auto ao = std::find_if(
        footprints.begin(), footprints.end(),
        [](const auto &entry) {
            return entry.resource == "ao";
        });
    REQUIRE(gbuffer != footprints.end());
    REQUIRE(
        gbuffer->footprint ==
        LogicalReadFootprint{
            LogicalReadFootprintKind::same_pixel,
            std::nullopt});
    REQUIRE(ao != footprints.end());
    REQUIRE(
        ao->footprint ==
        LogicalReadFootprint{
            LogicalReadFootprintKind::neighborhood,
            2});

    auto invalid = nlohmann::json::parse(R"json({
      "name": "invalid_typed_read",
      "render_targets": [{"name": "source"}],
      "passes": [{
        "name": "present",
        "type": "fullscreen",
        "input": ["source"],
        "input_footprints": {
          "not_an_input": "same_pixel"
        },
        "output": {"color": "swapchain", "depth": null}
      }]
    })json");
    REQUIRE_THROWS_AS(
        parseFrameGraphDefinitionFromJson(invalid),
        std::runtime_error);

    invalid["passes"][0]["input_footprints"] = {
        {"source", "temporal"}};
    REQUIRE_THROWS_AS(
        parseFrameGraphDefinitionFromJson(invalid),
        std::runtime_error);
}

TEST_CASE("frame planner emits barriers for explicit compute to render resource edges", "[frameplanner]") {
    const auto graph = parseFrameGraphDefinitionFromJson(nlohmann::json::parse(R"json({
  "name": "compute_to_present",
  "buffers": [{"name": "compute_color"}],
  "passes": [
    {
      "name": "present",
      "type": "fullscreen",
      "input": ["compute_color"],
      "output": {"color": "swapchain", "depth": null}
    }
  ],
  "compute_tasks": [
    {
      "name": "write_color",
      "writes": ["compute_color"],
      "before": ["present"]
    }
  ]
})json"));

    const auto plan = planFrameGraph(graph);
    REQUIRE(framePlanOrder(plan) == std::vector<std::string>{"write_color", "present"});
    REQUIRE(plan.barriers.size() == 1);
    REQUIRE(plan.barriers.front().resource == "compute_color");
    REQUIRE(plan.barriers.front().from == "write_color");
    REQUIRE(plan.barriers.front().to == "present");
}

TEST_CASE("frame planner emits a barrier for ordered write-after-write",
          "[frameplanner][synchronization]") {
    const auto graph =
        parseFrameGraphDefinitionFromJson(
            nlohmann::json::parse(R"json({
              "name": "ordered_waw",
              "buffers": [{"name": "shared"}],
              "compute_tasks": [
                {
                  "name": "write_a",
                  "writes": ["shared"],
                  "before": ["write_b"]
                },
                {
                  "name": "write_b",
                  "writes": ["shared"]
                }
              ]
            })json"));

    const auto plan = planFrameGraph(graph);
    REQUIRE(
        framePlanOrder(plan) ==
        std::vector<std::string>{"write_a", "write_b"});
    REQUIRE(plan.barriers.size() == 1);
    REQUIRE(plan.barriers.front().kind == "write_after_write");
    REQUIRE(plan.barriers.front().resource == "shared");
    REQUIRE(plan.barriers.front().from == "write_a");
    REQUIRE(plan.barriers.front().to == "write_b");
}

TEST_CASE(
    "material resource ports create typed compute to geometry reads",
    "[frameplanner][material-resource][wp207b]") {
    const auto graph =
        parseFrameGraphDefinitionFromJson(
            nlohmann::json::parse(R"json({
              "name": "compute_vertex_displacement",
              "buffers": [
                {"name": "deformed_positions"}
              ],
              "passes": [{
                "name": "geometry",
                "type": "material",
                "material_resources": {
                  "displacement": {
                    "resource": "deformed_positions",
                    "access": "storage",
                    "footprint": "arbitrary"
                  }
                },
                "output": {
                  "color": "scene_color",
                  "depth": "scene_depth"
                }
              }],
              "compute_tasks": [{
                "name": "deform",
                "writes": ["deformed_positions"],
                "before": ["geometry"]
              }]
            })json"));

    const auto geometry = std::find_if(
        graph.nodes.begin(), graph.nodes.end(),
        [](const auto &node) {
            return node.name == "geometry";
        });
    REQUIRE(geometry != graph.nodes.end());
    REQUIRE(
        geometry->reads ==
        std::vector<std::string>{
            "deformed_positions"});
    REQUIRE(
        geometry->read_footprints ==
        std::vector<FrameGraphReadFootprintDefinition>{
            {
                "deformed_positions",
                {
                    LogicalReadFootprintKind::
                        arbitrary,
                    std::nullopt,
                },
            }});
    REQUIRE(
        geometry->resource_accesses ==
        std::vector<FrameGraphResourceAccessDefinition>{
            {
                "deformed_positions",
                LogicalAccessIntent::storage,
            }});

    const auto plan = planFrameGraph(graph);
    REQUIRE(
        framePlanOrder(plan) ==
        std::vector<std::string>{
            "deform", "geometry"});
    REQUIRE(
        std::any_of(
            plan.barriers.begin(), plan.barriers.end(),
            [](const auto &barrier) {
                return barrier.resource ==
                           "deformed_positions" &&
                       barrier.from == "deform" &&
                       barrier.to == "geometry";
            }));

    const auto logical =
        compileLogicalFrameGraphShadow(
            graph, makeBuiltinLogicalTypeRegistry());
    const auto logical_geometry = std::find_if(
        logical.nodes.begin(), logical.nodes.end(),
        [](const auto &node) {
            return node.name == "geometry";
        });
    REQUIRE(
        logical_geometry != logical.nodes.end());
    const auto displacement = std::find_if(
        logical_geometry->uses.begin(),
        logical_geometry->uses.end(),
        [](const auto &use) {
            return use.input_value &&
                   use.input_value->resource ==
                       "deformed_positions";
        });
    REQUIRE(
        displacement !=
        logical_geometry->uses.end());
    REQUIRE(
        displacement->intent ==
        LogicalAccessIntent::storage);
}

TEST_CASE(
    "shader resource ports lower sampled and storage intent without "
    "creating graph edges",
    "[frameplanner][logical][resource-port][wp207a]") {
    const auto graph =
        parseFrameGraphDefinitionFromJson(
            nlohmann::json::parse(R"json({
              "name": "typed_resource_access",
              "render_targets": [
                {"name": "scene_color"},
                {"name": "filtered_color"}
              ],
              "compute_tasks": [{
                "name": "filter",
                "reads": ["scene_color"],
                "writes": ["filtered_color"],
                "resource_ports": {
                  "source": {
                    "resource": "scene_color",
                    "access": "sampled"
                  },
                  "destination": {
                    "resource": "filtered_color",
                    "access": "storage"
                  }
                }
              }]
            })json"));

    REQUIRE(graph.nodes.size() == 1);
    REQUIRE(
        graph.nodes.front().reads ==
        std::vector<std::string>{"scene_color"});
    REQUIRE(
        graph.nodes.front().writes ==
        std::vector<std::string>{"filtered_color"});
    REQUIRE((
        graph.nodes.front().resource_accesses ==
        std::vector<FrameGraphResourceAccessDefinition>{
            {"filtered_color",
             LogicalAccessIntent::storage},
            {"scene_color",
             LogicalAccessIntent::sampled}}));

    const auto logical =
        compileLogicalFrameGraphShadow(
            graph, makeBuiltinLogicalTypeRegistry());
    REQUIRE(logical.nodes.size() == 1);
    const auto &uses = logical.nodes.front().uses;
    const auto sampled = std::find_if(
        uses.begin(), uses.end(),
        [](const LogicalResourceUse &use) {
            return use.input_value &&
                   use.input_value->resource ==
                       "scene_color";
        });
    const auto storage = std::find_if(
        uses.begin(), uses.end(),
        [](const LogicalResourceUse &use) {
            return use.output_value &&
                   use.output_value->resource ==
                       "filtered_color";
        });
    REQUIRE(sampled != uses.end());
    REQUIRE(
        sampled->intent ==
        LogicalAccessIntent::sampled);
    REQUIRE(storage != uses.end());
    REQUIRE(
        storage->intent ==
        LogicalAccessIntent::storage);
}

TEST_CASE(
    "generic raster nodes participate in one logical planner with typed inputs",
    "[frameplanner][raster][resource-port][wp238b]") {
    const auto graph =
        parseFrameGraphDefinitionFromJson(
            nlohmann::json::parse(R"json({
              "name": "custom_raster",
              "render_targets": [
                {"name": "source"},
                {"name": "gbuffer_a"},
                {"name": "gbuffer_b"}
              ],
              "passes": [{
                "name": "custom_geometry",
                "type": "raster",
                "resolution_domain": "scene",
                "input": ["source"],
                "resource_ports": {
                  "source_image": {
                    "resource": "source",
                    "access": "sampled"
                  }
                },
                "output": {
                  "color": ["gbuffer_a", "gbuffer_b"],
                  "depth": null
                }
              }]
            })json"));

    REQUIRE(graph.nodes.size() == 1);
    const auto &node = graph.nodes.front();
    CHECK(
        node.kind ==
        FramePlanNodeKind::render);
    CHECK(node.raster_geometry);
    CHECK(
        node.resolution_domain ==
        RenderResolutionDomain::scene);
    CHECK(
        node.reads ==
        std::vector<std::string>{"source"});
    CHECK(
        node.writes ==
        std::vector<std::string>{
            "gbuffer_a", "gbuffer_b"});
    CHECK(
        node.local_read_shader_inputs ==
        std::vector<std::string>{"source"});
    REQUIRE(
        node.resource_accesses.size() == 1);
    CHECK(
        node.resource_accesses.front().intent ==
        LogicalAccessIntent::sampled);

    const auto plan = planFrameGraph(graph);
    REQUIRE(plan.nodes.size() == 1);
    CHECK(
        plan.nodes.front().name ==
        "custom_geometry");
}

TEST_CASE("frame planner reports invalid graph fixtures", "[frameplanner]") {
    const auto expectations = readJson(fixtureRoot() / "errors" / "expectations.json");
    for (const auto &entry : expectations) {
        DYNAMIC_SECTION(entry.at("file").get<std::string>()) {
            std::string message;
            try {
                const auto graph = parseFrameGraphDefinitionFromJson(
                    readJson(fixtureRoot() / "errors" / entry.at("file").get<std::string>()));
                (void)planFrameGraph(graph);
            } catch (const std::exception &ex) {
                message = ex.what();
            }

            REQUIRE_FALSE(message.empty());
            REQUIRE(contains(message, entry.at("contains").get<std::string>()));
        }
    }
}

TEST_CASE("frame planner example plan JSON matches fixture", "[frameplanner]") {
    const auto config = readJson(sourceRoot() / "projects" / "example" / "passes" / "main_rendering_config.json");
    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(config);
    REQUIRE(graphs.size() == 1);

    const auto plan_json = framePlanToJson(planFrameGraph(graphs.front()));
    requirePlanFixture(plan_json, fixtureRoot() / "plans" / "example_main_render.json");
}

TEST_CASE("snapshot plan node reports copy bytes and screen input read dependency",
          "[frameplanner][snapshot]") {
    const auto config = nlohmann::json::parse(R"json({
  "snapshots": [{"name": "opaque_color", "after": "opaque"}],
  "render_targets": [],
  "rendering_passes": [{
    "name": "snapshot_demo",
    "passes": [
      {
        "name": "opaque",
        "type": "fullscreen",
        "output": {"color": "swapchain", "depth": null}
      },
      {
        "name": "refract",
        "type": "fullscreen",
        "canonical_anchor": "post_ldr",
        "input": ["opaque_color"],
        "output": {"color": "swapchain", "depth": null}
      }
    ]
  }]
})json");
    const auto composed = composeRenderFeatureConfig(config);
    const auto resolved = resolveRenderTargetFormatClassesV2(
        composed.config, vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 32}, false);
    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(resolved);
    REQUIRE(graphs.size() == 1);
    const auto plan = planFrameGraph(graphs.front());
    const auto copy = std::find_if(plan.nodes.begin(), plan.nodes.end(), [](const auto &node) {
        return node.kind == FramePlanNodeKind::snapshot_copy;
    });
    REQUIRE(copy != plan.nodes.end());
    REQUIRE(copy->byte_size == 64 * 32 * 4);
    const auto refract = std::find_if(plan.nodes.begin(), plan.nodes.end(), [](const auto &node) {
        return node.name == "refract";
    });
    REQUIRE(refract != plan.nodes.end());
    REQUIRE(refract->reads == std::vector<std::string>{"opaque_color"});
    REQUIRE(std::any_of(plan.barriers.begin(), plan.barriers.end(), [](const auto &barrier) {
        return barrier.from == "__snapshot_opaque_color" && barrier.to == "refract" &&
               barrier.resource == "opaque_color";
    }));
    const auto plan_json = framePlanToJson(plan);
    REQUIRE(plan_json.at("version") == 1);
    REQUIRE_FALSE(plan_json.contains("materials"));
    REQUIRE_FALSE(plan_json.contains("resources"));
    REQUIRE_FALSE(plan_json.contains("snapshots"));
    REQUIRE_FALSE(plan_json.contains("anchors"));
    requirePlanFixture(plan_json, fixtureRoot() / "plans" / "snapshot_screen_input.json");
}

TEST_CASE("canonical C1b frame-plan diff preserves every legacy pass and target field",
          "[frameplanner][color-c1b]") {
    auto legacy = readJson(sourceRoot() / "projects" / "example" / "passes" /
                           "main_rendering_config.json");
    // This C1b fixture isolates the color rewrite; feature insertion has its own gates.
    legacy.erase("features");
    const auto old_graphs = parseFrameGraphDefinitionsFromConfigJson(legacy);
    REQUIRE(old_graphs.size() == 1);
    const auto old_plan = planFrameGraph(old_graphs.front());

    const auto composed = composeRenderFeatureConfig(legacy);
    REQUIRE(composed.config.at("resolver_version").get<int>() == 2);
    const auto new_graphs = parseFrameGraphDefinitionsFromConfigJson(composed.config);
    REQUIRE(new_graphs.size() == 1);
    const auto new_plan = planFrameGraph(new_graphs.front());

    std::vector<std::string> new_legacy_order;
    for (const auto &node : new_plan.nodes) {
        if (node.kind == FramePlanNodeKind::render || node.kind == FramePlanNodeKind::compute) {
            new_legacy_order.push_back(node.name);
        }
    }
    std::vector<std::string> old_legacy_order;
    for (const auto &node : old_plan.nodes) {
        if (node.kind == FramePlanNodeKind::render ||
            node.kind == FramePlanNodeKind::compute) {
            old_legacy_order.push_back(node.name);
        }
    }
    REQUIRE(new_legacy_order == old_legacy_order);
    REQUIRE(legacyPassProjection(composed.config) == legacyPassProjection(legacy));
    REQUIRE(legacyTargetProjection(composed.config) == legacyTargetProjection(legacy));

    const auto &display = composed.config.at("render_targets").back();
    REQUIRE(display.at("name").get<std::string>() == "display");
    REQUIRE(display.at("format").get<std::string>() == "B8G8R8A8_SRGB");
    REQUIRE(display.at("usage").get<std::vector<std::string>>() ==
            std::vector<std::string>{"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"});
    REQUIRE(new_plan.nodes.back().kind == FramePlanNodeKind::output_transform);
    REQUIRE(new_plan.nodes.back().name == "output_transform");
}

TEST_CASE("frame planner explicit after and before edges change levels", "[frameplanner]") {
    const auto base_graph = parseFrameGraphDefinitionFromJson(nlohmann::json::parse(R"json({
  "name": "explicit_edges_base",
  "render_targets": [
    {"name": "rt_a"},
    {"name": "rt_b"},
    {"name": "rt_c"}
  ],
  "passes": [
    {"name": "clear_a", "type": "fullscreen", "output": {"color": "rt_a", "depth": null}},
    {"name": "clear_b", "type": "fullscreen", "output": {"color": "rt_b", "depth": null}},
    {"name": "clear_c", "type": "fullscreen", "output": {"color": "rt_c", "depth": null}},
    {
      "name": "present",
      "type": "fullscreen",
      "input": ["rt_a", "rt_b", "rt_c"],
      "output": {"color": "swapchain", "depth": null}
    }
  ]
})json"));
    const auto edged_graph = parseFrameGraphDefinitionFromJson(nlohmann::json::parse(R"json({
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

    const auto base_plan_json = framePlanToJson(planFrameGraph(base_graph));
    const auto edged_plan_json = framePlanToJson(planFrameGraph(edged_graph));

    requirePlanFixture(base_plan_json, fixtureRoot() / "plans" / "explicit_edges_base.json");
    requirePlanFixture(edged_plan_json, fixtureRoot() / "plans" / "explicit_edges_after_before.json");
    REQUIRE(base_plan_json.at("levels") != edged_plan_json.at("levels"));
}

TEST_CASE("frame planner feature-composed config plan matches fixture", "[frameplanner]") {
    const auto config = nlohmann::json::parse(R"json({
  "features": ["engine://features/debug_draw.json"],
  "render_targets": [],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "present",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null}
        }
      ]
    }
  ]
})json");

    const auto composed = composeRenderFeatureConfig(
        config,
        RenderFeatureComposeDependencies{
            [](std::string_view ref) {
                if (ref != "engine://features/debug_draw.json") {
                    throw std::runtime_error("unexpected feature ref: " + std::string{ref});
                }
                return readText(sourceRoot() / "src" / "core" / "resources" / "features" /
                                "debug_draw.json");
            },
            true,
        });
    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(composed.config);
    REQUIRE(graphs.size() == 1);

    const auto plan_json = framePlanToJson(planFrameGraph(graphs.front()));
    requirePlanFixture(plan_json, fixtureRoot() / "plans" / "debug_draw_feature_main.json");
}

TEST_CASE("frame planner debug text feature plan matches fixture", "[frameplanner]") {
    const auto config = nlohmann::json::parse(R"json({
  "features": ["engine://features/debug_text.json"],
  "render_targets": [],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "present",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null}
        }
      ]
    }
  ]
})json");

    const auto composed = composeRenderFeatureConfig(
        config,
        RenderFeatureComposeDependencies{
            [](std::string_view ref) {
                if (ref != "engine://features/debug_text.json") {
                    throw std::runtime_error("unexpected feature ref: " + std::string{ref});
                }
                return readText(sourceRoot() / "src" / "core" / "resources" / "features" /
                                "debug_text.json");
            },
            true,
        });
    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(composed.config);
    REQUIRE(graphs.size() == 1);

    const auto plan_json = framePlanToJson(planFrameGraph(graphs.front()));
    requirePlanFixture(plan_json, fixtureRoot() / "plans" / "debug_text_feature_main.json");
}

TEST_CASE("UI feature occupies the canonical pelican_ui anchor and has no base-pass fallback",
          "[frameplanner][ui][u1]") {
    const auto config = nlohmann::json::parse(R"json({
      "features":["engine://features/ui.json"],"render_targets":[],
      "rendering_passes":[{"name":"main","passes":[{
        "name":"present","type":"fullscreen","output":{"color":"swapchain","depth":null}
      }]}]
    })json");
    const auto composed = composeRenderFeatureConfig(
        config,
        RenderFeatureComposeDependencies{
            .load_feature_json = [](std::string_view ref) {
                if (ref != "engine://features/ui.json")
                    throw std::runtime_error("unexpected feature ref: " + std::string{ref});
                std::ifstream file{sourceRoot() / "src/core/resources/features/ui.json"};
                return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
            },
            .runtime_shader_compiler_enabled = true,
        });
    REQUIRE(composed.feature_names == std::vector<std::string>{"ui"});
    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(composed.config);
    const auto order = framePlanOrder(planFrameGraph(graphs.front()));
    const auto anchor = std::find(order.begin(), order.end(), "__anchor_pelican_ui");
    const auto pass = std::find(order.begin(), order.end(), "pelican_ui");
    const auto next_anchor = std::find(order.begin(), order.end(), "__anchor_debug_draw");
    REQUIRE(anchor != order.end());
    REQUIRE(pass == anchor + 1);
    REQUIRE(next_anchor == pass + 1);
}

TEST_CASE("frame planner shadow feature plan matches fixture", "[frameplanner]") {
    const auto config = nlohmann::json::parse(R"json({
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

    const auto composed = composeRenderFeatureConfig(
        config,
        RenderFeatureComposeDependencies{
            [](std::string_view ref) {
                if (ref != "engine://features/shadow_directional.json") {
                    throw std::runtime_error("unexpected feature ref: " + std::string{ref});
                }
                return readText(sourceRoot() / "src" / "core" / "resources" / "features" /
                                "shadow_directional.json");
            },
            true,
        });
    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(composed.config);
    REQUIRE(graphs.size() == 1);

    const auto plan_json = framePlanToJson(planFrameGraph(graphs.front()));
    REQUIRE(
        graphs.front().nodes.front()
            .view_family ==
        "$shadow/directional");
    const auto logical =
        compileLogicalFrameGraphShadow(
            graphs.front(),
            makeBuiltinLogicalTypeRegistry());
    REQUIRE(
        logical.nodes.front().view_family ==
        "$shadow/directional");
    requirePlanFixture(plan_json, fixtureRoot() / "plans" / "shadow_directional_feature_main.json");
}

TEST_CASE(
    "frame graph labels default and overridden resolution domains",
    "[frameplanner][upscale][resolution-domain]") {
    const auto graph =
        parseFrameGraphDefinitionFromJson(
            nlohmann::json::parse(R"json({
      "name":"resolution_domains",
      "passes":[
        {"name":"geometry","type":"material",
         "output":{"color":"scene","depth":"depth"}},
        {"name":"shadow","type":"shadow_depth",
         "output":{"color":null,"depth":"shadow_depth"}},
        {"name":"custom_scene","type":"fullscreen",
         "resolution_domain":"scene",
         "output":{"color":"custom","depth":null}},
        {"name":"present","type":"output_transform",
         "output":{"color":"swapchain","depth":null}}
      ]
    })json"));

    REQUIRE(
        graph.nodes.at(0).resolution_domain ==
        RenderResolutionDomain::scene);
    REQUIRE(
        graph.nodes.at(1).resolution_domain ==
        RenderResolutionDomain::independent);
    REQUIRE(
        graph.nodes.at(2).resolution_domain ==
        RenderResolutionDomain::scene);
    REQUIRE(
        graph.nodes.at(3).resolution_domain ==
        RenderResolutionDomain::output);
}

TEST_CASE(
    "frame plan preserves authored material tag filters with compile provenance",
    "[frameplanner][draw-tag][wp206a]") {
    const auto graph =
        parseFrameGraphDefinitionFromJson(
            nlohmann::json::parse(R"json({
      "name":"tagged_material",
      "passes":[
        {"name":"outline","type":"material",
         "material_filter":{
           "include":["outline","character"],
           "exclude":["hidden"]
         },
         "output":{"color":"scene","depth":"depth"}}
      ]
    })json"));
    REQUIRE(
        graph.nodes.front()
            .material_filter.has_value());

    auto plan_json =
        framePlanToJson(planFrameGraph(graph));
    const auto &filter =
        plan_json.at("nodes")
            .at(0)
            .at("material_filter");
    REQUIRE(
        filter.at("include") ==
        nlohmann::json{
            "character", "outline"});
    REQUIRE(
        filter.at("exclude") ==
        nlohmann::json{"hidden"});
    REQUIRE(
        filter.at("filter_id")
            .get<std::string>()
            .starts_with("fnv1a64:"));
    REQUIRE(
        filter.at("resolved_draw_count")
            .is_null());
    REQUIRE(
        filter.at("resolution_provenance") ==
        std::string{
            materialDrawTagFilterProvenance});
    REQUIRE(
        filter.at("resolution_state") ==
        "pending_draw_queue_compile");

    applyMaterialDrawFilterResolutionToFramePlanJson(
        plan_json, "outline",
        graph.nodes.front()
            .material_filter->id,
        7, {"missing_include"},
        {"missing_exclude"});
    const auto &resolved =
        plan_json.at("nodes")
            .at(0)
            .at("material_filter");
    REQUIRE(
        resolved.at("resolved_draw_count") ==
        7);
    REQUIRE(
        resolved.at("unmatched_include") ==
        nlohmann::json{"missing_include"});
    REQUIRE(
        resolved.at("unmatched_exclude") ==
        nlohmann::json{"missing_exclude"});
    REQUIRE(
        resolved.at("resolution_state") ==
        "resolved");

    auto invalid =
        nlohmann::json::parse(R"json({
      "name":"bad_filter",
      "passes":[
        {"name":"present","type":"fullscreen",
         "material_filter":{"include":["outline"]},
         "output":{"color":"scene","depth":null}}
      ]
    })json");
    REQUIRE_THROWS_WITH(
        parseFrameGraphDefinitionFromJson(invalid),
        Catch::Matchers::ContainsSubstring(
            "Pass 'present' type 'fullscreen' does not own field "
            "'material_filter'"));
}

TEST_CASE(
    "WP311 frame graph rejects GPU draw ownership before render and pseudo branches",
    "[frameplanner][wp311][pass-field-ownership]") {
    const auto gpu_draw_source = nlohmann::json{
        {"commands", "visible_draws"},
        {"count", "visible_draw_count"},
        {"max_draw_count", 2},
    };
    const auto require_rejected = [&](nlohmann::json pass) {
        const auto name = pass.at("name").get<std::string>();
        pass["gpu_draw_source"] = gpu_draw_source;
        const nlohmann::json graph{
            {"name", "ownership_negative"},
            {"passes", nlohmann::json::array({std::move(pass)})},
        };
        REQUIRE_THROWS_WITH(
            parseFrameGraphDefinitionFromJson(graph),
            Catch::Matchers::ContainsSubstring(
                "Pass '" + name + "'") &&
                Catch::Matchers::ContainsSubstring(
                    "does not own field 'gpu_draw_source'"));
    };

    require_rejected({
        {"name", "fullscreen_gpu_draw"},
        {"type", "fullscreen"},
        {"output", { {"color", "scene"}, {"depth", nullptr} }},
    });
    require_rejected({
        {"name", "anchor_gpu_draw"},
        {"type", "canonical_anchor"},
        {"anchor", "post_main"},
    });
    require_rejected({
        {"name", "snapshot_gpu_draw"},
        {"type", "snapshot_copy"},
        {"source", "scene"},
        {"destination", "snapshot"},
        {"snapshot", "snapshot"},
        {"snapshot_after", "scene"},
    });
}

TEST_CASE(
    "frame plan preserves a pass-local material variant beside its filter identity",
    "[frameplanner][material-variant][wp206b]") {
    auto authored = nlohmann::json::parse(R"json({
      "name":"material_variant",
      "passes":[
        {"name":"silhouette_overlay","type":"material",
         "material_contract":"forward_opaque_v1",
         "material_filter":{"include":["outlined"]},
         "material_variant":"silhouette",
         "output":{"color":"scene","depth":"depth"}}
      ]
    })json");
    const auto graph =
        parseFrameGraphDefinitionFromJson(authored);
    REQUIRE(graph.nodes.front().material_variant ==
            "silhouette");
    const auto plan_json =
        framePlanToJson(planFrameGraph(graph));
    REQUIRE(plan_json.at("nodes").at(0)
                .at("material_variant") ==
            "silhouette");

    authored["passes"][0].erase(
        "material_contract");
    REQUIRE_THROWS_WITH(
        parseFrameGraphDefinitionFromJson(authored),
        Catch::Matchers::ContainsSubstring(
            "requires explicit material_contract"));
}

TEST_CASE(
    "frame graph preserves per-aspect attachment operations",
    "[frameplanner][attachment-operations]") {
    auto authored = nlohmann::json::parse(R"json({
      "name":"attachment_operations",
      "passes":[
        {"name":"explicit","type":"material",
         "color_load_op":"dont_care",
         "color_store_op":"dont_care",
         "depth_load_op":"load",
         "depth_store_op":"store",
         "output":{"color":["first","second"],"depth":"depth"}},
        {"name":"ui","type":"ui",
         "output":{"color":"first","depth":null}}
      ]
    })json");
#if PELICAN_WITH_IMGUI
    authored["passes"].push_back({
        {"name", "imgui"},
        {"type", "imgui"},
        {"output", {{"color", "second"}, {"depth", nullptr}}},
    });
#endif
    const auto graph =
        parseFrameGraphDefinitionFromJson(authored);

    REQUIRE(graph.nodes.at(0).attachments.size() == 3);
    REQUIRE((
        graph.nodes.at(0).attachments.at(0) ==
        FrameGraphAttachmentDefinition{
            .resource = "first",
            .aspect =
                FrameGraphAttachmentAspect::color,
            .load_op =
                FrameGraphAttachmentLoadOp::discard,
            .store_op =
                FrameGraphAttachmentStoreOp::discard,
        }));
    REQUIRE((
        graph.nodes.at(0).attachments.at(2) ==
        FrameGraphAttachmentDefinition{
            .resource = "depth",
            .aspect =
                FrameGraphAttachmentAspect::depth,
            .load_op =
                FrameGraphAttachmentLoadOp::load,
            .store_op =
                FrameGraphAttachmentStoreOp::store,
        }));
    REQUIRE(
        std::find(
            graph.nodes.at(0).reads.begin(),
            graph.nodes.at(0).reads.end(),
            "depth") !=
        graph.nodes.at(0).reads.end());
    REQUIRE(
        graph.nodes.at(1).attachments.front().load_op ==
        FrameGraphAttachmentLoadOp::load);
#if PELICAN_WITH_IMGUI
    REQUIRE(
        graph.nodes.at(2).attachments.front().load_op ==
        FrameGraphAttachmentLoadOp::load);
#endif

    auto malformed = nlohmann::json::parse(R"json({
      "name":"bad_attachment_op",
      "passes":[
        {"name":"bad","type":"fullscreen",
         "color_load_op":"preserve_somehow",
         "output":{"color":"display","depth":null}}
      ]
    })json");
    REQUIRE_THROWS(
        parseFrameGraphDefinitionFromJson(
            malformed));
}

TEST_CASE(
    "WP352 resolves attachment operations per output in both planner inputs",
    "[frameplanner][attachment-operations][wp352]") {
    const auto require_color_state = [](
        const FrameGraphNodeDefinition &node,
        const std::vector<std::size_t> &loaded_indices) {
        REQUIRE(node.attachments.size() == 6);
        REQUIRE(node.reads.size() == loaded_indices.size());
        REQUIRE(
            node.read_footprints.size() ==
            loaded_indices.size());
        for (std::size_t index = 0; index < 5; ++index) {
            const auto &attachment =
                node.attachments.at(index);
            REQUIRE(
                attachment.aspect ==
                FrameGraphAttachmentAspect::color);
            const bool loaded =
                std::find(
                    loaded_indices.begin(),
                    loaded_indices.end(), index) !=
                loaded_indices.end();
            CHECK(
                attachment.load_op ==
                (loaded
                     ? FrameGraphAttachmentLoadOp::load
                     : FrameGraphAttachmentLoadOp::clear));
            CHECK(
                attachment.store_op ==
                (loaded
                     ? FrameGraphAttachmentStoreOp::discard
                     : FrameGraphAttachmentStoreOp::store));
            CHECK(
                (std::find(
                     node.reads.begin(), node.reads.end(),
                     attachment.resource) != node.reads.end()) ==
                loaded);
            const auto footprint = std::find_if(
                node.read_footprints.begin(),
                node.read_footprints.end(),
                [&](const auto &candidate) {
                    return candidate.resource ==
                           attachment.resource;
                });
            CHECK(
                (footprint != node.read_footprints.end()) ==
                loaded);
            if (loaded) {
                CHECK(
                    footprint->footprint.kind ==
                    LogicalReadFootprintKind::same_pixel);
            }
        }
    };

    // No authored operation: the historical color defaults are
    // Clear/Store, so no color output is inferred as a read.
    const auto defaults = wp352FiveColorConfig();
    const auto default_nodes =
        wp352NodesFromBothPlannerInputs(defaults);
    REQUIRE(default_nodes.size() == 2);
    for (const auto &node : default_nodes) {
        require_color_state(node, {});
    }

    // Pass-wide Load is the compatibility control: all five color outputs
    // become same-pixel reads, preserving the pre-WP352 behavior.
    auto pass_wide = defaults;
    auto &pass_wide_pass =
        pass_wide["rendering_passes"][0]["passes"][0];
    pass_wide_pass["color_load_op"] = "Load";
    pass_wide_pass["color_store_op"] = "DontCare";
    const auto pass_wide_nodes =
        wp352NodesFromBothPlannerInputs(pass_wide);
    REQUIRE(pass_wide_nodes.size() == 2);
    for (const auto &node : pass_wide_nodes) {
        require_color_state(node, {0, 1, 2, 3, 4});
    }

    // The attachment contradicts the pass-wide Clear/Store values. Only its
    // target becomes a read and receives same_pixel; the other four do not.
    auto per_attachment = defaults;
    auto &attachment_pass =
        per_attachment["rendering_passes"][0]["passes"][0];
    attachment_pass["color_load_op"] = "Clear";
    attachment_pass["color_store_op"] = "Store";
    attachment_pass["output"]["color"][0] = {
        {"target", "gbuffer_albedo"},
        {"load_op", "Load"},
        {"store_op", "DontCare"},
    };
    const auto attachment_nodes =
        wp352NodesFromBothPlannerInputs(per_attachment);
    REQUIRE(attachment_nodes.size() == 2);
    for (const auto &node : attachment_nodes) {
        require_color_state(node, {0});
    }

    const auto require_depth_state = [](
        const FrameGraphNodeDefinition &node,
        bool loaded) {
        REQUIRE(node.attachments.size() == 6);
        const auto &depth = node.attachments.back();
        REQUIRE(
            depth.aspect ==
            FrameGraphAttachmentAspect::depth);
        CHECK(
            depth.load_op ==
            (loaded
                 ? FrameGraphAttachmentLoadOp::load
                 : FrameGraphAttachmentLoadOp::clear));
        CHECK(
            depth.store_op ==
            (loaded
                 ? FrameGraphAttachmentStoreOp::store
                 : FrameGraphAttachmentStoreOp::discard));
        CHECK(node.reads.size() == (loaded ? 1 : 0));
        CHECK(
            (std::find(
                 node.reads.begin(), node.reads.end(),
                 depth.resource) != node.reads.end()) ==
            loaded);
        CHECK(
            node.read_footprints.size() ==
            (loaded ? 1 : 0));
        if (loaded) {
            CHECK(
                node.read_footprints.front().resource ==
                depth.resource);
            CHECK(
                node.read_footprints.front().footprint.kind ==
                LogicalReadFootprintKind::same_pixel);
        }
    };

    // Depth exercises the same default, pass-wide, and conflicting
    // attachment precedence without relying on the color assertions above.
    for (const auto &node : default_nodes) {
        require_depth_state(node, false);
    }
    auto depth_pass_wide = defaults;
    auto &depth_pass =
        depth_pass_wide["rendering_passes"][0]["passes"][0];
    depth_pass["depth_load_op"] = "Load";
    depth_pass["depth_store_op"] = "Store";
    for (const auto &node :
         wp352NodesFromBothPlannerInputs(depth_pass_wide)) {
        require_depth_state(node, true);
    }
    auto depth_attachment = defaults;
    auto &depth_attachment_pass =
        depth_attachment["rendering_passes"][0]["passes"][0];
    depth_attachment_pass["depth_load_op"] = "Clear";
    depth_attachment_pass["depth_store_op"] = "DontCare";
    depth_attachment_pass["output"]["depth"] = {
        {"target", "offscreen_depth"},
        {"load_op", "Load"},
        {"store_op", "Store"},
    };
    for (const auto &node :
         wp352NodesFromBothPlannerInputs(depth_attachment)) {
        require_depth_state(node, true);
    }

    auto unknown_attachment_field = defaults;
    unknown_attachment_field["rendering_passes"][0]
                            ["passes"][0]
                            ["output"]["color"][0] = {
        {"target", "gbuffer_albedo"},
        {"clear", nlohmann::json::array({0, 0, 0, 1})},
    };
    REQUIRE_THROWS_WITH(
        parseFrameGraphDefinitionsFromConfigJson(
            unknown_attachment_field),
        Catch::Matchers::ContainsSubstring(
            "unknown field 'clear'"));
}

TEST_CASE(
    "WP352 keeps all four shipping project frame plans unchanged",
    "[frameplanner][attachment-operations][wp352][shipping]") {
    const std::array shipping_configs{
        "projects/animgraph_demo/passes/main.json",
        "projects/example/passes/main_rendering_config.json",
        "projects/sprite_demo/passes/main.json",
        "projects/vrm_xr_demo/passes/main.json",
    };
    const auto dependencies =
        RenderFeatureComposeDependencies{
            .load_feature_json = loadEngineJsonForWp352,
            .runtime_shader_compiler_enabled = true,
            .load_pipeline_json = loadEngineJsonForWp352,
        };

    std::size_t compared_projects = 0;
    std::size_t compared_plans = 0;
    for (const auto *relative_path : shipping_configs) {
        INFO(relative_path);
        const auto composed = composeRenderFeatureConfig(
            readJson(sourceRoot() / relative_path),
            dependencies);
        const auto resolved =
            resolveRenderTargetFormatClassesV2(
                composed.config,
                vk::Format::eB8G8R8A8Srgb,
                vk::Extent2D{160, 90}, false);
        const auto before_graphs =
            parseFrameGraphDefinitionsFromConfigJson(
                resolved);
        const auto after_graphs =
            parseFrameGraphDefinitionsFromConfigJson(
                materializeShippingAttachmentOperations(
                    resolved));
        REQUIRE_FALSE(before_graphs.empty());
        REQUIRE(before_graphs.size() == after_graphs.size());
        for (std::size_t graph_index = 0;
             graph_index < before_graphs.size();
             ++graph_index) {
            const auto before = framePlanToJson(
                planFrameGraph(before_graphs[graph_index]));
            const auto after = framePlanToJson(
                planFrameGraph(after_graphs[graph_index]));

            // nodes includes each node's reads/writes; barriers and levels
            // cover the data/control dependency topology. The final equality
            // intentionally checks every other serialized plan field too.
            REQUIRE(before.at("nodes") == after.at("nodes"));
            REQUIRE(before.at("barriers") == after.at("barriers"));
            REQUIRE(before.at("levels") == after.at("levels"));
            REQUIRE(before == after);
            ++compared_plans;
        }
        ++compared_projects;
    }
    REQUIRE(compared_projects == 4);
    REQUIRE(compared_plans == 4);
}

TEST_CASE(
    "frame graph preserves raster attachment subresources for physical lowering",
    "[frameplanner][attachment-subresource][wp235]") {
    const auto graph =
        parseFrameGraphDefinitionFromJson(
            nlohmann::json::parse(R"json({
      "name":"attachment_views",
      "passes":[
        {"name":"probe","type":"shadow_depth",
         "output":{
           "color":null,
           "depth":{
             "target":"probe_depth",
             "subresource":{
               "mip":1,
               "layer":6,
               "layer_count":6
             }
           }
         }}
      ]
    })json"));
    REQUIRE(graph.nodes.size() == 1);
    REQUIRE(
        graph.nodes.front().writes ==
        std::vector<std::string>{"probe_depth"});
    REQUIRE(
        graph.nodes.front().attachments.size() ==
        1);
    const auto &attachment =
        graph.nodes.front().attachments.front();
    REQUIRE(
        attachment.aspect ==
        FrameGraphAttachmentAspect::depth);
    REQUIRE(attachment.subresource.has_value());
    REQUIRE(
        attachment.subresource->base_mip_level ==
        1);
    REQUIRE(
        attachment.subresource->base_array_layer ==
        6);
    REQUIRE(
        attachment.subresource->layer_count == 6);

    auto malformed = nlohmann::json::parse(R"json({
      "name":"bad_attachment_views",
      "passes":[
        {"name":"probe","type":"shadow_depth",
         "output":{
           "color":null,
           "depth":{
             "target":"probe_depth",
             "subresource":{"mip_count":2}
           }
         }}
      ]
    })json");
    REQUIRE_THROWS_WITH(
        parseFrameGraphDefinitionFromJson(
            malformed),
        Catch::Matchers::ContainsSubstring(
            "must select exactly one mip"));
}

TEST_CASE("WP181 frame graph runtime retains one immutable typed pipeline",
          "[wp181][frameplanner][render-pipeline]") {
    FrameGraphDefinition definition;
    definition.name = "typed_runtime";
    CompiledRenderingPass rendering_pass;
    rendering_pass.name = definition.name;
    CompiledRenderPipeline compiled_pipeline;
    compiled_pipeline.feature_names = {"typed_fixture"};
    compiled_pipeline.material_routing = CompiledMaterialRouting{};
    constexpr std::array routes{
        MaterialRouteClass::deferred_geometry,
        MaterialRouteClass::forward_opaque,
        MaterialRouteClass::forward_transparent,
    };
    constexpr std::array contracts{
        MaterialPassContract::deferred_geometry_v1,
        MaterialPassContract::forward_opaque_v1,
        MaterialPassContract::forward_transparent_v1,
    };
    constexpr std::array<std::string_view, 3> pass_names{
        "deferred_geometry", "forward_opaque", "forward_transparent"};
    for (std::size_t index = 0; index < routes.size(); ++index) {
        FrameGraphNodeDefinition node;
        node.name = pass_names[index];
        node.kind = FramePlanNodeKind::render;
        definition.nodes.push_back(std::move(node));

        PassDefinition pass_definition;
        pass_definition.name = pass_names[index];
        pass_definition.materialInfo().contract = contracts[index];
        rendering_pass.passes.push_back(CompiledPass{
            std::move(pass_definition),
            PassId{static_cast<int>(17 + index)},
        });
        compiled_pipeline.material_routing->routes.push_back(
            CompiledMaterialRoute{routes[index], std::string{pass_names[index]},
                                  contracts[index]});
    }
    auto pipeline = std::make_shared<const CompiledRenderPipeline>(
        std::move(compiled_pipeline));

    FrameGraphRuntimeContainer runtime;
    const auto rendering_pass_id = RenderingPassId{23};
    runtime.registerExecutionPlan(rendering_pass_id, rendering_pass,
                                  planFrameGraph(definition), pipeline);

    const auto execution = runtime.find(rendering_pass_id);
    REQUIRE(execution != nullptr);
    REQUIRE(execution->render_pipeline == pipeline);
    REQUIRE(execution->render_pipeline->feature_names ==
            std::vector<std::string>{"typed_fixture"});
    REQUIRE(execution->material_routes.size() == 3);
    REQUIRE(execution->material_routes.at(0).pass_id == PassId{17});
    REQUIRE(execution->material_routes.at(2).route ==
            MaterialRouteClass::forward_transparent);
    REQUIRE(execution->material_routes.at(2).pass_id == PassId{19});
    REQUIRE(execution->material_routes.at(2).pass_contract ==
            MaterialPassContract::forward_transparent_v1);
    REQUIRE(execution->nodes.size() == 3);
    REQUIRE(execution->nodes.front().index == 0);
}

} // namespace Pelican
