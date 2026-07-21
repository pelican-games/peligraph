#include "../src/project/featurecompose.hpp"
#include "../src/project/renderpipeline.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {

namespace {

std::filesystem::path fixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "render_features";
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open fixture: " + path.string());
    }
    return nlohmann::json::parse(file);
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open fixture: " + path.string());
    }
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

nlohmann::json baseConfigWithFeature(std::string feature_ref) {
    auto config = nlohmann::json::parse(R"json({
  "features": [],
  "render_targets": [
    {
      "name": "lit_color",
      "extent_scale": 1.0,
      "format": "B8G8R8A8_UNORM",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    }
  ],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "prepare",
          "type": "fullscreen",
          "output": {"color": "lit_color", "depth": null},
          "shader": {"vertex": "shaders/fullscreen", "fragment": "shaders/base"}
        },
        {
          "name": "present",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null},
          "input": ["lit_color"],
          "shader": {"vertex": "shaders/fullscreen", "fragment": "shaders/present"}
        }
      ]
    }
  ]
})json")
    ;
    config["features"] = nlohmann::json::array({std::move(feature_ref)});
    return config;
}

std::string loadFixtureFeature(std::string_view ref) {
    return readText(fixtureRoot() / std::string{ref});
}

std::string loadEngineFeature(std::string_view ref) {
    constexpr std::string_view engine_prefix = "engine://";
    if (ref.rfind(engine_prefix, 0) != 0) {
        throw std::runtime_error("expected engine feature ref");
    }
    return engineResourceOrThrow(ref.substr(engine_prefix.size()));
}

std::vector<std::string> passNames(const nlohmann::json &config) {
    std::vector<std::string> names;
    for (const auto &pass : config.at("rendering_passes").at(0).at("passes")) {
        const auto type = pass.value("type", std::string{});
        if (type == "canonical_anchor" || type == "output_transform") {
            continue;
        }
        names.push_back(pass.at("name").get<std::string>());
    }
    return names;
}

const nlohmann::json &passByName(const nlohmann::json &config, std::string_view name) {
    for (const auto &pass : config.at("rendering_passes").at(0).at("passes")) {
        if (pass.value("name", std::string{}) == name) {
            return pass;
        }
    }
    throw std::runtime_error("pass not found: " + std::string{name});
}

void requireErrorKind(std::string_view message, std::string_view error_kind) {
    if (error_kind == "render_target_collision") {
        REQUIRE(contains(message, "render target name collides"));
    } else if (error_kind == "pass_collision") {
        REQUIRE(contains(message, "pass name collides"));
    } else if (error_kind == "missing_anchor") {
        REQUIRE(contains(message, "anchor was not found"));
    } else if (error_kind == "bad_override") {
        REQUIRE(contains(message, "only supports format"));
    } else {
        FAIL("unknown render feature error_kind: " << error_kind);
    }
}

} // namespace

TEST_CASE("canonical color pipeline is composed even without features", "[render-feature]") {
    const auto config = nlohmann::json::parse(R"json({
  "shader_defines": ["PELICAN_BASE_DEFINE"],
  "render_targets": [],
  "rendering_passes": []
})json");
    bool loader_called = false;
    const auto result = composeRenderFeatureConfig(
        config,
        RenderFeatureComposeDependencies{
            [&loader_called](std::string_view) {
                loader_called = true;
                return std::string{};
            },
            false,
        });

    REQUIRE_FALSE(result.used_features);
    REQUIRE_FALSE(loader_called);
    REQUIRE(result.shader_defines == std::vector<std::string>{"PELICAN_BASE_DEFINE"});
    REQUIRE(result.feature_names.empty());
    REQUIRE(result.config.at("resolver_version").get<int>() == 2);
    REQUIRE(result.config.at("render_targets").size() == 1);
    REQUIRE(result.config.at("render_targets").at(0).at("name").get<std::string>() == "display");
    REQUIRE(result.config.at("render_targets").at(0).at("format_class").get<std::string>() == "display");
}

TEST_CASE("canonical color pipeline rejects unsupported resolver versions", "[render-feature]") {
    const auto config = nlohmann::json{
        {"resolver_version", 1},
        {"render_targets", nlohmann::json::array()},
        {"rendering_passes", nlohmann::json::array()},
    };
    std::string message;
    try {
        (void)composeRenderFeatureConfig(config);
    } catch (const std::exception &ex) {
        message = ex.what();
    }
    REQUIRE(message == "Only rendering resolver_version 2 is supported");
}

CompiledRenderPipeline compileComposition(
    const RenderFeatureComposeResult &composition) {
    ResolvedRenderPipeline resolved;
    resolved.shader_defines = composition.shader_defines;
    resolved.feature_names = composition.feature_names;
    resolved.excluded_feature_names = composition.excluded_feature_names;
    resolved.projection_jitter = composition.projection_jitter;
    resolved.feature_instances = composition.feature_instances;
    resolved.material_routing = composition.material_routing;
    resolved.pipeline_preset = composition.pipeline_preset;
    resolved.used_features = composition.used_features;
    return compileRenderPipeline(resolved);
}

