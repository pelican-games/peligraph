#include "render_pass_frame_setup.hpp"

namespace Pelican {

vk::Extent2D getRenderPassTargetExtent(const FrameRenderContext &frame, const PassDefinition &pass_def,
                                       RenderTargetContainer &rt_container) {
    for (const auto &rt_id : pass_def.output_color) {
        if (isConcreteRenderTarget(rt_id)) {
            return rt_container.getMetadata(rt_id).extent;
        }
    }
    if (isConcreteRenderTarget(pass_def.output_depth)) {
        return rt_container.getMetadata(pass_def.output_depth).extent;
    }
    return frame.extent;
}

void transitionPassOutputsToAttachmentLayouts(vk::CommandBuffer cmd_buf, const PassDefinition &pass_def,
                                              RenderTargetContainer &rt_container, VulkanUtils &vk_utils,
                                              RenderTargetLayoutTracker &layout_tracker) {
    for (const auto &rt_id : pass_def.output_color) {
        layout_tracker.transition(cmd_buf, rt_container, vk_utils, rt_id, vk::ImageLayout::eColorAttachmentOptimal);
    }
    layout_tracker.transition(cmd_buf, rt_container, vk_utils, pass_def.output_depth,
                              vk::ImageLayout::eDepthAttachmentOptimal);
}

void transitionPassInputsToShaderRead(vk::CommandBuffer cmd_buf, const PassDefinition &pass_def,
                                      RenderTargetContainer &rt_container, VulkanUtils &vk_utils,
                                      RenderTargetLayoutTracker &layout_tracker) {
    for (const auto &rt_id : pass_def.input_targets) {
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
        if (isSwapchainRenderTarget(rt_id)) {
            color_att.imageView = frame.color_attachment;
        } else {
            color_att.imageView = rt_container.getImageView(rt_id);
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
    depth_attachment.imageView = rt_container.getImageView(pass_def.output_depth);
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

} // namespace Pelican
