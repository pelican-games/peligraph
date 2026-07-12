#include "render_pass_executor.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "render_pass_dispatch.hpp"
#include "render_pass_frame_setup.hpp"

namespace Pelican {

void RenderPassExecutor::execute(const FrameRenderContext &frame, const CompiledPass &pass,
                                 const RenderPassExecutorDependencies &dependencies,
                                 RenderTargetLayoutTracker &layout_tracker) const {
    auto &rt_container = dependencies.render_target_container;
    auto &vk_utils = dependencies.vk_utils;
    const auto cmd_buf = frame.cmd_buf;
    const auto &pass_def = pass.definition;
    const auto target_extent = getRenderPassTargetExtent(frame, pass_def, rt_container);

    transitionPassInputsToShaderRead(cmd_buf, pass_def, rt_container, vk_utils, layout_tracker);
    transitionPassOutputsToAttachmentLayouts(cmd_buf, pass_def, rt_container, vk_utils, layout_tracker);

    if (pass_def.isUi()) {
        renderUiPass(cmd_buf, frame, pass_def, target_extent, rt_container, dependencies.dispatch);
        return;
    }
#if PELICAN_WITH_IMGUI
    if (pass_def.isImGui()) {
        renderImGuiPass(cmd_buf, frame, pass_def, target_extent, rt_container, dependencies.dispatch);
        return;
    }
#endif

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

    renderDynamicPassDrawCalls(cmd_buf, pass.pass_id, pass_def, target_extent, dependencies.dispatch);

    cmd_buf.endRendering();
}

} // namespace Pelican
