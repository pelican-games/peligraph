#include "renderer.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/fullscreenpassrenderer.hpp"
#include "../renderer/frameresources.hpp"
#include "../renderer/materialrender.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../renderer/shadowdepthpasscontainer.hpp"
#include "../renderer/spriterenderer.hpp"
#include "../renderer/spritescene.hpp"
#include "../renderer/atlasassetresource.hpp"
#include "../renderer/velocitypasscontainer.hpp"
#include "../renderer/uicontainer.hpp"
#include "../renderer/uirenderer.hpp"
#include "../ui/module.hpp"
#include "../watch/reloadgate.hpp"
#include "../watch/reloadservice.hpp"
#include "../launchconfig.hpp"
#include "../light/lightcontainer.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../material/materialcontainer.hpp"
#include "../material/standardmaterialresource.hpp"
#include "../renderer/debugdraw.hpp"
#include "../renderer/debugtext.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../renderingpass/computetask.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/renderingpassjsonhelpers.hpp"
#include "../renderingpass/rendertargetimageviewresolver.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../appflow/enginetime.hpp"
#include "deletionqueue.hpp"
#include "render_pass_dispatch.hpp"
#include "render_pass_executor.hpp"
#include "render_pass_frame_setup.hpp"
#include "renderer_config.hpp"
#include "rendertarget.hpp"
#include "rendertiming.hpp"
#include "util.hpp"
#if PELICAN_WITH_IMGUI
#include "../imgui/imguiruntime.hpp"
#include "../imgui/imguisystem.hpp"
#endif
#include <algorithm>
#include <map>

namespace Pelican {

namespace {

struct SpriteRenderModules {
    SpriteScene *scene = nullptr;
    SpriteRenderer *renderer = nullptr;
    AtlasAssetResource *atlas = nullptr;
};

struct ShaderHotReloadModules {
    watch::ReloadGate &gate;
    watch::ReloadService *reload_service = nullptr;
};

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
    VelocityPassContainer &velocity_pass_container;
    PolygonInstanceContainer &instance_container;
    const VertBufContainer &vert_buf_container;
    const MaterialContainer &material_container;
    FrameResources &frame_resources;
    FullscreenPassRenderer &fullscreen_pass_renderer;
    FullscreenPassContainer &fullscreen_pass_container;
    UiRenderer *ui_renderer;
    const UIContainer *ui_container;
    const ui::UiModule *ui_module;
    DebugDraw *debug_draw;
    DebugText *debug_text;
#if PELICAN_WITH_IMGUI
    ImGuiSystem *imgui_system;
#endif
    RenderTiming *render_timing;
    const Camera &camera;
    LightContainer &light_container;
    SpriteRenderModules sprite;
};

SpriteRenderModules resolveSpriteRenderModules(const RenderingPassContainer &rendering_pass_container) {
    if (!rendering_pass_container.isFeatureEnabled("sprite")) return {};

    auto *scene = FastModuleContainer::tryGet<SpriteScene>();
    if (scene == nullptr || scene->commandCountForTesting() == 0) return {};

    return {scene, &GET_MODULE(SpriteRenderer), &GET_MODULE(AtlasAssetResource)};
}

ShaderHotReloadModules resolveShaderHotReloadModules() {
    auto &gate = GET_MODULE(watch::ReloadGate);
    // EngineLaunchConfig remains a compatibility adapter. Syncing here keeps
    // direct renderer fixtures and legacy embedders on the centralized gate,
    // without changing ShaderLibrary's polling implementation.
    gate.configureFromLaunch(GET_MODULE(EngineLaunchConfig));
    if (!gate.shaderPollEnabled()) return {gate, nullptr};

    // The composition root creates the domain modules. ReloadService then
    // invokes them through the shared runtime-participant boundary.
    (void)GET_MODULE(ShaderLibrary);
    (void)GET_MODULE(PipelineFactory);
    return {gate, &GET_MODULE(watch::ReloadService)};
}

DeletionQueue &resolveFrameDeletionQueue() {
    return GET_MODULE(DeletionQueue);
}

EngineTime &resolveFrameEngineTime() {
    return GET_MODULE(EngineTime);
}

RenderFrameModules resolveRenderFrameModules() {
    auto &rendering_pass_container = GET_MODULE(RenderingPassContainer);
    DebugDraw *debug_draw =
        rendering_pass_container.isFeatureEnabled("debug_draw") ? &GET_MODULE(DebugDraw) : nullptr;
    DebugText *debug_text =
        rendering_pass_container.isFeatureEnabled("debug_text") ? &GET_MODULE(DebugText) : nullptr;
    RenderTiming *render_timing =
        rendering_pass_container.isFeatureEnabled("gpu_timing") ? &GET_MODULE(RenderTiming) : nullptr;
#if PELICAN_WITH_IMGUI
    ImGuiSystem *imgui_system = nullptr;
    invokeImGuiRuntimeCallback(GET_MODULE(EngineLaunchConfig), [&] {
        imgui_system = &GET_MODULE(ImGuiSystem);
    });
#endif
    const bool ui_enabled = rendering_pass_container.isFeatureEnabled("ui");

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
        GET_MODULE(VelocityPassContainer),
        GET_MODULE(PolygonInstanceContainer),
        GET_MODULE(VertBufContainer),
        GET_MODULE(MaterialContainer),
        GET_MODULE(FrameResources),
        GET_MODULE(FullscreenPassRenderer),
        GET_MODULE(FullscreenPassContainer),
        ui_enabled ? &GET_MODULE(UiRenderer) : nullptr,
        ui_enabled ? &GET_MODULE(UIContainer) : nullptr,
        ui_enabled ? &GET_MODULE(ui::UiModule) : nullptr,
        debug_draw,
        debug_text,
#if PELICAN_WITH_IMGUI
        imgui_system,
#endif
        render_timing,
        GET_MODULE(Camera),
        GET_MODULE(LightContainer),
        resolveSpriteRenderModules(rendering_pass_container),
    };
}

