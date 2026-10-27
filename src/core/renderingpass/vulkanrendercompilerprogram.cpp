#include "vulkanrendercompilerpackage.hpp"

#include "computetask.hpp"
#include "graphtransformregistry.hpp"
#include "passfieldownershipcapabilities.hpp"
#include "renderstrategyregistry.hpp"
#include "rendertargetjsonparser.hpp"
#include "subgraphreplacementregistry.hpp"
#include "../loader/pathresolver.hpp"
#include "../shader/raygrouplayout.hpp"
#include "../renderer/viewfamilyproviderregistry.hpp"
#include "../../project/graphcomposition.hpp"
#include "../../project/graphvariantpolicy.hpp"
#include "../../project/renderconfigsection.hpp"
#include "../../project/rendergraphparse.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <iterator>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace Pelican {
namespace {

// RC-3c1b: read from the finalized IR (renderGraphMultiviewCapableNodes and
// the nodes' schedule, family and shape).
std::optional<VulkanViewExecutionPlanRequest>
targetViewExecutionRequest(
    const CompiledRenderPipeline &pipeline,
    const RenderGraphIR &render_graph,
    bool enable_multiview_runtime) {
    if (pipeline.graph_variant_policy.variant !=
        RenderPipelineGraphVariant::xr) {
        return std::nullopt;
    }
    if (!pipeline.xr_target_policy.active ||
        pipeline.graph_variant_policy.view_count == 0) {
        throw std::runtime_error(
            "compiled XR pipeline lacks an active target-view "
            "policy");
    }
    VulkanViewExecutionPlanRequest request{
        .view_count =
            pipeline.graph_variant_policy.view_count,
        .preference =
            pipeline.xr_target_policy.view_execution,
        .automatic_policy =
            pipeline.xr_target_policy.multiview_auto,
    };
    const auto capable_candidates =
        renderGraphMultiviewCapableNodes(
            render_graph, enable_multiview_runtime);
    std::set<std::string, std::less<>>
        independent_candidates;
    const bool sprite_enabled =
        std::find(
            pipeline.feature_names.begin(),
            pipeline.feature_names.end(),
            "sprite") != pipeline.feature_names.end();
    for (const auto &graph : render_graph.graphs) {
        for (const auto &node : graph.nodes) {
            // A config compute task; PS-1: a once task, when it runs, runs
            // as a per_frame one.
            if (node.shape == RenderNodeShape::compute &&
                node.scope == NodeDeclarationScope::shared &&
                node.schedule != NodeSchedule::per_view) {
                independent_candidates.insert(node.name);
            }
            if (node.view_family !=
                mainRenderViewFamilyId) {
                independent_candidates.insert(
                    node.name);
            }
            if (node.shape == RenderNodeShape::acceleration_structure)
                independent_candidates.insert(node.name);
            if (node.shape == RenderNodeShape::anchor &&
                (!sprite_enabled ||
                 node.name != "__anchor_sprite")) {
                independent_candidates.insert(node.name);
            }
        }
    }

    // One request is shared by every graph in a variant. Advertise only names
    // present in every graph so graph-specific work remains sequential.
    const auto present_in_every_graph =
        [&](const std::string &name) {
            return !render_graph.graphs.empty() &&
                   std::all_of(
                       render_graph.graphs.begin(), render_graph.graphs.end(),
                       [&](const GraphIR &graph) {
                           return std::find_if(
                                      graph.nodes.begin(),
                                      graph.nodes.end(),
                                      [&](const auto &node) {
                                          return node.name ==
                                                 name;
                                      }) !=
                                  graph.nodes.end();
                       });
        };
    for (const auto &name : capable_candidates) {
        if (present_in_every_graph(name)) {
            request.multiview_capable_nodes.push_back(
                name);
        }
    }
    for (const auto &name : independent_candidates) {
        if (present_in_every_graph(name)) {
            request.view_independent_nodes.push_back(
                name);
        }
    }
    return request;
}

std::unordered_map<
    std::string,
    std::shared_ptr<const VulkanTargetPlan>>
targetPlansByName(
    const RenderingTargetPlanCompilation
        &compilation) {
    std::unordered_map<
        std::string,
        std::shared_ptr<const VulkanTargetPlan>>
        by_name;
    for (const auto &plan : compilation.plans) {
        if (plan == nullptr ||
            !by_name.emplace(plan->graph, plan).second) {
            throw std::runtime_error(
                "Duplicate or null physical target plan");
        }
    }
    return by_name;
}

} // namespace

