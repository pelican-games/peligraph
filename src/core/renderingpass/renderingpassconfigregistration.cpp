#include "renderingpassconfigregistration.hpp"
#include "computetask.hpp"
#include "../../project/renderpipeline.hpp"
#include "framegraphruntime.hpp"
#include "frameplanner.hpp"
#include "passimplementationregistry.hpp"
#include "renderpipelinegpuarena.hpp"
#include "renderingpassconfigjsonparser.hpp"
#include "renderingpassconfigloader.hpp"
#include "renderingpasscontainer.hpp"
#include "renderingpassruntimecompiler.hpp"
#include "renderingsamplecount.hpp"
#include "rendertargetconfigregistration.hpp"
#include "rendertargetcontainer.hpp"
#include "rendertargetimageviewresolver.hpp"
#include "rendertargetjsonparser.hpp"
#include "rendertargetmetadataresolver.hpp"
#include "rendertargetnameresolver.hpp"
#include "../loader/pathresolver.hpp"
#include "../renderer/shadowdepthpasscontainer.hpp"
#include "../renderer/velocitypasscontainer.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/rendertarget.hpp"
#if PELICAN_WITH_IMGUI
#include "../imgui/imguiruntime.hpp"
#include "../launchconfig.hpp"
#endif
#include <algorithm>
#include <iterator>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Pelican {

