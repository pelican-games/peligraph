#include "render_pass_dispatch.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/debugdraw.hpp"
#include "../renderer/debugtext.hpp"
#include "../renderer/fullscreenpassrenderer.hpp"
#include "../renderer/materialrender.hpp"
#include "../renderer/shadowdepthpasscontainer.hpp"
#include "../renderer/velocitypasscontainer.hpp"
#include "../renderer/uirenderer.hpp"
#if PELICAN_WITH_PHYSICS
#include "../phys/physworld.hpp"
#endif
#if PELICAN_WITH_IMGUI
#include "../imgui/imguisystem.hpp"
#endif
#include <stdexcept>

namespace Pelican {

namespace {

void renderMaterialPass(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def,
                        const RenderPassDispatchDependencies &dependencies) {
    const auto &materialInfo = pass_def.materialInfo();
    if (materialInfo.material_count > 0) {
        dependencies.material_renderer.renderWithMaterialRange(cmd_buf, pass_id, pass_def,
                                                               materialInfo.material_start,
                                                               materialInfo.material_count,
                                                               dependencies.material_renderer_dependencies);
    } else {
        dependencies.material_renderer.render(cmd_buf, pass_id, pass_def,
                                              dependencies.material_renderer_dependencies);
    }
}

void renderFullscreenPass(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def,
                          const RenderPassDispatchDependencies &dependencies) {
    dependencies.fullscreen_pass_renderer.render(cmd_buf, pass_id, pass_def,
                                                 dependencies.fullscreen_pass_renderer_dependencies);
}

void renderDebugDrawPass(vk::CommandBuffer cmd_buf, PassId pass_id,
                         const RenderPassDispatchDependencies &dependencies) {
    if (dependencies.debug_draw == nullptr) {
        throw std::runtime_error("DebugDraw pass requires DebugDraw dependency");
    }
#if PELICAN_WITH_PHYSICS
    GET_MODULE(PhysWorld).enqueueDebugDraw(*dependencies.debug_draw, dependencies.view_projection);
#endif
    dependencies.debug_draw->render(cmd_buf, pass_id, dependencies.frame_resources);
}

void renderDebugTextPass(vk::CommandBuffer cmd_buf, PassId pass_id, vk::Extent2D target_extent,
                         const RenderPassDispatchDependencies &dependencies) {
    if (dependencies.debug_text == nullptr) {
        throw std::runtime_error("DebugText pass requires DebugText dependency");
    }
    dependencies.debug_text->render(cmd_buf, pass_id, target_extent, dependencies.frame_resources);
}

void renderShadowDepthPass(vk::CommandBuffer cmd_buf, PassId pass_id,
                           const RenderPassDispatchDependencies &dependencies) {
    dependencies.material_renderer.renderShadowDepth(cmd_buf, pass_id,
                                                     dependencies.shadow_depth_pass_container,
                                                     dependencies.material_renderer_dependencies);
}

void renderVelocityPass(vk::CommandBuffer cmd_buf, PassId pass_id,
                        const RenderPassDispatchDependencies &dependencies) {
    dependencies.material_renderer.renderVelocity(
        cmd_buf, pass_id, dependencies.velocity_pass_container,
        dependencies.material_renderer_dependencies);
}

} // namespace

void renderUiPass(vk::CommandBuffer cmd_buf, const FrameRenderContext &frame, const PassDefinition &pass_def,
                  vk::Extent2D target_extent, RenderTargetContainer &rt_container,
                  const RenderPassDispatchDependencies &dependencies) {
    if (pass_def.output_color.empty()) {
        throw std::runtime_error("UI pass has no color output: " + pass_def.name);
    }
    if (dependencies.ui_renderer == nullptr || dependencies.ui_renderer_dependencies == nullptr)
        throw std::runtime_error("UI pass requires the ui feature runtime: " + pass_def.name);

    const auto rt_id = pass_def.output_color.front();
    const bool targets_swapchain = isSwapchainRenderTarget(rt_id);
    const vk::ImageView target_view =
        targets_swapchain ? frame.color_attachment
                          : rt_container.getAttachmentImageView(rt_id);
    const vk::ImageView resolve_view =
        !targets_swapchain && rt_container.hasSeparateAttachment(rt_id)
            ? rt_container.getImageView(rt_id)
            : vk::ImageView{};
    const vk::Format target_format = targets_swapchain ? dependencies.swapchain_color_format
                                                       : rt_container.getMetadata(rt_id).format;
    dependencies.ui_renderer->render(cmd_buf, UiDrawRequest{
                                                           .target_view = target_view,
                                                           .target_extent = target_extent,
                                                           .target_format = target_format,
                                                           .load_op = pass_def.color_load_op,
                                                           .store_op = pass_def.color_store_op,
                                                           .clear_color = pass_def.clear_color,
                                                           .resolve_view = resolve_view,
                                                           .samples = pass_def.rasterization_samples,
                                                           .resolve_mode =
                                                               targets_swapchain
                                                                   ? vk::ResolveModeFlagBits::eNone
                                                                   : rt_container.resolveMode(rt_id),
                                                       },
                                    *dependencies.ui_renderer_dependencies);
}

#if PELICAN_WITH_IMGUI
void renderImGuiPass(vk::CommandBuffer cmd_buf, const FrameRenderContext &frame,
                     const PassDefinition &pass_def, vk::Extent2D target_extent,
                     RenderTargetContainer &rt_container,
                     const RenderPassDispatchDependencies &dependencies) {
    if (dependencies.imgui_system == nullptr) {
        throw std::runtime_error("ImGui pass requires an interactive ImGuiSystem");
    }
    if (pass_def.output_color.size() != 1) {
        throw std::runtime_error("ImGui pass requires one color output");
    }
    const auto target = pass_def.output_color.front();
    const bool swapchain = isSwapchainRenderTarget(target);
    dependencies.imgui_system->render(
        cmd_buf,
        swapchain ? frame.color_attachment
                  : rt_container.getAttachmentImageView(target),
        target_extent,
        swapchain ? dependencies.swapchain_color_format : rt_container.getMetadata(target).format,
        !swapchain && rt_container.hasSeparateAttachment(target)
            ? rt_container.getImageView(target)
            : vk::ImageView{},
        swapchain ? vk::ResolveModeFlagBits::eNone
                  : rt_container.resolveMode(target));
}
#endif

void renderDynamicPassDrawCalls(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def,
                                vk::Extent2D target_extent,
                                const RenderPassDispatchDependencies &dependencies) {
    if (pass_def.isMaterial()) {
        renderMaterialPass(cmd_buf, pass_id, pass_def, dependencies);
    } else if (pass_def.isFullscreen()) {
        renderFullscreenPass(cmd_buf, pass_id, pass_def, dependencies);
    } else if (pass_def.isShadowDepth()) {
        renderShadowDepthPass(cmd_buf, pass_id, dependencies);
    } else if (pass_def.isVelocity()) {
        renderVelocityPass(cmd_buf, pass_id, dependencies);
    } else if (pass_def.isDebugDraw()) {
        renderDebugDrawPass(cmd_buf, pass_id, dependencies);
    } else if (pass_def.isDebugText()) {
        renderDebugTextPass(cmd_buf, pass_id, target_extent, dependencies);
    } else {
        throw std::runtime_error("Unsupported dynamic render pass type: " + pass_def.name);
    }
}

} // namespace Pelican