namespace {

// The provider phase's result: the final typed revision and its policy.
// Temporary composition/provider JSON stays inside compileDefaultLogicalVariant.
struct DefaultLogicalVariantCompilation {
    CompiledRenderPipeline compiled_pipeline;
    RenderGraphIR render_graph;
};

void validateVulkanDevicePlanningContext(
    const VulkanRenderCompilerBackendContext
        &backend) {
    switch (backend.device_planning_mode) {
    case VulkanRenderCompilerDevicePlanningMode::
        device_required:
        if (!backend.physical_device) {
            throw std::runtime_error(
                "Vulkan render compiler "
                "device_planning_mode=device_required "
                "requires physical_device");
        }
        return;
    case VulkanRenderCompilerDevicePlanningMode::
        compiler_only:
        if (backend.physical_device) {
            throw std::runtime_error(
                "Vulkan render compiler "
                "device_planning_mode=compiler_only "
                "forbids physical_device");
        }
        return;
    default:
        throw std::runtime_error(
            "Vulkan render compiler backend context has "
            "unknown device_planning_mode");
    }
}

void requireRayMaterialBindlessForGraphs(
    CompiledRenderPipeline &pipeline,
    std::span<const std::string> graph_names) {
    for (const auto &graph_name : graph_names) {
        auto constraints = std::find_if(
            pipeline.target_planning.graphs.begin(),
            pipeline.target_planning.graphs.end(),
            [&](const auto &candidate) {
                return candidate.graph == graph_name;
            });
        if (constraints == pipeline.target_planning.graphs.end()) {
            pipeline.target_planning.graphs.push_back(
                PlanningGraphConstraints{.graph = graph_name});
            constraints =
                std::prev(pipeline.target_planning.graphs.end());
        }
        auto &required = constraints->required_capabilities;
        const std::string capability{
            vulkanRayMaterialBindlessCapability};
        if (std::find(required.begin(), required.end(), capability) ==
            required.end()) {
            required.push_back(capability);
            std::sort(required.begin(), required.end());
        }
    }
}

DefaultLogicalVariantCompilation
compileDefaultLogicalVariant(
    const RenderCompilerProgramInput &input,
    const VulkanRenderCompilerBackendContext
        &backend,
    const RenderCompilerProgramVariantRequest
        &request) {
    std::optional<RenderStrategySelection>
        resolved_render_strategy;
    const auto load_document = [&input](std::string_view ref) {
        if (input.load_document) {
            return input.load_document(ref);
        }
        return input.path_resolver.loadText(ref);
    };
    RenderPipelineResolveDependencies
        resolve_dependencies{
            .load_feature_json =
                [&load_document](std::string_view ref) {
                    return load_document(ref);
                },
            .load_pipeline_json =
                [&load_document](std::string_view ref) {
                    return load_document(ref);
                },
            .resolve_render_strategy =
                [&input,
                 &resolved_render_strategy](
                    const nlohmann::json &config,
                    const CompiledGraphVariantPolicy
                        &graph_variant_policy) {
                    auto generated =
                        resolveRenderStrategy(
                            config,
                            graph_variant_policy,
                            input.runtime_shader_compiler_enabled,
                            input.render_strategies);
                    resolved_render_strategy =
                        generated.selection;
                    return std::move(generated.config);
                },
            .validate_feature = input.validate_feature,
            .inactive_graph_scopes = input.inactive_graph_scopes,
            // LP-Vb: a graph.loop over view_axis is sized by the providers.
            .view_axis_length = renderGraphViewAxisLengthResolver(),
        };
    if (request.artifact ==
        RenderCompilerProgramArtifact::
            runtime_package) {
        resolve_dependencies.normalize_config =
            [&backend](
                const nlohmann::json &config,
                const std::vector<std::string>
                    &feature_names) {
                const bool hdr_enabled =
                    std::find(
                        feature_names.begin(),
                        feature_names.end(),
                        "hdr") !=
                    feature_names.end();
                return resolveRenderTargetFormatClassesV2(
                    config,
                    backend.output_format,
                    backend.output_extent,
                    hdr_enabled);
            };
    }
    auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{
            input.rendering_config,
            input.source_name},
        RenderEnvironmentCapabilities{
            .runtime_shader_compiler_enabled =
                input.runtime_shader_compiler_enabled,
            .graph_variant = request.graph_variant,
            .pass_field_ownership =
                buildPassFieldOwnershipCapabilities(),
        },
        resolve_dependencies);
    // Parse revision zero once, after runtime host additions. Routing must
    // see that typed declaration before unrelated metadata indices can reject
    // it (for example a surface-contract index across graph variants).
    auto composed_config = resolved.composed_config;
    if (request.artifact != RenderCompilerProgramArtifact::data_only && request.compose_runtime_config)
        request.compose_runtime_config(composed_config);
    auto initial_graph = parseRenderGraph(composed_config, buildPassFieldOwnershipCapabilities());
    auto compiled_pipeline_value = compileRenderPipeline(resolved, &initial_graph);
    if (input.ray_tracing_project) {
        compiled_pipeline_value.ray_group_layout_reference =
            input.ray_tracing_project->layout.source_reference;
        if (input.ray_tracing_project->bindings) {
            compiled_pipeline_value.ray_bindings_reference =
                input.ray_tracing_project->bindings->source_reference;
        }
    }
    if (request.artifact ==
        RenderCompilerProgramArtifact::data_only) {
        // Preview remains a request-local CPU program: resolve feature and
        // strategy policy here, but do not apply runtime host additions or
        // enter transform/subgraph/device planning that assumes concrete
        // target storage.
        compiled_pipeline_value.render_strategy =
            std::move(resolved_render_strategy);
        // The preview's IR too, with its final references resolved (a table
        // naming a task the preview left out is refused here, inside the
        // preview's own try); it plans no further.
        auto render_graph = resolveRenderGraphReferences(
            std::move(initial_graph),
            compiled_pipeline_value.scene_acceleration_structures);
        return {
            .compiled_pipeline =
                std::move(
                    compiled_pipeline_value),
            .render_graph = std::move(render_graph),
        };
    }
    // RC-3c1b: the completed graph, revision 0 (after the host's additions),
    // parsed once; every provider response is the next revision, parsed once
    // by the provider chain, which hands its IR on.
    auto resolved_transforms =
        resolveLogicalGraphTransforms(
            composed_config, std::move(initial_graph), input.graph_transforms);
    composed_config =
        std::move(resolved_transforms.config);
    validateGraphVariantConfig(
        resolved.graph_variant_policy,
        composed_config);
    auto resolved_subgraphs =
        resolveTaggedSubgraphReplacements(
            composed_config,
            std::move(resolved_transforms.render_graph),
            input.subgraph_replacements);
    composed_config =
        std::move(resolved_subgraphs.config);
    // The provenance of the final revision's graphs (the logical graph and
    // the physical plan's fingerprint carry it; the provider ABI did not).
    auto render_graph =
        std::move(resolved_subgraphs.render_graph);
    applyResolvedLogicalGraphTransformSelections(
        render_graph, resolved_transforms.selections);
    applyResolvedRenderStrategySelection(
        render_graph, resolved_render_strategy);
    applyResolvedTaggedSubgraphSelections(
        render_graph, resolved_subgraphs.graphs);
    compiled_pipeline_value.graph_transforms =
        std::move(resolved_transforms.selections);
    compiled_pipeline_value.render_strategy =
        std::move(resolved_render_strategy);

