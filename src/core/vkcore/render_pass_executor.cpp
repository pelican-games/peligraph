#include "render_pass_executor.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/fullscreenpassrenderer.hpp"
#include "../renderer/materialrender.hpp"
#include "../renderer/uirenderer.hpp"
#include "util.hpp"
#include <vector>

namespace Pelican {

namespace {

vk::Extent2D getTargetExtent(const FrameRenderContext &frame, const PassDefinition &pass_def,
                             RenderTargetContainer &rt_container) {
    for (const auto &rt_id : pass_def.output_color) {
        if (rt_id.value >= 0) {
            const auto &output_rt = rt_container.get(rt_id);
            return vk::Extent2D{output_rt.image.extent.width, output_rt.image.extent.height};
        }
    }
    if (pass_def.output_depth.value >= 0) {
        const auto &output_rt = rt_container.get(pass_def.output_depth);
        return vk::Extent2D{output_rt.image.extent.width, output_rt.image.extent.height};
    }
    return frame.extent;
}

void transitionOutputsToAttachmentLayouts(vk::CommandBuffer cmd_buf, const PassDefinition &pass_def,
                                          RenderTargetContainer &rt_container, VulkanUtils &vk_utils,
                                          RenderTargetLayoutTracker &layout_tracker) {
    for (const auto &rt_id : pass_def.output_color) {
        layout_tracker.transition(cmd_buf, rt_container, vk_utils, rt_id, vk::ImageLayout::eColorAttachmentOptimal);
    }
    layout_tracker.transition(cmd_buf, rt_container, vk_utils, pass_def.output_depth,
                              vk::ImageLayout::eDepthAttachmentOptimal);
}

void transitionInputsToShaderRead(vk::CommandBuffer cmd_buf, const PassDefinition &pass_def,
                                  RenderTargetContainer &rt_container, VulkanUtils &vk_utils,
                                  RenderTargetLayoutTracker &layout_tracker) {
    for (const auto &rt_id : pass_def.input_targets) {
        layout_tracker.transition(cmd_buf, rt_container, vk_utils, rt_id, vk::ImageLayout::eShaderReadOnlyOptimal);
    }
}

void transitionColorOutputsToShaderRead(vk::CommandBuffer cmd_buf, const PassDefinition &pass_def,
                                        RenderTargetContainer &rt_container, VulkanUtils &vk_utils,
                                        RenderTargetLayoutTracker &layout_tracker) {
    for (const auto &rt_id : pass_def.output_color) {
        layout_tracker.transition(cmd_buf, rt_container, vk_utils, rt_id, vk::ImageLayout::eShaderReadOnlyOptimal);
    }
}

std::vector<vk::RenderingAttachmentInfo> createColorAttachments(const FrameRenderContext &frame,
                                                                const PassDefinition &pass_def,
                                                                RenderTargetContainer &rt_container) {
    std::vector<vk::RenderingAttachmentInfo> color_attachments;
    color_attachments.reserve(pass_def.output_color.size());

    for (const auto &rt_id : pass_def.output_color) {
        vk::RenderingAttachmentInfo color_att;
        if (rt_id.value < 0) {
            color_att.imageView = frame.color_attachment;
        } else {
            color_att.imageView = rt_container.get(rt_id).image_view.get();
        }

        color_att.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        color_att.loadOp = pass_def.color_load_op;
        color_att.storeOp = pass_def.color_store_op;
        color_att.clearValue.color = pass_def.clear_color;
        color_attachments.push_back(color_att);
    }

    return color_attachments;
}

vk::RenderingAttachmentInfo createDepthAttachment(const PassDefinition &pass_def,
                                                  RenderTargetContainer &rt_container) {
    vk::RenderingAttachmentInfo depth_attachment;
    const auto &depth_rt = rt_container.get(pass_def.output_depth);
    depth_attachment.imageView = depth_rt.image_view.get();
    depth_attachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    depth_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eDontCare;
    depth_attachment.clearValue.depthStencil = vk::ClearDepthStencilValue{1.0f, 0};
    return depth_attachment;
}

void setDynamicViewportAndScissor(vk::CommandBuffer cmd_buf, vk::Extent2D extent) {
    vk::Viewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f,
                          1.0f};
    cmd_buf.setViewport(0, viewport);