TEST_CASE("hybrid pipeline preset expands to explicit versioned material routes",
          "[render-feature][pipeline-preset][hybrid]") {
    const auto authored = nlohmann::json{
        {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
        {"features", nlohmann::json::array({"engine://features/ui.json"})},
    };
    const auto result = composeRenderFeatureConfig(
        authored, RenderFeatureComposeDependencies{loadEngineFeature, true});

    REQUIRE(result.pipeline_preset.has_value());
    REQUIRE(result.pipeline_preset->name == "hybrid_v1");
    REQUIRE(result.pipeline_preset->version == 1);
    REQUIRE_FALSE(result.config.contains("pipeline"));
    REQUIRE(result.feature_names == std::vector<std::string>{"ui"});
    REQUIRE(std::find(result.shader_defines.begin(), result.shader_defines.end(),
                      "PELICAN_HYBRID_SCENE_LINEAR") != result.shader_defines.end());

    const auto &routes = result.material_routing.at("routes");
    REQUIRE(routes.at("deferred_geometry").at("pass") == "deferred_geometry");
    REQUIRE(routes.at("forward_opaque").at("shader_contract") ==
            "forward_scene_color_v1");
    REQUIRE(routes.at("forward_transparent").at("phase") == "transparent");
    REQUIRE(passByName(result.config, "deferred_geometry").at("material_contract") ==
            "deferred_geometry_v1");
    REQUIRE(passByName(result.config, "forward_opaque").at("material_contract") ==
            "forward_opaque_v1");
    REQUIRE(passByName(result.config, "forward_transparent").at("material_contract") ==
            "forward_transparent_v1");
    REQUIRE(passByName(result.config, "scene_present").at("shader").at("fragment") ==
            "engine://scene_present");
    const auto &targets = result.config.at("render_targets");
    const auto lit_color = std::find_if(targets.begin(), targets.end(), [](const auto &target) {
        return target.value("name", std::string{}) == "lit_color";
    });
    REQUIRE(lit_color != targets.end());
    REQUIRE(lit_color->at("format") == "R16G16B16A16_SFLOAT");
    REQUIRE(lit_color->at("format_class") == "explicit(R16G16B16A16_SFLOAT)");
}

TEST_CASE("pipeline preset refuses structural deep merge and invalid route contracts",
          "[render-feature][pipeline-preset]") {
    auto structural_override = nlohmann::json{
        {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
        {"render_targets", nlohmann::json::array()},
    };
    REQUIRE_THROWS_WITH(
        composeRenderFeatureConfig(
            structural_override,
            RenderFeatureComposeDependencies{loadEngineFeature, true}),
        Catch::Matchers::ContainsSubstring("copy/eject") ||
            Catch::Matchers::ContainsSubstring("unknown key 'render_targets'"));

    auto invalid = nlohmann::json::parse(
        engineResourceOrThrow("render_pipelines/hybrid_v1.json"));
    invalid["config"]["rendering_passes"][0]["passes"][4]["material_contract"] =
        "forward_transparent_v1";
    REQUIRE_THROWS_WITH(
        composeRenderFeatureConfig(
            nlohmann::json{{"pipeline", {{"preset", "project://bad.json"}}}},
            RenderFeatureComposeDependencies{
                {}, true, {}, [text = invalid.dump()](std::string_view) { return text; }}),
        Catch::Matchers::ContainsSubstring("forward_opaque_v1"));
}

TEST_CASE("render feature fixtures compose and reject expected cases", "[render-feature]") {
    const auto expectations = readJson(fixtureRoot() / "expectations.json");

    for (const auto &entry : expectations) {
        const auto file = entry.at("file").get<std::string>();
        DYNAMIC_SECTION(file) {
            const auto expected = entry.at("expect").get<std::string>();
            const auto config = baseConfigWithFeature(file);
            if (expected == "ok") {
                const auto result = composeRenderFeatureConfig(
                    config,
                    RenderFeatureComposeDependencies{
                        loadFixtureFeature,
                        true,
                    });

                REQUIRE(result.used_features);
                REQUIRE_FALSE(result.config.contains("features"));
                REQUIRE(result.feature_names == std::vector<std::string>{"dummy_feature"});
                REQUIRE(passNames(result.config) ==
                        entry.at("expected_pass_order").get<std::vector<std::string>>());
                const auto &feature_pass = passByName(result.config, "feature_present");
                REQUIRE(feature_pass.at("name").get<std::string>() == "feature_present");
                REQUIRE(feature_pass.at("after").get<std::vector<std::string>>() ==
                        std::vector<std::string>{"__anchor_post_ldr", "present"});
                REQUIRE(result.shader_defines ==
                        entry.at("expected_shader_defines").get<std::vector<std::string>>());
                REQUIRE(result.config.at("render_targets").size() == 3);
                REQUIRE(result.config.at("render_targets").at(0).at("usage").get<std::vector<std::string>>() ==
                        std::vector<std::string>{"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"});
            } else {
                std::string message;
                try {
                    (void)composeRenderFeatureConfig(
                        config,
                        RenderFeatureComposeDependencies{
                            loadFixtureFeature,
                            true,
                        });
                } catch (const std::exception &ex) {
                    message = ex.what();
                }
                REQUIRE_FALSE(message.empty());
                requireErrorKind(message, entry.at("error_kind").get<std::string>());
            }
        }
    }
}

TEST_CASE("passless render feature records its feature name", "[render-feature]") {
    const auto config = nlohmann::json::parse(R"json({
  "features": ["engine://features/gpu_timing.json"],
  "render_targets": [],
  "rendering_passes": [
    {"name": "main", "passes": []}
  ]
})json");

    const auto result = composeRenderFeatureConfig(
        config,
        RenderFeatureComposeDependencies{
            [](std::string_view ref) {
                REQUIRE(std::string{ref} == "engine://features/gpu_timing.json");
                return std::string{R"json({
  "schema": "pelican.render_feature",
  "version": 1,
  "name": "gpu_timing"
})json"};
            },
            true,
        });

    REQUIRE(result.used_features);
    REQUIRE(result.feature_names == std::vector<std::string>{"gpu_timing"});
    REQUIRE_FALSE(result.config.contains("features"));
    REQUIRE(passNames(result.config).empty());
}

