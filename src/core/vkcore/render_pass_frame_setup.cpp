#include "render_pass_frame_setup.hpp"

#include <stdexcept>

namespace Pelican {

namespace {

std::uint32_t sequentialAttachmentLayer(
    GlobalRenderTargetId id,
    const RenderTargetContainer &rt_container,
    RenderPassViewInvocation invocation) {
    const auto layers =
        rt_container.getMetadata(id).array_layers;
    if (layers == 1) return 0;
    if (invocation.logical_view_count == 0 ||
        invocation.view_index >=
            invocation.logical_view_count ||
        layers < invocation.logical_view_count) {
        throw std::runtime_error(
            "sequential render-pass invocation does not fit "
            "the target array-layer contract");
    }
    return invocation.view_index;
}

vk::ImageView colorAttachmentView(
    GlobalRenderTargetId id,
    RenderTargetContainer &rt_container,
    const GraphicsPipelineViewContract &view,
    RenderPassViewInvocation invocation,
    bool resolve) {
    if (view.execution ==
        GraphicsPipelineViewExecution::multiview) {
        return resolve
                   ? rt_container.getLayeredImageView(id)
                   : rt_container
                         .getLayeredAttachmentImageView(id);
    }
    const auto layer = sequentialAttachmentLayer(
        id, rt_container, invocation);
    return resolve
               ? rt_container.getImageLayerView(id, layer)
               : rt_container
                     .getAttachmentImageLayerView(id, layer);
}

} // namespace

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
    for (std::size_t index = 0;
         index < pass_def.output_color.size();
         ++index) {
        const auto rt_id =
            pass_def.output_color[index];
        if (isConcreteRenderTarget(rt_id) &&
            rt_container.hasSeparateAttachment(rt_id) &&
            pass_def
                    .colorAttachmentOperations(index)
                    .load_op ==
                vk::AttachmentLoadOp::eLoad &&
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
    const auto depth_operations =
        pass_def.depthAttachmentOperations();
    if (isConcreteRenderTarget(pass_def.output_depth) &&
        rt_container.hasSeparateAttachment(pass_def.output_depth) &&
        depth_operations.load_op ==
            vk::AttachmentLoadOp::eLoad &&
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
                                                                RenderTargetContainer &rt_container,
                                                                const GraphicsPipelineViewContract &view,
                                                                RenderPassViewInvocation invocation) {
    std::vector<vk::RenderingAttachmentInfo> color_attachments;
    color_attachments.reserve(pass_def.output_color.size());

    for (std::size_t index = 0;
         index < pass_def.output_color.size();
         ++index) {
        const auto rt_id =
            pass_def.output_color[index];
        const auto operations =
            pass_def.colorAttachmentOperations(
                index);
        vk::RenderingAttachmentInfo color_att;
        if (isSwapchainRenderTarget(rt_id)) {
            if (view.execution ==
                    GraphicsPipelineViewExecution::single_view &&
                !frame.color_layer_attachments.empty()) {
                if (invocation.view_index >=
                    frame.color_layer_attachments.size()) {
                    throw std::runtime_error(
                        "sequential frame-target layer is out of range");
                }
                color_att.imageView =
                    frame.color_layer_attachments[
                        invocation.view_index];
            } else {
                color_att.imageView = frame.color_attachment;
            }
        } else {
            color_att.imageView =
                colorAttachmentView(
                    rt_id, rt_container, view,
                    invocation, false);
            if (rt_container.hasSeparateAttachment(rt_id)) {
                color_att.resolveMode = rt_container.resolveMode(rt_id);
                color_att.resolveImageView =
                    colorAttachmentView(
                        rt_id, rt_container, view,
                        invocation, true);
                color_att.resolveImageLayout =
                    vk::ImageLayout::eColorAttachmentOptimal;
            }
        }

        color_att.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        color_att.loadOp = operations.load_op;
        color_att.storeOp = operations.store_op;
        color_att.clearValue.color = pass_def.clear_color;
        color_attachments.push_back(color_att);
    }

    return color_attachments;
}

vk::RenderingAttachmentInfo createDepthAttachment(const PassDefinition &pass_def,
                                                  RenderTargetContainer &rt_container,
                                                  const GraphicsPipelineViewContract &view,
                                                  RenderPassViewInvocation invocation) {
    vk::RenderingAttachmentInfo depth_attachment;
    depth_attachment.imageView =
        colorAttachmentView(
            pass_def.output_depth, rt_container, view,
            invocation, false);
    depth_attachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    if (rt_container.hasSeparateAttachment(pass_def.output_depth)) {
        depth_attachment.resolveMode =
            rt_container.resolveMode(pass_def.output_depth);
        depth_attachment.resolveImageView =
            colorAttachmentView(
                pass_def.output_depth, rt_container,
                view, invocation, true);
        depth_attachment.resolveImageLayout =
            vk::ImageLayout::eDepthAttachmentOptimal;
    }
    const auto operations =
        pass_def.depthAttachmentOperations();
    depth_attachment.loadOp = operations.load_op;
    depth_attachment.storeOp = operations.store_op;
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
