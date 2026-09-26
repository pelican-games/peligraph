#include "../src/project/featurecompose.hpp"
#include "../src/project/graphpartition.hpp"
#include "../src/project/rendergraphcomposition.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/project/renderconfigsection.hpp"
#include "render_graph_root_test_support.hpp"
#include "../src/project/renderfeatureoverlay.hpp"
#include "../src/project/renderpipeline.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/pathresolver.hpp"
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
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
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

} // namespace Pelican