TEST_CASE("render features append buffers and compute tasks", "[render-feature]") {
    const auto config = nlohmann::json::parse(R"json({
  "features": ["compute_feature.json"],
  "render_targets": [],
  "rendering_passes": [
    {"name": "main", "passes": []}
  ]
})json");

    const auto result = composeRenderFeatureConfig(
        config,
        RenderFeatureComposeDependencies{
            [](std::string_view) {
                return std::string{R"json({
  "schema": "pelican.render_feature",
  "version": 1,
  "name": "compute_feature",
  "buffers": [
    {"name": "compute_color", "size": 16, "lifetime": "persistent"}
  ],
  "compute_tasks": [
    {
      "name": "write_color",
      "shader": "shaders/write_color",
      "writes": ["compute_color"],
      "before": ["present"],
      "dispatch": {"groups": [1, 1, 1]}
    }
  ]
})json"};
            },
            true,
        });

    REQUIRE(result.used_features);
    REQUIRE(result.feature_names == std::vector<std::string>{"compute_feature"});
    REQUIRE(result.config.at("buffers").at(0).at("name").get<std::string>() == "compute_color");
    REQUIRE(result.config.at("compute_tasks").at(0).at("name").get<std::string>() == "write_color");
}

TEST_CASE("HDR render feature marks scene targets and uses the canonical tonemap anchor", "[render-feature]") {
    const auto result = composeRenderFeatureConfig(
        baseConfigWithFeature("engine://features/hdr.json"),
        RenderFeatureComposeDependencies{
            loadEngineFeature,
            true,
        });

    REQUIRE(result.used_features);
    REQUIRE_FALSE(result.config.contains("features"));
    REQUIRE(passNames(result.config) ==
            std::vector<std::string>{"prepare", "present", "hdr_tonemap"});
    REQUIRE(result.shader_defines == std::vector<std::string>{"PELICAN_FEATURE_HDR"});

    const auto &lit_color = result.config.at("render_targets").at(0);
    REQUIRE(lit_color.at("name").get<std::string>() == "lit_color");
    REQUIRE(lit_color.at("format_class").get<std::string>() == "scene");
    REQUIRE(lit_color.at("role").get<std::string>() == "color");
    REQUIRE(lit_color.at("usage").get<std::vector<std::string>>() ==
            std::vector<std::string>{"COLOR_ATTACHMENT", "SAMPLED"});

    const auto &tonemap = passByName(result.config, "hdr_tonemap");
    REQUIRE(tonemap.at("name").get<std::string>() == "hdr_tonemap");
    REQUIRE(tonemap.at("after").get<std::vector<std::string>>() ==
            std::vector<std::string>{"__anchor_tonemap"});
    REQUIRE(tonemap.at("input").get<std::vector<std::string>>() ==
            std::vector<std::string>{"scene_ldr_in"});
    REQUIRE(tonemap.at("shader").at("vertex").get<std::string>() == "engine://tonemap");
    REQUIRE(tonemap.at("shader").at("fragment").get<std::string>() == "engine://tonemap");
    REQUIRE(tonemap.at("color_load_op").get<std::string>() == "load");
}

TEST_CASE("velocity feature is purgeable and occupies the scene-to-post boundary",
          "[render-feature][temporal]") {
    const auto result = composeRenderFeatureConfig(
        baseConfigWithFeature("engine://features/velocity.json"),
        RenderFeatureComposeDependencies{
            [](std::string_view ref) {
                REQUIRE(std::string{ref} == "engine://features/velocity.json");
                std::ifstream file{std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                                   "src/core/resources/features/velocity.json"};
                return std::string{std::istreambuf_iterator<char>{file},
                                   std::istreambuf_iterator<char>{}};
            },
            true,
        });
    REQUIRE(result.feature_names == std::vector<std::string>{"velocity"});
    const auto &targets = result.config.at("render_targets");
    const auto velocity = std::find_if(targets.begin(), targets.end(), [](const auto &target) {
        return target.value("name", std::string{}) == "velocity";
    });
    REQUIRE(velocity != targets.end());
    REQUIRE(velocity->at("format_class") == "data");
    const auto &passes = result.config.at("rendering_passes").at(0).at("passes");
    const auto pass = std::find_if(passes.begin(), passes.end(), [](const auto &entry) {
        return entry.value("name", std::string{}) == "velocity_pass";
    });
    const auto post_main = std::find_if(passes.begin(), passes.end(), [](const auto &entry) {
        return entry.value("name", std::string{}) == "__anchor_post_main";
    });
    REQUIRE(pass != passes.end());
    REQUIRE(post_main != passes.end());
    REQUIRE(pass < post_main);
}

