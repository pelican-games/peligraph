#include "render_pass_frame_setup.hpp"

#include <stdexcept>

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
        if (isConcreteRenderTarget(rt_id) &&
            rt_container.hasSeparateAttachment(rt_id) &&
            pass_def.color_load_op == vk::AttachmentLoadOp::eLoad &&
            layout_tracker.currentLayout(
                rt_id, false, &rt_container,
                RenderTargetImageKind::attachment) ==
                vk::ImageLayout::eUndefined) {
            throw std::runtime_error(
                "multisampled color attachment Load requires a preceding "
                "raster access to the multisample surface");
        }
        layout_tracker.transition(cmd_buf, rt_container, vk_utils, rt_id,
                                  vk::ImageLayout::eColorAttachmentOptimal);
        if (isConcreteRenderTarget(rt_id) &&
            rt_container.hasSeparateAttachment(rt_id)) {
            layout_tracker.transition(
                cmd_buf, rt_container, vk_utils, rt_id,
                vk::ImageLayout::eColorAttachmentOptimal, false,
                RenderTargetImageKind::attachment);
        }
    }
    if (isConcreteRenderTarget(pass_def.output_depth) &&
        rt_container.hasSeparateAttachment(pass_def.output_depth) &&
        pass_def.depth_load_op == vk::AttachmentLoadOp::eLoad &&
        layout_tracker.currentLayout(
            pass_def.output_depth, false, &rt_container,
            RenderTargetImageKind::attachment) ==
            vk::ImageLayout::eUndefined) {
        throw std::runtime_error(
            "multisampled depth attachment Load requires a preceding raster "
            "access to the multisample surface");
    }
    layout_tracker.transition(cmd_buf, rt_container, vk_utils, pass_def.output_depth,
                              vk::ImageLayout::eDepthAttachmentOptimal);
    if (isConcreteRenderTarget(pass_def.output_depth) &&
        rt_container.hasSeparateAttachment(pass_def.output_depth)) {
        layout_tracker.transition(
            cmd_buf, rt_container, vk_utils, pass_def.output_depth,
            vk::ImageLayout::eDepthAttachmentOptimal, false,
            RenderTargetImageKind::attachment);
    }
}

void transitionPassInputsToShaderRead(vk::CommandBuffer cmd_buf, const PassDefinition &pass_def,
                                      RenderTargetContainer &rt_container, VulkanUtils &vk_utils,
                                      RenderTargetLayoutTracker &layout_tracker) {
    for (size_t i = 0; i < pass_def.input_targets.size(); ++i) {
        layout_tracker.transition(cmd_buf, rt_container, vk_utils, pass_def.input_targets[i],
                                  vk::ImageLayout::eShaderReadOnlyOptimal,
                                  pass_def.input_target_history.at(i));
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
            color_att.imageView =
                rt_container.getAttachmentImageView(rt_id);
            if (rt_container.hasSeparateAttachment(rt_id)) {
                color_att.resolveMode = rt_container.resolveMode(rt_id);
                color_att.resolveImageView =
                    rt_container.getImageView(rt_id);
                color_att.resolveImageLayout =
                    vk::ImageLayout::eColorAttachmentOptimal;
            }
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
    depth_attachment.imageView =
        rt_container.getAttachmentImageView(pass_def.output_depth);
    depth_attachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    if (rt_container.hasSeparateAttachment(pass_def.output_depth)) {
        depth_attachment.resolveMode =
            rt_container.resolveMode(pass_def.output_depth);
        depth_attachment.resolveImageView =
            rt_container.getImageView(pass_def.output_depth);
        depth_attachment.resolveImageLayout =
            vk::ImageLayout::eDepthAttachmentOptimal;
    }
    depth_attachment.loadOp = pass_def.depth_load_op;
    depth_attachment.storeOp = pass_def.depth_store_op;
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
