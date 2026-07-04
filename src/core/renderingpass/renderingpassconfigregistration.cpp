#include "renderingpassconfigregistration.hpp"
#include "computetask.hpp"
#include "featurecompose.hpp"
#include "framegraphruntime.hpp"
#include "frameplanner.hpp"
#include "renderingpassconfigjsonparser.hpp"
#include "renderingpassconfigloader.hpp"
#include "renderingpasscontainer.hpp"
#include "renderingpassruntimecompiler.hpp"
#include "rendertargetconfigregistration.hpp"
#include "rendertargetimageviewresolver.hpp"
#include "rendertargetjsonparser.hpp"
#include "rendertargetmetadataresolver.hpp"
#include "rendertargetnameresolver.hpp"
#include "../loader/pathresolver.hpp"
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Pelican {

namespace {

RenderingPassRuntimeDependencies toRuntimeDependencies(
    RenderingPassConfigRuntimeDependencies &dependencies,
    const RenderTargetMetadataResolver &rt_metadata,
    const RenderTargetImageViewResolver &rt_views,
    FrameGraphResourceContainer &frame_graph_resources) {
    return RenderingPassRuntimeDependencies{
        &dependencies.render_target,
        &rt_metadata,
        &rt_views,
        &dependencies.shader_library,
        &dependencies.fullscreen_pass_container,
        &frame_graph_resources,
        &dependencies.path_resolver,
        dependencies.shader_defines,
        dependencies.warn_backend_specific_shader_refs,
        dependencies.debug_draw_provider,
    };
}

std::unordered_map<std::string, FrameGraphDefinition> graphDefinitionsByName(
    std::vector<FrameGraphDefinition> definitions) {
    std::unordered_map<std::string, FrameGraphDefinition> by_name;
    for (auto &definition : definitions) {
        by_name.emplace(definition.name, std::move(definition));
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
            });
        compiled.push_back(CompiledComputeTask{definition, task_id});
    }
    return compiled;
}

RenderFeatureComposeResult composeRenderFeaturesForRegistration(
    const nlohmann::json &rendering_pass_data,
    RenderingPassConfigRuntimeDependencies &dependencies) {
    const auto composed = composeRenderFeatureConfig(
        rendering_pass_data,
        RenderFeatureComposeDependencies{
            [&dependencies](std::string_view ref) {
                return dependencies.path_resolver.loadText(ref);
            },
#if PELICAN_RUNTIME_SHADER_COMPILER
            true,
#else
            false,
#endif
        });
    dependencies.shader_defines = composed.shader_defines;
    return composed;
}

void registerRenderingPassConfigData(const nlohmann::json &rendering_pass_data, vk::Extent2D base_extent,
                                     RenderingPassConfigRegistrationDependencies dependencies) {
    const auto composed =
        composeRenderFeaturesForRegistration(rendering_pass_data, dependencies.runtime);
    const auto &composed_rendering_pass_data = composed.config;
    const auto render_target_definitions = parseRenderTargetDefinitionsFromJson(composed_rendering_pass_data);
    registerRenderTargetDefinitions(render_target_definitions, base_extent,
                                    dependencies.render_targets.render_target_container);
    const auto buffer_definitions = parseFrameGraphBufferDefinitionsFromJson(composed_rendering_pass_data);
    const auto buffer_names = frameGraphBufferNameSet(buffer_definitions);
    dependencies.frame_graph_resources.registerBuffers(buffer_definitions);

    const RenderTargetNameResolver rt_resolver{dependencies.render_targets.render_target_container};
    const RenderTargetMetadataResolver rt_metadata{dependencies.render_targets.render_target_container};
    const RenderTargetImageViewResolver rt_views{dependencies.render_targets.render_target_container};
    const auto pass_definitions =
        parseRenderingPassDefinitionsFromConfigJson(composed_rendering_pass_data, rt_resolver, rt_metadata,
                                                    buffer_names);
    const auto compute_task_definitions = parseComputeTaskDefinitionsFromConfigJson(composed_rendering_pass_data);
    auto compiled_compute_tasks =
        compileComputeTasks(compute_task_definitions, dependencies);
    auto graph_definitions = compiled_compute_tasks.empty()
                                 ? std::unordered_map<std::string, FrameGraphDefinition>{}
                                 : graphDefinitionsByName(parseFrameGraphDefinitionsFromConfigJson(
                                       composed_rendering_pass_data));
    auto compiled_passes =
        compileRenderingPassesRuntime(pass_definitions,
                                      toRuntimeDependencies(dependencies.runtime, rt_metadata, rt_views,
                                                            dependencies.frame_graph_resources));
    dependencies.pass_container.setEnabledFeatures(composed.feature_names);
    for (auto &compiled_pass : compiled_passes) {
        compiled_pass.compute_tasks = compiled_compute_tasks;
        const auto pass_name = compiled_pass.name;
        const auto rendering_pass_id = dependencies.pass_container.registerCompiledRenderingPass(std::move(compiled_pass));
        if (!compiled_compute_tasks.empty()) {
            auto found_graph = graph_definitions.find(pass_name);
            if (found_graph == graph_definitions.end()) {
                throw std::runtime_error("Frame graph definition not found for rendering pass: " + pass_name);
            }
            dependencies.frame_graph_runtime.registerExecutionPlan(
                rendering_pass_id,
                dependencies.pass_container.getCompiledRenderingPass(rendering_pass_id),
                found_graph->second);
        }
    }
}

} // namespace

void registerRenderingPassConfigFromJson(const std::string &json_path, vk::Extent2D base_extent,
                                         RenderingPassConfigRegistrationDependencies dependencies) {
    registerRenderingPassConfigData(loadRenderingPassConfigJson(json_path), base_extent, std::move(dependencies));
}

void registerRenderingPassConfigFromJsonData(std::string_view json_data, vk::Extent2D base_extent,
                                             RenderingPassConfigRegistrationDependencies dependencies) {
    registerRenderingPassConfigData(loadRenderingPassConfigJsonFromString(json_data, "ProjectBasicConfig"),
                                    base_extent, std::move(dependencies));
}

} // namespace Pelican