TEST_CASE("bundled TAA composes into the example pipeline as a two-pass history graph",
          "[render-feature][temporal][taa]") {
    auto config = readJson(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                           "projects/example/passes/main_rendering_config.json");
    config["features"].push_back("engine://features/velocity.json");
    config["features"].push_back(nlohmann::json{
        {"ref", "engine://features/taa.json"},
        {"parameters", {
            {"scene_color", "lit_color"},
            {"velocity", "velocity"},
            {"depth", "offscreen_depth"},
            {"downstream_color", "lit_color"},
            {"alpha", 0.1},
            {"disocclusion_tau", 0.1},
            {"depth_epsilon", 0.00001},
        }},
    });

    const auto result = composeRenderFeatureConfig(
        config, RenderFeatureComposeDependencies{loadEngineFeature, true});
    REQUIRE(result.feature_names ==
            std::vector<std::string>{"ui", "velocity", "taa"});
    REQUIRE(result.projection_jitter == nlohmann::json{
        {"provider", "taa"}, {"pattern", "halton23"}, {"phases", 8}});

    const auto &resolve = passByName(result.config, "taa_resolve");
    REQUIRE(resolve.at("input") == nlohmann::json::array(
        {"lit_color", "velocity", "offscreen_depth", "taa_accum@history"}));
    REQUIRE(resolve.at("output").at("color") == "taa_accum");
    const auto &composite = passByName(result.config, "taa_composite");
    REQUIRE(composite.at("input") == nlohmann::json::array({"taa_accum"}));
    REQUIRE(composite.at("output").at("color") == "lit_color");

    const auto &targets = result.config.at("render_targets");
    const auto depth = std::find_if(targets.begin(), targets.end(), [](const auto &target) {
        return target.value("name", std::string{}) == "offscreen_depth";
    });
    REQUIRE(depth != targets.end());
    REQUIRE(depth->at("usage") ==
            nlohmann::json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"}));
    const auto accum = std::find_if(targets.begin(), targets.end(), [](const auto &target) {
        return target.value("name", std::string{}) == "taa_accum";
    });
    REQUIRE(accum != targets.end());
    REQUIRE(accum->at("history") == true);

    REQUIRE(result.shader_defines == std::vector<std::string>{
        "PELICAN_FEATURE_TAA",
        "PELICAN_FEATURE_TAA_ALPHA=0.100000001",
        "PELICAN_FEATURE_TAA_DISOCCLUSION_TAU=0.100000001",
        "PELICAN_FEATURE_TAA_DEPTH_EPSILON=9.99999975e-06",
    });

    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(result.config);
    REQUIRE(graphs.size() == 1);
    auto plan = planFrameGraph(graphs.front());
    const auto compiled_pipeline = compileComposition(result);
    const auto frame_plan = framePlanToJson(plan, &compiled_pipeline);
    const auto findNode = [&](std::string_view name) -> const nlohmann::json & {
        const auto &nodes = frame_plan.at("nodes");
        const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const auto &node) {
            return node.value("name", std::string{}) == name;
        });
        REQUIRE(found != nodes.end());
        return *found;
    };
    REQUIRE(findNode("taa_resolve").at("reads_history") ==
            nlohmann::json::array({"taa_accum"}));
    REQUIRE(findNode("taa_resolve").at("writes") ==
            nlohmann::json::array({"taa_accum"}));
    REQUIRE(findNode("taa_composite").at("writes") ==
            nlohmann::json::array({"lit_color"}));
}

TEST_CASE("shadow directional feature inserts depth pass and lighting dependency", "[render-feature]") {
    auto config = nlohmann::json::parse(R"json({
  "render_targets": [
    {
      "name": "lit_color",
      "extent_scale": 1.0,
      "format": "B8G8R8A8_UNORM",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "gbuffer_albedo",
      "extent_scale": 1.0,
      "format": "B8G8R8A8_UNORM",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    }
  ],
  "features": ["engine://features/shadow_directional.json"],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "gbuffer_pass",
          "type": "material",
          "output": {"color": ["gbuffer_albedo"], "depth": null}
        },
        {
          "name": "lighting_pass",
          "type": "fullscreen",
          "output": {"color": "lit_color", "depth": null},
          "input": ["gbuffer_albedo"],
          "shader": {"vertex": "engine://fullscreen", "fragment": "engine://fullscreen"}
        }
      ]
    }
  ]
})json");

    const auto result = composeRenderFeatureConfig(
        config,
        RenderFeatureComposeDependencies{
            loadEngineFeature,
            true,
        });

    REQUIRE(result.feature_names == std::vector<std::string>{"shadow_directional"});
    REQUIRE(result.shader_defines == std::vector<std::string>{"PELICAN_FEATURE_SHADOW"});
    REQUIRE(passNames(result.config) == std::vector<std::string>{"gbuffer_pass", "shadow_depth", "lighting_pass"});

    const auto &shadow_map = result.config.at("render_targets").back();
    REQUIRE(shadow_map.at("name").get<std::string>() == "shadow_map");
    REQUIRE(shadow_map.at("width").get<int>() == 2048);
    REQUIRE(shadow_map.at("height").get<int>() == 2048);

    const auto &shadow_pass = passByName(result.config, "shadow_depth");
    REQUIRE(shadow_pass.at("before").get<std::vector<std::string>>() ==
            std::vector<std::string>{"lighting_pass"});
    REQUIRE(shadow_pass.at("output").at("depth").get<std::string>() == "shadow_map");
    REQUIRE(shadow_pass.at("depth_store_op").get<std::string>() == "store");

    const auto &lighting_pass = passByName(result.config, "lighting_pass");
    REQUIRE(lighting_pass.at("input").get<std::vector<std::string>>() ==
            std::vector<std::string>{"gbuffer_albedo", "shadow_map"});
}

