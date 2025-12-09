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
#include "core.hpp"
#include "rendertarget.hpp"
#include "util.hpp"
#include "battery/embed.hpp"
#include <filesystem>

namespace Pelican {

Renderer::Renderer() : device{GET_MODULE(VulkanManageCore).getDevice()} {
    auto& rt_container = GET_MODULE(RenderTargetContainer);
    auto& shader_container = GET_MODULE(ShaderContainer);
    auto& pass_container = GET_MODULE(RenderingPassContainer);

    try {
        // JSONファイルが存在するか確認
        std::string json_path = "example_renderingpass_data.json";
        if (std::filesystem::exists(json_path)) {
            pass_container.registerRenderingPassFromJson(json_path);
        } else {
            LOG_WARNING(logger, "Rendering pass JSON file not found: {}", json_path);
        }
    } catch (const std::exception& e) {
        LOG_ERROR(logger, "Failed to load rendering pass: {}", e.what());
    }
}

Renderer::~Renderer() {}

void Renderer::render() {
    static bool is_first_frame = true;
    
    auto &rt = GET_MODULE(RenderTarget);
    auto &mat_renderer = GET_MODULE(MaterialRenderer);
    auto &ui_renderer = GET_MODULE(UiRenderer);
    auto &fs_renderer = GET_MODULE(FullscreenPassRenderer);
    auto &pass_container = GET_MODULE(RenderingPassContainer);
    auto &rt_container = GET_MODULE(RenderTargetContainer);
    auto &vk_utils = GET_MODULE(VulkanUtils);

    const auto render_ctx = rt.render_begin();
    const auto cmd_buf = render_ctx.cmd_buf;

    RenderingPassId rendering_pass_id{0};
    const auto passes = pass_container.getPasses(rendering_pass_id);
    const size_t pass_count = pass_container.getPassCount(rendering_pass_id);

    for (size_t i = 0; i < pass_count; ++i) {
        const auto& pass_def = pass_container.getPassDefinition(rendering_pass_id, i);
        const auto pass_id = passes[i];

        // ジオメトリパス開始前：初フレームはUNDEFINEDから、2フレーム目以降はSHADER_READ_ONLY_OPTIMALから遷移
        if (pass_def.type == PassType::eMaterial) {
            const auto& offscreen_rt = rt_container.get(pass_def.output_color);
            
            if (is_first_frame) {
                // 初フレーム：UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
                vk_utils.changeImageLayoutCmd(cmd_buf, offscreen_rt.image,
                    vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                    {vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                     vk::AccessFlagBits::eNone, vk::AccessFlagBits::eColorAttachmentWrite});
                is_first_frame = false;
            } else if (i > 0) {
                // フレーム2以降：SHADER_READ_ONLY_OPTIMAL -> COLOR_ATTACHMENT_OPTIMAL
                vk_utils.changeImageLayoutCmd(cmd_buf, offscreen_rt.image,
                    vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eColorAttachmentOptimal,
                    {vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                     vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eColorAttachmentWrite});
            }
        }

        // レンダーターゲット取得
        // スワップチェーン出力の場合はスキップ
        bool is_swapchain_output = (pass_def.output_color.value < 0);

        // カラーアタッチメント設定
        vk::RenderingAttachmentInfo color_attachment;
        if (is_swapchain_output) {
            // スワップチェーン出力
            color_attachment.imageView = render_ctx.color_attachment;
        } else {
            // オフスクリーン出力
            const auto& output_rt = rt_container.get(pass_def.output_color);
            color_attachment.imageView = output_rt.image_view.get();
        }
        color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        color_attachment.loadOp = pass_def.color_load_op;
        color_attachment.storeOp = pass_def.color_store_op;
        color_attachment.clearValue.color = pass_def.clear_color;

        vk::RenderingInfo render_info;
        render_info.renderArea = vk::Rect2D{{0, 0}, render_ctx.extent};
        render_info.layerCount = 1;
        render_info.setColorAttachments(color_attachment);

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

        // ジオメトリパス終了後：次がフルスクリーンパスなら SHADER_READ_ONLY_OPTIMAL に遷移
        if (pass_def.type == PassType::eMaterial && i < pass_count - 1) {
            const auto& next_pass_def = pass_container.getPassDefinition(rendering_pass_id, i + 1);
            if (next_pass_def.type == PassType::eFullscreen) {
                const auto& output_rt = rt_container.get(pass_def.output_color);
                vk_utils.changeImageLayoutCmd(cmd_buf, output_rt.image,
                    vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    {vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
                     vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead});
            }
        }
        // フルスクリーンパス終了後：offscreen_colorをCOLOR_ATTACHMENT_OPTIMALに戻す（次フレーム用）
        else if (pass_def.type == PassType::eFullscreen && i > 0) {
            const auto& prev_pass_def = pass_container.getPassDefinition(rendering_pass_id, i - 1);
            if (prev_pass_def.type == PassType::eMaterial) {
                const auto& offscreen_rt = rt_container.get(prev_pass_def.output_color);
                vk_utils.changeImageLayoutCmd(cmd_buf, offscreen_rt.image,
                    vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eColorAttachmentOptimal,
                    {vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                     vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eColorAttachmentWrite});
            }
        }
    }

    rt.render_end();
}

} // namespace Pelican