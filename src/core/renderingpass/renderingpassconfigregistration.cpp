#include "renderingpassconfigregistration.hpp"
#include "computetask.hpp"
#include "../../project/renderpipeline.hpp"
#include "framegraphruntime.hpp"
#include "frameplanner.hpp"
#include "graphtransformregistry.hpp"
#include "materialpassinfojsonparser.hpp"
#include "passimplementationregistry.hpp"
#include "rendercompilerprogram.hpp"
#include "renderpipelinegpuarena.hpp"
#include "renderstrategyregistry.hpp"
#include "renderingpassconfigjsonparser.hpp"
#include "renderingpassconfigloader.hpp"
#include "renderingpasscontainer.hpp"
#include "renderingpassruntimecompiler.hpp"
#include "renderingsamplecount.hpp"
#include "subgraphreplacementregistry.hpp"
#include "vulkanrendercompilerpackage.hpp"
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
#include <optional>
#include <span>
#include <stdexcept>
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
        nullptr,
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

std::vector<CompiledComputeTask> compileComputeTasks(
    const std::vector<ComputeTaskDefinition> &definitions,
    const std::unordered_map<
        std::string,
        std::shared_ptr<const VulkanTargetPlan>>
        &target_plans,
    RenderingPassConfigRegistrationDependencies &dependencies) {
    std::vector<CompiledComputeTask> compiled;
    compiled.reserve(definitions.size());
    for (const auto &definition : definitions) {
        std::unordered_map<
            std::string, VulkanResourceViewLayout>
            resource_views;
        std::unordered_map<
            std::string, std::uint32_t>
            resource_view_counts;
        bool task_has_physical_plan = false;
        for (const auto &[graph, plan] :
             target_plans) {
            (void)graph;
            if (plan == nullptr) continue;
            const auto contains_task =
                std::find_if(
                    plan->scopes.begin(),
                    plan->scopes.end(),
                    [&](const VulkanPhysicalScopePlan
                            &scope) {
                        return std::find(
                                   scope.nodes.begin(),
                                   scope.nodes.end(),
                                   definition.name) !=
                               scope.nodes.end();
                    }) != plan->scopes.end();
            if (!contains_task) continue;
            task_has_physical_plan = true;
            for (const auto &port :
                 definition.resource_ports) {
                if (port.kind ==
                    ShaderResourcePortKind::buffer) {
                    continue;
                }
                constexpr std::string_view
                    history_suffix = "@history";
                auto resource = port.resource;
                if (resource.ends_with(
                        history_suffix)) {
                    resource.resize(
                        resource.size() -
                        history_suffix.size());
                }
                const auto physical =
                    std::find_if(
                        plan->resources.begin(),
                        plan->resources.end(),
                        [&](const VulkanPhysicalResourcePlan
                                &candidate) {
                            return candidate
                                       .logical_resource ==
                                   resource;
                        });
                if (physical ==
                    plan->resources.end()) {
                    throw std::runtime_error(
                        "Compute resource port '" +
                        port.name + "' (resource '" +
                        port.resource +
                        "') is absent from physical target plan '" +
                        plan->graph + "'");
                }
                const auto [found, inserted] =
                    resource_views.emplace(
                        resource,
                        physical->view_layout);
                if (!inserted &&
                    found->second !=
                        physical->view_layout) {
                    throw std::runtime_error(
                        "Compute resource port '" +
                        port.name + "' (resource '" +
                        port.resource +
                        "') has incompatible physical views across "
                        "target plans");
                }
                const auto view_count =
                    physical->view_layout ==
                            VulkanResourceViewLayout::
                                shared_2d
                        ? 1u
                        : plan->view_execution_plan
                              .view_count;
                if (view_count == 0) {
                    throw std::runtime_error(
                        "Compute resource port '" +
                        port.name + "' (resource '" +
                        port.resource +
                        "') has a zero physical view count");
                }
                const auto [count, count_inserted] =
                    resource_view_counts.emplace(
                        resource, view_count);
                if (!count_inserted &&
                    count->second != view_count) {
                    throw std::runtime_error(
                        "Compute resource port '" +
                        port.name + "' (resource '" +
                        port.resource +
                        "') has incompatible physical view counts "
                        "across target plans");
                }
            }
        }
        if (!definition.resource_ports.empty() &&
            !task_has_physical_plan) {
            throw std::runtime_error(
                "Typed compute task has no physical target plan: " +
                definition.name);
        }
        const auto task_id = dependencies.compute_task_container.registerComputeTask(
            definition,
            ComputeTaskRuntimeDependencies{
                dependencies.runtime.shader_library,
                dependencies.runtime.path_resolver,
                dependencies.render_targets.render_target_container,
                dependencies.frame_graph_resources,
                &resource_views,
                &resource_view_counts,
            });
        compiled.push_back(CompiledComputeTask{definition, task_id});
    }
    return compiled;
}

