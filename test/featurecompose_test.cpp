#include "../src/project/featurecompose.hpp"
#include "../src/project/renderpipeline.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
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

const nlohmann::json &computeTaskByName(
    const nlohmann::json &config,
    std::string_view name) {
    for (const auto &task :
         config.at("compute_tasks")) {
        if (task.value("name", std::string{}) ==
            name) {
            return task;
        }
    }
    throw std::runtime_error(
        "compute task not found: " +
        std::string{name});
}

const nlohmann::json &renderTargetByName(
    const nlohmann::json &config,
    std::string_view name) {
    for (const auto &target :
         config.at("render_targets")) {
        if (target.value("name", std::string{}) ==
            name) {
            return target;
        }
    }
    throw std::runtime_error(
        "render target not found: " +
        std::string{name});
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
    resolved.normalized_config = composition.config;
    resolved.shader_defines = composition.shader_defines;
    resolved.feature_names = composition.feature_names;
    resolved.excluded_feature_names = composition.excluded_feature_names;
    resolved.projection_jitter = composition.projection_jitter;
    resolved.feature_instances = composition.feature_instances;
    resolved.surface_resource_contracts =
        composition.surface_resource_contracts;
    resolved.material_routing = composition.material_routing;
    resolved.draw_sort = composition.draw_sort;
    resolved.sample_count_policy =
        compileSampleCountPolicy(composition.config);
    resolved.pipeline_preset = composition.pipeline_preset;
    resolved.used_features = composition.used_features;
    return compileRenderPipeline(resolved);
}

TEST_CASE(
    "directional shadow contract binds hybrid deferred and forward consumers",
    "[render-feature][shadow][hybrid][wp205]") {
    const auto compose = [](std::string feature_ref) {
        return composeRenderFeatureConfig(
            nlohmann::json{
                {"pipeline",
                 {{"preset",
                   "engine://render_pipelines/hybrid_v1.json"}}},
                {"features",
                 nlohmann::json::array(
                     {std::move(feature_ref)})},
            },
            RenderFeatureComposeDependencies{
                [](std::string_view ref) {
                    if (ref ==
                        "project://features/shadow_copy.json") {
                        return engineResourceOrThrow(
                            "features/shadow_directional.json");
                    }
                    return loadEngineFeature(ref);
                },
                true,
            });
    };

    const auto engine = compose(
        "engine://features/shadow_directional.json");
    REQUIRE(passNames(engine.config).front() ==
            "shadow_depth");
    REQUIRE(passByName(engine.config, "forward_opaque")
                .at("surface_resources")
                .at("directional_shadow") ==
            "shadow_map");
    REQUIRE(passByName(engine.config, "forward_transparent")
                .at("surface_resources")
                .at("directional_shadow") ==
            "shadow_map");
    REQUIRE(passByName(engine.config, "deferred_lighting")
                .at("input")
                .back() ==
            "shadow_map");

    const auto &declaration =
        engine.surface_resource_contracts.front();
    REQUIRE(declaration.at("material_consumers") ==
            nlohmann::json::array(
                {"forward_opaque",
                 "forward_transparent"}));
    REQUIRE(declaration.at("fullscreen_consumers") ==
            nlohmann::json::array(
                {"deferred_lighting"}));

    const auto compiled =
        compileComposition(engine);
    REQUIRE(compiled.surface_resource_contracts.size() ==
            1);
    const auto metadata =
        serializeCompiledRenderPipelineMetadata(
            compiled)
            .at("surface_resource_contracts")
            .front();
    REQUIRE(metadata.at("producer") == "shadow_depth");
    REQUIRE(metadata.at("view_policy") == "family_array");
    REQUIRE(metadata.at("fallback") == "fully_lit");
    REQUIRE(metadata.at("fallback_reason") ==
            "provider_feature_absent");
    REQUIRE(metadata.at("relation").at("kind") ==
            "pelican.light.directional_shadow@1");
    REQUIRE(metadata.at("relation").at("light_index") ==
            0);
    REQUIRE(metadata.at("relation").at("transform") ==
            "pelican.light.shadow_view_projection@1");

    const auto copied = compose(
        "project://features/shadow_copy.json");
    REQUIRE(copied.config == engine.config);
    REQUIRE(copied.shader_defines ==
            engine.shader_defines);
    auto copied_contract =
        copied.surface_resource_contracts.front();
    auto engine_contract =
        engine.surface_resource_contracts.front();
    copied_contract.erase("provider_ref");
    engine_contract.erase("provider_ref");
    REQUIRE(copied_contract == engine_contract);
}

TEST_CASE(
    "rt shadow mask is purgeable, compiler-optional, and propagates ray-query planning",
    "[render-feature][wp283][ray-query]") {
    const auto make_config = [](bool enabled) {
        return nlohmann::json{
            {"pipeline",
             {{"preset",
               "engine://render_pipelines/hybrid_v1.json"}}},
            {"features",
             enabled
                 ? nlohmann::json::array({
                       "engine://features/rt_shadow_mask.json"})
                 : nlohmann::json::array()},
        };
    };
    // This feature has a no-define embedded SPIR-V artifact, so composition
    // itself is valid with the optional runtime compiler disabled.
    const auto enabled = composeRenderFeatureConfig(
        make_config(true),
        RenderFeatureComposeDependencies{
            loadEngineFeature, false});
    REQUIRE(enabled.feature_names ==
            std::vector<std::string>{"rt_shadow_mask"});
    const auto &target =
        renderTargetByName(enabled.config, "rt_shadow_mask");
    REQUIRE(target.at("format") == "R8_UNORM");
    REQUIRE(target.at("usage") ==
            nlohmann::json::array(
                {"COLOR_ATTACHMENT", "TRANSFER_SRC"}));
    const auto &pass =
        passByName(enabled.config, "rt_shadow_mask");
    REQUIRE(pass.at("type") == "fullscreen");
    REQUIRE(pass.at("input") ==
            nlohmann::json::array(
                {"gbuffer_worldpos", "gbuffer_normal"}));
    REQUIRE(pass.at("output").at("color") ==
            "rt_shadow_mask");
    const auto &required =
        enabled.config.at("target_planning")
            .at("graphs")
            .at("main_render")
            .at("required_capabilities");
    REQUIRE(required == nlohmann::json::array(
                            {"pelican.vulkan.ray_query@1"}));
    for (const auto &candidate :
         enabled.config.at("rendering_passes")
             .at(0)
             .at("passes")) {
        if (candidate.value("name", std::string{}) ==
            "rt_shadow_mask") {
            continue;
        }
        if (!candidate.contains("input")) continue;
        REQUIRE(std::none_of(
            candidate.at("input").begin(),
            candidate.at("input").end(),
            [](const auto &input) {
                return input.is_string() &&
                       input.get<std::string>() ==
                           "rt_shadow_mask";
            }));
    }

    const auto disabled = composeRenderFeatureConfig(
        make_config(false),
        RenderFeatureComposeDependencies{
            loadEngineFeature, false});
    REQUIRE(disabled.feature_names.empty());
    REQUIRE_THROWS_WITH(
        renderTargetByName(disabled.config, "rt_shadow_mask"),
        Catch::Matchers::ContainsSubstring(
            "render target not found"));
    REQUIRE_THROWS_WITH(
        passByName(disabled.config, "rt_shadow_mask"),
        Catch::Matchers::ContainsSubstring("pass not found"));
    REQUIRE_FALSE(disabled.config.contains("target_planning"));
}