    return {
        .compiled_pipeline =
            std::move(compiled_pipeline_value),
        .render_graph = std::move(render_graph),
    };
}

RenderCompilerProgramVariantOutput
compileDefaultVulkanVariant(
    const RenderCompilerProgramInput &input,
    const VulkanRenderCompilerBackendContext
        &backend,
    const RenderCompilerProgramVariantRequest
        &request) {
    auto logical = compileDefaultLogicalVariant(
        input, backend, request);
    const auto graph_names_of =
        [](const RenderGraphIR &render_graph) {
            std::vector<std::string> names;
            names.reserve(render_graph.graphs.size());
            for (const auto &graph : render_graph.graphs)
                names.push_back(graph.name);
            return names;
        };
    if (request.artifact ==
        RenderCompilerProgramArtifact::data_only) {
        if (input.ray_tracing_project &&
            input.ray_tracing_project->layout.usesRayTables()) {
            requireRayMaterialBindlessForGraphs(
                logical.compiled_pipeline,
                graph_names_of(logical.render_graph));
        }
        return {
            .graph_variant = request.graph_variant,
            .compiled_pipeline =
                std::make_shared<
                    const CompiledRenderPipeline>(
                    std::move(
                        logical.compiled_pipeline)),
            .ray_tracing_project = input.ray_tracing_project,
            .render_graph =
                std::make_shared<const RenderGraphIR>(
                    std::move(logical.render_graph)),
        };
    }

    auto render_graph = std::move(logical.render_graph);
    // RC-3c2a: the config's images as the IR read them (an image whose format
    // class is unresolved is refused by name).
    auto render_target_definitions =
        renderTargetDefinitionsOf(render_graph);
    {
        // The authored names, before the compute tasks are namespaced, so
        // a refusal names the authored pass or task.
        std::vector<FrameGraphDefinition> graph_definitions;
        graph_definitions.reserve(render_graph.graphs.size());
        for (const auto &graph : render_graph.graphs)
            graph_definitions.push_back(
                frameGraphDefinitionOf(render_graph, graph));
        // LP-Va: every view_select names an axis the family's provider
        // declares, with a value inside its compile-time length.
        validateRenderViewSelects(
            graph_definitions,
            logical.compiled_pipeline);
        // VF-1: the raster passes of one secondary view family write
        // targets of one extent, which the renderer sizes the family's
        // views from; a pass that does not is refused here by name rather
        // than by the renderer's per-frame check
        // (resolveViewFamilyRasterExtent).
        validateRenderViewFamilyExtents(
            graph_definitions,
            render_target_definitions,
            logical.compiled_pipeline);
        // VF-2: the extent VF-1 compared each family against, for
        // get_frame_plan (Studio sizes a family's images from it).
        logical.compiled_pipeline.view_family_extents =
            renderViewFamilyExtentReferences(
                graph_definitions, render_target_definitions);
    }
    if (input.ray_tracing_project &&
        input.ray_tracing_project->layout.usesRayTables()) {
        requireRayMaterialBindlessForGraphs(
            logical.compiled_pipeline,
            graph_names_of(render_graph));
    }
    // RC-3c1b: once, after the provider chain, on the final revision: the
    // checks only it must pass, the variant's names, the scene's
    // acceleration-structure builds and frame_node_order (other variants own
    // their graph keys; the complete set is checked across the program).
    render_graph = finalizeRenderGraph(
        std::move(render_graph),
        FinalizeRenderGraphRequest{
            .named = logical.compiled_pipeline
                         .scene_acceleration_structures,
            .ray_tracing_project =
                input.ray_tracing_project != nullptr,
            .compute_task_suffix =
                logical.compiled_pipeline.graph_variant_policy
                    .rendering_pass_name_suffix,
            .output_extent =
                ResolvedResourceExtent{
                    backend.output_extent.width,
                    backend.output_extent.height},
        },
        logical.compiled_pipeline.target_planning);

    // RC-3c2b1: the provenance of the final revision, from its IR (the
    // variant's names; each config task found by its authored name). The
    // compute tasks are made from the IR by runRenderCompilerProgram.
    synchronizeRenderGraphProvenance(
        render_graph,
        logical.compiled_pipeline.pass_provenance,
        logical.compiled_pipeline.resource_provenance);

    // The frame plans, before the physical plan: the planner refuses an
    // invalid graph by name first. IA-1 / EN-2: an image holding an asset or
    // a file's pixels may be read without an earlier writer, as a buffer is.
    std::unordered_map<std::string, FramePlan> frame_plans;
    FramePlanningOptions frame_options;
    if (logical.compiled_pipeline.target_planning.profile.kind == PlanningProfileKind::hazard_stress)
        frame_options.hazard_stress_seed = logical.compiled_pipeline.target_planning.profile.seed;
    for (const auto &graph : render_graph.graphs) {
        auto plan = planRenderGraph(render_graph, graph, frame_options);
        requireRenderGraphInputsProduced(
            render_graph, graph, plan);
        frame_plans.emplace(graph.name, std::move(plan));
    }

    auto compiled_pipeline =
        std::make_shared<const CompiledRenderPipeline>(
            std::move(logical.compiled_pipeline));

    auto physical =
        std::make_unique<
            VulkanRenderCompilerPhysicalPackage>();
    auto view_execution = targetViewExecutionRequest(
        *compiled_pipeline,
        render_graph,
        request.enable_multiview_runtime);
    auto external_depth_export =
        request.enable_external_depth_export
            ? std::optional{
                  VulkanExternalDepthExportRequest{}}
            : std::nullopt;
    if (backend.device_planning_mode ==
        VulkanRenderCompilerDevicePlanningMode::
            device_required) {
        physical->target_plan_compilation =
            compileRenderingTargetPlansForVulkanDevice(
                render_graph,
                frame_plans,
                render_target_definitions,
                compiled_pipeline->sample_count_policy,
                backend.output_format,
                backend.physical_device,
                backend.runtime_capabilities,
                std::move(view_execution),
                std::move(external_depth_export),
                compiled_pipeline->target_planning,
                compiled_pipeline->vulkan_plan_pins,
                compiled_pipeline
                    ->vulkan_physical_fragments,
                compiled_pipeline->pass_provenance);
    } else {
        physical->target_plan_compilation =
            compileRenderingTargetPlans(
                render_graph,
                frame_plans,
                render_target_definitions,
                compiled_pipeline->sample_count_policy,
                backend.output_format,
                RenderingTargetPlanDeviceFacts{},
                std::move(view_execution),
                std::move(external_depth_export),
                compiled_pipeline->target_planning,
                compiled_pipeline->vulkan_plan_pins,
                compiled_pipeline
                    ->vulkan_physical_fragments,
                compiled_pipeline->pass_provenance);
    }
    for (auto &verification :
         physical->target_plan_compilation
             .verification_contexts) {
        verification.enabled_device_extensions =
            backend.enabled_device_extensions;
    }
    physical->image_placements = imagePlacementsOf(
        render_target_definitions,
        physical->target_plan_compilation);
    physical->target_plans =
        targetPlansByName(
            physical->target_plan_compilation);

    return RenderCompilerProgramVariantOutput{
        .graph_variant = request.graph_variant,
        .compiled_pipeline =
            std::move(compiled_pipeline),
        .ray_tracing_project = input.ray_tracing_project,
        .frame_plans =
            std::move(frame_plans),
        .physical_package = std::move(physical),
        .render_graph =
            std::make_shared<const RenderGraphIR>(
                std::move(render_graph)),
    };
}

