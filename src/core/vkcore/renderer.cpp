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
#include "core.hpp"
#include "debugutils.hpp"
#include "render_pass_dispatch.hpp"
#include "render_pass_executor.hpp"
#include "render_pass_frame_setup.hpp"
#include "renderer_config.hpp"
#include "rendertarget.hpp"
#include "rendertiming.hpp"
#include "util.hpp"
#if PELICAN_WITH_OPENXR
#include "../openxr/openxrmirrorsink.hpp"
#endif
#if PELICAN_WITH_IMGUI
#include "../imgui/imguiruntime.hpp"
#include "../imgui/imguisystem.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_inverse.hpp>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>

namespace Pelican {

namespace {

struct SpriteRenderModules {
    SpriteScene *scene = nullptr;
    SpriteRenderer *renderer = nullptr;
    AtlasAssetResource *atlas = nullptr;
};

struct ShaderHotReloadModules {
    watch::ReloadService *reload_service = nullptr;
};

struct RenderFrameModules {
    const DebugUtilsDispatch &debug_utils;
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
    // direct renderer fixtures and legacy embedders on the centralized gate.
    gate.configureFromLaunch(GET_MODULE(EngineLaunchConfig));
    if (!gate.shaderReloadEnabled()) {
        // A publication committed at frame start still has to be consumed if
        // a dynamic determinism gate closes before render start.
        return {FastModuleContainer::tryGet<watch::ReloadService>()};
    }