void mergeVariantRenderTargetPhysicalRequirements(
    std::span<RenderCompilerProgramVariantOutput>
        variants) {
    struct Requirements {
        std::uint32_t maximum_layers = 1;
        vk::ImageUsageFlags usage;
        std::optional<vk::Format> format;
        std::optional<ImageMipLevelCount>
            mip_levels;
        std::optional<RenderTargetStorageMode>
            storage_mode;
        std::optional<std::string> alias_group;
        bool alias_group_initialized = false;
        bool alias_group_compatible = true;
    };
    std::unordered_map<std::string, Requirements>
        requirements;
    for (const auto &variant : variants) {
        const auto &physical =
            requireVulkanRenderCompilerPhysicalPackage(
                *variant.physical_package);
        for (const auto &target :
             physical.render_target_definitions) {
            auto &merged =
                requirements[target.name];
            merged.maximum_layers = std::max(
                merged.maximum_layers,
                target.array_layers);
            merged.usage |= target.usage;
            if (!merged.mip_levels) {
                merged.mip_levels =
                    target.mip_levels;
            } else if (*merged.mip_levels !=
                       target.mip_levels) {
                throw std::runtime_error(
                    "render graph variants require conflicting "
                    "mip-level contracts for target '" +
                    target.name + "'");
            }
            if (!merged.format) {
                merged.format = target.format;
            } else if (*merged.format !=
                       target.format) {
                throw std::runtime_error(
                    "render graph variants require conflicting "
                    "physical formats for target '" +
                    target.name + "'");
            }
            if (!merged.storage_mode) {
                merged.storage_mode =
                    target.storage_mode;
            } else if (
                *merged.storage_mode !=
                target.storage_mode) {
                // Variant images share one allocation. Materialized storage
                // is the safe superset when (for example) flat rendering
                // chooses a transient depth attachment while XR requires the
                // same depth image as an external transfer source.
                merged.storage_mode =
                    RenderTargetStorageMode::
                        materialized;
            }
            if (!merged.alias_group_initialized) {
                merged.alias_group =
                    target.alias_group;
                merged.alias_group_initialized =
                    true;
            } else if (
                merged.alias_group !=
                target.alias_group) {
                // An alias relationship is valid only when every variant
                // publishes the same relationship. Falling back to distinct
                // materialized allocations is always safe.
                merged.alias_group_compatible =
                    false;
                merged.alias_group.reset();
                merged.storage_mode =
                    RenderTargetStorageMode::
                        materialized;
            }
        }
    }
    for (auto &variant : variants) {
        auto &physical =
            requireVulkanRenderCompilerPhysicalPackage(
                *variant.physical_package);
        for (auto &target :
             physical.render_target_definitions) {
            const auto &merged =
                requirements.at(target.name);
            target.array_layers =
                merged.maximum_layers;
            target.usage = merged.usage;
            target.mip_levels =
                *merged.mip_levels;
            target.format = *merged.format;
            target.storage_mode =
                *merged.storage_mode;
            target.alias_group =
                merged.alias_group_compatible
                    ? merged.alias_group
                    : std::nullopt;
        }
    }
}

