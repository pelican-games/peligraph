#include "../src/project/featurecompose.hpp"
#include "../src/project/graphpartition.hpp"
#include "../src/project/rendergraphcomposition.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/project/renderconfigsection.hpp"
#include "render_graph_root_test_support.hpp"
#include "lp3a_decompose_support.hpp"
#include "lp1c_decompose_support.hpp"
#include "../src/project/materialdrawloop.hpp"
#include "../src/project/materialpassdecompose.hpp"
#include "../src/project/projecttemplate.hpp"
#include "../src/project/renderfeatureoverlay.hpp"
#include "../src/project/renderpipeline.hpp"
#include "../src/project/passshapepolicy.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/renderer/viewfamilyproviderregistry.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/frameexecutionadapter.hpp"
#include "../src/core/renderingpass/graphtransformregistry.hpp"
#include "../src/core/renderingpass/renderstrategyregistry.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerpackage.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerprogramdetail.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
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

inline constexpr std::string_view
    wp309ComputeFeatureRef =
        "valid/wp309_compute_provenance.json";
inline constexpr std::string_view
    wp309GateFeatureRef =
        "valid/wp309_subgraph_gate.json";
inline constexpr std::string_view
    wp309SubgraphProvider =
        "fixture.wp309.subgraph";

struct Wp309SubgraphProviderState {
    std::string implementation =
        "fixture.wp309.subgraph@1";
    std::string replacement;
};

RenderSubgraph::Status resolveWp309Subgraph(
    void *context,
    const RenderSubgraph::ResolveRegionInputV1 *input,
    RenderSubgraph::RegionReplacementV1 *output) noexcept {
    if (context == nullptr || input == nullptr ||
        output == nullptr ||
        input->authored_subgraph_json_utf8 == nullptr) {
        return RenderSubgraph::Status::invalid_argument;
    }
    try {
        auto &state =
            *static_cast<Wp309SubgraphProviderState *>(
                context);
        auto replacement = nlohmann::json::parse(
            input->authored_subgraph_json_utf8,
            input->authored_subgraph_json_utf8 +
                input->authored_subgraph_json_size);
        if (!replacement.is_array() ||
            replacement.size() != 1 ||
            !replacement.front().contains("input") ||
            !replacement.front().at("input").is_array()) {
            return RenderSubgraph::Status::invalid_argument;
        }
        const auto &inputs =
            replacement.front().at("input");
        const auto gate_enabled = std::find(
                                      inputs.begin(),
                                      inputs.end(),
                                      "gate_marker") !=
                                  inputs.end();
        if (gate_enabled) {
            replacement.front()["name"] =
                "resolved_probe_path";
        }
        state.replacement = replacement.dump();
        *output = RenderSubgraph::descriptor<
            RenderSubgraph::RegionReplacementV1>();
        output->implementation_id_utf8 =
            state.implementation.data();
        output->implementation_id_size =
            static_cast<std::uint32_t>(
                state.implementation.size());
        output->subgraph_json_utf8 =
            state.replacement.data();
        output->subgraph_json_size =
            static_cast<std::uint32_t>(
                state.replacement.size());
        return RenderSubgraph::Status::ok;
    } catch (...) {
        return RenderSubgraph::Status::provider_error;
    }
}

RenderSubgraph::ProviderV1 wp309Provider(
    Wp309SubgraphProviderState &state) {
    auto provider = RenderSubgraph::descriptor<
        RenderSubgraph::ProviderV1>();
    provider.capability_bits =
        RenderSubgraph::builtinProviderCapabilitiesV1;
    provider.name_utf8 = wp309SubgraphProvider.data();
    provider.name_size =
        static_cast<std::uint32_t>(
            wp309SubgraphProvider.size());
    provider.context = &state;
    provider.resolve_region = resolveWp309Subgraph;
    return provider;
}

nlohmann::json wp309CompilerConfig(
    bool gate_feature_enabled) {
    // MG-2b: the compiler reads this as the authored `pelican.graph` root's
    // section, which cannot declare buffers (the graph owns them), so the
    // probe task's buffer is declared by its own feature.
    auto config = nlohmann::json::parse(R"json({
  "features": [],
  "render_targets": [
    {
      "name": "scene_in",
      "extent_scale": 1.0,
      "format": "R8G8B8A8_UNORM",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "gate_marker",
      "extent_scale": 1.0,
      "format": "R8G8B8A8_UNORM",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "scene_out",
      "extent_scale": 1.0,
      "format": "R8G8B8A8_UNORM",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    }
  ],
  "rendering_passes": [
    {
      "name": "main",
      "region_replacements": [
        {
          "region": "region.wp309.probe",
          "provider": "fixture.wp309.subgraph"
        }
      ],
      "passes": [
        {
          "name": "prepare_scene",
          "type": "fullscreen",
          "output": {"color": "scene_in", "depth": null},
          "shader": {
            "vertex": "engine://fullscreen",
            "fragment": "engine://scene_present"
          }
        },
        {
          "name": "prepare_gate",
          "type": "fullscreen",
          "output": {"color": "gate_marker", "depth": null},
          "shader": {
            "vertex": "engine://fullscreen",
            "fragment": "engine://scene_present"
          }
        },
        {
          "name": "probe#xr",
          "type": "fullscreen",
          "regions": ["region.wp309.probe"],
          "input": ["scene_in"],
          "output": {"color": "scene_out", "depth": null},
          "shader": {
            "vertex": "engine://fullscreen",
            "fragment": "engine://scene_present"
          }
        },
        {
          "name": "present",
          "type": "fullscreen",
          "input": ["scene_out"],
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "engine://fullscreen",
            "fragment": "engine://scene_present"
          }
        }
      ]
    }
  ]
})json");
    config["features"].push_back(
        wp309ComputeFeatureRef);
    if (gate_feature_enabled) {
        config["features"].push_back(
            wp309GateFeatureRef);
    }
    return config;
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
    resolved.pass_provenance = composition.pass_provenance;
    resolved.resource_provenance =
        composition.resource_provenance;
    resolved.graph_loops = composition.graph_loops;
    resolved.material_draw_loops = composition.material_draw_loops;
    resolved.used_features = composition.used_features;
    return compileRenderPipeline(resolved);
}

TEST_CASE(
    "WP300 get_frame_plan provenance follows the build compiler capability",
    "[render-feature][frame-plan][provenance][wp300]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    constexpr bool build_runtime_shader_compiler_enabled = true;
#else
    constexpr bool build_runtime_shader_compiler_enabled = false;
#endif
    const auto make_config = [](bool feature_enabled) {
        auto config = baseConfigWithFeature(
            "engine://features/provenance_probe.json");
        if (!feature_enabled) {
            config["features"] = nlohmann::json::array();
        }
        return config;
    };
    const auto resolve = [&](bool feature_enabled,
                             bool runtime_shader_compiler_enabled) {
        return resolveRenderPipeline(
            RenderPipelineRequest{
                Pelican::TestSupport::renderGraphRoot(make_config(feature_enabled)),
                "WP300 provenance capability contrast"},
            RenderEnvironmentCapabilities{
                runtime_shader_compiler_enabled,
                RenderPipelineGraphVariant::flat},
            RenderPipelineResolveDependencies{
                .load_feature_json = [](std::string_view ref) {
                    REQUIRE(std::string{ref} ==
                            "engine://features/provenance_probe.json");
                    return std::string{R"json({
  "schema": "pelican.render_feature",
  "version": 1,
  "name": "provenance_probe",
  "runtime_shader_compiler": "optional",
  "render_targets": [
    {
      "name": "provenance_target",
      "extent_scale": 1.0,
      "format": "R8_UNORM",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    }
  ],
  "buffers": [
    {"name": "provenance_buffer", "size": 16}
  ],
  "passes": [
    {
      "insert": "end",
      "pass": {
        "name": "provenance_pass",
        "type": "fullscreen",
        "output": {"color": "provenance_target", "depth": null},
        "shader": {
          "vertex": "engine://fullscreen",
          "fragment": "engine://fullscreen"
        }
      }
    }
  ]
})json"};
                },
                .resolve_render_strategy =
                    [runtime_shader_compiler_enabled](
                        const nlohmann::json &composed,
                        const CompiledGraphVariantPolicy &) {
                        auto resolved = composed;
                        resolved["rendering_passes"][0]["passes"][0]
                                ["name"] =
                            runtime_shader_compiler_enabled
                                ? "runtime_compiler_seed"
                                : "embedded_artifact_seed";
                        return resolved;
                    },
            });
    };
    const auto response = [](const ResolvedRenderPipeline &resolved) {
        const auto graphs =
            parseFrameGraphDefinitionsFromConfigJson(
                resolved.normalized_config);
        REQUIRE(graphs.size() == 1);
        const auto compiled = compileRenderPipeline(resolved);
        return framePlanToJson(
            planFrameGraph(graphs.front()), &compiled);
    };
    const auto find_named = [](const nlohmann::json &entries,
                               std::string_view name)
        -> const nlohmann::json * {
        const auto found = std::find_if(
            entries.begin(), entries.end(),
            [&](const auto &entry) {
                return entry.value("name", std::string{}) == name;
            });
        return found == entries.end() ? nullptr : &*found;
    };

    // Both runs use the build's real capability. Removing the feature is the
    // same-entry negative control required by the WP contract.
    const auto without_feature = response(resolve(
        false, build_runtime_shader_compiler_enabled));
    const auto with_feature = response(resolve(
        true, build_runtime_shader_compiler_enabled));
    const auto *feature_pass =
        find_named(with_feature.at("nodes"), "provenance_pass");
    REQUIRE(feature_pass != nullptr);
    REQUIRE(feature_pass->at("source") ==
            "feature:provenance_probe");
    REQUIRE(feature_pass->at("provider_feature") ==
            "provenance_probe");
    REQUIRE(feature_pass->at("provider_ref") ==
            "engine://features/provenance_probe.json");
    REQUIRE(find_named(without_feature.at("nodes"),
                       "provenance_pass") == nullptr);

    const auto *project_pass =
        find_named(with_feature.at("nodes"), "present");
    REQUIRE(project_pass != nullptr);
    REQUIRE(project_pass->at("source") == "project");
    const auto actual_seed =
        build_runtime_shader_compiler_enabled
            ? std::string_view{"runtime_compiler_seed"}
            : std::string_view{"embedded_artifact_seed"};
    const auto *engine_pass =
        find_named(with_feature.at("nodes"), actual_seed);
    REQUIRE(engine_pass != nullptr);
    REQUIRE(engine_pass->at("source") == "engine");
    const auto *without_project_pass =
        find_named(without_feature.at("nodes"), "present");
    REQUIRE(without_project_pass != nullptr);
    REQUIRE(without_project_pass->at("source") == "project");
    const auto *without_engine_pass =
        find_named(without_feature.at("nodes"), actual_seed);
    REQUIRE(without_engine_pass != nullptr);
    REQUIRE(without_engine_pass->at("source") == "engine");

    const auto *feature_target =
        find_named(with_feature.at("resources"),
                   "provenance_target");
    REQUIRE(feature_target != nullptr);
    REQUIRE(feature_target->at("source") ==
            "feature:provenance_probe");
    const auto *feature_buffer =
        find_named(with_feature.at("resources"),
                   "provenance_buffer");
    REQUIRE(feature_buffer != nullptr);
    REQUIRE(feature_buffer->at("kind") == "buffer");
    REQUIRE(feature_buffer->at("source") ==
            "feature:provenance_probe");
    REQUIRE(find_named(without_feature.at("resources"),
                       "provenance_target") == nullptr);
    const auto *without_project_target =
        find_named(without_feature.at("resources"), "lit_color");
    REQUIRE(without_project_target != nullptr);
    REQUIRE(without_project_target->at("source") == "project");
    const auto *without_engine_target =
        find_named(without_feature.at("resources"), "display");
    REQUIRE(without_engine_target != nullptr);
    REQUIRE(without_engine_target->at("source") == "engine");

    // On the OFF build this makes the old inference path's false premise
    // observable: a second resolve with compiler=true produces a different
    // engine node and cannot truthfully annotate the actual frame plan.
    const auto hard_coded_true = response(resolve(true, true));
    REQUIRE(find_named(hard_coded_true.at("nodes"),
                       "runtime_compiler_seed") != nullptr);
    if constexpr (!build_runtime_shader_compiler_enabled) {
        REQUIRE(find_named(hard_coded_true.at("nodes"),
                           "embedded_artifact_seed") == nullptr);
        REQUIRE(find_named(with_feature.at("nodes"),
                           "runtime_compiler_seed") == nullptr);
        REQUIRE(find_named(with_feature.at("nodes"),
                           "embedded_artifact_seed") != nullptr);
    }
}

TEST_CASE(
    "WP301 xr compute provenance survives variant namespacing",
    "[render-feature][frame-plan][provenance][xr][wp301]") {
    const auto resolve = [](RenderPipelineGraphVariant variant) {
        return resolveRenderPipeline(
            RenderPipelineRequest{
                Pelican::TestSupport::renderGraphRoot(baseConfigWithFeature(
                    "engine://features/compute_provenance_probe.json")),
                "WP301 compute provenance variant contrast"},
            RenderEnvironmentCapabilities{
                false,
                variant},
            RenderPipelineResolveDependencies{
                .load_feature_json = [](std::string_view ref) {
                    REQUIRE(
                        std::string{ref} ==
                        "engine://features/compute_provenance_probe.json");
                    return std::string{R"json({
  "schema": "pelican.render_feature",
  "version": 1,
  "name": "compute_provenance_probe",
  "runtime_shader_compiler": "optional",
  "buffers": [
    {
      "name": "compute_provenance_buffer",
      "size": 16,
      "lifetime": "persistent"
    }
  ],
  "compute_tasks": [
    {
      "name": "compute_provenance_task",
      "shader": "engine://shaders/compute/provenance_probe",
      "writes": ["compute_provenance_buffer"],
      "before": ["present"],
      "dispatch": {"groups": [1, 1, 1]}
    }
  ]
})json"};
                },
            });
    };
    struct VariantPlans {
        nlohmann::json stale_provenance;
        nlohmann::json synchronized_provenance;
        nlohmann::json normalized_config;
    };
    const auto plan_variant = [&](RenderPipelineGraphVariant variant) {
        const auto resolved = resolve(variant);
        auto compiled = compileRenderPipeline(resolved);
        const auto stale_compiled = compiled;
        std::vector<ComputeTaskDefinition> tasks{
            ComputeTaskDefinition{
                .name =
                    resolved.normalized_config
                        .at("compute_tasks")
                        .front()
                        .at("name")
                        .get<std::string>(),
            },
        };
        auto graphs =
            parseFrameGraphDefinitionsFromConfigJson(
                resolved.normalized_config);
        REQUIRE(tasks.size() == 1);
        REQUIRE(graphs.size() == 1);

        detail::namespaceComputeTasks(
            tasks, graphs, resolved.normalized_config,
            compiled);
        const auto plan = planFrameGraph(graphs.front());
        return VariantPlans{
            .stale_provenance =
                framePlanToJson(plan, &stale_compiled),
            .synchronized_provenance =
                framePlanToJson(plan, &compiled),
            .normalized_config =
                resolved.normalized_config,
        };
    };
    const auto find_named = [](const nlohmann::json &entries,
                               std::string_view name)
        -> const nlohmann::json * {
        const auto found = std::find_if(
            entries.begin(), entries.end(),
            [&](const auto &entry) {
                return entry.value("name", std::string{}) ==
                       name;
            });
        return found == entries.end() ? nullptr : &*found;
    };

    const auto flat = plan_variant(
        RenderPipelineGraphVariant::flat);
    REQUIRE(
        flat.synchronized_provenance ==
        flat.stale_provenance);
    const auto *flat_compute = find_named(
        flat.synchronized_provenance.at("nodes"),
        "compute_provenance_task");
    REQUIRE(flat_compute != nullptr);
    REQUIRE(flat_compute->at("authored_name") == "compute_provenance_task");
    REQUIRE(
        flat_compute->at("source") ==
        "feature:compute_provenance_probe");
    REQUIRE(
        flat_compute->at("provider_feature") ==
        "compute_provenance_probe");

#if PELICAN_WITH_OPENXR
    const auto xr = plan_variant(
        RenderPipelineGraphVariant::xr);
    REQUIRE(
        xr.normalized_config.at("compute_tasks")
            .front()
            .at("name") ==
        "compute_provenance_task");
    const auto *stale_xr_compute = find_named(
        xr.stale_provenance.at("nodes"),
        "compute_provenance_task#xr");
    REQUIRE(stale_xr_compute != nullptr);
    REQUIRE_FALSE(stale_xr_compute->contains("source"));
    REQUIRE_FALSE(
        stale_xr_compute->contains("provider_feature"));

    const auto *xr_compute = find_named(
        xr.synchronized_provenance.at("nodes"),
        "compute_provenance_task#xr");
    REQUIRE(xr_compute != nullptr);
    REQUIRE(xr_compute->at("authored_name") == "compute_provenance_task");
    REQUIRE(
        xr_compute->at("source") ==
        "feature:compute_provenance_probe");
    REQUIRE(
        xr_compute->at("provider_feature") ==
        "compute_provenance_probe");
    REQUIRE(
        xr_compute->at("provider_ref") ==
        "engine://features/compute_provenance_probe.json");
    REQUIRE(
        find_named(
            xr.synchronized_provenance.at("nodes"),
            "compute_provenance_task") == nullptr);
#else
    REQUIRE_THROWS_WITH(
        resolve(RenderPipelineGraphVariant::xr),
        "XR graph variant is unavailable in this build");
#endif
}

TEST_CASE(
    "WP309 runtime compiler preserves compute provenance after subgraph replacement",
    "[render-feature][render-compiler][provenance][xr][wp309]") {
    const auto request = [](
                             RenderPipelineGraphVariant
                                 variant) {
        return RenderCompilerProgramVariantRequest{
            .graph_variant = variant,
            .artifact =
                RenderCompilerProgramArtifact::
                    runtime_package,
            .compose_runtime_config =
                [](nlohmann::json &config) {
                    auto &targets =
                        config.at("render_targets");
                    const auto display = std::find_if(
                        targets.begin(), targets.end(),
                        [](const auto &target) {
                            return target.value(
                                       "name",
                                       std::string{}) ==
                                   "display";
                        });
                    if (display == targets.end()) {
                        throw std::runtime_error(
                            "WP309 fixture lacks canonical "
                            "display target");
                    }
                    (*display)["width"] = 1;
                    (*display)["height"] = 1;
                },
        };
    };
    const auto compile = [&request](
                             const nlohmann::json &config,
                             RenderPipelineGraphVariant
                                 variant,
                             const VulkanRenderCompilerBackendContext
                                 &backend) {
        PathResolver path_resolver;
        path_resolver.setup(fixtureRoot(), false);
        GraphTransformRegistry graph_registry;
        RenderStrategyRegistry strategy_registry;
        SubgraphReplacementRegistry
            subgraph_registry;
        Wp309SubgraphProviderState provider_state;
        RenderSubgraph::ProviderHandleV1 handle{};
        REQUIRE(
            subgraph_registry.registerProvider(
                wp309Provider(provider_state),
                internal::engineRegistrationOwner,
                handle) ==
            RenderSubgraph::Status::ok);

        auto graph_providers =
            graph_registry.snapshot();
        auto strategy_providers =
            strategy_registry.snapshot();
        auto subgraph_providers =
            subgraph_registry.snapshot();
        const std::array requests{
            request(variant)};
        return runRenderCompilerProgram(
            defaultVulkanRenderCompilerProgram(),
            RenderCompilerProgramInput{
                .rendering_config = Pelican::TestSupport::renderGraphRoot(config),
                .source_name =
                    "WP309 production compiler fixture",
                .path_resolver = path_resolver,
                .runtime_shader_compiler_enabled =
                    false,
                .graph_transforms =
                    graph_providers,
                .render_strategies =
                    strategy_providers,
                .subgraph_replacements =
                    subgraph_providers,
                .backend_context = backend,
                .variants = requests,
            });
    };
    const auto require_compute_provenance = [](
                                                const auto
                                                    &variant,
                                                std::string_view
                                                    graph,
                                                std::string_view
                                                    name) {
        const std::string graph_name{graph};
        const std::string node_name{name};
        REQUIRE(variant.physical_package != nullptr);
        REQUIRE(
            variant.compute_task_definitions.size() ==
            1);
        REQUIRE(
            variant.compute_task_definitions.front()
                .name == node_name);
        REQUIRE(variant.frame_plans.size() == 1);
        const auto plan = framePlanToJson(
            variant.frame_plans.at(graph_name),
            variant.compiled_pipeline.get());
        REQUIRE(plan.at("graph") == graph_name);

        const nlohmann::json *node = nullptr;
        std::size_t node_count = 0;
        for (const auto &candidate :
             plan.at("nodes")) {
            if (candidate.at("name") == node_name) {
                node = &candidate;
                ++node_count;
            }
        }
        REQUIRE(node_count == 1);
        REQUIRE(node != nullptr);
        REQUIRE(node->at("kind") == "compute");
        REQUIRE(
            node->at("source") ==
            "feature:wp309_compute_provenance");
        REQUIRE(
            node->at("provider_feature") ==
            "wp309_compute_provenance");
        REQUIRE(
            node->at("provider_ref") ==
            std::string{wp309ComputeFeatureRef});

        const RenderPassProvenance *provenance =
            nullptr;
        std::size_t provenance_count = 0;
        for (const auto &candidate :
             variant.compiled_pipeline
                 ->pass_provenance) {
            if (candidate.name == name) {
                provenance = &candidate;
                ++provenance_count;
            }
        }
        REQUIRE(provenance_count == 1);
        REQUIRE(provenance != nullptr);
        REQUIRE(
            provenance->source ==
            RenderPipelineProvenanceSource::feature);
        REQUIRE(
            provenance->provider_feature ==
            "wp309_compute_provenance");
        REQUIRE(
            provenance->provider_reference ==
            std::string{wp309ComputeFeatureRef});
        return plan;
    };
    const auto has_node = [](
                              const nlohmann::json &plan,
                              std::string_view name,
                              std::string_view kind) {
        const std::string node_name{name};
        const std::string node_kind{kind};
        return std::count_if(
                   plan.at("nodes").begin(),
                   plan.at("nodes").end(),
                   [&](const auto &node) {
                       return node.at("name") == node_name &&
                              node.at("kind") == node_kind;
                   }) == 1;
    };
    const auto find_node = [](
                               const nlohmann::json &plan,
                               std::string_view name,
                               std::string_view kind)
        -> const nlohmann::json * {
        const std::string node_name{name};
        const std::string node_kind{kind};
        const auto found = std::find_if(
            plan.at("nodes").begin(),
            plan.at("nodes").end(),
            [&](const auto &node) {
                return node.at("name") == node_name &&
                       node.at("kind") == node_kind;
            });
        return found == plan.at("nodes").end()
                   ? nullptr
                   : &*found;
    };
    const auto provenance_count = [](
                                      const auto &variant,
                                      std::string_view name) {
        return std::count_if(
            variant.compiled_pipeline
                ->pass_provenance.begin(),
            variant.compiled_pipeline
                ->pass_provenance.end(),
            [&](const auto &provenance) {
                return provenance.name == name;
            });
    };

    const VulkanRenderCompilerBackendContext
        default_backend;
    REQUIRE(
        default_backend.device_planning_mode ==
        VulkanRenderCompilerDevicePlanningMode::
            device_required);
    const VulkanRenderCompilerBackendContext
        compiler_only_backend{
            vk::Format::eUndefined,
            {},
            {},
            {},
            {},
            VulkanRenderCompilerDevicePlanningMode::
                compiler_only};
    const VulkanRenderCompilerBackendContext
        compiler_only_with_device_backend{
            vk::Format::eUndefined,
            {},
            vk::PhysicalDevice{
                reinterpret_cast<VkPhysicalDevice>(
                    std::uintptr_t{1})},
            {},
            {},
            VulkanRenderCompilerDevicePlanningMode::
                compiler_only};

    REQUIRE_THROWS_WITH(
        compile(
            wp309CompilerConfig(true),
            RenderPipelineGraphVariant::flat,
            default_backend),
        "Vulkan render compiler "
        "device_planning_mode=device_required "
        "requires physical_device");
    REQUIRE_THROWS_WITH(
        compile(
            wp309CompilerConfig(true),
            RenderPipelineGraphVariant::flat,
            compiler_only_with_device_backend),
        "Vulkan render compiler "
        "device_planning_mode=compiler_only "
        "forbids physical_device");

    const auto flat_with_feature = compile(
        wp309CompilerConfig(true),
        RenderPipelineGraphVariant::flat,
        compiler_only_backend);
    REQUIRE(flat_with_feature.variants.size() == 1);
    const auto &flat_variant =
        flat_with_feature.variants.front();
    REQUIRE(
        flat_variant.compiled_pipeline
            ->feature_names ==
        std::vector<std::string>{
            "wp309_compute_provenance",
            "wp309_subgraph_gate"});
    const auto flat_plan =
        require_compute_provenance(
            flat_variant, "main", "probe");
    REQUIRE(has_node(
        flat_plan, "resolved_probe_path", "render"));
    REQUIRE_FALSE(has_node(
        flat_plan, "probe#xr", "render"));
    const auto *resolved_probe_path = find_node(
        flat_plan, "resolved_probe_path", "render");
    REQUIRE(resolved_probe_path != nullptr);
    REQUIRE(
        resolved_probe_path->value(
            "source", std::string{}) == "engine");
    REQUIRE(
        provenance_count(
            flat_variant,
            "resolved_probe_path") == 1);
    REQUIRE(
        provenance_count(flat_variant, "probe#xr") ==
        0);

    const auto flat_without_gate_feature = compile(
        wp309CompilerConfig(false),
        RenderPipelineGraphVariant::flat,
        compiler_only_backend);
    REQUIRE(
        flat_without_gate_feature.variants.size() ==
        1);
    const auto &flat_without_gate_variant =
        flat_without_gate_feature.variants.front();
    REQUIRE(
        flat_without_gate_variant.compiled_pipeline
            ->feature_names ==
        std::vector<std::string>{
            "wp309_compute_provenance"});
    const auto flat_without_gate_plan =
        require_compute_provenance(
            flat_without_gate_variant,
            "main", "probe");
    REQUIRE(has_node(
        flat_without_gate_plan,
        "probe#xr", "render"));
    const auto *authored_probe_path = find_node(
        flat_without_gate_plan, "probe#xr", "render");
    REQUIRE(authored_probe_path != nullptr);
    REQUIRE(
        authored_probe_path->value(
            "source", std::string{}) == "project");
    REQUIRE(
        provenance_count(
            flat_without_gate_variant,
            "probe#xr") == 1);

#if PELICAN_WITH_OPENXR
    const auto xr_with_feature = compile(
        wp309CompilerConfig(true),
        RenderPipelineGraphVariant::xr,
        compiler_only_backend);
    REQUIRE(xr_with_feature.variants.size() == 1);
    const auto &xr_variant =
        xr_with_feature.variants.front();
    const auto xr_plan =
        require_compute_provenance(
            xr_variant, "main#xr", "probe#xr");
    REQUIRE(has_node(
        xr_plan, "resolved_probe_path", "render"));
    REQUIRE_FALSE(has_node(
        xr_plan, "probe", "compute"));

    REQUIRE_THROWS_WITH(
        compile(
            wp309CompilerConfig(false),
            RenderPipelineGraphVariant::xr,
            compiler_only_backend),
        "compute task variant name collision in graph "
        "'main#xr': probe#xr");
#else
    REQUIRE_THROWS_WITH(
        compile(
            wp309CompilerConfig(true),
            RenderPipelineGraphVariant::xr,
            compiler_only_backend),
        "XR graph variant is unavailable in this build");
#endif
}