TEST_CASE("render features require the runtime shader compiler", "[render-feature]") {
    std::string message;
    try {
        (void)composeRenderFeatureConfig(
            baseConfigWithFeature("valid/dummy_feature.json"),
            RenderFeatureComposeDependencies{
                loadFixtureFeature,
                false,
            });
    } catch (const std::exception &ex) {
        message = ex.what();
    }
    REQUIRE(contains(message, "feature"));
    REQUIRE(contains(message, "実行時コンパイラ"));
    REQUIRE(contains(message, "runtime shader compiler"));
}

TEST_CASE("one opaque snapshot becomes a real copy node and sampled render target",
          "[render-feature][snapshot]") {
    const auto config = nlohmann::json::parse(R"json({
  "snapshots": [{"name": "opaque_color", "after": "opaque"}],
  "render_targets": [],
  "rendering_passes": [{"name": "main", "passes": [{
    "name": "opaque",
    "type": "fullscreen",
    "output": {"color": "swapchain", "depth": null}
  }]}]
})json");
    const auto result = composeRenderFeatureConfig(config);
    const auto &passes = result.config.at("rendering_passes").at(0).at("passes");
    const auto copy = std::find_if(passes.begin(), passes.end(), [](const auto &pass) {
        return pass.value("name", std::string{}) == "__snapshot_opaque_color";
    });
    REQUIRE(copy != passes.end());
    REQUIRE(copy->at("type") == "snapshot_copy");
    REQUIRE(copy->at("source") == "display");
    REQUIRE(copy->at("destination") == "opaque_color");
    REQUIRE(copy->at("snapshot_after") == "opaque");

    const auto &target = result.config.at("render_targets").back();
    REQUIRE(target.at("name") == "opaque_color");
    REQUIRE(target.at("format_class") == "display");
    REQUIRE(target.at("usage") == nlohmann::json::array({"TRANSFER_DST", "SAMPLED"}));
}

TEST_CASE("snapshot v1 rejects a second copy point by name", "[render-feature][snapshot]") {
    const auto config = nlohmann::json::parse(R"json({
  "snapshots": [
    {"name": "opaque_color", "after": "post_ldr"},
    {"name": "transparent_color", "after": "pelican_ui"}
  ],
  "render_targets": [],
  "rendering_passes": [{"name": "main", "passes": []}]
})json");
    REQUIRE_THROWS_WITH(composeRenderFeatureConfig(config),
                        Catch::Matchers::ContainsSubstring("transparent_color") &&
                            Catch::Matchers::ContainsSubstring("exactly one opaque snapshot") &&
                            Catch::Matchers::ContainsSubstring("sequential refraction"));
}

TEST_CASE("snapshot v1 rejects transparent-after copy points by name",
          "[render-feature][snapshot]") {
    const auto config = nlohmann::json::parse(R"json({
  "snapshots": [{"name": "late_color", "after": "pelican_ui"}],
  "render_targets": [],
  "rendering_passes": [{"name": "main", "passes": []}]
})json");
    REQUIRE_THROWS_WITH(composeRenderFeatureConfig(config),
                        Catch::Matchers::ContainsSubstring("late_color") &&
                            Catch::Matchers::ContainsSubstring("transparent-after") &&
                            Catch::Matchers::ContainsSubstring("unsupported in v1"));
}

TEST_CASE("projection jitter feature declaration is validated and exposed in compose result",
          "[render-feature][projection-jitter]") {
    auto config = baseConfigWithFeature("jitter.json");
    const auto compose = [&](nlohmann::json declaration) {
        return composeRenderFeatureConfig(
            config,
            RenderFeatureComposeDependencies{
                [declaration = std::move(declaration)](std::string_view) {
                    return nlohmann::json{
                        {"schema", "pelican.render_feature"},
                        {"version", 1},
                        {"name", "camera_jitter"},
                        {"projection_jitter", declaration},
                    }.dump();
                },
                true,
            });
    };

    const auto valid = compose(nlohmann::json{{"pattern", "halton23"}, {"phases", 8}});
    REQUIRE(valid.projection_jitter == nlohmann::json{
        {"provider", "camera_jitter"}, {"pattern", "halton23"}, {"phases", 8}});

    for (const auto &[declaration, needle] :
         std::vector<std::pair<nlohmann::json, std::string>>{
             {nlohmann::json{{"pattern", "random"}, {"phases", 8}}, "unknown pattern"},
             {nlohmann::json{{"pattern", "halton23"}, {"phases", 0}}, "range 1..64"},
             {nlohmann::json{{"pattern", "halton23"}, {"phases", 8}, {"extra", 1}},
              "unknown key"},
             {nlohmann::json{{"pattern", "halton23"}, {"phases", 8}, {"render_scale", 1}},
              "reserved key"},
         }) {
        INFO(declaration.dump());
        REQUIRE_THROWS_WITH(compose(declaration),
                            Catch::Matchers::ContainsSubstring("camera_jitter") &&
                                Catch::Matchers::ContainsSubstring(needle));
    }
}