TEST_CASE(
    "RT pipeline shadow mask is a purgeable storage-image compute node",
    "[render-feature][wp285][ray-tracing]") {
    const auto make_config = [](bool enabled) {
        return nlohmann::json{
            {"pipeline",
             {{"preset",
               "engine://render_pipelines/hybrid_v1.json"}}},
            {"features",
             enabled
                 ? nlohmann::json::array({
                       "engine://features/rt_shadow_mask_pipeline.json"})
                 : nlohmann::json::array()},
        };
    };
    const auto enabled = composeRenderFeatureConfig(
        make_config(true),
        RenderFeatureComposeDependencies{
            loadEngineFeature, false});
    REQUIRE(enabled.feature_names ==
            std::vector<std::string>{
                "rt_shadow_mask_pipeline"});
    const auto &target = renderTargetByName(
        enabled.config, "rt_shadow_mask_pipeline");
    CHECK(target.at("format") == "R8_UNORM");
    CHECK(target.at("usage") ==
          nlohmann::json::array(
              {"STORAGE", "TRANSFER_SRC"}));
    const auto &task = computeTaskByName(
        enabled.config, "rt_shadow_mask_pipeline");
    CHECK(task.contains("ray_tracing"));
    CHECK(task.at("writes") ==
          nlohmann::json::array(
              {"rt_shadow_mask_pipeline"}));
    CHECK(task.at("dispatch").at("rays_from").at("port") ==
          "shadow_mask");
    const auto &required =
        enabled.config.at("target_planning")
            .at("graphs")
            .at("main_render")
            .at("required_capabilities");
    CHECK(required == nlohmann::json::array(
                          {"pelican.vulkan.ray_tracing_pipeline@1"}));
    for (const auto &pass_set :
         enabled.config.at("rendering_passes")) {
        for (const auto &pass : pass_set.at("passes")) {
            CHECK(pass.value("name", std::string{}) !=
                  "rt_shadow_mask_pipeline");
        }
    }

    const auto disabled = composeRenderFeatureConfig(
        make_config(false),
        RenderFeatureComposeDependencies{
            loadEngineFeature, false});
    CHECK(disabled.feature_names.empty());
    CHECK_THROWS_WITH(
        renderTargetByName(
            disabled.config, "rt_shadow_mask_pipeline"),
        Catch::Matchers::ContainsSubstring(
            "render target not found"));
    CHECK_FALSE(disabled.config.contains("compute_tasks"));
    CHECK_FALSE(disabled.config.contains("target_planning"));
}

TEST_CASE(
    "surface resource selectors validate even without fullscreen passes",
    "[render-feature][shadow][validation][wp205]") {
    auto feature = nlohmann::json::parse(
        engineResourceOrThrow(
            "features/shadow_directional.json"));
    feature["surface_resources"][0]
           ["fullscreen_consumers"] =
        nlohmann::json::array(
            {{{"unknown_selector_field", true}}});

    const auto authored = nlohmann::json{
        {"features",
         nlohmann::json::array(
             {"fixture://invalid_shadow_selector"})},
        {"render_targets", nlohmann::json::array()},
        {"rendering_passes",
         nlohmann::json::array(
             {{{"name", "main_render"},
               {"passes",
                nlohmann::json::array(
                    {{{"name", "forward_opaque"},
                      {"type", "material"},
                      {"material_contract",
                       "forward_opaque_v1"},
                      {"output",
                       {{"color", "swapchain"},
                        {"depth", nullptr}}}}})}}})},
    };

    REQUIRE_THROWS_WITH(
        composeRenderFeatureConfig(
            authored,
            RenderFeatureComposeDependencies{
                [text = feature.dump()](
                    std::string_view) {
                    return text;
                },
                true,
            }),
        Catch::Matchers::ContainsSubstring(
            "unknown_selector_field"));
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
    REQUIRE(passByName(result.config, "forward_transparent")
                .at("screen_inputs")
                .at("opaque_color") == "opaque_color");
    REQUIRE(passByName(result.config, "forward_transparent")
                .at("screen_inputs")
                .at("linear_view_depth") == "opaque_depth");
    REQUIRE(passByName(result.config, "__snapshot_opaque_color").at("source") ==
            "lit_color");
    REQUIRE(passByName(result.config, "__snapshot_opaque_depth").at("source") ==
            "scene_depth");
    REQUIRE(passByName(result.config, "scene_present").at("shader").at("fragment") ==
            "engine://scene_present");
    REQUIRE(result.draw_sort.at("opaque").at("provider") ==
            "state_batched_v1");
    REQUIRE(result.draw_sort.at("transparent").at("provider") ==
            "back_to_front_v1");
    REQUIRE(result.draw_sort.at("xr_view_policy") ==
            "logical_view_center");
    const auto &targets = result.config.at("render_targets");
    const auto lit_color = std::find_if(targets.begin(), targets.end(), [](const auto &target) {
        return target.value("name", std::string{}) == "lit_color";
    });
    REQUIRE(lit_color != targets.end());
    REQUIRE(lit_color->at("format") == "R16G16B16A16_SFLOAT");
    REQUIRE(lit_color->at("format_class") == "explicit(R16G16B16A16_SFLOAT)");
    const auto opaque_color = std::find_if(
        targets.begin(), targets.end(), [](const auto &target) {
            return target.value("name", std::string{}) == "opaque_color";
        });
    const auto opaque_depth = std::find_if(
        targets.begin(), targets.end(), [](const auto &target) {
            return target.value("name", std::string{}) == "opaque_depth";
        });
    REQUIRE(opaque_color != targets.end());
    REQUIRE(opaque_depth != targets.end());
    REQUIRE(opaque_color->at("format") == "R16G16B16A16_SFLOAT");
    REQUIRE(opaque_depth->at("format") == "D32_SFLOAT");

    const auto &passes = result.config.at("rendering_passes").at(0).at("passes");
    REQUIRE(std::count_if(passes.begin(), passes.end(), [](const auto &pass) {
                return pass.value("name", std::string{}) == "scene_present";
            }) == 1);
    REQUIRE(std::count_if(passes.begin(), passes.end(), [](const auto &pass) {
                return pass.value("type", std::string{}) == "output_transform";
            }) == 1);
}