void updateFrameAnimation(LightContainer &light_container, double time) {
    light_container.updateAnimation(static_cast<float>(time));
    light_container.update();
}

FrameUniformData updateFrameResources(RenderFrameModules &modules, EngineTime &engine_time,
                                      vk::Extent2D extent,
                                      const glm::mat4 &previous_view,
                                      const glm::mat4 &previous_projection) {
    const auto frame_index = engine_time.frameIndex();
    const auto inverse_width = extent.width == 0 ? 0.0f : 1.0f / static_cast<float>(extent.width);
    const auto inverse_height = extent.height == 0 ? 0.0f : 1.0f / static_cast<float>(extent.height);

    FrameUniformData data;
    data.time_delta = glm::vec4{static_cast<float>(engine_time.now()),
                                static_cast<float>(engine_time.dt()), 0.0f, 0.0f};
    data.frame_index = glm::uvec4{static_cast<uint32_t>(frame_index),
                                  static_cast<uint32_t>(frame_index >> 32), 0u, 0u};
    data.resolution = glm::vec4{static_cast<float>(extent.width), static_cast<float>(extent.height),
                                inverse_width, inverse_height};
    data.camera_position = glm::vec4{modules.camera.getPos(), 1.0f};
    data.view = modules.camera.getViewMatrix();
    data.projection = modules.camera.getProjectionMatrix();
    data.previous_view = previous_view;
    data.previous_projection = previous_projection;

    modules.frame_resources.setSceneBuffers(modules.instance_container.getObjectBuf(),
                                            modules.instance_container.getPreviousObjectBuf(),
                                            modules.light_container.lightBuffer());
    modules.frame_resources.update(data);
    return data;
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
    case vk::ImageLayout::eTransferDstOptimal:
        return "transfer_dst_optimal";
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
                              const RenderTargetLayoutTracker &layout_tracker,
                              const RenderTargetContainer &rt_container,
                              bool history_read = false) {
    return layoutName(isConcreteRenderTarget(rt_id)
                          ? layout_tracker.currentLayout(rt_id, history_read, &rt_container)
                          : special_layout);
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
        const auto format = isConcreteRenderTarget(target)
                                ? rt_container.getMetadata(target).format
                                : vk::Format::eUndefined;
        nlohmann::json attachment{
            {"resource", renderTargetName(target, rt_container)},
            {"aspect", "color"},
            {"load", loadOpName(definition.color_load_op)},
            {"store", storeOpName(definition.color_store_op)},
            {"final_layout", trackedLayoutName(target, vk::ImageLayout::eColorAttachmentOptimal,
                                                 layout_tracker, rt_container)},
            {"format", isConcreteRenderTarget(target) ? formatToString(format) : "frame_target"},
            {"samples", 1},
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
                                                 layout_tracker, rt_container)},
        };
        if (definition.depth_load_op == vk::AttachmentLoadOp::eClear) {
            attachment["clear"] = 1.0;
        }
        attachments.push_back(std::move(attachment));
    }

    nlohmann::json inputs = nlohmann::json::array();
    for (size_t i = 0; i < definition.input_targets.size(); ++i) {
        const auto target = definition.input_targets[i];
        const bool history_read = definition.input_target_history.at(i);
        inputs.push_back({
            {"resource", renderTargetName(target, rt_container) + (history_read ? "@history" : "")},
            {"final_layout", trackedLayoutName(target, vk::ImageLayout::eShaderReadOnlyOptimal,
                                                 layout_tracker, rt_container, history_read)},
        });
    }

    return nlohmann::json{
        {"name", definition.name},
        {"kind", "render"},
        {"order", order},
        {"inputs", std::move(inputs)},
        {"input_buffers", definition.input_buffers},
        {"attachments", std::move(attachments)},
        {"blend", definition.isUi() || definition.isDebugDraw() || definition.isDebugText()
#if PELICAN_WITH_IMGUI
                      || definition.isImGui()
#endif
        },
    };
}

