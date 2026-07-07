#include "renderer.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/fullscreenpassrenderer.hpp"
#include "../renderer/materialrender.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../renderer/shadowdepthpasscontainer.hpp"
#include "../renderer/uicontainer.hpp"
#include "../renderer/uirenderer.hpp"
#include "../launchconfig.hpp"
#include "../light/lightcontainer.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../material/materialcontainer.hpp"
#include "../renderer/debugdraw.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../renderingpass/computetask.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/rendertargetimageviewresolver.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../appflow/enginetime.hpp"
#include "deletionqueue.hpp"
#include "render_pass_dispatch.hpp"
#include "render_pass_executor.hpp"
#include "renderer_config.hpp"
#include "rendertarget.hpp"
#include "rendertiming.hpp"
#include "util.hpp"
#include <unordered_map>
#include <unordered_set>

namespace Pelican {

namespace {

struct RenderFrameModules {
    RenderTarget &render_target;
    RenderTargetContainer &render_target_container;
    RenderingPassContainer &rendering_pass_container;
    RenderPassExecutor &pass_executor;
    FrameGraphRuntimeContainer &frame_graph_runtime;
    ComputeTaskContainer &compute_task_container;
    FrameGraphResourceContainer &frame_graph_resources;
    VulkanUtils &vk_utils;
    MaterialRenderer &material_renderer;
    ShadowDepthPassContainer &shadow_depth_pass_container;
    PolygonInstanceContainer &instance_container;
    const VertBufContainer &vert_buf_container;
    const MaterialContainer &material_container;
    FullscreenPassRenderer &fullscreen_pass_renderer;
    FullscreenPassContainer &fullscreen_pass_container;
    UiRenderer &ui_renderer;
    const UIContainer &ui_container;
    DebugDraw *debug_draw;
    RenderTiming *render_timing;
    const Camera &camera;
    LightContainer &light_container;
};

RenderFrameModules resolveRenderFrameModules() {
    auto &rendering_pass_container = GET_MODULE(RenderingPassContainer);
    DebugDraw *debug_draw =
        rendering_pass_container.isFeatureEnabled("debug_draw") ? &GET_MODULE(DebugDraw) : nullptr;
    RenderTiming *render_timing =
        rendering_pass_container.isFeatureEnabled("gpu_timing") ? &GET_MODULE(RenderTiming) : nullptr;

    return RenderFrameModules{
        GET_MODULE(RenderTarget),
        GET_MODULE(RenderTargetContainer),
        rendering_pass_container,
        GET_MODULE(RenderPassExecutor),
        GET_MODULE(FrameGraphRuntimeContainer),
        GET_MODULE(ComputeTaskContainer),
        GET_MODULE(FrameGraphResourceContainer),
        GET_MODULE(VulkanUtils),
        GET_MODULE(MaterialRenderer),
        GET_MODULE(ShadowDepthPassContainer),
        GET_MODULE(PolygonInstanceContainer),
        GET_MODULE(VertBufContainer),
        GET_MODULE(MaterialContainer),
        GET_MODULE(FullscreenPassRenderer),
        GET_MODULE(FullscreenPassContainer),
        GET_MODULE(UiRenderer),
        GET_MODULE(UIContainer),
        debug_draw,
        render_timing,
        GET_MODULE(Camera),
        GET_MODULE(LightContainer),
    };
}

void updateFrameAnimation(LightContainer &light_container, double time) {
    light_container.updateAnimation(static_cast<float>(time));
    light_container.update();
}

void beginTiming(RenderTiming *render_timing, vk::CommandBuffer cmd_buf, const std::vector<std::string> &node_names) {
    if (render_timing != nullptr) {
        render_timing->beginGpuFrame(cmd_buf, node_names);
    }
}

void endTiming(RenderTiming *render_timing) {
    if (render_timing != nullptr) {
        render_timing->endGpuFrame();
    }
}

std::vector<std::string> renderPassNodeNames(const CompiledRenderingPass &rendering_pass) {
    std::vector<std::string> pass_names;
    pass_names.reserve(rendering_pass.passes.size());
    for (const auto &pass : rendering_pass.passes) {
        pass_names.push_back(pass.definition.name);
    }
    return pass_names;
}

std::vector<std::string> plannedNodeNames(const CompiledFrameGraphExecution &frame_graph) {
    std::vector<std::string> names;
    names.reserve(frame_graph.nodes.size());
    for (const auto &node : frame_graph.nodes) {
        names.push_back(node.name);
    }
    return names;
}

std::unordered_map<std::string, FramePlanNodeKind> plannedNodeKinds(const FramePlan &plan) {
    std::unordered_map<std::string, FramePlanNodeKind> kinds;
    for (const auto &node : plan.nodes) {
        kinds.emplace(node.name, node.kind);
    }
    return kinds;
}

void applyPlanBarriersForNode(vk::CommandBuffer cmd_buf,
                              const FramePlan &plan,
                              const std::string &node_name,
                              const std::unordered_map<std::string, FramePlanNodeKind> &node_kinds,
                              const std::unordered_set<std::string> &executed_nodes,
                              ComputeTaskContainer &compute_task_container) {
    const auto to_kind = node_kinds.at(node_name);
    for (const auto &barrier : plan.barriers) {
        if (barrier.to != node_name) {
            continue;
        }
        if (barrier.kind != "read_after_write") {
            throw std::runtime_error("Unsupported frame plan barrier kind: " + barrier.kind);
        }
        if (executed_nodes.find(barrier.from) == executed_nodes.end()) {
            throw std::runtime_error("Frame plan barrier source was not executed before target: " +
                                     barrier.from + " -> " + barrier.to);
        }
        const auto from_kind = node_kinds.at(barrier.from);
        compute_task_container.bufferReadAfterWriteBarrier(cmd_buf, barrier.resource, from_kind, to_kind);
    }
}

void executeLegacyRenderingPasses(const FrameRenderContext &render_ctx,
                                  const CompiledRenderingPass &rendering_pass,
                                  RenderFrameModules &modules,
                                  RenderTargetLayoutTracker &layout_tracker,
                                  const RenderPassExecutorDependencies &pass_executor_dependencies) {
    beginTiming(modules.render_timing, render_ctx.cmd_buf, renderPassNodeNames(rendering_pass));

    for (uint32_t pass_index = 0; pass_index < rendering_pass.passes.size(); ++pass_index) {
        const auto &pass = rendering_pass.passes[pass_index];
        if (modules.render_timing != nullptr) {
            modules.render_timing->writePassStart(render_ctx.cmd_buf, pass_index);
        }
        modules.pass_executor.execute(render_ctx, pass, pass_executor_dependencies, layout_tracker);
        if (modules.render_timing != nullptr) {
            modules.render_timing->writePassEnd(render_ctx.cmd_buf, pass_index);
        }
    }

    endTiming(modules.render_timing);
}

void executePlannedFrameGraph(const FrameRenderContext &render_ctx,
                              const CompiledRenderingPass &rendering_pass,
                              const CompiledFrameGraphExecution &frame_graph,
                              RenderFrameModules &modules,
                              RenderTargetLayoutTracker &layout_tracker,
                              const RenderPassExecutorDependencies &pass_executor_dependencies) {
    beginTiming(modules.render_timing, render_ctx.cmd_buf, plannedNodeNames(frame_graph));
    const auto node_kinds = plannedNodeKinds(frame_graph.plan);
    std::unordered_set<std::string> executed_nodes;

    for (uint32_t node_index = 0; node_index < frame_graph.nodes.size(); ++node_index) {
        const auto &execution_node = frame_graph.nodes[node_index];
        if (node_index >= frame_graph.plan.nodes.size() ||
            frame_graph.plan.nodes[node_index].name != execution_node.name ||
            frame_graph.plan.nodes[node_index].kind != execution_node.kind) {
            throw std::runtime_error("Frame graph execution no longer matches frame plan");
        }

        applyPlanBarriersForNode(render_ctx.cmd_buf, frame_graph.plan, execution_node.name, node_kinds,
                                 executed_nodes, modules.compute_task_container);

        if (modules.render_timing != nullptr) {
            modules.render_timing->writePassStart(render_ctx.cmd_buf, node_index);
        }

        if (execution_node.kind == FramePlanNodeKind::render) {
            modules.pass_executor.execute(render_ctx, rendering_pass.passes.at(execution_node.index),
                                          pass_executor_dependencies, layout_tracker);
        } else {
            const auto &task = rendering_pass.compute_tasks.at(execution_node.index);
            modules.compute_task_container.transitionResourcesForDispatch(
                render_ctx.cmd_buf, task.task_id, modules.render_target_container, modules.vk_utils,
                layout_tracker);
            modules.compute_task_container.dispatch(render_ctx.cmd_buf, task.task_id);
        }

        if (modules.render_timing != nullptr) {
            modules.render_timing->writePassEnd(render_ctx.cmd_buf, node_index);
        }
        executed_nodes.insert(execution_node.name);
    }

    endTiming(modules.render_timing);
}

void executeRenderingPasses(const FrameRenderContext &render_ctx,
                            RenderingPassId rendering_pass_id,
                            const CompiledRenderingPass &rendering_pass,
                            RenderFrameModules &modules,
                            RenderTargetLayoutTracker &layout_tracker) {
    const MaterialRendererDependencies material_renderer_dependencies{modules.instance_container,
                                                                      modules.vert_buf_container,
                                                                      modules.material_container,
                                                                      modules.light_container,
                                                                      modules.camera,
                                                                      GET_MODULE(EngineTime).now()};
    const FullscreenPassRendererDependencies fullscreen_pass_renderer_dependencies{modules.fullscreen_pass_container,
                                                                                  modules.light_container};
    const UiRendererDependencies ui_renderer_dependencies{modules.ui_container};
    const RenderPassDispatchDependencies pass_dispatch_dependencies{
        modules.material_renderer,
        material_renderer_dependencies,
        modules.fullscreen_pass_renderer,
        fullscreen_pass_renderer_dependencies,
        modules.shadow_depth_pass_container,
        modules.ui_renderer,
        ui_renderer_dependencies,
        modules.debug_draw,
        modules.camera,
        modules.render_target.getSwapchainFormat()};
    const RenderPassExecutorDependencies pass_executor_dependencies{modules.render_target_container, modules.vk_utils,
                                                                    pass_dispatch_dependencies};

    const auto *frame_graph = modules.frame_graph_runtime.find(rendering_pass_id);
    if (frame_graph != nullptr && frame_graph->has_compute) {
        executePlannedFrameGraph(render_ctx, rendering_pass, *frame_graph, modules, layout_tracker,
                                 pass_executor_dependencies);
        return;
    }
    executeLegacyRenderingPasses(render_ctx, rendering_pass, modules, layout_tracker, pass_executor_dependencies);
}

void rebindFullscreenInputs(RenderFrameModules &modules) {
    RenderTargetImageViewResolver rt_views{modules.render_target_container};
    for (const auto pass_id : modules.rendering_pass_container.getRegisteredPassIds()) {
        const auto &compiled_pass = modules.rendering_pass_container.getCompiledRenderingPass(pass_id);
        for (const auto &pass : compiled_pass.passes) {
            if (pass.definition.isFullscreen() &&
                (!pass.definition.input_targets.empty() || !pass.definition.input_buffers.empty())) {
                modules.fullscreen_pass_container.setInputResources(pass.pass_id, pass.definition.input_targets,
                                                                    pass.definition.input_buffers, rt_views,
                                                                    modules.frame_graph_resources);
            }
        }
    }
}

void handleShaderHotReload(RenderFrameModules &modules) {
    if (!GET_MODULE(EngineLaunchConfig).shader_hot_reload) {
        return;
    }

    if (GET_MODULE(ShaderLibrary).reloadModifiedSources() == 0) {
        return;
    }

    GET_MODULE(PipelineFactory).rebuildDirty();
    rebindFullscreenInputs(modules);
}

void handleFrameTargetResize(RenderFrameModules &modules, RenderTargetLayoutTracker &layout_tracker) {
    if (!modules.render_target.consumeExtentChanged()) {
        return;
    }

    modules.render_target_container.recreateForExtent(modules.render_target.getExtent());
    rebindFullscreenInputs(modules);
    layout_tracker.reset();
}

} // namespace

Renderer::Renderer() {
    current_rendering_pass_id = loadDefaultRenderingPassFromConfig();
}

Renderer::~Renderer() = default;

nlohmann::json Renderer::currentFramePlanJson() const {
    const auto *frame_graph = GET_MODULE(FrameGraphRuntimeContainer).find(current_rendering_pass_id);
    if (frame_graph == nullptr) {
        throw std::runtime_error("Current frame plan is not registered");
    }
    return framePlanToJson(frame_graph->plan);
}

void Renderer::render() {
    GET_MODULE(DeletionQueue).beginFrame();

    auto modules = resolveRenderFrameModules();
    handleShaderHotReload(modules);
    updateFrameAnimation(modules.light_container, GET_MODULE(EngineTime).now());

    const auto render_ctx = modules.render_target.render_begin();
    handleFrameTargetResize(modules, render_target_layout_tracker);

    const auto &rendering_pass =
        modules.rendering_pass_container.getCompiledRenderingPass(current_rendering_pass_id);
    executeRenderingPasses(render_ctx, current_rendering_pass_id, rendering_pass, modules,
                           render_target_layout_tracker);

    modules.render_target.render_end();
}

} // namespace Pelican