TEST_CASE(
    "sky ambient feature is purgeable and binds runtime-only values to hybrid",
    "[render-feature][sky][ambient][wp240b]") {
    const auto compose =
        [](nlohmann::json features) {
            return composeRenderFeatureConfig(
                nlohmann::json{
                    {"pipeline",
                     {{"preset",
                       "engine://render_pipelines/hybrid_v1.json"}}},
                    {"features",
                     std::move(features)},
                },
                RenderFeatureComposeDependencies{
                    loadEngineFeature, true});
        };
    const auto passCount =
        [](const nlohmann::json &config,
           std::string_view name) {
            const auto &passes =
                config.at("rendering_passes")
                    .at(0)
                    .at("passes");
            return std::count_if(
                passes.begin(), passes.end(),
                [name](const auto &pass) {
                    return pass.value(
                               "name",
                               std::string{}) ==
                           name;
                });
        };

    const auto without_feature =
        compose(nlohmann::json::array());
    REQUIRE(
        passCount(
            without_feature.config,
            "sky_background") == 0);
    REQUIRE(
        std::find(
            without_feature.shader_defines.begin(),
            without_feature.shader_defines.end(),
            "PELICAN_FEATURE_SKY_AMBIENT") ==
        without_feature.shader_defines.end());

    const auto defaults =
        compose(nlohmann::json::array(
            {"engine://features/sky_ambient.json"}));
    REQUIRE(defaults.feature_names ==
            std::vector<std::string>{
                "sky_ambient"});
    REQUIRE(
        passCount(
            defaults.config,
            "sky_background") == 1);
    const auto names =
        passNames(defaults.config);
    const auto lighting =
        std::find(
            names.begin(), names.end(),
            "deferred_lighting");
    const auto sky =
        std::find(
            names.begin(), names.end(),
            "sky_background");
    const auto forward =
        std::find(
            names.begin(), names.end(),
            "forward_opaque");
    REQUIRE(lighting != names.end());
    REQUIRE(sky != names.end());
    REQUIRE(forward != names.end());
    REQUIRE(lighting < sky);
    REQUIRE(sky < forward);

    const auto &pass =
        passByName(
            defaults.config,
            "sky_background");
    REQUIRE(pass.at("input") ==
            nlohmann::json::array(
                {"scene_depth"}));
    REQUIRE(pass.at("input_sampling") ==
            nlohmann::json::array(
                {{{"filter", "nearest"},
                  {"address",
                   "clamp_to_edge"}}}));
    REQUIRE(pass.at("output").at("color") ==
            nlohmann::json::array(
                {"lit_color"}));
    REQUIRE(pass.at("color_load_op") ==
            "load");
    REQUIRE(pass.at("uses_light_data") ==
            true);
    REQUIRE(pass.at("shader").at("fragment") ==
            "engine://sky_ambient");
    REQUIRE(
        renderTargetByName(
            defaults.config,
            "scene_depth")
            .at("usage") ==
        nlohmann::json::array(
            {"DEPTH_STENCIL_ATTACHMENT",
             "TRANSFER_SRC",
             "SAMPLED"}));
    REQUIRE(
        std::find(
            defaults.shader_defines.begin(),
            defaults.shader_defines.end(),
            "PELICAN_FEATURE_SKY_AMBIENT") !=
        defaults.shader_defines.end());
    REQUIRE(
        std::none_of(
            defaults.shader_defines.begin(),
            defaults.shader_defines.end(),
            [](const std::string &define) {
                return define.starts_with(
                    "PELICAN_FEATURE_SKY_AMBIENT_");
            }));

    const auto compiled_defaults =
        compileComposition(defaults);
    REQUIRE(
        compiled_defaults
            .feature_instances.size() == 1);
    const auto &default_parameters =
        compiled_defaults
            .feature_instances.front()
            .parameters;
    const auto parameter =
        [&default_parameters](
            std::string_view name) {
            const auto found =
                std::find_if(
                    default_parameters.begin(),
                    default_parameters.end(),
                    [name](const auto &candidate) {
                        return candidate.name ==
                               name;
                    });
            REQUIRE(
                found !=
                default_parameters.end());
            return std::get<double>(
                found->value);
        };
    REQUIRE(parameter("color_r") == 0.12);
    REQUIRE(parameter("color_g") == 0.16);
    REQUIRE(parameter("color_b") == 0.24);
    REQUIRE(
        parameter("ambient_intensity") ==
        0.5);
    REQUIRE(parameter("sky_intensity") ==
            1.0);

    const auto overridden =
        compose(nlohmann::json::array(
            {nlohmann::json{
                {"ref",
                 "engine://features/sky_ambient.json"},
                {"parameters",
                 {{"color_r", 0.25},
                  {"ambient_intensity",
                   0.75}}},
            }}));
    const auto overridden_pipeline =
        compileComposition(overridden);
    const auto &overridden_parameters =
        overridden_pipeline
            .feature_instances.front()
            .parameters;
    const auto color_r =
        std::find_if(
            overridden_parameters.begin(),
            overridden_parameters.end(),
            [](const auto &candidate) {
                return candidate.name ==
                       "color_r";
            });
    const auto intensity =
        std::find_if(
            overridden_parameters.begin(),
            overridden_parameters.end(),
            [](const auto &candidate) {
                return candidate.name ==
                       "ambient_intensity";
            });
    REQUIRE(color_r !=
            overridden_parameters.end());
    REQUIRE(intensity !=
            overridden_parameters.end());
    REQUIRE(
        std::get<double>(color_r->value) ==
        0.25);
    REQUIRE(
        std::get<double>(intensity->value) ==
        0.75);

    auto runtime_config = defaults.config;
    for (auto &target :
         runtime_config.at("render_targets")) {
        target["width"] = 16;
        target["height"] = 16;
    }
    const auto graphs =
        parseFrameGraphDefinitionsFromConfigJson(
            runtime_config);
    REQUIRE(graphs.size() == 1);
    const auto plan =
        planFrameGraph(graphs.front());
    const auto plan_json =
        framePlanToJson(
            plan, &compiled_defaults);
    const auto node =
        std::find_if(
            plan_json.at("nodes").begin(),
            plan_json.at("nodes").end(),
            [](const auto &candidate) {
                return candidate.at("name") ==
                       "sky_background";
            });
    REQUIRE(node !=
            plan_json.at("nodes").end());
    REQUIRE(node->at("reads") ==
            nlohmann::json::array(
                {"scene_depth", "lit_color"}));
    REQUIRE(node->at("writes") ==
            nlohmann::json::array(
                {"lit_color"}));
}