nlohmann::json anchorNodeTrace(const std::string &name, size_t order) {
    return nlohmann::json{{"name", name}, {"kind", "anchor"}, {"order", order}};
}

std::size_t formatTexelBytes(vk::Format format) {
    switch (format) {
    case vk::Format::eR8Unorm: return 1;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb: return 4;
    case vk::Format::eR16G16Sfloat: return 4;
    case vk::Format::eR16G16B16A16Sfloat: return 8;
    default: return 0;
    }
}

nlohmann::json snapshotCopyTrace(const FramePlanNode &node, const RenderTargetMetadata &source,
                                 const RenderTargetMetadata &destination) {
    return nlohmann::json{
        {"name", node.name},
        {"kind", "snapshot_copy"},
        {"order", node.order},
        {"snapshot_after", node.snapshot_after},
        {"source", source.name},
        {"destination", destination.name},
        {"format", formatToString(destination.format)},
        {"extent", {destination.extent.width, destination.extent.height}},
        {"texel_block_bytes", formatTexelBytes(destination.format)},
        {"byte_size", static_cast<std::size_t>(destination.extent.width) * destination.extent.height *
                          formatTexelBytes(destination.format)},
        {"semantics", "fixed_once_before_transparency"},
        {"sequential_refraction", false},
        {"transitions", nlohmann::json::array({
            {{"resource", source.name}, {"new_layout", "transfer_src_optimal"}},
            {{"resource", destination.name}, {"new_layout", "transfer_dst_optimal"}},
            {{"resource", destination.name}, {"new_layout", "shader_read_only_optimal"}},
        })},
    };
}