TEST_CASE("projection jitter table schema derives phases and rejects invalid tables",
          "[render-feature][projection-jitter][table]") {
    auto config = baseConfigWithFeature("table_jitter.json");
    const auto compose = [&](nlohmann::json declaration) {
        return composeRenderFeatureConfig(
            config,
            RenderFeatureComposeDependencies{
                [declaration = std::move(declaration)](std::string_view) {
                    return nlohmann::json{
                        {"schema", "pelican.render_feature"},
                        {"version", 1},
                        {"name", "table_jitter"},
                        {"projection_jitter", declaration},
                    }.dump();
                },
                true,
            });
    };
    const auto fixtures = readJson(fixtureRoot() / "projection_jitter_table.json");

    for (const auto &fixture : fixtures.at("valid")) {
        DYNAMIC_SECTION(fixture.at("name").get<std::string>()) {
            const auto result = compose(fixture.at("declaration"));
            const auto expected = nlohmann::json{
                {"provider", "table_jitter"},
                {"pattern", "table"},
                {"phases", fixture.at("expected_phases")},
                {"offsets_px", fixture.at("declaration").at("offsets_px")},
            };
            REQUIRE(result.projection_jitter == expected);

            const auto graphs = parseFrameGraphDefinitionsFromConfigJson(result.config);
            REQUIRE(graphs.size() == 1);
            auto plan = planFrameGraph(graphs.front());
            const auto compiled_pipeline = compileComposition(result);
            const auto dumped_jitter =
                framePlanToJson(plan, &compiled_pipeline)
                    .at("projection_jitter");
            REQUIRE(dumped_jitter == expected);
            REQUIRE(dumped_jitter.dump() == expected.dump());
        }
    }

    for (const auto &fixture : fixtures.at("invalid")) {
        DYNAMIC_SECTION(fixture.at("name").get<std::string>()) {
            REQUIRE_THROWS_WITH(
                compose(fixture.at("declaration")),
                Catch::Matchers::ContainsSubstring("table_jitter") &&
                    Catch::Matchers::ContainsSubstring(fixture.at("error").get<std::string>()));
        }
    }

    auto too_many = nlohmann::json::array();
    for (std::size_t index = 0; index < 65; ++index) {
        too_many.push_back({0.0, 0.0});
    }
    REQUIRE_THROWS_WITH(
        compose(nlohmann::json{{"pattern", "table"}, {"offsets_px", too_many}}),
        Catch::Matchers::ContainsSubstring("table_jitter") &&
            Catch::Matchers::ContainsSubstring("length must be in range 1..64"));
}

TEST_CASE("projection jitter rejects a second provider by both feature names",
          "[render-feature][projection-jitter]") {
    auto config = baseConfigWithFeature("first.json");
    config["features"] = nlohmann::json::array({"first.json", "second.json"});
    REQUIRE_THROWS_WITH(
        composeRenderFeatureConfig(
            config,
            RenderFeatureComposeDependencies{
                [](std::string_view ref) {
                    const auto name = ref == "first.json" ? "first_jitter" : "second_jitter";
                    return std::string{R"json({"schema":"pelican.render_feature","version":1,"name":")json"} +
                           name +
                           R"json(","projection_jitter":{"pattern":"halton23","phases":8}})json";
                },
                true,
            }),
        Catch::Matchers::ContainsSubstring("first_jitter") &&
            Catch::Matchers::ContainsSubstring("second_jitter"));
}