TEST_CASE("pipeline preset accepts an explicit typed draw-sort override",
          "[render-feature][pipeline-preset][draw-sort][wp184]") {
    const auto authored = nlohmann::json{
        {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
        {"draw_sort",
         {
             {"opaque", {{"provider", "fixture.opaque"}}},
             {"transparent", {{"provider", "fixture.transparent"}}},
             {"xr_view_policy", "per_view"},
         }},
    };
    const auto composition = composeRenderFeatureConfig(
        authored, RenderFeatureComposeDependencies{loadEngineFeature, true});
    const auto compiled = compileComposition(composition);

    REQUIRE(composition.config.at("draw_sort") == authored.at("draw_sort"));
    REQUIRE(compiled.draw_sorting.opaque.provider == "fixture.opaque");
    REQUIRE(compiled.draw_sorting.transparent.provider ==
            "fixture.transparent");
    REQUIRE(compiled.draw_sorting.xr_view_policy ==
            DrawSortXrViewPolicy::per_view);
}

TEST_CASE("pipeline preset compiles MSAA settings to a typed policy",
          "[render-feature][pipeline-preset][sample-count]") {
    const auto authored = nlohmann::json{
        {"pipeline",
         {
             {"preset", "engine://render_pipelines/hybrid_v1.json"},
             {"settings",
              {{"msaa",
                {{"samples", 4},
                 {"fallback", "lower_supported"},
                 {"scope", "geometry"}}}}},
         }},
    };
    const auto composition = composeRenderFeatureConfig(
        authored,
        RenderFeatureComposeDependencies{loadEngineFeature, true});
    const auto compiled = compileComposition(composition);

    REQUIRE(composition.config.at("multisampling") ==
            authored.at("pipeline").at("settings").at("msaa"));
    REQUIRE(compiled.sample_count_policy.authored);
    REQUIRE(compiled.sample_count_policy.request ==
            (SampleCountRequest{SampleCountRequestMode::prefer, 4}));
    REQUIRE(compiled.sample_count_policy.scope ==
            SampleCountScope::geometry);
    REQUIRE(serializeCompiledRenderPipelineMetadata(compiled)
                .at("sample_count")
                .at("samples") == 4);
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

TEST_CASE(
    "clustered lighting feature shares one typed selection across hybrid consumers",
    "[render-feature][clustered][lighting-data][wp208]") {
    const auto result =
        composeRenderFeatureConfig(
            nlohmann::json{
                {"pipeline",
                 {{"preset",
                   "engine://render_pipelines/hybrid_v1.json"}}},
                {"features",
                 nlohmann::json::array(
                     {"engine://features/clustered_lighting.json"})},
            },
            RenderFeatureComposeDependencies{
                loadEngineFeature,
                true,
            });

    REQUIRE(result.feature_names ==
            std::vector<std::string>{
                "clustered_lighting"});
    REQUIRE(std::find(
                result.shader_defines.begin(),
                result.shader_defines.end(),
                "PELICAN_FEATURE_CLUSTERED_LIGHTING") !=
            result.shader_defines.end());
    REQUIRE(
        result.config.at("buffers").size() == 2);
    REQUIRE(
        result.config.at("compute_tasks").size() == 1);
    const auto &selector =
        result.config.at("compute_tasks").front();
    REQUIRE(selector.at("name") ==
            "clustered_light_select");
    REQUIRE(selector.at("reads") ==
            nlohmann::json::array(
                {"clustered_light_inventory"}));
    REQUIRE(selector.at("writes") ==
            nlohmann::json::array(
                {"clustered_light_selection"}));
    REQUIRE(
        selector.at("schedule") ==
        "per_view");
    REQUIRE(
        result.config.at("buffers")
            .at(1)
            .at("size_from_extent")
            .at("copies") == 2);

    const auto &deferred =
        passByName(
            result.config, "deferred_lighting");
    REQUIRE(
        deferred.at("resource_ports")
            .at("light_inventory")
            .at("element") == "uvec4");
    REQUIRE(
        deferred.at("resource_ports")
            .at("light_selection")
            .at("element") == "uint");
    for (const auto pass :
         {"forward_opaque",
          "forward_transparent"}) {
        const auto &resources =
            passByName(result.config, pass)
                .at("material_resources");
        REQUIRE(
            resources.at("light_inventory") ==
            "clustered_light_inventory");
        REQUIRE(
            resources.at("light_selection") ==
            "clustered_light_selection");
    }

    const auto compiled =
        compileComposition(result);
    REQUIRE(compiled.lighting_data.has_value());
    REQUIRE(
        compiled.lighting_data->provider_feature ==
        "clustered_lighting");
    REQUIRE(
        compiled.lighting_data->provider_reference ==
        "engine://features/clustered_lighting.json");
    REQUIRE(
        compiled.lighting_data->max_lights_per_tile ==
        64);
    const auto metadata =
        serializeCompiledRenderPipelineMetadata(
            compiled);
    REQUIRE(
        metadata.at("lighting_data")
            .at("inventory_contract") ==
        "pelican.light_inventory_v2");
    REQUIRE(
        metadata.at("lighting_data")
            .at("selection_contract") ==
        "project.clustered_selection_v2");
    REQUIRE(
        metadata.at("lighting_data")
            .at("xr_path") ==
        "compute_clustered");

}

TEST_CASE(
    "hybrid preset keeps the legacy small-light path when clustered lighting is absent",
    "[render-feature][clustered][feature-off][wp208]") {
    const auto result =
        composeRenderFeatureConfig(
            nlohmann::json{
                {"pipeline",
                 {{"preset",
                   "engine://render_pipelines/hybrid_v1.json"}}},
            },
            RenderFeatureComposeDependencies{
                loadEngineFeature,
                true,
            });

    REQUIRE(result.feature_names.empty());
    REQUIRE(std::find(
                result.shader_defines.begin(),
                result.shader_defines.end(),
                "PELICAN_FEATURE_CLUSTERED_LIGHTING") ==
            result.shader_defines.end());
    REQUIRE_FALSE(
        result.config.contains("lighting_data"));
    REQUIRE_FALSE(result.config.contains("buffers"));
    REQUIRE_FALSE(
        result.config.contains("compute_tasks"));
    REQUIRE_FALSE(
        passByName(
            result.config, "deferred_lighting")
            .contains("resource_ports"));
    for (const auto pass :
         {"forward_opaque",
          "forward_transparent"}) {
        REQUIRE_FALSE(
            passByName(result.config, pass)
                .contains("material_resources"));
    }

    const auto compiled =
        compileComposition(result);
    REQUIRE_FALSE(compiled.lighting_data);
}

TEST_CASE(
    "planar reflection feature builds a clipped secondary-family render slice",
    "[render-feature][reflection][view-family]") {
    const auto compose =
        [](nlohmann::json features) {
            return composeRenderFeatureConfig(
                nlohmann::json{
                    {"pipeline",
                     {{"preset",
                       "engine://render_pipelines/hybrid_v1.json"}}},
                    {"features",
                     std::move(features)},
                },
                RenderFeatureComposeDependencies{
                    loadEngineFeature,
                    true,
                });
        };
    const auto reflection_instance =
        nlohmann::json{
            {"ref",
             "engine://features/planar_reflection.json"},
            {"parameters",
             {
                 {"resolution", 64},
                 {"plane_x", 0.0},
                 {"plane_y", 2.0},
                 {"plane_z", 0.0},
                 {"plane_offset", -2.0},
                 {"preserve_raster_winding", true},
                 {"oblique_near_plane", true},
             }},
        };
    const auto result =
        compose(nlohmann::json::array(
            {reflection_instance}));

    REQUIRE(
        result.feature_names ==
        std::vector<std::string>{
            "planar_reflection"});
    for (const auto target_name : {
             "planar_reflection_albedo",
             "planar_reflection_normal",
             "planar_reflection_material",
             "planar_reflection_worldpos",
             "planar_reflection_emissive",
             "planar_reflection_depth",
             "planar_reflection_ao",
             "planar_reflection_ao_blur",
             "planar_reflection_color",
             "planar_reflection_opaque_color",
             "planar_reflection_opaque_depth",
         }) {
        const auto target =
            std::find_if(
                result.config.at("render_targets")
                    .begin(),
                result.config.at("render_targets")
                    .end(),
                [&](const auto &candidate) {
                    return candidate.at("name") ==
                           target_name;
                });
        REQUIRE(
            target !=
            result.config.at("render_targets")
                .end());
        REQUIRE(target->at("width") == 64);
        REQUIRE(target->at("height") == 64);
        REQUIRE(target->at("layers") == 2);
        REQUIRE(target->at("extent_scale") == 1.0);
        if (target_name ==
            std::string_view{
                "planar_reflection_color"}) {
            REQUIRE(
                target->at("mip_levels") == 7);
            REQUIRE(
                std::find(
                    target->at("usage").begin(),
                    target->at("usage").end(),
                    "STORAGE") !=
                target->at("usage").end());
        }
    }
    for (const auto pass_name : {
             "planar_reflection_geometry",
             "planar_reflection_ssao",
             "planar_reflection_ssao_blur",
             "planar_reflection_lighting",
             "planar_reflection_forward_opaque",
             "planar_reflection_forward_transparent",
         }) {
        REQUIRE(
            passByName(
                result.config, pass_name)
                .at("view_family") ==
            "$reflection/planar");
        REQUIRE(
            passByName(
                result.config, pass_name)
                .at("resolution_domain") ==
            "independent");
    }
    for (const auto pass_name : {
             "planar_reflection_snapshot_opaque_color",
             "planar_reflection_snapshot_opaque_depth",
         }) {
        const auto &snapshot =
            passByName(
                result.config, pass_name);
        REQUIRE(
            snapshot.at("type") ==
            "snapshot_copy");
        REQUIRE(
            snapshot.at("view_family") ==
            "$reflection/planar");
    }
    const auto &forward_capture =
        passByName(
            result.config,
            "planar_reflection_forward_opaque");
    REQUIRE(
        forward_capture.at(
            "material_contract") ==
        "forward_opaque_v1");
    REQUIRE(
        forward_capture.at("output")
            .at("color") ==
        nlohmann::json::array(
            {"planar_reflection_color"}));
    REQUIRE(
        forward_capture.at("output")
            .at("depth") ==
        "planar_reflection_depth");
    REQUIRE(
        forward_capture.at(
            "color_load_op") ==
        "load");
    REQUIRE(
        forward_capture.at(
            "depth_load_op") ==
        "load");
    REQUIRE_FALSE(
        forward_capture.contains(
            "inherit_bindings_from"));
    const auto &transparent_capture =
        passByName(
            result.config,
            "planar_reflection_forward_transparent");
    REQUIRE(
        transparent_capture.at(
            "material_contract") ==
        "forward_transparent_v1");
    REQUIRE(
        transparent_capture.at(
            "screen_inputs")
            .at("opaque_color") ==
        "planar_reflection_opaque_color");
    REQUIRE(
        transparent_capture.at(
            "screen_inputs")
            .at("opaque_depth") ==
        "planar_reflection_opaque_depth");
    const auto reflection_feedback_binding =
        nlohmann::json{
            {"resource",
             "planar_reflection_opaque_color"},
            {"access", "sampled"},
            {"view", "family_array"},
            {"sampling",
             {
                 {"filter", "linear"},
                 {"address", "clamp_to_edge"},
             }},
            {"footprint", "arbitrary"},
        };
    REQUIRE(
        transparent_capture.at(
            "material_resources")
            .at("planar_reflection") ==
        reflection_feedback_binding);
    REQUIRE_FALSE(
        transparent_capture.contains(
            "inherit_bindings_from"));
    const auto &main_reflection =
        passByName(
            result.config,
            "forward_transparent")
            .at("material_resources")
            .at("planar_reflection");
    REQUIRE(
        main_reflection.at("resource") ==
        "planar_reflection_color");
    REQUIRE(
        main_reflection.at("view") ==
        "family_array");
    const auto remaining_mips =
        nlohmann::json{
            {"mip", 0},
            {"mip_count", "remaining"},
            {"layer", 0},
            {"layer_count", 1},
        };
    REQUIRE(
        main_reflection.at("subresource") ==
        remaining_mips);
    REQUIRE(
        result.config.at("compute_tasks")
            .size() == 6);
    for (std::uint32_t mip = 1;
         mip < 7; ++mip) {
        const auto &task =
            computeTaskByName(
                result.config,
                "planar_reflection_filter_mip_" +
                    std::to_string(mip));
        REQUIRE(
            task.at("schedule") ==
            "per_view");
        REQUIRE(
            task.at("view_family") ==
            "$reflection/planar");
        REQUIRE(
            task.at("shader") ==
            "engine://render_algorithms/planar_reflection/standard_prefilter");
        REQUIRE(
            task.at("resource_ports")
                .at("source_color")
                .at("view") ==
            "family_array");
        REQUIRE(
            task.at("resource_ports")
                .at("filtered_color")
                .at("view") ==
            "family_array");
        REQUIRE(
            task.at("resource_ports")
                .at("source_color")
                .at("subresource")
                .at("mip") ==
            mip - 1);
        REQUIRE(
            task.at("resource_ports")
                .at("filtered_color")
                .at("subresource")
                .at("mip") ==
            mip);
        REQUIRE(
            task.at("dispatch")
                .at("groups_from")
                .at("port") ==
            "filtered_color");
    }
    REQUIRE(
        computeTaskByName(
            result.config,
            "planar_reflection_filter_mip_6")
            .at("before") ==
        nlohmann::json::array(
            {"forward_transparent"}));
    REQUIRE(
        std::find(
            result.shader_defines.begin(),
            result.shader_defines.end(),
            "PELICAN_FEATURE_PLANAR_REFLECTION_PREFILTER_RADIUS=1") !=
        result.shader_defines.end());

    const auto compiled =
        compileComposition(result);
    const auto feature =
        std::find_if(
            compiled.feature_instances.begin(),
            compiled.feature_instances.end(),
            [](const auto &candidate) {
                return candidate.feature ==
                       "planar_reflection";
            });
    REQUIRE(
        feature !=
        compiled.feature_instances.end());
    const auto plane_y =
        std::find_if(
            feature->parameters.begin(),
            feature->parameters.end(),
            [](const auto &parameter) {
                return parameter.name ==
                       "plane_y";
            });
    REQUIRE(
        plane_y !=
        feature->parameters.end());
    REQUIRE(
        std::get<double>(
            plane_y->value) ==
        Catch::Approx(2.0));
    const auto oblique_near_plane =
        std::find_if(
            feature->parameters.begin(),
            feature->parameters.end(),
            [](const auto &parameter) {
                return parameter.name ==
                       "oblique_near_plane";
            });
    REQUIRE(
        oblique_near_plane !=
        feature->parameters.end());
    REQUIRE(
        std::get<bool>(
            oblique_near_plane->value));
    const auto prefilter_radius =
        std::find_if(
            feature->parameters.begin(),
            feature->parameters.end(),
            [](const auto &parameter) {
                return parameter.name ==
                       "prefilter_radius";
            });
    REQUIRE(
        prefilter_radius !=
        feature->parameters.end());
    REQUIRE(
        std::get<double>(
            prefilter_radius->value) ==
        Catch::Approx(1.0));
    const auto prefilter_shader =
        std::find_if(
            feature->parameters.begin(),
            feature->parameters.end(),
            [](const auto &parameter) {
                return parameter.name ==
                       "prefilter_shader";
            });
    REQUIRE(
        prefilter_shader !=
        feature->parameters.end());
    REQUIRE(
        std::get<std::string>(
            prefilter_shader->value) ==
        "engine://render_algorithms/planar_reflection/standard_prefilter");

    for (const auto shadow_first :
         {false, true}) {
        auto features =
            nlohmann::json::array();
        if (shadow_first) {
            features.push_back(
                "engine://features/shadow_directional.json");
        }
        features.push_back(
            reflection_instance);
        if (!shadow_first) {
            features.push_back(
                "engine://features/shadow_directional.json");
        }
        const auto combined =
            compose(std::move(features));
        const auto &lighting =
            passByName(
                combined.config,
                "planar_reflection_lighting");
        REQUIRE(
            std::find(
                lighting.at("input").begin(),
                lighting.at("input").end(),
                "shadow_map") !=
            lighting.at("input").end());
        REQUIRE(
            passByName(
                combined.config,
                "planar_reflection_forward_opaque")
                .at("surface_resources")
                .at("directional_shadow") ==
            "shadow_map");
        REQUIRE(
            passByName(
                combined.config,
                "planar_reflection_forward_transparent")
                .at("surface_resources")
                .at("directional_shadow") ==
            "shadow_map");
    }

    for (const auto clustered_first :
         {false, true}) {
        auto features =
            nlohmann::json::array();
        if (clustered_first) {
            features.push_back(
                "engine://features/clustered_lighting.json");
        }
        features.push_back(
            reflection_instance);
        if (!clustered_first) {
            features.push_back(
                "engine://features/clustered_lighting.json");
        }
        const auto combined =
            compose(std::move(features));
        const auto &lighting =
            passByName(
                combined.config,
                "planar_reflection_lighting");
        REQUIRE(
            lighting.at("resource_ports")
                .at("light_inventory")
                .at("resource") ==
            "clustered_light_inventory");
        REQUIRE(
            lighting.at("resource_ports")
                .at("light_selection")
                .at("resource") ==
            "planar_reflection_light_selection");
        REQUIRE(
            std::find(
                lighting.at("input").begin(),
                lighting.at("input").end(),
                "clustered_light_inventory") !=
            lighting.at("input").end());
        REQUIRE(
            std::find(
                lighting.at("input").begin(),
                lighting.at("input").end(),
                "planar_reflection_light_selection") !=
            lighting.at("input").end());
        const auto &resources =
            passByName(
                combined.config,
                "planar_reflection_forward_opaque")
                .at("material_resources");
        REQUIRE(
            resources.at(
                "light_inventory") ==
            "clustered_light_inventory");
        REQUIRE(
            resources.at(
                "light_selection") ==
            "planar_reflection_light_selection");
        const auto &transparent_resources =
            passByName(
                combined.config,
                "planar_reflection_forward_transparent")
                .at("material_resources");
        REQUIRE(
            transparent_resources.at(
                "light_inventory") ==
            "clustered_light_inventory");
        REQUIRE(
            transparent_resources.at(
                "light_selection") ==
            "planar_reflection_light_selection");
        REQUIRE(
            transparent_resources.at(
                "planar_reflection") ==
            transparent_capture.at(
                "material_resources")
                .at("planar_reflection"));
        const auto &buffers =
            combined.config.at("buffers");
        REQUIRE(
            std::count_if(
                buffers.begin(),
                buffers.end(),
                [](const auto &buffer) {
                    return buffer.at("name") ==
                           "planar_reflection_light_selection";
                }) == 1);
        const auto &tasks =
            combined.config.at(
                "compute_tasks");
        const auto reflection_selector =
            std::find_if(
                tasks.begin(), tasks.end(),
                [](const auto &task) {
                    return task.at("name") ==
                           "planar_reflection_light_select";
                });
        REQUIRE(
            reflection_selector !=
            tasks.end());
        REQUIRE(
            reflection_selector->at(
                "view_family") ==
            "$reflection/planar");
        REQUIRE(
            reflection_selector->at(
                "schedule") ==
            "per_view");
        REQUIRE(
            reflection_selector
                ->at("writes") ==
            nlohmann::json::array({
                "planar_reflection_light_selection"}));
    }

    auto deferred_only_feature =
        nlohmann::json::parse(
            loadEngineFeature(
                "engine://features/planar_reflection.json"));
    auto &deferred_only_passes =
        deferred_only_feature.at("passes");
    deferred_only_passes.erase(
        std::remove_if(
            deferred_only_passes.begin(),
            deferred_only_passes.end(),
            [](const auto &entry) {
                const auto name =
                    entry.at("pass")
                        .value(
                            "name",
                            std::string{});
                return name ==
                           "planar_reflection_forward_opaque" ||
                       name.starts_with(
                           "planar_reflection_snapshot_opaque_") ||
                       name ==
                           "planar_reflection_forward_transparent";
            }),
        deferred_only_passes.end());
    const auto deferred_only =
        composeRenderFeatureConfig(
            nlohmann::json{
                {"pipeline",
                 {{"preset",
                   "engine://render_pipelines/hybrid_v1.json"}}},
                {"features",
                 nlohmann::json::array({
                     {
                         {"ref",
                          "fixture://planar_reflection_deferred_only"},
                         {"parameters",
                          {
                              {"resolution", 64},
                              {"plane_x", 0.0},
                              {"plane_y", 1.0},
                              {"plane_z", 0.0},
                              {"plane_offset", 0.0},
                              {"preserve_raster_winding", true},
                          }},
                     },
                     "engine://features/clustered_lighting.json",
                 })},
            },
            RenderFeatureComposeDependencies{
                [&](std::string_view ref) {
                    if (ref ==
                        "fixture://planar_reflection_deferred_only") {
                        return deferred_only_feature.dump();
                    }
                    return loadEngineFeature(ref);
                },
                true,
            });
    const auto deferred_only_names =
        passNames(deferred_only.config);
    REQUIRE(
        std::find(
            deferred_only_names.begin(),
            deferred_only_names.end(),
            "planar_reflection_forward_opaque") ==
        deferred_only_names.end());
    REQUIRE(
        passByName(
            deferred_only.config,
            "planar_reflection_lighting")
            .at("resource_ports")
            .at("light_selection")
            .at("resource") ==
        "planar_reflection_light_selection");
    REQUIRE(
        std::count_if(
            deferred_only.config.at("compute_tasks")
                .begin(),
            deferred_only.config.at("compute_tasks")
                .end(),
            [](const auto &task) {
                return task.at("name") ==
                       "planar_reflection_light_select";
            }) == 1);
}

TEST_CASE(
    "cube capture feature composes one replaceable six-face hybrid slice",
    "[render-feature][cube-capture][view-family]") {
    const auto capture =
        nlohmann::json{
            {"ref",
             "engine://features/cube_capture.json"},
            {"parameters",
             {
                 {"resolution", 64},
                 {"position_x", 2.0},
                 {"position_y", 3.0},
                 {"position_z", -4.0},
                 {"near_distance", 0.25},
                 {"far_distance", 250.0},
             }},
        };
    const auto compose =
        [&](nlohmann::json features) {
            return composeRenderFeatureConfig(
                nlohmann::json{
                    {"pipeline",
                     {{"preset",
                       "engine://render_pipelines/hybrid_v1.json"}}},
                    {"features",
                     std::move(features)},
                },
                RenderFeatureComposeDependencies{
                    loadEngineFeature,
                    true,
                });
        };
    const auto result =
        compose(
            nlohmann::json::array({
                capture,
            }));

    REQUIRE(
        result.feature_names ==
        std::vector<std::string>{
            "cube_capture"});
    for (const auto target_name : {
             "cube_capture_albedo",
             "cube_capture_normal",
             "cube_capture_material",
             "cube_capture_worldpos",
             "cube_capture_emissive",
             "cube_capture_depth",
             "cube_capture_ao",
             "cube_capture_ao_blur",
             "cube_capture_color",
             "cube_capture_opaque_color",
             "cube_capture_opaque_depth",
         }) {
        const auto &target =
            renderTargetByName(
                result.config,
                target_name);
        REQUIRE(target.at("width") == 64);
        REQUIRE(target.at("height") == 64);
        REQUIRE(target.at("layers") == 6);
        REQUIRE(
            target.at("extent_scale") ==
            1.0);
    }
    const auto &cube =
        renderTargetByName(
            result.config,
            "cube_capture_color");
    REQUIRE(
        cube.at("dimension") == "cube");
    REQUIRE(cube.at("mip_levels") == 1);

    for (const auto pass_name : {
             "cube_capture_geometry",
             "cube_capture_ssao",
             "cube_capture_ssao_blur",
             "cube_capture_lighting",
             "cube_capture_forward_opaque",
             "cube_capture_forward_transparent",
         }) {
        const auto &pass =
            passByName(
                result.config,
                pass_name);
        REQUIRE(
            pass.at("view_family") ==
            "$capture/cube");
        REQUIRE(
            pass.at("resolution_domain") ==
            "independent");
        REQUIRE_FALSE(
            pass.contains(
                "inherit_bindings_from"));
    }
    for (const auto pass_name : {
             "cube_capture_snapshot_opaque_color",
             "cube_capture_snapshot_opaque_depth",
         }) {
        const auto &pass =
            passByName(
                result.config,
                pass_name);
        REQUIRE(
            pass.at("type") ==
            "snapshot_copy");
        REQUIRE(
            pass.at("view_family") ==
            "$capture/cube");
    }
    REQUIRE(
        passByName(
            result.config,
            "cube_capture_lighting")
            .at("output")
            .at("color") ==
        nlohmann::json::array({
            "cube_capture_color"}));
    REQUIRE(
        passByName(
            result.config,
            "cube_capture_forward_transparent")
            .at("screen_inputs")
            .at("opaque_color") ==
        "cube_capture_opaque_color");
    REQUIRE(
        std::find(
            result.shader_defines.begin(),
            result.shader_defines.end(),
            "PELICAN_FEATURE_CUBE_CAPTURE") !=
        result.shader_defines.end());

    const auto compiled =
        compileComposition(result);
    const auto feature =
        std::find_if(
            compiled.feature_instances.begin(),
            compiled.feature_instances.end(),
            [](const auto &candidate) {
                return candidate.feature ==
                       "cube_capture";
            });
    REQUIRE(
        feature !=
        compiled.feature_instances.end());
    const auto position_x =
        std::find_if(
            feature->parameters.begin(),
            feature->parameters.end(),
            [](const auto &parameter) {
                return parameter.name ==
                       "position_x";
            });
    REQUIRE(
        position_x !=
        feature->parameters.end());
    REQUIRE(
        std::get<double>(
            position_x->value) ==
        Catch::Approx(2.0));

    for (const auto clustered_first :
         {false, true}) {
        auto features =
            nlohmann::json::array();
        if (clustered_first) {
            features.push_back(
                "engine://features/clustered_lighting.json");
        }
        features.push_back(capture);
        if (!clustered_first) {
            features.push_back(
                "engine://features/clustered_lighting.json");
        }
        const auto combined =
            compose(std::move(features));
        const auto &lighting =
            passByName(
                combined.config,
                "cube_capture_lighting");
        REQUIRE(
            lighting.at("resource_ports")
                .at("light_selection")
                .at("resource") ==
            "cube_capture_light_selection");
        REQUIRE(
            passByName(
                combined.config,
                "cube_capture_forward_opaque")
                .at("material_resources")
                .at("light_selection") ==
            "cube_capture_light_selection");
        REQUIRE(
            passByName(
                combined.config,
                "cube_capture_forward_transparent")
                .at("material_resources")
                .at("light_selection") ==
            "cube_capture_light_selection");
        const auto selection =
            std::find_if(
                combined.config.at("buffers")
                    .begin(),
                combined.config.at("buffers")
                    .end(),
                [](const auto &buffer) {
                    return buffer.at("name") ==
                           "cube_capture_light_selection";
                });
        REQUIRE(
            selection !=
            combined.config.at("buffers")
                .end());
        REQUIRE(
            selection->at("size_from_extent")
                .at("copies") == 6);
        const auto &selector =
            computeTaskByName(
                combined.config,
                "cube_capture_light_select");
        REQUIRE(
            selector.at("view_family") ==
            "$capture/cube");
        REQUIRE(
            selector.at("schedule") ==
            "per_view");
        REQUIRE(
            selector.at("before") ==
            nlohmann::json::array({
                "cube_capture_lighting"}));
        REQUIRE_NOTHROW(
            compileComposition(
                combined));
    }
}

TEST_CASE(
    "render feature pass binding inheritance rejects invalid dependency graphs",
    "[render-feature][binding-inheritance]") {
    const auto compose =
        [](nlohmann::json feature) {
            return composeRenderFeatureConfig(
                nlohmann::json{
                    {"pipeline",
                     {{"preset",
                       "engine://render_pipelines/hybrid_v1.json"}}},
                    {"features",
                     nlohmann::json::array(
                         {"fixture://binding_inheritance"})},
                },
                RenderFeatureComposeDependencies{
                    [text = feature.dump()](
                        std::string_view) {
                        return text;
                    },
                    true,
                    {},
                    loadEngineFeature,
                });
        };

    SECTION("unknown source") {
        auto feature =
            nlohmann::json::parse(
                engineResourceOrThrow(
                    "features/planar_reflection.json"));
        feature["passes"][4]["pass"]
               ["inherit_bindings_from"] =
            "missing_pass";
        REQUIRE_THROWS_WITH(
            compose(std::move(feature)),
            Catch::Matchers::ContainsSubstring(
                "references unknown pass 'missing_pass'"));
    }

    SECTION("cycle") {
        auto feature =
            nlohmann::json::parse(
                engineResourceOrThrow(
                    "features/planar_reflection.json"));
        feature["passes"][3]["pass"]
               ["inherit_bindings_from"] =
            "planar_reflection_forward_opaque";
        feature["passes"][4]["pass"]
               ["inherit_bindings_from"] =
            "planar_reflection_lighting";
        REQUIRE_THROWS_WITH(
            compose(std::move(feature)),
            Catch::Matchers::ContainsSubstring(
                "contains a cycle"));
    }

    SECTION("material contract mismatch") {
        auto feature =
            nlohmann::json::parse(
                engineResourceOrThrow(
                    "features/planar_reflection.json"));
        feature["passes"][4]["pass"]
               ["material_contract"] =
            "deferred_geometry_v1";
        REQUIRE_THROWS_WITH(
            compose(std::move(feature)),
            Catch::Matchers::ContainsSubstring(
                "requires the same material_contract"));
    }
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

TEST_CASE("picking feature owns a purgeable integer target and geometry pass",
          "[render-feature][picking][wp262]") {
    const auto enabled = composeRenderFeatureConfig(
        baseConfigWithFeature("engine://features/picking.json"),
        RenderFeatureComposeDependencies{loadEngineFeature, true});
    REQUIRE(enabled.feature_names ==
            std::vector<std::string>{"picking"});

    const auto &id_target =
        renderTargetByName(enabled.config, "picking_id");
    REQUIRE(id_target.at("format") == "R32_UINT");
    REQUIRE(id_target.at("format_class") == "data");
    REQUIRE(id_target.at("usage") ==
            nlohmann::json::array(
                {"COLOR_ATTACHMENT", "TRANSFER_SRC"}));
    const auto &depth_target =
        renderTargetByName(enabled.config, "picking_depth");
    REQUIRE(depth_target.at("format") == "D32_SFLOAT");

    const auto &passes =
        enabled.config.at("rendering_passes").at(0).at("passes");
    const auto picking = std::find_if(
        passes.begin(), passes.end(), [](const auto &entry) {
            return entry.value("name", std::string{}) ==
                   "picking_pass";
        });
    const auto post_main = std::find_if(
        passes.begin(), passes.end(), [](const auto &entry) {
            return entry.value("name", std::string{}) ==
                   "__anchor_post_main";
        });
    REQUIRE(picking != passes.end());
    REQUIRE(picking->at("type") == "picking");
    REQUIRE(post_main != passes.end());
    REQUIRE(picking < post_main);

    auto disabled_config =
        baseConfigWithFeature("engine://features/picking.json");
    disabled_config["features"] = nlohmann::json::array();
    bool loader_called = false;
    const auto disabled = composeRenderFeatureConfig(
        disabled_config,
        RenderFeatureComposeDependencies{
            [&loader_called](std::string_view) {
                loader_called = true;
                return std::string{};
            },
            true,
        });
    REQUIRE_FALSE(loader_called);
    REQUIRE(disabled.feature_names.empty());
    REQUIRE(std::none_of(
        disabled.config.at("render_targets").begin(),
        disabled.config.at("render_targets").end(),
        [](const auto &target) {
            return target.value("name", std::string{}) ==
                       "picking_id" ||
                   target.value("name", std::string{}) ==
                       "picking_depth";
        }));
    REQUIRE(std::none_of(
        disabled.config.at("rendering_passes").at(0).at("passes").begin(),
        disabled.config.at("rendering_passes").at(0).at("passes").end(),
        [](const auto &entry) {
            return entry.value("name", std::string{}) ==
                   "picking_pass";
        }));
}

TEST_CASE("gizmo feature owns a purgeable output overlay pass",
          "[render-feature][gizmo][wp274]") {
    const auto enabled = composeRenderFeatureConfig(
        baseConfigWithFeature("engine://features/gizmo.json"),
        RenderFeatureComposeDependencies{loadEngineFeature, true});
    REQUIRE(enabled.feature_names ==
            std::vector<std::string>{"gizmo"});

    const auto &passes =
        enabled.config.at("rendering_passes").at(0).at("passes");
    const auto gizmo = std::find_if(
        passes.begin(), passes.end(), [](const auto &entry) {
            return entry.value("name", std::string{}) == "gizmo_pass";
        });
    const auto debug_text = std::find_if(
        passes.begin(), passes.end(), [](const auto &entry) {
            return entry.value("name", std::string{}) ==
                   "__anchor_debug_text";
        });
    REQUIRE(gizmo != passes.end());
    REQUIRE(gizmo->at("type") == "gizmo");
    REQUIRE(gizmo->at("output").at("color") == "display");
    REQUIRE(gizmo->at("color_load_op") == "load");
    REQUIRE(debug_text != passes.end());
    REQUIRE(gizmo < debug_text);

    // The overlay deliberately writes the canonical display target instead
    // of allocating a feature-private intermediate. Therefore no gizmo
    // target survives either configuration, and disabling the feature drops
    // the only gizmo pass/GPU owner.
    const auto has_gizmo_target = [](const auto &config) {
        return std::any_of(
            config.at("render_targets").begin(),
            config.at("render_targets").end(), [](const auto &target) {
                return target.value("name", std::string{})
                    .starts_with("gizmo");
            });
    };
    REQUIRE_FALSE(has_gizmo_target(enabled.config));

    auto disabled_config =
        baseConfigWithFeature("engine://features/gizmo.json");
    disabled_config["features"] = nlohmann::json::array();
    bool loader_called = false;
    const auto disabled = composeRenderFeatureConfig(
        disabled_config,
        RenderFeatureComposeDependencies{
            [&loader_called](std::string_view) {
                loader_called = true;
                return std::string{};
            },
            true,
        });
    REQUIRE_FALSE(loader_called);
    REQUIRE(disabled.feature_names.empty());
    REQUIRE_FALSE(has_gizmo_target(disabled.config));
    REQUIRE(std::none_of(
        disabled.config.at("rendering_passes")
            .at(0)
            .at("passes")
            .begin(),
        disabled.config.at("rendering_passes")
            .at(0)
            .at("passes")
            .end(),
        [](const auto &entry) {
            return entry.value("name", std::string{}) == "gizmo_pass";
        }));
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
          "shader": {"vertex": "engine://fullscreen", "fragment": "engine://fullscreen"},
          "uses_light_data": true
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
    REQUIRE(result.shader_defines ==
            std::vector<std::string>{
                "PELICAN_FEATURE_SHADOW",
                "PELICAN_FEATURE_SHADOW_DIRECTIONAL_CASCADE_COUNT=1"});
    REQUIRE(passNames(result.config) ==
            std::vector<std::string>{
                "shadow_depth", "gbuffer_pass",
                "lighting_pass"});

    const auto &shadow_map = result.config.at("render_targets").back();
    REQUIRE(shadow_map.at("name").get<std::string>() == "shadow_map");
    REQUIRE(shadow_map.at("width").get<int>() == 2048);
    REQUIRE(shadow_map.at("height").get<int>() == 2048);
    REQUIRE(shadow_map.at("layers").get<int>() == 1);
    REQUIRE(
        result.feature_instances.at(0)
            .at("parameters")
            .at("cascade_count") == 1);

    const auto &shadow_pass = passByName(result.config, "shadow_depth");
    REQUIRE(shadow_pass.at("output").at("depth").get<std::string>() == "shadow_map");
    REQUIRE(shadow_pass.at("depth_store_op").get<std::string>() == "store");

    const auto &lighting_pass = passByName(result.config, "lighting_pass");
    REQUIRE(lighting_pass.at("input").get<std::vector<std::string>>() ==
            std::vector<std::string>{"gbuffer_albedo", "shadow_map"});
    REQUIRE(lighting_pass.at("input_sampling").back() ==
            (nlohmann::json{
                {"filter", "nearest"},
                {"address", "clamp_to_edge"}}));
    REQUIRE(result.surface_resource_contracts.size() == 1);
    const auto &contract =
        result.surface_resource_contracts.front();
    REQUIRE(contract.at("contract") ==
            "directional_shadow");
    REQUIRE(contract.at("producer") == "shadow_depth");
    REQUIRE(contract.at("material_consumers").empty());
    REQUIRE(contract.at("fullscreen_consumers") ==
            nlohmann::json::array({"lighting_pass"}));

    auto cascaded_config = config;
    cascaded_config["features"] =
        nlohmann::json::array({
            {
                {"ref",
                 "engine://features/shadow_directional.json"},
                {"parameters",
                 {
                     {"cascade_count", 3},
                     {"resolution", 1024},
                     {"max_distance", 60.0},
                     {"split_lambda", 0.75},
                     {"stabilize", false},
                 }},
            },
        });
    const auto cascaded =
        composeRenderFeatureConfig(
            cascaded_config,
            RenderFeatureComposeDependencies{
                loadEngineFeature,
                true,
            });
    const auto &cascaded_target =
        cascaded.config.at(
            "render_targets").back();
    REQUIRE(
        cascaded_target.at("width") ==
        1024);
    REQUIRE(
        cascaded_target.at("height") ==
        1024);
    REQUIRE(
        cascaded_target.at("layers") ==
        3);
    REQUIRE(
        cascaded.shader_defines ==
        std::vector<std::string>{
            "PELICAN_FEATURE_SHADOW",
            "PELICAN_FEATURE_SHADOW_DIRECTIONAL_CASCADE_COUNT=3"});
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
      "render_target_overrides":{
        "$source":{
          "usage":["TRANSFER_SRC"],
          "format_candidates":["R16G16B16A16_SFLOAT"]
        }
      },
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
        REQUIRE(
            target->at("format_candidates") ==
            nlohmann::json::array(
                {"R16G16B16A16_SFLOAT"}));

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
          {"name":"enabled","type":"bool","default":true},
          {"name":"physical_only","type":"int","range":[1,32],"default":9,
           "shader_define":false}
        ]
      },
      "shader_defines":["PELICAN_FEATURE_TAA"],
      "render_targets":[{
        "name":"scalar_target",
        "extent_scale":"$alpha",
        "width":"$physical_only",
        "height":"$iterations",
        "format":"R8_UNORM",
        "format_class":"data",
        "usage":["SAMPLED"],
        "history":"$enabled",
        "layers":"$iterations"
      }]
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
        {"alpha", 0.1}, {"enabled", false}, {"iterations", 7},
        {"physical_only", 9}, {"source", "lit_color"}});
    const auto &first_target =
        first.config.at("render_targets").back();
    REQUIRE(first_target.at("extent_scale") == 0.1);
    REQUIRE(first_target.at("width") == 9);
    REQUIRE(first_target.at("height") == 7);
    REQUIRE(first_target.at("layers") == 7);
    REQUIRE(first_target.at("history") == false);

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