    // The composition root creates the domain modules. ReloadService then
    // invokes them through the shared runtime-participant boundary.
    (void)GET_MODULE(ShaderLibrary);
    (void)GET_MODULE(PipelineFactory);
    return {&GET_MODULE(watch::ReloadService)};
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
        GET_MODULE(VulkanManageCore).getDebugUtils(),
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

void updateFrameLights(LightContainer &light_container) {
    light_container.update();
}

FrameUniformData updateFrameResources(RenderFrameModules &modules, EngineTime &engine_time,
                                      vk::Extent2D extent,
                                      const RenderFrameSnapshot &snapshot) {
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
    data.camera_position = glm::vec4{snapshot.camera_position, 1.0f};
    data.view = snapshot.view;
    data.projection = snapshot.projection_jittered;
    data.previous_view = snapshot.previous_view;
    data.previous_projection = snapshot.previous_projection_jittered;
    data.jitter_ndc = snapshot.jitter_ndc;
    data.previous_jitter_ndc = snapshot.previous_jitter_ndc;
    data.temporal_reset_epoch = snapshot.temporal_reset_epoch;
    data.previous_temporal_reset_epoch = snapshot.previous_temporal_reset_epoch;

    modules.frame_resources.setSceneBuffers(modules.instance_container.getObjectBuf(),
                                            modules.instance_container.getPreviousObjectBuf(),
                                            modules.light_container.lightBuffer());
    modules.frame_resources.update(data);
    return data;
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
    case vk::Format::eR16Sfloat:
    case vk::Format::eD16Unorm: return 2;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb: return 4;
    case vk::Format::eR16G16Sfloat:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32Sfloat: return 4;
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eD32SfloatS8Uint: return 8;
    default: return 0;
    }
}

vk::ImageAspectFlags snapshotAspect(vk::Format format) {
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD32SfloatS8Uint:
        return vk::ImageAspectFlagBits::eDepth;
    default:
        return vk::ImageAspectFlagBits::eColor;
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
        {"aspect", snapshotAspect(destination.format) ==
                           vk::ImageAspectFlagBits::eDepth
                       ? "depth"
                       : "color"},
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

std::vector<GpuTimingNodeDescriptor> plannedTimingNodes(
    const CompiledFrameGraphExecution &frame_graph, const SpriteRenderModules &sprite) {
    std::vector<GpuTimingNodeDescriptor> nodes;
    nodes.reserve(frame_graph.nodes.size());
    for (std::size_t ordinal = 0; ordinal < frame_graph.nodes.size(); ++ordinal) {
        const auto &node = frame_graph.nodes[ordinal];
        const bool anchor_has_work = node.kind != FramePlanNodeKind::anchor ||
                                     (node.name == "__anchor_sprite" &&
                                      sprite.scene != nullptr);
        nodes.push_back(GpuTimingNodeDescriptor{
            ordinal, std::string{framePlanNodeKindName(node.kind)}, node.name,
            anchor_has_work});
    }
    return nodes;
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
                              vk::Format frame_target_format,
                              RenderTargetLayoutTracker &layout_tracker,
                              const RenderPassExecutorDependencies &pass_executor_dependencies,
                              nlohmann::json *node_trace,
                              std::uint64_t logical_frame,
                              std::string_view graph_variant,
                              std::uint32_t view_index) {
    if (modules.render_timing != nullptr) {
        modules.render_timing->beginGpuRange(
            render_ctx.cmd_buf, render_ctx.in_flight_frame_index, view_index,
            GpuTimingRangeIdentity{logical_frame, std::string{graph_variant}, view_index},
            plannedTimingNodes(frame_graph, modules.sprite));
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

        std::string node_debug_name;
        if (modules.debug_utils.commandLabelsEnabled()) {
            node_debug_name = makeFrameGraphDebugLabel(FrameGraphDebugLabelIdentity{
                logical_frame, graph_variant, view_index, node_index,
                framePlanNodeKindName(execution_node.kind), execution_node.name});
        }
        ScopedCommandDebugLabel node_label{modules.debug_utils, render_ctx.cmd_buf,
                                           node_debug_name.c_str()};
        if (modules.render_timing != nullptr) {
            modules.render_timing->writeNodeSubrangeStart(
                render_ctx.cmd_buf, node_index, GpuTimingSubrange::barriers);
        }
        {
            ScopedCommandDebugLabel barrier_label{modules.debug_utils, render_ctx.cmd_buf,
                                                   "barriers"};
            for (const auto &barrier : execution_node.incoming_barriers) {
                if (barrier.from_node_index >= node_index) {
                    throw std::runtime_error(
                        "Compiled frame graph barrier source was not executed before target");
                }
                if (modules.frame_graph_resources.hasBuffer(barrier.resource)) {
                    modules.compute_task_container.bufferReadAfterWriteBarrier(
                        render_ctx.cmd_buf, barrier.resource, barrier.from_kind,
                        barrier.to_kind);
                    continue;
                }
                if (barrier.resource == "swapchain") {
                    if (!render_ctx.color_image) {
                        throw std::runtime_error(
                            "Compiled frame graph swapchain barrier has no frame image");
                    }
                    vk::ImageMemoryBarrier image_barrier;
                    image_barrier.srcAccessMask =
                        vk::AccessFlagBits::eColorAttachmentRead |
                        vk::AccessFlagBits::eColorAttachmentWrite;
                    image_barrier.dstAccessMask =
                        vk::AccessFlagBits::eColorAttachmentRead |
                        vk::AccessFlagBits::eColorAttachmentWrite;
                    image_barrier.oldLayout =
                        vk::ImageLayout::eColorAttachmentOptimal;
                    image_barrier.newLayout =
                        vk::ImageLayout::eColorAttachmentOptimal;
                    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    image_barrier.image = render_ctx.color_image;
                    image_barrier.subresourceRange = {
                        vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
                    render_ctx.cmd_buf.pipelineBarrier(
                        vk::PipelineStageFlagBits::eColorAttachmentOutput,
                        vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {},
                        {image_barrier});
                    continue;
                }
                const auto target_id = modules.render_target_container
                                           .getRenderTargetIdByName(barrier.resource);
                if (!isConcreteRenderTarget(target_id)) {
                    throw std::runtime_error(
                        "Compiled frame graph barrier references an unknown resource: " +
                        barrier.resource);
                }
                layout_tracker.memoryDependency(
                    render_ctx.cmd_buf, modules.render_target_container,
                    modules.vk_utils, target_id);
            }
        }
        if (modules.render_timing != nullptr) {
            modules.render_timing->writeNodeSubrangeEnd(
                render_ctx.cmd_buf, node_index, GpuTimingSubrange::barriers);
        }
        ScopedCommandDebugLabel body_label{modules.debug_utils, render_ctx.cmd_buf, "body"};

        if (modules.render_timing != nullptr) {
            modules.render_timing->writeNodeSubrangeStart(
                render_ctx.cmd_buf, node_index, GpuTimingSubrange::body);
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
                                            : modules.render_target_container
                                                  .getAttachmentImageView(color_id);
                const auto &depth_meta = modules.render_target_container.getMetadata(depth_id);
                const auto color_format = isSwapchainRenderTarget(color_id)
                                              ? frame_target_format
                                              : modules.render_target_container.getMetadata(color_id).format;
                const auto extent = isSwapchainRenderTarget(color_id)
                                        ? render_ctx.extent
                                        : modules.render_target_container.getMetadata(color_id).extent;
                if (depth_meta.extent != extent)
                    throw std::runtime_error("sprite color and depth attachments must have matching extents");
                modules.sprite.renderer->render(
                    render_ctx.cmd_buf,
                    SpriteDrawRequest{
                        .color_view = color_view,
                        .depth_view =
                            modules.render_target_container
                                .getAttachmentImageView(depth_id),
                        .extent = extent,
                        .color_format = color_format,
                        .depth_format = depth_meta.format,
                        .color_resolve_view =
                            isConcreteRenderTarget(color_id) &&
                                    modules.render_target_container
                                        .hasSeparateAttachment(color_id)
                                ? modules.render_target_container
                                      .getImageView(color_id)
                                : vk::ImageView{},
                        .depth_resolve_view =
                            modules.render_target_container
                                    .hasSeparateAttachment(depth_id)
                                ? modules.render_target_container
                                      .getImageView(depth_id)
                                : vk::ImageView{},
                        .samples =
                            isConcreteRenderTarget(color_id)
                                ? modules.render_target_container
                                      .sampleCount(color_id)
                                : vk::SampleCountFlagBits::e1,
                        .color_resolve_mode =
                            isConcreteRenderTarget(color_id)
                                ? modules.render_target_container
                                      .resolveMode(color_id)
                                : vk::ResolveModeFlagBits::eNone,
                        .depth_resolve_mode =
                            modules.render_target_container
                                .resolveMode(depth_id),
                    },
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
            const auto aspect = snapshotAspect(source.format);
            copy.srcSubresource = {aspect, 0, 0, 1};
            copy.dstSubresource = {aspect, 0, 0, 1};
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
                                                            frame_target_format,
                                                           paired_storage_edges));
            }
        } else {
            throw std::runtime_error("Unsupported frame graph execution node: " + execution_node.name);
        }

        if (modules.render_timing != nullptr) {
            modules.render_timing->writeNodeSubrangeEnd(
                render_ctx.cmd_buf, node_index, GpuTimingSubrange::body);
        }
    }

    if (modules.render_timing != nullptr) modules.render_timing->endGpuRange();
}

void executeRenderingPasses(const FrameRenderContext &render_ctx,
                            const CompiledRenderingPass &rendering_pass,
                            const CompiledFrameGraphExecution &frame_graph,
                            RenderFrameModules &modules,
                            const RenderFrameSnapshot &snapshot,
                            bool first_person_view,
                            vk::Format frame_target_format,
                            RenderTargetLayoutTracker &layout_tracker,
                            nlohmann::json *node_trace,
                            std::uint64_t logical_frame,
                            std::string_view graph_variant,
                            std::uint32_t view_index,
                            std::uint32_t draw_sort_view_index) {
    const MaterialRendererDependencies material_renderer_dependencies{modules.instance_container,
                                                                      modules.vert_buf_container,
                                                                      modules.material_container,
                                                                      modules.frame_resources,
                                                                      modules.light_container,
                                                                      modules.camera,
                                                                      snapshot.view_projection_jittered,
                                                                      first_person_view,
                                                                      draw_sort_view_index};
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
        snapshot.view_projection_jittered,
        frame_target_format};
    const RenderPassExecutorDependencies pass_executor_dependencies{modules.render_target_container, modules.vk_utils,
                                                                    pass_dispatch_dependencies};

