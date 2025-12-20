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
#include "util.hpp"
#include "battery/embed.hpp"
#include <filesystem>
#include <cmath>
#include <chrono>

namespace Pelican {

Renderer::Renderer() : device{GET_MODULE(VulkanManageCore).getDevice()} {
    auto& rt_container = GET_MODULE(RenderTargetContainer);
    auto& shader_container = GET_MODULE(ShaderContainer);
    auto& pass_container = GET_MODULE(RenderingPassContainer);
    auto& rt_main = GET_MODULE(RenderTarget); // Get the main RenderTarget module

    // Load the unified main rendering configuration JSON.
    // This JSON now defines all render targets and passes, including bloom.
    try {
        std::string main_config_path = "main_rendering_config.json";
        if (std::filesystem::exists(main_config_path)) {
            // This registerRenderingPassFromJson should also handle render target registration
            // as per the new unified JSON format.
            // Assuming RenderingPassContainer::registerRenderingPassFromJson is extended
            // to process both "render_targets" and "rendering_passes".
            // If not, a custom parser function would be needed here.
            pass_container.registerRenderingPassFromJson(main_config_path);
        } else {
            LOG_ERROR(logger, "Main rendering configuration JSON file not found: {}", main_config_path);
            // Handle error: e.g., throw exception or load a default minimal config
        }
    } catch (const std::exception& e) {
        LOG_ERROR(logger, "Failed to load main rendering configuration: {}", e.what());
        // Handle error
    }

    // Now that passes are loaded, get the ID for "lit_color" which serves as the scene color RT.
    // This assumes "lit_color" is consistently used as the scene color output before post-processing.
    m_scene_color_rt_id = rt_container.getRenderTargetIdByName("lit_color");

}

Renderer::~Renderer() {}

void Renderer::render() {
    static bool is_first_frame = true;
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
        GET_MODULE(LightContainer).UpdateAnimation(time);
    }
    // ----------------------------------------------------

    const auto render_ctx = rt.render_begin();
    const auto cmd_buf = render_ctx.cmd_buf;

    RenderingPassId rendering_pass_id{0};
    const auto passes = pass_container.getPasses(rendering_pass_id);
    const size_t pass_count = pass_container.getPassCount(rendering_pass_id);

    for (size_t i = 0; i < pass_count; ++i) {
        const auto& pass_def = pass_container.getPassDefinition(rendering_pass_id, i);
        const auto pass_id = passes[i];

        // Determine target extent for this pass
        vk::Extent2D target_extent = render_ctx.extent; // Default to swapchain extent
        if (!pass_def.output_color.empty() && pass_def.output_color[0].value >= 0) {
             target_extent = vk::Extent2D(rt_container.get(pass_def.output_color[0]).image.extent.width, rt_container.get(pass_def.output_color[0]).image.extent.height);
        }

        // マテリアルパス開始前：レイアウト遷移（複数出力対応）
        if (pass_def.type == PassType::eMaterial) {
            for (const auto& rt_id : pass_def.output_color) {
                if (rt_id.value < 0) continue;  // swapchain等は対象外
                
                const auto& offscreen_rt = rt_container.get(rt_id);
                
                // 毎フレーム確実にカラー用レイアウトへ戻す
                const vk::ImageLayout old_layout = is_first_frame ? vk::ImageLayout::eUndefined
                                                                   : vk::ImageLayout::eShaderReadOnlyOptimal;
                vk_utils.changeImageLayoutCmd(cmd_buf, offscreen_rt.image,
                    old_layout, vk::ImageLayout::eColorAttachmentOptimal,
                    {vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                     vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eColorAttachmentWrite});
            }
            is_first_frame = false;
        }

        if (pass_def.type == PassType::eUi) {
            if (!pass_def.output_color.empty()) {
                const auto rt_id = pass_def.output_color.front();
                vk::ImageView target_view = (rt_id.value < 0) ? render_ctx.color_attachment
                                                              : rt_container.get(rt_id).image_view.get();
                ui_renderer.render(cmd_buf, UiDrawRequest{target_view, render_ctx.extent});
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
                                 static_cast<float>(render_ctx.extent.width), 
                                 static_cast<float>(render_ctx.extent.height), 
                                 0.0f, 1.0f};
            cmd_buf.setViewport(0, viewport);
            
            vk::Rect2D scissor{{0, 0}, render_ctx.extent};
            cmd_buf.setScissor(0, scissor);

            // パスタイプに応じてレンダリング
            if (pass_def.type == PassType::eMaterial) {
                mat_renderer.render(cmd_buf, pass_id);
            } else if (pass_def.type == PassType::eFullscreen) {
                fs_renderer.render(cmd_buf, pass_id);
            }

            cmd_buf.endRendering();

            // Transition fullscreen pass outputs to be readable by the next pass
            if (pass_def.type == PassType::eFullscreen) {
                for (const auto& rt_id : pass_def.output_color) {
                    if (rt_id.value < 0) continue; // Don't transition swapchain
                    
                    const auto& output_rt = rt_container.get(rt_id);
                    vk_utils.changeImageLayoutCmd(cmd_buf, output_rt.image,
                        vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                        {vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
                         vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead});
                }
            }

            // マテリアルパス終了後：出力テクスチャを SHADER_READ_ONLY_OPTIMAL に遷移
            if (pass_def.type == PassType::eMaterial) {
                for (const auto& rt_id : pass_def.output_color) {
                    // Skip if it's the scene color RT, as it's handled after the loop
                    if (rt_id == m_scene_color_rt_id) continue;
                    if (rt_id.value < 0) continue;
                    
                    const auto& output_rt = rt_container.get(rt_id);
                    vk_utils.changeImageLayoutCmd(cmd_buf, output_rt.image,
                        vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                        {vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
                            vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead});
                }
            }
        }
    }

    



    rt.render_end();
}

} // namespace Pelican