nlohmann::json outputTransformTrace(size_t order, vk::ImageLayout source_old_layout,
                                    vk::ImageLayout destination_final_layout,
                                    const RenderTargetMetadata &display,
                                    vk::Format destination_format,
                                    const std::vector<std::string> &paired_storage_edges) {
    const bool fallback = destination_format == vk::Format::eR8G8B8A8Unorm ||
                          destination_format == vk::Format::eB8G8R8A8Unorm;
    return nlohmann::json{
        {"name", "output_transform"},
        {"kind", "output_transform"},
        {"order", order},
        {"mode", fallback ? "shader_oetf_unorm_fallback" : "hardware_srgb_encode"},
        {"source", "display"},
        {"destination", "swapchain"},
        {"source_format", formatToString(display.format)},
        {"destination_format", formatToString(destination_format)},
        {"extent", {display.extent.width, display.extent.height}},
        {"samples", 1},
        {"texel_block_bytes", 4},
        {"color_counters", {{"tone_curve", 1},
                            {"terminal_display_encode", 1},
                            {"shader_oetf", fallback ? 1 : 0},
                            {"paired_storage_round_trip", paired_storage_edges.size()}}},
        {"paired_storage_edges", paired_storage_edges},
        {"queue_ownership", "same_family_or_concurrent; VK_QUEUE_FAMILY_IGNORED"},
        {"transitions",
         nlohmann::json::array({
             {{"resource", "display"},
              {"old_layout", layoutName(source_old_layout)},
              {"new_layout", "shader_read_only_optimal"},
              {"src_stage", "color_attachment_output"},
              {"src_access", "color_attachment_read|color_attachment_write"},
              {"dst_stage", "fragment_shader"},
              {"dst_access", "shader_read"}},
             {{"resource", "swapchain"},
              {"old_layout", "color_attachment_optimal"},
              {"new_layout", "color_attachment_optimal"},
              {"src_stage", "color_attachment_output"},
              {"src_access", "color_attachment_read|color_attachment_write"},
              {"dst_stage", "color_attachment_output"},
              {"dst_access", "color_attachment_write"}},
             {{"resource", "swapchain"},
              {"old_layout", "color_attachment_optimal"},
              {"new_layout", layoutName(destination_final_layout)},
              {"src_stage", "color_attachment_output"},
              {"src_access", "color_attachment_write"},
              {"dst_stage", destination_final_layout == vk::ImageLayout::ePresentSrcKHR
                                    ? "bottom_of_pipe"
                                    : "transfer"},
              {"dst_access", destination_final_layout == vk::ImageLayout::ePresentSrcKHR
                                     ? "none"
                                     : "transfer_read"}},
         })},
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
                entry["final_layout"] = layoutName(layout_tracker.currentLayout(target, false, &rt_container));
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
            layouts[renderTargetName(target, rt_container)] =
                layoutName(layout_tracker.currentLayout(target, false, &rt_container));
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
    const auto display = rt_container.getRenderTargetIdByName("display");
    if (isConcreteRenderTarget(display)) {
        add_target(display);
    }
    layouts["swapchain"] = layoutName(frame_target_layout);

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

std::vector<std::string> pairedSrgbStorageEdges(
    const CompiledRenderingPass &rendering_pass,
    const CompiledFrameGraphExecution &frame_graph,
    const RenderTargetContainer &rt_container) {
    std::vector<std::string> edges;
    for (size_t to = 0; to < frame_graph.nodes.size(); ++to) {
        const auto &node = frame_graph.nodes[to];
        for (const auto &barrier : node.incoming_barriers) {
            const auto target = rt_container.getRenderTargetIdByName(barrier.resource);
            if (!isConcreteRenderTarget(target)) {
                continue;
            }
            const auto format = rt_container.getMetadata(target).format;
            if (format != vk::Format::eR8G8B8A8Srgb && format != vk::Format::eB8G8R8A8Srgb) {
                continue;
            }
            bool sampled = node.kind == FramePlanNodeKind::output_transform && barrier.resource == "display";
            if (node.kind == FramePlanNodeKind::render) {
                const auto &pass = rendering_pass.passes.at(node.index).definition;
                sampled = std::find_if(pass.input_targets.begin(), pass.input_targets.end(),
                                       [&](GlobalRenderTargetId id) { return id == target; }) !=
                          pass.input_targets.end();
            }
            if (sampled) {
                edges.push_back(frame_graph.nodes[barrier.from_node_index].name + ":" + barrier.resource +
                                "->" + node.name);
            }
        }
    }
    return edges;
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
    const auto paired_storage_edges = pairedSrgbStorageEdges(
        rendering_pass, frame_graph, modules.render_target_container);

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
        } else if (execution_node.kind == FramePlanNodeKind::compute) {
            const auto &task = rendering_pass.compute_tasks.at(execution_node.index);
            modules.compute_task_container.transitionResourcesForDispatch(
                render_ctx.cmd_buf, task.task_id, modules.render_target_container, modules.vk_utils,
                layout_tracker);
            modules.compute_task_container.dispatch(render_ctx.cmd_buf, task.task_id);
            if (node_trace != nullptr) {
                node_trace->push_back(computeNodeTrace(task, node_index, modules.render_target_container,
                                                       layout_tracker));
            }
        } else if (execution_node.kind == FramePlanNodeKind::anchor) {
            if (node_trace != nullptr) {
                node_trace->push_back(anchorNodeTrace(execution_node.name, node_index));
            }
            if (execution_node.name == "__anchor_sprite" && modules.sprite.scene != nullptr) {
                GlobalRenderTargetId color_id = noRenderTargetId();
                GlobalRenderTargetId depth_id = noRenderTargetId();
                for (std::size_t previous = node_index; previous-- > 0;) {
                    const auto &candidate = frame_graph.nodes[previous];
                    if (candidate.kind != FramePlanNodeKind::render) continue;
                    const auto &definition = rendering_pass.passes.at(candidate.index).definition;
                    if (!isConcreteRenderTarget(color_id) && !isSwapchainRenderTarget(color_id) &&
                        !definition.output_color.empty()) color_id = definition.output_color.front();
                    if (!isConcreteRenderTarget(depth_id) && isConcreteRenderTarget(definition.output_depth))
                        depth_id = definition.output_depth;
                    if ((isConcreteRenderTarget(color_id) || isSwapchainRenderTarget(color_id)) &&
                        isConcreteRenderTarget(depth_id)) break;
                }
                if ((!isConcreteRenderTarget(color_id) && !isSwapchainRenderTarget(color_id)) ||
                    !isConcreteRenderTarget(depth_id))
                    throw std::runtime_error("sprite feature requires a scene color and depth attachment before sprite anchor");
                PassDefinition sprite_attachments;
                sprite_attachments.output_color = {color_id};
                sprite_attachments.output_depth = depth_id;
                transitionPassOutputsToAttachmentLayouts(render_ctx.cmd_buf, sprite_attachments,
                                                          modules.render_target_container, modules.vk_utils,
                                                          layout_tracker);
                const auto color_view = isSwapchainRenderTarget(color_id)
                                            ? render_ctx.color_attachment
                                            : modules.render_target_container.getImageView(color_id);
                const auto &depth_meta = modules.render_target_container.getMetadata(depth_id);
                const auto color_format = isSwapchainRenderTarget(color_id)
                                              ? modules.render_target.getSwapchainFormat()
                                              : modules.render_target_container.getMetadata(color_id).format;
                const auto extent = isSwapchainRenderTarget(color_id)
                                        ? render_ctx.extent
                                        : modules.render_target_container.getMetadata(color_id).extent;
                if (depth_meta.extent != extent)
                    throw std::runtime_error("sprite color and depth attachments must have matching extents");
                modules.sprite.renderer->render(
                    render_ctx.cmd_buf,
                    SpriteDrawRequest{color_view, modules.render_target_container.getImageView(depth_id), extent,
                                      color_format, depth_meta.format},
                    SpriteRendererDependencies{*modules.sprite.scene, *modules.sprite.atlas,
                                               modules.frame_resources});
                if (node_trace != nullptr && isConcreteRenderTarget(color_id)) {
                    node_trace->back()["sprite_draw"] = {
                        {"color", modules.render_target_container.getMetadata(color_id).name},
                        {"depth", depth_meta.name}, {"depth_test", true}, {"depth_write", false},
                        {"blend", "straight_alpha"},
                        {"policy", modules.sprite.scene->statusJson()},
                    };
                }
            }
        } else if (execution_node.kind == FramePlanNodeKind::snapshot_copy) {
            const auto &planned_node = frame_graph.plan.nodes.at(node_index);
            if (planned_node.reads.size() != 1 || planned_node.writes.size() != 1) {
                throw std::runtime_error("snapshot copy node requires exactly one source and destination: " +
                                         execution_node.name);
            }
            const auto source_id = modules.render_target_container.getRenderTargetIdByName(
                planned_node.reads.front());
            const auto destination_id = modules.render_target_container.getRenderTargetIdByName(
                planned_node.writes.front());
            if (!isConcreteRenderTarget(source_id) || !isConcreteRenderTarget(destination_id)) {
                throw std::runtime_error("snapshot copy node references an unknown render target: " +
                                         execution_node.name);
            }
            const auto source = modules.render_target_container.getMetadata(source_id);
            const auto destination = modules.render_target_container.getMetadata(destination_id);
            if (source.format != destination.format || source.extent != destination.extent) {
                throw std::runtime_error("snapshot copy source/destination mismatch: " +
                                         execution_node.name);
            }
            layout_tracker.transition(render_ctx.cmd_buf, modules.render_target_container,
                                      modules.vk_utils, source_id,
                                      vk::ImageLayout::eTransferSrcOptimal);
            layout_tracker.transition(render_ctx.cmd_buf, modules.render_target_container,
                                      modules.vk_utils, destination_id,
                                      vk::ImageLayout::eTransferDstOptimal);
            vk::ImageCopy copy;
            copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            copy.extent = vk::Extent3D{source.extent.width, source.extent.height, 1};
            render_ctx.cmd_buf.copyImage(modules.render_target_container.getImage(source_id).image.get(),
                                         vk::ImageLayout::eTransferSrcOptimal,
                                         modules.render_target_container.getImage(destination_id).image.get(),
                                         vk::ImageLayout::eTransferDstOptimal, copy);
            layout_tracker.transition(render_ctx.cmd_buf, modules.render_target_container,
                                      modules.vk_utils, destination_id,
                                      vk::ImageLayout::eShaderReadOnlyOptimal);
            if (node_trace != nullptr) {
                node_trace->push_back(snapshotCopyTrace(planned_node, source, destination));
            }
        } else if (execution_node.kind == FramePlanNodeKind::output_transform) {
            const auto display_id = modules.render_target_container.getRenderTargetIdByName("display");
            if (!isConcreteRenderTarget(display_id)) {
                throw std::runtime_error("output_transform requires the canonical display target");
            }
            const auto display = modules.render_target_container.getMetadata(display_id);
            const auto source_old_layout = layout_tracker.currentLayout(display_id, false,
                                                                         &modules.render_target_container);
            const auto output_pass = std::find_if(
                rendering_pass.passes.begin(), rendering_pass.passes.end(),
                [](const CompiledPass &pass) { return pass.definition.name == "output_transform"; });
            if (output_pass == rendering_pass.passes.end()) {
                throw std::runtime_error("output_transform fullscreen pass was not compiled");
            }
            modules.pass_executor.execute(render_ctx, *output_pass,
                                          pass_executor_dependencies, layout_tracker);
            if (node_trace != nullptr) {
                node_trace->push_back(outputTransformTrace(node_index, source_old_layout,
                                                           render_ctx.required_layout, display,
                                                           modules.render_target.getSwapchainFormat(),
                                                           paired_storage_edges));
            }
        } else {
            throw std::runtime_error("Unsupported frame graph execution node: " + execution_node.name);
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
                                                                      modules.frame_resources,
                                                                      modules.light_container,
                                                                      modules.camera};
    const FullscreenPassRendererDependencies fullscreen_pass_renderer_dependencies{modules.fullscreen_pass_container,
                                                                                  modules.frame_resources};
    std::optional<UiRendererDependencies> ui_renderer_dependencies;
    if (modules.ui_renderer != nullptr && modules.ui_container != nullptr && modules.ui_module != nullptr)
        ui_renderer_dependencies.emplace(*modules.ui_container, *modules.ui_module, modules.frame_resources);
    const RenderPassDispatchDependencies pass_dispatch_dependencies{
        modules.material_renderer,
        material_renderer_dependencies,
        modules.fullscreen_pass_renderer,
        fullscreen_pass_renderer_dependencies,
        modules.shadow_depth_pass_container,
        modules.velocity_pass_container,
        modules.ui_renderer,
        ui_renderer_dependencies ? &*ui_renderer_dependencies : nullptr,
        modules.debug_draw,
        modules.debug_text,
#if PELICAN_WITH_IMGUI
        modules.imgui_system,
#endif
        modules.frame_resources,
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
                                                                    pass.definition.input_target_history,
                                                                    pass.definition.input_buffers, rt_views,
                                                                    modules.frame_graph_resources);
            }
        }
    }
}