TEST_CASE("punctual shadow features wire graph resources into lighting",
          "[render-feature][punctual-shadow]") {
#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
    const auto combined = composeRenderFeatureConfig(
        nlohmann::json{
            {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
            {"features", nlohmann::json::array({"engine://features/shadow_point.json",
                                               "engine://features/shadow_spot.json"})}},
        RenderFeatureComposeDependencies{loadEngineFeature, true});
    CHECK(passByName(combined.config, "point_shadow_depth").at("view_family") == "$shadow/point");
    CHECK(passByName(combined.config, "spot_shadow_depth").at("view_family") == "$shadow/spot");
    const auto compiled = compileComposition(combined);
    CHECK(compiled.surface_resource_contracts.size() == 2);
    for (const auto kind : {std::string{"point"}, std::string{"spot"}}) {
        const auto composed = composeRenderFeatureConfig(
            nlohmann::json{
                {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
                {"features", nlohmann::json::array({"engine://features/shadow_" + kind + ".json"})}},
            RenderFeatureComposeDependencies{loadEngineFeature, true});
        const auto &ports = passByName(composed.config, "deferred_lighting").at("resource_ports");
        CHECK(ports.at(kind + "_shadow_texture").at("view") == "family_array");
        CHECK(ports.at(kind + "_shadow_matrices").at("element") == "uvec4");
        CHECK(passByName(composed.config, "forward_opaque").at("material_resources").contains(kind + "_shadow_matrices"));
    }
#endif
}

TEST_CASE("Materialized shadow features preserve declarations and runtime parameters",
          "[render-feature][materialize-feature]") {
    using Json = nlohmann::json;
    const Json root{{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}};
    const auto base = composeRenderFeatureConfig(root, RenderFeatureComposeDependencies{loadEngineFeature, true});
    for (const auto &kind : {std::string{"directional"}, std::string{"point"}, std::string{"spot"}}) {
#if !PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
        if (kind != "directional") continue;
#endif
        INFO(kind);
        const auto reference = "engine://features/shadow_" + kind + ".json";
        const auto authored = Json::parse(loadEngineFeature(reference));
        const Json parameters{{"resolution", 512}, {"pcf_radius", 2}, {"bias", 0.01}, {"strength", 0.8}};
        auto original_root = root;
        original_root["features"] = Json::array({{{"ref", reference}, {"parameters", parameters}}});
        const auto original = composeRenderFeatureConfig(original_root, RenderFeatureComposeDependencies{loadEngineFeature, true});
        auto document = Json::parse(materializeRenderFeatureDocument(authored, parameters, base.config).dump());
        // P-2a-1c: the derivative keeps the scalar declarations it stays
        // editable through, and the value it was cut with lives in
        // bound_parameters instead of being frozen into the physical fields.
        CHECK(document.at("parameters").at("scalars") == authored.at("parameters").at("scalars"));
        CHECK_FALSE(document.at("parameters").contains("render_targets"));
        CHECK(document.at("bound_parameters").at("resolution") == 512);
        CHECK(document.at("render_targets").at(0).at("width") == "$resolution");
        auto materialized_root = root;
        materialized_root["features"] = Json::array({"project://features/editable_shadow.json"});
        const auto compose = [&] {
            return composeRenderFeatureConfig(materialized_root, RenderFeatureComposeDependencies{
                [&](std::string_view ref) {
                    return ref == "project://features/editable_shadow.json" ? document.dump() : loadEngineFeature(ref);
                }, true});
        };
        const auto materialized = compose();
        CHECK(materialized.config == original.config);
        CHECK(materialized.shader_defines == original.shader_defines);
        REQUIRE_FALSE(original.feature_instances.empty());
        REQUIRE_FALSE(materialized.feature_instances.empty());
        CHECK(materialized.feature_instances.back().at("parameters") == original.feature_instances.back().at("parameters"));
        // Direct node edits survive re-composition instead of being rebound to
        // the old resolution parameter recorded for the runtime provider.
        document["render_targets"][0]["width"] = 256;
        document["render_targets"][0]["height"] = 256;
        const auto edited = compose();
        const auto name = document.at("render_targets").at(0).at("name");
        const auto &targets = edited.config.at("render_targets");
        const auto target = std::find_if(targets.begin(), targets.end(), [&](const auto &value) { return value.at("name") == name; });
        REQUIRE(target != targets.end());
        CHECK(target->at("width") == 256);
        // P-2a-1c: an instance override on a materialized feature is accepted
        // and wins over the value it was cut with.  Before 1c this threw.
        auto edited_parameters = parameters;
        edited_parameters["resolution"] = 128;
        materialized_root["features"] =
            Json::array({{{"ref", "project://features/editable_shadow.json"}, {"parameters", edited_parameters}}});
        const auto overridden = compose();
        REQUIRE_FALSE(overridden.feature_instances.empty());
        CHECK(overridden.feature_instances.back().at("parameters").at("resolution") == 128);
        // Materializing a derivative again is still refused: the RPC offers it
        // only for a document that has no bound_parameters.
        CHECK_THROWS(materializeRenderFeatureDocument(document, parameters, base.config));
    }
}

TEST_CASE("shadow filter policy comes from feature parameters",
          "[render-feature][shadow-filter]") {
    for (const auto kind : {std::string{"directional"}, std::string{"point"}, std::string{"spot"}}) {
#if !PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
        if (kind != "directional") continue;
#endif
        const auto composed = composeRenderFeatureConfig(
            nlohmann::json{
                {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
                {"features", nlohmann::json::array({{
                    {"ref", "engine://features/shadow_" + kind + ".json"},
                    {"parameters", {{"pcf_radius", 2}, {"bias", 0.01}, {"strength", 0.0}}}}})}},
            RenderFeatureComposeDependencies{loadEngineFeature, true});
        std::string prefix = "PELICAN_FEATURE_SHADOW_" + kind + "_";
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        CHECK(std::find(composed.shader_defines.begin(), composed.shader_defines.end(), prefix + "PCF_RADIUS=2") != composed.shader_defines.end());
        const auto &parameters = composed.feature_instances.back().at("parameters");
        CHECK(parameters.at("bias") == 0.01);
        CHECK(parameters.at("strength") == 0.0);
    }
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

TEST_CASE("startup feature overlay is explicit, additive, and purgeable",
          "[render-feature][overlay][editor][wp289][wp351]") {
    // P-2a-4b: the shipping project is a `pelican.graph` root, so this is
    // the first e2e that puts an overlay on the shipping document; the
    // feature list it must edit lives in the `rendering` section.
    const auto base = readJson(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
        "example" / "passes" / "main_rendering_config.json");
    REQUIRE(base.at("schema") == "pelican.graph");
    const std::vector<std::string> no_overlays;
    bool loader_called = false;
    const auto unchanged = applyRenderFeatureOverlays(
        base, no_overlays,
        [&loader_called](std::string_view) {
            loader_called = true;
            return std::string{};
        });
    REQUIRE_FALSE(loader_called);
    REQUIRE(unchanged == base);

    const std::vector<std::string> overlays{
        std::string{editorFeatureOverlayReference}};
    const auto editor_overlay = nlohmann::json::parse(
        loadEngineFeature(editorFeatureOverlayReference));
    REQUIRE(editor_overlay.at("features") ==
            nlohmann::json::array(
                {"engine://features/gizmo.json",
                 "engine://features/picking.json",
                 "engine://features/gpu_timing.json"}));

    auto editor_before_gpu_timing = editor_overlay;
    auto &features_before_gpu_timing =
        editor_before_gpu_timing.at("features");
    const auto gpu_timing = std::find(
        features_before_gpu_timing.begin(),
        features_before_gpu_timing.end(),
        nlohmann::json("engine://features/gpu_timing.json"));
    REQUIRE(gpu_timing != features_before_gpu_timing.end());
    features_before_gpu_timing.erase(gpu_timing);

    const auto apply_editor = [&](const nlohmann::json &overlay) {
        return applyRenderFeatureOverlays(
            base, overlays,
            [&](std::string_view reference) {
                REQUIRE(std::string{reference} ==
                        std::string{editorFeatureOverlayReference});
                return overlay.dump();
            });
    };
    const auto before_gpu_timing =
        apply_editor(editor_before_gpu_timing);
    const auto effective = apply_editor(editor_overlay);
    REQUIRE(renderingSectionOf(base).at("features") ==
            nlohmann::json::array(
                {"engine://features/bloom.json",
                 "engine://features/ui.json"}));
    REQUIRE(renderingSectionOf(effective).at("features") ==
            nlohmann::json::array(
                {"engine://features/bloom.json",
                 "engine://features/ui.json",
                 "engine://features/gizmo.json",
                 "engine://features/picking.json",
                 "engine://features/gpu_timing.json"}));

    const auto baseline = composeRenderFeatureConfig(
        base, RenderFeatureComposeDependencies{loadEngineFeature, true});
    const auto enabled = composeRenderFeatureConfig(
        effective,
        RenderFeatureComposeDependencies{loadEngineFeature, true});
    REQUIRE(baseline.feature_names ==
            std::vector<std::string>{"bloom", "ui"});
    REQUIRE(enabled.feature_names ==
            std::vector<std::string>{"bloom", "ui", "gizmo", "picking",
                                     "gpu_timing"});

    struct FramePlanTopology {
        std::set<std::tuple<std::string, std::string>> nodes;
        std::set<std::tuple<std::string, std::string, std::string,
                            std::string>> edges;
    };
    const auto topology = [](const nlohmann::json &config) {
        const auto composition = composeRenderFeatureConfig(
            config,
            RenderFeatureComposeDependencies{loadEngineFeature, true});
        const auto graphs =
            parseFrameGraphDefinitionsFromConfigJson(composition.config);
        REQUIRE(graphs.size() == 1);
        const auto plan = planFrameGraph(graphs.front());
        const auto execution = compileFrameExecutionPlan(
            graphs.front(), plan,
            ExecutionEndpoint{
                .id = "device:0",
                .endpoint_class = ExecutionEndpointClass::device,
                .backend = "vulkan",
            });
        FramePlanTopology result;
        for (const auto &node : plan.nodes) {
            result.nodes.emplace(node.name,
                                 framePlanNodeKindName(node.kind));
        }
        for (const auto &edge : execution.dependencies) {
            result.edges.emplace(edge.from, edge.to, edge.reason,
                                 edge.resource);
        }
        return result;
    };

    const auto authored_topology = topology(base);
    const auto before_gpu_timing_topology = topology(before_gpu_timing);
    const auto with_gpu_timing_topology = topology(effective);
    // Negative controls: path-owning editor features must be visible to both
    // set comparisons.  The marker-only gpu_timing feature must be invisible.
    REQUIRE(authored_topology.nodes != before_gpu_timing_topology.nodes);
    REQUIRE(authored_topology.edges != before_gpu_timing_topology.edges);
    REQUIRE(with_gpu_timing_topology.nodes ==
            before_gpu_timing_topology.nodes);
    REQUIRE(with_gpu_timing_topology.edges ==
            before_gpu_timing_topology.edges);

    const auto has_pass = [](const nlohmann::json &config,
                             std::string_view name) {
        for (const auto &pass_set : config.at("rendering_passes")) {
            for (const auto &pass : pass_set.at("passes")) {
                if (pass.value("name", std::string{}) == name) {
                    return true;
                }
            }
        }
        return false;
    };
    const auto has_target = [](const nlohmann::json &config,
                               std::string_view name) {
        return std::any_of(
            config.at("render_targets").begin(),
            config.at("render_targets").end(),
            [name](const auto &target) {
                return target.value("name", std::string{}) == name;
            });
    };
    REQUIRE_FALSE(has_pass(baseline.config, "gizmo_pass"));
    REQUIRE_FALSE(has_pass(baseline.config, "picking_pass"));
    REQUIRE_FALSE(has_target(baseline.config, "picking_id"));
    REQUIRE(has_pass(enabled.config, "gizmo_pass"));
    REQUIRE(has_pass(enabled.config, "picking_pass"));
    REQUIRE(has_target(enabled.config, "picking_id"));

    auto project_already_uses_gizmo = base;
    renderingSectionOf(project_already_uses_gizmo)
        .at("features")
        .push_back("engine://features/gizmo.json");
    const auto deduplicated = applyRenderFeatureOverlays(
        project_already_uses_gizmo, overlays, loadEngineFeature);
    const auto &deduplicated_features =
        renderingSectionOf(deduplicated).at("features");
    REQUIRE(std::count(
                deduplicated_features.begin(),
                deduplicated_features.end(),
                nlohmann::json("engine://features/gizmo.json")) == 1);
}

TEST_CASE("P-2a-4a a pelican.graph root composes like its legacy twin and takes overlays in its section",
          "[render-feature][graph-root][overlay][p2a4a]") {
    const auto source_root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
    const auto graph_root =
        readJson(source_root / "test" / "fixtures" / "graph_root_example" /
                 "passes" / "main.json");
    const auto shipping = readJson(source_root / "projects" / "example" /
                                   "passes" / "main_rendering_config.json");
    // P-2a-4b: the shipping project is itself a `pelican.graph` root now, so
    // the fixture's definition is "the same section, wrapped by the same one
    // production function". Comparing the section alone would not catch a
    // fixture whose root keys drifted, so compare the wrap too.
    REQUIRE(graph_root.at("schema") == "pelican.graph");
    REQUIRE(shipping.at("schema") == "pelican.graph");
    REQUIRE(graph_root.at("rendering") == renderingSectionOf(shipping));
    REQUIRE(graph_root ==
            Pelican::TestSupport::renderGraphRoot(renderingSectionOf(shipping)));
    const auto legacy = renderingSectionOf(shipping);

    struct Topology {
        std::set<std::tuple<std::string, std::string>> nodes;
        std::set<std::tuple<std::string, std::string, std::string,
                            std::string>> edges;
    };
    const auto compose = [](const nlohmann::json &document) {
        return composeRenderFeatureConfig(
            document,
            RenderFeatureComposeDependencies{loadEngineFeature, true});
    };
    const auto topology = [](const RenderFeatureComposeResult &composition) {
        const auto graphs =
            parseFrameGraphDefinitionsFromConfigJson(composition.config);
        REQUIRE(graphs.size() == 1);
        const auto plan = planFrameGraph(graphs.front());
        const auto execution = compileFrameExecutionPlan(
            graphs.front(), plan,
            ExecutionEndpoint{
                .id = "device:0",
                .endpoint_class = ExecutionEndpointClass::device,
                .backend = "vulkan",
            });
        Topology result;
        for (const auto &node : plan.nodes)
            result.nodes.emplace(node.name, framePlanNodeKindName(node.kind));
        for (const auto &edge : execution.dependencies)
            result.edges.emplace(edge.from, edge.to, edge.reason, edge.resource);
        return result;
    };

    const auto legacy_composition = compose(legacy);
    const auto graph_composition = compose(graph_root);
    const auto passNames = [](const RenderFeatureComposeResult &composition) {
        std::vector<std::string> names;
        for (const auto &family : composition.config.at("rendering_passes"))
            for (const auto &pass : family.at("passes"))
                names.push_back(pass.value("name", std::string{}));
        return names;
    };
    const auto targetNames = [](const RenderFeatureComposeResult &composition) {
        std::vector<std::string> names;
        for (const auto &target : composition.config.at("render_targets"))
            names.push_back(target.value("name", std::string{}));
        return names;
    };
    REQUIRE(graph_composition.feature_names ==
            legacy_composition.feature_names);
    REQUIRE(graph_composition.feature_names ==
            std::vector<std::string>{"bloom", "ui"});
    REQUIRE(passNames(graph_composition) == passNames(legacy_composition));
    REQUIRE(targetNames(graph_composition) == targetNames(legacy_composition));
    // The graph lowering adds its own (empty) buffer and compute_task arrays;
    // everything the frame is built from is the same document.
    auto graph_config = graph_composition.config;
    auto legacy_config = legacy_composition.config;
    for (const auto *key : {"buffers", "compute_tasks"}) {
        if (graph_config.value(key, nlohmann::json::array()).empty())
            graph_config.erase(key);
        if (legacy_config.value(key, nlohmann::json::array()).empty())
            legacy_config.erase(key);
    }
    REQUIRE(graph_config == legacy_config);
    const auto legacy_topology = topology(legacy_composition);
    REQUIRE(topology(graph_composition).nodes == legacy_topology.nodes);
    REQUIRE(topology(graph_composition).edges == legacy_topology.edges);

    // A preset under a graph root resolves the same way (no test existed, and
    // P-2a-4b moves a preset project first).
    const auto preset_graph_root =
        readJson(source_root / "test" / "fixtures" / "graph_root_example" /
                 "passes" / "preset_main.json");
    // MG-2b: the animgraph demo is itself a graph root now; its section is
    // the flat config this compares against.
    const auto preset_shipping = readJson(source_root / "projects" /
                                          "animgraph_demo" / "passes" / "main.json");
    const auto preset_legacy = renderingSectionOf(preset_shipping);
    REQUIRE(preset_graph_root.at("rendering") == preset_legacy);
    const auto preset_legacy_composition = compose(preset_legacy);
    const auto preset_graph_composition = compose(preset_graph_root);
    REQUIRE(preset_graph_composition.feature_names ==
            preset_legacy_composition.feature_names);
    REQUIRE(passNames(preset_graph_composition) ==
            passNames(preset_legacy_composition));
    REQUIRE(targetNames(preset_graph_composition) ==
            targetNames(preset_legacy_composition));
    REQUIRE(preset_graph_composition.pipeline_preset.has_value());
    REQUIRE(preset_graph_composition.pipeline_preset->reference ==
            preset_legacy_composition.pipeline_preset->reference);

    // The startup overlay lands in the section the composer reads.  Adding it
    // at the root is exactly the silent loss this work removes.
    const std::vector<std::string> overlays{
        std::string{editorFeatureOverlayReference}};
    const auto overlaid =
        applyRenderFeatureOverlays(graph_root, overlays, loadEngineFeature);
    REQUIRE_FALSE(overlaid.contains("features"));
    REQUIRE(overlaid.at("rendering").at("features") ==
            nlohmann::json::array({"engine://features/bloom.json",
                                   "engine://features/ui.json",
                                   "engine://features/gizmo.json",
                                   "engine://features/picking.json",
                                   "engine://features/gpu_timing.json"}));
    REQUIRE(compose(overlaid).feature_names ==
            std::vector<std::string>{"bloom", "ui", "gizmo", "picking",
                                     "gpu_timing"});
    // Everything else in the document is untouched.
    auto without_features = overlaid;
    without_features.at("rendering").at("features") =
        graph_root.at("rendering").at("features");
    REQUIRE(without_features == graph_root);

    // A graph root with no place to put them says so by name.
    auto sectionless = graph_root;
    sectionless.erase("rendering");
    REQUIRE_THROWS_WITH(
        applyRenderFeatureOverlays(sectionless, overlays, loadEngineFeature),
        Catch::Matchers::ContainsSubstring(
            "render_config.section_unlocated@1"));
    sectionless["rendering"] = 7;
    REQUIRE_THROWS_WITH(
        applyRenderFeatureOverlays(sectionless, overlays, loadEngineFeature),
        Catch::Matchers::ContainsSubstring(
            "render_config.section_unlocated@1"));
}

TEST_CASE("startup feature overlay accepts only its strict v1 envelope",
          "[render-feature][overlay][format][wp289]") {
    const auto base = Pelican::TestSupport::renderGraphRoot(
        nlohmann::json{{"features", nlohmann::json::array()}});
    const std::vector<std::string> overlays{"project://overlay.json"};
    const auto apply = [&](std::string bytes) {
        return applyRenderFeatureOverlays(
            base, overlays,
            [bytes = std::move(bytes)](std::string_view) {
                return bytes;
            });
    };

    REQUIRE_THROWS_WITH(
        apply(R"json({"schema":"pelican.render_feature_overlay","version":2,"name":"tool","features":[]})json"),
        Catch::Matchers::ContainsSubstring("version must be exactly 1"));
    REQUIRE_THROWS_WITH(
        apply(R"json({"schema":"pelican.render_feature_overlay","version":1,"name":"tool","features":[],"project":true})json"),
        Catch::Matchers::ContainsSubstring("unknown key 'project'"));
    REQUIRE_THROWS_WITH(
        apply(R"json({"schema":"pelican.render_feature_overlay","version":1,"name":"tool","features":[""]})json"),
        Catch::Matchers::ContainsSubstring("empty feature reference"));
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
    auto document = readJson(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                             "projects/example/passes/main_rendering_config.json");
    // P-2a-4b: edit the shipping document through the section locator; the
    // composer then takes the graph root exactly as the engine does.
    auto &config = renderingSectionOf(document);
    // TAA's named binding intentionally requires the dynamic scene format
    // class. The example itself pins float16 because its forward material ABI
    // consumes scene-linear HDR without enabling the optional HDR feature.
    auto &authored_targets = config.at("render_targets");
    const auto lit_color = std::find_if(
        authored_targets.begin(), authored_targets.end(), [](const auto &target) {
            return target.value("name", std::string{}) == "lit_color";
        });
    REQUIRE(lit_color != authored_targets.end());
    (*lit_color)["format_class"] = "scene";
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
        document, RenderFeatureComposeDependencies{loadEngineFeature, true});
    // P-2a-1b: the example's own bloom is the first feature it composes.
    REQUIRE(result.feature_names ==
            std::vector<std::string>{"bloom", "ui", "velocity", "taa"});
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
            nlohmann::json::array(
                {"DEPTH_STENCIL_ATTACHMENT", "TRANSFER_SRC", "SAMPLED"}));
    const auto accum = std::find_if(targets.begin(), targets.end(), [](const auto &target) {
        return target.value("name", std::string{}) == "taa_accum";
    });
    REQUIRE(accum != targets.end());
    REQUIRE(accum->at("history") == true);

    // P-2a-1c: the example pipeline's bloom is a feature with declared scalars,
    // so its defines are in force alongside TAA's.  THRESHOLD prints in
    // std::to_chars general form, which is why the bloom shaders wrap the
    // define in float().  P-2a-2: `intensity` is `binding: buffer`, so it emits
    // no define at all; its value reaches the composite through the
    // `bloom.intensity` buffer instead.
    REQUIRE(result.shader_defines == std::vector<std::string>{
        "PELICAN_FEATURE_BLOOM_THRESHOLD=0.400000006",
        "PELICAN_FEATURE_BLOOM_KNEE=0.699999988",
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
    for (const auto *required_define : {"PELICAN_FEATURE_SHADOW",
             "PELICAN_FEATURE_SHADOW_DIRECTIONAL_CASCADE_COUNT=1"})
        REQUIRE(std::find(result.shader_defines.begin(), result.shader_defines.end(),
                          required_define) != result.shader_defines.end());
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
    for (const auto *required_define : {"PELICAN_FEATURE_SHADOW",
             "PELICAN_FEATURE_SHADOW_DIRECTIONAL_CASCADE_COUNT=3"})
        REQUIRE(std::find(cascaded.shader_defines.begin(), cascaded.shader_defines.end(),
                          required_define) != cascaded.shader_defines.end());
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

TEST_CASE("binding buffer scalars seed the feature's runtime_f32 buffer and refuse a define",
          "[render-feature][binding][scalar-parameter][p2a2]") {
    // P-2a-2. The feature declares one single-float runtime_f32 buffer named
    // `<feature>.<scalar>`; the scalar is its start value and `render.scalar`
    // overwrites it every frame.
    const auto makeFeature = [](nlohmann::json scalar, nlohmann::json buffers) {
        return nlohmann::json{
            {"schema", "pelican.render_feature"},
            {"version", 1},
            {"name", "scalar_buffer"},
            {"parameters", {
                {"schema", "pelican.render_feature_parameters"},
                {"version", 1},
                {"scalars", nlohmann::json::array({std::move(scalar)})},
            }},
            {"buffers", std::move(buffers)},
        };
    };
    const auto compose = [&](const nlohmann::json &feature, nlohmann::json values) {
        auto config = baseConfigWithFeature("scalar_buffer.json");
        config["features"] = nlohmann::json::array({nlohmann::json{
            {"ref", "scalar_buffer.json"}, {"parameters", std::move(values)}}});
        return composeRenderFeatureConfig(
            config, RenderFeatureComposeDependencies{
                        [text = feature.dump()](std::string_view) { return text; }, true});
    };
    const nlohmann::json declared_buffer = nlohmann::json::array({
        {{"name", "scalar_buffer.level"}, {"size", 4},
         {"lifetime", "persistent"}, {"host_source", "runtime_f32"}}});
    const nlohmann::json scalar{{"name", "level"}, {"type", "float"},
                                {"range", {0.0, 8.0}}, {"default", 2.0},
                                {"binding", "buffer"}};
    const auto bufferByName = [](const nlohmann::json &config, std::string_view name) {
        const auto &buffers = config.at("buffers");
        const auto found = std::find_if(buffers.begin(), buffers.end(), [&](const auto &entry) {
            return entry.value("name", std::string{}) == name;
        });
        REQUIRE(found != buffers.end());
        return *found;
    };

    const auto defaults = compose(makeFeature(scalar, declared_buffer), nlohmann::json::object());
    // The declared default is the buffer's start value, and nothing is baked
    // into a shader define: that is the whole point of `binding: buffer`.
    CHECK(bufferByName(defaults.config, "scalar_buffer.level").at("initial_f32") ==
          nlohmann::json::array({2.0}));
    CHECK(defaults.shader_defines.empty());

    // What `set_parameters` writes: the instance value replaces the start value.
    const auto overridden = compose(makeFeature(scalar, declared_buffer), {{"level", 0.25}});
    CHECK(bufferByName(overridden.config, "scalar_buffer.level").at("initial_f32") ==
          nlohmann::json::array({0.25}));

    // The derivative keeps the binding and the buffer declaration, and does not
    // freeze the value into the buffer: compose regenerates it from
    // bound_parameters.
    const auto materialized = nlohmann::json::parse(
        materializeRenderFeatureDocument(makeFeature(scalar, declared_buffer),
                                         {{"level", 0.25}}, defaults.config)
            .dump());
    CHECK(materialized.at("parameters").at("scalars").at(0).at("binding") == "buffer");
    CHECK(materialized.at("buffers").at(0).at("name") == "scalar_buffer.level");
    CHECK_FALSE(materialized.at("buffers").at(0).contains("initial_f32"));
    CHECK(materialized.at("bound_parameters").at("level") == 0.25);
    auto derived_config = baseConfigWithFeature("scalar_buffer.json");
    derived_config["features"] = nlohmann::json::array({"scalar_buffer.json"});
    const auto recomposed = composeRenderFeatureConfig(
        derived_config, RenderFeatureComposeDependencies{
            [text = materialized.dump()](std::string_view) { return text; }, true});
    CHECK(bufferByName(recomposed.config, "scalar_buffer.level").at("initial_f32") ==
          nlohmann::json::array({0.25}));

    const auto requireError = [&](const nlohmann::json &feature, std::string_view detail) {
        try {
            (void)compose(feature, nlohmann::json::object());
            FAIL("expected a named rejection");
        } catch (const std::exception &error) {
            const std::string message = error.what();
            REQUIRE(contains(message, "scalar_buffer"));
            REQUIRE(contains(message, detail));
        }
    };
    auto with_define = scalar;
    with_define["shader_define"] = true;
    requireError(makeFeature(with_define, declared_buffer),
                 "binding 'buffer' cannot also emit a shader define");
    auto unknown_binding = scalar;
    unknown_binding["binding"] = "somewhere";
    requireError(makeFeature(unknown_binding, declared_buffer),
                 "unknown scalar binding: somewhere");
    auto integral = scalar;
    integral["type"] = "int";
    integral["range"] = nlohmann::json::array({0, 8});
    integral["default"] = 2;
    requireError(makeFeature(integral, declared_buffer),
                 "binding 'buffer' requires scalar type float");
    requireError(makeFeature(scalar, nlohmann::json::array()),
                 "requires the feature to declare a buffer named 'scalar_buffer.level'");
    auto wrong_source = declared_buffer;
    wrong_source[0]["host_source"] = "scene_lights_v2";
    wrong_source[0]["size"] = 65568;
    requireError(makeFeature(scalar, wrong_source),
                 "host_source 'runtime_f32' and size 4");
    auto authored_initial = declared_buffer;
    authored_initial[0]["initial_f32"] = nlohmann::json::array({1.0});
    requireError(makeFeature(scalar, authored_initial),
                 "initial_f32 is derived from the scalar, not authored");

    // The frame-graph parser is the second gate: an initial_f32 that does not
    // fill the declared buffer is refused there, by name.
    auto mismatched = defaults.config;
    for (auto &entry : mismatched.at("buffers")) {
        if (entry.value("name", std::string{}) != "scalar_buffer.level") continue;
        entry["initial_f32"] = nlohmann::json::array({1.0, 2.0});
    }
    REQUIRE_THROWS_WITH(parseFrameGraphBufferDefinitionsFromJson(mismatched),
        Catch::Matchers::ContainsSubstring("one float per declared byte quad"));
}

TEST_CASE("common graph buffer ports lower into native compute declarations", "[render-feature][graph-composition]") {
    auto document = nlohmann::json::parse(R"({
      "schema":"pelican.graph","version":1,
      "definitions":{"kernel":{"nodes":[
        {"id":"data","op":"render.buffer","parameters":{"size":16,"element":"float","lifetime":"persistent"},"outputs":["resource"]},
        {"id":"run","op":"render.compute","parameters":{
          "shader":"project://shaders/custom","dispatch":{"groups":[1,1,1]},
          "ports":{"values":{"access":"write","element":"float"}}},
          "inputs":{"values":{"node":"data","port":"resource"}},"outputs":["values"]}],
        "outputs":{"data":{"node":"run","port":"values"}}}},
      "nodes":[{"id":"consume","op":"render.compute","parameters":{
                 "shader":"project://shaders/consume","dispatch":{"groups":[1,1,1]},
                 "ports":{"source":{"access":"read","element":"float"}}},
                 "inputs":{"source":{"node":"left","port":"data"}},"outputs":[]},
               {"id":"left","op":"graph.instance","definition":"kernel"},
               {"id":"right","op":"graph.instance","definition":"kernel"}],
      "outputs":{"data":{"node":"left","port":"data"}},
      "rendering":{"render_targets":[],"rendering_passes":[]}
    })");
    const auto original = document;
    const auto resolved = resolveRenderPipeline(RenderPipelineRequest{document}, {});
    REQUIRE(document == original);
    const auto &config = resolved.normalized_config;
    const auto buffers = parseFrameGraphBufferDefinitionsFromJson(config);
    const auto tasks = parseComputeTaskDefinitionsFromConfigJson(config);
    REQUIRE(buffers.size() == 2);
    REQUIRE(tasks.size() == 3);
    REQUIRE_NOTHROW(validateComputeTaskBufferContracts(buffers, tasks));
    REQUIRE(config.at("buffers")[0].at("name") == "left/data");
    REQUIRE(config.at("buffers")[1].at("name") == "right/data");
    REQUIRE(computeTaskByName(config, "left/run").at("writes") == nlohmann::json::array({"left/data"}));
    REQUIRE(computeTaskByName(config, "right/run").at("resource_ports").at("values").at("resource") == "right/data");
    REQUIRE(computeTaskByName(config, "left/run").at("shader") == "project://shaders/custom");
    auto planning = config;
    planning["compute_tasks"] = nlohmann::json::array({
        computeTaskByName(config, "consume"), computeTaskByName(config, "right/run"),
        computeTaskByName(config, "left/run")});
    const auto order = framePlanOrder(planFrameGraph(parseFrameGraphDefinitionFromJson(planning)));
    REQUIRE(order.size() == 3);
    REQUIRE(std::find(order.begin(), order.end(), "left/run") <
            std::find(order.begin(), order.end(), "consume"));
    REQUIRE_NOTHROW(compileRenderPipeline(resolved));
    // Change the shared definition's storage type without changing its shader
    // input contract: matching names alone must not accept the connection.
    document["definitions"]["kernel"]["nodes"][0]["parameters"]["element"] = "vec4";
    REQUIRE_THROWS_AS(resolveRenderPipeline(RenderPipelineRequest{document}, {}), std::invalid_argument);
    document = original;
    document["definitions"]["kernel"]["nodes"][1]["parameters"]["reads"] = {"hidden"};
    REQUIRE_THROWS_AS(resolveRenderPipeline(RenderPipelineRequest{document}, {}), std::invalid_argument);
}

TEST_CASE("attribute sources resolve instances without changing authored definitions", "[render-feature][graph-attribute]") {
    using Json = nlohmann::json;
    const auto document = Json::parse(R"({"schema":"pelican.graph","version":1,
      "definitions":{"mesh":{"nodes":[{"id":"position","op":"render.attribute",
        "parameters":{"element":"vec3","domain":"vertex","source":{
          "asset":"project://character.glb","mesh":0,"primitive":0,"semantic":"POSITION"}},
        "outputs":["resource"]}],"outputs":{"vertices":{"node":"position","port":"resource"}}}},
      "nodes":[{"id":"left","op":"graph.instance","definition":"mesh"},
               {"id":"right","op":"graph.instance","definition":"mesh"}],
      "outputs":{"positions":{"node":"left","port":"vertices"}}})");
    std::set<std::string> resolved;
    const auto compiled = resolveGraphAttributeSources(document, [&](std::string_view id, const Json &parameters) {
        resolved.insert(std::string{id});
        REQUIRE(parameters.at("source").at("semantic") == "POSITION");
        return 4;
    });
    REQUIRE(resolved == std::set<std::string>{"left/position", "right/position"});
    const auto config = lowerRenderGraphDocument(compiled);
    REQUIRE(config.at("buffers").size() == 2);
    REQUIRE(config.at("buffers")[0].at("size") == 64);
    REQUIRE(config.at("buffers")[1].at("size") == 64);
    REQUIRE(document.at("definitions").at("mesh").at("nodes")[0].at("parameters").contains("source"));
    REQUIRE_FALSE(document.at("definitions").at("mesh").at("nodes")[0].at("parameters").contains("count"));
    REQUIRE_THROWS(lowerRenderGraphDocument(document));
    REQUIRE_THROWS(resolveGraphAttributeSources(document, [](auto, const auto &) { return 0; }));
    auto bad = document;
    bad["definitions"]["mesh"]["nodes"][0]["parameters"]["count"] = 4;
    REQUIRE_THROWS(partitionGraphDocument(bad));
    bad = document;
    bad["definitions"]["mesh"]["nodes"][0]["parameters"]["source"]["mesh"] = -1;
    REQUIRE_THROWS(partitionGraphDocument(bad));
    bad = document;
    bad["definitions"]["unused"] = bad["definitions"]["mesh"];
    bad["definitions"]["unused"]["nodes"][0]["parameters"]["source"]["asset"] = "missing.glb";
    REQUIRE_NOTHROW(resolveGraphAttributeSources(bad, [](auto, const auto &parameters) {
        REQUIRE(parameters.at("source").at("asset") == "project://character.glb");
        return 4;
    }));
}

TEST_CASE("common graphics ports retain shaders and order attachment consumers", "[render-feature][graph-composition]") {
    using Json = nlohmann::json;
    const Json type{{"semantic", "pelican.render.color_signal@1"},
        {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
    Json document{{"schema", "pelican.graph"}, {"version", 1},
        {"nodes", Json::array({
            {{"id", "data"}, {"op", "render.buffer"},
             {"parameters", {{"size", 4}, {"element", "float"}, {"host_source", "runtime_f32"}}}, {"outputs", {"resource"}}},
            {{"id", "image"}, {"op", "render.image"},
             {"parameters", {{"type", type}, {"format", "R32G32B32A32_SFLOAT"}, {"extent_scale", 1.0}}}, {"outputs", {"resource"}}},
            {{"id", "draw"}, {"op", "render.fullscreen"},
             {"parameters", {{"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "project://custom"}}},
                 {"color_load_op", "dont_care"},
                 {"ports", {{"source", {{"element", "float"}, {"access", "read"}}},
                            {"color", {{"kind", "image"}, {"type", type}, {"access", "write"},
                                       {"attachment", "color"}, {"location", 0}}}}}}},
             {"inputs", {{"source", {{"node", "data"}, {"port", "resource"}}},
                         {"color", {{"node", "image"}, {"port", "resource"}}}}}, {"outputs", {"color"}}},
            {{"id", "consume"}, {"op", "render.compute"},
             {"parameters", {{"shader", "project://consume"}, {"dispatch", {{"groups", {1, 1, 1}}}},
                 {"ports", {{"source", {{"kind", "image"}, {"type", type}, {"access", "read"}, {"binding", "sampled"}}}}}}},
             {"inputs", {{"source", {{"node", "draw"}, {"port", "color"}}}}}, {"outputs", Json::array()}}
        })}, {"outputs", {{"color", {{"node", "draw"}, {"port", "color"}}}}},
        {"rendering", {{"render_targets", Json::array()}, {"rendering_passes", Json::array()}}}};
    std::uint64_t expected_size = 4;
    const auto verify = [&]() {
        const auto resolved = resolveRenderPipeline(RenderPipelineRequest{document}, {});
        const auto &config = resolved.normalized_config;
        const auto &pass = config.at("rendering_passes").at(0).at("passes").at(0);
        REQUIRE(config.at("buffers").at(0).at("size") == expected_size);
        if (expected_size == 48) REQUIRE(pass.at("draw").at("vertex_count") == 2);
        if (expected_size == 24) {
            REQUIRE(pass.at("draw").at("operation") == "indexed");
            REQUIRE(pass.at("draw").at("index_buffer") == "data");
            REQUIRE(pass.at("draw").at("index_count") == 3);
            REQUIRE(pass.at("draw").at("first_index") == 3);
            REQUIRE(config.at("buffers").at(0).at("index_buffer") == true);
        }
        REQUIRE(pass.at("name") == "draw");
        REQUIRE(pass.at("shader").at("fragment") == "project://custom");
        REQUIRE(pass.at("output").at("color") == Json::array({"image"}));
        REQUIRE(pass.at("resource_ports").at("source").at("resource") == "data");
        REQUIRE(computeTaskByName(config, "consume").at("after") == Json::array({"draw"}));
        auto planning = config;
        planning["passes"] = config.at("rendering_passes").at(0).at("passes");
        const auto order = framePlanOrder(planFrameGraph(parseFrameGraphDefinitionFromJson(planning)));
        REQUIRE(std::find(order.begin(), order.end(), "draw") < std::find(order.begin(), order.end(), "consume"));
        REQUIRE_NOTHROW(compileRenderPipeline(resolved));
        auto invalid = document;
        invalid["nodes"][2]["parameters"]["ports"]["color"]["location"] = 1;
        REQUIRE_THROWS(resolveRenderPipeline(RenderPipelineRequest{invalid}, {}));
        invalid = document;
        invalid["nodes"][2]["parameters"]["output"] = {{"color", "hidden"}};
        REQUIRE_THROWS(resolveRenderPipeline(RenderPipelineRequest{invalid}, {}));
        invalid = document;
        invalid["nodes"][2]["inputs"]["color"] = {{"node", "data"}, {"port", "resource"}};
        REQUIRE_THROWS(resolveRenderPipeline(RenderPipelineRequest{invalid}, {}));
    };
    SECTION("index attribute determines indexed draw range") {
        expected_size = 24;
        document["nodes"][0]["op"] = "render.attribute";
        document["nodes"][0]["parameters"] = {{"element", "uint"}, {"domain", "index"}, {"count", 6}};
        document["nodes"][2]["parameters"]["ports"]["source"] = {
            {"element", "uint"}, {"domain", "index"}, {"access", "read"}};
        document["nodes"][2]["op"] = "render.raster";
        document["nodes"][2]["parameters"]["draw"] = {{"index_source", "source"}, {"first_index", 3}};
        for (const auto key : {"index_count", "vertex_count", "vertex_source", "index_buffer"}) {
            auto invalid = document;
            invalid["nodes"][2]["parameters"]["draw"][key] = 3;
            REQUIRE_THROWS(resolveRenderPipeline(RenderPipelineRequest{invalid}, {}));
        }
        auto invalid = document;
        invalid["nodes"][2]["parameters"]["draw"]["first_index"] = 7;
        REQUIRE_THROWS(resolveRenderPipeline(RenderPipelineRequest{invalid}, {}));
        verify();
    }
    SECTION("fullscreen") { verify(); }
    SECTION("raster") {
        document["nodes"][2]["op"] = "render.raster";
        document["nodes"][2]["parameters"]["draw"] = {{"vertex_count", 3}};
        verify();
    }
    SECTION("instance attribute") {
        document["nodes"][0]["op"] = "render.attribute";
        document["nodes"][0]["parameters"]["domain"] = "instance";
        document["nodes"][0]["parameters"].erase("size");
        document["nodes"][0]["parameters"]["count"] = 1;
        document["nodes"][2]["parameters"]["ports"]["source"]["domain"] = "instance";
        auto mismatched = document;
        mismatched["nodes"][2]["parameters"]["ports"]["source"]["domain"] = "vertex";
        REQUIRE_THROWS(resolveRenderPipeline(RenderPipelineRequest{mismatched}, {}));
        mismatched["nodes"][2]["parameters"]["ports"]["source"].erase("domain");
        REQUIRE_THROWS(resolveRenderPipeline(RenderPipelineRequest{mismatched}, {}));
        mismatched = document;
        mismatched["nodes"][0]["parameters"]["domain"] = "unknown";
        REQUIRE_THROWS(resolveRenderPipeline(RenderPipelineRequest{mismatched}, {}));
        mismatched = document;
        mismatched["nodes"][0]["parameters"]["count"] = 0;
        REQUIRE_THROWS(resolveRenderPipeline(RenderPipelineRequest{mismatched}, {}));
        mismatched["nodes"][0]["parameters"]["count"] = -1;
        REQUIRE_THROWS(resolveRenderPipeline(RenderPipelineRequest{mismatched}, {}));
        verify();
    }
    SECTION("vertex attribute determines draw range") {
        expected_size = 48;
        document["nodes"][0]["op"] = "render.attribute";
        document["nodes"][0]["parameters"].erase("size");
        document["nodes"][0]["parameters"]["element"] = "vec3";
        document["nodes"][0]["parameters"]["domain"] = "vertex";
        document["nodes"][0]["parameters"]["count"] = 3;
        document["nodes"][2]["parameters"]["ports"]["source"]["domain"] = "vertex";
        document["nodes"][2]["parameters"]["ports"]["source"]["element"] = "vec3";
        document["nodes"][2]["op"] = "render.raster";
        document["nodes"][2]["parameters"]["draw"] = {{"vertex_source", "source"}, {"first_vertex", 1}};
        auto invalid_count = document;
        invalid_count["nodes"][2]["parameters"]["draw"]["vertex_count"] = 2;
        REQUIRE_THROWS(resolveRenderPipeline(RenderPipelineRequest{invalid_count}, {}));
        verify();
    }
}

// LP-Va: a graph render node carries view_family and view_select in its
// parameters, and lowering copies them to the pass it becomes, so graph
// authors (and LP-Vb) select views the same way the rendering section does.
TEST_CASE("graph render nodes lower view_select onto their pass",
          "[render-feature][graph-composition][view-select][lpva]") {
    using Json = nlohmann::json;
    const auto document = Json::parse(R"({"schema":"pelican.graph","version":1,
      "definitions":{},
      "nodes":[{"id":"probe","op":"render.compute","parameters":{
                 "shader":"project://shaders/probe","dispatch":{"groups":[1,1,1]},
                 "ports":{},"view_family":"$capture/cube",
                 "view_select":{"face":2}},"outputs":[]}],
      "outputs":{}})");
    const auto config = lowerRenderGraphDocument(document);
    const auto &task = computeTaskByName(config, "probe");
    CHECK(task.at("view_family") == "$capture/cube");
    CHECK(task.at("view_select") == Json{{"face", 2}});
    const auto tasks = parseComputeTaskDefinitionsFromConfigJson(config);
    REQUIRE(tasks.size() == 1);
    CHECK(tasks.front().view_select == RenderViewSelect{{"face", 2}});
    const auto graph = parseFrameGraphDefinitionFromJson(config);
    REQUIRE(graph.nodes.size() == 1);
    CHECK(graph.nodes.front().view_select == RenderViewSelect{{"face", 2}});
}

namespace {
// LP-0a: a graph over the hybrid preset with bloom. `lit` refers to the
// preset's lit_color, `copy` is an image the graph owns, and `blit` copies the
// one into the other with engine://passthrough. `paint` writes bloom's
// threshold target through the reference `threshold`.
nlohmann::json lp0aSceneColorType() {
    return {{"semantic", "pelican.render.color_signal@1"},
            {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
}

nlohmann::json lp0aTargetRefGraph() {
    using Json = nlohmann::json;
    const auto type = lp0aSceneColorType();
    const Json read{{"kind", "image"}, {"type", type}, {"access", "read"}};
    const Json color{{"kind", "image"}, {"type", type}, {"access", "write"},
                     {"attachment", "color"}, {"location", 0}};
    const Json shader{{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}};
    return Json{{"schema", "pelican.graph"}, {"version", 1}, {"definitions", Json::object()},
        {"nodes", Json::array({
            {{"id", "lit"}, {"op", "render.target_ref"},
             {"parameters", {{"name", "lit_color"}, {"type", type}}}, {"outputs", {"resource"}}},
            {{"id", "copy"}, {"op", "render.image"},
             {"parameters", {{"type", type}, {"format", "R16G16B16A16_SFLOAT"}, {"extent_scale", 1.0},
                             {"usage", {"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"}}}},
             {"outputs", {"resource"}}},
            {{"id", "blit"}, {"op", "render.fullscreen"},
             {"parameters", {{"pass_set", "main_render"}, {"shader", shader},
                             {"ports", {{"source", read}, {"color", color}}}}},
             {"inputs", {{"source", {{"node", "lit"}, {"port", "resource"}}},
                         {"color", {{"node", "copy"}, {"port", "resource"}}}}},
             {"outputs", {"color"}}},
            {{"id", "threshold"}, {"op", "render.target_ref"},
             {"parameters", {{"name", "Bloom_Threshold_RT"}, {"type", type}}}, {"outputs", {"resource"}}},
            {{"id", "paint"}, {"op", "render.fullscreen"},
             {"parameters", {{"pass_set", "main_render"}, {"shader", shader},
                             {"ports", {{"color", color}}}}},
             {"inputs", {{"color", {{"node", "threshold"}, {"port", "resource"}}}}},
             {"outputs", {"color"}}},
        })},
        {"outputs", Json::object()},
        {"rendering", {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
                       {"features", Json::array({"engine://features/bloom.json"})}}}};
}

RenderFeatureComposeResult lp0aCompose(const nlohmann::json &document) {
    return composeRenderFeatureConfig(document, RenderFeatureComposeDependencies{loadEngineFeature, true});
}

// The code and fields of the graph refusal that composing `document` raises.
std::pair<std::string, nlohmann::json> lp0aRefusal(const nlohmann::json &document) {
    try {
        (void)lp0aCompose(document);
    } catch (const GraphRegistryError &error) {
        return {error.code(), error.detail()};
    }
    FAIL("composition was not refused by name");
    return {};
}
} // namespace

TEST_CASE("LP-0a render.target_ref reads and writes targets declared outside the graph",
          "[render-feature][graph-composition][lp0a]") {
    using Json = nlohmann::json;
    const auto document = lp0aTargetRefGraph();

    SECTION("a reference lowers to the target's own name and adds no target") {
        const auto composed = lp0aCompose(document);
        const auto &config = composed.config;
        const auto &blit = passByName(config, "blit");
        CHECK(blit.at("input") == Json::array({"lit_color"}));
        CHECK(blit.at("resource_ports").at("source").at("resource") == "lit_color");
        CHECK(blit.at("output").at("color") == Json::array({"copy"}));
        CHECK(passByName(config, "paint").at("output").at("color") == Json::array({"Bloom_Threshold_RT"}));
        // Connected straight to a reference, the pass waits for no graph node.
        INFO(blit.dump());
        for (const auto &name : blit.value("after", Json::array())) {
            CHECK(name != "lit");
            CHECK(name != "copy");
        }
        std::size_t lit_targets = 0;
        for (const auto &target : config.at("render_targets")) {
            CHECK(target.at("name") != "lit");
            CHECK(target.at("name") != "threshold");
            if (target.at("name") == "lit_color") ++lit_targets;
        }
        CHECK(lit_targets == 1);
        // The Frame Plan keeps whoever declared a referenced target as its source.
        const auto compiled = compileComposition(composed);
        const auto graphs = parseFrameGraphDefinitionsFromConfigJson(config);
        const auto graph = std::find_if(graphs.begin(), graphs.end(),
            [](const auto &candidate) { return candidate.name == "main_render"; });
        REQUIRE(graph != graphs.end());
        const auto plan = planFrameGraph(*graph);
        const auto plan_json = framePlanToJson(plan, &compiled);
        const auto source = [&](std::string_view name) {
            for (const auto &resource : plan_json.at("resources"))
                if (resource.at("name").get<std::string>() == name) return resource.at("source").get<std::string>();
            FAIL("no frame plan resource " + std::string{name});
            return std::string{};
        };
        CHECK(source("lit_color") == "engine");
        CHECK(source("Bloom_Threshold_RT") == "feature:bloom");
        const auto order = framePlanOrder(plan);
        const auto position = [&](std::string_view name) {
            return std::find(order.begin(), order.end(), name) - order.begin();
        };
        // The copy reads lit_color after the preset's last write of it.
        CHECK(position("forward_transparent") < position("blit"));
    }

    SECTION("an unknown name is refused after composition, not when edited") {
        auto unknown = document;
        unknown["nodes"][0]["parameters"]["name"] = "no_such_target";
        // Editing and loading check only the shape.
        REQUIRE_NOTHROW(partitionGraphDocument(unknown));
        const auto [code, fields] = lp0aRefusal(unknown);
        CHECK(code == "graph.target_ref_unknown@1");
        CHECK(fields == Json{{"node", "lit"}, {"name", "no_such_target"}});
        // Bloom's targets exist only because bloom is composed.
        auto without_bloom = document;
        without_bloom["rendering"]["features"] = Json::array();
        const auto [bloom_code, bloom_fields] = lp0aRefusal(without_bloom);
        CHECK(bloom_code == "graph.target_ref_unknown@1");
        CHECK(bloom_fields == Json{{"node", "threshold"}, {"name", "Bloom_Threshold_RT"}});
    }

    SECTION("the shape is checked when edited") {
        const Json buffer_type{{"semantic", "pelican.render.storage_buffer@1"},
                               {"arguments", {{"element", "float"}}}};
        for (const auto &parameters : {Json{{"name", ""}, {"type", lp0aSceneColorType()}},
                                       Json{{"type", lp0aSceneColorType()}},
                                       Json{{"name", "lit_color@history"}, {"type", lp0aSceneColorType()}},
                                       Json{{"name", "lit_color"}},
                                       Json{{"name", "lit_color"}, {"type", lp0aSceneColorType()},
                                            {"format", "D32_SFLOAT"}},
                                       Json{{"name", "lit_color"}, {"type", buffer_type}}}) {
            INFO(parameters.dump());
            auto invalid = document;
            invalid["nodes"][0]["parameters"] = parameters;
            CHECK_THROWS(partitionGraphDocument(invalid));
        }
    }

    SECTION("a declared type that contradicts the target is refused by name") {
        const Json depth{{"semantic", "pelican.render.depth@1"},
                         {"arguments", {{"representation", "device"}, {"space", "projection"}}}};
        auto mismatched = document;
        mismatched["nodes"][0]["parameters"]["type"] = depth;
        mismatched["nodes"][2]["parameters"]["ports"]["source"]["type"] = depth;
        REQUIRE_NOTHROW(partitionGraphDocument(mismatched));
        auto [code, fields] = lp0aRefusal(mismatched);
        CHECK(code == "graph.target_ref_type_mismatch@1");
        CHECK(fields == Json{{"node", "lit"}, {"name", "lit_color"}, {"field", "kind"},
                             {"declared", "depth"}, {"actual", "color"}});
        auto cube = lp0aSceneColorType();
        cube["arguments"]["topology"] = "cube";
        mismatched = document;
        mismatched["nodes"][0]["parameters"]["type"] = cube;
        mismatched["nodes"][2]["parameters"]["ports"]["source"]["type"] = cube;
        std::tie(code, fields) = lp0aRefusal(mismatched);
        CHECK(code == "graph.target_ref_type_mismatch@1");
        CHECK(fields == Json{{"node", "lit"}, {"name", "lit_color"}, {"field", "topology"},
                             {"declared", "cube"}, {"actual", "two_d"}});
        // A depth reference to a depth target passes.
        auto depth_ref = document;
        depth_ref["nodes"][0]["parameters"] = {{"name", "scene_depth"}, {"type", depth}};
        depth_ref["nodes"][2]["parameters"]["ports"]["source"]["type"] = depth;
        const auto composed = lp0aCompose(depth_ref);
        CHECK(passByName(composed.config, "blit").at("input") == Json::array({"scene_depth"}));
    }
}

TEST_CASE("LP-0a a port reads a target's history and never writes it",
          "[render-feature][graph-composition][lp0a]") {
    using Json = nlohmann::json;
    const auto type = lp0aSceneColorType();
    const Json target_base{{"extent_scale", 1.0}, {"format", "R16G16B16A16_SFLOAT"},
                           {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
                           {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}};
    auto trail = target_base;
    trail["name"] = "trail";
    trail["history"] = true;
    auto plain = target_base;
    plain["name"] = "plain";
    auto resolved = target_base;
    resolved["name"] = "resolved";
    // The rendering section declares the targets; the graph refers to them.
    Json document{{"schema", "pelican.graph"}, {"version", 1}, {"definitions", Json::object()},
        {"nodes", Json::array({
            {{"id", "previous"}, {"op", "render.target_ref"},
             {"parameters", {{"name", "trail"}, {"type", type}}}, {"outputs", {"resource"}}},
            {{"id", "out"}, {"op", "render.target_ref"},
             {"parameters", {{"name", "resolved"}, {"type", type}}}, {"outputs", {"resource"}}},
            {{"id", "resolve"}, {"op", "render.fullscreen"},
             {"parameters", {{"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}},
                 {"ports", {{"source", {{"kind", "image"}, {"type", type}, {"access", "read"}, {"history", true}}},
                            {"color", {{"kind", "image"}, {"type", type}, {"access", "write"},
                                       {"attachment", "color"}, {"location", 0}}}}}}},
             {"inputs", {{"source", {{"node", "previous"}, {"port", "resource"}}},
                         {"color", {{"node", "out"}, {"port", "resource"}}}}},
             {"outputs", {"color"}}}})},
        {"outputs", Json::object()},
        {"rendering", {{"render_targets", Json::array({trail, plain, resolved})},
                       {"rendering_passes", Json::array()}}}};

    SECTION("the read names the history resource") {
        const auto composed = lp0aCompose(document);
        const auto &pass = passByName(composed.config, "resolve");
        CHECK(pass.at("input") == Json::array({"trail@history"}));
        CHECK(pass.at("resource_ports").at("source").at("resource") == "trail@history");
        CHECK_FALSE(pass.at("resource_ports").at("source").contains("history"));
        REQUIRE_NOTHROW(compileComposition(composed));
    }
    SECTION("a target without history is refused by name") {
        auto without = document;
        without["nodes"][0]["parameters"]["name"] = "plain";
        const auto [code, fields] = lp0aRefusal(without);
        CHECK(code == "graph.history_unavailable@1");
        CHECK(fields == Json{{"node", "resolve"}, {"port", "source"}, {"target", "plain"}});
    }
    SECTION("a port that writes history is refused when edited") {
        auto writing = document;
        writing["nodes"][2]["parameters"]["ports"]["color"]["history"] = true;
        try {
            (void)partitionGraphDocument(writing);
            FAIL("a history write was accepted");
        } catch (const GraphRegistryError &error) {
            CHECK(error.code() == "graph.history_write@1");
            CHECK(error.detail() == Json{{"node", "resolve"}, {"port", "color"}});
        }
        writing = document;
        writing["nodes"][2]["parameters"]["ports"]["source"]["access"] = "read_write";
        writing["nodes"][2]["outputs"] = {"color", "source"};
        CHECK_THROWS_AS(partitionGraphDocument(writing), GraphRegistryError);
        writing = document;
        writing["nodes"][2]["parameters"]["ports"]["source"]["history"] = "yes";
        CHECK_THROWS(partitionGraphDocument(writing));
    }
}

namespace {
// LP-0b: graph passes over the hybrid preset with bloom. `lit` refers to the
// preset's lit_color; each pass in `chain` reads the previous image and writes
// its own, so the passes depend on one another in the order given.
nlohmann::json lp0bPass(const std::string &id, const std::string &source, const std::string &image,
                        const nlohmann::json &placement) {
    using Json = nlohmann::json;
    const auto type = lp0aSceneColorType();
    Json parameters{{"pass_set", "main_render"},
                    {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}},
                    {"ports", {{"source", {{"kind", "image"}, {"type", type}, {"access", "read"}}},
                               {"color", {{"kind", "image"}, {"type", type}, {"access", "write"},
                                          {"attachment", "color"}, {"location", 0}}}}}};
    for (const auto &[key, value] : placement.items()) parameters[key] = value;
    return Json{{"id", id}, {"op", "render.fullscreen"}, {"parameters", std::move(parameters)},
                {"inputs", {{"source", {{"node", source}, {"port", source == "lit" ? "resource" : "color"}}},
                            {"color", {{"node", image}, {"port", "resource"}}}}},
                {"outputs", {"color"}}};
}

nlohmann::json lp0bImage(const std::string &id) {
    return {{"id", id}, {"op", "render.image"},
            {"parameters", {{"type", lp0aSceneColorType()}, {"format", "R16G16B16A16_SFLOAT"},
                            {"extent_scale", 1.0}, {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}},
            {"outputs", {"resource"}}};
}

// `chain` lists pass ids in dependency order; each gets `placement`.
nlohmann::json lp0bGraph(const std::vector<std::string> &chain, const nlohmann::json &placement) {
    using Json = nlohmann::json;
    Json nodes = Json::array({{{"id", "lit"}, {"op", "render.target_ref"},
                               {"parameters", {{"name", "lit_color"}, {"type", lp0aSceneColorType()}}},
                               {"outputs", {"resource"}}}});
    std::string source = "lit";
    for (const auto &id : chain) {
        nodes.push_back(lp0bImage(id + "_image"));
        nodes.push_back(lp0bPass(id, source, id + "_image", placement));
        source = id;
    }
    return Json{{"schema", "pelican.graph"}, {"version", 1}, {"definitions", Json::object()},
                {"nodes", std::move(nodes)}, {"outputs", Json::object()},
                {"rendering", {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
                               {"features", Json::array({"engine://features/bloom.json"})}}}};
}

// Every pass of the first family, canonical anchors included, in declaration order.
std::vector<std::string> lp0bDeclared(const nlohmann::json &config) {
    std::vector<std::string> names;
    for (const auto &pass : config.at("rendering_passes").at(0).at("passes"))
        names.push_back(pass.at("name").get<std::string>());
    return names;
}

std::ptrdiff_t lp0bIndex(const std::vector<std::string> &names, std::string_view name) {
    const auto found = std::find(names.begin(), names.end(), name);
    REQUIRE(found != names.end());
    return found - names.begin();
}

std::vector<std::string> lp0bPlanOrder(const RenderFeatureComposeResult &composed) {
    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(composed.config);
    const auto graph = std::find_if(graphs.begin(), graphs.end(),
        [](const auto &candidate) { return candidate.name == "main_render"; });
    REQUIRE(graph != graphs.end());
    return framePlanOrder(planFrameGraph(*graph));
}

bool lp0bListed(const nlohmann::json &list, const std::string &name) {
    return list.is_array() && std::find(list.begin(), list.end(), name) != list.end();
}

std::pair<std::string, nlohmann::json> lp0bEditRefusal(const nlohmann::json &document) {
    try {
        (void)partitionGraphDocument(document);
    } catch (const GraphRegistryError &error) {
        return {error.code(), error.detail()};
    }
    FAIL("the edit was not refused by name");
    return {};
}
} // namespace

TEST_CASE("LP-0b a graph pass lands right after or before the pass its insert names",
          "[render-feature][graph-composition][lp0b]") {
    using Json = nlohmann::json;
    const auto placed = lp0aCompose(lp0bGraph({"blit"}, {{"insert", {{"after", "deferred_lighting"}}}}));
    const auto declared = lp0bDeclared(placed.config);
    CHECK(lp0bIndex(declared, "blit") == lp0bIndex(declared, "deferred_lighting") + 1);
    const auto &blit = passByName(placed.config, "blit");
    CHECK(lp0bListed(blit.at("after"), "deferred_lighting"));
    CHECK_FALSE(blit.contains("insert"));
    CHECK_FALSE(blit.contains("canonical_anchor"));
    const auto order = lp0bPlanOrder(placed);
    CHECK(lp0bIndex(order, "blit") == lp0bIndex(order, "deferred_lighting") + 1);
    REQUIRE_NOTHROW(compileComposition(placed));

    // Control: without insert the pass is appended to its family as before,
    // and the frame plan runs it after the scene's other passes.
    const auto appended = lp0aCompose(lp0bGraph({"blit"}, Json::object()));
    const auto appended_declared = lp0bDeclared(appended.config);
    CHECK(lp0bIndex(appended_declared, "blit") > lp0bIndex(appended_declared, "forward_transparent"));
    const auto appended_order = lp0bPlanOrder(appended);
    CHECK(lp0bIndex(appended_order, "blit") > lp0bIndex(appended_order, "forward_transparent"));
    // The placed pass keeps the provenance an appended graph pass has.
    const auto provenance = [](const RenderFeatureComposeResult &result) {
        const auto found = std::find_if(result.pass_provenance.begin(), result.pass_provenance.end(),
                                        [](const auto &entry) { return entry.name == "blit"; });
        REQUIRE(found != result.pass_provenance.end());
        return *found;
    };
    CHECK(provenance(placed) == provenance(appended));

    SECTION("before") {
        const auto before = lp0aCompose(lp0bGraph({"blit"}, {{"insert", {{"before", "forward_opaque"}}}}));
        const auto names = lp0bDeclared(before.config);
        CHECK(lp0bIndex(names, "blit") + 1 == lp0bIndex(names, "forward_opaque"));
        CHECK(lp0bListed(passByName(before.config, "blit").at("before"), "forward_opaque"));
    }
    SECTION("a feature's pass") {
        const auto feature = lp0aCompose(lp0bGraph({"blit"}, {{"insert", {{"after", "HighLuminanceExtraction"}}}}));
        const auto names = lp0bDeclared(feature.config);
        CHECK(lp0bIndex(names, "blit") == lp0bIndex(names, "HighLuminanceExtraction") + 1);
    }
}

TEST_CASE("LP-0b canonical_anchor puts a graph pass in the anchor's bucket after the features' passes",
          "[render-feature][graph-composition][lp0b]") {
    const auto composed = lp0aCompose(lp0bGraph({"blit"}, {{"canonical_anchor", "post_ldr"}}));
    const auto names = lp0bDeclared(composed.config);
    const auto blit = lp0bIndex(names, "blit");
    CHECK(blit > lp0bIndex(names, "__anchor_post_ldr"));
    // The end of the bucket: after bloom, which the feature put after post_ldr.
    CHECK(blit > lp0bIndex(names, "HighLuminanceExtraction"));
    CHECK(blit + 1 == lp0bIndex(names, "__anchor_pelican_ui"));
    const auto &pass = passByName(composed.config, "blit");
    CHECK(lp0bListed(pass.at("after"), "__anchor_post_ldr"));
    CHECK_FALSE(pass.contains("canonical_anchor"));
    REQUIRE_NOTHROW(compileComposition(composed));
}

TEST_CASE("LP-0b graph passes at one anchor keep the graph's dependency order",
          "[render-feature][graph-composition][lp0b]") {
    const nlohmann::json placement{{"insert", {{"after", "deferred_lighting"}}}};
    const auto expect_run = [&](const std::vector<std::string> &chain) {
        INFO(chain.front());
        const auto composed = lp0aCompose(lp0bGraph(chain, placement));
        const auto names = lp0bDeclared(composed.config);
        const auto anchor = lp0bIndex(names, "deferred_lighting");
        for (std::size_t i = 0; i < chain.size(); ++i)
            CHECK(lp0bIndex(names, chain[i]) == anchor + 1 + static_cast<std::ptrdiff_t>(i));
        // The first waits on the anchor and each later pass on the one before it.
        CHECK(lp0bListed(passByName(composed.config, chain[0]).at("after"), "deferred_lighting"));
        for (std::size_t i = 1; i < chain.size(); ++i)
            CHECK(lp0bListed(passByName(composed.config, chain[i]).at("after"), chain[i - 1]));
        const auto order = lp0bPlanOrder(composed);
        for (std::size_t i = 0; i < chain.size(); ++i)
            CHECK(lp0bIndex(order, chain[i]) == lp0bIndex(order, "deferred_lighting") + 1 +
                                                 static_cast<std::ptrdiff_t>(i));
        REQUIRE_NOTHROW(compileComposition(composed));
    };
    SECTION("ids in dependency order are not reversed") {
        expect_run({"a_pass", "b_pass", "c_pass"});
    }
    SECTION("ids against dependency order follow the dependencies") {
        expect_run({"z_pass", "m_pass", "a_pass"});
    }
    SECTION("passes with no dependency between them are chained in id order") {
        auto document = lp0bGraph({"p_one", "p_two"}, placement);
        for (auto &node : document.at("nodes"))
            if (node.at("id") == "p_two") node["inputs"]["source"] = {{"node", "lit"}, {"port", "resource"}};
        const auto composed = lp0aCompose(document);
        const auto names = lp0bDeclared(composed.config);
        CHECK(lp0bIndex(names, "p_one") == lp0bIndex(names, "deferred_lighting") + 1);
        CHECK(lp0bIndex(names, "p_two") == lp0bIndex(names, "p_one") + 1);
        CHECK_FALSE(lp0bListed(passByName(composed.config, "p_two").at("after"), "deferred_lighting"));
        CHECK(lp0bListed(passByName(composed.config, "p_two").at("after"), "p_one"));
    }
    SECTION("a run may name a pass another run places") {
        auto document = lp0bGraph({"a_pass", "b_pass"}, placement);
        for (auto &node : document.at("nodes"))
            if (node.at("id") == "b_pass") node["parameters"]["insert"] = {{"after", "a_pass"}};
        const auto composed = lp0aCompose(document);
        const auto names = lp0bDeclared(composed.config);
        CHECK(lp0bIndex(names, "a_pass") == lp0bIndex(names, "deferred_lighting") + 1);
        CHECK(lp0bIndex(names, "b_pass") == lp0bIndex(names, "a_pass") + 1);
    }
}

TEST_CASE("LP-0b placement refusals are named", "[render-feature][graph-composition][lp0b]") {
    using Json = nlohmann::json;
    SECTION("an insert naming no pass is refused after composition") {
        const auto document = lp0bGraph({"blit"}, {{"insert", {{"after", "no_such_pass"}}}});
        REQUIRE_NOTHROW(partitionGraphDocument(document));
        const auto [code, fields] = lp0aRefusal(document);
        CHECK(code == "graph.insert_anchor_unknown@1");
        CHECK(fields == Json{{"node", "blit"}, {"anchor", "no_such_pass"}});
    }
    SECTION("an unknown canonical anchor is refused when edited") {
        const auto [code, fields] = lp0bEditRefusal(lp0bGraph({"blit"}, {{"canonical_anchor", "nowhere"}}));
        CHECK(code == "graph.insert_anchor_unknown@1");
        CHECK(fields == Json{{"node", "blit"}, {"anchor", "nowhere"}});
    }
    SECTION("an anchor in another pass family is refused after composition") {
        auto document = lp0bGraph({"blit"}, {{"insert", {{"after", "deferred_lighting"}}}});
        document["nodes"][2]["parameters"]["pass_set"] = "overlay";
        REQUIRE(document["nodes"][2]["id"] == "blit");
        auto [code, fields] = lp0aRefusal(document);
        CHECK(code == "graph.insert_family_mismatch@1");
        CHECK(fields == Json{{"node", "blit"}, {"anchor", "deferred_lighting"}});
        document["nodes"][2]["parameters"].erase("insert");
        document["nodes"][2]["parameters"]["canonical_anchor"] = "post_ldr";
        std::tie(code, fields) = lp0aRefusal(document);
        CHECK(code == "graph.insert_family_mismatch@1");
        CHECK(fields == Json{{"node", "blit"}, {"anchor", "post_ldr"}});
    }
    SECTION("a compute node takes no place among passes") {
        const Json buffer_port{{"kind", "buffer"}, {"element", "float"}, {"access", "read_write"}};
        for (const auto &placement : {Json{{"insert", {{"after", "deferred_lighting"}}}},
                                      Json{{"canonical_anchor", "post_ldr"}}}) {
            INFO(placement.dump());
            auto document = lp0bGraph({}, Json::object());
            document["nodes"].push_back({{"id", "data"}, {"op", "render.buffer"},
                                         {"parameters", {{"element", "float"}, {"size", 16}}},
                                         {"outputs", {"resource"}}});
            Json parameters{{"shader", "engine://ray_deform"}, {"ports", {{"data", buffer_port}}}};
            for (const auto &[key, value] : placement.items()) parameters[key] = value;
            document["nodes"].push_back({{"id", "simulate"}, {"op", "render.compute"},
                                         {"parameters", std::move(parameters)},
                                         {"inputs", {{"data", {{"node", "data"}, {"port", "resource"}}}}},
                                         {"outputs", {"data"}}});
            const auto [code, fields] = lp0bEditRefusal(document);
            CHECK(code == "graph.insert_on_compute@1");
            CHECK(fields == Json{{"node", "simulate"}});
        }
    }
    SECTION("the shape is checked when edited") {
        for (const auto &placement : {Json{{"insert", {{"after", "deferred_lighting"}}}, {"canonical_anchor", "post_ldr"}},
                                      Json{{"insert", "after:deferred_lighting"}},
                                      Json{{"insert", {{"after", ""}}}},
                                      Json{{"insert", {{"after", 3}}}},
                                      Json{{"insert", {{"over", "deferred_lighting"}}}},
                                      Json{{"insert", {{"after", "deferred_lighting"}, {"before", "forward_opaque"}}}},
                                      Json{{"canonical_anchor", 1}}}) {
            INFO(placement.dump());
            CHECK_THROWS_AS(partitionGraphDocument(lp0bGraph({"blit"}, placement)), std::invalid_argument);
        }
    }
}

namespace {
// LP-1a: graph.loop over the hybrid preset with bloom. `paint` draws
// engine://passthrough into an image of its own; `accumulate` draws into the
// image its `target` port is given, loading what is there.
nlohmann::json lp1aColorPort() {
    return {{"kind", "image"}, {"type", lp0aSceneColorType()}, {"access", "write"},
            {"attachment", "color"}, {"location", 0}};
}

nlohmann::json lp1aFullscreen(const std::string &id, const nlohmann::json &color_source,
                              const nlohmann::json &extra = nlohmann::json::object()) {
    using Json = nlohmann::json;
    Json parameters{{"pass_set", "main_render"},
                    {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}},
                    {"ports", {{"color", lp1aColorPort()}}}};
    for (const auto &[key, value] : extra.items()) parameters[key] = value;
    return Json{{"id", id}, {"op", "render.fullscreen"}, {"parameters", std::move(parameters)},
                {"inputs", {{"color", color_source}}}, {"outputs", {"color"}}};
}

nlohmann::json lp1aPaintBody(const std::string &pass = "fill") {
    using Json = nlohmann::json;
    return Json{{"nodes", Json::array({lp0bImage("image"),
                                       lp1aFullscreen(pass, {{"node", "image"}, {"port", "resource"}})})},
                {"outputs", {{"image", {{"node", pass}, {"port", "color"}}}}}};
}

nlohmann::json lp1aAccumulateBody() {
    using Json = nlohmann::json;
    return Json{{"nodes", Json::array({{{"id", "target"}, {"op", "graph.input"}},
                                       lp1aFullscreen("blend", {{"node", "target"}, {"port", "value"}},
                                                      {{"color_load_op", "Load"}})})},
                {"outputs", Json::object()}};
}

nlohmann::json lp1aLoop(const std::string &id, const nlohmann::json &parameters,
                        const nlohmann::json &inputs = nlohmann::json::object(),
                        const nlohmann::json &outputs = nlohmann::json::array()) {
    return {{"id", id}, {"op", "graph.loop"}, {"parameters", parameters}, {"inputs", inputs}, {"outputs", outputs}};
}

nlohmann::json lp1aRange(std::int64_t count, const std::string &body = "paint") {
    return {{"over", {{"range", {{"count", count}}}}}, {"branches", nlohmann::json::array({{{"body", body}}})}};
}

nlohmann::json lp1aLit() {
    return {{"id", "lit"}, {"op", "render.target_ref"},
            {"parameters", {{"name", "lit_color"}, {"type", lp0aSceneColorType()}}}, {"outputs", {"resource"}}};
}

nlohmann::json lp1aGraph(nlohmann::json nodes, nlohmann::json definitions = nullptr) {
    using Json = nlohmann::json;
    if (definitions.is_null())
        definitions = Json{{"paint", lp1aPaintBody()}, {"accumulate", lp1aAccumulateBody()}};
    return Json{{"schema", "pelican.graph"}, {"version", 1}, {"definitions", std::move(definitions)},
                {"nodes", std::move(nodes)}, {"outputs", Json::object()},
                {"rendering", {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
                               {"features", Json::array({"engine://features/bloom.json"})}}}};
}

// The main_render frame plan of a composition, as get_frame_plan serializes it.
nlohmann::json lp1aPlanJson(const RenderFeatureComposeResult &composed) {
    const auto compiled = compileComposition(composed);
    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(composed.config);
    const auto graph = std::find_if(graphs.begin(), graphs.end(),
        [](const auto &candidate) { return candidate.name == "main_render"; });
    REQUIRE(graph != graphs.end());
    return framePlanToJson(planFrameGraph(*graph), &compiled);
}

const nlohmann::json *lp1aPlanNode(const nlohmann::json &plan, std::string_view name) {
    for (const auto &node : plan.at("nodes"))
        if (node.at("name").get<std::string>() == name) return &node;
    return nullptr;
}

std::vector<std::string> lp1aNamesUnder(const nlohmann::json &config, const std::string &prefix) {
    std::vector<std::string> names;
    for (const auto &family : config.at("rendering_passes"))
        for (const auto &pass : family.at("passes"))
            if (pass.value("name", std::string{}).starts_with(prefix)) names.push_back(pass.at("name").get<std::string>());
    return names;
}

std::pair<std::string, nlohmann::json> lp1aEditRefusal(const nlohmann::json &document) {
    try {
        (void)partitionGraphDocument(document);
    } catch (const GraphRegistryError &error) {
        return {error.code(), error.detail()};
    }
    FAIL("the document was not refused by name");
    return {};
}
} // namespace

TEST_CASE("LP-1a a range loop copies its body per element into passes and targets",
          "[render-feature][graph-composition][lp1a]") {
    using Json = nlohmann::json;
    const auto document = lp1aGraph(Json::array({lp1aLoop("steps", lp1aRange(4))}));
    const auto composed = lp0aCompose(document);
    const auto &config = composed.config;
    std::vector<std::string> expected_passes;
    for (int element = 0; element < 4; ++element) {
        const auto prefix = "steps/" + std::to_string(element) + "/";
        expected_passes.push_back(prefix + "fill");
        const auto &pass = passByName(config, prefix + "fill");
        CHECK(pass.at("output").at("color") == Json::array({prefix + "image"}));
        CHECK(std::count_if(config.at("render_targets").begin(), config.at("render_targets").end(),
                            [&](const Json &target) { return target.at("name") == prefix + "image"; }) == 1);
    }
    CHECK(lp1aNamesUnder(config, "steps/") == expected_passes);
    // The template the body was checked as is never declared.
    CHECK(config.dump().find("steps/$") == std::string::npos);
    // The authored document holds the loop, not its expansion.
    CHECK(document.dump().find("steps/0") == std::string::npos);

    const auto plan = lp1aPlanJson(composed);
    REQUIRE(plan.contains("loops"));
    CHECK(plan.at("loops") == Json::array({{{"node", "steps"}, {"tier", "expand"}, {"count", 4},
        {"elements", Json::array({{{"element", 0}, {"index", 0}, {"branch", 0}},
                                  {{"element", 1}, {"index", 1}, {"branch", 0}},
                                  {{"element", 2}, {"index", 2}, {"branch", 0}},
                                  {{"element", 3}, {"index", 3}, {"branch", 0}}})},
        {"unmatched_branches", Json::array()}}}));
    for (int element = 0; element < 4; ++element) {
        const auto prefix = "steps/" + std::to_string(element) + "/";
        const auto *node = lp1aPlanNode(plan, prefix + "fill");
        REQUIRE(node != nullptr);
        CHECK(node->at("loop_origin") == Json{{"loop", "steps"}, {"element", element}, {"node", "fill"}});
        const auto resource = std::find_if(plan.at("resources").begin(), plan.at("resources").end(),
            [&](const Json &entry) { return entry.at("name") == prefix + "image"; });
        REQUIRE(resource != plan.at("resources").end());
        CHECK(resource->at("loop_origin") == Json{{"loop", "steps"}, {"element", element}, {"node", "image"}});
        CHECK(resource->at("source") == "engine");
    }
    // A pass no loop made carries no loop_origin.
    CHECK_FALSE(lp1aPlanNode(plan, "deferred_lighting")->contains("loop_origin"));

    SECTION("a loop that expands nothing is reported with count 0") {
        const auto empty = lp0aCompose(lp1aGraph(Json::array({lp1aLoop("steps", lp1aRange(0))})));
        CHECK(lp1aNamesUnder(empty.config, "steps/").empty());
        const auto empty_plan = lp1aPlanJson(empty);
        CHECK(empty_plan.at("loops") == Json::array({{{"node", "steps"}, {"tier", "expand"}, {"count", 0},
            {"elements", Json::array()}, {"unmatched_branches", Json::array({0})}}}));
    }
    SECTION("a graph with no loop reports no loops") {
        CHECK_FALSE(lp1aPlanJson(lp0aCompose(lp0bGraph({"blit"}, Json::object()))).contains("loops"));
    }
}

TEST_CASE("LP-1a elements that touch an outside target run in element order",
          "[render-feature][graph-composition][lp1a]") {
    using Json = nlohmann::json;
    for (const auto direction : {"ascending", "descending"}) {
        DYNAMIC_SECTION(direction) {
            auto parameters = lp1aRange(12, "accumulate");
            parameters["order"] = {{"by", "element"}, {"direction", direction}};
            const auto composed = lp0aCompose(lp1aGraph(Json::array({
                lp1aLit(),
                lp1aLoop("acc", parameters, {{"target", {{"node", "lit"}, {"port", "resource"}}}})})));
            std::vector<std::string> element_order;
            for (int step = 0; step < 12; ++step) {
                const auto element = std::string{direction} == "ascending" ? step : 11 - step;
                element_order.push_back("acc/" + std::to_string(element) + "/blend");
            }
            // Each copy waits on the previous element's by an explicit edge.
            for (std::size_t i = 1; i < element_order.size(); ++i)
                CHECK(lp0bListed(passByName(composed.config, element_order[i]).at("after"), element_order[i - 1]));
            // Declared and planned in element order, though `acc/10/blend`
            // sorts before `acc/9/blend`.
            CHECK(lp1aNamesUnder(composed.config, "acc/") == element_order);
            std::vector<std::string> planned;
            for (const auto &name : lp0bPlanOrder(composed))
                if (name.starts_with("acc/")) planned.push_back(name);
            CHECK(planned == element_order);
            REQUIRE_NOTHROW(compileComposition(composed));
        }
    }
}

TEST_CASE("LP-1a branches choose a body per element, and where, order and limit choose the elements",
          "[render-feature][graph-composition][lp1a]") {
    using Json = nlohmann::json;
    const Json definitions{{"paint", lp1aPaintBody()}, {"first", lp1aPaintBody("first_fill")},
                           {"never", lp1aPaintBody("never_fill")}};
    const Json branches = Json::array({{{"when", {{"element", 7}}}, {"body", "never"}},
                                       {{"when", {{"element", 0}}}, {"body", "first"}},
                                       {{"when", {{"element_in", {2}}}}, {"body", nullptr}},
                                       {{"body", "paint"}}});
    auto parameters = lp1aRange(4);
    parameters["branches"] = branches;
    const auto composed = lp0aCompose(lp1aGraph(Json::array({lp1aLoop("steps", parameters)}), definitions));
    CHECK(lp1aNamesUnder(composed.config, "steps/") ==
          std::vector<std::string>{"steps/0/first_fill", "steps/1/fill", "steps/3/fill"});
    const auto plan = lp1aPlanJson(composed);
    CHECK(plan.at("loops").at(0).at("elements") ==
          Json::array({{{"element", 0}, {"index", 0}, {"branch", 1}},
                       {{"element", 1}, {"index", 1}, {"branch", 3}},
                       {{"element", 2}, {"index", 2}, {"branch", 2}},
                       {{"element", 3}, {"index", 3}, {"branch", 3}}}));
    CHECK(plan.at("loops").at(0).at("unmatched_branches") == Json::array({0}));
    CHECK(lp1aPlanNode(plan, "steps/0/first_fill")->at("loop_origin") ==
          Json{{"loop", "steps"}, {"element", 0}, {"node", "first_fill"}});

    SECTION("where -> order -> limit") {
        auto chosen = lp1aRange(6);
        chosen["where"] = {{"element_not_in", {4}}};
        chosen["order"] = {{"by", "element"}, {"direction", "descending"}};
        chosen["limit"] = 3;
        const auto limited = lp0aCompose(lp1aGraph(Json::array({lp1aLoop("steps", chosen)})));
        CHECK(lp1aPlanJson(limited).at("loops").at(0).at("elements") ==
              Json::array({{{"element", 5}, {"index", 0}, {"branch", 0}},
                           {{"element", 3}, {"index", 1}, {"branch", 0}},
                           {{"element", 2}, {"index", 2}, {"branch", 0}}}));
        CHECK(lp1aNamesUnder(limited.config, "steps/").size() == 3);
        chosen["limit"] = 100;
        chosen["where"] = {{"element", 1}};
        const auto one = lp0aCompose(lp1aGraph(Json::array({lp1aLoop("steps", chosen)})));
        CHECK(lp1aNamesUnder(one.config, "steps/") == std::vector<std::string>{"steps/1/fill"});
    }
}

TEST_CASE("LP-1a ordering against a loop names the loop, and a loop's output is its last element's",
          "[render-feature][graph-composition][lp1a]") {
    using Json = nlohmann::json;
    const auto type = lp0aSceneColorType();
    // `show` reads the loop's output; `after_all` is after the loop; the loop
    // is after `first`.
    auto show = lp1aFullscreen("show", {{"node", "shown"}, {"port", "resource"}});
    show["parameters"]["ports"]["source"] = {{"kind", "image"}, {"type", type}, {"access", "read"}};
    show["inputs"]["source"] = {{"node", "steps"}, {"port", "image"}};
    auto after_all = lp1aFullscreen("after_all", {{"node", "late"}, {"port", "resource"}}, {{"after", {"steps"}}});
    auto first = lp1aFullscreen("first", {{"node", "early"}, {"port", "resource"}});
    const auto graph = [&](std::int64_t count) {
        auto parameters = lp1aRange(count);
        parameters["after"] = {"first"};
        return lp1aGraph(Json::array({lp0bImage("shown"), lp0bImage("late"), lp0bImage("early"), first, show,
                                      after_all, lp1aLoop("steps", parameters, Json::object(), {"image"})}));
    };
    const auto composed = lp0aCompose(graph(3));
    const auto &config = composed.config;
    const auto &show_pass = passByName(config, "show");
    CHECK(show_pass.at("input") == Json::array({"steps/2/image"}));
    CHECK(show_pass.at("resource_ports").at("source").at("resource") == "steps/2/image");
    CHECK(lp0bListed(show_pass.at("after"), "steps/2/fill"));
    const auto &after_pass = passByName(config, "after_all");
    for (const auto *name : {"steps/0/fill", "steps/1/fill", "steps/2/fill"}) {
        CHECK(lp0bListed(after_pass.at("after"), name));
        CHECK(lp0bListed(passByName(config, name).at("after"), "first"));
    }
    CHECK_FALSE(lp0bListed(after_pass.at("after"), "steps"));
    const auto order = lp0bPlanOrder(composed);
    CHECK(lp0bIndex(order, "first") < lp0bIndex(order, "steps/0/fill"));
    CHECK(lp0bIndex(order, "steps/2/fill") < lp0bIndex(order, "show"));
    CHECK(lp0bIndex(order, "steps/2/fill") < lp0bIndex(order, "after_all"));
    REQUIRE_NOTHROW(compileComposition(composed));

    SECTION("with no element, an order against the loop is dropped") {
        auto without_reader = graph(0);
        auto &nodes = without_reader["nodes"];
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [](const Json &node) { return node.at("id") == "show"; }),
                    nodes.end());
        const auto empty = lp0aCompose(without_reader);
        CHECK(lp1aNamesUnder(empty.config, "steps/").empty());
        CHECK_FALSE(lp0bListed(passByName(empty.config, "after_all").value("after", Json::array()), "steps"));
        REQUIRE_NOTHROW(compileComposition(empty));
    }
    SECTION("with no element, reading the loop's output is refused by name") {
        const auto [code, fields] = lp0aRefusal(graph(0));
        CHECK(code == "graph.loop_output_empty@1");
        CHECK(fields == Json{{"node", "steps"}, {"port", "image"}});
    }
}

TEST_CASE("LP-1a a loop's insert places its whole expansion as one run",
          "[render-feature][graph-composition][lp1a]") {
    using Json = nlohmann::json;
    auto parameters = lp1aRange(3);
    parameters["insert"] = {{"after", "deferred_lighting"}};
    const auto composed = lp0aCompose(lp1aGraph(Json::array({lp1aLoop("steps", parameters)})));
    const auto names = lp0bDeclared(composed.config);
    const auto anchor = lp0bIndex(names, "deferred_lighting");
    for (int element = 0; element < 3; ++element)
        CHECK(lp0bIndex(names, "steps/" + std::to_string(element) + "/fill") == anchor + 1 + element);
    CHECK(lp0bListed(passByName(composed.config, "steps/0/fill").at("after"), "deferred_lighting"));
    const auto order = lp0bPlanOrder(composed);
    CHECK(lp0bIndex(order, "steps/0/fill") == lp0bIndex(order, "deferred_lighting") + 1);
    REQUIRE_NOTHROW(compileComposition(composed));

    SECTION("canonical_anchor") {
        auto anchored = lp1aRange(2);
        anchored["canonical_anchor"] = "post_ldr";
        const auto placed = lp0aCompose(lp1aGraph(Json::array({lp1aLoop("steps", anchored)})));
        const auto declared = lp0bDeclared(placed.config);
        CHECK(lp0bIndex(declared, "steps/0/fill") > lp0bIndex(declared, "HighLuminanceExtraction"));
        CHECK(lp0bIndex(declared, "steps/1/fill") + 1 == lp0bIndex(declared, "__anchor_pelican_ui"));
    }
    SECTION("an anchor nothing declares is refused under the loop's id") {
        auto unknown = lp1aRange(2);
        unknown["insert"] = {{"after", "no_such_pass"}};
        const auto [code, fields] = lp0aRefusal(lp1aGraph(Json::array({lp1aLoop("steps", unknown)})));
        CHECK(code == "graph.insert_anchor_unknown@1");
        CHECK(fields == Json{{"node", "steps"}, {"anchor", "no_such_pass"}});
    }
}

TEST_CASE("LP-1a loop refusals are named", "[render-feature][graph-composition][lp1a]") {
    using Json = nlohmann::json;
    const auto loop_graph = [](const Json &parameters, const Json &definitions = nullptr,
                               const Json &inputs = Json::object(), const Json &outputs = Json::array()) {
        return lp1aGraph(Json::array({lp1aLoop("steps", parameters, inputs, outputs)}), definitions);
    };
    const auto expect = [](const Json &document, const std::string &code, const Json &fields) {
        const auto [actual_code, actual_fields] = lp1aEditRefusal(document);
        CHECK(actual_code == code);
        CHECK(actual_fields == fields);
    };
    SECTION("the last branch is the else") {
        auto parameters = lp1aRange(2);
        parameters["branches"] = Json::array({{{"when", {{"element", 0}}}, {"body", "paint"}}});
        expect(loop_graph(parameters), "graph.loop_branch_else_missing@1", {{"node", "steps"}});
    }
    SECTION("every branch body has the same ports") {
        auto other = lp1aPaintBody();
        other["outputs"] = {{"picture", {{"node", "fill"}, {"port", "color"}}}};
        auto parameters = lp1aRange(2);
        parameters["branches"] = Json::array({{{"when", {{"element", 0}}}, {"body", "paint"}}, {{"body", "other"}}});
        expect(loop_graph(parameters, {{"paint", lp1aPaintBody()}, {"other", other}}),
               "graph.loop_branch_interface@1", {{"node", "steps"}, {"branch", 1}, {"definition", "other"}, {"port", "image"}});
    }
    SECTION("a loop stands in the root document only") {
        auto body = lp1aPaintBody();
        body["nodes"].push_back(lp1aLoop("inner", lp1aRange(1)));
        expect(lp1aGraph(Json::array(), {{"paint", lp1aPaintBody()}, {"holder", body}}), "graph.loop_nested@1",
               {{"node", "inner"}, {"definition", "holder"}});
    }
    SECTION("what the expand stage cannot run") {
        // LP-Vb expands view_axis (its runtime axes are refused when the
        // graph is resolved); a set v0 does not know is refused here.
        auto view = lp1aRange(1);
        view["over"] = {{"lights", Json::object()}};
        expect(loop_graph(view), "graph.loop_tier_unsupported@1", {{"node", "steps"}, {"reason", "set_unsupported"}});
        // LP-3a: draw_items runs draw.* bodies; a body that makes a
        // resource has no place in one material pass.
        auto draws = lp1aRange(1);
        draws["over"] = {{"draw_items", {{"contract", "forward_opaque_v1"}}}};
        expect(loop_graph(draws), "graph.loop_tier_unsupported@1",
               {{"node", "steps"}, {"reason", "resource_in_draw_body"}});
        auto per_frame = lp1aRange(1);
        per_frame["over"]["range"]["count"] = {{"node", "n"}, {"port", "value"}};
        expect(loop_graph(per_frame), "graph.loop_tier_unsupported@1", {{"node", "steps"}, {"reason", "per_frame_value"}});
        auto nested = lp1aPaintBody();
        nested["nodes"].push_back({{"id", "inner"}, {"op", "graph.instance"}, {"definition", "paint"}});
        expect(loop_graph(lp1aRange(1, "nested"), {{"paint", lp1aPaintBody()}, {"nested", nested}}),
               "graph.loop_tier_unsupported@1", {{"node", "steps"}, {"reason", "nested"}});
        auto draw = lp1aPaintBody();
        draw["nodes"].push_back({{"id", "mat"}, {"op", "draw.material"}, {"parameters", Json::object()},
                                 {"outputs", Json::array()}});
        expect(loop_graph(lp1aRange(1, "drawn"), {{"paint", lp1aPaintBody()}, {"drawn", draw}}),
               "graph.loop_tier_unsupported@1", {{"node", "steps"}, {"reason", "mixed_body"}});
    }
    SECTION("a fixed tier the loop cannot run in") {
        auto parameters = lp1aRange(1);
        parameters["tier"] = "draw_loop";
        expect(loop_graph(parameters), "graph.loop_tier_mismatch@1",
               {{"node", "steps"}, {"tier", "draw_loop"}, {"actual", "expand"}});
        parameters["tier"] = "expand";
        REQUIRE_NOTHROW(partitionGraphDocument(loop_graph(parameters)));
    }
    SECTION("an order other than by element") {
        auto parameters = lp1aRange(1);
        parameters["order"] = {{"by", "phase_default"}};
        expect(loop_graph(parameters), "graph.loop_order_unsupported@1",
               {{"node", "steps"}, {"order", {{"by", "phase_default"}}}});
        parameters["order"] = {{"by", "element"}, {"direction", "sideways"}};
        expect(loop_graph(parameters), "graph.loop_order_unsupported@1",
               {{"node", "steps"}, {"order", {{"by", "element"}, {"direction", "sideways"}}}});
    }
    SECTION("what LP-1a held back is no longer refused as pending (LP-1b)") {
        auto expression = lp1aRange(1);
        expression["over"]["range"]["count"] = {{"$expr", "2 + 2"}};
        REQUIRE_NOTHROW(partitionGraphDocument(loop_graph(expression)));
        auto when = lp1aRange(1);
        when["branches"] = Json::array({{{"when", {{"element", {{"$expr", "count - 1"}}}}}, {"body", "paint"}},
                                        {{"body", "paint"}}});
        REQUIRE_NOTHROW(partitionGraphDocument(loop_graph(when)));
        REQUIRE_NOTHROW(partitionGraphDocument(loop_graph(lp1aRange(1), nullptr, Json::object(), {"image[]"})));
        // An element is named only of a loop's p[].
        auto document = lp1aGraph(Json::array({lp1aLit(), lp1aLoop("steps", lp1aRange(1, "accumulate"),
            {{"target", {{"node", "lit"}, {"port", "resource"}, {"element", {{"$expr", "element"}}}}}})}));
        CHECK_THROWS_AS(partitionGraphDocument(document), std::invalid_argument);
    }
    SECTION("a loop of compute tasks takes no place among passes") {
        Json compute_body{{"nodes", Json::array({
            {{"id", "data"}, {"op", "render.buffer"}, {"parameters", {{"element", "float"}, {"size", 16}}},
             {"outputs", {"resource"}}},
            {{"id", "simulate"}, {"op", "render.compute"},
             {"parameters", {{"shader", "engine://ray_deform"},
                             {"ports", {{"data", {{"kind", "buffer"}, {"element", "float"}, {"access", "read_write"}}}}}}},
             {"inputs", {{"data", {{"node", "data"}, {"port", "resource"}}}}}, {"outputs", {"data"}}}})},
            {"outputs", Json::object()}};
        auto parameters = lp1aRange(2, "tasks");
        parameters["insert"] = {{"after", "deferred_lighting"}};
        expect(loop_graph(parameters, {{"tasks", compute_body}}), "graph.insert_on_compute@1", {{"node", "steps"}});
    }
    SECTION("the expansion's names are not the author's to use") {
        auto document = lp1aGraph(Json::array({lp1aLoop("steps", lp1aRange(2)), lp0bImage("late"),
            lp1aFullscreen("late_pass", {{"node", "late"}, {"port", "resource"}}, {{"after", {"steps/1/fill"}}})}));
        expect(document, "graph.loop_expansion_referenced@1", {{"name", "steps/1/fill"}, {"pointer", "/nodes/2/parameters/after/0"}});
        auto ordered = lp1aGraph(Json::array({lp1aLoop("steps", lp1aRange(2))}));
        ordered["rendering"]["frame_node_order"] = {{"main_render", {"deferred_lighting", "steps/0/fill"}}};
        expect(ordered, "graph.loop_expansion_referenced@1",
               {{"name", "steps/0/fill"}, {"pointer", "/rendering/frame_node_order/main_render/1"}});
        auto planned = lp1aGraph(Json::array({lp1aLoop("steps", lp1aRange(2))}));
        planned["rendering"]["target_planning"] = {{"graphs", {{"main_render", {{"nodes", {{"steps/0/fill", {{"isolate", true}}}}}}}}}};
        expect(planned, "graph.loop_expansion_referenced@1",
               {{"name", "steps/0/fill"}, {"pointer", "/rendering/target_planning/graphs/main_render/nodes/steps~10~1fill"}});
        // The loop's own id is how an order names it.
        auto by_id = lp1aGraph(Json::array({lp1aLoop("steps", lp1aRange(2)), lp0bImage("late"),
            lp1aFullscreen("late_pass", {{"node", "late"}, {"port", "resource"}}, {{"after", {"steps"}}})}));
        REQUIRE_NOTHROW(partitionGraphDocument(by_id));
    }
    SECTION("too many elements, or too many passes in the graph") {
        REQUIRE_NOTHROW(partitionGraphDocument(loop_graph(lp1aRange(65))));
        auto [code, fields] = lp0aRefusal(loop_graph(lp1aRange(65)));
        CHECK(code == "graph.loop_expansion_limit@1");
        CHECK(fields == Json{{"node", "steps"}, {"count", 65}, {"limit", 64}});
        // A limit keeps a long range within bounds.
        auto limited = lp1aRange(1000);
        limited["limit"] = 64;
        REQUIRE_NOTHROW(lp0aCompose(loop_graph(limited)));
        // Two loops of 64 and one graph pass: 129 in the graph.
        auto document = lp1aGraph(Json::array({lp1aLoop("a_steps", lp1aRange(64)), lp1aLoop("b_steps", lp1aRange(64)),
            lp0bImage("late"), lp1aFullscreen("late_pass", {{"node", "late"}, {"port", "resource"}})}));
        std::tie(code, fields) = lp0aRefusal(document);
        CHECK(code == "graph.loop_expansion_limit@1");
        CHECK(fields == Json{{"node", "b_steps"}, {"count", 129}, {"limit", 128}});
    }
    SECTION("the shape is checked when edited") {
        for (const auto &parameters : {Json{{"branches", Json::array({{{"body", "paint"}}})}},
                                       Json{{"over", {{"range", {{"count", -1}}}}}, {"branches", Json::array({{{"body", "paint"}}})}},
                                       Json{{"over", {{"range", {{"count", 1.5}}}}}, {"branches", Json::array({{{"body", "paint"}}})}},
                                       Json{{"over", {{"range", {{"count", 1}}}}}, {"branches", Json::array()}},
                                       Json{{"over", {{"range", {{"count", 1}}}}}, {"branches", Json::array({{{"body", "nowhere"}}})}},
                                       Json{{"over", {{"range", {{"count", 1}}}}}, {"branches", Json::array({{{"body", "paint"}}})},
                                            {"where", {{"element_greater", 1}}}},
                                       Json{{"over", {{"range", {{"count", 1}}}}}, {"branches", Json::array({{{"body", "paint"}}})},
                                            {"limit", -1}},
                                       Json{{"over", {{"range", {{"count", 1}}}}}, {"branches", Json::array({{{"body", "paint"}}})},
                                            {"tier", "sometimes"}},
                                       Json{{"over", {{"range", {{"count", 1}}}}}, {"branches", Json::array({{{"body", "paint"}}})},
                                            {"colour", "red"}},
                                       Json{{"over", {{"range", {{"count", 1}}}}},
                                            {"branches", Json::array({{{"body", "paint"}}, {{"when", {{"element", 0}}}, {"body", "paint"}}})}}}) {
            INFO(parameters.dump());
            CHECK_THROWS_AS(partitionGraphDocument(loop_graph(parameters)), std::invalid_argument);
        }
        // A body node takes its place from the loop.
        auto placed = lp1aPaintBody();
        placed["nodes"][1]["parameters"]["insert"] = {{"after", "deferred_lighting"}};
        CHECK_THROWS_AS(partitionGraphDocument(loop_graph(lp1aRange(1, "placed"), {{"placed", placed}})),
                        std::invalid_argument);
    }
    SECTION("a composition refusal names the loop's copy") {
        auto body = lp1aAccumulateBody();
        body["nodes"].push_back({{"id", "missing"}, {"op", "render.target_ref"},
                                 {"parameters", {{"name", "no_such_target"}, {"type", lp0aSceneColorType()}}},
                                 {"outputs", {"resource"}}});
        body["nodes"][1]["inputs"]["color"] = {{"node", "missing"}, {"port", "resource"}};
        auto parameters = lp1aRange(2, "broken");
        const auto document = lp1aGraph(Json::array({lp1aLit(), lp1aLoop("steps", parameters,
            {{"target", {{"node", "lit"}, {"port", "resource"}}}})}), {{"broken", body}});
        const auto [code, fields] = lp0aRefusal(document);
        CHECK(code == "graph.target_ref_unknown@1");
        // LP-1b: and where the copy came from, as the frame plan says it.
        CHECK(fields == Json{{"node", "steps/0/missing"}, {"name", "no_such_target"},
                             {"loop_origin", {{"loop", "steps"}, {"element", 0}, {"node", "missing"}}}});
    }
}

// LP-1a: what a recompile costs as a loop grows to the expansion limit. Not
// run by ctest (hidden); run by name to measure:
//   pelican_test_group_render_pipeline.exe "LP-1a recompile time up to the expansion limit"
TEST_CASE("LP-1a recompile time up to the expansion limit", "[.][lp1a-timing]") {
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    for (const int per_loop : {0, 16, 32, 48, 64}) {
        const auto document = lp1aGraph(Json::array({lp1aLoop("a_steps", lp1aRange(per_loop)),
                                                     lp1aLoop("b_steps", lp1aRange(per_loop))}));
        constexpr int repeats = 5;
        double compose_ms = 0.0;
        double plan_ms = 0.0;
        for (int repeat = 0; repeat < repeats; ++repeat) {
            const auto start = Clock::now();
            const auto composed = lp0aCompose(document);
            const auto compiled = compileComposition(composed);
            const auto composed_at = Clock::now();
            for (const auto &graph : parseFrameGraphDefinitionsFromConfigJson(composed.config))
                (void)framePlanToJson(planFrameGraph(graph), &compiled);
            const auto planned_at = Clock::now();
            compose_ms += std::chrono::duration<double, std::milli>(composed_at - start).count();
            plan_ms += std::chrono::duration<double, std::milli>(planned_at - composed_at).count();
        }
        std::cout << "LP-1a timing: " << 2 * per_loop << " loop passes: compose+compile "
                  << compose_ms / repeats << " ms, plan " << plan_ms / repeats << " ms\n";
    }
    SUCCEED();
}

namespace {
// LP-1b: a sampled read port, a pass that reads `source` and draws into
// `color`, and a body that does that into an image of its own and outputs it
// as `out` -- the shape of a blur step or a bloom down step.
nlohmann::json lp1bReadPort() {
    return {{"kind", "image"}, {"type", lp0aSceneColorType()}, {"access", "read"},
            {"sampling", {{"filter", "nearest"}, {"address", "clamp_to_edge"}}}};
}

nlohmann::json lp1bStep(const std::string &id, const nlohmann::json &source, const nlohmann::json &color,
                        const nlohmann::json &extra = nlohmann::json::object()) {
    auto pass = lp1aFullscreen(id, color, extra);
    pass["parameters"]["ports"]["source"] = lp1bReadPort();
    pass["inputs"]["source"] = source;
    return pass;
}

nlohmann::json lp1bChainBody(const nlohmann::json &extent_scale = 1.0, const nlohmann::json &defines = nullptr) {
    using Json = nlohmann::json;
    auto image = lp0bImage("image");
    image["parameters"]["extent_scale"] = extent_scale;
    auto step = lp1bStep("step", {{"node", "source"}, {"port", "value"}}, {{"node", "image"}, {"port", "resource"}});
    if (!defines.is_null()) step["parameters"]["shader"]["defines"] = defines;
    return Json{{"nodes", Json::array({{{"id", "source"}, {"op", "graph.input"}}, image, step})},
                {"outputs", {{"out", {{"node", "step"}, {"port", "color"}}}}}};
}

// `seed` draws `seed_image`: the first value a chain starts from.
nlohmann::json lp1bSeed() {
    return nlohmann::json::array({lp0bImage("seed_image"),
                                  lp1aFullscreen("seed", {{"node", "seed_image"}, {"port", "resource"}})});
}

// `chain`: `count` copies of chain_body, each reading the previous one's out.
nlohmann::json lp1bChainLoop(std::int64_t count, const nlohmann::json &outputs = nlohmann::json::array({"out"})) {
    using Json = nlohmann::json;
    auto parameters = lp1aRange(count, "chain_body");
    parameters["carry"] = Json::array({{{"from", "out"}, {"to", "source"}}});
    return lp1aLoop("chain", parameters, {{"source", {{"node", "seed"}, {"port", "color"}}}}, outputs);
}

nlohmann::json lp1bChainGraph(const nlohmann::json &loop, const nlohmann::json &definitions,
                              const nlohmann::json &extra = nlohmann::json::array()) {
    auto nodes = lp1bSeed();
    nodes.push_back(loop);
    for (const auto &node : extra) nodes.push_back(node);
    return lp1aGraph(nodes, definitions);
}

const nlohmann::json &lp1bTarget(const nlohmann::json &config, const std::string &name) {
    for (const auto &target : config.at("render_targets"))
        if (target.value("name", std::string{}) == name) return target;
    FAIL("no render target " << name);
    static const nlohmann::json none;
    return none;
}

const nlohmann::json &lp1bTask(const nlohmann::json &config, const std::string &name) {
    for (const auto &task : config.at("compute_tasks"))
        if (task.value("name", std::string{}) == name) return task;
    FAIL("no compute task " << name);
    static const nlohmann::json none;
    return none;
}

std::string lp1bSource(const nlohmann::json &config, const std::string &pass) {
    return passByName(config, pass).at("resource_ports").at("source").at("resource").get<std::string>();
}

bool lp1bContains(const nlohmann::json &list, const std::string &name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}
} // namespace

TEST_CASE("LP-1b expressions give each element its own extent and shader defines",
          "[render-feature][graph-composition][lp1b]") {
    using Json = nlohmann::json;
    const Json definitions{{"chain_body", lp1bChainBody({{"$expr", "1 / pow2(element + 1)"}},
                                                        Json::array({{{"$format", "STEP={element}"}}, "FIXED=1"}))}};
    const auto composed = lp0aCompose(lp1bChainGraph(lp1bChainLoop(3), definitions));
    const auto &config = composed.config;
    const std::array<double, 3> scales{0.5, 0.25, 0.125};
    for (int element = 0; element < 3; ++element) {
        const auto prefix = "chain/" + std::to_string(element) + "/";
        INFO(prefix);
        CHECK(lp1bTarget(config, prefix + "image").at("extent_scale") == scales[element]);
        CHECK(passByName(config, prefix + "step").at("shader").at("defines") ==
              Json::array({"STEP=" + std::to_string(element), "FIXED=1"}));
    }
    // Nothing of an expression is left for the runtime to read.
    CHECK(config.dump().find("$expr") == std::string::npos);
    CHECK(config.dump().find("$format") == std::string::npos);
    CHECK(config.dump().find("$pointer") == std::string::npos);
    // The per-element passes compile and plan like any other.
    const auto plan = lp1aPlanJson(composed);
    REQUIRE(lp1aPlanNode(plan, "chain/2/step") != nullptr);

    SECTION("the count and a branch's when read expressions") {
        auto loop = lp1bChainLoop(0);
        loop["parameters"]["over"]["range"]["count"] = {{"$expr", "max(2, 1) * 2 - 1"}};
        loop["parameters"]["branches"] = Json::array({{{"when", {{"element", {{"$expr", "count - 1"}}}}}, {"body", nullptr}},
                                                      {{"body", "chain_body"}}});
        const auto counted = lp0aCompose(lp1bChainGraph(loop, definitions));
        CHECK(lp1aNamesUnder(counted.config, "chain/") == std::vector<std::string>{"chain/0/step", "chain/1/step"});
        CHECK(lp1aPlanJson(counted).at("loops").at(0).at("elements") ==
              Json::array({{{"element", 0}, {"index", 0}, {"branch", 1}},
                           {{"element", 1}, {"index", 1}, {"branch", 1}},
                           {{"element", 2}, {"index", 2}, {"branch", 0}}}));
        // `where` reads the element; a real literal is refused where an
        // integer is required.
        loop["parameters"]["where"] = {{"element_not_in", Json::array({{{"$expr", "3 % 2"}}})}};
        // Without element 1 there are two elements, so `count - 1` names none.
        const auto filtered = lp0aCompose(lp1bChainGraph(loop, definitions));
        CHECK(lp1aNamesUnder(filtered.config, "chain/") == std::vector<std::string>{"chain/0/step", "chain/2/step"});
    }
    SECTION("reals are exact fractions until the end") {
        const Json exact{{"chain_body", lp1bChainBody({{"$expr", "(1 / 3) * 3 - 0.5 + min(0.25, 1) - 0.25"}})}};
        const auto thirds = lp0aCompose(lp1bChainGraph(lp1bChainLoop(1), exact));
        CHECK(lp1bTarget(thirds.config, "chain/0/image").at("extent_scale") == 0.5);
    }
}

TEST_CASE("LP-1b expression refusals are named with the node, the pointer and the reason",
          "[render-feature][graph-composition][lp1b]") {
    using Json = nlohmann::json;
    // chain_body's nodes are [source, image, step]; the root's are
    // [seed_image, seed, chain].
    const auto withExtent = [](const Json &extent) {
        return lp1bChainGraph(lp1bChainLoop(3), {{"chain_body", lp1bChainBody(extent)}});
    };
    const auto withCount = [](const Json &count) {
        auto loop = lp1bChainLoop(0);
        loop["parameters"]["over"]["range"]["count"] = count;
        return lp1bChainGraph(loop, {{"chain_body", lp1bChainBody()}});
    };
    const auto invalid = [](const std::string &node, const std::string &pointer, const std::string &reason) {
        return Json{{"node", node}, {"pointer", pointer}, {"reason", reason}};
    };
    // A refusal of one element's copy says where it came from.
    const auto ofCopy = [](Json fields, std::int64_t element, const std::string &node) {
        fields["loop_origin"] = {{"loop", "chain"}, {"element", element}, {"node", node}};
        return fields;
    };
    const std::string extent_pointer = "/definitions/chain_body/nodes/1/parameters/extent_scale";
    const std::string count_pointer = "/nodes/2/parameters/over/range/count";
    const auto expectEdit = [&](const Json &document, const Json &fields) {
        const auto [code, actual] = lp1aEditRefusal(document);
        CHECK(code == "graph.loop_expression_invalid@1");
        CHECK(actual == fields);
    };
    const auto expectCompose = [&](const Json &document, const Json &fields) {
        REQUIRE_NOTHROW(partitionGraphDocument(document));
        const auto [code, actual] = lp0aRefusal(document);
        CHECK(code == "graph.loop_expression_invalid@1");
        CHECK(actual == fields);
    };
    SECTION("syntax") {
        expectEdit(withExtent({{"$expr", "1 +"}}), invalid("image", extent_pointer, "syntax"));
        expectEdit(withExtent({{"$expr", "sqrt(4)"}}), invalid("image", extent_pointer, "syntax"));
        expectEdit(withExtent({{"$expr", 1}}), invalid("image", extent_pointer, "syntax"));
        expectEdit(withExtent({{"$expr", "1"}, {"extra", 2}}), invalid("image", extent_pointer, "syntax"));
    }
    SECTION("too long, or nested too deep") {
        std::string sum = "1";
        while (sum.size() <= 256) sum += "+1";
        expectEdit(withExtent({{"$expr", sum}}), invalid("image", extent_pointer, "too_long"));
        const auto nested = [](int depth) { return std::string(depth, '(') + "1" + std::string(depth, ')'); };
        REQUIRE_NOTHROW(partitionGraphDocument(withExtent({{"$expr", nested(16)}})));
        expectEdit(withExtent({{"$expr", nested(17)}}), invalid("image", extent_pointer, "too_long"));
    }
    SECTION("variables the place does not have") {
        expectEdit(withExtent({{"$expr", "elements"}}), invalid("image", extent_pointer, "unknown_variable"));
        expectEdit(withCount({{"$expr", "element + 1"}}), invalid("chain", count_pointer, "unknown_variable"));
        auto where = lp1bChainLoop(3);
        where["parameters"]["where"] = {{"element", {{"$expr", "index"}}}};
        expectEdit(lp1bChainGraph(where, {{"chain_body", lp1bChainBody()}}),
                   invalid("chain", "/nodes/2/parameters/where/element", "unknown_variable"));
        where["parameters"]["where"] = {{"element_in", Json::array({0, {{"$expr", "count"}}})}};
        expectEdit(lp1bChainGraph(where, {{"chain_body", lp1bChainBody()}}),
                   invalid("chain", "/nodes/2/parameters/where/element_in/1", "unknown_variable"));
    }
    SECTION("an integer place") {
        expectEdit(withCount({{"$expr", "1.5"}}), invalid("chain", count_pointer, "not_integer"));
        const Json defines = Json::array({{{"$format", "STEP={element / 2.0}"}}});
        expectEdit(lp1bChainGraph(lp1bChainLoop(3), {{"chain_body", lp1bChainBody(1.0, defines)}}),
                   invalid("step", "/definitions/chain_body/nodes/2/parameters/shader/defines/0", "not_integer"));
        expectCompose(withExtent({{"$expr", "pow2(element) % 1.5"}}),
                      ofCopy(invalid("chain/0/image", extent_pointer, "not_integer"), 0, "image"));
    }
    SECTION("places decision 5 does not list") {
        auto limited = lp1bChainLoop(3);
        limited["parameters"]["limit"] = {{"$expr", "1"}};
        expectEdit(lp1bChainGraph(limited, {{"chain_body", lp1bChainBody()}}),
                   invalid("chain", "/nodes/2/parameters/limit", "field_not_allowed"));
        auto formatted = lp1bChainBody();
        formatted["nodes"][1]["parameters"]["format"] = {{"$format", "R{element}"}};
        expectEdit(lp1bChainGraph(lp1bChainLoop(3), {{"chain_body", formatted}}),
                   invalid("image", "/definitions/chain_body/nodes/1/parameters/format", "field_not_allowed"));
        expectEdit(withExtent({{"$format", "{element}"}}), invalid("image", extent_pointer, "field_not_allowed"));
        // Outside a loop body nothing takes an expression.
        auto root = lp1bChainGraph(lp1bChainLoop(3), {{"chain_body", lp1bChainBody()}});
        root["nodes"][0]["parameters"]["extent_scale"] = {{"$expr", "1"}};
        expectEdit(root, invalid("seed_image", "/nodes/0/parameters/extent_scale", "field_not_allowed"));
    }
    SECTION("what only the composed graph can say") {
        expectCompose(withExtent({{"$expr", "1 / (element - 1)"}}),
                      ofCopy(invalid("chain/1/image", extent_pointer, "division_by_zero"), 1, "image"));
        expectCompose(withCount({{"$expr", "pow2(62) + pow2(62)"}}), invalid("chain", count_pointer, "overflow"));
        expectCompose(withCount({{"$expr", "pow2(63)"}}), invalid("chain", count_pointer, "overflow"));
        expectCompose(withCount({{"$expr", "mip_levels_of(\"no_such_target\")"}}),
                      invalid("chain", count_pointer, "not_static"));
        // A mip chain that follows the extent is known only at run time.
        auto full = withCount({{"$expr", "mip_levels_of(\"full_chain\")"}});
        full["rendering"]["render_targets"] = Json::array({{{"name", "full_chain"}, {"extent_scale", 1.0},
            {"format", "R16G16B16A16_SFLOAT"}, {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
            {"usage", {"STORAGE", "SAMPLED"}}, {"mip_levels", "full"}}});
        expectCompose(full, invalid("chain", count_pointer, "not_static"));
        // The preset's targets answer: lit_color has one layer and one mip.
        const auto one = lp0aCompose(withCount({{"$expr", "layers_of(\"lit_color\") * mip_levels_of(\"lit_color\") + 1"}}));
        CHECK(lp1aNamesUnder(one.config, "chain/").size() == 2);
    }
}

TEST_CASE("LP-1b carry hands each element's output to the next element's input",
          "[render-feature][graph-composition][lp1b]") {
    using Json = nlohmann::json;
    const Json definitions{{"chain_body", lp1bChainBody()}};
    const Json show = Json::array({lp0bImage("shown"), lp1bStep("show", {{"node", "chain"}, {"port", "out"}},
                                                                 {{"node", "shown"}, {"port", "resource"}})});
    const auto composed = lp0aCompose(lp1bChainGraph(lp1bChainLoop(4), definitions, show));
    const auto &config = composed.config;
    CHECK(lp1bSource(config, "chain/0/step") == "seed_image");
    for (int element = 1; element < 4; ++element) {
        const auto name = "chain/" + std::to_string(element) + "/step";
        const auto previous = "chain/" + std::to_string(element - 1) + "/";
        INFO(name);
        CHECK(lp1bSource(config, name) == previous + "image");
        CHECK(passByName(config, name).at("input") == Json::array({previous + "image"}));
        CHECK(lp0bListed(passByName(config, name).at("after"), previous + "step"));
    }
    // The loop's output is the last element's.
    CHECK(lp1bSource(config, "show") == "chain/3/image");
    CHECK(lp0bListed(passByName(config, "show").at("after"), "chain/3/step"));
    // The frame plan reads and writes the chain in order.
    const auto plan = lp1aPlanJson(composed);
    for (int element = 1; element < 4; ++element) {
        const auto *node = lp1aPlanNode(plan, "chain/" + std::to_string(element) + "/step");
        REQUIRE(node != nullptr);
        CHECK(lp1bContains(node->at("reads"), "chain/" + std::to_string(element - 1) + "/image"));
        CHECK(lp1bContains(node->at("writes"), "chain/" + std::to_string(element) + "/image"));
    }
    const auto order = lp0bPlanOrder(composed);
    CHECK(lp0bIndex(order, "seed") < lp0bIndex(order, "chain/0/step"));
    for (int element = 1; element < 4; ++element)
        CHECK(lp0bIndex(order, "chain/" + std::to_string(element - 1) + "/step") <
              lp0bIndex(order, "chain/" + std::to_string(element) + "/step"));
    CHECK(lp0bIndex(order, "chain/3/step") < lp0bIndex(order, "show"));
    REQUIRE_NOTHROW(compileComposition(composed));

    SECTION("an element with no body passes the value on") {
        auto loop = lp1bChainLoop(4);
        loop["parameters"]["branches"] = Json::array({{{"when", {{"element_in", {1, 3}}}}, {"body", nullptr}},
                                                      {{"body", "chain_body"}}});
        const auto skipped = lp0aCompose(lp1bChainGraph(loop, definitions, show));
        CHECK(lp1aNamesUnder(skipped.config, "chain/") == std::vector<std::string>{"chain/0/step", "chain/2/step"});
        CHECK(lp1bSource(skipped.config, "chain/2/step") == "chain/0/image");
        CHECK(lp1bSource(skipped.config, "show") == "chain/2/image");
    }
    SECTION("with no element, the loop's input comes out") {
        const auto empty = lp0aCompose(lp1bChainGraph(lp1bChainLoop(0), definitions, show));
        CHECK(lp1aNamesUnder(empty.config, "chain/").empty());
        CHECK(lp1bSource(empty.config, "show") == "seed_image");
        REQUIRE_NOTHROW(compileComposition(empty));
    }
    SECTION("a carry whose ends are of different types is refused") {
        // The body never reads `source`, so only the carry compares the
        // image `out` with the buffer the loop's edge gives `source`.
        Json odd{{"nodes", Json::array({{{"id", "source"}, {"op", "graph.input"}}, lp0bImage("image"),
                                        lp1aFullscreen("fill", {{"node", "image"}, {"port", "resource"}})})},
                 {"outputs", {{"out", {{"node", "fill"}, {"port", "color"}}}}}};
        auto loop = lp1bChainLoop(2);
        loop["inputs"]["source"] = {{"node", "data"}, {"port", "resource"}};
        const Json data{{"id", "data"}, {"op", "render.buffer"}, {"parameters", {{"element", "float"}, {"size", 16}}},
                        {"outputs", {"resource"}}};
        const auto [code, fields] = lp1aEditRefusal(lp1bChainGraph(loop, {{"chain_body", odd}}, Json::array({data})));
        CHECK(code == "graph.loop_carry_type_mismatch@1");
        CHECK(fields == Json{{"node", "chain"}, {"from", "out"}, {"to", "source"}});
    }
    SECTION("the carry's shape is checked when edited") {
        for (const auto &carry : {Json::array({{{"from", "nowhere"}, {"to", "source"}}}),
                                  Json::array({{{"from", "out"}, {"to", "nothing"}}}),
                                  Json::array({{{"from", "out"}}}),
                                  Json::array({{{"from", "out"}, {"to", "source"}}, {{"from", "out"}, {"to", "source"}}})}) {
            auto loop = lp1bChainLoop(2);
            loop["parameters"]["carry"] = carry;
            INFO(carry.dump());
            CHECK_THROWS_AS(partitionGraphDocument(lp1bChainGraph(loop, definitions)), std::invalid_argument);
        }
        // A carried input starts from the loop's edge.
        auto unfed = lp1bChainLoop(2);
        unfed["inputs"] = Json::object();
        CHECK_THROWS_AS(partitionGraphDocument(lp1bChainGraph(unfed, definitions)), std::invalid_argument);
    }
}

namespace {
// LP-1b: the bloom shape. `down` halves four times, each element reading the
// previous one's image (carry) and publishing its own (`out[]`). `up` runs
// three times from the smallest, reads what it carries (first `down`'s last
// image) and draws into `down`'s image of the same element, which it names
// with a per-element input.
nlohmann::json lp1bBloomDocument(const nlohmann::json &target_element = {{"$expr", "element"}},
                                 const nlohmann::json &extra = nlohmann::json::array()) {
    using Json = nlohmann::json;
    Json up_body{{"nodes", Json::array({{{"id", "source"}, {"op", "graph.input"}},
                                        {{"id", "target"}, {"op", "graph.input"}},
                                        lp1bStep("blend", {{"node", "source"}, {"port", "value"}},
                                                 {{"node", "target"}, {"port", "value"}}, {{"color_load_op", "Load"}})})},
                 {"outputs", {{"out", {{"node", "blend"}, {"port", "color"}}}}}};
    auto down = lp1bChainLoop(4, Json::array({"out", "out[]"}));
    down["id"] = "down";
    auto up_parameters = lp1aRange(3, "up_body");
    up_parameters["order"] = {{"by", "element"}, {"direction", "descending"}};
    up_parameters["carry"] = Json::array({{{"from", "out"}, {"to", "source"}}});
    const auto up = lp1aLoop("up", up_parameters,
                             {{"source", {{"node", "down"}, {"port", "out"}}},
                              {"target", {{"node", "down"}, {"port", "out[]"}, {"element", target_element}}}},
                             Json::array({"out"}));
    auto nodes = lp1bSeed();
    nodes.push_back(down);
    nodes.push_back(up);
    for (const auto &node : extra) nodes.push_back(node);
    return lp1aGraph(nodes, {{"chain_body", lp1bChainBody({{"$expr", "1 / pow2(element + 1)"}})}, {"up_body", up_body}});
}
} // namespace

TEST_CASE("LP-1b a per-element input reads another loop's element, as bloom's up steps read its down steps",
          "[render-feature][graph-composition][lp1b]") {
    using Json = nlohmann::json;
    const auto composed = lp0aCompose(lp1bBloomDocument());
    const auto &config = composed.config;
    CHECK(lp1aNamesUnder(config, "up/") == std::vector<std::string>{"up/2/blend", "up/1/blend", "up/0/blend"});
    for (int element = 0; element < 3; ++element) {
        const auto name = "up/" + std::to_string(element) + "/blend";
        const auto own = "down/" + std::to_string(element) + "/image";
        const auto below = "down/" + std::to_string(element + 1) + "/image";
        INFO(name);
        // It draws into the down step of its own element, and reads the one
        // below: the first up step reads down's last, the others what the
        // up step before them drew.
        CHECK(passByName(config, name).at("output").at("color") == Json::array({own}));
        CHECK(lp1bSource(config, name) == below);
        CHECK(lp0bListed(passByName(config, name).at("after"), "down/" + std::to_string(element) + "/step"));
    }
    CHECK(lp0bListed(passByName(config, "up/2/blend").at("after"), "down/3/step"));
    CHECK(lp0bListed(passByName(config, "up/1/blend").at("after"), "up/2/blend"));
    const auto plan = lp1aPlanJson(composed);
    for (int element = 0; element < 3; ++element) {
        const auto *node = lp1aPlanNode(plan, "up/" + std::to_string(element) + "/blend");
        REQUIRE(node != nullptr);
        CHECK(lp1bContains(node->at("reads"), "down/" + std::to_string(element + 1) + "/image"));
        CHECK(lp1bContains(node->at("writes"), "down/" + std::to_string(element) + "/image"));
        CHECK(node->at("loop_origin") == Json{{"loop", "up"}, {"element", element}, {"node", "blend"}});
    }
    const auto order = lp0bPlanOrder(composed);
    CHECK(lp0bIndex(order, "down/3/step") < lp0bIndex(order, "up/2/blend"));
    CHECK(lp0bIndex(order, "up/2/blend") < lp0bIndex(order, "up/1/blend"));
    CHECK(lp0bIndex(order, "up/1/blend") < lp0bIndex(order, "up/0/blend"));
    REQUIRE_NOTHROW(compileComposition(composed));

    SECTION("an element no copy has is refused by name") {
        const auto [code, fields] = lp0aRefusal(lp1bBloomDocument({{"$expr", "element + 2"}}));
        CHECK(code == "graph.loop_element_unknown@1");
        CHECK(fields == Json{{"node", "up"}, {"port", "target"}, {"element", 4}});
    }
    SECTION("a node that is no loop reads a constant element") {
        const Json peek = Json::array({lp0bImage("shown"),
            lp1bStep("peek", {{"node", "down"}, {"port", "out[]"}, {"element", 1}}, {{"node", "shown"}, {"port", "resource"}})});
        const auto read = lp0aCompose(lp1bBloomDocument({{"$expr", "element"}}, peek));
        CHECK(lp1bSource(read.config, "peek") == "down/1/image");
        CHECK(lp0bListed(passByName(read.config, "peek").at("after"), "down/1/step"));
        auto variable = peek;
        variable[1]["inputs"]["source"]["element"] = {{"$expr", "element"}};
        const auto [code, fields] = lp1aEditRefusal(lp1bBloomDocument({{"$expr", "element"}}, variable));
        CHECK(code == "graph.loop_expression_invalid@1");
        CHECK(fields == Json{{"node", "peek"}, {"pointer", "/nodes/5/inputs/source/element"}, {"reason", "unknown_variable"}});
        auto whole = peek;
        whole[1]["inputs"]["source"].erase("element");
        CHECK_THROWS_AS(partitionGraphDocument(lp1bBloomDocument({{"$expr", "element"}}, whole)), std::invalid_argument);
    }
    SECTION("an edit sets a per-element edge") {
        auto document = lp1bBloomDocument();
        const Json source{{"node", "down"}, {"port", "out[]"}, {"element", {{"$expr", "element"}}}};
        auto without = document;
        without["nodes"][3]["inputs"].erase("target");
        const auto edited = editGraphDocument(without, Json::array({Json{{"op", "set_input"}, {"node", "up"},
            {"port", "target"}, {"source", source}}}));
        CHECK(edited.at("nodes").at(3).at("inputs").at("target") == source);
    }
}

TEST_CASE("LP-1b mip_levels_of counts a compute chain down one image's mips",
          "[render-feature][graph-composition][lp1b]") {
    using Json = nlohmann::json;
    const auto type = lp0aSceneColorType();
    const auto port = [&](const char *access, const char *binding, const Json &mip) {
        return Json{{"kind", "image"}, {"type", type}, {"access", access}, {"binding", binding},
                    {"subresource", {{"mip", mip}, {"layer", 0}}}};
    };
    const Json body{{"nodes", Json::array({
        {{"id", "color"}, {"op", "render.target_ref"}, {"parameters", {{"name", "chain_color"}, {"type", type}}},
         {"outputs", {"resource"}}},
        {{"id", "filter"}, {"op", "render.compute"},
         {"parameters", {{"shader", "engine://ray_deform"},
                         {"ports", {{"source", port("read", "sampled", {{"$expr", "element"}})},
                                    {"filtered", port("write", "storage", {{"$expr", "element + 1"}})}}}}},
         {"inputs", {{"source", {{"node", "color"}, {"port", "resource"}}},
                     {"filtered", {{"node", "color"}, {"port", "resource"}}}}},
         {"outputs", {"filtered"}}}})},
        {"outputs", Json::object()}};
    auto parameters = lp1aRange(0, "mip_step");
    parameters["over"]["range"]["count"] = {{"$expr", "mip_levels_of(\"chain_color\") - 1"}};
    auto document = lp1aGraph(Json::array({lp1aLoop("mips", parameters)}), {{"mip_step", body}});
    document["rendering"]["render_targets"] = Json::array({{{"name", "chain_color"}, {"extent_scale", 1.0},
        {"format", "R16G16B16A16_SFLOAT"}, {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
        {"usage", {"STORAGE", "SAMPLED", "COLOR_ATTACHMENT"}}, {"mip_levels", 5}}});
    const auto composed = lp0aCompose(document);
    const auto &config = composed.config;
    for (int element = 0; element < 4; ++element) {
        const auto name = "mips/" + std::to_string(element) + "/filter";
        INFO(name);
        const auto &task = lp1bTask(config, name);
        CHECK(task.at("resource_ports").at("source").at("subresource") == Json{{"mip", element}, {"layer", 0}});
        CHECK(task.at("resource_ports").at("filtered").at("subresource") == Json{{"mip", element + 1}, {"layer", 0}});
        CHECK(task.at("resource_ports").at("source").at("resource") == "chain_color");
        // One image, so each element waits on the one before.
        if (element > 0)
            CHECK(lp1bContains(task.at("after"), "mips/" + std::to_string(element - 1) + "/filter"));
    }
    CHECK(std::none_of(config.at("compute_tasks").begin(), config.at("compute_tasks").end(),
                       [](const Json &task) { return task.value("name", std::string{}) == "mips/4/filter"; }));
    REQUIRE(composed.graph_loops.size() == 1);
    CHECK(composed.graph_loops.front().count == 4);
}

TEST_CASE("LP-1b a loop body's value nodes run once per element",
          "[render-feature][graph-composition][lp1b]") {
    using Json = nlohmann::json;
    // `double` adds the loop's `gain` to itself and uploads it to a buffer of
    // its own; a compute pass reads the buffer.
    const Json body{{"nodes", Json::array({
        {{"id", "gain"}, {"op", "graph.input"}},
        {{"id", "twice"}, {"op", "value.add"},
         {"inputs", {{"a", {{"node", "gain"}, {"port", "value"}}}, {"b", {{"node", "gain"}, {"port", "value"}}}}},
         {"outputs", {"value"}}},
        {{"id", "uniform"}, {"op", "render.buffer"}, {"parameters", {{"element", "float"}}},
         {"inputs", {{"value", {{"node", "twice"}, {"port", "value"}}}}}, {"outputs", {"resource"}}}})},
        {"outputs", {{"doubled", {{"node", "twice"}, {"port", "value"}}}}}};
    auto parameters = lp1aRange(3, "double");
    parameters["carry"] = Json::array({{{"from", "doubled"}, {"to", "gain"}}});
    const Json document{{"schema", "pelican.graph"}, {"version", 1}, {"definitions", {{"double", body}}},
        {"nodes", Json::array({
            {{"id", "start"}, {"op", "value.constant"}, {"parameters", {{"value", 1.0}}}, {"outputs", {"value"}}},
            lp1aLoop("steps", parameters, {{"gain", {{"node", "start"}, {"port", "value"}}}}, Json::array({"doubled", "doubled[]"})),
            {{"id", "last"}, {"op", "value.add"},
             {"inputs", {{"a", {{"node", "steps"}, {"port", "doubled"}}},
                         {"b", {{"node", "steps"}, {"port", "doubled[]"}, {"element", 0}}}}},
             {"outputs", {"value"}}}})},
        {"outputs", Json::object()}};
    const auto values = partitionGraphDocument(document).values;
    std::map<std::string, Json> nodes;
    for (const auto &node : values.nodes) nodes.emplace(node.at("id").get<std::string>(), node);
    // The template is gone; each element has its own node and upload, and
    // each reads the element before it.
    for (const auto &[id, node] : nodes) CHECK(id.find("/$") == std::string::npos);
    CHECK(nodes.at("steps/0/twice").at("inputs").at("a") == Json{{"node", "start"}, {"port", "value"}});
    CHECK(nodes.at("steps/1/twice").at("inputs").at("a") == Json{{"node", "steps/0/twice"}, {"port", "value"}});
    CHECK(nodes.at("steps/2/twice").at("inputs").at("b") == Json{{"node", "steps/1/twice"}, {"port", "value"}});
    for (int element = 0; element < 3; ++element) {
        const auto upload = "$upload/steps/" + std::to_string(element) + "/uniform";
        REQUIRE(nodes.contains(upload));
        CHECK(nodes.at(upload).at("parameters").at("target") == "steps/" + std::to_string(element) + "/uniform");
        CHECK(nodes.at(upload).at("inputs").at("value") ==
              Json{{"node", "steps/" + std::to_string(element) + "/twice"}, {"port", "value"}});
    }
    // `p` is the last element's, `p[]` the named one's.
    CHECK(nodes.at("last").at("inputs").at("a") == Json{{"node", "steps/2/twice"}, {"port", "value"}});
    CHECK(nodes.at("last").at("inputs").at("b") == Json{{"node", "steps/0/twice"}, {"port", "value"}});

    SECTION("a count only the composed targets know is refused for a body with values") {
        auto counted = document;
        counted["nodes"][1]["parameters"]["over"]["range"]["count"] = {{"$expr", "mip_levels_of(\"lit_color\")"}};
        const auto [code, fields] = lp1aEditRefusal(counted);
        CHECK(code == "graph.loop_expression_invalid@1");
        CHECK(fields == Json{{"node", "steps"}, {"pointer", "/nodes/1/parameters/over/range/count"}, {"reason", "not_static"}});
    }
    SECTION("LP-Vb: so is a view axis, whose length only the resolved graph knows") {
        auto over_axis = document;
        over_axis["nodes"][1]["parameters"]["over"] = {
            {"view_axis", {{"family", "$shadow/directional"}, {"axis", "cascade"}}}};
        const auto [code, fields] = lp1aEditRefusal(over_axis);
        CHECK(code == "graph.loop_expression_invalid@1");
        CHECK(fields == Json{{"node", "steps"}, {"pointer", "/nodes/1/parameters/over/view_axis"}, {"reason", "not_static"}});
    }
}


namespace {
// LP-3a: the hybrid preset's material passes written in the graph
// (test/lp3a_decompose_support.hpp): render.material_pass nodes placed where
// the preset has them, each drawn by a graph.loop over draw_items whose else
// branch is draw.material.
nlohmann::json lp3aHybridRoot(nlohmann::json features) {
    return TestSupport::renderGraphRoot(nlohmann::json{
        {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
        {"features", std::move(features)}});
}

nlohmann::json lp3aHybridPreset() {
    return nlohmann::json::parse(engineResourceOrThrow("render_pipelines/hybrid_v1.json")).at("config");
}

nlohmann::json lp3aDecomposed(const nlohmann::json &root, bool with_loop = true,
                              std::vector<TestSupport::Lp3aDecomposedPass> passes =
                                  TestSupport::lp3aHybridMaterialPasses()) {
    return TestSupport::lp3aDecomposeMaterialPasses(root, lp3aHybridPreset(), passes, with_loop);
}

// A pass as the frame graph reads it, less the ordering the placement adds
// (the frame plan order is compared on its own).
nlohmann::json lp3aPassFields(const nlohmann::json &config, std::string_view name) {
    auto pass = passByName(config, name);
    pass.erase("after");
    pass.erase("before");
    return pass;
}

MaterialDrawSubject lp3aSubject(std::string key, std::vector<std::string> tags,
                                MaterialRouteClass route = MaterialRouteClass::forward_opaque) {
    MaterialDrawSubject subject;
    subject.key = std::move(key);
    subject.name = subject.key;
    subject.tags = std::move(tags);
    subject.route = route;
    subject.contract = route == MaterialRouteClass::deferred_geometry ? MaterialShaderContract::gbuffer_v1
                                                                      : MaterialShaderContract::forward_scene_color_v1;
    return subject;
}

// forward_opaque written in the graph and drawn by its loop, with the loop's
// parameters laid over the default (null removes one) and more definitions.
nlohmann::json lp3aForwardLoop(const nlohmann::json &loop_parameters, const nlohmann::json &definitions) {
    using Json = nlohmann::json;
    auto document = lp3aDecomposed(lp3aHybridRoot(Json::array()), true,
                                   {{"forward_opaque", {{"before", "__snapshot_opaque_color"}}}});
    for (const auto &[name, body] : definitions.items()) document["definitions"][name] = body;
    for (auto &node : document["nodes"])
        if (node.at("id") == "forward_opaque_draws")
            for (const auto &[key, value] : loop_parameters.items()) {
                if (value.is_null()) node["parameters"].erase(key);
                else node["parameters"][key] = value;
            }
    return document;
}

// A draw body: the steps chained by `after`, the last one its `draws`.
nlohmann::json lp3aDrawBody(const nlohmann::json &steps) {
    using Json = nlohmann::json;
    Json nodes = Json::array();
    std::string previous;
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const auto &step = steps.at(index);
        const auto id = "s" + std::to_string(index);
        Json node{{"id", id}, {"op", step.at("op")}, {"parameters", step.value("parameters", Json::object())},
                  {"outputs", {"draws"}}};
        if (!previous.empty()) node["inputs"] = {{"after", {{"node", previous}, {"port", "draws"}}}};
        nodes.push_back(std::move(node));
        previous = id;
    }
    return Json{{"nodes", std::move(nodes)}, {"outputs", {{"draws", {{"node", previous}, {"port", "draws"}}}}}};
}
} // namespace

TEST_CASE("LP-3a the hybrid material passes written in the graph compose as the preset's own",
          "[render-feature][graph-composition][lp3a]") {
    using Json = nlohmann::json;
    for (const auto &features : {Json::array({"engine://features/shadow_directional.json",
                                              "engine://features/sky_ambient.json"}),
                                 Json::array({"engine://features/bloom.json"})}) {
        INFO(features.dump());
        const auto one_piece = lp0aCompose(lp3aHybridRoot(features));
        for (const bool with_loop : {true, false}) {
            INFO(with_loop);
            const auto decomposed = lp0aCompose(lp3aDecomposed(lp3aHybridRoot(features), with_loop));
            // The same passes in the same places, planned in the same order.
            CHECK(lp0bDeclared(decomposed.config) == lp0bDeclared(one_piece.config));
            CHECK(lp0bPlanOrder(decomposed) == lp0bPlanOrder(one_piece));
            for (const auto *pass : {"deferred_geometry", "forward_opaque", "forward_transparent"}) {
                INFO(pass);
                CHECK(lp3aPassFields(decomposed.config, pass) == lp3aPassFields(one_piece.config, pass));
            }
            // A placed material pass is picked by the surface resources like
            // the preset's own (the LP-0b hole, docs/design_graph_loops.md 14.6).
            CHECK(decomposed.surface_resource_contracts == one_piece.surface_resource_contracts);
            if (features.at(0) == "engine://features/shadow_directional.json") {
                CHECK(passByName(decomposed.config, "forward_opaque").at("surface_resources") ==
                      Json{{"directional_shadow", "shadow_map"}});
                CHECK(passByName(decomposed.config, "forward_transparent").at("surface_resources") ==
                      Json{{"directional_shadow", "shadow_map"}});
            }
            REQUIRE_NOTHROW(compileComposition(decomposed));
            if (!with_loop) {
                // draws unconnected: today's drawing, no rule, no loop.
                CHECK(decomposed.material_draw_loops == nullptr);
                CHECK(decomposed.graph_loops.empty());
                continue;
            }
            REQUIRE(decomposed.material_draw_loops != nullptr);
            CHECK(decomposed.material_draw_loops->passes.size() == 3);
            // Every material of each pass's contract is drawn once, itself.
            const auto &rules = decomposed.material_draw_loops->passes;
            CHECK(rules.at("forward_opaque").rule(lp3aSubject("a#m", {})) ==
                  std::optional{MaterialDrawStepList{MaterialDrawStep::material()}});
            CHECK(rules.at("deferred_geometry").rule(lp3aSubject("a#m", {}, MaterialRouteClass::deferred_geometry)) ==
                  std::optional{MaterialDrawStepList{MaterialDrawStep::material()}});
            // Another contract's material is not the loop's: the pass's
            // default drawing (which draws none of it) stays.
            CHECK(rules.at("forward_opaque").rule(lp3aSubject("a#m", {}, MaterialRouteClass::deferred_geometry)) ==
                  std::nullopt);
            const auto compiled = compileComposition(decomposed);
            CHECK(compiled.material_draw_loops == decomposed.material_draw_loops);
            // loops[]: one draw_loop per pass, named by its loop, with the
            // pass it draws in; the elements are the running generation's.
            const auto plan = lp1aPlanJson(decomposed);
            REQUIRE(plan.contains("loops"));
            CHECK(plan.at("loops") == Json::array({
                {{"node", "deferred_geometry_draws"}, {"tier", "draw_loop"}, {"count", 0}, {"elements", Json::array()},
                 {"unmatched_branches", Json::array()}, {"pass", "deferred_geometry"}},
                {{"node", "forward_opaque_draws"}, {"tier", "draw_loop"}, {"count", 0}, {"elements", Json::array()},
                 {"unmatched_branches", Json::array()}, {"pass", "forward_opaque"}},
                {{"node", "forward_transparent_draws"}, {"tier", "draw_loop"}, {"count", 0},
                 {"elements", Json::array()}, {"unmatched_branches", Json::array()}, {"pass", "forward_transparent"}},
            }));
        }
    }
}

TEST_CASE("LP-3a a draw_loop's branches choose each material's draw steps",
          "[render-feature][graph-composition][lp3a]") {
    using Json = nlohmann::json;
    // outline materials: a cull-front draw, then the material; one named
    // material: nothing (a null body); every other: the material.
    Json where = Json::object();
    where["tags"] = {{"exclude", {"hidden"}}, {"require_all", {"lit"}}};
    Json outline_when = Json::object();
    outline_when["tags"] = {{"include", {"outline"}}};
    Json loop_parameters = Json::object();
    loop_parameters["where"] = where;
    loop_parameters["branches"] = Json::array({
        Json{{"when", outline_when}, {"body", "twice"}},
        Json{{"when", {{"material", "m.json#skip"}}}, {"body", nullptr}},
        Json{{"body", "forward_opaque_draw"}}});
    const auto document = lp3aForwardLoop(
        loop_parameters,
        {{"twice", lp3aDrawBody(Json::array({
             {{"op", "draw.override"}, {"parameters", {{"render_state", {{"cull", "front"}}}}}},
             {{"op", "draw.material"}}}))}});
    const auto composed = lp0aCompose(document);
    // where's include / exclude is the pass's queue filter; require_all is
    // the loop's alone.
    CHECK(passByName(composed.config, "forward_opaque").at("material_filter") == Json{{"exclude", {"hidden"}}});
    REQUIRE(composed.material_draw_loops != nullptr);
    const auto &rule = composed.material_draw_loops->passes.at("forward_opaque");
    CHECK(rule.node == "forward_opaque_draws");
    CHECK(rule.branch_count == 3);
    MaterialDrawRenderStateOverride cull_front;
    cull_front.cull = SurfaceCullMode::front;
    const MaterialDrawStepList twice{MaterialDrawStep::overridden(cull_front), MaterialDrawStep::material()};
    const auto outlined = lp3aSubject("m.json#outlined", {"lit", "outline"});
    const auto skipped = lp3aSubject("m.json#skip", {"lit"});
    const auto plain = lp3aSubject("asset.glb#material[0]", {"lit"});
    const auto unlit = lp3aSubject("m.json#unlit", {});
    const auto hidden = lp3aSubject("m.json#hidden", {"lit", "hidden"});
    const auto deferred = lp3aSubject("m.json#deferred", {"lit"}, MaterialRouteClass::deferred_geometry);
    CHECK(rule.rule(outlined) == std::optional{twice});
    CHECK(rule.rule(skipped) == std::optional{MaterialDrawStepList{}});
    CHECK(rule.rule(plain) == std::optional{MaterialDrawStepList{MaterialDrawStep::material()}});
    // In the queue range but outside require_all: drawn nothing.
    CHECK(rule.rule(unlit) == std::optional{MaterialDrawStepList{}});
    // Outside the queue filter or the contract: the default drawing, which
    // the queue or the route leaves out.
    CHECK(rule.rule(hidden) == std::nullopt);
    CHECK(rule.rule(deferred) == std::nullopt);
    REQUIRE(rule.classify(outlined).has_value());
    CHECK(rule.classify(outlined)->branch == 0);
    REQUIRE(rule.classify(skipped).has_value());
    CHECK(rule.classify(skipped)->branch == 1);
    CHECK(rule.classify(skipped)->served);
    REQUIRE(rule.classify(plain).has_value());
    CHECK(rule.classify(plain)->branch == 2);
    CHECK_FALSE(rule.classify(unlit).has_value());

    // get_frame_plan: the elements are the materials in over and where.
    Json plan{{"loops", Json::array({{{"node", "forward_opaque_draws"}, {"tier", "draw_loop"}, {"count", 0},
                                      {"elements", Json::array()}, {"unmatched_branches", Json::array()},
                                      {"pass", "forward_opaque"}}})}};
    auto filled = plan;
    applyMaterialDrawLoopElementsToFramePlanJson(filled, *composed.material_draw_loops,
                                                 {outlined, unlit, skipped, hidden, plain, deferred});
    CHECK(filled.at("loops").at(0) == Json{
        {"node", "forward_opaque_draws"}, {"tier", "draw_loop"}, {"count", 3},
        {"elements", Json::array({{{"element", "m.json#outlined"}, {"index", 0}, {"branch", 0}},
                                  {{"element", "m.json#skip"}, {"index", 1}, {"branch", 1}},
                                  {{"element", "asset.glb#material[0]"}, {"index", 2}, {"branch", 2}}})},
        {"unmatched_branches", Json::array()}, {"pass", "forward_opaque"}});
    applyMaterialDrawLoopElementsToFramePlanJson(plan, *composed.material_draw_loops, {plain});
    CHECK(plan.at("loops").at(0).at("unmatched_branches") == Json::array({0, 1}));

    SECTION("when compares the material's own attributes, every key at once") {
        GraphLoopDrawCondition when;
        when.contract = "forward_scene_color_v1";
        when.skinned = true;
        when.has_variant_named = "silhouette";
        auto subject = lp3aSubject("m.json#a", {});
        CHECK_FALSE(graphLoopDrawConditionMatches(when, subject));
        subject.skinned = true;
        CHECK_FALSE(graphLoopDrawConditionMatches(when, subject));
        subject.variants = {"silhouette"};
        CHECK(graphLoopDrawConditionMatches(when, subject));
        when.has_variant = false;
        CHECK_FALSE(graphLoopDrawConditionMatches(when, subject));
    }
    SECTION("the steps of every draw node, and an override's render state") {
        const auto every = lp3aForwardLoop(
            {{"branches", Json::array({{{"body", "all"}}})}},
            {{"all", lp3aDrawBody(Json::array({
                 {{"op", "draw.variant"}, {"parameters", {{"variant", "silhouette"}}}},
                 {{"op", "draw.override"},
                  {"parameters", {{"render_state", {{"blend", "opaque"}, {"depth_write", false},
                                                    {"depth_compare", "less_equal"}}},
                                  {"defines", {"LP3A_MARK"}}}}},
                 {{"op", "draw.material"}}}))}});
        const auto steps = lp0aCompose(every).material_draw_loops->passes.at("forward_opaque").rule(plain);
        MaterialDrawRenderStateOverride state;
        state.blend = SurfaceBlendMode::opaque;
        state.depth_write = false;
        state.depth_compare = SurfaceDepthCompare::less_equal;
        CHECK(steps == std::optional{MaterialDrawStepList{MaterialDrawStep::named("silhouette"),
                                                          MaterialDrawStep::overridden(state, {"LP3A_MARK"}),
                                                          MaterialDrawStep::material()}});
    }
}

TEST_CASE("LP-3a a draw_loop leaves no material of its over and where unserved",
          "[render-feature][graph-composition][lp3a]") {
    // A branch with a body must draw; "body": null is a decision not to
    // (docs/design_graph_loops.md 4.2.5).
    GraphDrawLoop loop;
    loop.loop = "loop";
    loop.pass = "forward_opaque";
    loop.contract = MaterialPassContract::forward_opaque_v1;
    GraphDrawLoop::Branch skipped;
    skipped.when.tags = GraphLoopTagFilter{{"skip"}, {}, {}};
    GraphDrawLoop::Branch empty;
    empty.when.tags = GraphLoopTagFilter{{"empty"}, {}, {}};
    empty.steps = MaterialDrawStepList{};
    GraphDrawLoop::Branch drawn;
    drawn.steps = MaterialDrawStepList{MaterialDrawStep::material()};
    loop.branches = {skipped, empty, drawn};
    MaterialDrawLoopRules rules;
    rules.passes.emplace("forward_opaque", makeGraphDrawLoopPassRule(loop));
    const std::vector<MaterialDrawSubject> subjects{
        lp3aSubject("m#skip", {"skip"}), lp3aSubject("m#empty", {"empty"}), lp3aSubject("m#plain", {}),
        lp3aSubject("m#empty_deferred", {"empty"}, MaterialRouteClass::deferred_geometry)};
    CHECK(materialDrawLoopUnservedSubjects(rules, subjects) == std::vector<std::size_t>{1});
    // The generation refuses with today's code, the material under its route.
    const auto message = materialRouteUnservedMessage(
        {UnservedMaterialRoute{MaterialRouteClass::forward_opaque, MaterialShaderContract::forward_scene_color_v1,
                               {describeUnservedMaterial("empty", "m", "m")}}},
        "project://passes/main.json");
    const auto refusal = splitCodedRefusalMessage(message);
    REQUIRE(refusal.has_value());
    CHECK(refusal->code == "material_route_unserved@1");
}

TEST_CASE("LP-3a draw_loop refusals are named", "[render-feature][graph-composition][lp3a]") {
    using Json = nlohmann::json;
    const auto expect = [](const Json &document, const std::string &code, const Json &fields) {
        const auto [actual_code, actual_fields] = lp1aEditRefusal(document);
        CHECK(actual_code == code);
        CHECK(actual_fields == fields);
    };
    SECTION("the phase's own order only") {
        expect(lp3aForwardLoop({{"order", {{"by", "element"}}}}, Json::object()), "graph.loop_order_unsupported@1",
               {{"node", "forward_opaque_draws"}, {"order", {{"by", "element"}}}});
        REQUIRE_NOTHROW(partitionGraphDocument(lp3aForwardLoop({{"order", nullptr}}, Json::object())));
    }
    SECTION("no legacy G-buffer pass") {
        expect(lp3aForwardLoop({{"over", {{"draw_items", {{"contract", "legacy_gbuffer_v1"}}}}}}, Json::object()),
               "graph.loop_contract_unsupported@1",
               {{"node", "forward_opaque_draws"}, {"contract", "legacy_gbuffer_v1"}});
    }
    SECTION("draws reach exactly one material pass") {
        auto alone = lp3aForwardLoop(Json::object(), Json::object());
        for (auto &item : alone["nodes"])
            if (item.at("id") == "forward_opaque") item["inputs"].erase("draws");
        expect(alone, "graph.loop_draws_unconnected@1", {{"node", "forward_opaque_draws"}, {"count", 0}});
        auto twice = lp3aForwardLoop(Json::object(), Json::object());
        Json copy;
        for (const auto &item : twice["nodes"])
            if (item.at("id") == "forward_opaque") copy = item;
        copy["id"] = "forward_opaque_again";
        copy["parameters"]["insert"] = {{"after", "forward_opaque"}};
        twice["nodes"].push_back(std::move(copy));
        expect(twice, "graph.loop_draws_unconnected@1", {{"node", "forward_opaque_draws"}, {"count", 2}});
        // Over another contract than the pass's.
        CHECK_THROWS_AS(partitionGraphDocument(lp3aForwardLoop(
                            {{"over", {{"draw_items", {{"contract", "forward_transparent_v1"}}}}}}, Json::object())),
                        std::invalid_argument);
    }
    SECTION("a draw body is a loop body only") {
        auto document = lp3aForwardLoop(Json::object(), Json::object());
        document["nodes"].push_back({{"id", "misuse"}, {"op", "graph.instance"}, {"definition", "forward_opaque_draw"}});
        expect(document, "graph.definition_role@1", {{"definition", "forward_opaque_draw"}, {"node", "misuse"}});
        auto root = lp3aForwardLoop(Json::object(), Json::object());
        root["nodes"].push_back({{"id", "stray"}, {"op", "draw.material"}, {"parameters", Json::object()},
                                 {"outputs", {"draws"}}});
        CHECK_THROWS_AS(partitionGraphDocument(root), std::invalid_argument);
    }
    SECTION("what one material pass cannot run") {
        const auto reason = [&](const Json &parameters, const Json &definitions, const char *expected) {
            INFO(expected);
            expect(lp3aForwardLoop(parameters, definitions), "graph.loop_tier_unsupported@1",
                   {{"node", "forward_opaque_draws"}, {"reason", expected}});
        };
        reason({{"carry", Json::array({{{"from", "draws"}, {"to", "after"}}})}}, Json::object(), "carry_in_draw_loop");
        reason({{"limit", 2}}, Json::object(), "limit_in_draw_loop");
        auto resource = lp3aDrawBody(Json::array({{{"op", "draw.material"}}}));
        resource["nodes"].push_back(lp0bImage("scratch"));
        reason({{"branches", Json::array({{{"body", "resource"}}})}}, {{"resource", resource}}, "resource_in_draw_body");
        auto mixed = lp3aDrawBody(Json::array({{{"op", "draw.material"}}}));
        mixed["nodes"].push_back(lp1aLit());
        reason({{"branches", Json::array({{{"body", "mixed"}}})}}, {{"mixed", mixed}}, "mixed_body");
        expect(lp3aForwardLoop({{"tier", "expand"}}, Json::object()), "graph.loop_tier_mismatch@1",
               {{"node", "forward_opaque_draws"}, {"tier", "expand"}, {"actual", "draw_loop"}});
        REQUIRE_NOTHROW(partitionGraphDocument(lp3aForwardLoop({{"tier", "draw_loop"}}, Json::object())));
    }
}

namespace {
// LP-0d: shadow_directional over the hybrid preset (cascade_count 3), its
// shadow_depth replaced by what the graph draws into the same target. Until
// LP-0e brings render.shadow_depth, the stand-in is a render.raster in the
// $shadow/directional view family with a depth attachment to shadow_map
// through a render.target_ref.
nlohmann::json lp0dDepthType() {
    return {{"semantic", "pelican.render.depth@1"},
            {"arguments", {{"representation", "device"}, {"space", "projection"}, {"topology", "two_d_array"}}}};
}

nlohmann::json lp0dShadowRef() {
    return {{"id", "shadow"}, {"op", "render.target_ref"},
            {"parameters", {{"name", "shadow_map"}, {"type", lp0dDepthType()}}}, {"outputs", {"resource"}}};
}

nlohmann::json lp0dRaster(const std::string &id, const nlohmann::json &depth_source,
                          const nlohmann::json &extra = nlohmann::json::object()) {
    using Json = nlohmann::json;
    Json parameters{{"pass_set", "main_render"},
                    {"shader", {{"vertex", "engine://shadow_depth"}}},
                    {"view_family", "$shadow/directional"},
                    {"draw", {{"vertex_count", 3}}},
                    {"ports", {{"depth", {{"kind", "image"}, {"type", lp0dDepthType()}, {"access", "write"},
                                          {"attachment", "depth"}}}}}};
    for (const auto &[key, value] : extra.items()) parameters[key] = value;
    return Json{{"id", id}, {"op", "render.raster"}, {"parameters", std::move(parameters)},
                {"inputs", {{"depth", depth_source}}}, {"outputs", {"depth"}}};
}

// A loop body that draws the depth its `target` input is given.
nlohmann::json lp0dCascadeBody() {
    using Json = nlohmann::json;
    return Json{{"nodes", Json::array({{{"id", "target"}, {"op", "graph.input"}},
                                       lp0dRaster("depth", {{"node", "target"}, {"port", "value"}})})},
                {"outputs", Json::object()}};
}

nlohmann::json lp0dShadowFeature(const nlohmann::json &replacements) {
    auto feature = nlohmann::json{{"ref", "engine://features/shadow_directional.json"},
                                  {"parameters", nlohmann::json::parse(R"({"cascade_count": 3})")}};
    if (!replacements.is_null()) feature["pass_replacements"] = replacements;
    return feature;
}

nlohmann::json lp0dGraph(nlohmann::json nodes, const nlohmann::json &replacements,
                         nlohmann::json definitions = nlohmann::json::object()) {
    using Json = nlohmann::json;
    return Json{{"schema", "pelican.graph"}, {"version", 1}, {"definitions", std::move(definitions)},
                {"nodes", std::move(nodes)}, {"outputs", Json::object()},
                {"rendering", {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
                               {"features", Json::array({lp0dShadowFeature(replacements)})}}}};
}

// `probe` is placed after shadow_depth, `early` before it.
nlohmann::json lp0dNeighbours() {
    using Json = nlohmann::json;
    return Json::array({lp1aLit(), lp0bImage("probe_image"),
                        lp0bPass("probe", "lit", "probe_image", {{"insert", {{"after", "shadow_depth"}}}}),
                        lp0bImage("early_image"),
                        lp0bPass("early", "lit", "early_image", {{"insert", {{"before", "shadow_depth"}}}})});
}

const nlohmann::json &lp0dContract(const nlohmann::json &contracts, std::string_view name) {
    for (const auto &contract : contracts)
        if (contract.at("contract").get<std::string>() == name) return contract;
    throw std::runtime_error("no surface resource contract " + std::string{name});
}
} // namespace

TEST_CASE("LP-0d pass_replacements puts a graph pass where the feature's pass stood",
          "[render-feature][graph-composition][lp0d]") {
    using Json = nlohmann::json;
    auto nodes = lp0dNeighbours();
    nodes.push_back(lp0dShadowRef());
    nodes.push_back(lp0dRaster("cascade_depth", {{"node", "shadow"}, {"port", "resource"}}));
    // The same graph without the replacement: shadow_depth draws, and the
    // graph pass is appended with the other scene passes.
    const auto kept = lp0aCompose(lp0dGraph(nodes, nullptr));
    const auto composed = lp0aCompose(lp0dGraph(nodes, {{"shadow_depth", "cascade_depth"}}));
    const auto &config = composed.config;

    // shadow_depth leaves the frame plan; cascade_depth stands where it stood.
    auto expected = lp0bDeclared(kept.config);
    expected.erase(std::find(expected.begin(), expected.end(), "cascade_depth"));
    *std::find(expected.begin(), expected.end(), "shadow_depth") = "cascade_depth";
    CHECK(lp0bDeclared(config) == expected);
    const auto order = lp0bPlanOrder(composed);
    CHECK(std::find(order.begin(), order.end(), "shadow_depth") == order.end());
    const auto kept_order = lp0bPlanOrder(kept);
    CHECK(lp0bIndex(order, "cascade_depth") < lp0bIndex(order, "forward_opaque"));
    CHECK(lp0bIndex(kept_order, "shadow_depth") < lp0bIndex(kept_order, "forward_opaque"));

    // What named shadow_depth names the replacement.
    CHECK(lp0bListed(passByName(config, "probe").at("after"), "cascade_depth"));
    CHECK(lp0bListed(passByName(config, "early").at("before"), "cascade_depth"));
    CHECK(config.dump().find("\"shadow_depth\"") == std::string::npos);

    // The surface resource's producer follows; the provider keeps the
    // feature's cascade_count and its target.
    const auto &contract = lp0dContract(composed.surface_resource_contracts, "directional_shadow");
    CHECK(contract.at("producer") == "cascade_depth");
    CHECK(contract.at("resource") == "shadow_map");
    CHECK(contract.at("provider_feature") == "shadow_directional");
    CHECK(lp0dContract(kept.surface_resource_contracts, "directional_shadow").at("producer") == "shadow_depth");
    const auto instance = std::find_if(composed.feature_instances.begin(), composed.feature_instances.end(),
        [](const Json &entry) { return entry.at("feature") == "shadow_directional"; });
    REQUIRE(instance != composed.feature_instances.end());
    CHECK(instance->at("parameters").at("cascade_count") == 3);
    // The instance's pass_replacements is authoring, not a resolved field.
    CHECK_FALSE(instance->contains("pass_replacements"));
    const auto compiled = compileComposition(composed);
    const auto compiled_contract = std::find_if(compiled.surface_resource_contracts.begin(),
        compiled.surface_resource_contracts.end(),
        [](const auto &entry) { return entry.contract.name == "directional_shadow"; });
    REQUIRE(compiled_contract != compiled.surface_resource_contracts.end());
    CHECK(compiled_contract->producer == "cascade_depth");
    CHECK(compiled_contract->resource == "shadow_map");
    const auto shadow_target = std::find_if(config.at("render_targets").begin(), config.at("render_targets").end(),
        [](const Json &target) { return target.at("name") == "shadow_map"; });
    REQUIRE(shadow_target != config.at("render_targets").end());
    CHECK(shadow_target->at("layers") == 3);
}

TEST_CASE("LP-0d a graph.loop replaces a pass with its expansion, ordered by its first and last copy",
          "[render-feature][graph-composition][lp0d]") {
    using Json = nlohmann::json;
    auto nodes = lp0dNeighbours();
    nodes.push_back(lp0dShadowRef());
    nodes.push_back(lp1aLoop("cascades", lp1aRange(3, "cascade"), {{"target", {{"node", "shadow"}, {"port", "resource"}}}}));
    const Json definitions{{"cascade", lp0dCascadeBody()}};
    const auto kept = lp0aCompose(lp0dGraph(nodes, nullptr, definitions));
    const auto composed = lp0aCompose(lp0dGraph(nodes, {{"shadow_depth", "cascades"}}, definitions));
    const auto &config = composed.config;
    const std::vector<std::string> copies{"cascades/0/depth", "cascades/1/depth", "cascades/2/depth"};

    // The copies, in element order, stand where shadow_depth stood.
    const auto declared = lp0bDeclared(config);
    // Where shadow_depth stands when the copies are declared elsewhere.
    auto kept_declared = lp0bDeclared(kept.config);
    std::erase_if(kept_declared, [](const std::string &name) { return name.starts_with("cascades/"); });
    const auto slot = lp0bIndex(kept_declared, "shadow_depth");
    CHECK(std::find(declared.begin(), declared.end(), "shadow_depth") == declared.end());
    for (std::size_t element = 0; element < copies.size(); ++element)
        CHECK(lp0bIndex(declared, copies[element]) == slot + static_cast<std::ptrdiff_t>(element));
    // Each copy draws the one target, so each waits on the one before.
    CHECK(lp0bListed(passByName(config, copies[1]).at("after"), copies[0]));
    CHECK(lp0bListed(passByName(config, copies[2]).at("after"), copies[1]));

    // After shadow_depth is after the last copy, before it before the first.
    const auto &probe_after = passByName(config, "probe").at("after");
    CHECK(lp0bListed(probe_after, copies.back()));
    CHECK_FALSE(lp0bListed(probe_after, copies.front()));
    const auto &early_before = passByName(config, "early").at("before");
    CHECK(lp0bListed(early_before, copies.front()));
    CHECK_FALSE(lp0bListed(early_before, copies.back()));
    CHECK(lp0dContract(composed.surface_resource_contracts, "directional_shadow").at("producer") == copies.back());

    const auto order = lp0bPlanOrder(composed);
    CHECK(lp0bIndex(order, copies[0]) < lp0bIndex(order, copies[1]));
    CHECK(lp0bIndex(order, copies[1]) < lp0bIndex(order, copies[2]));
    CHECK(lp0bIndex(order, copies[2]) < lp0bIndex(order, "probe"));
    CHECK(lp0bIndex(order, "early") < lp0bIndex(order, copies[0]));
    REQUIRE_NOTHROW(compileComposition(composed));
}

namespace {
// LP-0e: render.shadow_depth drawing into what its `depth` socket is given, as
// shadow_directional's shadow_depth pass draws into shadow_map.
nlohmann::json lp0eShadowDepth(const std::string &id, const nlohmann::json &depth_source,
                               const nlohmann::json &extra = nlohmann::json::object()) {
    using Json = nlohmann::json;
    Json parameters{{"pass_set", "main_render"},
                    {"view_family", "$shadow/directional"},
                    {"depth_store_op", "store"},
                    {"shader", {{"vertex", "engine://shadow_depth"}}}};
    for (const auto &[key, value] : extra.items()) parameters[key] = value;
    return Json{{"id", id}, {"op", "render.shadow_depth"}, {"parameters", std::move(parameters)},
                {"inputs", {{"depth", depth_source}}}, {"outputs", Json::array()}};
}

nlohmann::json lp0eSnapshotCopy(const std::string &id, const nlohmann::json &source,
                                const nlohmann::json &destination, const nlohmann::json &parameters) {
    return {{"id", id}, {"op", "render.snapshot_copy"}, {"parameters", parameters},
            {"inputs", {{"source", source}, {"destination", destination}}}, {"outputs", nlohmann::json::array()}};
}

nlohmann::json lp0eColorType(const std::string &topology) {
    auto type = lp0aSceneColorType();
    type["arguments"]["topology"] = topology;
    return type;
}

nlohmann::json lp0eTargetRef(const std::string &id, const std::string &name, const nlohmann::json &type) {
    return {{"id", id}, {"op", "render.target_ref"}, {"parameters", {{"name", name}, {"type", type}}},
            {"outputs", {"resource"}}};
}

// The hybrid preset with cube_capture, whose opaque color copy `replacement`
// (when given) takes the place of.
nlohmann::json lp0eCubeGraph(nlohmann::json nodes, const nlohmann::json &replacement) {
    using Json = nlohmann::json;
    Json feature{{"ref", "engine://features/cube_capture.json"}};
    if (!replacement.is_null())
        feature["pass_replacements"] = {{"cube_capture_snapshot_opaque_color", replacement}};
    return Json{{"schema", "pelican.graph"}, {"version", 1}, {"definitions", Json::object()},
                {"nodes", std::move(nodes)}, {"outputs", Json::object()},
                {"rendering", {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
                               {"features", Json::array({feature})}}}};
}

// The cube capture's opaque color copy, as a graph node.
nlohmann::json lp0eCubeCopyNodes() {
    using Json = nlohmann::json;
    return Json::array({lp0eTargetRef("capture_color", "cube_capture_color", lp0eColorType("cube")),
                        lp0eTargetRef("opaque_color", "cube_capture_opaque_color", lp0eColorType("two_d_array")),
                        lp0eSnapshotCopy("opaque_color_copy", {{"node", "capture_color"}, {"port", "resource"}},
                                         {{"node", "opaque_color"}, {"port", "resource"}},
                                         {{"pass_set", "main_render"}, {"view_family", "$capture/cube"},
                                          {"snapshot_after", "cube_capture_forward_opaque"}})});
}

// A main_render frame plan with `from` renamed `to`, less what says who
// authored each node (a feature's pass and a graph node differ only there).
nlohmann::json lp0eComparablePlan(const nlohmann::json &plan, const std::string &from, const std::string &to) {
    using Json = nlohmann::json;
    const auto rename = [&](Json &value) {
        if (value.is_string() && value.get<std::string>() == from) value = to;
    };
    Json result{{"nodes", plan.at("nodes")}, {"barriers", plan.at("barriers")}, {"levels", plan.at("levels")}};
    for (auto &node : result["nodes"]) {
        for (const auto *key : {"source", "provider_feature", "provider_ref", "authored_name"}) node.erase(key);
        rename(node["name"]);
        if (node.contains("snapshot_after")) rename(node["snapshot_after"]);
    }
    for (auto &barrier : result["barriers"]) {
        rename(barrier["from"]);
        rename(barrier["to"]);
    }
    for (auto &level : result["levels"])
        for (auto &name : level) rename(name);
    return result;
}
} // namespace

TEST_CASE("LP-0e render.shadow_depth takes shadow_depth's place as the same pass",
          "[render-feature][graph-composition][lp0e][shadow]") {
    using Json = nlohmann::json;
    const Json nodes = Json::array({lp0dShadowRef(),
                                    lp0eShadowDepth("graph_shadow", {{"node", "shadow"}, {"port", "resource"}})});
    const auto kept = lp0aCompose(lp0dGraph(Json::array(), nullptr));
    const auto composed = lp0aCompose(lp0dGraph(nodes, {{"shadow_depth", "graph_shadow"}}));

    // The feature's pass, named by the node, where the feature's stood.
    auto expected = passByName(kept.config, "shadow_depth");
    expected["name"] = "graph_shadow";
    CHECK(passByName(composed.config, "graph_shadow") == expected);
    auto declared = lp0bDeclared(kept.config);
    *std::find(declared.begin(), declared.end(), "shadow_depth") = "graph_shadow";
    CHECK(lp0bDeclared(composed.config) == declared);
    CHECK(lp0dContract(composed.surface_resource_contracts, "directional_shadow").at("producer") == "graph_shadow");

    // The same frame plan: node, level, reads, writes, view axes, barriers.
    CHECK(lp0eComparablePlan(lp1aPlanJson(composed), "graph_shadow", "graph_shadow") ==
          lp0eComparablePlan(lp1aPlanJson(kept), "shadow_depth", "graph_shadow"));

    SECTION("without a replacement it is one more pass of its family") {
        const auto both = lp0aCompose(lp0dGraph(nodes, nullptr));
        CHECK(passByName(both.config, "graph_shadow").at("type") == "shadow_depth");
        CHECK(passByName(both.config, "shadow_depth").at("type") == "shadow_depth");
    }
    SECTION("its insert places it, as for any graph pass") {
        auto placed = nodes;
        placed[1]["parameters"]["insert"] = {{"after", "shadow_depth"}};
        const auto result = lp0aCompose(lp0dGraph(placed, nullptr));
        const auto order = lp0bDeclared(result.config);
        CHECK(lp0bIndex(order, "graph_shadow") == lp0bIndex(order, "shadow_depth") + 1);
    }
}

TEST_CASE("LP-0e render.snapshot_copy takes a cube capture copy's place as the same copy",
          "[render-feature][graph-composition][lp0e][snapshot][cube]") {
    using Json = nlohmann::json;
    const auto kept = lp0aCompose(lp0eCubeGraph(Json::array(), nullptr));
    const auto composed = lp0aCompose(lp0eCubeGraph(lp0eCubeCopyNodes(), "opaque_color_copy"));

    auto expected = passByName(kept.config, "cube_capture_snapshot_opaque_color");
    expected["name"] = "opaque_color_copy";
    CHECK(passByName(composed.config, "opaque_color_copy") == expected);
    auto declared = lp0bDeclared(kept.config);
    *std::find(declared.begin(), declared.end(), "cube_capture_snapshot_opaque_color") = "opaque_color_copy";
    CHECK(lp0bDeclared(composed.config) == declared);
    // The same frame plan: the copy node (reads, writes, snapshot_after, byte
    // size, face axis), its level and the barriers around it.
    const auto plan = lp1aPlanJson(composed);
    CHECK(lp1aPlanNode(plan, "opaque_color_copy")->at("kind") == "snapshot_copy");
    CHECK(lp0eComparablePlan(plan, "opaque_color_copy", "opaque_color_copy") ==
          lp0eComparablePlan(lp1aPlanJson(kept), "cube_capture_snapshot_opaque_color", "opaque_color_copy"));

    SECTION("in a loop body, snapshot_after names the body's own pass per element") {
        const Json body{{"nodes", Json::array({{{"id", "target"}, {"op", "graph.input"}},
                                               {{"id", "snap"}, {"op", "graph.input"}},
                                               lp1aFullscreen("paint", {{"node", "target"}, {"port", "value"}},
                                                              {{"color_load_op", "Load"}}),
                                               lp0eSnapshotCopy("copy", {{"node", "target"}, {"port", "value"}},
                                                                {{"node", "snap"}, {"port", "value"}},
                                                                {{"pass_set", "main_render"},
                                                                 {"snapshot_after", "paint"}})})},
                        {"outputs", Json::object()}};
        const auto result = lp0aCompose(lp1aGraph(
            Json::array({lp1aLit(), lp0bImage("snap_image"),
                         lp1aLoop("copies", lp1aRange(2, "copy_body"),
                                  {{"target", {{"node", "lit"}, {"port", "resource"}}},
                                   {"snap", {{"node", "snap_image"}, {"port", "resource"}}}})}),
            Json{{"copy_body", body}}));
        const auto order = lp0bDeclared(result.config);
        for (const auto *element : {"0", "1"}) {
            INFO("element " << element);
            const auto prefix = std::string{"copies/"} + element + "/";
            const auto &copy = passByName(result.config, prefix + "copy");
            CHECK(copy.at("type") == "snapshot_copy");
            CHECK(copy.at("snapshot_after") == prefix + "paint");
            CHECK(copy.at("source") == "lit_color");
            CHECK(copy.at("destination") == "snap_image");
            CHECK(lp0bListed(copy.at("after"), prefix + "paint"));
            CHECK(lp0bIndex(order, prefix + "paint") < lp0bIndex(order, prefix + "copy"));
        }
        // Each element writes the one snap_image, so the second waits on the first.
        CHECK(lp0bListed(passByName(result.config, "copies/1/copy").at("after"), "copies/0/copy"));
    }
}

TEST_CASE("LP-0e a loop of render.shadow_depth selects one cascade per element in shadow_depth's place",
          "[render-feature][graph-composition][lp0e][shadow]") {
    using Json = nlohmann::json;
    const Json body{{"nodes", Json::array({{{"id", "target"}, {"op", "graph.input"}},
                                           lp0eShadowDepth("depth", {{"node", "target"}, {"port", "value"}},
                                                           {{"view_select", {{"cascade", {{"$expr", "element"}}}}}})})},
                    {"outputs", Json::object()}};
    auto nodes = Json::array({lp0dShadowRef(),
                              lp1aLoop("cascades", lp1aRange(3, "cascade"),
                                       {{"target", {{"node", "shadow"}, {"port", "resource"}}}})});
    const auto kept = lp0aCompose(lp0dGraph(Json::array(), nullptr));
    const auto composed = lp0aCompose(lp0dGraph(nodes, {{"shadow_depth", "cascades"}}, Json{{"cascade", body}}));
    const auto &config = composed.config;

    const auto declared = lp0bDeclared(config);
    const auto slot = lp0bIndex(lp0bDeclared(kept.config), "shadow_depth");
    CHECK(std::find(declared.begin(), declared.end(), "shadow_depth") == declared.end());
    for (std::int64_t element = 0; element < 3; ++element) {
        INFO("element " << element);
        const auto name = "cascades/" + std::to_string(element) + "/depth";
        const auto &pass = passByName(config, name);
        CHECK(pass.at("type") == "shadow_depth");
        CHECK(pass.at("view_family") == "$shadow/directional");
        CHECK(pass.at("view_select") == Json{{"cascade", element}});
        CHECK(pass.at("output") == Json{{"color", nullptr}, {"depth", "shadow_map"}});
        CHECK(lp0bIndex(declared, name) == slot + element);
    }
    CHECK(lp0dContract(composed.surface_resource_contracts, "directional_shadow").at("producer") ==
          "cascades/2/depth");
    // The frame plan carries each copy's cascade.
    const auto plan = lp1aPlanJson(composed);
    for (std::int64_t element = 0; element < 3; ++element)
        CHECK(lp1aPlanNode(plan, "cascades/" + std::to_string(element) + "/depth")->at("view_select") ==
              Json{{"cascade", element}});
}

TEST_CASE("LP-0e render.shadow_depth and render.snapshot_copy refusals",
          "[render-feature][graph-composition][lp0e]") {
    using Json = nlohmann::json;
    const auto shadow = [](const Json &extra) {
        return lp0dGraph(Json::array({lp0dShadowRef(),
                                      lp0eShadowDepth("graph_shadow", {{"node", "shadow"}, {"port", "resource"}},
                                                      extra)}),
                         nullptr);
    };
    SECTION("a field the pass does not own, or one the sockets decide") {
        for (const auto *field : {"draw", "output", "type", "name", "material_contract", "color_load_op"}) {
            INFO(field);
            CHECK_THROWS_AS(partitionGraphDocument(shadow({{field, "x"}})), std::invalid_argument);
        }
    }
    SECTION("an unconnected depth socket") {
        auto document = shadow(Json::object());
        document["nodes"][1]["inputs"] = Json::object();
        CHECK_THROWS_AS(partitionGraphDocument(document), std::invalid_argument);
    }
    SECTION("a canonical anchor that is not one") {
        const auto [code, fields] = lp0bEditRefusal(shadow({{"canonical_anchor", "nowhere"}}));
        CHECK(code == "graph.insert_anchor_unknown@1");
        CHECK(fields == Json{{"node", "graph_shadow"}, {"anchor", "nowhere"}});
    }
    SECTION("a copy without snapshot_after, or with a field it does not own") {
        auto nodes = lp0eCubeCopyNodes();
        nodes[2]["parameters"].erase("snapshot_after");
        CHECK_THROWS_AS(partitionGraphDocument(lp0eCubeGraph(nodes, nullptr)), std::invalid_argument);
        nodes = lp0eCubeCopyNodes();
        nodes[2]["parameters"]["shader"] = Json::object();
        CHECK_THROWS_AS(partitionGraphDocument(lp0eCubeGraph(nodes, nullptr)), std::invalid_argument);
    }
    SECTION("a target the composition does not declare") {
        auto nodes = lp0eCubeCopyNodes();
        nodes[1]["parameters"]["name"] = "no_such_target";
        const auto [code, fields] = lp0aRefusal(lp0eCubeGraph(nodes, nullptr));
        CHECK(code == "graph.target_ref_unknown@1");
        CHECK(fields == Json{{"node", "opaque_color"}, {"name", "no_such_target"}});
    }
}

TEST_CASE("LP-0d pass_replacements refusals", "[render-feature][graph-composition][lp0d]") {
    using Json = nlohmann::json;
    const Json nodes = Json::array({lp0dShadowRef(),
                                    lp0dRaster("cascade_depth", {{"node", "shadow"}, {"port", "resource"}}),
                                    lp1aLit(), lp0bImage("elsewhere_image"),
                                    lp0bPass("elsewhere", "lit", "elsewhere_image", Json::object())});

    SECTION("a replacement that does not write what the pass wrote is refused as a producer that no longer writes it") {
        try {
            (void)lp0aCompose(lp0dGraph(nodes, {{"shadow_depth", "elsewhere"}}));
            FAIL("the replacement was not refused");
        } catch (const GraphRegistryError &error) {
            FAIL(std::string{"refused by name: "} + error.what());
        } catch (const std::runtime_error &error) {
            const std::string message = error.what();
            INFO(message);
            CHECK(message.find("pass_replacements['shadow_depth']") != std::string::npos);
            CHECK(message.find("'elsewhere' no longer writes resource 'shadow_map'") != std::string::npos);
        }
    }
    SECTION("a pass the feature does not declare is refused by name") {
        for (const std::string pass : {"no_such_pass", "deferred_lighting"}) {
            INFO(pass);
            const auto [code, fields] = lp0aRefusal(lp0dGraph(nodes, {{pass, "cascade_depth"}}));
            CHECK(code == "render_feature.pass_replacement_unknown@1");
            CHECK(fields == Json{{"feature", "shadow_directional"}, {"pass", pass}});
        }
    }
    SECTION("a replacement that is no graph pass, task or loop is refused by name") {
        for (const std::string node : {"no_such_node", "shadow", "forward_opaque"}) {
            INFO(node);
            const auto [code, fields] = lp0aRefusal(lp0dGraph(nodes, {{"shadow_depth", node}}));
            CHECK(code == "graph.node_missing@1");
            CHECK(fields == Json{{"node", node}, {"feature", "shadow_directional"}, {"pass", "shadow_depth"}});
        }
        // A rendering config that is no graph has no node to name.
        const Json native{{"features", Json::array({lp0dShadowFeature({{"shadow_depth", "cascade_depth"}})})},
                          {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}};
        try {
            (void)lp0aCompose(native);
            FAIL("the native config was not refused");
        } catch (const GraphRegistryError &error) {
            CHECK(error.code() == "graph.node_missing@1");
        }
    }
    SECTION("the shape is checked") {
        for (const auto &replacements : {Json::array({"shadow_depth"}), Json{{"shadow_depth", 3}},
                                         Json{{"shadow_depth", ""}}, Json{{"", "cascade_depth"}}}) {
            INFO(replacements.dump());
            CHECK_THROWS_AS(lp0aCompose(lp0dGraph(nodes, replacements)), std::runtime_error);
        }
    }
}

namespace {
// LP-0d: the LP-0a graph (`blit` copies lit_color, `paint` writes bloom's
// threshold target through a render.target_ref) with `paint` replacing
// bloom's HighLuminanceExtraction, `glow` placed after bloom's last pass,
// `trail` placed after `glow`, and `early` ordered before `glow`. Bloom here
// keeps a history target, which the preview variant excludes.
nlohmann::json lp0dBloomVariantGraph() {
    using Json = nlohmann::json;
    auto document = lp0aTargetRefGraph();
    document["rendering"]["features"] = Json::array({{{"ref", "engine://features/bloom.json"},
        {"pass_replacements", {{"HighLuminanceExtraction", "paint"}}}}});
    document["nodes"].push_back(lp0bImage("glow_image"));
    document["nodes"].push_back(lp0bPass("glow", "lit", "glow_image", {{"insert", {{"after", "FinalBloomComposite"}}}}));
    document["nodes"].push_back(lp0bImage("trail_image"));
    document["nodes"].push_back(lp0bPass("trail", "lit", "trail_image", {{"insert", {{"after", "glow"}}}}));
    document["nodes"].push_back(lp0bImage("early_image"));
    document["nodes"].push_back(lp0bPass("early", "lit", "early_image", {{"before", Json::array({"glow"})}}));
    return document;
}

std::string lp0dHistoryBloom(std::string_view ref) {
    auto document = nlohmann::json::parse(loadEngineFeature(ref));
    if (ref == "engine://features/bloom.json") document["render_targets"][0]["history"] = true;
    return document.dump();
}

ResolvedRenderPipeline lp0dResolve(const nlohmann::json &document, RenderPipelineGraphVariant variant) {
    return resolveRenderPipeline(RenderPipelineRequest{document, "LP-0d variant"},
                                 RenderEnvironmentCapabilities{true, variant},
                                 RenderPipelineResolveDependencies{.load_feature_json = lp0dHistoryBloom,
                                                                   .load_pipeline_json = lp0dHistoryBloom});
}

bool lp0dDeclares(const nlohmann::json &config, std::string_view name) {
    for (const auto &family : config.at("rendering_passes"))
        for (const auto &pass : family.at("passes"))
            if (pass.value("name", std::string{}) == name) return true;
    return false;
}
} // namespace

TEST_CASE("LP-0d a graph node that depends on a feature the variant excluded is made inactive with it",
          "[render-feature][graph-composition][graph-variant][preview][lp0d]") {
    using Json = nlohmann::json;
    const auto document = lp0dBloomVariantGraph();

    // Flat keeps bloom: paint stands where HighLuminanceExtraction stood.
    const auto flat = lp0dResolve(document, RenderPipelineGraphVariant::flat);
    CHECK(flat.excluded_feature_names.empty());
    CHECK(flat.deactivated_graph_nodes.empty());
    for (const auto *name : {"blit", "paint", "glow", "trail", "early"}) CHECK(lp0dDeclares(flat.normalized_config, name));
    CHECK_FALSE(lp0dDeclares(flat.normalized_config, "HighLuminanceExtraction"));

    // Preview excludes bloom, and the recompose passes: what depends on bloom
    // is inactive, the rest stays.
    const auto preview = lp0dResolve(document, RenderPipelineGraphVariant::preview);
    CHECK(preview.excluded_feature_names == std::vector<std::string>{"bloom"});
    // `trail` goes with the pass it was placed after; `early` stays, and its
    // edge to `glow` leaves.
    CHECK(preview.deactivated_graph_nodes == std::vector<RenderGraphDeactivatedNode>{
        {"paint", "bloom", "pass_replacement"}, {"glow", "bloom", "insert"}, {"trail", "bloom", "insert"}});
    const auto &config = preview.normalized_config;
    for (const auto *name : {"paint", "glow", "trail", "HighLuminanceExtraction"})
        CHECK_FALSE(lp0dDeclares(config, name));
    CHECK(lp0dDeclares(config, "blit"));
    CHECK(lp0dDeclares(config, "early"));
    CHECK(config.dump().find("\"glow\"") == std::string::npos);
    for (const auto &[node, detail] : {std::pair{"paint", "preview:pass_replacement:bloom"},
                                       std::pair{"glow", "preview:insert:bloom"},
                                       std::pair{"trail", "preview:insert:bloom"}}) {
        INFO(node);
        CHECK(std::any_of(preview.diagnostics.begin(), preview.diagnostics.end(), [&](const auto &diagnostic) {
            return diagnostic.kind == RenderPipelineDiagnosticKind::graph_node_deactivated &&
                   diagnostic.subject == node && diagnostic.detail == detail;
        }));
    }
    REQUIRE_NOTHROW(compileRenderPipeline(preview));

    SECTION("a node that reaches bloom only through a render.target_ref") {
        auto plain = lp0aTargetRefGraph();
        const auto resolved = lp0dResolve(plain, RenderPipelineGraphVariant::preview);
        CHECK(resolved.deactivated_graph_nodes ==
              std::vector<RenderGraphDeactivatedNode>{{"paint", "bloom", "target_ref"}});
        CHECK_FALSE(lp0dDeclares(resolved.normalized_config, "paint"));
        CHECK(lp0dDeclares(resolved.normalized_config, "blit"));
    }
    SECTION("loops: one that draws into bloom's target, one placed by a bloom pass, whoever reads it") {
        auto placed = lp1aRange(2);
        placed["insert"] = {{"after", "FinalBloomComposite"}};
        auto later = lp1aRange(2);
        later["after"] = Json::array({"acc"});
        auto reads = lp0bPass("reads", "lit", "read_image", {{"canonical_anchor", "post_ldr"}});
        // `reads` samples the last image `steps` painted.
        reads["inputs"]["source"] = {{"node", "steps"}, {"port", "image"}};
        const auto looped = lp1aGraph(Json::array({
            {{"id", "threshold"}, {"op", "render.target_ref"},
             {"parameters", {{"name", "Bloom_Threshold_RT"}, {"type", lp0aSceneColorType()}}},
             {"outputs", {"resource"}}},
            lp1aLoop("acc", lp1aRange(2, "accumulate"), {{"target", {{"node", "threshold"}, {"port", "resource"}}}}),
            lp1aLoop("steps", placed, Json::object(), Json::array({"image"})),
            lp1aLoop("later", later),
            lp0bImage("read_image"), reads, lp1aLit()}));
        const auto flat_looped = lp0dResolve(looped, RenderPipelineGraphVariant::flat);
        CHECK(flat_looped.deactivated_graph_nodes.empty());
        const auto resolved = lp0dResolve(looped, RenderPipelineGraphVariant::preview);
        CHECK(resolved.deactivated_graph_nodes == std::vector<RenderGraphDeactivatedNode>{
            {"acc", "bloom", "target_ref"}, {"steps", "bloom", "insert"}, {"reads", "bloom", "loop_output"}});
        CHECK(lp1aNamesUnder(resolved.normalized_config, "acc/").empty());
        CHECK(lp1aNamesUnder(resolved.normalized_config, "steps/").empty());
        CHECK_FALSE(lp0dDeclares(resolved.normalized_config, "reads"));
        CHECK(lp1aNamesUnder(resolved.normalized_config, "later/").size() == 2);
        REQUIRE_NOTHROW(compileRenderPipeline(resolved));
    }
}

namespace {
// The hybrid preset with `features`, and nothing in the graph.
nlohmann::json lp1cOnePiece(const nlohmann::json &features) {
    using Json = nlohmann::json;
    return Json{{"schema", "pelican.graph"}, {"version", 1}, {"definitions", Json::object()},
                {"nodes", Json::array()}, {"outputs", Json::object()},
                {"rendering", {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
                               {"features", features}}}};
}

nlohmann::json lp1cTemplate(const std::string &name) {
    return nlohmann::json::parse(engineResourceOrThrow("graphs/" + name));
}

// What a pass declaration binds: its sampled images in their input order
// (both run the engine's shaders, which bind them by position; LP-1d), its
// attachments, shader, load, clear and blend.
nlohmann::json lp1cPassBinding(const nlohmann::json &pass, const std::map<std::string, std::string> &names,
                               const nlohmann::json &config) {
    using Json = nlohmann::json;
    std::set<std::string> buffers;
    for (const auto &buffer : config.value("buffers", Json::array())) buffers.insert(buffer.at("name").get<std::string>());
    Json images = Json::array();
    for (const auto &input : pass.value("input", Json::array()))
        if (!buffers.contains(input.get<std::string>())) images.push_back(TestSupport::lp1cRenamed(names, input.get<std::string>()));
    Json colors = Json::array();
    for (const auto &color : pass.at("output").at("color"))
        colors.push_back(TestSupport::lp1cRenamed(names, color.is_string() ? color.get<std::string>()
                                                              : color.at("target").get<std::string>()));
    // The one-piece upsample loads through its attachment, the graph's
    // through the pass: either is a load.
    auto load = pass.value("color_load_op", std::string{});
    for (const auto &color : pass.at("output").at("color"))
        if (color.is_object() && color.contains("load_op")) load = color.at("load_op").get<std::string>();
    auto shader = pass.at("shader");
    shader.erase("defines");
    return Json{{"images", images}, {"colors", colors}, {"shader", shader}, {"load", load},
                {"clear_color", pass.value("clear_color", Json{})},
                {"raster_state", pass.value("raster_state", Json{})}};
}

void lp1cRequireSamePlan(const RenderFeatureComposeResult &one_piece, const RenderFeatureComposeResult &decomposed,
                         const std::map<std::string, std::string> &names) {
    const auto one_piece_plan = TestSupport::lp1cComparablePlan(lp1aPlanJson(one_piece), names);
    const auto decomposed_plan = TestSupport::lp1cComparablePlan(lp1aPlanJson(decomposed), {});
    const auto &one_nodes = one_piece_plan.at("nodes");
    const auto &graph_nodes = decomposed_plan.at("nodes");
    CHECK(one_nodes.size() == graph_nodes.size());
    for (std::size_t i = 0; i < std::min(one_nodes.size(), graph_nodes.size()); ++i) {
        INFO(one_nodes.at(i).dump() << "\n" << graph_nodes.at(i).dump());
        CHECK(one_nodes.at(i) == graph_nodes.at(i));
    }
    CHECK(one_piece_plan.at("levels") == decomposed_plan.at("levels"));
    CHECK(one_piece_plan.at("barriers") == decomposed_plan.at("barriers"));
}
} // namespace

// LP-1c acceptance 3, bloom: engine://graphs/bloom_loop.json taken into a
// project in place of engine://features/bloom.json composes the same main
// frame plan -- the same passes in the same order and levels, reading and
// writing the same resources, with the same barriers -- once each name of the
// feature is spelled as the graph spells it (lp1cBloomNames). Its targets have
// the feature's formats and sizes, and its passes bind the same images in the
// same order with the same shaders, loads, clears and blends.
TEST_CASE("LP-1c the shipped bloom graph composes the bloom feature's frame plan, pass for pass",
          "[render-feature][graph-composition][lp1c]") {
    using Json = nlohmann::json;
    const auto names = TestSupport::lp1cBloomNames();
    const auto one_piece_document = lp1cOnePiece(Json::array({"engine://features/bloom.json"}));
    const auto decomposed_document =
        TestSupport::lp1cApplyDecomposition(one_piece_document, lp1cTemplate("bloom_loop.json"));
    CHECK(decomposed_document.at("rendering").at("features").empty());
    const auto one_piece = lp0aCompose(one_piece_document);
    const auto decomposed = lp0aCompose(decomposed_document);

    // Nothing of the feature is left.
    for (const auto &[from, to] : names) {
        INFO(from);
        CHECK(decomposed.config.dump().find("\"" + from + "\"") == std::string::npos);
    }
    REQUIRE(decomposed.graph_loops.size() == 2);
    CHECK(decomposed.graph_loops.at(0).node == "bloom_down");
    CHECK(decomposed.graph_loops.at(0).count == 4);
    CHECK(decomposed.graph_loops.at(1).node == "bloom_up");
    CHECK(decomposed.graph_loops.at(1).count == 3);

    lp1cRequireSamePlan(one_piece, decomposed, names);

    // The declarations behind the plan.
    const auto target = [](const Json &config, const std::string &name) {
        for (const auto &entry : config.at("render_targets"))
            if (entry.at("name") == name) return entry;
        FAIL("no render target " << name);
        return Json{};
    };
    for (const auto &[from, to] : names) {
        INFO(from << " -> " << to);
        if (from.ends_with("_RT")) {
            auto one = target(one_piece.config, from);
            auto graph = target(decomposed.config, to);
            for (auto *entry : {&one, &graph}) entry->erase("name");
            CHECK(one == graph);
        } else if (from != "bloom.intensity") {
            CHECK(lp1cPassBinding(passByName(one_piece.config, from), names, one_piece.config) ==
                  lp1cPassBinding(passByName(decomposed.config, to), {}, decomposed.config));
        }
    }
    // The feature's parameter defaults reach the shaders as the same
    // defines: every shader compiles with the same set.
    CHECK_FALSE(passByName(decomposed.config, "bloom_threshold").at("shader").contains("defines"));
    const std::set<std::string> one_piece_defines(one_piece.shader_defines.begin(), one_piece.shader_defines.end());
    const std::set<std::string> decomposed_defines(decomposed.shader_defines.begin(), decomposed.shader_defines.end());
    CHECK(one_piece_defines == decomposed_defines);
    CHECK(one_piece_defines.contains("PELICAN_FEATURE_BLOOM_THRESHOLD=0.400000006"));

    REQUIRE_NOTHROW(compileComposition(decomposed));
}

// LP-1c acceptance 3, planar: engine://graphs/planar_mip_chain_loop.json with
// the planar feature's six filter tasks replaced by its loop composes the same
// main frame plan, and each copy is the task it stands for: the same shader,
// region, views, dispatch and mips.
TEST_CASE("LP-1c the shipped planar mip chain replaces the six filter tasks with the same plan",
          "[render-feature][graph-composition][lp1c]") {
    using Json = nlohmann::json;
    const auto names = TestSupport::lp1cPlanarNames();
    const auto one_piece_document = lp1cOnePiece(Json::array({"engine://features/planar_reflection.json"}));
    const auto decomposed_document =
        TestSupport::lp1cApplyDecomposition(one_piece_document, lp1cTemplate("planar_mip_chain_loop.json"));
    const auto one_piece = lp0aCompose(one_piece_document);
    const auto decomposed = lp0aCompose(decomposed_document);
    REQUIRE(decomposed.graph_loops.size() == 1);
    CHECK(decomposed.graph_loops.at(0).count == 6);

    const auto task_names = [](const Json &config) {
        std::vector<std::string> result;
        for (const auto &task : config.at("compute_tasks")) result.push_back(task.at("name").get<std::string>());
        return result;
    };
    std::vector<std::string> expected;
    for (const auto &name : task_names(one_piece.config)) expected.push_back(TestSupport::lp1cRenamed(names, name));
    CHECK(task_names(decomposed.config) == expected);
    for (const auto &[from, to] : names) {
        INFO(from << " -> " << to);
        auto one = lp1bTask(one_piece.config, from);
        auto graph = lp1bTask(decomposed.config, to);
        for (auto *task : {&one, &graph}) {
            task->erase("name");
            // Ordering is compared through the plan: the copies chain by
            // explicit edges where the tasks relied on declaration order.
            task->erase("after");
            task->erase("before");
            // A graph port says `kind` where the feature lets it be inferred.
            for (auto &[port, binding] : task->at("resource_ports").items())
                if (binding.value("kind", std::string{}) == "image") binding.erase("kind");
        }
        CHECK(one == graph);
    }
    // The last copy keeps the last task's place before forward_transparent.
    CHECK(lp1bContains(lp1bTask(decomposed.config, "planar_mip_chain/5/filter").at("before"), "forward_transparent"));

    lp1cRequireSamePlan(one_piece, decomposed, names);
    REQUIRE_NOTHROW(compileComposition(decomposed));
}

namespace {
// LP-1d: engine://passthrough in a graph render.fullscreen, reading `beta`
// through port z_first as input 0 and `alpha` through a_second as input 1
// (both names sort the other way round), and `gamma` through an ordinary
// resource port. The rendering section declares the targets.
nlohmann::json lp1dNumberedInputs() {
    using Json = nlohmann::json;
    const auto type = lp0aSceneColorType();
    const Json target_base{{"extent_scale", 1.0}, {"format", "R16G16B16A16_SFLOAT"},
                           {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
                           {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}};
    auto targets = Json::array();
    for (const auto *name : {"alpha", "beta", "gamma", "trail", "out"}) {
        auto target = target_base;
        target["name"] = name;
        if (std::string_view{name} == "trail") target["history"] = true;
        targets.push_back(std::move(target));
    }
    const auto ref = [&](const std::string &id, const std::string &name) {
        return Json{{"id", id}, {"op", "render.target_ref"},
                    {"parameters", {{"name", name}, {"type", type}}}, {"outputs", {"resource"}}};
    };
    const auto read = [&](std::optional<int> input_index) {
        Json port{{"kind", "image"}, {"type", type}, {"access", "read"}};
        if (input_index) {
            port["binding"] = "input";
            port["input_index"] = *input_index;
        }
        return port;
    };
    return Json{{"schema", "pelican.graph"}, {"version", 1}, {"definitions", Json::object()},
        {"nodes", Json::array({ref("alpha_ref", "alpha"), ref("beta_ref", "beta"), ref("gamma_ref", "gamma"),
                               ref("trail_ref", "trail"), ref("out_ref", "out"),
            {{"id", "show"}, {"op", "render.fullscreen"},
             {"parameters", {{"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"},
                                         {"defines", {"PELICAN_PASSTHROUGH_INPUT=1"}}}},
                 {"ports", {{"z_first", read(0)}, {"a_second", read(1)}, {"gamma", read(std::nullopt)},
                            {"color", {{"kind", "image"}, {"type", type}, {"access", "write"},
                                       {"attachment", "color"}, {"location", 0}}}}}}},
             {"inputs", {{"z_first", {{"node", "beta_ref"}, {"port", "resource"}}},
                         {"a_second", {{"node", "alpha_ref"}, {"port", "resource"}}},
                         {"gamma", {{"node", "gamma_ref"}, {"port", "resource"}}},
                         {"color", {{"node", "out_ref"}, {"port", "resource"}}}}},
             {"outputs", {"color"}}}})},
        {"outputs", Json::object()},
        {"rendering", {{"render_targets", targets}, {"rendering_passes", Json::array()}}}};
}

// The graph refusal `document` raises when it is partitioned (as an edit is).
std::pair<std::string, nlohmann::json> lp1dPartitionRefusal(const nlohmann::json &document) {
    try {
        (void)partitionGraphDocument(document);
    } catch (const GraphRegistryError &error) {
        return {error.code(), error.detail()};
    }
    FAIL("the graph was not refused by name");
    return {};
}

nlohmann::json &lp1dShowPorts(nlohmann::json &document) { return document["nodes"][5]["parameters"]["ports"]; }
} // namespace

// LP-1d acceptance 2 (the CPU half; the GPU half is in VI-1's file): a port
// with `binding: "input"` is not a resource port. It is the pass's input
// number input_index, whatever its port or resource name; the resource
// ports' reads follow the numbered ones.
TEST_CASE("LP-1d a graph port with binding input is the pass's input number input_index",
          "[render-feature][graph-composition][lp1d]") {
    using Json = nlohmann::json;
    const auto document = lp1dNumberedInputs();

    SECTION("numbered inputs in their order, then the resource ports' reads") {
        const auto composed = lp0aCompose(document);
        const auto &pass = passByName(composed.config, "show");
        CHECK(pass.at("input") == Json::array({"beta", "alpha", "gamma"}));
        CHECK(pass.at("resource_ports").size() == 1);
        CHECK(pass.at("resource_ports").at("gamma").at("resource") == "gamma");
        CHECK(pass.at("shader").at("defines") == Json::array({"PELICAN_PASSTHROUGH_INPUT=1"}));
        REQUIRE_NOTHROW(compileComposition(composed));
    }
    SECTION("only numbered inputs: no resource port at all") {
        auto plain = document;
        lp1dShowPorts(plain).erase("gamma");
        plain["nodes"][5]["inputs"].erase("gamma");
        const auto composed = lp0aCompose(plain);
        const auto &pass = passByName(composed.config, "show");
        CHECK(pass.at("input") == Json::array({"beta", "alpha"}));
        CHECK(pass.at("resource_ports").empty());
        REQUIRE_NOTHROW(compileComposition(composed));
    }
    SECTION("a history read is a numbered input too (LP-0a)") {
        auto history = document;
        lp1dShowPorts(history)["z_first"]["history"] = true;
        history["nodes"][5]["inputs"]["z_first"]["node"] = "trail_ref";
        const auto composed = lp0aCompose(history);
        const auto &pass = passByName(composed.config, "show");
        CHECK(pass.at("input") == Json::array({"trail@history", "alpha", "gamma"}));
        CHECK_FALSE(pass.at("resource_ports").contains("z_first"));
        REQUIRE_NOTHROW(compileComposition(composed));
        // A target without history: the same named refusal as a resource port's.
        auto without = history;
        without["nodes"][3]["parameters"]["name"] = "beta";
        const auto [code, fields] = lp0aRefusal(without);
        CHECK(code == "graph.history_unavailable@1");
        CHECK(fields == Json{{"node", "show"}, {"port", "z_first"}, {"target", "beta"}});
    }
    SECTION("the same image as two inputs is the rendering section's duplicate input") {
        auto twice = document;
        twice["nodes"][5]["inputs"]["a_second"]["node"] = "beta_ref";
        const auto composed = lp0aCompose(twice);
        const auto &pass = passByName(composed.config, "show");
        CHECK(pass.at("input") == Json::array({"beta", "beta", "gamma"}));
        // The pass-shape rule the engine's pass parser applies when it loads.
        const auto violations = evaluatePassShape(
            defaultPassShapePolicy(), passShapeObservationFromJson(RenderPassType::fullscreen, pass));
        CHECK(std::any_of(violations.begin(), violations.end(), [](const PassShapeViolation &violation) {
            return violation.kind == PassShapeViolationKind::duplicate_input && violation.target == "beta";
        }));
    }
}

// LP-1d acceptance 3: the numbers run 0, 1, ... n - 1, each once.
TEST_CASE("LP-1d input_index gaps and duplicates are refused by name",
          "[render-feature][graph-composition][lp1d]") {
    using Json = nlohmann::json;
    const auto document = lp1dNumberedInputs();
    REQUIRE_NOTHROW(partitionGraphDocument(document));

    auto gap = document;
    lp1dShowPorts(gap)["a_second"]["input_index"] = 2;
    auto [code, fields] = lp1dPartitionRefusal(gap);
    CHECK(code == "graph.input_index_invalid@1");
    CHECK(fields == Json{{"node", "show"}, {"input_index", 1}, {"reason", "missing"}, {"ports", Json::array()}});

    auto no_zero = document;
    lp1dShowPorts(no_zero)["z_first"]["input_index"] = 2;
    std::tie(code, fields) = lp1dPartitionRefusal(no_zero);
    CHECK(code == "graph.input_index_invalid@1");
    CHECK(fields == Json{{"node", "show"}, {"input_index", 0}, {"reason", "missing"}, {"ports", Json::array()}});

    auto duplicate = document;
    lp1dShowPorts(duplicate)["a_second"]["input_index"] = 0;
    std::tie(code, fields) = lp1dPartitionRefusal(duplicate);
    CHECK(code == "graph.input_index_invalid@1");
    CHECK(fields == Json{{"node", "show"}, {"input_index", 0}, {"reason", "duplicate"},
                         {"ports", Json::array({"a_second", "z_first"})}});

    // The same in a loop body's definition, as the shipped bloom's passes are.
    auto bloom = lp1cTemplate("bloom_loop.json");
    const auto one_piece = lp1cOnePiece(Json::array({"engine://features/bloom.json"}));
    REQUIRE_NOTHROW(partitionGraphDocument(TestSupport::lp1cApplyDecomposition(one_piece, bloom)));
    auto &h_blur = bloom["definitions"]["bloom_down_step"]["nodes"][2];
    REQUIRE(h_blur.at("id") == "h_blur");
    h_blur["parameters"]["ports"]["source"]["input_index"] = 1;
    std::tie(code, fields) = lp1dPartitionRefusal(TestSupport::lp1cApplyDecomposition(one_piece, bloom));
    CHECK(code == "graph.input_index_invalid@1");
    CHECK(fields == Json{{"node", "bloom_down/$0/h_blur"}, {"input_index", 0}, {"reason", "missing"}, {"ports", Json::array()}});

    // What an input port may be is checked as a malformed declaration.
    for (const auto &[port, change] : std::vector<std::pair<std::string, Json>>{
             {"a_second", {{"input_index", -1}}},
             {"a_second", {{"input_index", "1"}}},
             {"a_second", {{"input_index", nullptr}}},
             {"gamma", {{"input_index", 2}}},
             {"a_second", {{"sampling", {{"filter", "nearest"}}}}},
             {"a_second", {{"view", "per_view"}}},
             {"a_second", {{"subresource", {{"mip", 0}}}}},
             {"color", {{"binding", "input"}, {"input_index", 2}}}}) {
        INFO(port << " " << change.dump());
        auto invalid = document;
        for (const auto &[key, value] : change.items()) {
            if (value.is_null()) lp1dShowPorts(invalid)[port].erase(key);
            else lp1dShowPorts(invalid)[port][key] = value;
        }
        CHECK_THROWS(partitionGraphDocument(invalid));
    }
    // A compute task binds resource ports only.
    auto compute = document;
    compute["nodes"][5]["op"] = "render.compute";
    lp1dShowPorts(compute).erase("color");
    compute["nodes"][5]["inputs"].erase("color");
    compute["nodes"][5]["parameters"]["shader"] = "engine://passthrough";
    CHECK_THROWS(partitionGraphDocument(compute));
}


namespace {
// LP-3b: the hybrid preset forked into the rendering section -- a direct root
// that declares the preset's passes itself.
nlohmann::json lp3bDirectRoot(const nlohmann::json &features) {
    return TestSupport::renderGraphRoot(TestSupport::lp3aForkPreset(
        nlohmann::json{{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
                       {"features", features}},
        lp3aHybridPreset()));
}

// The production rewrite (decompose_material_pass's), one pass after another.
nlohmann::json lp3bDecompose(const nlohmann::json &root, std::initializer_list<const char *> passes) {
    auto ordered = nlohmann::ordered_json::parse(root.dump());
    for (const auto *pass : passes) (void)decomposeMaterialPass(ordered.at("rendering"), ordered, pass);
    return nlohmann::json::parse(ordered.dump());
}

// The refusal decomposing `pass` of `root` raises, with the documents it
// was given left as they were.
std::pair<std::string, nlohmann::json> lp3bRefusal(const nlohmann::json &root, const std::string &pass) {
    auto ordered = nlohmann::ordered_json::parse(root.dump());
    const auto before = ordered;
    try {
        (void)decomposeMaterialPass(ordered.at("rendering"), ordered, pass);
    } catch (const MaterialPassDecomposeError &error) {
        CHECK(ordered == before);
        return {error.code(), nlohmann::json::parse(error.fields().dump())};
    } catch (const GraphRegistryError &error) {
        CHECK(ordered == before);
        return {error.code(), error.detail()};
    }
    FAIL("decompose was not refused by name");
    return {};
}

nlohmann::json lp3bWithoutDeclared(nlohmann::json document) {
    for (auto &node : document.at("nodes"))
        if (node.at("op") == "render.material_pass") node["parameters"]["insert"].erase("declared");
    return document;
}
} // namespace

TEST_CASE("LP-3b decompose writes a declared material pass as the graph nodes it means",
          "[render-feature][graph-composition][lp3b]") {
    using Json = nlohmann::json;
    const auto root = lp3bDirectRoot(Json::array());
    const auto decomposed = lp3bDecompose(root, {"forward_opaque"});

    // The section loses the pass and nothing else.
    auto expected_rendering = root.at("rendering");
    auto &passes = expected_rendering.at("rendering_passes").at(0).at("passes");
    passes.erase(std::find_if(passes.begin(), passes.end(),
                              [](const Json &pass) { return pass.at("name") == "forward_opaque"; }));
    CHECK(decomposed.at("rendering") == expected_rendering);
    // The graph gains exactly the pass, its references, its loop and the body.
    CHECK(decomposed.at("nodes") == Json::array({
        {{"id", "lit_color_ref"}, {"op", "render.target_ref"},
         {"parameters", {{"name", "lit_color"},
                         {"type", {{"semantic", "pelican.render.legacy_opaque_image@1"}, {"arguments", Json::object()}}}}},
         {"outputs", {"resource"}}},
        {{"id", "scene_depth_ref"}, {"op", "render.target_ref"},
         {"parameters", {{"name", "scene_depth"},
                         {"type", {{"semantic", "pelican.render.depth@1"},
                                   {"arguments", {{"representation", "device"}, {"space", "projection"}}}}}}},
         {"outputs", {"resource"}}},
        {{"id", "forward_opaque_draws"}, {"op", "graph.loop"},
         {"parameters", {{"over", {{"draw_items", {{"contract", "forward_opaque_v1"}}}}},
                         {"order", {{"by", "phase_default"}}},
                         {"branches", Json::array({{{"body", "forward_opaque_draw"}}})}}},
         {"outputs", {"draws"}}},
        {{"id", "forward_opaque"}, {"op", "render.material_pass"},
         {"parameters", {{"material_contract", "forward_opaque_v1"}, {"color_load_op", "load"},
                         {"depth_load_op", "load"}, {"depth_store_op", "store"}, {"pass_set", "main_render"},
                         {"insert", {{"before", "__snapshot_opaque_color"}, {"declared", true}}}}},
         {"inputs", {{"color", {{"node", "lit_color_ref"}, {"port", "resource"}}},
                     {"depth", {{"node", "scene_depth_ref"}, {"port", "resource"}}},
                     {"draws", {{"node", "forward_opaque_draws"}, {"port", "draws"}}}}},
         {"outputs", Json::array()}},
    }));
    CHECK(decomposed.at("definitions") == Json{
        {"forward_opaque_draw",
         {{"nodes", Json::array({{{"id", "step"}, {"op", "draw.material"}, {"parameters", Json::object()},
                                  {"outputs", {"draws"}}}})},
          {"outputs", {{"draws", {{"node", "step"}, {"port", "draws"}}}}}}}});
    REQUIRE_NOTHROW(partitionGraphDocument(decomposed));

    // The next pass reuses the references the first one added.
    auto ordered = nlohmann::ordered_json::parse(decomposed.dump());
    const auto transparent = decomposeMaterialPass(ordered.at("rendering"), ordered, "forward_transparent");
    CHECK(transparent.target_refs == std::vector<std::string>{"lit_color_ref", "scene_depth_ref"});
    CHECK(transparent.added_target_refs.empty());
    CHECK(transparent.insert == nlohmann::ordered_json{{"before", "scene_present"}, {"declared", true}});
    // The G-buffer pass: five colors, first in its family.
    const auto geometry = decomposeMaterialPass(ordered.at("rendering"), ordered, "deferred_geometry");
    CHECK(geometry.target_refs == std::vector<std::string>{"gbuffer_albedo_ref", "gbuffer_normal_ref",
                                                           "gbuffer_material_ref", "gbuffer_worldpos_ref",
                                                           "g_emissive_ref", "scene_depth_ref"});
    CHECK(geometry.insert == nlohmann::ordered_json{{"before", "ssao_pass"}, {"declared", true}});
    const auto composed = lp0aCompose(Json::parse(ordered.dump()));
    CHECK(passByName(composed.config, "deferred_geometry").at("clear_colors") ==
          Json{{"gbuffer_worldpos", {0.0, 0.0, 0.0, 0.0}}});
}

TEST_CASE("LP-3b a decomposed pass composes where the declared one stood, features and all",
          "[render-feature][graph-composition][lp3b]") {
    using Json = nlohmann::json;
    // shadow_directional + sky_ambient: surface resources reach the forward
    // passes, a feature pass lands after deferred_lighting. clustered_lighting
    // and shadow_point: pass_overrides name forward_opaque and
    // forward_transparent. rt_shadow_mask: its insert names deferred_geometry.
    for (const auto &features : {Json::array(),
                                 Json::array({"engine://features/shadow_directional.json",
                                              "engine://features/sky_ambient.json"}),
                                 Json::array({"engine://features/clustered_lighting.json",
                                              "engine://features/shadow_point.json"}),
                                 Json::array({"engine://features/rt_shadow_mask.json"})}) {
        INFO(features.dump());
        const auto one_piece = lp0aCompose(lp3bDirectRoot(features));
        const auto document =
            lp3bDecompose(lp3bDirectRoot(features), {"deferred_geometry", "forward_opaque", "forward_transparent"});
        const auto decomposed = lp0aCompose(document);
        CHECK(lp0bDeclared(decomposed.config) == lp0bDeclared(one_piece.config));
        CHECK(lp0bPlanOrder(decomposed) == lp0bPlanOrder(one_piece));
        for (const auto *pass : {"deferred_geometry", "forward_opaque", "forward_transparent"}) {
            INFO(pass);
            CHECK(lp3aPassFields(decomposed.config, pass) == lp3aPassFields(one_piece.config, pass));
        }
        CHECK(decomposed.surface_resource_contracts == one_piece.surface_resource_contracts);
        CHECK(decomposed.pass_provenance == one_piece.pass_provenance);
        REQUIRE(decomposed.material_draw_loops != nullptr);
        CHECK(decomposed.material_draw_loops->passes.size() == 3);
        REQUIRE_NOTHROW(compileComposition(decomposed));
        // Control: placed after the features (no `declared`), the passes are
        // not there when a feature names them (a pass_override, an insert).
        if (features.empty() || features.at(0) == "engine://features/shadow_directional.json") continue;
        CHECK_THROWS(lp0aCompose(lp3bWithoutDeclared(document)));
    }
}

TEST_CASE("LP-3b decompose refuses what the graph cannot say, by name",
          "[render-feature][graph-composition][lp3b]") {
    using Json = nlohmann::json;
    const auto root = lp3bDirectRoot(Json::array());
    const auto forward = [&](const std::function<void(Json &)> &change) {
        auto changed = root;
        for (auto &pass : changed.at("rendering").at("rendering_passes").at(0).at("passes"))
            if (pass.at("name") == "forward_opaque") change(pass);
        return changed;
    };
    using Refusal = std::pair<std::string, Json>;
    const std::string code{materialPassNotDecomposableCode};
    CHECK(lp3bRefusal(root, "no_such_pass") == Refusal{code, {{"pass", "no_such_pass"}, {"reason", "not_found"}}});
    CHECK(lp3bRefusal(root, "ssao_pass") ==
          Refusal{code, {{"pass", "ssao_pass"}, {"reason", "not_material"}, {"type", "fullscreen"}}});
    CHECK(lp3bRefusal(forward([](Json &pass) { pass["input"] = Json::array({"ssao_blur"}); }), "forward_opaque") ==
          Refusal{code, {{"pass", "forward_opaque"}, {"reason", "field_unsupported"}, {"field", "input"}}});
    CHECK(lp3bRefusal(forward([](Json &pass) { pass.erase("material_contract"); }), "forward_opaque") ==
          Refusal{code, {{"pass", "forward_opaque"}, {"reason", "contract_unsupported"}, {"contract", nullptr}}});
    CHECK(lp3bRefusal(forward([](Json &pass) {
                          pass["output"]["color"] = Json::array({{{"target", "lit_color"}, {"subresource", {{"mip", 0}}}}});
                      }),
                      "forward_opaque") ==
          Refusal{code, {{"pass", "forward_opaque"}, {"reason", "attachment_unsupported"}, {"socket", "color"}}});
    auto taken = root;
    taken["nodes"].push_back({{"id", "forward_opaque_draws"}, {"op", "value.constant"}, {"parameters", {{"value", 1.0}}},
                              {"outputs", {"value"}}});
    CHECK(lp3bRefusal(taken, "forward_opaque") ==
          Refusal{"graph.node_duplicate@1", {{"node", "forward_opaque_draws"}, {"scope", "root"}}});
    auto defined = root;
    defined["definitions"]["forward_opaque_draw"] = {{"nodes", Json::array()}, {"outputs", Json::object()}};
    CHECK(lp3bRefusal(defined, "forward_opaque") ==
          Refusal{"graph.definition_duplicate@1", {{"definition", "forward_opaque_draw"}}});
}

TEST_CASE("LP-3b insert.declared places a graph pass among the declared passes only",
          "[render-feature][graph-composition][lp0b][lp3b]") {
    using Json = nlohmann::json;
    // A feature's pass is not there yet when the declared passes are placed.
    const auto feature_anchor =
        lp0bGraph({"blit"}, {{"insert", {{"after", "HighLuminanceExtraction"}, {"declared", true}}}});
    const auto [code, fields] = lp0aRefusal(feature_anchor);
    CHECK(code == "graph.insert_anchor_unknown@1");
    CHECK(fields == Json{{"node", "blit"}, {"anchor", "HighLuminanceExtraction"}});
    // A declared pass: the same place as without the flag.
    const auto declared = lp0aCompose(lp0bGraph({"blit"}, {{"insert", {{"after", "deferred_lighting"}, {"declared", true}}}}));
    const auto names = lp0bDeclared(declared.config);
    CHECK(lp0bIndex(names, "blit") == lp0bIndex(names, "deferred_lighting") + 1);
    REQUIRE_NOTHROW(compileComposition(declared));
    // The flag is a boolean beside one side.
    CHECK_THROWS(partitionGraphDocument(lp0bGraph({"blit"}, {{"insert", {{"after", "deferred_lighting"}, {"declared", "yes"}}}})));
    CHECK_THROWS(partitionGraphDocument(lp0bGraph({"blit"}, {{"insert", {{"declared", true}}}})));
}

TEST_CASE("LP-3b get_frame_plan pages a draw_loop's elements",
          "[render-feature][graph-composition][lp3b]") {
    using Json = nlohmann::json;
    const auto element = [](int index) {
        return Json{{"element", "m" + std::to_string(index)}, {"index", index}, {"branch", 0}};
    };
    const Json plan{{"loops", Json::array({
        {{"node", "forward_opaque_draws"}, {"tier", "draw_loop"}, {"count", 3},
         {"elements", Json::array({element(0), element(1), element(2)})}, {"unmatched_branches", Json::array()},
         {"pass", "forward_opaque"}},
        {{"node", "steps"}, {"tier", "expand"}, {"count", 1}, {"elements", Json::array({element(0)})},
         {"unmatched_branches", Json::array()}},
    })}};
    const auto paged = [&](const Json &params) {
        auto copy = plan;
        REQUIRE(pageDrawLoopElements(copy, parseDrawLoopElementPage(params)));
        CHECK(copy.at("loops").at(1) == plan.at("loops").at(1));
        return copy.at("loops").at(0);
    };
    const auto first = paged({{"loop_elements", {{"node", "forward_opaque_draws"}, {"limit", 2}}}});
    CHECK(first.at("elements") == Json::array({element(0), element(1)}));
    CHECK(first.at("total") == 3);
    CHECK(first.at("count") == 3);
    CHECK(first.at("offset") == 0);
    CHECK(first.at("limit") == 2);
    const auto rest = paged({{"loop_elements", {{"node", "forward_opaque_draws"}, {"offset", 2}, {"limit", 2}}}});
    CHECK(rest.at("elements") == Json::array({element(2)}));
    CHECK(rest.at("total") == 3);
    const auto whole = paged(Json::object());
    CHECK(whole.at("elements").size() == 3);
    CHECK(whole.at("limit") == drawLoopElementPageDefaultLimit);
    // An unknown loop, or an expand loop, is not paged; bad shapes are refused.
    auto unchanged = plan;
    CHECK_FALSE(pageDrawLoopElements(unchanged, parseDrawLoopElementPage({{"loop_elements", {{"node", "steps"}}}})));
    CHECK(unchanged == plan);
    CHECK_THROWS_AS(parseDrawLoopElementPage({{"loop_elements", {{"node", "x"}, {"limit", 0}}}}), std::invalid_argument);
    CHECK_THROWS_AS(parseDrawLoopElementPage({{"loop_elements", {{"node", "x"}, {"offset", -1}}}}), std::invalid_argument);
    CHECK_THROWS_AS(parseDrawLoopElementPage({{"loop_elements", {{"limit", 2}}}}), std::invalid_argument);
    CHECK_THROWS_AS(parseDrawLoopElementPage({{"loop_elements", {{"node", "x"}, {"page", 1}}}}), std::invalid_argument);
}

namespace {
// LP-Vb: a template the engine ships under engine://graphs/.
nlohmann::json lpvbTemplate(std::string_view name) {
    return nlohmann::json::parse(engineResourceOrThrow("graphs/" + std::string{name}));
}

// The hybrid preset with `features`, as a graph root with no node.
nlohmann::json lpvbGraph(nlohmann::json features) {
    using Json = nlohmann::json;
    return Json{{"schema", "pelican.graph"}, {"version", 1}, {"definitions", Json::object()},
                {"nodes", Json::array()}, {"outputs", Json::object()},
                {"rendering", {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
                               {"features", std::move(features)}}}};
}

// Composed as the engine composes: a view axis sized by the providers.
RenderFeatureComposeResult lpvbCompose(const nlohmann::json &document) {
    return composeRenderFeatureConfig(document, RenderFeatureComposeDependencies{
                                                    .load_feature_json = loadEngineFeature,
                                                    .runtime_shader_compiler_enabled = true,
                                                    .view_axis_length = renderGraphViewAxisLengthResolver(),
                                                });
}

std::pair<std::string, nlohmann::json> lpvbRefusal(const nlohmann::json &document) {
    try {
        (void)lpvbCompose(document);
    } catch (const GraphRegistryError &error) {
        return {error.code(), error.detail()};
    }
    FAIL("composition was not refused by name");
    return {};
}

// The cube capture's body nodes, in the order the feature declares its passes.
const std::array<std::string_view, 8> lpvbCubeNodes{"geometry",       "ssao",
                                                    "ssao_blur",      "lighting",
                                                    "forward_opaque", "snapshot_opaque_color",
                                                    "snapshot_opaque_depth", "forward_transparent"};

// The fields a copy shares with the feature pass it stands for.
const std::array<std::string_view, 20> lpvbPassFields{
    "type",          "output",          "input_sampling",  "material_contract",
    "resolution_domain", "view_family", "shader",          "push_constants",   "clear_color",
    "color_load_op", "depth_load_op",   "depth_store_op",  "uses_light_data",  "screen_inputs",
    "surface_resources", "material_resources", "source",   "destination",      "snapshot",
    "regions"};

void lpvbRequireSameAsFeaturePass(const nlohmann::json &copy, const nlohmann::json &pass) {
    using Json = nlohmann::json;
    for (const auto field : lpvbPassFields) {
        INFO(field);
        CHECK(copy.value(std::string{field}, Json()) == pass.value(std::string{field}, Json()));
    }
    // A pass whose shader names its inputs (resource ports) reads them in
    // any order; one that takes them by position reads them in the same.
    const auto ports = pass.value("resource_ports", Json::object());
    if (ports.empty()) {
        CHECK(copy.value("input", Json()) == pass.value("input", Json()));
    } else {
        auto copied = copy.at("input").get<std::vector<std::string>>();
        auto expected = pass.at("input").get<std::vector<std::string>>();
        std::sort(copied.begin(), copied.end());
        std::sort(expected.begin(), expected.end());
        CHECK(copied == expected);
    }
    // A named resource port binds the same resource the same way (the graph
    // spells out the kind the feature leaves implicit).
    REQUIRE(copy.value("resource_ports", Json::object()).size() == ports.size());
    for (const auto &[port, binding] : ports.items()) {
        INFO(port);
        const auto &copied = copy.at("resource_ports").at(port);
        CHECK(copied.at("resource") == binding.at("resource"));
        CHECK(copied.value("view", Json()) == binding.value("view", Json()));
        CHECK(copied.value("access", Json()) == binding.value("access", Json()));
    }
}

const nlohmann::json &lpvbContract(const nlohmann::json &contracts, std::string_view name) {
    for (const auto &contract : contracts)
        if (contract.at("contract").get<std::string>() == name) return contract;
    throw std::runtime_error("no surface resource contract " + std::string{name});
}
} // namespace

TEST_CASE("LP-Vb a loop over the cube's face axis takes the place of cube_capture's passes, face by face",
          "[render-feature][graph-composition][lpvb][view-select][cube]") {
#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
    using Json = nlohmann::json;
    const auto features = Json::array({"engine://features/cube_capture.json"});
    const auto kept = lpvbCompose(lpvbGraph(features));
    const auto document =
        TestSupport::lp1cApplyDecomposition(lpvbGraph(features), lpvbTemplate("cube_capture_loop.json"));
    const auto composed = lpvbCompose(document);
    const auto &config = composed.config;

    // The copies stand where the feature's first pass stood, face by face,
    // each face's in the feature's order; none of the feature's is left.
    const auto declared = lp0bDeclared(config);
    const auto slot = lp0bIndex(lp0bDeclared(kept.config), "cube_capture_geometry");
    for (std::size_t face = 0; face < 6; ++face) {
        for (std::size_t index = 0; index < lpvbCubeNodes.size(); ++index) {
            const auto node = std::string{lpvbCubeNodes[index]};
            const auto name = "cube_capture_faces/" + std::to_string(face) + "/" + node;
            INFO(name);
            CHECK(lp0bIndex(declared, name) == slot + static_cast<std::ptrdiff_t>(face * 8 + index));
            CHECK(std::find(declared.begin(), declared.end(), "cube_capture_" + node) == declared.end());
            // The feature's pass, on one face.
            const auto &copy = passByName(config, name);
            CHECK(copy.at("view_select") == Json{{"face", face}});
            lpvbRequireSameAsFeaturePass(copy, passByName(kept.config, "cube_capture_" + node));
            if (copy.contains("snapshot_after"))
                CHECK(copy.at("snapshot_after") == "cube_capture_faces/" + std::to_string(face) + "/forward_opaque");
        }
    }
    // The deferred lighting shader takes its inputs by position: the copy
    // reads them in the feature pass's order, with no named port.
    CHECK(passByName(config, "cube_capture_faces/0/lighting").at("input") ==
          Json::array({"cube_capture_albedo", "cube_capture_normal", "cube_capture_material",
                       "cube_capture_worldpos", "cube_capture_emissive", "cube_capture_ao_blur"}));
    CHECK(passByName(config, "cube_capture_faces/0/lighting").value("resource_ports", Json::object()).empty());
    // Face f + 1 waits on face f: the order is face-major.
    CHECK(lp0bListed(passByName(config, "cube_capture_faces/1/geometry").at("after"),
                     "cube_capture_faces/0/forward_transparent"));

    // get_frame_plan's loops[]: the six faces, and every copy's origin.
    REQUIRE(composed.graph_loops.size() == 1);
    CHECK(composed.graph_loops.front().node == "cube_capture_faces");
    CHECK(composed.graph_loops.front().tier == "expand");
    CHECK(composed.graph_loops.front().count == 6);
    const auto plan = lp1aPlanJson(composed);
    for (std::size_t face = 0; face < 6; ++face) {
        const auto *node = lp1aPlanNode(plan, "cube_capture_faces/" + std::to_string(face) + "/ssao");
        REQUIRE(node != nullptr);
        CHECK(node->at("view_select") == Json{{"face", face}});
        CHECK(node->at("loop_origin") == Json{{"loop", "cube_capture_faces"}, {"element", face}, {"node", "ssao"}});
    }
#else
    SKIP("the $capture/cube provider is a standard render algorithm");
#endif
}

TEST_CASE("LP-Vb the cube's decomposed passes receive the directional shadow as the feature's passes did",
          "[render-feature][graph-composition][lpvb][cube][shadow]") {
#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
    using Json = nlohmann::json;
    // docs/design_graph_loops.md 14.7: the copies join after the surface
    // resources picked their consumers, in the place of consumers. The
    // shadow comes second so its shadow_depth (inserted at the beginning)
    // runs before the capture reads the map.
    const auto features = Json::array({"engine://features/cube_capture.json", lp0dShadowFeature(nullptr)});
    const auto kept = lpvbCompose(lpvbGraph(features));
    const auto composed = lpvbCompose(
        TestSupport::lp1cApplyDecomposition(lpvbGraph(features), lpvbTemplate("cube_capture_loop.json")));
    const auto &kept_contract = lpvbContract(kept.surface_resource_contracts, "directional_shadow");
    REQUIRE(lp0bListed(kept_contract.at("fullscreen_consumers"), "cube_capture_lighting"));
    REQUIRE(lp0bListed(kept_contract.at("material_consumers"), "cube_capture_forward_opaque"));
    REQUIRE(lp0bListed(kept_contract.at("material_consumers"), "cube_capture_forward_transparent"));

    const auto &contract = lpvbContract(composed.surface_resource_contracts, "directional_shadow");
    for (const auto *replaced : {"cube_capture_lighting", "cube_capture_forward_opaque",
                                 "cube_capture_forward_transparent"}) {
        CHECK_FALSE(lp0bListed(contract.at("fullscreen_consumers"), replaced));
        CHECK_FALSE(lp0bListed(contract.at("material_consumers"), replaced));
    }
    for (std::size_t face = 0; face < 6; ++face) {
        const auto prefix = "cube_capture_faces/" + std::to_string(face) + "/";
        INFO(prefix);
        CHECK(lp0bListed(contract.at("fullscreen_consumers"), prefix + "lighting"));
        CHECK(lp0bListed(contract.at("material_consumers"), prefix + "forward_opaque"));
        CHECK(lp0bListed(contract.at("material_consumers"), prefix + "forward_transparent"));
        // Bound as the feature's were: the shadow map is the lighting's
        // seventh input (PELICAN_DECLARE_INPUT_6), and the forward passes'
        // directional_shadow.
        for (const auto *node : {"lighting", "forward_opaque", "forward_transparent"})
            lpvbRequireSameAsFeaturePass(passByName(composed.config, prefix + node),
                                         passByName(kept.config, std::string{"cube_capture_"} + node));
        CHECK(passByName(composed.config, prefix + "lighting").at("input").at(6) == "shadow_map");
        CHECK(passByName(composed.config, prefix + "forward_opaque").at("surface_resources").at("directional_shadow") ==
              "shadow_map");
    }
    const auto declared = lp0bDeclared(composed.config);
    CHECK(lp0bIndex(declared, "shadow_depth") < lp0bIndex(declared, "cube_capture_faces/0/geometry"));
    // The pipeline compiles: every consumer the contract lists binds it.
    const auto compiled = compileComposition(composed);
    const auto found = std::find_if(compiled.surface_resource_contracts.begin(),
                                    compiled.surface_resource_contracts.end(),
                                    [](const auto &entry) { return entry.contract.name == "directional_shadow"; });
    REQUIRE(found != compiled.surface_resource_contracts.end());
    CHECK(found->producer == "shadow_depth");
#else
    SKIP("the $capture/cube provider is a standard render algorithm");
#endif
}

TEST_CASE("LP-Vb a loop over the cascade axis draws one cascade per copy in shadow_depth's place",
          "[render-feature][graph-composition][lpvb][view-select][shadow]") {
    using Json = nlohmann::json;
    for (const std::int64_t cascades : {3, 2}) {
        INFO(cascades << " cascades");
        auto feature = lp0dShadowFeature(nullptr);
        feature["parameters"]["cascade_count"] = cascades;
        const auto kept = lpvbCompose(lpvbGraph(Json::array({feature})));
        const auto composed = lpvbCompose(TestSupport::lp1cApplyDecomposition(
            lpvbGraph(Json::array({feature})), lpvbTemplate("shadow_cascade_loop.json")));
        const auto &config = composed.config;
        const auto declared = lp0bDeclared(config);
        const auto slot = lp0bIndex(lp0bDeclared(kept.config), "shadow_depth");
        CHECK(std::find(declared.begin(), declared.end(), "shadow_depth") == declared.end());
        // As many copies as the feature has cascades: the axis's length is the
        // provider's, read off the composed feature instance.
        CHECK(lp1aNamesUnder(config, "shadow_cascades/").size() == static_cast<std::size_t>(cascades));
        for (std::int64_t cascade = 0; cascade < cascades; ++cascade) {
            const auto name = "shadow_cascades/" + std::to_string(cascade) + "/depth";
            INFO(name);
            CHECK(lp0bIndex(declared, name) == slot + cascade);
            const auto &copy = passByName(config, name);
            CHECK(copy.at("view_select") == Json{{"cascade", cascade}});
            lpvbRequireSameAsFeaturePass(copy, passByName(kept.config, "shadow_depth"));
        }
        CHECK(lpvbContract(composed.surface_resource_contracts, "directional_shadow").at("producer") ==
              "shadow_cascades/" + std::to_string(cascades - 1) + "/depth");
        REQUIRE(composed.graph_loops.size() == 1);
        CHECK(composed.graph_loops.front().count == static_cast<std::size_t>(cascades));
        (void)compileComposition(composed);
    }
}

TEST_CASE("LP-Vb a view axis known only per frame, or not declared, is refused by name",
          "[render-feature][graph-composition][lpvb][view-select]") {
    using Json = nlohmann::json;
    const auto shadowLoop = [](const Json &over) {
        auto graph = lpvbTemplate("shadow_cascade_loop.json");
        graph["nodes"][0]["parameters"]["over"] = over;
        return TestSupport::lp1cApplyDecomposition(lpvbGraph(Json::array({lp0dShadowFeature(nullptr)})), graph);
    };
    SECTION("the light axis is as long as the frame's lights") {
        const auto [code, fields] =
            lpvbRefusal(shadowLoop({{"view_axis", {{"family", "$shadow/directional"}, {"axis", "light"}}}}));
        CHECK(code == "graph.loop_tier_unsupported@1");
        CHECK(fields == Json{{"node", "shadow_cascades"}, {"reason", "set_unsupported"}});
    }
    SECTION("an axis the family does not have") {
        const auto [code, fields] =
            lpvbRefusal(shadowLoop({{"view_axis", {{"family", "$shadow/directional"}, {"axis", "face"}}}}));
        CHECK(code == "graph.view_select_axis_unknown@1");
        CHECK(fields == Json{{"pass", "shadow_cascades"}, {"family", "$shadow/directional"}, {"axis", "face"}});
    }
    SECTION("a family no provider builds") {
        const auto [code, fields] =
            lpvbRefusal(shadowLoop({{"view_axis", {{"family", "$main"}, {"axis", "cascade"}}}}));
        CHECK(code == "graph.view_select_axis_unknown@1");
        CHECK(fields == Json{{"pass", "shadow_cascades"}, {"family", "$main"}, {"axis", "cascade"}});
    }
    SECTION("a view_axis that names no family or no axis") {
        for (const auto &over : {Json{{"view_axis", {{"family", "$shadow/directional"}}}},
                                 Json{{"view_axis", {{"axis", "cascade"}}}},
                                 Json{{"view_axis", {{"family", "$shadow/directional"}, {"axis", ""}}}},
                                 Json{{"view_axis", {{"family", "$shadow/directional"}, {"axis", "cascade"},
                                                     {"count", 2}}}}}) {
            INFO(over.dump());
            CHECK_THROWS_AS(partitionGraphDocument(shadowLoop(over)), std::invalid_argument);
        }
    }
    SECTION("a composition without the providers cannot size it") {
        CHECK_THROWS_AS(lp0aCompose(shadowLoop({{"view_axis", {{"family", "$shadow/directional"},
                                                               {"axis", "cascade"}}}})),
                        std::runtime_error);
    }
    SECTION("the tier is expand") {
        auto document = shadowLoop({{"view_axis", {{"family", "$shadow/directional"}, {"axis", "cascade"}}}});
        document["nodes"][0]["parameters"]["tier"] = "draw_loop";
        const auto [code, fields] = lp0bEditRefusal(document);
        CHECK(code == "graph.loop_tier_mismatch@1");
        CHECK(fields == Json{{"node", "shadow_cascades"}, {"tier", "draw_loop"}, {"actual", "expand"}});
    }
}

TEST_CASE("LP-Vb region_tags reach the pass as its regions, and input_index orders a pass's inputs",
          "[render-feature][graph-composition][lpvb]") {
    using Json = nlohmann::json;
    SECTION("render.material_pass, render.shadow_depth and render.snapshot_copy") {
        auto nodes = lp0eCubeCopyNodes();
        nodes[2]["parameters"]["region_tags"] = Json::array({"capture", "copy"});
        const auto copy = lp0aCompose(lp0eCubeGraph(nodes, "opaque_color_copy"));
        CHECK(passByName(copy.config, "opaque_color_copy").at("regions") == Json::array({"capture", "copy"}));
        CHECK_FALSE(passByName(copy.config, "opaque_color_copy").contains("region_tags"));
        // The frame graph reads them: the planner's node carries the tags.
        const auto graphs = parseFrameGraphDefinitionsFromConfigJson(copy.config);
        const auto frame_graph = std::find_if(graphs.begin(), graphs.end(),
                                              [](const auto &candidate) { return candidate.name == "main_render"; });
        REQUIRE(frame_graph != graphs.end());
        const auto frame_node =
            std::find_if(frame_graph->nodes.begin(), frame_graph->nodes.end(),
                         [](const auto &candidate) { return candidate.name == "opaque_color_copy"; });
        REQUIRE(frame_node != frame_graph->nodes.end());
        CHECK(frame_node->region_tags == std::vector<std::string>{"capture", "copy"});

        const auto shadow = lp0aCompose(lp0dGraph(
            Json::array({lp0dShadowRef(),
                         lp0eShadowDepth("graph_shadow", {{"node", "shadow"}, {"port", "resource"}},
                                         {{"region_tags", Json::array({"shadow"})}})}),
            {{"shadow_depth", "graph_shadow"}}));
        CHECK(passByName(shadow.config, "graph_shadow").at("regions") == Json::array({"shadow"}));

        auto graph = lpvbTemplate("cube_capture_loop.json");
        for (auto &node : graph["definitions"]["cube_capture_face"]["nodes"])
            if (node.at("id") == "geometry") node["parameters"]["region_tags"] = Json::array({"capture"});
        const auto document = TestSupport::lp1cApplyDecomposition(
            lpvbGraph(Json::array({"engine://features/cube_capture.json"})), graph);
#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
        const auto cube = lpvbCompose(document);
        CHECK(passByName(cube.config, "cube_capture_faces/3/geometry").at("regions") == Json::array({"capture"}));
#endif
    }
    // LP-1d's form: a port with binding "input" is the pass's numbered input
    // input_index (the lighting shader's PELICAN_DECLARE_INPUT_<n>).
    SECTION("binding input with input_index puts a read image at its position, ahead of the named ports") {
        auto ports = Json{{"color", {{"kind", "image"}, {"type", lp0aSceneColorType()}, {"access", "write"},
                                     {"attachment", "color"}, {"location", 0}}}};
        const auto read = [](const Json &extra) {
            Json port{{"kind", "image"}, {"type", lp0aSceneColorType()}, {"access", "read"}};
            for (const auto &[key, value] : extra.items()) port[key] = value;
            return port;
        };
        const auto graph = [&](const Json &port_set) {
            Json nodes = Json::array({lp1aLit(), lp0bImage("a_image"), lp0bImage("b_image"), lp0bImage("out_image")});
            nodes.push_back({{"id", "pass"}, {"op", "render.fullscreen"},
                             {"parameters", {{"pass_set", "main_render"},
                                             {"shader", {{"vertex", "engine://fullscreen"},
                                                         {"fragment", "engine://fullscreen"}}},
                                             {"ports", port_set}}},
                             {"inputs", {{"z_first", {{"node", "b_image"}, {"port", "resource"}}},
                                         {"a_second", {{"node", "a_image"}, {"port", "resource"}}},
                                         {"named", {{"node", "lit"}, {"port", "resource"}}},
                                         {"color", {{"node", "out_image"}, {"port", "resource"}}}}},
                             {"outputs", {"color"}}});
            return lp1aGraph(nodes);
        };
        const auto numbered = [&](std::int64_t index) {
            return read({{"binding", "input"}, {"input_index", index}});
        };
        auto positional = ports;
        positional["z_first"] = numbered(0);
        positional["a_second"] = numbered(1);
        positional["named"] = read(Json::object());
        const auto composed = lp0aCompose(graph(positional));
        const auto &pass = passByName(composed.config, "pass");
        CHECK(pass.at("input") == Json::array({"b_image", "a_image", "lit_color"}));
        CHECK(pass.at("resource_ports").size() == 1);
        CHECK(pass.at("resource_ports").contains("named"));

        // A gap is LP-1d's graph.input_index_invalid@1.
        auto gap = positional;
        gap["z_first"] = numbered(1);
        gap["a_second"] = numbered(2);
        CHECK_THROWS_AS(lp0aCompose(graph(gap)), GraphRegistryError);
        for (const auto &bad : {Json{{"binding", "input"}, {"input_index", -1}},
                                Json{{"binding", "input"}, {"input_index", 0}, {"view", "per_view"}},
                                Json{{"input_index", 0}}}) {
            INFO(bad.dump());
            auto refused = positional;
            refused["z_first"] = read(bad);
            CHECK_THROWS_AS(lp0aCompose(graph(refused)), std::invalid_argument);
        }
    }
}

namespace {
// LP-1e: the rendering config a new blank project is made with, as
// `project init --template blank` and Studio's New Project write it.
nlohmann::json lp1eBlankRenderingConfig() {
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_lp1e_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    initializeProjectTemplate(root, ProjectTemplate::Blank);
    std::ifstream file(root / "passes" / "main_rendering_config.json");
    REQUIRE(file.good());
    auto config = nlohmann::json::parse(file);
    file.close();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return config;
}

// Every name in `names` is declared after `after` (when given) and before the
// family's first canonical anchor, and so before output_transform.
void lp1eAmongScenePasses(const std::vector<std::string> &declared, const std::vector<std::string> &names,
                          std::string_view after = {}) {
    const auto first_anchor = lp0bIndex(declared, "__anchor_sprite");
    for (std::ptrdiff_t i = 0; i < first_anchor; ++i)
        CHECK_FALSE(declared[static_cast<std::size_t>(i)].starts_with("__anchor_"));
    for (const auto &name : names) {
        INFO(name);
        CHECK(lp0bIndex(declared, name) < first_anchor);
        CHECK(lp0bIndex(declared, name) < lp0bIndex(declared, "output_transform"));
        if (!after.empty()) CHECK(lp0bIndex(declared, name) > lp0bIndex(declared, after));
    }
}

const std::vector<std::string> lp1eCopies{"steps/0/fill", "steps/1/fill"};
} // namespace

TEST_CASE("LP-1e an unplaced graph pass and an unplaced loop join a blank project's scene passes",
          "[render-feature][graph-composition][lp1e]") {
    using Json = nlohmann::json;
    auto document = lp1eBlankRenderingConfig();
    REQUIRE(document.at("rendering").at("rendering_passes").at(0).at("name") == "main_render");
    document["definitions"]["paint"] = lp1aPaintBody();
    SECTION("a fullscreen pass and a loop") {
        document["nodes"] = Json::array({lp0bImage("blit_image"),
                                         lp1aFullscreen("blit", {{"node", "blit_image"}, {"port", "resource"}}),
                                         lp1aLoop("steps", lp1aRange(2))});
        const auto composed = lp0aCompose(document);
        const auto declared = lp0bDeclared(composed.config);
        lp1eAmongScenePasses(declared, {"present", "blit", "steps/0/fill", "steps/1/fill"});
        // The graph's own order: the loop stands by the graph pass it ranks
        // after (ids, with no edge between them).
        CHECK(lp0bIndex(declared, "blit") < lp0bIndex(declared, "steps/0/fill"));
        CHECK(lp0bIndex(declared, "steps/0/fill") + 1 == lp0bIndex(declared, "steps/1/fill"));
        const auto order = lp0bPlanOrder(composed);
        for (const auto *name : {"blit", "steps/0/fill", "steps/1/fill"})
            CHECK(lp0bIndex(order, name) < lp0bIndex(order, "output_transform"));
        REQUIRE_NOTHROW(compileComposition(composed));
    }
    SECTION("a loop alone") {
        document["nodes"] = Json::array({lp1aLoop("steps", lp1aRange(2))});
        const auto composed = lp0aCompose(document);
        const auto declared = lp0bDeclared(composed.config);
        lp1eAmongScenePasses(declared, lp1eCopies);
        CHECK(lp0bIndex(declared, "steps/0/fill") + 1 == lp0bIndex(declared, "steps/1/fill"));
        const auto order = lp0bPlanOrder(composed);
        for (const auto &name : lp1eCopies)
            CHECK(lp0bIndex(order, name) < lp0bIndex(order, "output_transform"));
        REQUIRE_NOTHROW(compileComposition(composed));
    }
    SECTION("a pass and a loop with no pass_set join the config's first family") {
        auto blit = lp1aFullscreen("blit", {{"node", "blit_image"}, {"port", "resource"}});
        blit["parameters"].erase("pass_set");
        auto body = lp1aPaintBody();
        for (auto &node : body["nodes"]) node["parameters"].erase("pass_set");
        document["definitions"]["paint"] = body;
        document["nodes"] = Json::array({lp0bImage("blit_image"), blit, lp1aLoop("steps", lp1aRange(2))});
        const auto composed = lp0aCompose(document);
        std::vector<std::string> families;
        for (const auto &family : composed.config.at("rendering_passes"))
            families.push_back(family.at("name").get<std::string>());
        CHECK(families == std::vector<std::string>{"main_render"});
        const auto declared = lp0bDeclared(composed.config);
        lp1eAmongScenePasses(declared, {"present", "blit", "steps/0/fill", "steps/1/fill"});
        const auto order = lp0bPlanOrder(composed);
        for (const auto *name : {"blit", "steps/0/fill", "steps/1/fill"})
            CHECK(lp0bIndex(order, name) < lp0bIndex(order, "output_transform"));
        REQUIRE_NOTHROW(compileComposition(composed));
    }
}

TEST_CASE("LP-1e an unplaced graph pass and an unplaced loop land after the hybrid scene passes, before sprite",
          "[render-feature][graph-composition][lp1e]") {
    using Json = nlohmann::json;
    SECTION("a fullscreen pass and a loop") {
        const auto composed = lp0aCompose(lp1aGraph(Json::array({
            lp0bImage("blit_image"), lp1aFullscreen("blit", {{"node", "blit_image"}, {"port", "resource"}}),
            lp1aLoop("steps", lp1aRange(2))})));
        const auto declared = lp0bDeclared(composed.config);
        lp1eAmongScenePasses(declared, {"blit", "steps/0/fill", "steps/1/fill"}, "forward_transparent");
        const auto order = lp0bPlanOrder(composed);
        for (const auto *name : {"blit", "steps/0/fill", "steps/1/fill"}) {
            CHECK(lp0bIndex(order, name) > lp0bIndex(order, "forward_transparent"));
            CHECK(lp0bIndex(order, name) < lp0bIndex(order, "__anchor_sprite"));
        }
        REQUIRE_NOTHROW(compileComposition(composed));
    }
    SECTION("a loop alone") {
        const auto composed = lp0aCompose(lp1aGraph(Json::array({lp1aLoop("steps", lp1aRange(2))})));
        const auto declared = lp0bDeclared(composed.config);
        lp1eAmongScenePasses(declared, lp1eCopies, "forward_transparent");
        CHECK(lp0bIndex(declared, "steps/1/fill") + 1 == lp0bIndex(declared, "__anchor_sprite"));
        const auto order = lp0bPlanOrder(composed);
        for (const auto &name : lp1eCopies)
            CHECK(lp0bIndex(order, name) < lp0bIndex(order, "__anchor_sprite"));
        REQUIRE_NOTHROW(compileComposition(composed));
    }
    SECTION("a loop that waits on a feature's pass stands after it") {
        // An after naming a pass past the first anchor keeps the copies
        // after that pass, not among the scene passes.
        auto parameters = lp1aRange(2);
        parameters["after"] = {"HighLuminanceExtraction"};
        const auto composed = lp0aCompose(lp1aGraph(Json::array({lp1aLoop("steps", parameters)})));
        const auto declared = lp0bDeclared(composed.config);
        for (const auto &name : lp1eCopies) {
            CHECK(lp0bIndex(declared, name) > lp0bIndex(declared, "HighLuminanceExtraction"));
            CHECK(lp0bIndex(declared, name) < lp0bIndex(declared, "output_transform"));
        }
        REQUIRE_NOTHROW(compileComposition(composed));
    }
}

} // namespace Pelican
