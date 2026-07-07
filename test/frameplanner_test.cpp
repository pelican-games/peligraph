#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/project/featurecompose.hpp"
#include "../src/core/renderingpass/renderingpassconfigjsonparser.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/rendertargetmetadataresolver.hpp"
#include "../src/core/renderingpass/rendertargetnameresolver.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        const auto definitions = parseRenderTargetDefinitionsFromJson(config);
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
                const auto graph = makeFrameGraphDefinition(definition);
                const auto plan = planFrameGraph(graph);
                REQUIRE(framePlanOrder(plan) == passNames(definition));
            }
        }
    }
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
    REQUIRE(plan_json == readJson(fixtureRoot() / "plans" / "example_main_render.json"));
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

    REQUIRE(base_plan_json == readJson(fixtureRoot() / "plans" / "explicit_edges_base.json"));
    REQUIRE(edged_plan_json == readJson(fixtureRoot() / "plans" / "explicit_edges_after_before.json"));
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
    REQUIRE(plan_json == readJson(fixtureRoot() / "plans" / "debug_draw_feature_main.json"));
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
          "shader": {"vertex": "engine://fullscreen", "fragment": "engine://fullscreen"}
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
    REQUIRE(plan_json == readJson(fixtureRoot() / "plans" / "shadow_directional_feature_main.json"));
}

} // namespace Pelican
