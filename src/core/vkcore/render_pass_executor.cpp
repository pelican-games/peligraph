#include "render_pass_executor.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/fullscreenpassrenderer.hpp"
#include "../renderer/materialrender.hpp"
#include "../renderer/uirenderer.hpp"
#include "render_pass_frame_setup.hpp"

namespace Pelican {

namespace {

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
    const auto target_extent = getRenderPassTargetExtent(frame, pass_def, rt_container);

    transitionPassInputsToShaderRead(cmd_buf, pass_def, rt_container, vk_utils, layout_tracker);
    transitionPassOutputsToAttachmentLayouts(cmd_buf, pass_def, rt_container, vk_utils, layout_tracker);

    if (pass_def.isUi()) {
        if (pass_def.output_color.empty()) {
            return;
        }

        const auto rt_id = pass_def.output_color.front();
        const bool targets_swapchain = isSwapchainRenderTarget(rt_id);
        const auto &rt_module = GET_MODULE(RenderTarget);
        const vk::ImageView target_view = targets_swapchain ? frame.color_attachment
                                                            : rt_container.get(rt_id).image_view.get();
        const vk::Format target_format = targets_swapchain ? rt_module.getSwapchainFormat()
                                                           : rt_container.get(rt_id).image.format;
        GET_MODULE(UiRenderer).render(cmd_buf, UiDrawRequest{target_view, target_extent, target_format,
                                                             pass_def.color_load_op, pass_def.color_store_op,
                                                             pass_def.clear_color});
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

    if (pass_def.isMaterial()) {
        renderMaterialPass(cmd_buf, pass_id, pass_def);
    } else if (pass_def.isFullscreen()) {
        renderFullscreenPass(cmd_buf, pass_id, pass_def);
    }

    cmd_buf.endRendering();
}

} // namespace Pelican
