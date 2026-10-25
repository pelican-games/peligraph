#include "previewgraph.hpp"

#include "passfieldownershipcapabilities.hpp"
#include "renderingpassconfigloader.hpp"
#include "renderstrategyregistry.hpp"
#include "../../project/graphcomposition.hpp"
#include "../../project/renderpipeline.hpp"
#include "../renderer/viewfamilyproviderregistry.hpp"

#include <exception>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

// The data-only resolve and compile of one variant (the edit preflight's).
std::pair<CompiledRenderPipeline, nlohmann::json> resolveDataOnlyVariant(
    const nlohmann::json &base, std::string_view source_name,
    RenderPipelineGraphVariant variant,
    const std::function<std::string(std::string_view)> &load_feature_json,
    bool runtime_shader_compiler_enabled) {
    const auto strategy_providers =
        renderStrategyRegistry().snapshot();
    std::optional<RenderStrategySelection>
        resolved_render_strategy;
    auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{base, std::string{source_name}},
        RenderEnvironmentCapabilities{
            .runtime_shader_compiler_enabled =
                runtime_shader_compiler_enabled,
            .graph_variant = variant,
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
            // LP-Vb: a graph.loop over view_axis is sized by the providers.
            .view_axis_length = renderGraphViewAxisLengthResolver(),
        });
    auto compiled_pipeline =
        compileRenderPipeline(resolved);
    compiled_pipeline.render_strategy =
        std::move(resolved_render_strategy);
    return {std::move(compiled_pipeline),
            std::move(resolved.composed_config)};
}

} // namespace

nlohmann::json previewUnsupportedAuthoringReason(
    const std::exception_ptr &refusal) {
    nlohmann::json reason{
        {"code", "preview.unsupported_authoring@1"},
        {"feature", "composition"},
    };
    std::string message = "preview graph could not be composed";
    try {
        if (refusal) std::rethrow_exception(refusal);
    } catch (const PreviewUnsupportedAuthoringError &error) {
        reason["feature"] = error.feature();
        if (!error.target().empty()) reason["target"] = error.target();
        if (!error.graph().empty()) reason["graph"] = error.graph();
        if (!error.pass().empty()) reason["pass"] = error.pass();
        message = error.what();
    } catch (const GraphRegistryError &error) {
        auto cause = error.detail().is_object() ? error.detail()
                                                : nlohmann::json::object();
        cause["code"] = error.code();
        reason["cause"] = std::move(cause);
        message = error.what();
    } catch (const std::exception &error) {
        message = error.what();
        if (auto coded = splitCodedRefusalMessage(message)) {
            auto cause = coded->fields;
            cause["code"] = coded->code;
            reason["cause"] = std::move(cause);
        }
    } catch (...) {
    }
    reason["message"] = std::move(message);
    return reason;
}

PreviewGraphProgram unavailablePreviewGraphProgram(nlohmann::json reason) {
    PreviewGraphProgram result;
    result.available = false;
    result.unavailable_reason = std::move(reason);
    return result;
}

nlohmann::json previewGraphStatusJson(const PreviewGraphProgram &program) {
    if (program.available) return {{"available", true}};
    return {{"available", false}, {"reason", program.unavailable_reason}};
}

PreviewGraphProgram makePreviewGraphProgram(
    std::shared_ptr<const CompiledRenderPipeline>
        render_pipeline,
    const RenderGraphIR &render_graph) {
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
    std::set<std::string> graph_names;
    for (const auto &graph : render_graph.graphs) {
        if (graph.name.empty() || !graph_names.insert(graph.name).second)
            throw GraphRegistryError{"render_graph.preview_invalid@1",
                {{"graph", graph.name}, {"reason", "graph_name_invalid"}}};
        std::set<std::string> node_names;
        for (const auto &node : graph.nodes) {
            if (node.name.empty() || !node_names.insert(node.name).second)
                throw GraphRegistryError{"render_graph.preview_invalid@1",
                    {{"graph", graph.name}, {"node", node.name}, {"reason", "node_name_invalid"}}};
        }
    }

    PreviewGraphProgram result;
    result.excluded_feature_names =
        render_pipeline->excluded_feature_names;
    result.render_pipeline =
        std::move(render_pipeline);
    result.graph_variant_policy =
        result.render_pipeline
            ->graph_variant_policy;
    // Keep the RPC value exactly representable by JSON number consumers.
    result.generation = renderGraphFingerprint(render_graph) & ((1ull << 53u) - 1u);
    if (result.generation == 0) result.generation = 1;
    // The graphs' own passes (not their compute tasks), anchors aside.
    for (const auto &graph : render_graph.graphs) {
        for (const auto &node : graph.nodes) {
            if (node.scope == NodeDeclarationScope::graph &&
                node.shape != RenderNodeShape::compute &&
                node.shape != RenderNodeShape::anchor) {
                result.pass_names.push_back(node.name);
            }
        }
    }
    // The authored swapchain terminal is a semantic output in this program;
    // PreviewExecutor binds it to its request-local capture image.
    return result;
}

void preflightRenderGraphVariants(
    std::string_view rendering_config_json,
    const std::function<std::string(std::string_view)> &load_feature_json,
    bool runtime_shader_compiler_enabled, bool xr_active) {
    try {
        const auto base = loadRenderingPassConfigJsonFromString(
            rendering_config_json, "edit preflight");
        (void)resolveDataOnlyVariant(
            base, "edit preflight",
            RenderPipelineGraphVariant::flat, load_feature_json,
            runtime_shader_compiler_enabled);
    } catch (const std::exception &) {
        return;
    }
    // PV-1: preview is not composed: what it cannot take is published with
    // preview unavailable, not refused.
#if PELICAN_WITH_OPENXR
    if (xr_active) {
        try {
            const auto base = loadRenderingPassConfigJsonFromString(
                rendering_config_json, "edit preflight");
            (void)resolveDataOnlyVariant(
                base, "edit preflight",
                RenderPipelineGraphVariant::xr, load_feature_json,
                runtime_shader_compiler_enabled);
        } catch (const std::exception &error) {
            throw GraphVariantCompositionError{
                RenderPipelineGraphVariant::xr, error.what(),
                std::current_exception()};
        }
    }
#else
    (void)xr_active;
#endif
}

} // namespace Pelican