class DefaultVulkanRenderCompilerProgram final
    : public RenderCompilerProgram {
  public:
    RenderCompilerProgramSelection selection(
        const RenderCompilerBackendContext
            &backend_context) const override {
        requireVulkanRenderCompilerBackendContext(
            backend_context);
        return {
            .schema_version = 1,
            .name = "engine.default",
            .implementation =
                "builtin.default_vulkan_v1",
            .backend =
                std::string{
                    vulkanRenderCompilerBackend},
            .mode =
                RenderCompilerProgramMode::mixed,
        };
    }

    RenderCompilerProgramOutput compile(
        const RenderCompilerProgramInput
            &input) const override {
        const auto &backend =
            requireVulkanRenderCompilerBackendContext(
                input.backend_context);
        validateVulkanDevicePlanningContext(backend);
        RenderCompilerProgramOutput output;
        output.variants.reserve(
            input.variants.size());
        // RG-3: a non-preview variant of this invocation composed the
        // authoring, so a later preview failure is the preview's own.
        bool runtime_variant_composed = false;
        for (const auto &request :
             input.variants) {
            if (request.artifact ==
                    RenderCompilerProgramArtifact::data_only &&
                request.graph_variant ==
                    RenderPipelineGraphVariant::preview) {
                // PV-1: the request-local preview not composing the
                // authoring does not stop the runtime variants. It is
                // recorded as unavailable, with the refusal, and normal
                // play publishes. The registration puts runtime variants
                // first, so one that cannot compose has already thrown.
                // RG-3: only the preview's own refusals are kept here: an
                // authored surface the preview does not support, or any
                // failure after a runtime variant composed the same
                // authoring (the "composition" reason). Without a runtime
                // variant to compare with, any other failure (a pass that
                // does not own a field, say) is the authoring's and throws
                // as it would for flat.
                const auto recordUnavailable = [&] {
                    RenderCompilerProgramVariantOutput unavailable;
                    unavailable.graph_variant = request.graph_variant;
                    unavailable.unavailable = std::current_exception();
                    output.variants.push_back(std::move(unavailable));
                };
                try {
                    output.variants.push_back(
                        compileDefaultVulkanVariant(
                            input, backend, request));
                } catch (const PreviewUnsupportedAuthoringError &) {
                    recordUnavailable();
                } catch (...) {
                    if (!runtime_variant_composed) throw;
                    recordUnavailable();
                }
                continue;
            }
            output.variants.push_back(
                compileDefaultVulkanVariant(
                    input, backend, request));
            if (request.graph_variant !=
                RenderPipelineGraphVariant::preview)
                runtime_variant_composed = true;
        }
        // P-2a-4b: `input.rendering_config` is the authored document, so a
        // `pelican.graph` root keeps `frame_node_order` in its `rendering`
        // section. Reading the root here made the cross-variant unknown-graph
        // rejection vacuous for every graph-root project.
        const auto &authored_section =
            renderingSectionOrEmpty(input.rendering_config);
        // Each variant's parse held its frame_node_order to the form and its
        // finalize applied it to the variant's own graphs; every graph key
        // names a graph of some variant (require_all_graphs across the
        // program).
        if (const auto order = authored_section.find("frame_node_order");
            order != authored_section.end()) {
            if (!order->is_object())
                throw GraphRegistryError("render_graph.frame_node_order_invalid@1",
                                         {{"reason", "not_an_object"}});
            std::unordered_set<std::string> graph_names;
            for (const auto &variant : output.variants) {
                if (variant.unavailable || !variant.render_graph) continue;
                for (const auto &graph : variant.render_graph->graphs)
                    graph_names.insert(graph.name);
            }
            for (const auto &[name, nodes] : order->items()) {
                if (!graph_names.contains(name))
                    throw GraphRegistryError("render_graph.frame_node_order_unknown@1",
                                             {{"graph", name}});
            }
        }
        return output;
    }
};