    vk::Rect2D scissor{{0, 0}, extent};
    cmd_buf.setScissor(0, scissor);
}

void renderMaterialPass(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def) {
    auto &mat_renderer = GET_MODULE(MaterialRenderer);
    const auto &materialInfo = pass_def.materialInfo();
    if (materialInfo.material_count > 0) {
        mat_renderer.renderWithMaterialRange(cmd_buf, pass_id, materialInfo.material_start,
                                             materialInfo.material_count);
    } else {
        mat_renderer.render(cmd_buf, pass_id);
    }
}

FullscreenPassCameraData createFullscreenPassCameraData(FullscreenPushConstantData push_constants) {
    FullscreenPassCameraData camera_data;
    if (push_constants == FullscreenPushConstantData::eNone) {
        return camera_data;
    }

    const auto &camera = GET_MODULE(Camera);
    if (push_constants == FullscreenPushConstantData::eCameraPosition) {
        camera_data.position = camera.getPos();
    } else if (push_constants == FullscreenPushConstantData::eProjectionView) {
        camera_data.projection = camera.getProjectionMatrix();
        camera_data.view = camera.getViewMatrix();
    }
    return camera_data;
}

void renderFullscreenPass(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def) {
    const auto &fullscreenInfo = pass_def.fullscreenInfo();
    const auto camera_data = createFullscreenPassCameraData(fullscreenInfo.push_constants);
    GET_MODULE(FullscreenPassRenderer).render(cmd_buf, pass_id, pass_def, camera_data);
}

} // namespace

void RenderPassExecutor::execute(const FrameRenderContext &frame, const PassDefinition &pass_def, PassId pass_id,
                                 RenderTargetLayoutTracker &layout_tracker) const {
    auto &rt_container = GET_MODULE(RenderTargetContainer);
    auto &vk_utils = GET_MODULE(VulkanUtils);
    const auto cmd_buf = frame.cmd_buf;
    const auto target_extent = getTargetExtent(frame, pass_def, rt_container);

    transitionInputsToShaderRead(cmd_buf, pass_def, rt_container, vk_utils, layout_tracker);
    transitionOutputsToAttachmentLayouts(cmd_buf, pass_def, rt_container, vk_utils, layout_tracker);

    if (pass_def.isUi()) {
        if (pass_def.output_color.empty()) {
            return;
        }

        const auto rt_id = pass_def.output_color.front();
        const bool targets_swapchain = rt_id.value < 0;
        const auto &rt_module = GET_MODULE(RenderTarget);
        const vk::ImageView target_view = targets_swapchain ? frame.color_attachment
                                                            : rt_container.get(rt_id).image_view.get();
        const vk::Format target_format = targets_swapchain ? rt_module.getSwapchainFormat()
                                                           : rt_container.get(rt_id).image.format;
        GET_MODULE(UiRenderer).render(cmd_buf, UiDrawRequest{target_view, target_extent, target_format,
                                                             pass_def.color_load_op, pass_def.color_store_op,
                                                             pass_def.clear_color});
        transitionColorOutputsToShaderRead(cmd_buf, pass_def, rt_container, vk_utils, layout_tracker);
        return;
    }

    auto color_attachments = createColorAttachments(frame, pass_def, rt_container);

    vk::RenderingInfo render_info;
    render_info.renderArea = vk::Rect2D{{0, 0}, target_extent};
    render_info.layerCount = 1;
    render_info.setColorAttachments(color_attachments);

    vk::RenderingAttachmentInfo depth_attachment;
    if (pass_def.output_depth.value >= 0) {
        depth_attachment = createDepthAttachment(pass_def, rt_container);
        render_info.pDepthAttachment = &depth_attachment;
    }

    cmd_buf.beginRendering(render_info);
    setDynamicViewportAndScissor(cmd_buf, target_extent);

    if (pass_def.isMaterial()) {
        renderMaterialPass(cmd_buf, pass_id, pass_def);
    } else if (pass_def.isFullscreen()) {
        renderFullscreenPass(cmd_buf, pass_id, pass_def);
    }

    cmd_buf.endRendering();
    transitionColorOutputsToShaderRead(cmd_buf, pass_def, rt_container, vk_utils, layout_tracker);
}

} // namespace Pelican
