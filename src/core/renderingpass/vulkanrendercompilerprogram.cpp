#include "vulkanrendercompilerpackage.hpp"

#include "computetask.hpp"
#include "graphtransformregistry.hpp"
#include "materialpassinfojsonparser.hpp"
#include "renderstrategyregistry.hpp"
#include "rendertargetjsonparser.hpp"
#include "subgraphreplacementregistry.hpp"
#include "../loader/pathresolver.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace Pelican {
namespace {

std::optional<VulkanViewExecutionPlanRequest>
targetViewExecutionRequest(
    const CompiledRenderPipeline &pipeline,
    const nlohmann::json &config,
    std::span<const FrameGraphDefinition> graphs,
    std::span<const ComputeTaskDefinition> compute_tasks,
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
    std::set<std::string, std::less<>>
        capable_candidates;
    std::set<std::string, std::less<>>
        independent_candidates;
    const auto builtin_fragment =
        [](std::string_view reference) {
            static constexpr std::array known{
                std::string_view{"engine://fullscreen"},
                std::string_view{"engine://ssao"},
                std::string_view{"engine://ssao_blur"},
                std::string_view{"engine://scene_present"},
                std::string_view{"engine://output_transform"},
            };
            return std::find(
                       known.begin(), known.end(),
                       reference) != known.end();
        };
    if (config.contains("rendering_passes") &&
        config.at("rendering_passes").is_array()) {
        for (const auto &rendering_pass :
             config.at("rendering_passes")) {
            if (!rendering_pass.is_object() ||
                !rendering_pass.contains("passes") ||
                !rendering_pass.at("passes").is_array()) {
                continue;
            }
            for (const auto &pass :
                 rendering_pass.at("passes")) {
                if (!pass.is_object() ||
                    !pass.contains("name") ||
                    !pass.at("name").is_string() ||
                    !pass.contains("type") ||
                    !pass.at("type").is_string()) {
                    continue;
                }
                const auto name =
                    pass.at("name").get<std::string>();
                const auto type =
                    pass.at("type").get<std::string>();
                if (type == "shadow_depth") {
                    independent_candidates.insert(name);
                    continue;
                }
                if (type != "fullscreen" ||
                    pass.contains("implementation") ||
                    !pass.contains("shader") ||
                    !pass.at("shader").is_object()) {
                    continue;
                }
                const auto &shader = pass.at("shader");
                if (!shader.contains("vertex") ||
                    !shader.at("vertex").is_string() ||
                    !shader.contains("fragment") ||
                    !shader.at("fragment").is_string()) {
                    continue;
                }
                const auto vertex =
                    shader.at("vertex").get<std::string>();
                const auto fragment =
                    shader.at("fragment").get<std::string>();
                if (enable_multiview_runtime &&
                    vertex == "engine://fullscreen" &&
                    builtin_fragment(fragment)) {
                    capable_candidates.insert(name);
                }
            }
        }
    }
    for (const auto &task : compute_tasks) {
        if (task.schedule ==
            ComputeTaskSchedule::per_frame) {
            independent_candidates.insert(task.name);
        }
    }
    const bool sprite_enabled =
        std::find(
            pipeline.feature_names.begin(),
            pipeline.feature_names.end(),
            "sprite") != pipeline.feature_names.end();
    for (const auto &graph : graphs) {
        for (const auto &node : graph.nodes) {
            if (node.kind == FramePlanNodeKind::anchor &&
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
            return !graphs.empty() &&
                   std::all_of(
                       graphs.begin(), graphs.end(),
                       [&](const FrameGraphDefinition &graph) {
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

std::unordered_map<std::string, FramePlan>
framePlansByName(
    const std::vector<FrameGraphDefinition>
        &definitions) {
    std::unordered_map<std::string, FramePlan> by_name;
    for (const auto &definition : definitions) {
        auto plan = planFrameGraph(definition);
        const auto name = plan.name;
        if (!by_name.emplace(name, std::move(plan)).second) {
            throw std::runtime_error(
                "Duplicate frame graph definition: " +
                name);
        }
    }
    return by_name;
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

void namespaceComputeTasks(
    std::vector<ComputeTaskDefinition> &tasks,
    std::vector<FrameGraphDefinition> &graphs,
    std::string_view suffix) {
    if (suffix.empty() || tasks.empty()) return;
    std::unordered_map<std::string, std::string>
        renamed;
    for (auto &task : tasks) {
        auto next = task.name + std::string{suffix};
        renamed.emplace(task.name, next);
        task.name = std::move(next);
    }
    const auto rename =
        [&renamed](std::string &name) {
            if (const auto found = renamed.find(name);
                found != renamed.end()) {
                name = found->second;
            }
        };
    for (auto &graph : graphs) {
        for (auto &node : graph.nodes) {
            if (node.kind ==
                FramePlanNodeKind::compute) {
                rename(node.name);
            }
            for (auto &after : node.after) {
                rename(after);
            }
            for (auto &before : node.before) {
                rename(before);
            }
        }
    }
}

RenderCompilerProgramVariantOutput
compileDefaultVulkanVariant(
    const RenderCompilerProgramInput &input,
    const VulkanRenderCompilerBackendContext
        &backend,
    const RenderCompilerProgramVariantRequest
        &request) {
    std::optional<RenderStrategySelection>
        resolved_render_strategy;
    auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{
            input.rendering_config,
            input.source_name},
        RenderEnvironmentCapabilities{
            input.runtime_shader_compiler_enabled,
            request.graph_variant,
        },
        RenderPipelineResolveDependencies{
            .load_feature_json =
                [&input](std::string_view ref) {
                    return input.path_resolver.loadText(
                        ref);
                },
            .load_pipeline_json =
                [&input](std::string_view ref) {
                    return input.path_resolver.loadText(
                        ref);
                },
            .normalize_config =
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
        });
    auto compiled_pipeline_value =
        compileRenderPipeline(resolved);
    auto composed_config =
        std::move(resolved.normalized_config);
    if (request.compose_runtime_config) {
        request.compose_runtime_config(
            composed_config);
    }
    auto resolved_transforms =
        resolveLogicalGraphTransforms(
            composed_config,
            input.graph_transforms);
    composed_config =
        std::move(resolved_transforms.config);
    validateGraphVariantConfig(
        resolved.graph_variant_policy,
        composed_config);
    auto resolved_subgraphs =
        resolveTaggedSubgraphReplacements(
            composed_config,
            input.subgraph_replacements);
    composed_config =
        std::move(resolved_subgraphs.config);

    auto render_target_definitions =
        parseRenderTargetDefinitionsFromJson(
            composed_config);
    auto buffer_definitions =
        parseFrameGraphBufferDefinitionsFromJson(
            composed_config);
    auto buffer_names =
        frameGraphBufferNameSet(buffer_definitions);
    auto compute_task_definitions =
        parseComputeTaskDefinitionsFromConfigJson(
            composed_config);
    validateComputeTaskBufferContracts(
        buffer_definitions,
        compute_task_definitions);
    validateGpuDrawSourceBufferContracts(
        composed_config,
        buffer_definitions);
    auto graph_definitions =
        parseFrameGraphDefinitionsFromConfigJson(
            composed_config);
    applyResolvedLogicalGraphTransformSelections(
        graph_definitions,
        resolved_transforms.selections);
    applyResolvedRenderStrategySelection(
        graph_definitions,
        resolved_render_strategy);
    applyResolvedTaggedSubgraphSelections(
        graph_definitions,
        resolved_subgraphs.graphs);
    compiled_pipeline_value.graph_transforms =
        resolved_transforms.selections;
    compiled_pipeline_value.render_strategy =
        resolved_render_strategy;
    auto compiled_pipeline =
        std::make_shared<const CompiledRenderPipeline>(
            std::move(compiled_pipeline_value));
    namespaceComputeTasks(
        compute_task_definitions,
        graph_definitions,
        compiled_pipeline->graph_variant_policy
            .rendering_pass_name_suffix);

    auto physical =
        std::make_unique<
            VulkanRenderCompilerPhysicalPackage>();
    physical->target_plan_compilation =
        compileRenderingTargetPlansForVulkanDevice(
            graph_definitions,
            render_target_definitions,
            compiled_pipeline->sample_count_policy,
            backend.output_format,
            backend.physical_device,
            targetViewExecutionRequest(
                *compiled_pipeline,
                composed_config,
                graph_definitions,
                compute_task_definitions,
                request.enable_multiview_runtime),
            request.enable_external_depth_export
                ? std::optional{
                      VulkanExternalDepthExportRequest{}}
                : std::nullopt,
            compiled_pipeline->target_planning,
            compiled_pipeline->vulkan_plan_pins,
            compiled_pipeline
                ->vulkan_physical_fragments);
    applyRenderingTargetPlan(
        render_target_definitions,
        physical->target_plan_compilation);
    physical->target_plans =
        targetPlansByName(
            physical->target_plan_compilation);
    physical->render_target_definitions =
        std::move(render_target_definitions);

    return RenderCompilerProgramVariantOutput{
        .graph_variant = request.graph_variant,
        .compiled_pipeline =
            std::move(compiled_pipeline),
        .normalized_config =
            std::move(composed_config),
        .buffer_definitions =
            std::move(buffer_definitions),
        .buffer_names =
            std::move(buffer_names),
        .compute_task_definitions =
            std::move(compute_task_definitions),
        .frame_plans =
            framePlansByName(graph_definitions),
        .physical_package = std::move(physical),
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
        RenderCompilerProgramOutput output;
        output.variants.reserve(
            input.variants.size());
        for (const auto &request :
             input.variants) {
            output.variants.push_back(
                compileDefaultVulkanVariant(
                    input, backend, request));
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
    for (const auto &definition :
         physical.render_target_definitions) {
        if (definition.name.empty() ||
            !target_names.insert(definition.name).second) {
            throw std::runtime_error(
                "Render compiler program produced a "
                "duplicate or unnamed Vulkan render "
                "target");
        }
    }
}

} // namespace

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

const RenderCompilerProgram &
defaultVulkanRenderCompilerProgram() {
    static const DefaultVulkanRenderCompilerProgram
        program;
    return program;
}

} // namespace Pelican
