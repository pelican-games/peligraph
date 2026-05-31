#include "render_pass_executor.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "render_pass_dispatch.hpp"
#include "render_pass_frame_setup.hpp"

namespace Pelican {

void RenderPassExecutor::execute(const FrameRenderContext &frame, const PassDefinition &pass_def, PassId pass_id,
                                 RenderTargetLayoutTracker &layout_tracker) const {
    auto &rt_container = GET_MODULE(RenderTargetContainer);
    auto &vk_utils = GET_MODULE(VulkanUtils);
    const auto cmd_buf = frame.cmd_buf;
    const auto target_extent = getRenderPassTargetExtent(frame, pass_def, rt_container);

    transitionPassInputsToShaderRead(cmd_buf, pass_def, rt_container, vk_utils, layout_tracker);
    transitionPassOutputsToAttachmentLayouts(cmd_buf, pass_def, rt_container, vk_utils, layout_tracker);

    if (pass_def.isUi()) {
        renderUiPass(cmd_buf, frame, pass_def, target_extent, rt_container);
        return;
    }

    auto color_attachments = createColorAttachments(frame, pass_def, rt_container);

    vk::RenderingInfo render_info;
    render_info.renderArea = vk::Rect2D{{0, 0}, target_extent};
    render_info.layerCount = 1;
    render_info.setColorAttachments(color_attachments);

    vk::RenderingAttachmentInfo depth_attachment;
    if (isConcreteRenderTarget(pass_def.output_depth)) {
        depth_attachment = createDepthAttachment(pass_def, rt_container);
        render_info.pDepthAttachment = &depth_attachment;
    }

    cmd_buf.beginRendering(render_info);
    setDynamicViewportAndScissor(cmd_buf, target_extent);

    renderDynamicPassDrawCalls(cmd_buf, pass_id, pass_def);

    cmd_buf.endRendering();
}

} // namespace Pelican