TEST_CASE("named render-target binding resolves pass IO history and override keys",
          "[render-feature][binding]") {
    const std::string feature = R"json({
      "schema":"pelican.render_feature",
      "version":1,
      "name":"bound_temporal",
      "parameters":{
        "schema":"pelican.render_feature_parameters",
        "version":1,
        "render_targets":[
          {"name":"source","required":true,"role":"color","format_class":"scene",
           "extent_scale":1.0,"sample_count":1,"usage":["SAMPLED"]},
          {"name":"destination","required":false,"default":"bound_output","role":"color",
           "format_class":"scene","extent_scale":1.0,"sample_count":1,
           "usage":["COLOR_ATTACHMENT"]}
        ]
      },
      "render_target_overrides":{"$source":{"usage":["TRANSFER_SRC"]}},
      "passes":[{
        "insert":"before:present",
        "pass":{
          "name":"bound_temporal_pass","type":"fullscreen",
          "input":["$source@history"],
          "output":{"color":"$destination","depth":null},
          "shader":{"vertex":"shaders/fullscreen","fragment":"shaders/bound"}
        }
      }]
    })json";

    const auto composeFor = [&](std::string source) {
        auto config = baseConfigWithFeature("bound.json");
        config["features"] = nlohmann::json::array({nlohmann::json{
            {"ref", "bound.json"}, {"parameters", {{"source", source}}}}});
        for (const auto &name : {"history_a", "history_b"}) {
            config["render_targets"].push_back({
                {"name", name}, {"extent_scale", 1.0},
                {"format", "B8G8R8A8_UNORM"}, {"format_class", "scene"},
                {"role", "color"}, {"sample_count", 1}, {"history", true},
                {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
            });
        }
        config["render_targets"].push_back({
            {"name", "bound_output"}, {"extent_scale", 1.0},
            {"format", "B8G8R8A8_UNORM"}, {"format_class", "scene"},
            {"role", "color"}, {"sample_count", 1},
            {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
        });
        return composeRenderFeatureConfig(
            config, RenderFeatureComposeDependencies{
                        [feature](std::string_view) { return feature; }, true});
    };

    for (const auto &source : {"history_a", "history_b"}) {
        const auto result = composeFor(source);
        INFO(source);
        REQUIRE(result.feature_instances.size() == 1);
        REQUIRE(result.feature_instances.at(0).at("parameters") == nlohmann::json{
            {"destination", "bound_output"}, {"source", source}});
        const auto &pass = passByName(result.config, "bound_temporal_pass");
        REQUIRE(pass.at("input").at(0) == source + std::string{"@history"});
        REQUIRE(pass.at("output").at("color") == "bound_output");
        const auto target = std::find_if(
            result.config.at("render_targets").begin(), result.config.at("render_targets").end(),
            [&](const auto &entry) { return entry.value("name", std::string{}) == source; });
        REQUIRE(target != result.config.at("render_targets").end());
        REQUIRE(std::find(target->at("usage").begin(), target->at("usage").end(),
                          "TRANSFER_SRC") != target->at("usage").end());

        const auto graphs = parseFrameGraphDefinitionsFromConfigJson(result.config);
        REQUIRE(graphs.size() == 1);
        auto plan = planFrameGraph(graphs.front());
        const auto compiled_pipeline = compileComposition(result);
        const auto plan_json = framePlanToJson(plan, &compiled_pipeline);
        REQUIRE(plan_json.at("feature_instances").at(0).at("parameters").at("source") == source);
        const auto planned = std::find_if(
            plan_json.at("nodes").begin(), plan_json.at("nodes").end(),
            [](const auto &node) { return node.at("name") == "bound_temporal_pass"; });
        REQUIRE(planned != plan_json.at("nodes").end());
        REQUIRE(planned->at("reads_history").at(0) == source);
        REQUIRE(planned->at("writes").at(0) == "bound_output");
    }
}

TEST_CASE("named render-target binding errors identify feature parameter and target",
          "[render-feature][binding]") {
    const std::string feature = R"json({
      "schema":"pelican.render_feature","version":1,"name":"binding_errors",
      "parameters":{
        "schema":"pelican.render_feature_parameters","version":1,
        "render_targets":[
          {"name":"source","required":true,"role":"color","format_class":"scene",
           "extent_scale":1.0,"sample_count":1,"usage":["SAMPLED"]}
        ]
      }
    })json";
    const auto composeParameters = [&](nlohmann::json parameters) {
        auto config = baseConfigWithFeature("errors.json");
        config["features"] = nlohmann::json::array({nlohmann::json{
            {"ref", "errors.json"}, {"parameters", std::move(parameters)}}});
        return composeRenderFeatureConfig(
            config, RenderFeatureComposeDependencies{
                        [feature](std::string_view) { return feature; }, true});
    };
    const auto requireBindingError = [&](nlohmann::json parameters, std::string parameter,
                                         std::string target, std::string detail) {
        try {
            (void)composeParameters(std::move(parameters));
            FAIL("expected named binding rejection");
        } catch (const std::exception &error) {
            const std::string message = error.what();
            REQUIRE(contains(message, "binding_errors"));
            REQUIRE(contains(message, parameter));
            REQUIRE(contains(message, target));
            REQUIRE(contains(message, detail));
        }
    };

    requireBindingError(nlohmann::json::object(), "source", "<missing>", "required");
    requireBindingError({{"unknown", "lit_color"}}, "unknown", "lit_color", "unknown parameter");
    requireBindingError({{"source", 42}}, "source", "<non-string>",
                        "binding must name a render target");
    requireBindingError({{"source", "does_not_exist"}}, "source", "does_not_exist", "unknown render target");

    auto compatible = baseConfigWithFeature("errors.json");
    compatible["features"] = nlohmann::json::array({nlohmann::json{
        {"ref", "errors.json"}, {"parameters", {{"source", "lit_color"}}}}});
    compatible["render_targets"].at(0)["role"] = "color";
    compatible["render_targets"].at(0)["format_class"] = "scene";
    compatible["render_targets"].at(0)["sample_count"] = 1;

    const auto requireConstraintError = [&](std::string field, nlohmann::json value,
                                            std::string detail) {
        auto incompatible = compatible;
        incompatible["render_targets"].at(0)[field] = std::move(value);
        try {
            (void)composeRenderFeatureConfig(
                incompatible, RenderFeatureComposeDependencies{
                                  [feature](std::string_view) { return feature; }, true});
            FAIL("expected target constraint rejection");
        } catch (const std::exception &error) {
            const std::string message = error.what();
            REQUIRE(contains(message, "binding_errors"));
            REQUIRE(contains(message, "source"));
            REQUIRE(contains(message, "lit_color"));
            REQUIRE(contains(message, detail));
        }
    };
    requireConstraintError("role", "data", "role is incompatible");
    requireConstraintError("format_class", "data", "format_class is incompatible");
    requireConstraintError("extent_scale", 0.5, "extent_scale is incompatible");
    requireConstraintError("sample_count", 4, "sample_count is incompatible");
    requireConstraintError("usage", nlohmann::json::array({"COLOR_ATTACHMENT"}),
                           "usage is incompatible");
}

