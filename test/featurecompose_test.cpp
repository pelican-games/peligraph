#include "../src/project/featurecompose.hpp"
#include "../src/core/loader/engineresources.hpp"

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

} // namespace Pelican
