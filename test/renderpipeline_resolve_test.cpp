#include "../src/core/renderingpass/previewgraph.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/project/featurecompose.hpp"
#include "../src/project/renderpipeline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;

Json featureEnvelope(std::string name) {
    return {
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", std::move(name)},
    };
}

Json baseConfig(std::vector<std::string> features = {}) {
    return {
        {"features", std::move(features)},
        {"render_targets", Json::array()},
        {"rendering_passes",
         Json::array({
             {
                 {"name", "main"},
                 {"passes",
                  Json::array({
                      {
                          {"name", "scene"},
                          {"type", "material"},
                          {"output",
                           {{"color", Json::array({"swapchain"})},
                            {"depth", nullptr}}},
                      },
                  })},
             },
         })},
    };
}

std::string loadEngineResource(std::string_view reference) {
    constexpr std::string_view prefix = "engine://";
    if (!reference.starts_with(prefix)) {
        throw std::runtime_error("expected engine resource reference");
    }
    return engineResourceOrThrow(reference.substr(prefix.size()));
}

} // namespace

TEST_CASE("WP180 flat resolver preserves FeatureCompose bytes without modules",
          "[wp180][render-pipeline][resolve][flat]") {
    const auto preset = Json{
        {"schema", "pelican.render_pipeline"},
        {"version", 1},
        {"name", "fixture_v1"},
        {"config", baseConfig({"fixture://safe"})},
    };
    const auto authored = Json{
        {"pipeline", {{"preset", "fixture://pipeline"}}},
        {"shader_defines", Json::array({"PELICAN_FIXTURE"})},
    };
    const auto load_feature = [](std::string_view ref) {
        if (ref != "fixture://safe") {
            throw std::runtime_error("unknown feature");
        }
        return featureEnvelope("safe").dump();
    };
    const auto load_pipeline = [&preset](std::string_view ref) {
        if (ref != "fixture://pipeline") {
            throw std::runtime_error("unknown pipeline");
        }
        return preset.dump();
    };

    const auto legacy = composeRenderFeatureConfig(
        authored,
        RenderFeatureComposeDependencies{load_feature, true, {},
                                         load_pipeline});
    const auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{authored, "flat fixture"},
        RenderEnvironmentCapabilities{true,
                                      RenderPipelineGraphVariant::flat},
        RenderPipelineResolveDependencies{load_feature, load_pipeline});

    REQUIRE(resolved.normalized_config.dump() == legacy.config.dump());
    REQUIRE(resolved.shader_defines == legacy.shader_defines);
    REQUIRE(resolved.feature_names == legacy.feature_names);
    REQUIRE(resolved.excluded_feature_names ==
            legacy.excluded_feature_names);
    REQUIRE(resolved.material_routing == legacy.material_routing);
    REQUIRE(resolved.pipeline_preset.has_value());
    REQUIRE(resolved.pipeline_preset->reference == "fixture://pipeline");
    REQUIRE(resolved.pipeline_preset->name == "fixture_v1");
    REQUIRE(resolved.pipeline_preset->version == 1);

    const auto metadata =
        serializeRenderPipelineCompositionMetadata(resolved);
    REQUIRE(metadata == Json{
                            {"pipeline_preset",
                             {{"ref", "fixture://pipeline"},
                              {"name", "fixture_v1"},
                              {"version", 1}}},
                        });
    REQUIRE(resolved.diagnostics.front().kind ==
            RenderPipelineDiagnosticKind::graph_variant_selected);
    REQUIRE(resolved.diagnostics.front().subject == "flat");
}

TEST_CASE("WP180 XR resolution preserves policy order suffix and metadata",
          "[wp180][render-pipeline][resolve][xr]") {
    const std::unordered_map<std::string, Json> features{
        {"fixture://safe", featureEnvelope("safe")},
        {"fixture://ui", featureEnvelope("ui")},
    };
    const auto authored = baseConfig({"fixture://safe", "fixture://ui"});
    const auto load_feature = [&features](std::string_view ref) {
        return features.at(std::string{ref}).dump();
    };
    const auto include_feature =
        [](std::string_view name, const Json &) { return name != "ui"; };

    auto legacy = composeRenderFeatureConfig(
        authored,
        RenderFeatureComposeDependencies{load_feature, true,
                                         include_feature});
    legacy.config["normalization_probe"] = "normalized";
    legacy.config["transform_probe"] = "transformed";
    for (auto &graph : legacy.config.at("rendering_passes")) {
        graph["name"] = graph.at("name").get<std::string>() + "#xr";
    }

    std::vector<std::string> resolution_order;
    const auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{authored, "xr fixture"},
        RenderEnvironmentCapabilities{true,
                                      RenderPipelineGraphVariant::xr},
        RenderPipelineResolveDependencies{
            load_feature,
            {},
            include_feature,
            [&resolution_order](const Json &config,
                                const std::vector<std::string> &) {
                resolution_order.push_back("normalize");
                auto normalized = config;
                normalized["normalization_probe"] = "normalized";
                return normalized;
            },
            [&resolution_order](Json &config) {
                resolution_order.push_back("transform");
                REQUIRE(config.at("normalization_probe") == "normalized");
                config["transform_probe"] = "transformed";
            },
            [&resolution_order](const Json &config) {
                resolution_order.push_back("validate");
                REQUIRE(config.at("normalization_probe") == "normalized");
                REQUIRE(config.at("transform_probe") == "transformed");
                REQUIRE(config.at("rendering_passes").at(0).at("name") ==
                        "main");
            },
            "#xr",
        });

    REQUIRE(resolution_order ==
            std::vector<std::string>{"normalize", "transform", "validate"});
    REQUIRE(resolved.normalized_config.dump() == legacy.config.dump());
    REQUIRE(resolved.feature_names == std::vector<std::string>{"safe"});
    REQUIRE(resolved.excluded_feature_names ==
            std::vector<std::string>{"ui"});
    REQUIRE(serializeRenderPipelineCompositionMetadata(resolved) ==
            Json{{"graph_variant", "xr"},
                 {"excluded_features", Json::array({"ui"})}});
    REQUIRE(std::any_of(
        resolved.diagnostics.begin(), resolved.diagnostics.end(),
        [](const auto &diagnostic) {
            return diagnostic.kind ==
                       RenderPipelineDiagnosticKind::feature_excluded &&
                   diagnostic.subject == "ui" && diagnostic.detail == "xr";
        }));
}

