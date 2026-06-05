#include "render_pass_dispatch.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/fullscreenpassrenderer.hpp"
#include "../renderer/materialrender.hpp"
#include "../renderer/uirenderer.hpp"
#include <stdexcept>

namespace Pelican {

namespace {

void renderMaterialPass(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def,
                        const RenderPassDispatchDependencies &dependencies) {
    const auto &materialInfo = pass_def.materialInfo();
    if (materialInfo.material_count > 0) {
        dependencies.material_renderer.renderWithMaterialRange(cmd_buf, pass_id, materialInfo.material_start,
                                                               materialInfo.material_count,
                                                               dependencies.material_renderer_dependencies);
    } else {
        dependencies.material_renderer.render(cmd_buf, pass_id, dependencies.material_renderer_dependencies);
    }
}

FullscreenPassCameraData createFullscreenPassCameraData(FullscreenPushConstantData push_constants,
                                                        const Camera &camera) {
    FullscreenPassCameraData camera_data;
    if (push_constants == FullscreenPushConstantData::eNone) {
        return camera_data;
    }

    if (push_constants == FullscreenPushConstantData::eCameraPosition) {
        camera_data.position = camera.getPos();
    } else if (push_constants == FullscreenPushConstantData::eProjectionView) {
        camera_data.projection = camera.getProjectionMatrix();
        camera_data.view = camera.getViewMatrix();
    }
    return camera_data;
}

void renderFullscreenPass(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def,
                          const RenderPassDispatchDependencies &dependencies) {
    const auto &fullscreenInfo = pass_def.fullscreenInfo();
    const auto camera_data = createFullscreenPassCameraData(fullscreenInfo.push_constants, dependencies.camera);
    dependencies.fullscreen_pass_renderer.render(cmd_buf, pass_id, pass_def, camera_data,
                                                 dependencies.fullscreen_pass_renderer_dependencies);
}

} // namespace

void renderUiPass(vk::CommandBuffer cmd_buf, const FrameRenderContext &frame, const PassDefinition &pass_def,
                  vk::Extent2D target_extent, RenderTargetContainer &rt_container,
                  const RenderPassDispatchDependencies &dependencies) {
    if (pass_def.output_color.empty()) {
        throw std::runtime_error("UI pass has no color output: " + pass_def.name);
    }

    const auto rt_id = pass_def.output_color.front();
    const bool targets_swapchain = isSwapchainRenderTarget(rt_id);
    const vk::ImageView target_view = targets_swapchain ? frame.color_attachment
                                                        : rt_container.getImageView(rt_id);
    const vk::Format target_format = targets_swapchain ? dependencies.swapchain_color_format
                                                       : rt_container.getMetadata(rt_id).format;
    dependencies.ui_renderer.render(cmd_buf, UiDrawRequest{target_view, target_extent, target_format,
                                                           pass_def.color_load_op, pass_def.color_store_op,
                                                           pass_def.clear_color});
}

void renderDynamicPassDrawCalls(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def,
                                const RenderPassDispatchDependencies &dependencies) {
    if (pass_def.isMaterial()) {
        renderMaterialPass(cmd_buf, pass_id, pass_def, dependencies);
    } else if (pass_def.isFullscreen()) {
        renderFullscreenPass(cmd_buf, pass_id, pass_def, dependencies);
    } else {
        throw std::runtime_error("Unsupported dynamic render pass type: " + pass_def.name);
    }
}

} // namespace Pelican
