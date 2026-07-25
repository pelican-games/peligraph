#include "previewgraph.hpp"

#include "renderingpassconfigloader.hpp"
#include "renderstrategyregistry.hpp"
#include "../../project/renderpipeline.hpp"

#include <stdexcept>

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

PreviewGraphProgram precompilePreviewGraph(
    std::string_view rendering_config_json,
    const std::function<std::string(std::string_view)> &load_feature_json,
    bool runtime_shader_compiler_enabled) {
    const auto base = loadRenderingPassConfigJsonFromString(
        rendering_config_json, "preview graph");
    const auto strategy_providers =
        renderStrategyRegistry().snapshot();
    auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{base, "preview graph"},
        RenderEnvironmentCapabilities{
            runtime_shader_compiler_enabled,
            RenderPipelineGraphVariant::preview},
        RenderPipelineResolveDependencies{
            .load_feature_json = load_feature_json,
            .load_pipeline_json = load_feature_json,
            .resolve_render_strategy =
                [&strategy_providers,
                 runtime_shader_compiler_enabled](
                    const nlohmann::json &config,
                    const CompiledGraphVariantPolicy
                        &policy) {
                    return resolveRenderStrategy(
                               config, policy,
                               runtime_shader_compiler_enabled,
                               strategy_providers)
                        .config;
                },
        });

    PreviewGraphProgram result;
    result.excluded_feature_names =
        std::move(resolved.excluded_feature_names);
    result.graph_variant_policy =
        resolved.graph_variant_policy;
    result.composed_config =
        std::move(resolved.normalized_config);
    result.generation = generationOf(result.composed_config);
    for (const auto &graph :
         result.composed_config.at("rendering_passes")) {
        if (!graph.is_object() || !graph.contains("passes") ||
            !graph.at("passes").is_array()) {
            continue;
        }
        for (const auto &pass : graph.at("passes")) {
            if (pass.is_object() &&
                pass.value("type", std::string{}) !=
                    "canonical_anchor") {
                result.pass_names.push_back(
                    pass.value("name",
                               std::string{"unnamed"}));
            }
        }
    }
    // The authored swapchain terminal is a semantic output in this program;
    // PreviewExecutor binds it to its request-local capture image.
    return result;
}

} // namespace Pelican