namespace {

RenderingPassRuntimeDependencies toRuntimeDependencies(
    RenderingPassConfigRuntimeDependencies &dependencies,
    const RenderTargetMetadataResolver &rt_metadata,
    const RenderTargetImageViewResolver &rt_views,
    FrameGraphResourceContainer &frame_graph_resources,
    RenderPipelineGpuRegistrationArena &gpu_arena) {
    auto debug_draw_provider =
        dependencies.debug_draw_provider;
    if (debug_draw_provider) {
        debug_draw_provider =
            [provider = std::move(debug_draw_provider),
             &gpu_arena]() -> DebugDraw & {
            auto &debug_draw = provider();
            gpu_arena.enlist(debug_draw);
            return debug_draw;
        };
    }
    auto debug_text_provider =
        dependencies.debug_text_provider;
    if (debug_text_provider) {
        debug_text_provider =
            [provider = std::move(debug_text_provider),
             &gpu_arena]() -> DebugText & {
            auto &debug_text = provider();
            gpu_arena.enlist(debug_text);
            return debug_text;
        };
    }
    return RenderingPassRuntimeDependencies{
        &dependencies.render_target,
        &rt_metadata,
        &rt_views,
        &dependencies.shader_library,
        &dependencies.fullscreen_pass_container,
        &dependencies.shadow_depth_pass_container,
        &dependencies.velocity_pass_container,
        &frame_graph_resources,
        &dependencies.path_resolver,
        dependencies.shader_defines,
        dependencies.warn_backend_specific_shader_refs,
        std::move(debug_draw_provider),
        std::move(debug_text_provider),
    };
}

void injectGpuRegistrationFault(
    const RenderingPassConfigRegistrationDependencies::Options
        &options,
    RenderPipelineGpuRegistrationFaultPoint point) {
    if (options.fault_point != point) return;
    throw std::runtime_error(
        "Injected render pipeline GPU registration failure");
}

std::string gpuOwnerScope(
    const RenderingPassConfigRegistrationDependencies::Options
        &options) {
    if (!options.gpu_owner_scope.empty()) {
        return options.gpu_owner_scope;
    }
    return "render_pipeline/" +
           std::string{renderPipelineGraphVariantName(
               options.graph_variant)};
}

std::unordered_map<std::string, FramePlan> framePlansByName(
    const std::vector<FrameGraphDefinition> &definitions) {
    std::unordered_map<std::string, FramePlan> by_name;
    for (const auto &definition : definitions) {
        auto plan = planFrameGraph(definition);
        const auto name = plan.name;
        if (!by_name.emplace(name, std::move(plan)).second) {
            throw std::runtime_error("Duplicate frame graph definition: " +
                                     name);
        }
    }
    return by_name;
}

std::unordered_map<std::string,
                   std::shared_ptr<const VulkanTargetPlan>>
targetPlansByName(
    const RenderingTargetPlanCompilation &compilation) {
    std::unordered_map<std::string,
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

std::vector<CompiledComputeTask> compileComputeTasks(
    const std::vector<ComputeTaskDefinition> &definitions,
    RenderingPassConfigRegistrationDependencies &dependencies) {
    std::vector<CompiledComputeTask> compiled;
    compiled.reserve(definitions.size());
    for (const auto &definition : definitions) {
        const auto task_id = dependencies.compute_task_container.registerComputeTask(
            definition,
            ComputeTaskRuntimeDependencies{
                dependencies.runtime.shader_library,
                dependencies.runtime.path_resolver,
                dependencies.render_targets.render_target_container,
                dependencies.frame_graph_resources,
            });
        compiled.push_back(CompiledComputeTask{definition, task_id});
    }
    return compiled;
}

void namespaceComputeTasks(std::vector<ComputeTaskDefinition> &tasks,
                           std::vector<FrameGraphDefinition> &graphs,
                           std::string_view suffix) {
    if (suffix.empty() || tasks.empty()) return;
    std::unordered_map<std::string, std::string> renamed;
    for (auto &task : tasks) {
        auto next = task.name + std::string{suffix};
        renamed.emplace(task.name, next);
        task.name = std::move(next);
    }
    const auto rename = [&renamed](std::string &name) {
        if (const auto found = renamed.find(name); found != renamed.end()) {
            name = found->second;
        }
    };
    for (auto &graph : graphs) {
        for (auto &node : graph.nodes) {
            if (node.kind == FramePlanNodeKind::compute) rename(node.name);
            for (auto &after : node.after) rename(after);
            for (auto &before : node.before) rename(before);
        }
    }
}

struct PreparedRenderingPassConfigVariant {
    std::shared_ptr<const CompiledRenderPipeline> compiled_pipeline;
    nlohmann::json normalized_config;
    std::vector<RenderTargetDefinition> render_target_definitions;
    std::vector<FrameGraphBufferDefinition> buffer_definitions;
    std::unordered_set<std::string> buffer_names;
    std::vector<ComputeTaskDefinition> compute_task_definitions;
    std::unordered_map<std::string, FramePlan> frame_plans;
    RenderingTargetPlanCompilation target_plan_compilation;
    std::unordered_map<std::string,
                       std::shared_ptr<const VulkanTargetPlan>>
        target_plans;
};

PreparedRenderingPassConfigVariant prepareRenderingPassConfigVariant(
    const nlohmann::json &rendering_pass_data,
    RenderingPassConfigRegistrationDependencies &dependencies) {
    const auto swapchain_format =
        dependencies.runtime.render_target.getSwapchainFormat();
    const auto target_extent = dependencies.runtime.render_target.getExtent();
    auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{rendering_pass_data,
                              "rendering pass registration"},
        RenderEnvironmentCapabilities{
#if PELICAN_RUNTIME_SHADER_COMPILER
            true,
#else
            false,
#endif
            dependencies.options.graph_variant,
        },
        RenderPipelineResolveDependencies{
            .load_feature_json = [&dependencies](std::string_view ref) {
                return dependencies.runtime.path_resolver.loadText(ref);
            },
            .load_pipeline_json = [&dependencies](std::string_view ref) {
                return dependencies.runtime.path_resolver.loadText(ref);
            },
            .normalize_config = [swapchain_format, target_extent](
                const nlohmann::json &config,
                const std::vector<std::string> &feature_names) {
                const bool hdr_enabled =
                    std::find(feature_names.begin(), feature_names.end(),
                              "hdr") != feature_names.end();
                return resolveRenderTargetFormatClassesV2(
                    config, swapchain_format, target_extent, hdr_enabled);
            },
        });
    auto compiled_pipeline =
        std::make_shared<const CompiledRenderPipeline>(
            compileRenderPipeline(resolved));
    dependencies.runtime.shader_defines = compiled_pipeline->shader_defines;
    auto composed_rendering_pass_data = std::move(resolved.normalized_config);
#if PELICAN_WITH_IMGUI
    if (resolved.graph_variant_policy
            .rendering_pass_name_suffix.empty()) {
        invokeImGuiRuntimeCallback(GET_MODULE(EngineLaunchConfig), [&] {
            appendImGuiPassToCanonicalGraphs(composed_rendering_pass_data);
        });
    }
#endif
    auto render_target_definitions =
        parseRenderTargetDefinitionsFromJson(composed_rendering_pass_data);
    auto buffer_definitions =
        parseFrameGraphBufferDefinitionsFromJson(composed_rendering_pass_data);
    auto buffer_names = frameGraphBufferNameSet(buffer_definitions);
    auto compute_task_definitions =
        parseComputeTaskDefinitionsFromConfigJson(composed_rendering_pass_data);
    auto graph_definition_list =
        parseFrameGraphDefinitionsFromConfigJson(composed_rendering_pass_data);
    namespaceComputeTasks(compute_task_definitions, graph_definition_list,
                          compiled_pipeline->graph_variant_policy
                              .rendering_pass_name_suffix);
    auto target_plan_compilation =
        compileRenderingTargetPlansForVulkanDevice(
            graph_definition_list, render_target_definitions,
            compiled_pipeline->sample_count_policy,
            swapchain_format,
            GET_MODULE(VulkanManageCore).getPhysDevice());
    applyRenderingTargetPlan(render_target_definitions,
                             target_plan_compilation);
    auto target_plans =
        targetPlansByName(target_plan_compilation);
    auto frame_plans = framePlansByName(graph_definition_list);
    return {
        std::move(compiled_pipeline),
        std::move(composed_rendering_pass_data),
        std::move(render_target_definitions),
        std::move(buffer_definitions),
        std::move(buffer_names),
        std::move(compute_task_definitions),
        std::move(frame_plans),
        std::move(target_plan_compilation),
        std::move(target_plans),
    };
}

std::vector<RenderPipelineProgramPreparation>
registerPreparedRenderingPassConfigVariant(
    PreparedRenderingPassConfigVariant &prepared,
    vk::Extent2D base_extent,
    RenderingPassConfigRegistrationDependencies &dependencies,
    RenderPipelineGpuRegistrationArena &gpu_arena,
    const PassImplementationRegistrySnapshot
        &pass_implementation_providers,
    RenderingPassConfigRegistrationResult &result) {
    if (dependencies.options
            .prepare_additional_gpu_resources) {
        dependencies.options
            .prepare_additional_gpu_resources();
    }
    registerRenderTargetDefinitions(
        prepared.render_target_definitions, base_extent,
        dependencies.render_targets.render_target_container);
    injectGpuRegistrationFault(
        dependencies.options,
        RenderPipelineGpuRegistrationFaultPoint::
            after_render_targets);
    dependencies.frame_graph_resources.registerBuffers(
        prepared.buffer_definitions);
    injectGpuRegistrationFault(
        dependencies.options,
        RenderPipelineGpuRegistrationFaultPoint::
            after_frame_graph_buffers);

    const RenderTargetNameResolver rt_resolver{
        dependencies.render_targets.render_target_container};
    const RenderTargetMetadataResolver rt_metadata{
        dependencies.render_targets.render_target_container};
    const RenderTargetImageViewResolver rt_views{
        dependencies.render_targets.render_target_container};
    auto pass_definitions =
        parseRenderingPassDefinitionsFromConfigJson(
            // The normalized JSON is no longer needed after the CPU phase,
            // but pass parsing depends on the concrete target metadata
            // registered above. Reconstruct it from the compiled definitions
            // is intentionally avoided; retain it in the prepared variant.
            prepared.normalized_config,
            rt_resolver, rt_metadata, prepared.buffer_names);
    for (auto &definition : pass_definitions) {
        const auto target_plan =
            prepared.target_plans.find(definition.name);
        if (target_plan ==
                prepared.target_plans.end() ||
            target_plan->second == nullptr) {
            throw std::runtime_error(
                "Physical target plan not found while resolving pass "
                "implementations: " +
                definition.name);
        }
        resolveRenderingPassImplementations(
            definition, *target_plan->second,
            pass_implementation_providers);
    }
    auto compiled_compute_tasks =
        compileComputeTasks(prepared.compute_task_definitions,
                            dependencies);
    injectGpuRegistrationFault(
        dependencies.options,
        RenderPipelineGpuRegistrationFaultPoint::
            after_compute_tasks);
    auto compiled_passes =
        compileRenderingPassesRuntime(
            pass_definitions,
            toRuntimeDependencies(
                dependencies.runtime, rt_metadata, rt_views,
                dependencies.frame_graph_resources, gpu_arena));
    injectGpuRegistrationFault(
        dependencies.options,
        RenderPipelineGpuRegistrationFaultPoint::
            after_rendering_passes);

    result.feature_names =
        prepared.compiled_pipeline->feature_names;
    result.excluded_feature_names =
        prepared.compiled_pipeline->excluded_feature_names;
    result.target_plans =
        prepared.target_plan_compilation.plans;
    if (!result.target_plans.empty() &&
        result.target_plans.front()->sample_count_plan) {
        result.sample_count_plan =
            std::shared_ptr<const ResolvedSampleCountPlan>(
                result.target_plans.front(),
                &*result.target_plans.front()
                      ->sample_count_plan);
    }
    std::vector<RenderPipelineProgramPreparation>
        program_preparations;
    program_preparations.reserve(compiled_passes.size());
    const auto render_target_bindings =
        dependencies.render_targets.render_target_container
            .currentNameBindings();
    const auto buffer_bindings =
        dependencies.frame_graph_resources
            .currentNameBindings();
    const auto owner_scope =
        gpuOwnerScope(dependencies.options);
    for (auto &compiled_pass : compiled_passes) {
        compiled_pass.compute_tasks =
            compiled_compute_tasks;
        const auto pass_name = compiled_pass.name;
        auto found_plan =
            prepared.frame_plans.find(pass_name);
        if (found_plan == prepared.frame_plans.end()) {
            throw std::runtime_error(
                "Frame graph definition not found for rendering pass: " +
                pass_name);
        }
        const auto found_target_plan =
            prepared.target_plans.find(pass_name);
        if (found_target_plan ==
            prepared.target_plans.end()) {
            throw std::runtime_error(
                "Physical target plan not found for rendering pass: " +
                pass_name);
        }
        program_preparations.push_back(
            RenderPipelineProgramPreparation{
                .owner_scope = owner_scope,
                .rendering_pass =
                    std::move(compiled_pass),
                .frame_plan =
                    std::move(found_plan->second),
                .render_pipeline =
                    prepared.compiled_pipeline,
                .target_plan =
                    found_target_plan->second,
                .render_target_bindings =
                    render_target_bindings,
                .buffer_bindings = buffer_bindings,
            });
    }
    return program_preparations;
}

void requireSameRegistrationDomain(
    const RenderingPassConfigRegistrationDependencies &first,
    const RenderingPassConfigRegistrationDependencies &candidate) {
    const bool same =
        &first.render_targets.render_target_container ==
            &candidate.render_targets.render_target_container &&
        &first.runtime.render_target ==
            &candidate.runtime.render_target &&
        &first.runtime.shader_library ==
            &candidate.runtime.shader_library &&
        &first.runtime.fullscreen_pass_container ==
            &candidate.runtime.fullscreen_pass_container &&
        &first.runtime.pipeline_factory ==
            &candidate.runtime.pipeline_factory &&
        &first.runtime.shadow_depth_pass_container ==
            &candidate.runtime.shadow_depth_pass_container &&
        &first.runtime.velocity_pass_container ==
            &candidate.runtime.velocity_pass_container &&
        &first.runtime.path_resolver ==
            &candidate.runtime.path_resolver &&
        &first.frame_graph_resources ==
            &candidate.frame_graph_resources &&
        &first.compute_task_container ==
            &candidate.compute_task_container &&
        &first.frame_graph_runtime ==
            &candidate.frame_graph_runtime &&
        &first.pass_container ==
            &candidate.pass_container;
    if (!same) {
        throw std::runtime_error(
            "Render pipeline variant transaction spans different registration domains");
    }
}

std::mutex &gpuRegistrationMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<RenderingPassConfigRegistrationResult>
registerRenderingPassConfigVariantsData(
    const nlohmann::json &rendering_pass_data,
    vk::Extent2D base_extent,
    std::vector<RenderingPassConfigRegistrationDependencies>
        dependencies) {
    if (dependencies.empty()) {
        throw std::runtime_error(
            "Render pipeline variant transaction is empty");
    }
    const auto owner_scope =
        gpuOwnerScope(dependencies.front().options);
    std::size_t enabled_feature_publishers = 0;
    for (const auto &variant : dependencies) {
        requireSameRegistrationDomain(
            dependencies.front(), variant);
        if (gpuOwnerScope(variant.options) !=
            owner_scope) {
            throw std::runtime_error(
                "Render pipeline variant transaction requires one GPU owner scope");
        }
        if (variant.options.publish_enabled_features) {
            ++enabled_feature_publishers;
        }
    }
    if (enabled_feature_publishers > 1) {
        throw std::runtime_error(
            "Render pipeline variant transaction has multiple feature publishers");
    }

    // Keep one immutable provider generation across every variant and through
    // the complete prepare-to-publication transaction.
    auto pass_implementation_providers =
        passImplementationRegistry().snapshot();

    // Every pure CPU candidate is completed before any mutable GPU registry
    // checkpoint is opened.
    std::vector<PreparedRenderingPassConfigVariant>
        prepared_variants;
    prepared_variants.reserve(dependencies.size());
    for (auto &variant : dependencies) {
        prepared_variants.push_back(
            prepareRenderingPassConfigVariant(
                rendering_pass_data, variant));
    }

    // Legacy registries are mutable containers rather than isolated
    // candidates. Serialize the checkpoint-to-publication interval so a
    // stale transaction cannot roll back another transaction's records.
    const std::scoped_lock gpu_registration_lock{
        gpuRegistrationMutex()};
    auto &first = dependencies.front();
    const auto current_generation =
        first.frame_graph_runtime.snapshot();
    const auto *replaced_scope =
        current_generation != nullptr &&
                current_generation->gpu_arena != nullptr
            ? current_generation->gpu_arena->findScope(
                  owner_scope)
            : nullptr;
    RenderPipelineGpuRegistrationArena gpu_arena{
        RenderPipelineGpuRegistrationDependencies{
            first.render_targets.render_target_container,
            first.frame_graph_resources,
            first.compute_task_container,
            first.runtime.fullscreen_pass_container,
            first.runtime.shader_library,
            first.runtime.pipeline_factory,
            first.runtime.shadow_depth_pass_container,
            first.runtime.velocity_pass_container,
        },
        replaced_scope};

    std::vector<RenderingPassConfigRegistrationResult>
        results(dependencies.size());
    std::vector<std::size_t> program_offsets;
    program_offsets.reserve(dependencies.size() + 1);
    program_offsets.push_back(0);
    std::vector<RenderPipelineProgramPreparation>
        program_preparations;
    std::optional<std::vector<std::string>>
        enabled_feature_names;
    for (std::size_t index = 0;
         index < dependencies.size(); ++index) {
        auto variant_programs =
            registerPreparedRenderingPassConfigVariant(
                prepared_variants[index], base_extent,
                dependencies[index], gpu_arena,
                pass_implementation_providers,
                results[index]);
        program_preparations.insert(
            program_preparations.end(),
            std::make_move_iterator(
                variant_programs.begin()),
            std::make_move_iterator(
                variant_programs.end()));
        program_offsets.push_back(
            program_preparations.size());
        if (dependencies[index].options
                .publish_enabled_features) {
            enabled_feature_names =
                results[index].feature_names;
        }
    }
    auto prepared_generation =
        first.frame_graph_runtime.prepareGeneration(
            std::move(program_preparations),
            std::move(enabled_feature_names),
            gpu_arena.preparedScope(owner_scope));
    for (const auto &variant : dependencies) {
        injectGpuRegistrationFault(
            variant.options,
            RenderPipelineGpuRegistrationFaultPoint::
                after_runtime_prepare);
        if (variant.options
                .validate_prepared_generation) {
            variant.options
                .validate_prepared_generation(
                    prepared_generation.candidate());
        }
    }
    const auto &ids =
        prepared_generation.renderingPassIds();
    for (std::size_t index = 0;
         index < results.size(); ++index) {
        results[index].rendering_pass_ids.assign(
            ids.begin() +
                static_cast<std::ptrdiff_t>(
                    program_offsets[index]),
            ids.begin() +
                static_cast<std::ptrdiff_t>(
                    program_offsets[index + 1]));
        results[index].runtime_generation =
            prepared_generation.generation();
    }
    first.pass_container.bindRuntimePublication(
        first.frame_graph_runtime.publicationState());
    first.frame_graph_runtime.publishPreparedGeneration(
        std::move(prepared_generation));
    gpu_arena.commit();
    return results;
}

} // namespace

