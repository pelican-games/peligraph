#include "renderer.hpp"
#include "../log.hpp"
#include "../renderer/materialrender.hpp"
#include "../renderer/uirenderer.hpp"
#include "../renderer/fullscreenpassrenderer.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../shader/shadercontainer.hpp"
#include "../loader/basicconfig.hpp"
#include "../light/lightcontainer.hpp"
#include "core.hpp"
#include "rendertarget.hpp"
#include "battery/embed.hpp"
#include <filesystem>
#include <cmath>
#include <chrono>

namespace Pelican {

Renderer::Renderer() : device{GET_MODULE(VulkanManageCore).getDevice()} {
    auto& pass_container = GET_MODULE(RenderingPassContainer);
    const auto& config = GET_MODULE(ProjectBasicConfig);

    // Load the unified main rendering configuration JSON.
    // This JSON now defines all render targets and passes, including bloom.
    try {
        const auto main_config_path = config.renderingConfigJson();
        if (std::filesystem::exists(main_config_path)) {
            // This registerRenderingPassFromJson should also handle render target registration
            // as per the new unified JSON format.
            // Assuming RenderingPassContainer::registerRenderingPassFromJson is extended
            // to process both "render_targets" and "rendering_passes".
            // If not, a custom parser function would be needed here.
            pass_container.registerRenderingPassFromJson(main_config_path);
        } else {
            throw std::runtime_error("Main rendering configuration JSON file not found: " + main_config_path);
        }
    } catch (const std::exception& e) {
        LOG_ERROR(logger, "Failed to load main rendering configuration: {}", e.what());
        throw;
    }

    current_rendering_pass_id = pass_container.getRenderingPassIdByName(config.defaultRenderingPass());
    if (current_rendering_pass_id.value < 0) {
        throw std::runtime_error("Rendering pass not found: " + config.defaultRenderingPass());
    }
}

Renderer::~Renderer() {}

void Renderer::render() {
    static auto start_time = std::chrono::high_resolution_clock::now();
    
    auto &rt = GET_MODULE(RenderTarget);
    auto &mat_renderer = GET_MODULE(MaterialRenderer);
    auto &ui_renderer = GET_MODULE(UiRenderer);
    auto &fs_renderer = GET_MODULE(FullscreenPassRenderer);
    auto &pass_container = GET_MODULE(RenderingPassContainer);
    auto &rt_container = GET_MODULE(RenderTargetContainer);
    auto &vk_utils = GET_MODULE(VulkanUtils);

    // --- Temporary Light Animation (easily removable) ---
    {
        auto current_time = std::chrono::high_resolution_clock::now();
        float time = std::chrono::duration<float, std::chrono::seconds::period>(current_time - start_time).count();
        GET_MODULE(LightContainer).updateAnimation(time);
    }
    // ----------------------------------------------------

    const auto render_ctx = rt.render_begin();
    const auto cmd_buf = render_ctx.cmd_buf;

    const auto passes = pass_container.getPasses(current_rendering_pass_id);

    for (size_t i = 0; i < passes.size(); ++i) {
        const auto& pass_def = pass_container.getPassDefinition(current_rendering_pass_id, i);
        const auto pass_id = passes[i];

        // Determine target extent for this pass
        vk::Extent2D target_extent = render_ctx.extent; // Default to swapchain extent
        if (!pass_def.output_color.empty() && pass_def.output_color[0].value >= 0) {
             target_extent = vk::Extent2D(rt_container.get(pass_def.output_color[0]).image.extent.width, rt_container.get(pass_def.output_color[0]).image.extent.height);
        }

        for (const auto& rt_id : pass_def.output_color) {
            render_target_layout_tracker.transition(cmd_buf, rt_container, vk_utils, rt_id,
                                                    vk::ImageLayout::eColorAttachmentOptimal);
        }
        render_target_layout_tracker.transition(cmd_buf, rt_container, vk_utils, pass_def.output_depth,
                                                vk::ImageLayout::eDepthAttachmentOptimal);

        if (pass_def.type == PassType::eUi) {
            if (!pass_def.output_color.empty()) {
                const auto rt_id = pass_def.output_color.front();
                vk::ImageView target_view = (rt_id.value < 0) ? render_ctx.color_attachment
                                                              : rt_container.get(rt_id).image_view.get();
                ui_renderer.render(cmd_buf, UiDrawRequest{target_view, target_extent});
                for (const auto& output_rt_id : pass_def.output_color) {
                    render_target_layout_tracker.transition(cmd_buf, rt_container, vk_utils, output_rt_id,
                                                            vk::ImageLayout::eShaderReadOnlyOptimal);
                }
            }
        } else {
            // 複数のカラーアタッチメント設定
            std::vector<vk::RenderingAttachmentInfo> color_attachments;
            for (const auto& rt_id : pass_def.output_color) {
                vk::RenderingAttachmentInfo color_att;
                
                if (rt_id.value < 0) {
                    // スワップチェーン出力
                    color_att.imageView = render_ctx.color_attachment;
                } else {
                    // オフスクリーン出力
                    const auto& output_rt = rt_container.get(rt_id);
                    color_att.imageView = output_rt.image_view.get();
                }
                
                color_att.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
                color_att.loadOp = pass_def.color_load_op;
                color_att.storeOp = pass_def.color_store_op;
                color_att.clearValue.color = pass_def.clear_color;
                color_attachments.push_back(color_att);
            }

            vk::RenderingInfo render_info;
            render_info.renderArea = vk::Rect2D{{0, 0}, target_extent};
            render_info.layerCount = 1;
            render_info.setColorAttachments(color_attachments);

            // 深度アタッチメント
            vk::RenderingAttachmentInfo depth_attachment;
            if (pass_def.output_depth.value >= 0) {
                const auto& depth_rt = rt_container.get(pass_def.output_depth);
                depth_attachment.imageView = depth_rt.image_view.get();
                depth_attachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
                depth_attachment.loadOp = vk::AttachmentLoadOp::eClear;
                depth_attachment.storeOp = vk::AttachmentStoreOp::eDontCare;
                depth_attachment.clearValue.depthStencil = vk::ClearDepthStencilValue{1.0f, 0};
                render_info.pDepthAttachment = &depth_attachment;
            }

            cmd_buf.beginRendering(render_info);

            // ビューポート設定
            vk::Viewport viewport{0.0f, 0.0f,
                                 static_cast<float>(target_extent.width),
                                 static_cast<float>(target_extent.height),
                                 0.0f, 1.0f};
            cmd_buf.setViewport(0, viewport);
            
            vk::Rect2D scissor{{0, 0}, target_extent};
            cmd_buf.setScissor(0, scissor);

            // パスタイプに応じてレンダリング
            if (pass_def.type == PassType::eMaterial) {
                if (pass_def.material_info.material_count > 0) {
                    mat_renderer.renderWithMaterialRange(cmd_buf, pass_id, pass_def.material_info.material_start,
                                                         pass_def.material_info.material_count);
                } else {
                    mat_renderer.render(cmd_buf, pass_id);
                }
            } else if (pass_def.type == PassType::eFullscreen) {
                fs_renderer.render(cmd_buf, pass_id, pass_def);
            }

            cmd_buf.endRendering();

            for (const auto& rt_id : pass_def.output_color) {
                render_target_layout_tracker.transition(cmd_buf, rt_container, vk_utils, rt_id,
                                                        vk::ImageLayout::eShaderReadOnlyOptimal);
            }
        }
    }

    



    rt.render_end();
}

} // namespace Pelican
