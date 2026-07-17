#include "renderingpassconfigregistration.hpp"
#include "computetask.hpp"
#include "../../project/featurecompose.hpp"
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
#include "../renderer/shadowdepthpasscontainer.hpp"
#include "../renderer/velocitypasscontainer.hpp"
#include "../vkcore/rendertarget.hpp"
#if PELICAN_WITH_IMGUI
#include "../imgui/imguiruntime.hpp"
#include "../launchconfig.hpp"
#endif
#include <algorithm>
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
        &GET_MODULE(ShadowDepthPassContainer),
        &GET_MODULE(VelocityPassContainer),
        &frame_graph_resources,
        &dependencies.path_resolver,
        dependencies.shader_defines,
        dependencies.warn_backend_specific_shader_refs,
        dependencies.debug_draw_provider,
        dependencies.debug_text_provider,
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
    RenderingPassConfigRuntimeDependencies &dependencies,
    const RenderingPassConfigRegistrationDependencies::Options &options) {
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
            options.include_feature,
        });
    dependencies.shader_defines = composed.shader_defines;
    return composed;
}

void suffixRenderingPassNames(nlohmann::json &config, std::string_view suffix) {
    if (suffix.empty()) return;
    for (auto &pass : config.at("rendering_passes")) {
        pass["name"] = pass.at("name").get<std::string>() + std::string{suffix};
    }
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

RenderingPassConfigRegistrationResult registerRenderingPassConfigData(
    const nlohmann::json &rendering_pass_data, vk::Extent2D base_extent,
    RenderingPassConfigRegistrationDependencies dependencies) {
    const auto composed =
        composeRenderFeaturesForRegistration(rendering_pass_data, dependencies.runtime,
                                             dependencies.options);
    const bool hdr_enabled = std::find(composed.feature_names.begin(), composed.feature_names.end(), "hdr") !=
                             composed.feature_names.end();
    auto composed_rendering_pass_data = resolveRenderTargetFormatClassesV2(
        composed.config, dependencies.runtime.render_target.getSwapchainFormat(),
        dependencies.runtime.render_target.getExtent(), hdr_enabled);
    if (dependencies.options.validate_composed_config) {
        dependencies.options.validate_composed_config(composed_rendering_pass_data);
    }
    suffixRenderingPassNames(composed_rendering_pass_data,
                             dependencies.options.rendering_pass_name_suffix);
#if PELICAN_WITH_IMGUI
    if (dependencies.options.rendering_pass_name_suffix.empty()) {
        invokeImGuiRuntimeCallback(GET_MODULE(EngineLaunchConfig), [&] {
            appendImGuiPassToCanonicalGraphs(composed_rendering_pass_data);
        });
    }
#endif
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
    auto compute_task_definitions = parseComputeTaskDefinitionsFromConfigJson(composed_rendering_pass_data);
    auto graph_definition_list =
        parseFrameGraphDefinitionsFromConfigJson(composed_rendering_pass_data);
    namespaceComputeTasks(compute_task_definitions, graph_definition_list,
                          dependencies.options.rendering_pass_name_suffix);
    auto compiled_compute_tasks =
        compileComputeTasks(compute_task_definitions, dependencies);
    auto graph_definitions = graphDefinitionsByName(std::move(graph_definition_list));
    auto compiled_passes =
        compileRenderingPassesRuntime(pass_definitions,
                                      toRuntimeDependencies(dependencies.runtime, rt_metadata, rt_views,
                                                            dependencies.frame_graph_resources));
    if (dependencies.options.publish_enabled_features) {
        dependencies.pass_container.setEnabledFeatures(composed.feature_names);
    }
    nlohmann::json composition_metadata = nlohmann::json::object();
    if (composed.projection_jitter) {
        composition_metadata["projection_jitter"] = *composed.projection_jitter;
    }
    nlohmann::json bound_instances = nlohmann::json::array();
    for (const auto &instance : composed.feature_instances) {
        if (!instance.at("parameters").empty()) {
            bound_instances.push_back(instance);
        }
    }
    if (!bound_instances.empty()) {
        composition_metadata["feature_instances"] = std::move(bound_instances);
    }
    if (!dependencies.options.rendering_pass_name_suffix.empty()) {
        composition_metadata["graph_variant"] = "xr";
        composition_metadata["excluded_features"] = composed.excluded_feature_names;
    }
    RenderingPassConfigRegistrationResult result;
    result.feature_names = composed.feature_names;
    result.excluded_feature_names = composed.excluded_feature_names;
    for (auto &compiled_pass : compiled_passes) {
        compiled_pass.compute_tasks = compiled_compute_tasks;
        const auto pass_name = compiled_pass.name;
        const auto rendering_pass_id = dependencies.pass_container.registerCompiledRenderingPass(std::move(compiled_pass));
        result.rendering_pass_ids.push_back(rendering_pass_id);
        auto found_graph = graph_definitions.find(pass_name);
        if (found_graph == graph_definitions.end()) {
            throw std::runtime_error("Frame graph definition not found for rendering pass: " + pass_name);
        }
        dependencies.frame_graph_runtime.registerExecutionPlan(
            rendering_pass_id,
            dependencies.pass_container.getCompiledRenderingPass(rendering_pass_id),
            found_graph->second,
            composition_metadata);
    }
    return result;
}

} // namespace

RenderingPassConfigRegistrationResult registerRenderingPassConfigFromJson(
    const std::string &json_path, vk::Extent2D base_extent,
    RenderingPassConfigRegistrationDependencies dependencies) {
    return registerRenderingPassConfigData(loadRenderingPassConfigJson(json_path), base_extent,
                                           std::move(dependencies));
}

RenderingPassConfigRegistrationResult registerRenderingPassConfigFromJsonData(
    std::string_view json_data, vk::Extent2D base_extent,
    RenderingPassConfigRegistrationDependencies dependencies) {
    return registerRenderingPassConfigData(
        loadRenderingPassConfigJsonFromString(json_data, "ProjectBasicConfig"),
        base_extent, std::move(dependencies));
}

} // namespace Pelican