bool reloadModifiedShaderSources(ShaderHotReloadModules &modules) {
    return modules.gate.shaderPollEnabled() && modules.reload_service != nullptr &&
           modules.reload_service
                   ->applyRuntimeBoundary(watch::RuntimeReloadBoundary::render_start)
                   .committed != 0;
}

bool handleFrameTargetResize(RenderFrameModules &modules, RenderTargetLayoutTracker &layout_tracker) {
    if (!modules.render_target.consumeExtentChanged()) {
        return false;
    }

    modules.render_target_container.recreateForExtent(modules.render_target.getExtent());
    rebindFullscreenInputs(modules);
    layout_tracker.reset();
    return true;
}

} // namespace

Renderer::Renderer() {
    current_rendering_pass_id = loadDefaultRenderingPassFromConfig();
}

Renderer::~Renderer() = default;

nlohmann::json Renderer::currentFramePlanJson() const {
    const auto *frame_graph_runtime = FastModuleContainer::tryGet<FrameGraphRuntimeContainer>();
    const auto *frame_graph = frame_graph_runtime == nullptr
                                  ? nullptr
                                  : frame_graph_runtime->find(current_rendering_pass_id);
    if (frame_graph == nullptr) {
        throw std::runtime_error("Current frame plan is not registered");
    }
    auto result = framePlanToJson(frame_graph->plan);
    const auto *sprite_scene = FastModuleContainer::tryGet<SpriteScene>();
    result["sprite"] = sprite_scene != nullptr ? sprite_scene->statusJson()
                                                 : nlohmann::json{{"enabled", false}};
    return result;
}