std::vector<RenderPipelineProgramPreparation>
registerPreparedRenderingPassConfigVariant(
    RenderCompilerProgramVariantOutput &prepared,
    vk::Extent2D base_extent,
    RenderingPassConfigRegistrationDependencies &dependencies,
    RenderPipelineGpuRegistrationArena &gpu_arena,
    const PassImplementationRegistrySnapshot
        &pass_implementation_providers,
    RenderingPassConfigRegistrationResult &result) {
    auto &physical =
        requireVulkanRenderCompilerPhysicalPackage(
            *prepared.physical_package);
    if (dependencies.options
            .prepare_additional_gpu_resources) {
        dependencies.options
            .prepare_additional_gpu_resources();
    }
    registerRenderTargetDefinitions(
        physical.render_target_definitions,
        base_extent,
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
            physical.target_plans.find(definition.name);
        if (target_plan ==
                physical.target_plans.end() ||
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
        compileComputeTasks(
            prepared.compute_task_definitions,
            physical.target_plans,
            dependencies);
    injectGpuRegistrationFault(
        dependencies.options,
        RenderPipelineGpuRegistrationFaultPoint::
            after_compute_tasks);
    std::vector<CompiledRenderingPass> compiled_passes;
    compiled_passes.reserve(pass_definitions.size());
    for (const auto &definition : pass_definitions) {
        const auto target_plan =
            physical.target_plans.find(definition.name);
        if (target_plan == physical.target_plans.end() ||
            target_plan->second == nullptr) {
            throw std::runtime_error(
                "Physical target plan not found while compiling rendering "
                "pass runtime: " +
                definition.name);
        }
        auto runtime_dependencies =
            toRuntimeDependencies(
                dependencies.runtime, rt_metadata, rt_views,
                dependencies.frame_graph_resources, gpu_arena);
        runtime_dependencies.target_plan =
            target_plan->second.get();
        compiled_passes.push_back(
            compileRenderingPassRuntime(
                definition,
                std::move(runtime_dependencies)));
    }
    injectGpuRegistrationFault(
        dependencies.options,
        RenderPipelineGpuRegistrationFaultPoint::
            after_rendering_passes);

    result.feature_names =
        prepared.compiled_pipeline->feature_names;
    result.excluded_feature_names =
        prepared.compiled_pipeline->excluded_feature_names;
    result.target_plans =
        physical.target_plan_compilation.plans;
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
            physical.target_plans.find(pass_name);
        if (found_target_plan ==
            physical.target_plans.end()) {
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

const RenderCompilerProgram &
effectiveRenderCompilerProgram(
    const RenderingPassConfigRegistrationDependencies::Options
        &options) {
    if (options.render_compiler_program != nullptr) {
        return *options.render_compiler_program;
    }
    return defaultVulkanRenderCompilerProgram();
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
            &candidate.pass_container &&
        &effectiveRenderCompilerProgram(first.options) ==
            &effectiveRenderCompilerProgram(
                candidate.options);
    if (!same) {
        throw std::runtime_error(
            "Render pipeline variant transaction spans different registration domains");
    }
}

std::mutex &gpuRegistrationMutex() {
    static std::mutex mutex;
    return mutex;
}

struct RegisteredRenderGraphVariantFamily {
    std::vector<RenderingPassConfigRegistrationResult>
        runtime_variants;
    std::optional<PreviewGraphProgram> preview;
};

RegisteredRenderGraphVariantFamily
registerRenderingPassConfigVariantsData(
    const nlohmann::json &rendering_pass_data,
    vk::Extent2D base_extent,
    std::vector<RenderingPassConfigRegistrationDependencies>
        dependencies,
    bool include_preview = false) {
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

    // Acquire provider leases in the same order used by game-DLL owner
    // release (pass implementation, subgraph replacement, graph transform,
    // then render strategy). Keeping one immutable set across every variant
    // avoids mixed generations and a shared/exclusive lock-order inversion
    // during hot reload.
    auto pass_implementation_providers =
        passImplementationRegistry().snapshot();
    auto subgraph_replacement_providers =
        subgraphReplacementRegistry().snapshot();
    auto graph_transform_providers =
        graphTransformRegistry().snapshot();
    auto render_strategy_providers =
        renderStrategyRegistry().snapshot();

    // One top-level program sees and compiles the complete variant family.
    // Its backend package structure is checked before any mutable GPU
    // registry checkpoint is opened. The default program preserves the prior
    // logical + Vulkan sequence; a backend-native program can directly
    // construct the same physical package. Runtime object preparation retains
    // the existing rollback checks below.
    std::vector<RenderCompilerProgramVariantRequest>
        variant_requests;
    variant_requests.reserve(
        dependencies.size() +
        (include_preview ? 1u : 0u));
    for (const auto &variant : dependencies) {
        RenderCompilerProgramVariantRequest request{
            .graph_variant =
                variant.options.graph_variant,
            .enable_multiview_runtime =
                variant.options
                    .enable_multiview_runtime,
            .enable_external_depth_export =
                variant.options
                    .enable_external_depth_export,
        };
#if PELICAN_WITH_IMGUI
        if (request.graph_variant ==
            RenderPipelineGraphVariant::flat) {
            request.compose_runtime_config =
                [](nlohmann::json &config) {
                    invokeImGuiRuntimeCallback(
                        GET_MODULE(EngineLaunchConfig),
                        [&config] {
                            appendImGuiPassToCanonicalGraphs(
                                config);
                        });
                };
        }
#endif
        variant_requests.push_back(
            std::move(request));
    }
    if (include_preview) {
        variant_requests.push_back(
            RenderCompilerProgramVariantRequest{
                .graph_variant =
                    RenderPipelineGraphVariant::
                        preview,
                .artifact =
                    RenderCompilerProgramArtifact::
                        data_only,
            });
    }
#if PELICAN_RUNTIME_SHADER_COMPILER
    constexpr bool runtime_shader_compiler_enabled =
        true;
#else
    constexpr bool runtime_shader_compiler_enabled =
        false;
#endif
    const VulkanRenderCompilerBackendContext
        backend_context{
            dependencies.front()
                .runtime.render_target
                .getSwapchainFormat(),
            dependencies.front()
                .runtime.render_target
                .getExtent(),
            GET_MODULE(VulkanManageCore)
                .getPhysDevice(),
        };
    const RenderCompilerProgramInput compiler_input{
        .rendering_config = rendering_pass_data,
        .source_name =
            "rendering pass registration",
        .path_resolver =
            dependencies.front()
                .runtime.path_resolver,
        .runtime_shader_compiler_enabled =
            runtime_shader_compiler_enabled,
        .graph_transforms =
            graph_transform_providers,
        .render_strategies =
            render_strategy_providers,
        .subgraph_replacements =
            subgraph_replacement_providers,
        .backend_context = backend_context,
        .variants = variant_requests,
    };
    auto compiler_output =
        runRenderCompilerProgram(
            effectiveRenderCompilerProgram(
                dependencies.front().options),
            compiler_input);
    auto prepared_variants =
        std::move(compiler_output.variants);
    std::optional<PreviewGraphProgram>
        prepared_preview;
    if (include_preview) {
        auto preview_variant =
            std::move(prepared_variants.back());
        prepared_variants.pop_back();
        prepared_preview =
            makePreviewGraphProgram(
                std::move(
                    preview_variant
                        .compiled_pipeline),
                std::move(
                    preview_variant
                        .normalized_config));
    }
    for (std::size_t index = 0;
         index < dependencies.size(); ++index) {
        dependencies[index].runtime.shader_defines =
            prepared_variants[index]
                .compiled_pipeline->shader_defines;
    }
    // Flat and XR variants may share logical target names. Allocate the union
    // of image usage plus the maximum layer count before the first mutable
    // registration so later variants reuse a safe physical superset without
    // expanding live resources inside the transaction.
    mergeVariantRenderTargetPhysicalRequirements(
        prepared_variants);

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
    const auto output_facts =
        first.runtime.render_target.caps()
            .compile_facts;
    auto prepared_generation =
        first.frame_graph_runtime.prepareGeneration(
            std::move(program_preparations),
            std::move(enabled_feature_names),
            gpu_arena.preparedScope(owner_scope),
            output_facts.target_kind ==
                    OutputTargetKind::window
                ? std::optional<OutputCompileFacts>{
                      output_facts}
                : std::nullopt);
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
    return {
        .runtime_variants =
            std::move(results),
        .preview =
            std::move(prepared_preview),
    };
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
    return std::move(
        results.runtime_variants.front());
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
    return std::move(
        results.runtime_variants.front());
}

std::vector<RenderingPassConfigRegistrationResult>
registerRenderingPassConfigVariantsFromJsonData(
    std::string_view json_data, vk::Extent2D base_extent,
    std::vector<RenderingPassConfigRegistrationDependencies>
        dependencies) {
    return registerRenderingPassConfigVariantsData(
               loadRenderingPassConfigJsonFromString(
                   json_data,
                   "ProjectBasicConfig"),
               base_extent,
               std::move(dependencies))
        .runtime_variants;
}

RenderGraphVariantFamilyRegistrationResult
registerRenderGraphVariantFamilyFromJsonData(
    std::string_view json_data,
    vk::Extent2D base_extent,
    std::vector<RenderingPassConfigRegistrationDependencies>
        runtime_dependencies) {
    auto registered =
        registerRenderingPassConfigVariantsData(
            loadRenderingPassConfigJsonFromString(
                json_data,
                "ProjectBasicConfig"),
            base_extent,
            std::move(runtime_dependencies),
            true);
    if (!registered.preview) {
        throw std::runtime_error(
            "Render graph variant family omitted its "
            "preview artifact");
    }
    return {
        .runtime_variants =
            std::move(
                registered.runtime_variants),
        .preview =
            std::move(*registered.preview),
    };
}

} // namespace Pelican