TEST_CASE(
    "shader asset feature parameters replace typed shader slots without "
    "copying the feature",
    "[render-feature][binding][shader-asset]") {
    const auto feature =
        nlohmann::json::parse(R"json({
          "schema":"pelican.render_feature",
          "version":1,
          "name":"replaceable_filter",
          "parameters":{
            "schema":"pelican.render_feature_parameters",
            "version":1,
            "shader_assets":[
              {
                "name":"filter_shader",
                "stage":"compute",
                "default":"engine://render_algorithms/filter/default"
              },
              {
                "name":"skinned_shader",
                "stage":"vertex",
                "default":"engine://velocity_skinned"
              }
            ]
          },
          "passes":[{
            "insert":"end",
            "pass":{
              "name":"replaceable_velocity",
              "type":"velocity",
              "output":{"color":null,"depth":null},
              "shader":{
                "vertex":"engine://velocity",
                "skinned_vertex":"$skinned_shader",
                "fragment":"engine://velocity"
              }
            }
          }],
          "compute_tasks":[{
            "name":"filter",
            "shader":"$filter_shader",
            "dispatch":{"groups":[1,1,1]}
          }]
        })json");
    const auto compose =
        [&](nlohmann::json parameters) {
            return composeRenderFeatureConfig(
                nlohmann::json{
                    {"render_targets",
                     nlohmann::json::array()},
                    {"rendering_passes",
                     nlohmann::json::array({
                         {
                             {"name", "main"},
                             {"passes",
                              nlohmann::json::array()},
                         },
                     })},
                    {"features",
                     nlohmann::json::array({
                         {
                             {"ref",
                              "replaceable_filter.json"},
                             {"parameters",
                              std::move(parameters)},
                         },
                     })},
                },
                RenderFeatureComposeDependencies{
                    [text = feature.dump()](
                        std::string_view) {
                        return text;
                    },
                    true,
                });
        };

    const auto defaults =
        compose(nlohmann::json::object());
    REQUIRE(
        computeTaskByName(
            defaults.config, "filter")
            .at("shader") ==
        "engine://render_algorithms/filter/default");
    REQUIRE(
        defaults.feature_instances.at(0)
            .at("parameters")
            .at("filter_shader") ==
        "engine://render_algorithms/filter/default");
    REQUIRE(
        passByName(
            defaults.config,
            "replaceable_velocity")
            .at("shader")
            .at("skinned_vertex") ==
        "engine://velocity_skinned");

    const auto replaced =
        compose({
            {"filter_shader",
             "project://shaders/custom_filter"},
            {"skinned_shader",
             "project://shaders/custom_skin"},
        });
    REQUIRE(
        computeTaskByName(
            replaced.config, "filter")
            .at("shader") ==
        "project://shaders/custom_filter");
    REQUIRE(
        replaced.feature_instances.at(0)
            .at("parameters")
            .at("filter_shader") ==
        "project://shaders/custom_filter");
    REQUIRE(
        passByName(
            replaced.config,
            "replaceable_velocity")
            .at("shader")
            .at("skinned_vertex") ==
        "project://shaders/custom_skin");
}