    executePlannedFrameGraph(render_ctx, rendering_pass, frame_graph, modules,
                             frame_target_format, layout_tracker,
                             pass_executor_dependencies, node_trace, logical_frame,
                             graph_variant, view_index);
}

void rebindFullscreenInputs(RenderFrameModules &modules) {
    RenderTargetImageViewResolver rt_views{modules.render_target_container};
    const auto generation =
        modules.frame_graph_runtime.snapshot();
    if (generation == nullptr) {
        throw std::runtime_error(
            "Fullscreen input rebind requires a published render pipeline");
    }
    for (const auto pass_id :
         generation->rendering_pass_ids) {
        const auto *program = generation->find(pass_id);
        if (program == nullptr) {
            throw std::logic_error(
                "Published render pipeline pass table is inconsistent");
        }
        const auto &compiled_pass =
            program->rendering_pass;
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
    modules.material_container.rebindScreenInputs(rt_views);
    modules.compute_task_container.rebindRenderTargets(
        modules.render_target_container);
}

bool consumeShaderReloadPublication(ShaderHotReloadModules &modules) {
    return modules.reload_service != nullptr &&
           modules.reload_service
                   ->applyRuntimeBoundary(watch::RuntimeReloadBoundary::render_start)
                   .committed != 0;
}

bool handleFrameTargetResize(RenderFrameModules &modules,
                             RenderTargetLayoutTracker &layout_tracker,
                             ILogicalFrameTarget &target, vk::Extent2D extent,
                             bool logical_target_extent_changed) {
    const bool target_reported_change = target.consumeExtentChanged();
    if (!target_reported_change && !logical_target_extent_changed) {
        return false;
    }

    modules.render_target_container.recreateForExtent(extent);
    rebindFullscreenInputs(modules);
    layout_tracker.reset();
    return true;
}

#if PELICAN_WITH_OPENXR
void recordXrMirrorIntermediate(const FrameRenderContext &render_ctx,
                                RenderFrameModules &modules,
                                RenderTargetLayoutTracker &layout_tracker,
                                nlohmann::json *node_trace,
                                std::uint64_t logical_frame) {
    const auto source_id =
        modules.render_target_container.getRenderTargetIdByName("display");
    const auto destination_id = modules.render_target_container.getRenderTargetIdByName(
        std::string{OpenXr::xr_mirror_intermediate_name});
    if (!isConcreteRenderTarget(source_id) || !isConcreteRenderTarget(destination_id)) {
        throw std::runtime_error(
            "OpenXR mirror requires engine-owned display and left-eye intermediates");
    }
    const auto source = modules.render_target_container.getMetadata(source_id);
    const auto destination = modules.render_target_container.getMetadata(destination_id);
    if (source.format != destination.format || source.extent != destination.extent) {
        throw std::runtime_error(
            "OpenXR mirror intermediate must match the engine-owned display source "
            "(source=" + vk::to_string(source.format) + " " +
            std::to_string(source.extent.width) + "x" +
            std::to_string(source.extent.height) + ", destination=" +
            vk::to_string(destination.format) + " " +
            std::to_string(destination.extent.width) + "x" +
            std::to_string(destination.extent.height) + ")");
    }
    constexpr std::uint32_t mirror_view_index = 2;
    constexpr std::uint32_t mirror_intermediate_range_slot = 2;
    if (modules.render_timing != nullptr) {
        modules.render_timing->beginGpuRange(
            render_ctx.cmd_buf, render_ctx.in_flight_frame_index,
            mirror_intermediate_range_slot,
            GpuTimingRangeIdentity{logical_frame, "xr", mirror_view_index},
            {GpuTimingNodeDescriptor{0, "mirror", "intermediate_copy", true}});
    }
    std::string node_debug_name;
    if (modules.debug_utils.commandLabelsEnabled()) {
        node_debug_name = makeFrameGraphDebugLabel(FrameGraphDebugLabelIdentity{
            logical_frame, "xr", mirror_view_index, 0, "mirror", "intermediate_copy"});
    }
    ScopedCommandDebugLabel node_label{modules.debug_utils, render_ctx.cmd_buf,
                                       node_debug_name.c_str()};
    if (modules.render_timing != nullptr) {
        modules.render_timing->writeNodeSubrangeStart(
            render_ctx.cmd_buf, 0, GpuTimingSubrange::barriers);
    }
    {
        ScopedCommandDebugLabel barrier_label{modules.debug_utils, render_ctx.cmd_buf,
                                               "barriers"};
        layout_tracker.transition(render_ctx.cmd_buf, modules.render_target_container,
                                  modules.vk_utils, source_id,
                                  vk::ImageLayout::eTransferSrcOptimal);
        layout_tracker.transition(render_ctx.cmd_buf, modules.render_target_container,
                                  modules.vk_utils, destination_id,
                                  vk::ImageLayout::eTransferDstOptimal);
    }
    if (modules.render_timing != nullptr) {
        modules.render_timing->writeNodeSubrangeEnd(
            render_ctx.cmd_buf, 0, GpuTimingSubrange::barriers);
    }
    ScopedCommandDebugLabel body_label{modules.debug_utils, render_ctx.cmd_buf, "body"};
    if (modules.render_timing != nullptr) {
        modules.render_timing->writeNodeSubrangeStart(
            render_ctx.cmd_buf, 0, GpuTimingSubrange::body);
    }
    vk::ImageCopy copy;
    copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    copy.extent = vk::Extent3D{source.extent.width, source.extent.height, 1};
    render_ctx.cmd_buf.copyImage(
        modules.render_target_container.getImage(source_id).image.get(),
        vk::ImageLayout::eTransferSrcOptimal,
        modules.render_target_container.getImage(destination_id).image.get(),
        vk::ImageLayout::eTransferDstOptimal, copy);
    layout_tracker.transition(render_ctx.cmd_buf, modules.render_target_container,
                              modules.vk_utils, destination_id,
                              vk::ImageLayout::eShaderReadOnlyOptimal);
    if (modules.render_timing != nullptr) {
        modules.render_timing->writeNodeSubrangeEnd(
            render_ctx.cmd_buf, 0, GpuTimingSubrange::body);
        modules.render_timing->endGpuRange();
    }
    if (node_trace != nullptr) {
        node_trace->push_back({
            {"name", "xr_mirror_left_intermediate"},
            {"kind", "engine_owned_copy"},
            {"source", "display"},
            {"destination", OpenXr::xr_mirror_intermediate_name},
            {"source_is_xr_image", false},
        });
    }
}
#endif

std::optional<ProjectionJitterSettings>
projectionJitterSettingsFor(
    const CompiledFrameGraphExecution &frame_graph) {
    if (!frame_graph.render_pipeline ||
        !frame_graph.render_pipeline->projection_jitter) {
        return std::nullopt;
    }
    const auto &jitter =
        *frame_graph.render_pipeline->projection_jitter;
    ProjectionJitterSettings settings{
        jitter.provider,
        jitter.pattern,
        jitter.phases,
    };
    settings.offsets_px.reserve(jitter.offsets_px.size());
    for (const auto &offset : jitter.offsets_px) {
        settings.offsets_px.emplace_back(
            static_cast<float>(compiledRenderNumericValueAsDouble(offset[0])),
            static_cast<float>(compiledRenderNumericValueAsDouble(offset[1])));
    }
    return settings;
}

class FlatLogicalFrameTarget final : public ILogicalFrameTarget {
    RenderTarget &target;
    bool view_begun = false;

  public:
    explicit FlatLogicalFrameTarget(RenderTarget &target) : target{target} {}

    void beginLogicalFrame(std::uint32_t view_count) override {
        if (view_count != 1) {
            throw std::runtime_error("flat IFrameTarget requires exactly one logical-frame view");
        }
        view_begun = false;
    }

    FrameRenderContext beginView(std::uint32_t view_index) override {
        if (view_index != 0 || view_begun) {
            throw std::runtime_error("flat IFrameTarget view was begun out of order");
        }
        view_begun = true;
        return target.render_begin();
    }

    void endView(std::uint32_t view_index) override {
        if (view_index != 0 || !view_begun) {
            throw std::runtime_error("flat IFrameTarget view was ended out of order");
        }
    }

    void endLogicalFrame() override {
        if (!view_begun) {
            throw std::runtime_error("flat IFrameTarget logical frame ended without a view");
        }
        target.render_end();
        view_begun = false;
    }

    vk::Format colorFormat(std::uint32_t view_index) const override {
        if (view_index != 0) {
            throw std::runtime_error("flat IFrameTarget color format view is out of range");
        }
        return target.getSwapchainFormat();
    }

    bool consumeExtentChanged() override { return target.consumeExtentChanged(); }
};

} // namespace

Renderer::Renderer() {
    const auto variants = loadRenderGraphVariantsFromConfig();
    flat_rendering_pass_id = variants.flat;
    xr_rendering_pass_id = variants.xr;
    xr_excluded_features = variants.xr_excluded_features;
    preview_graph_program = variants.preview;
    current_rendering_pass_id = flat_rendering_pass_id;
    flat_temporal_histories.resize(1);
    if (xr_rendering_pass_id) xr_temporal_histories.resize(2);
    internal_render_extent = GET_MODULE(RenderTarget).getExtent();
}

Renderer::~Renderer() = default;

nlohmann::ordered_json Renderer::previewIsolationStateJson() const {
    using Json = nlohmann::ordered_json;
    const auto matrix = [](const glm::mat4 &value) {
        auto result = Json::array();
        for (glm::length_t row = 0; row < 4; ++row) {
            auto values = Json::array();
            for (glm::length_t column = 0; column < 4; ++column) {
                values.push_back(value[column][row]);
            }
            result.push_back(std::move(values));
        }
        return result;
    };
    const auto vec3 = [](glm::vec3 value) {
        return Json::array({value.x, value.y, value.z});
    };
    const auto vec2 = [](glm::vec2 value) {
        return Json::array({value.x, value.y});
    };
    const auto history = [&](const TemporalFrameHistory &value) {
        return Json{{"valid", value.valid},
                    {"projection_non_jittered", matrix(value.projection_non_jittered)},
                    {"view_projection_non_jittered", matrix(value.view_projection_non_jittered)},
                    {"projection_jittered", matrix(value.projection_jittered)},
                    {"view_projection_jittered", matrix(value.view_projection_jittered)},
                    {"view", matrix(value.view)},
                    {"camera_position", vec3(value.camera_position)},
                    {"jitter_ndc", vec2(value.jitter_ndc)},
                    {"temporal_reset_epoch", value.temporal_reset_epoch}};
    };
    const auto histories = [&](const std::vector<TemporalFrameHistory> &values) {
        auto result = Json::array();
        for (const auto &value : values) result.push_back(history(value));
        return result;
    };
    auto snapshots = Json::array();
    for (const auto &value : last_view_snapshots) {
        snapshots.push_back({
            {"projection_non_jittered", matrix(value.projection_non_jittered)},
            {"view_projection_non_jittered", matrix(value.view_projection_non_jittered)},
            {"previous_projection_non_jittered", matrix(value.previous_projection_non_jittered)},
            {"previous_view_projection_non_jittered", matrix(value.previous_view_projection_non_jittered)},
            {"projection_jittered", matrix(value.projection_jittered)},
            {"view_projection_jittered", matrix(value.view_projection_jittered)},
            {"previous_projection_jittered", matrix(value.previous_projection_jittered)},
            {"previous_view_projection_jittered", matrix(value.previous_view_projection_jittered)},
            {"view", matrix(value.view)}, {"previous_view", matrix(value.previous_view)},
            {"camera_position", vec3(value.camera_position)},
            {"previous_camera_position", vec3(value.previous_camera_position)},
            {"jitter_ndc", vec2(value.jitter_ndc)},
            {"previous_jitter_ndc", vec2(value.previous_jitter_ndc)},
            {"temporal_reset_epoch", value.temporal_reset_epoch},
            {"previous_temporal_reset_epoch", value.previous_temporal_reset_epoch},
        });
    }
    return {{"current_rendering_pass", current_rendering_pass_id.value},
            {"flat_rendering_pass", flat_rendering_pass_id.value},
            {"xr_rendering_pass", xr_rendering_pass_id
                                       ? Json(xr_rendering_pass_id->value) : Json(nullptr)},
            {"active_graph_variant", active_graph_variant == RenderGraphVariant::flat
                                         ? "flat" : "xr"},
            {"flat_histories", histories(flat_temporal_histories)},
            {"xr_histories", histories(xr_temporal_histories)},
            {"last_view_snapshots", std::move(snapshots)},
            {"temporal_reset_requested", temporal_reset_requested},
            {"observed_time_set_revision", observed_time_set_revision},
            {"observed_camera_discontinuity_revision", observed_camera_discontinuity_revision},
            {"internal_render_extent", internal_render_extent
                 ? Json{{"width", internal_render_extent->width},
                        {"height", internal_render_extent->height}} : Json(nullptr)},
            {"graph_transition_trace", graph_variant_transition_trace},
            {"pending_graph_transition", pending_graph_transition
                 ? Json(*pending_graph_transition) : Json(nullptr)},
            {"preview_graph_generation", preview_graph_program.generation}};
}

std::vector<TemporalFrameHistory> &Renderer::activeTemporalHistories() {
    return active_graph_variant == RenderGraphVariant::flat
               ? flat_temporal_histories
               : xr_temporal_histories;
}

const std::vector<TemporalFrameHistory> &Renderer::activeTemporalHistories() const {
    return active_graph_variant == RenderGraphVariant::flat
               ? flat_temporal_histories
               : xr_temporal_histories;
}

nlohmann::json Renderer::currentFramePlanJson() const {
    const auto *frame_graph_runtime = FastModuleContainer::tryGet<FrameGraphRuntimeContainer>();
    const auto generation =
        frame_graph_runtime != nullptr
            ? frame_graph_runtime->snapshot()
            : nullptr;
    const auto *program =
        generation != nullptr
            ? generation->find(current_rendering_pass_id)
            : nullptr;
    const auto *frame_graph =
        program != nullptr ? &program->frame_graph : nullptr;
    if (frame_graph == nullptr) {
        throw std::runtime_error("Current frame plan is not registered");
    }
    auto result = framePlanToJson(frame_graph->plan,
                                  frame_graph->render_pipeline.get());
    result["runtime_generation"] = generation->generation;
    if (generation->gpu_arena != nullptr) {
        nlohmann::json scopes = nlohmann::json::array();
        for (const auto &scope :
             generation->gpu_arena->scopes) {
            nlohmann::json resources =
                nlohmann::json::array();
            for (const auto &resource : scope.resources) {
                resources.push_back(
                    {{"kind",
                      renderPipelineGpuResourceKindName(
                          resource.kind)},
                     {"handle", resource.handle},
                     {"name", resource.name},
                     {"declared_bytes",
                      resource.declared_bytes}});
            }
            scopes.push_back(
                {{"owner_scope", scope.owner_scope},
                 {"resources", std::move(resources)}});
        }
        result["gpu_resource_arena"] = {
            {"runtime_generation",
             generation->gpu_arena
                 ->runtime_generation},
            {"resource_count",
             generation->gpu_arena->resourceCount()},
            {"scopes", std::move(scopes)},
        };
    }
    if (frame_graph->target_plan != nullptr) {
        result["physical_target_plan"] =
            vulkanTargetPlanToJson(*frame_graph->target_plan);
    }
    if (frame_graph->sample_count_plan != nullptr &&
        frame_graph->render_pipeline->sample_count_policy.authored) {
        result["sample_count_plan"] =
            resolvedSampleCountPlanToJson(*frame_graph->sample_count_plan);
    }
    const auto *sprite_scene = FastModuleContainer::tryGet<SpriteScene>();
    result["sprite"] = sprite_scene != nullptr ? sprite_scene->statusJson()
                                                 : nlohmann::json{{"enabled", false}};
    return result;
}

std::vector<std::string> Renderer::currentFramePlanOrderForTesting() const {
    const auto *frame_graph_runtime = FastModuleContainer::tryGet<FrameGraphRuntimeContainer>();
    const auto generation =
        frame_graph_runtime != nullptr
            ? frame_graph_runtime->snapshot()
            : nullptr;
    const auto *program =
        generation != nullptr
            ? generation->find(current_rendering_pass_id)
            : nullptr;
    const auto *frame_graph =
        program != nullptr ? &program->frame_graph : nullptr;
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
    temporal_reset_requested = true;
    render_target_layout_tracker.reset();
    internal_render_extent = extent;
}

void Renderer::resetTemporalHistory() {
    auto modules = resolveRenderFrameModules();
    modules.render_target_container.resetHistory();
    modules.instance_container.resetTemporalHistory();
    temporal_reset_requested = true;
    render_target_layout_tracker.reset();
}

void Renderer::selectGraphVariant(RenderGraphVariant variant) {
    if (variant == active_graph_variant) return;
    if (variant == RenderGraphVariant::xr && !xr_rendering_pass_id) {
        throw std::runtime_error(
            "OpenXR render graph was not precompiled at startup");
    }

    const auto from = active_graph_variant;
    auto modules = resolveRenderFrameModules();
    modules.render_target_container.resetHistory();
    modules.instance_container.resetTemporalHistory();
    render_target_layout_tracker.reset();
    temporal_reset_requested = true;

    active_graph_variant = variant;
    current_rendering_pass_id =
        variant == RenderGraphVariant::flat ? flat_rendering_pass_id
                                            : *xr_rendering_pass_id;
    graph_variant_transition_trace.push_back({
        {"from", from == RenderGraphVariant::flat ? "flat" : "xr"},
        {"to", variant == RenderGraphVariant::flat ? "flat" : "xr"},
        {"temporal_reset_requests", 1},
    });
    pending_graph_transition = graph_variant_transition_trace.size() - 1;
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

void Renderer::renderLogicalFrame(
    ILogicalFrameTarget &target,
    std::span<const RenderViewParameters> views) {
    if (views.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "Renderer logical frame view count exceeds the public index range");
    }
    const auto view_count = static_cast<std::uint32_t>(views.size());
    if (view_count == 0) {
        throw std::runtime_error("Renderer logical frame requires at least one view");
    }

    auto &deletion_queue = resolveFrameDeletionQueue();
    deletion_queue.beginFrame();

    auto modules = resolveRenderFrameModules();
    // Keep one immutable publication root alive for the whole logical frame.
    // A frame therefore observes either the complete old generation or the
    // complete new generation, never a pass/plan mixture.
    const auto runtime_generation =
        modules.frame_graph_runtime.snapshot();
    const auto *program =
        runtime_generation != nullptr
            ? runtime_generation->find(
                  current_rendering_pass_id)
            : nullptr;
    if (program == nullptr ||
        !program->frame_graph.render_pipeline) {
        throw std::runtime_error(
            "Renderer logical frame requires a compiled render pipeline");
    }
    const auto &rendering_pass =
        program->rendering_pass;
    const auto &frame_graph =
        program->frame_graph;
    const auto frame_projection_jitter =
        projectionJitterSettingsFor(frame_graph);

    if (modules.render_timing != nullptr) {
        std::size_t max_nodes = 0;
        for (const auto pass_id : {flat_rendering_pass_id,
                                   xr_rendering_pass_id.value_or(flat_rendering_pass_id)}) {
            const auto *candidate =
                runtime_generation->find(pass_id);
            if (candidate != nullptr) {
                max_nodes = std::max(
                    max_nodes,
                    candidate->frame_graph.nodes.size());
            }
        }
        // XR range slots 0/1 are the eyes; 2 is the engine mirror copy and 3
        // is the independently submitted desktop mirror sink.
        const auto range_slots = xr_rendering_pass_id ? 4u : view_count;
        modules.render_timing->configureGpuQueries(
            static_cast<std::uint32_t>(in_flight_frames_num), range_slots,
            static_cast<std::uint32_t>(max_nodes));
    }
    auto &temporal_histories = activeTemporalHistories();
    if (temporal_histories.size() != view_count) {
        modules.render_target_container.resetHistory();
        modules.instance_container.resetTemporalHistory();
        render_target_layout_tracker.reset();
        temporal_histories.assign(view_count, TemporalFrameHistory{});
        temporal_reset_requested = true;
    }
    modules.frame_resources.beginLogicalFrame(view_count);

    auto shader_hot_reload = resolveShaderHotReloadModules();
    if (consumeShaderReloadPublication(shader_hot_reload)) {
        rebindFullscreenInputs(modules);
    }
    auto &engine_time = resolveFrameEngineTime();
    const auto time_set_revision = engine_time.timeSetRevision();
    const auto camera_discontinuity_revision = modules.camera.discontinuityRevision();
    const bool has_temporal_history =
        std::any_of(temporal_histories.begin(), temporal_histories.end(),
                    [](const TemporalFrameHistory &history) { return history.valid; });
    if (has_temporal_history &&
        (time_set_revision != observed_time_set_revision ||
         camera_discontinuity_revision != observed_camera_discontinuity_revision)) {
        modules.render_target_container.resetHistory();
        modules.instance_container.resetTemporalHistory();
        render_target_layout_tracker.reset();
        temporal_reset_requested = true;
    }
    observed_time_set_revision = time_set_revision;
    observed_camera_discontinuity_revision = camera_discontinuity_revision;
    updateFrameLights(modules.light_container);

    const auto &graph_variant_policy =
        frame_graph.render_pipeline->graph_variant_policy;
    const auto expected_graph_variant =
        active_graph_variant == RenderGraphVariant::flat
            ? RenderPipelineGraphVariant::flat
            : RenderPipelineGraphVariant::xr;
    if (graph_variant_policy.variant !=
        expected_graph_variant) {
        throw std::runtime_error(
            "Renderer selected graph does not match its compiled graph variant policy");
    }
    if (graph_variant_policy.view_count != 0 &&
        view_count != graph_variant_policy.view_count) {
        throw std::runtime_error(
            "Renderer graph variant '" +
            std::string{renderPipelineGraphVariantName(
                graph_variant_policy.variant)} +
            "' requires " +
            std::to_string(graph_variant_policy.view_count) +
            " views");
    }
    const auto &draw_sorting =
        frame_graph.render_pipeline->draw_sorting;
    const bool per_view_sort =
        graph_variant_policy.view_family ==
            GraphVariantViewFamily::stereo &&
        draw_sorting.xr_view_policy == DrawSortXrViewPolicy::per_view;
    if (per_view_sort && view_count != 2) {
        throw std::runtime_error(
            "XR per_view draw sorting requires exactly two views");
    }
    const auto sort_view_for = [](const RenderViewParameters &view) {
        if (!std::isfinite(view.camera_position.x) ||
            !std::isfinite(view.camera_position.y) ||
            !std::isfinite(view.camera_position.z)) {
            throw std::runtime_error(
                "render view has a non-finite camera position");
        }
        for (glm::length_t column = 0; column < 4; ++column) {
            for (glm::length_t row = 0; row < 4; ++row) {
                if (!std::isfinite(view.view[column][row])) {
                    throw std::runtime_error(
                        "render view has a non-finite view matrix");
                }
            }
        }
        const auto world_from_view = glm::inverse(view.view);
        auto forward = -glm::vec3{world_from_view[2]};
        const auto length = glm::length(forward);
        if (!std::isfinite(length) || length <= 0.0F) {
            throw std::runtime_error(
                "render view has an invalid forward direction");
        }
        forward /= length;
        return DrawQueueSortView{
            .logical_view =
                view.first_person_view
                    ? RenderPolicy::DrawSortLogicalViewV1::first_person
                    : RenderPolicy::DrawSortLogicalViewV1::third_person,
            .origin = {view.camera_position.x, view.camera_position.y,
                       view.camera_position.z},
            .forward = {forward.x, forward.y, forward.z},
        };
    };
    std::vector<DrawQueueSortView> sort_views;
    if (per_view_sort) {
        sort_views.reserve(view_count);
        for (const auto &view : views)
            sort_views.push_back(sort_view_for(view));
    } else {
        glm::dvec3 origin{0.0};
        glm::dvec3 forward{0.0};
        bool all_first_person = true;
        bool all_third_person = true;
        for (const auto &view : views) {
            const auto snapshot = sort_view_for(view);
            origin += glm::dvec3{snapshot.origin[0], snapshot.origin[1],
                                 snapshot.origin[2]};
            forward += glm::dvec3{snapshot.forward[0], snapshot.forward[1],
                                  snapshot.forward[2]};
            all_first_person = all_first_person && view.first_person_view;
            all_third_person = all_third_person && !view.first_person_view;
        }
        origin /= static_cast<double>(view_count);
        const auto length = glm::length(forward);
        if (!std::isfinite(length) || length <= 0.0) {
            throw std::runtime_error(
                "logical view center has an invalid forward direction");
        }
        forward /= length;
        sort_views.push_back(DrawQueueSortView{
            .logical_view = all_first_person
                                ? RenderPolicy::DrawSortLogicalViewV1::first_person
                            : all_third_person
                                ? RenderPolicy::DrawSortLogicalViewV1::third_person
                                : RenderPolicy::DrawSortLogicalViewV1::shared,
            .origin = {static_cast<float>(origin.x),
                       static_cast<float>(origin.y),
                       static_cast<float>(origin.z)},
            .forward = {static_cast<float>(forward.x),
                        static_cast<float>(forward.y),
                        static_cast<float>(forward.z)},
        });
    }
    std::vector<RenderFrameSnapshot> snapshots;
    snapshots.reserve(view_count);
    nlohmann::json view_traces = nlohmann::json::array();
    std::optional<std::uint32_t> logical_in_flight_frame;
    std::optional<vk::Extent2D> logical_extent;

    target.beginLogicalFrame(view_count);
    for (std::uint32_t view_index = 0; view_index < view_count; ++view_index) {
        const auto render_ctx = target.beginView(view_index);
        if (view_index == 0) {
            logical_in_flight_frame = render_ctx.in_flight_frame_index;
            logical_extent = render_ctx.extent;
            const bool extent_changed =
                !internal_render_extent || *internal_render_extent != render_ctx.extent;
            if (handleFrameTargetResize(modules, render_target_layout_tracker, target,
                                        render_ctx.extent, extent_changed)) {
                modules.instance_container.resetTemporalHistory();
                temporal_reset_requested = true;
                internal_render_extent = render_ctx.extent;
            }
            // Object, skin, morph, and material-override GPU state is frozen
            // after target acquisition and before the first view records.
            modules.instance_container.triggerUpdate(DrawQueueFramePlan{
                .opaque_provider = draw_sorting.opaque.provider,
                .transparent_provider = draw_sorting.transparent.provider,
                .sort_views = sort_views,
            });
        } else {
            if (render_ctx.in_flight_frame_index != *logical_in_flight_frame) {
                throw std::runtime_error(
                    "Renderer logical-frame views must share one in-flight frame index");
            }
            if (render_ctx.extent != *logical_extent) {
                throw std::runtime_error(
                    "Renderer logical-frame v1 requires equal per-view extents");
            }
        }

        const auto frame_target_format = target.colorFormat(view_index);
        if (frame_target_format != modules.render_target.getSwapchainFormat()) {
            throw std::runtime_error(
                "Renderer logical-frame target format does not match the compiled flat graph");
        }

        const auto &view = views[view_index];
        glm::vec2 jitter_ndc{0.0f};
        if (frame_projection_jitter) {
            jitter_ndc = projectionJitterSample(*frame_projection_jitter, engine_time.frameIndex(),
                                                render_ctx.extent.width, render_ctx.extent.height)
                             .jitter_ndc;
        }
        snapshots.push_back(buildRenderFrameSnapshot(
            temporal_histories.at(view_index), view.projection, view.view,
            view.camera_position, jitter_ndc, temporal_reset_requested));
        const auto &snapshot = snapshots.back();

        modules.frame_resources.selectView(render_ctx.in_flight_frame_index, view_index);
        updateFrameResources(modules, engine_time, render_ctx.extent, snapshot);

        nlohmann::json node_trace;
        nlohmann::json *node_trace_ptr = nullptr;
        if (execution_tracing_for_testing) {
            node_trace = nlohmann::json::array();
            node_trace_ptr = &node_trace;
        }
        executeRenderingPasses(render_ctx, rendering_pass, frame_graph, modules,
                               snapshot, view.first_person_view, frame_target_format,
                               render_target_layout_tracker,
                               node_trace_ptr, engine_time.frameIndex(),
                               renderPipelineGraphVariantName(
                                   graph_variant_policy.variant),
                               view_index,
                               per_view_sort ? view_index : 0);
#if PELICAN_WITH_OPENXR
        if (graph_variant_policy.mirror_output ==
                GraphVariantMirrorOutput::left_eye &&
            view_index == 0) {
            recordXrMirrorIntermediate(render_ctx, modules,
                                       render_target_layout_tracker, node_trace_ptr,
                                       engine_time.frameIndex());
        }
#endif
        target.endView(view_index);

        if (execution_tracing_for_testing) {
            view_traces.push_back({
                {"view_index", view_index},
                {"nodes", std::move(node_trace)},
                {"final_layouts",
                 finalLayoutsTrace(rendering_pass, modules.render_target_container,
                                   render_target_layout_tracker, render_ctx.required_layout)},
            });
        }
    }

    target.endLogicalFrame();
    if (execution_tracing_for_testing) {
        if (view_count == 1) {
            last_execution_trace = nlohmann::json{
                {"nodes", std::move(view_traces.at(0).at("nodes"))},
                {"final_layouts", std::move(view_traces.at(0).at("final_layouts"))},
            };
        } else {
            last_execution_trace = nlohmann::json{{"views", std::move(view_traces)}};
        }
    }

    modules.render_target_container.advanceHistoryFrame();
    modules.instance_container.advanceTemporalHistoryAfterRender();
    for (std::uint32_t view_index = 0; view_index < view_count; ++view_index) {
        commitRenderFrameSnapshot(temporal_histories.at(view_index), snapshots.at(view_index));
    }
    last_view_snapshots = std::move(snapshots);
    temporal_reset_requested = false;
    if (pending_graph_transition) {
        auto &transition = graph_variant_transition_trace.at(*pending_graph_transition);
        transition["reset_epochs"] = nlohmann::json::array();
        for (const auto &snapshot : last_view_snapshots) {
            transition["reset_epochs"].push_back(snapshot.temporal_reset_epoch);
        }
        transition["projection_jitter"] =
            frame_projection_jitter.has_value();
        transition["frame_plan"] = currentFramePlanOrderForTesting();
        pending_graph_transition.reset();
    }
}

void Renderer::render() {
    selectGraphVariant(RenderGraphVariant::flat);
    FlatLogicalFrameTarget target{GET_MODULE(RenderTarget)};
    const auto &camera = GET_MODULE(Camera);
    const std::array views{
        RenderViewParameters{
            .view = camera.getViewMatrix(),
            .projection = camera.getProjectionMatrix(),
            .camera_position = camera.getPos(),
        },
    };
    renderLogicalFrame(target, views);
}

} // namespace Pelican