std::vector<std::string> Renderer::currentFramePlanOrderForTesting() const {
    const auto *frame_graph_runtime = FastModuleContainer::tryGet<FrameGraphRuntimeContainer>();
    const auto *frame_graph = frame_graph_runtime == nullptr
                                  ? nullptr
                                  : frame_graph_runtime->find(current_rendering_pass_id);
    if (frame_graph == nullptr) {
        throw std::runtime_error("Current frame plan is not registered");
    }
    return framePlanOrder(frame_graph->plan);
}

void Renderer::recreateRenderTargetsAndRebindForTesting(vk::Extent2D extent) {
    auto modules = resolveRenderFrameModules();
    modules.render_target_container.recreateForExtent(extent);
    rebindFullscreenInputs(modules);
    modules.instance_container.resetTemporalHistory();
    camera_history_valid = false;
    render_target_layout_tracker.reset();
}

void Renderer::resetTemporalHistory() {
    auto modules = resolveRenderFrameModules();
    modules.render_target_container.resetHistory();
    modules.instance_container.resetTemporalHistory();
    camera_history_valid = false;
    render_target_layout_tracker.reset();
}

void Renderer::prepareRuntimeModules() {
    auto &rendering_passes = GET_MODULE(RenderingPassContainer);
    if (rendering_passes.isFeatureEnabled("sprite")) {
        (void)GET_MODULE(SpriteScene);
        (void)GET_MODULE(SpriteRenderer);
        (void)GET_MODULE(AtlasAssetResource);
    }

    (void)resolveFrameDeletionQueue();
    (void)resolveRenderFrameModules();
    (void)resolveShaderHotReloadModules();
    (void)resolveFrameEngineTime();
    (void)GET_MODULE(PipelineFactory);
    // Runtime RPC scene loading may introduce its first standard material after
    // startup, so keep that data lazy within boot but not beyond the freeze.
    (void)GET_MODULE(StandardMaterialResource);
}