TEST_CASE(
    "shader asset parameters reject type and stage mismatches",
    "[render-feature][binding][shader-asset]") {
    const auto compose =
        [](nlohmann::json feature,
           nlohmann::json parameters =
               nlohmann::json::object()) {
            return composeRenderFeatureConfig(
                nlohmann::json{
                    {"render_targets",
                     nlohmann::json::array()},
                    {"rendering_passes",
                     nlohmann::json::array()},
                    {"features",
                     nlohmann::json::array({
                         {
                             {"ref", "asset.json"},
                             {"parameters",
                              std::move(parameters)},
                         },
                     })},
                },
                RenderFeatureComposeDependencies{
                    [text = feature.dump()](
                        std::string_view) {
                        return text;
                    },
                    true,
                });
        };
    auto feature =
        nlohmann::json::parse(R"json({
          "schema":"pelican.render_feature",
          "version":1,
          "name":"asset_errors",
          "parameters":{
            "schema":"pelican.render_feature_parameters",
            "version":1,
            "shader_assets":[{
              "name":"filter_shader",
              "stage":"fragment",
              "default":"project://shaders/filter"
            }]
          },
          "compute_tasks":[{
            "name":"filter",
            "shader":"$filter_shader",
            "dispatch":{"groups":[1,1,1]}
          }]
        })json");

    REQUIRE_THROWS_WITH(
        compose(feature),
        Catch::Matchers::ContainsSubstring(
            "declares stage 'fragment' but is used in a 'compute' slot"));
    REQUIRE_THROWS_WITH(
        compose(
            feature,
            {{"filter_shader", 7}}),
        Catch::Matchers::ContainsSubstring(
            "shader asset value must be a non-empty string reference"));

    feature["parameters"]["shader_assets"][0]
           ["stage"] = "compute";
    feature["compute_tasks"][0]["name"] =
        "$filter_shader";
    feature["compute_tasks"][0]["shader"] =
        "project://shaders/filter";
    REQUIRE_THROWS_WITH(
        compose(feature),
        Catch::Matchers::ContainsSubstring(
            "shader asset placeholder may only be used in a typed shader slot"));
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