void validateVulkanPhysicalPackage(
    const VulkanRenderCompilerPhysicalPackage
        &physical,
    std::span<const std::string>
        frame_graph_names) {
    if (frame_graph_names.empty()) {
        throw std::runtime_error(
            "Render compiler program produced no frame "
            "plans");
    }
    std::unordered_map<
        std::string,
        std::shared_ptr<const VulkanTargetPlan>>
        compiled_by_name;
    for (const auto &plan :
         physical.target_plan_compilation.plans) {
        if (plan == nullptr ||
            plan->graph.empty() ||
            !compiled_by_name
                 .emplace(plan->graph, plan)
                 .second) {
            throw std::runtime_error(
                "Render compiler program produced a "
                "duplicate, unnamed, or null Vulkan "
                "target plan");
        }
    }
    if (compiled_by_name.size() !=
        physical.target_plans.size()) {
        throw std::runtime_error(
            "Render compiler Vulkan package target-plan "
            "index is incomplete");
    }
    for (const auto &[name, indexed_plan] :
         physical.target_plans) {
        const auto found =
            compiled_by_name.find(name);
        if (name.empty() ||
            indexed_plan == nullptr ||
            found == compiled_by_name.end() ||
            found->second != indexed_plan) {
            throw std::runtime_error(
                "Render compiler Vulkan package has an "
                "invalid target-plan index entry");
        }
    }
    std::unordered_set<std::string>
        verification_graphs;
    for (const auto &context :
         physical.target_plan_compilation
             .verification_contexts) {
        if (context.logical_graph == nullptr) {
            throw std::runtime_error(
                "Render compiler Vulkan package complete-plan "
                "verification context has no logical graph");
        }
        if (context.automatic_plan == nullptr) {
            throw std::runtime_error(
                "Render compiler Vulkan package complete-plan "
                "verification context has no automatic plan for graph '" +
                context.logical_graph->name + "'");
        }
        if (context.compiled_plan == nullptr) {
            throw std::runtime_error(
                "Render compiler Vulkan package complete-plan "
                "verification context has no compiled plan for graph '" +
                context.logical_graph->name + "'");
        }
        const auto &graph = context.logical_graph->name;
        if (graph.empty()) {
            throw std::runtime_error(
                "Render compiler Vulkan package complete-plan "
                "verification context has an unnamed logical graph");
        }
        if (context.automatic_plan->graph != graph) {
            throw std::runtime_error(
                "Render compiler Vulkan package complete-plan "
                "verification context graph does not match its "
                "automatic plan for graph '" +
                graph + "'");
        }
        const auto compiled = compiled_by_name.find(graph);
        if (compiled == compiled_by_name.end()) {
            throw std::runtime_error(
                "Render compiler Vulkan package complete-plan "
                "verification context has no compiled target plan for "
                "graph '" +
                graph + "'");
        }
        if (!verification_graphs.insert(graph).second) {
            throw std::runtime_error(
                "Render compiler Vulkan package has duplicate "
                "complete-plan verification contexts for graph '" +
                graph + "'");
        }
        if (compiled->second != context.compiled_plan &&
            !physical.verified_complete_physical_plans.contains(graph)) {
            throw std::runtime_error(
                "Render compiler Vulkan package complete-plan "
                "verification context compiled plan is not the "
                "indexed target plan for graph '" +
                graph + "'");
        }
    }
    for (const auto &[name, verified] :
         physical.verified_complete_physical_plans) {
        const auto target =
            physical.target_plans.find(name);
        if (name.empty() ||
            verified == nullptr ||
            target ==
                physical.target_plans.end() ||
            target->second == nullptr) {
            throw std::runtime_error(
                "Render compiler Vulkan package has an invalid "
                "verified complete physical-plan entry");
        }
        validateVulkanTargetPlanMatchesCompletePhysicalPackage(
            *target->second, *verified);
    }
    if (frame_graph_names.size() !=
        physical.target_plans.size()) {
        throw std::runtime_error(
            "Render compiler program frame and Vulkan "
            "target graph sets differ");
    }
    for (const auto &name :
         frame_graph_names) {
        if (!physical.target_plans.contains(name)) {
            throw std::runtime_error(
                "Render compiler program frame and Vulkan "
                "target graph sets differ");
        }
    }
    std::unordered_set<std::string> target_names;
    for (const auto &placement :
         physical.image_placements) {
        if (placement.resource.empty() ||
            !target_names.insert(placement.resource).second) {
            throw std::runtime_error(
                "Render compiler program produced a "
                "duplicate or unnamed Vulkan render "
                "target");
        }
    }
}

} // namespace