TEST_CASE("scalar feature parameters resolve defaults and lower to deterministic defines",
          "[render-feature][binding][scalar-parameter]") {
    const auto feature = nlohmann::json::parse(R"json({
      "schema":"pelican.render_feature",
      "version":1,
      "name":"taa",
      "parameters":{
        "schema":"pelican.render_feature_parameters",
        "version":1,
        "render_targets":[
          {"name":"source","required":false,"default":"lit_color"}
        ],
        "scalars":[
          {"name":"alpha","type":"float","range":[0.0,1.0],"default":0.1},
          {"name":"iterations","type":"int","range":[1,16],"default":4},
          {"name":"enabled","type":"bool","default":true}
        ]
      },
      "shader_defines":["PELICAN_FEATURE_TAA"]
    })json");
    const auto composeFor = [&](nlohmann::json parameters) {
        auto config = baseConfigWithFeature("taa.json");
        config["features"] = nlohmann::json::array({nlohmann::json{
            {"ref", "taa.json"}, {"parameters", std::move(parameters)}}});
        return composeRenderFeatureConfig(
            config, RenderFeatureComposeDependencies{
                        [text = feature.dump()](std::string_view) { return text; }, true});
    };

    const auto first = composeFor({{"alpha", 0.1}, {"iterations", 7}, {"enabled", false}});
    REQUIRE(first.shader_defines == std::vector<std::string>{
        "PELICAN_FEATURE_TAA",
        "PELICAN_FEATURE_TAA_ALPHA=0.100000001",
        "PELICAN_FEATURE_TAA_ITERATIONS=7",
        "PELICAN_FEATURE_TAA_ENABLED=0",
    });
    REQUIRE(first.config.at("shader_defines") == first.shader_defines);
    REQUIRE(first.feature_instances.at(0).at("parameters") == nlohmann::json{
        {"alpha", 0.1}, {"enabled", false}, {"iterations", 7}, {"source", "lit_color"}});

    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(first.config);
    REQUIRE(graphs.size() == 1);
    auto plan = planFrameGraph(graphs.front());
    const auto compiled_pipeline = compileComposition(first);
    const auto plan_json = framePlanToJson(plan, &compiled_pipeline);
    REQUIRE(plan_json.at("feature_instances").at(0).at("parameters").at("alpha") == 0.1);
    REQUIRE(plan_json.at("feature_instances").at(0).at("parameters").at("enabled") == false);

    const auto second = composeFor({{"alpha", 0.2}});
    REQUIRE(second.shader_defines == std::vector<std::string>{
        "PELICAN_FEATURE_TAA",
        "PELICAN_FEATURE_TAA_ALPHA=0.200000003",
        "PELICAN_FEATURE_TAA_ITERATIONS=4",
        "PELICAN_FEATURE_TAA_ENABLED=1",
    });
    REQUIRE(second.feature_instances.at(0).at("parameters").at("alpha") == 0.2);
    REQUIRE(second.feature_instances.at(0).at("parameters").at("iterations") == 4);
    REQUIRE(second.feature_instances.at(0).at("parameters").at("enabled") == true);
    REQUIRE(first.shader_defines != second.shader_defines);
}

TEST_CASE("scalar feature parameter declaration and instance errors name feature and parameter",
          "[render-feature][binding][scalar-parameter]") {
    const auto compose = [&](nlohmann::json scalars, nlohmann::json values) {
        auto feature = nlohmann::json{
            {"schema", "pelican.render_feature"},
            {"version", 1},
            {"name", "scalar_errors"},
            {"parameters", {
                {"schema", "pelican.render_feature_parameters"},
                {"version", 1},
                {"scalars", std::move(scalars)},
            }},
        };
        auto config = baseConfigWithFeature("scalar_errors.json");
        config["features"] = nlohmann::json::array({nlohmann::json{
            {"ref", "scalar_errors.json"}, {"parameters", std::move(values)}}});
        return composeRenderFeatureConfig(
            config, RenderFeatureComposeDependencies{
                        [text = feature.dump()](std::string_view) { return text; }, true});
    };
    const auto valid_declarations = nlohmann::json::array({
        {{"name", "alpha"}, {"type", "float"}, {"range", {0.0, 1.0}}, {"default", 0.1}},
        {{"name", "iterations"}, {"type", "int"}, {"range", {1, 16}}, {"default", 4}},
        {{"name", "enabled"}, {"type", "bool"}, {"default", true}},
    });
    const auto requireError = [&](nlohmann::json declarations, nlohmann::json values,
                                  std::string_view parameter, std::string_view detail) {
        try {
            (void)compose(std::move(declarations), std::move(values));
            FAIL("expected scalar parameter rejection");
        } catch (const std::exception &error) {
            const std::string message = error.what();
            REQUIRE(contains(message, "scalar_errors"));
            REQUIRE(contains(message, parameter));
            REQUIRE(contains(message, detail));
        }
    };

    requireError(valid_declarations, {{"unknown", 1}}, "unknown", "unknown parameter");
    requireError(valid_declarations, {{"alpha", "wrong"}}, "alpha", "type float");
    requireError(valid_declarations, {{"iterations", 2.0}}, "iterations", "type int");
    requireError(valid_declarations, {{"enabled", 1}}, "enabled", "type bool");
    requireError(valid_declarations, {{"alpha", 1.5}}, "alpha", "outside declared range");
    requireError(nlohmann::json::array({
                     {{"name", "bad"}, {"type", "vec2"}, {"default", 0.0}}}),
                 nlohmann::json::object(), "bad", "unknown scalar type");
    requireError(nlohmann::json::array({
                     {{"name", "bad"}, {"type", "float"}, {"range", {2.0, 1.0}},
                      {"default", 1.0}}}),
                 nlohmann::json::object(), "bad", "minimum exceeds maximum");
    requireError(nlohmann::json::array({
                     {{"name", "bad"}, {"type", "int"}, {"range", {0, 2}},
                      {"default", 3}}}),
                 nlohmann::json::object(), "bad", "outside declared range");
    requireError(nlohmann::json::array({
                     {{"name", "bad"}, {"type", "bool"}, {"range", {false, true}},
                      {"default", true}}}),
                 nlohmann::json::object(), "bad", "must not have range");
}

} // namespace Pelican
