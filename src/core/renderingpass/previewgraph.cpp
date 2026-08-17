#include "previewgraph.hpp"

#include "passfieldownershipcapabilities.hpp"
#include "renderingpassconfigloader.hpp"
#include "renderstrategyregistry.hpp"
#include "../../project/renderpipeline.hpp"

#include <optional>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

std::uint64_t generationOf(const nlohmann::json &config) {
    // Keep the value exactly representable by JSON number consumers.
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : config.dump()) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    hash &= (1ull << 53u) - 1u;
    return hash == 0 ? 1 : hash;
}

} // namespace

PreviewGraphProgram makePreviewGraphProgram(
    std::shared_ptr<const CompiledRenderPipeline>
        render_pipeline,
    nlohmann::json composed_config) {
    if (render_pipeline == nullptr) {
        throw std::runtime_error(
            "preview graph requires a compiled render "
            "pipeline");
    }
    if (render_pipeline->graph_variant_policy.variant !=
        RenderPipelineGraphVariant::preview) {
        throw std::runtime_error(
            "preview graph requires the preview graph "
            "variant");
    }
    if (!composed_config.is_object() ||
        !composed_config.contains(
            "rendering_passes") ||
        !composed_config.at(
            "rendering_passes").is_array()) {
        throw std::runtime_error(
            "preview graph requires a composed "
            "rendering_passes array");
    }

    PreviewGraphProgram result;
    result.excluded_feature_names =
        render_pipeline->excluded_feature_names;
    result.render_pipeline =
        std::move(render_pipeline);
    result.graph_variant_policy =
        result.render_pipeline
            ->graph_variant_policy;
    result.composed_config =
        std::move(composed_config);
    result.generation =
        generationOf(result.composed_config);
    for (const auto &graph :
         result.composed_config.at(
             "rendering_passes")) {
        if (!graph.is_object() ||
            !graph.contains("passes") ||
            !graph.at("passes").is_array()) {
            continue;
        }
        for (const auto &pass :
             graph.at("passes")) {
            if (pass.is_object() &&
                pass.value(
                    "type", std::string{}) !=
                    "canonical_anchor") {
                result.pass_names.push_back(
                    pass.value(
                        "name",
                        std::string{"unnamed"}));
            }
        }
    }
    // The authored swapchain terminal is a semantic output in this program;
    // PreviewExecutor binds it to its request-local capture image.
    return result;
}

PreviewGraphProgram precompilePreviewGraph(
    std::string_view rendering_config_json,
    const std::function<std::string(std::string_view)> &load_feature_json,
    bool runtime_shader_compiler_enabled) {
    const auto base = loadRenderingPassConfigJsonFromString(
        rendering_config_json, "preview graph");
    const auto strategy_providers =
        renderStrategyRegistry().snapshot();
    std::optional<RenderStrategySelection>
        resolved_render_strategy;
    auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{base, "preview graph"},
        RenderEnvironmentCapabilities{
            .runtime_shader_compiler_enabled =
                runtime_shader_compiler_enabled,
            .graph_variant =
                RenderPipelineGraphVariant::preview,
            .pass_field_ownership =
                buildPassFieldOwnershipCapabilities(),
        },
        RenderPipelineResolveDependencies{
            .load_feature_json = load_feature_json,
            .load_pipeline_json = load_feature_json,
            .resolve_render_strategy =
                [&strategy_providers,
                 &resolved_render_strategy,
                 runtime_shader_compiler_enabled](
                    const nlohmann::json &config,
                    const CompiledGraphVariantPolicy
                        &policy) {
                    auto generated =
                        resolveRenderStrategy(
                            config, policy,
                            runtime_shader_compiler_enabled,
                            strategy_providers);
                    resolved_render_strategy =
                        generated.selection;
                    return std::move(
                        generated.config);
                },
        });
    auto compiled_pipeline =
        compileRenderPipeline(resolved);
    compiled_pipeline.render_strategy =
        std::move(resolved_render_strategy);
    return makePreviewGraphProgram(
        std::make_shared<
            const CompiledRenderPipeline>(
            std::move(compiled_pipeline)),
        std::move(resolved.normalized_config));
}

} // namespace Pelican