RenderingPassConfigRegistrationResult registerRenderingPassConfigFromJson(
    const std::string &json_path, vk::Extent2D base_extent,
    RenderingPassConfigRegistrationDependencies dependencies) {
    std::vector<RenderingPassConfigRegistrationDependencies>
        variants;
    variants.push_back(std::move(dependencies));
    auto results = registerRenderingPassConfigVariantsData(
        loadRenderingPassConfigJson(json_path), base_extent,
        std::move(variants));
    return std::move(results.front());
}

RenderingPassConfigRegistrationResult registerRenderingPassConfigFromJsonData(
    std::string_view json_data, vk::Extent2D base_extent,
    RenderingPassConfigRegistrationDependencies dependencies) {
    std::vector<RenderingPassConfigRegistrationDependencies>
        variants;
    variants.push_back(std::move(dependencies));
    auto results = registerRenderingPassConfigVariantsData(
        loadRenderingPassConfigJsonFromString(json_data, "ProjectBasicConfig"),
        base_extent, std::move(variants));
    return std::move(results.front());
}

std::vector<RenderingPassConfigRegistrationResult>
registerRenderingPassConfigVariantsFromJsonData(
    std::string_view json_data, vk::Extent2D base_extent,
    std::vector<RenderingPassConfigRegistrationDependencies>
        dependencies) {
    return registerRenderingPassConfigVariantsData(
        loadRenderingPassConfigJsonFromString(
            json_data, "ProjectBasicConfig"),
        base_extent, std::move(dependencies));
}

} // namespace Pelican