void Renderer::render() {
    auto &deletion_queue = resolveFrameDeletionQueue();
    deletion_queue.beginFrame();

    auto modules = resolveRenderFrameModules();
    auto shader_hot_reload = resolveShaderHotReloadModules();
    if (reloadModifiedShaderSources(shader_hot_reload)) {
        rebindFullscreenInputs(modules);
    }
    auto &engine_time = resolveFrameEngineTime();
    updateFrameAnimation(modules.light_container, engine_time.now());

    const auto render_ctx = modules.render_target.render_begin();
    if (handleFrameTargetResize(modules, render_target_layout_tracker)) {
        modules.instance_container.resetTemporalHistory();
        camera_history_valid = false;
    }
    const auto current_view = modules.camera.getViewMatrix();
    const auto current_projection = modules.camera.getProjectionMatrix();
    if (!camera_history_valid) {
        previous_view = current_view;
        previous_projection = current_projection;
    }
    updateFrameResources(modules, engine_time, render_ctx.extent, previous_view,
                         previous_projection);

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
    modules.render_target_container.advanceHistoryFrame();
    modules.instance_container.advanceTemporalHistoryAfterRender();
    previous_view = current_view;
    previous_projection = current_projection;
    camera_history_valid = true;
}

} // namespace Pelican