PhysicalNodeOrder VulkanRenderCompilerPhysicalPackage::physicalNodeOrder(std::string_view graph) const {
    return physicalNodeOrderOf(target_plans.at(std::string{graph})->scopes);
}

void VulkanRenderCompilerPhysicalPackage::validate(
    std::span<const std::string>
        frame_graph_names) const {
    validateVulkanPhysicalPackage(
        *this, frame_graph_names);
}

const VulkanRenderCompilerBackendContext &
requireVulkanRenderCompilerBackendContext(
    const RenderCompilerBackendContext &context) {
    const auto *vulkan =
        dynamic_cast<
            const VulkanRenderCompilerBackendContext *>(
            &context);
    if (vulkan == nullptr ||
        context.backend() !=
            vulkanRenderCompilerBackend) {
        throw std::runtime_error(
            "Render compiler program requires a Vulkan "
            "backend context");
    }
    return *vulkan;
}

VulkanRenderCompilerPhysicalPackage &
requireVulkanRenderCompilerPhysicalPackage(
    RenderCompilerBackendPhysicalPackage
        &package) {
    auto *vulkan =
        dynamic_cast<
            VulkanRenderCompilerPhysicalPackage *>(
            &package);
    if (vulkan == nullptr ||
        package.backend() !=
            vulkanRenderCompilerBackend) {
        throw std::runtime_error(
            "Render pipeline runtime requires a Vulkan "
            "physical package");
    }
    return *vulkan;
}

