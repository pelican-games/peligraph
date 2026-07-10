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
#include "../renderer/debugtext.hpp"
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
#include <map>

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
    DebugText *debug_text;
    RenderTiming *render_timing;
    const Camera &camera;
    LightContainer &light_container;
};

RenderFrameModules resolveRenderFrameModules() {
    auto &rendering_pass_container = GET_MODULE(RenderingPassContainer);
    DebugDraw *debug_draw =
        rendering_pass_container.isFeatureEnabled("debug_draw") ? &GET_MODULE(DebugDraw) : nullptr;
    DebugText *debug_text =
        rendering_pass_container.isFeatureEnabled("debug_text") ? &GET_MODULE(DebugText) : nullptr;
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
        debug_text,
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

std::string loadOpName(vk::AttachmentLoadOp op) {
    switch (op) {
    case vk::AttachmentLoadOp::eLoad:
        return "load";
    case vk::AttachmentLoadOp::eClear:
        return "clear";
    case vk::AttachmentLoadOp::eDontCare:
        return "dont_care";
    default:
        return vk::to_string(op);
    }
}

std::string storeOpName(vk::AttachmentStoreOp op) {
    switch (op) {
    case vk::AttachmentStoreOp::eStore:
        return "store";
    case vk::AttachmentStoreOp::eDontCare:
        return "dont_care";
    default:
        return vk::to_string(op);
    }
}

std::string layoutName(vk::ImageLayout layout) {
    switch (layout) {
    case vk::ImageLayout::eUndefined:
        return "undefined";
    case vk::ImageLayout::eGeneral:
        return "general";
    case vk::ImageLayout::eColorAttachmentOptimal:
        return "color_attachment_optimal";
    case vk::ImageLayout::eDepthAttachmentOptimal:
        return "depth_attachment_optimal";
    case vk::ImageLayout::eShaderReadOnlyOptimal:
        return "shader_read_only_optimal";
    case vk::ImageLayout::eTransferSrcOptimal:
        return "transfer_src_optimal";
    case vk::ImageLayout::ePresentSrcKHR:
        return "present_src";
    default:
        return vk::to_string(layout);
    }
}

std::string renderTargetName(GlobalRenderTargetId rt_id, const RenderTargetContainer &rt_container) {
    if (isSwapchainRenderTarget(rt_id)) {
        return "swapchain";
    }
    if (!isConcreteRenderTarget(rt_id)) {
        return "none";
    }
    return rt_container.getMetadata(rt_id).name;
}

std::string trackedLayoutName(GlobalRenderTargetId rt_id, vk::ImageLayout special_layout,
                              const RenderTargetLayoutTracker &layout_tracker) {
    return layoutName(isConcreteRenderTarget(rt_id) ? layout_tracker.currentLayout(rt_id) : special_layout);
}

nlohmann::json clearColorJson(const vk::ClearColorValue &clear_color) {
    return nlohmann::json::array({clear_color.float32[0], clear_color.float32[1], clear_color.float32[2],
                                  clear_color.float32[3]});
}

nlohmann::json renderNodeTrace(const CompiledPass &pass, size_t order,
                               const RenderTargetContainer &rt_container,
                               const RenderTargetLayoutTracker &layout_tracker) {
    const auto &definition = pass.definition;
    nlohmann::json attachments = nlohmann::json::array();
    for (const auto target : definition.output_color) {
        nlohmann::json attachment{
            {"resource", renderTargetName(target, rt_container)},
            {"aspect", "color"},
            {"load", loadOpName(definition.color_load_op)},
            {"store", storeOpName(definition.color_store_op)},
            {"final_layout", trackedLayoutName(target, vk::ImageLayout::eColorAttachmentOptimal,
                                                 layout_tracker)},
        };
        if (definition.color_load_op == vk::AttachmentLoadOp::eClear) {
            attachment["clear"] = clearColorJson(definition.clear_color);
        }
        attachments.push_back(std::move(attachment));
    }
    if (isConcreteRenderTarget(definition.output_depth)) {
        nlohmann::json attachment{
            {"resource", renderTargetName(definition.output_depth, rt_container)},
            {"aspect", "depth"},
            {"load", loadOpName(definition.depth_load_op)},
            {"store", storeOpName(definition.depth_store_op)},
            {"final_layout", trackedLayoutName(definition.output_depth,
                                                 vk::ImageLayout::eDepthAttachmentOptimal,
                                                 layout_tracker)},
        };
        if (definition.depth_load_op == vk::AttachmentLoadOp::eClear) {
            attachment["clear"] = 1.0;
        }
        attachments.push_back(std::move(attachment));
    }

    nlohmann::json inputs = nlohmann::json::array();
    for (const auto target : definition.input_targets) {
        inputs.push_back({
            {"resource", renderTargetName(target, rt_container)},
            {"final_layout", trackedLayoutName(target, vk::ImageLayout::eShaderReadOnlyOptimal,
                                                 layout_tracker)},
        });
    }

    return nlohmann::json{
        {"name", definition.name},
        {"kind", "render"},
        {"order", order},
        {"inputs", std::move(inputs)},
        {"input_buffers", definition.input_buffers},
        {"attachments", std::move(attachments)},
    };
}

nlohmann::json computeNodeTrace(const CompiledComputeTask &task, size_t order,
                                const RenderTargetContainer &rt_container,
                                const RenderTargetLayoutTracker &layout_tracker) {
    const auto trace_resources = [&](const std::vector<std::string> &resources) {
        nlohmann::json result = nlohmann::json::array();
        for (const auto &resource : resources) {
            nlohmann::json entry{{"resource", resource}};
            const auto target = rt_container.getRenderTargetIdByName(resource);
            if (isConcreteRenderTarget(target)) {
                entry["final_layout"] = layoutName(layout_tracker.currentLayout(target));
            } else {
                entry["kind"] = "buffer";
            }
            result.push_back(std::move(entry));
        }
        return result;
    };

    return nlohmann::json{
        {"name", task.definition.name},
        {"kind", "compute"},
        {"order", order},
        {"reads", trace_resources(task.definition.reads)},
        {"writes", trace_resources(task.definition.writes)},
    };
}

nlohmann::json finalLayoutsTrace(const CompiledRenderingPass &rendering_pass,
                                 const RenderTargetContainer &rt_container,
                                 const RenderTargetLayoutTracker &layout_tracker,
                                 vk::ImageLayout frame_target_layout) {
    std::map<std::string, std::string> layouts;
    const auto add_target = [&](GlobalRenderTargetId target) {
        if (isSwapchainRenderTarget(target)) {
            layouts["swapchain"] = layoutName(frame_target_layout);
        } else if (isConcreteRenderTarget(target)) {
            layouts[renderTargetName(target, rt_container)] = layoutName(layout_tracker.currentLayout(target));
        }
    };
    for (const auto &pass : rendering_pass.passes) {
        for (const auto target : pass.definition.input_targets) {
            add_target(target);
        }
        for (const auto target : pass.definition.output_color) {
            add_target(target);
        }
        add_target(pass.definition.output_depth);
    }
    for (const auto &task : rendering_pass.compute_tasks) {
        for (const auto &resource : task.definition.reads) {
            add_target(rt_container.getRenderTargetIdByName(resource));
        }
        for (const auto &resource : task.definition.writes) {
            add_target(rt_container.getRenderTargetIdByName(resource));
        }
    }

    nlohmann::json result = nlohmann::json::array();
    for (const auto &[resource, layout] : layouts) {
        result.push_back({{"resource", resource}, {"layout", layout}});
    }
    return result;
}

std::vector<std::string> plannedNodeNames(const CompiledFrameGraphExecution &frame_graph) {
    std::vector<std::string> names;
    names.reserve(frame_graph.nodes.size());
    for (const auto &node : frame_graph.nodes) {
        names.push_back(node.name);
    }
    return names;
}

void executePlannedFrameGraph(const FrameRenderContext &render_ctx,
                              const CompiledRenderingPass &rendering_pass,
                              const CompiledFrameGraphExecution &frame_graph,
                              RenderFrameModules &modules,
                              RenderTargetLayoutTracker &layout_tracker,
                              const RenderPassExecutorDependencies &pass_executor_dependencies,
                              nlohmann::json *node_trace) {
    if (modules.render_timing != nullptr) {
        beginTiming(modules.render_timing, render_ctx.cmd_buf, plannedNodeNames(frame_graph));
    }

    for (uint32_t node_index = 0; node_index < frame_graph.nodes.size(); ++node_index) {
        const auto &execution_node = frame_graph.nodes[node_index];
        if (node_index >= frame_graph.plan.nodes.size() ||
            frame_graph.plan.nodes[node_index].name != execution_node.name ||
            frame_graph.plan.nodes[node_index].kind != execution_node.kind) {
            throw std::runtime_error("Frame graph execution no longer matches frame plan");
        }

        for (const auto &barrier : execution_node.incoming_barriers) {
            if (barrier.from_node_index >= node_index) {
                throw std::runtime_error("Compiled frame graph barrier source was not executed before target");
            }
            modules.compute_task_container.bufferReadAfterWriteBarrier(
                render_ctx.cmd_buf, barrier.resource, barrier.from_kind, barrier.to_kind);
        }

        if (modules.render_timing != nullptr) {
            modules.render_timing->writePassStart(render_ctx.cmd_buf, node_index);
        }

        if (execution_node.kind == FramePlanNodeKind::render) {
            const auto &pass = rendering_pass.passes.at(execution_node.index);
            modules.pass_executor.execute(render_ctx, pass,
                                          pass_executor_dependencies, layout_tracker);
            if (node_trace != nullptr) {
                node_trace->push_back(renderNodeTrace(pass, node_index, modules.render_target_container,
                                                      layout_tracker));
            }
        } else {
            const auto &task = rendering_pass.compute_tasks.at(execution_node.index);
            modules.compute_task_container.transitionResourcesForDispatch(
                render_ctx.cmd_buf, task.task_id, modules.render_target_container, modules.vk_utils,
                layout_tracker);
            modules.compute_task_container.dispatch(render_ctx.cmd_buf, task.task_id);
            if (node_trace != nullptr) {
                node_trace->push_back(computeNodeTrace(task, node_index, modules.render_target_container,
                                                       layout_tracker));
            }
        }

        if (modules.render_timing != nullptr) {
            modules.render_timing->writePassEnd(render_ctx.cmd_buf, node_index);
        }
    }

    endTiming(modules.render_timing);
}

void executeRenderingPasses(const FrameRenderContext &render_ctx,
                            RenderingPassId rendering_pass_id,
                            const CompiledRenderingPass &rendering_pass,
                            RenderFrameModules &modules,
                            RenderTargetLayoutTracker &layout_tracker,
                            nlohmann::json *node_trace) {
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
        modules.debug_text,
        modules.camera,
        modules.render_target.getSwapchainFormat()};
    const RenderPassExecutorDependencies pass_executor_dependencies{modules.render_target_container, modules.vk_utils,
                                                                    pass_dispatch_dependencies};

    const auto *frame_graph = modules.frame_graph_runtime.find(rendering_pass_id);
    if (frame_graph == nullptr) {
        throw std::runtime_error("Frame graph execution is not registered");
    }
    executePlannedFrameGraph(render_ctx, rendering_pass, *frame_graph, modules, layout_tracker,
                             pass_executor_dependencies, node_trace);
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

std::vector<std::string> Renderer::currentFramePlanOrderForTesting() const {
    const auto *frame_graph = GET_MODULE(FrameGraphRuntimeContainer).find(current_rendering_pass_id);
    if (frame_graph == nullptr) {
        throw std::runtime_error("Current frame plan is not registered");
    }
    return framePlanOrder(frame_graph->plan);
}

void Renderer::recreateRenderTargetsAndRebindForTesting(vk::Extent2D extent) {
    auto modules = resolveRenderFrameModules();
    modules.render_target_container.recreateForExtent(extent);
    rebindFullscreenInputs(modules);
    render_target_layout_tracker.reset();
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
    nlohmann::json node_trace;
    nlohmann::json *node_trace_ptr = nullptr;
    if (execution_tracing_for_testing) {
        node_trace = nlohmann::json::array();
        node_trace_ptr = &node_trace;
    }
    executeRenderingPasses(render_ctx, current_rendering_pass_id, rendering_pass, modules,
                           render_target_layout_tracker, node_trace_ptr);

    modules.render_target.render_end();
    if (execution_tracing_for_testing) {
        last_execution_trace = nlohmann::json{
            {"nodes", std::move(node_trace)},
            {"final_layouts", finalLayoutsTrace(rendering_pass, modules.render_target_container,
                                                 render_target_layout_tracker, render_ctx.required_layout)},
        };
    }
}

} // namespace Pelican