TEST_CASE("WP180 hybrid resolver preserves semantic material routing",
          "[wp180][render-pipeline][resolve][hybrid]") {
    const auto authored = Json{
        {"pipeline",
         {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
        {"features", Json::array({"engine://features/ui.json"})},
    };
    const auto legacy = composeRenderFeatureConfig(
        authored,
        RenderFeatureComposeDependencies{loadEngineResource, true, {},
                                         loadEngineResource});
    const auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{authored, "hybrid fixture"},
        RenderEnvironmentCapabilities{true,
                                      RenderPipelineGraphVariant::flat},
        RenderPipelineResolveDependencies{loadEngineResource,
                                          loadEngineResource});

    REQUIRE(resolved.normalized_config.dump() == legacy.config.dump());
    REQUIRE(resolved.material_routing == legacy.material_routing);
    REQUIRE(resolved.material_routing.at("routes")
                .at("deferred_geometry")
                .at("pass") == "deferred_geometry");
    REQUIRE(resolved.material_routing.at("routes")
                .at("forward_transparent")
                .at("phase") == "transparent");
    const auto metadata =
        serializeRenderPipelineCompositionMetadata(resolved);
    REQUIRE(metadata.at("material_routing") == legacy.material_routing);
    REQUIRE(metadata.at("pipeline_preset").at("name") == "hybrid_v1");
    REQUIRE_FALSE(metadata.contains("graph_variant"));
}

TEST_CASE("WP180 resolve failure cannot publish partial registration state",
          "[wp180][render-pipeline][resolve][atomic]") {
    struct RegistrationProbe {
        int render_targets = 3;
        int passes = 5;
        void publish(const ResolvedRenderPipeline &) {
            ++render_targets;
            ++passes;
        }
    } registration;
    const auto before_targets = registration.render_targets;
    const auto before_passes = registration.passes;
    const auto authored = baseConfig();
    const auto authored_before = authored;

    const auto resolve_then_publish = [&] {
        const auto resolved = resolveRenderPipeline(
            RenderPipelineRequest{authored, "invalid fixture"},
            RenderEnvironmentCapabilities{},
            RenderPipelineResolveDependencies{
                {}, {}, {}, {},
                [](Json &config) { config["partial_transform"] = true; },
                [](const Json &config) {
                    REQUIRE(config.at("partial_transform") == true);
                    throw std::runtime_error("fixture validation failed");
                },
            });
        registration.publish(resolved);
    };

    REQUIRE_THROWS_WITH(resolve_then_publish(), "fixture validation failed");
    REQUIRE(authored == authored_before);
    REQUIRE(registration.render_targets == before_targets);
    REQUIRE(registration.passes == before_passes);
}

TEST_CASE("WP180 preview precompile resolves presets through the shared boundary",
          "[wp180][render-pipeline][resolve][preview]") {
    const auto preset = Json{
        {"schema", "pelican.render_pipeline"},
        {"version", 1},
        {"name", "preview_fixture_v1"},
        {"config", baseConfig()},
    };
    const auto program = precompilePreviewGraph(
        Json{{"pipeline", {{"preset", "fixture://preview_pipeline"}}}}
            .dump(),
        [&preset](std::string_view ref) {
            if (ref != "fixture://preview_pipeline") {
                throw std::runtime_error("unexpected preview resource");
            }
            return preset.dump();
        },
        true);

    REQUIRE(program.generation != 0);
    REQUIRE(program.composed_config.dump().find("swapchain") ==
            std::string::npos);
    REQUIRE(program.composed_config.dump().find("preview_capture") !=
            std::string::npos);
    REQUIRE(program.pass_names ==
            std::vector<std::string>{"scene", "output_transform"});
}

} // namespace Pelican