const VulkanRenderCompilerPhysicalPackage &
requireVulkanRenderCompilerPhysicalPackage(
    const RenderCompilerBackendPhysicalPackage
        &package) {
    const auto *vulkan =
        dynamic_cast<
            const VulkanRenderCompilerPhysicalPackage *>(
            &package);
    if (vulkan == nullptr ||
        package.backend() !=
            vulkanRenderCompilerBackend) {
        throw std::runtime_error(
            "Render pipeline runtime requires a Vulkan "
            "physical package");
    }
    return *vulkan;
}

const RenderingTargetPlanVerificationContext &
requireVulkanTargetPlanVerificationContext(
    const VulkanRenderCompilerPhysicalPackage &physical,
    std::string_view graph) {
    const auto &contexts =
        physical.target_plan_compilation
            .verification_contexts;
    const auto found = std::find_if(
        contexts.begin(), contexts.end(),
        [graph](const auto &context) {
            return context.logical_graph != nullptr &&
                   context.logical_graph->name == graph;
        });
    if (found == contexts.end() ||
        found->automatic_plan == nullptr) {
        throw std::runtime_error(
            "Vulkan compiler package has no verification context for "
            "graph '" +
            std::string{graph} + "'");
    }
    return *found;
}

void installVerifiedVulkanCompletePhysicalPlanPackage(
    VulkanRenderCompilerPhysicalPackage &physical,
    std::string_view graph,
    VulkanCompletePhysicalPlanPackage package) {
    if (graph.empty() ||
        physical.verified_complete_physical_plans
            .contains(std::string{graph})) {
        throw std::runtime_error(
            "Vulkan compiler package complete plan is unnamed or "
            "already installed");
    }
    const auto indexed =
        physical.target_plans.find(
            std::string{graph});
    if (indexed == physical.target_plans.end() ||
        indexed->second == nullptr) {
        throw std::runtime_error(
            "Vulkan compiler package has no target plan for graph '" +
            std::string{graph} + "'");
    }
    const auto &context =
        requireVulkanTargetPlanVerificationContext(
            physical, graph);
    if (indexed->second !=
        context.automatic_plan) {
        throw std::runtime_error(
            "Vulkan compiler target plan was modified before complete "
            "plan verification");
    }
    const auto plan = std::find_if(
        physical.target_plan_compilation.plans.begin(),
        physical.target_plan_compilation.plans.end(),
        [&](const auto &candidate) {
            return candidate ==
                   context.automatic_plan;
        });
    if (plan ==
        physical.target_plan_compilation.plans.end()) {
        throw std::runtime_error(
            "Vulkan compiler automatic target plan is absent from its "
            "compilation");
    }

    for (const auto &scope : package.scopes)
        if (scope.nodes.size() > 1)
            for (const auto &node : scope.nodes)
                if (std::find(context.scope_owning_nodes.begin(), context.scope_owning_nodes.end(), node) != context.scope_owning_nodes.end())
                    throw GraphRegistryError("render_graph.scope_implementation_unfusible@1", {{"graph", graph}, {"node", node}});
    auto verified =
        verifyVulkanCompletePhysicalPlanPackage(
            *context.logical_graph,
            context.topology,
            *context.automatic_plan,
            context.node_order,
            std::move(package),
            context.format_capabilities,
            context.enabled_device_extensions);
    auto applied =
        std::make_shared<const VulkanTargetPlan>(
            applyVerifiedVulkanCompletePhysicalPlanPackage(
                *context.automatic_plan,
                verified));
    auto retained_verified =
        std::make_shared<
            const VerifiedVulkanCompletePhysicalPlanPackage>(
            std::move(verified));

    const auto [verification, inserted] =
        physical.verified_complete_physical_plans
            .emplace(
                std::string{graph},
                std::move(retained_verified));
    (void)verification;
    if (!inserted) {
        throw std::logic_error(
            "Vulkan compiler complete plan insertion raced with "
            "validation");
    }
    *plan = applied;
    indexed->second = std::move(applied);
}

const RenderCompilerProgram &
defaultVulkanRenderCompilerProgram() {
    static const DefaultVulkanRenderCompilerProgram
        program;
    return program;
}

} // namespace Pelican
