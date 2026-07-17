#include "openxrmirrorsink.hpp"

#include "openxrfeaturepolicy.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"
#include "../renderer/frameresources.hpp"
#include "../renderer/fullscreenpassrenderer.hpp"
#include "../renderer/uicontainer.hpp"
#include "../renderer/uirenderer.hpp"
#include "../renderingpass/computetask.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/renderingpassruntimecompiler.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../renderingpass/rendertargetimageviewresolver.hpp"
#include "../renderingpass/rendertargetmetadataresolver.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../ui/module.hpp"
#include "../vkcore/rendertarget.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Pelican::OpenXr {

std::optional<vk::Rect2D> mirrorLetterboxRect(vk::Extent2D source,
                                              vk::Extent2D destination) {
    if (source.width == 0 || source.height == 0 || destination.width == 0 ||
        destination.height == 0) {
        return std::nullopt;
    }
    const double scale = std::min(
        static_cast<double>(destination.width) / static_cast<double>(source.width),
        static_cast<double>(destination.height) / static_cast<double>(source.height));
    const auto width = std::max(
        1u, std::min(destination.width,
                     static_cast<std::uint32_t>(std::floor(source.width * scale))));
    const auto height = std::max(
        1u, std::min(destination.height,
                     static_cast<std::uint32_t>(std::floor(source.height * scale))));
    return vk::Rect2D{
        vk::Offset2D{static_cast<std::int32_t>((destination.width - width) / 2),
                     static_cast<std::int32_t>((destination.height - height) / 2)},
        vk::Extent2D{width, height}};
}

XrMirrorSink::XrMirrorSink() noexcept {
    try {
        initialize();
    } catch (const std::exception &e) {
        disabled = true;
        LOG_WARNING(logger, "OpenXR mirror disabled during startup: {}", e.what());
    } catch (...) {
        disabled = true;
        LOG_WARNING(logger, "OpenXR mirror disabled during startup: unknown error");
    }
}

void XrMirrorSink::initialize() {
    auto &target = GET_MODULE(RenderTarget);
    if (!target.caps().presents) {
        disabled = true;
        return;
    }

    auto &targets = GET_MODULE(RenderTargetContainer);
    source_id = targets.getRenderTargetIdByName(
        std::string{xr_mirror_intermediate_name});
    if (!isConcreteRenderTarget(source_id)) {
        throw std::runtime_error("engine-owned OpenXR mirror intermediate is unavailable");
    }

    PassDefinition pass;
    // Keep the canonical name so the runtime compiler applies the same UNORM
    // fallback color encoding as the flat presentation graph when required.
    pass.name = "output_transform";
    pass.output_color = {swapchainRenderTargetId()};
    pass.input_targets = {source_id};
    pass.input_target_history = {false};
    FullscreenPassInfo fullscreen;
    fullscreen.vert_shader =
        makeShaderReference("engine://fullscreen", ShaderStage::vertex);
    fullscreen.frag_shader =
        makeShaderReference("engine://output_transform", ShaderStage::fragment);
    pass.pass_info = std::move(fullscreen);

    const RenderTargetMetadataResolver metadata{targets};
    const RenderTargetImageViewResolver views{targets};
    const RenderingPassDefinition definition{
        "__xr_mirror_sink", std::vector<PassDefinition>{std::move(pass)}};
    auto compiled = compileRenderingPassRuntime(
        definition,
        RenderingPassRuntimeDependencies{
            .render_target = &target,
            .render_target_metadata = &metadata,
            .render_target_views = &views,
            .shader_library = &GET_MODULE(ShaderLibrary),
            .fullscreen_pass_container = &GET_MODULE(FullscreenPassContainer),
            .frame_graph_resources = &GET_MODULE(FrameGraphResourceContainer),
            .path_resolver = &GET_MODULE(PathResolver),
            .warn_backend_specific_shader_refs =
                GET_MODULE(ProjectBasicConfig).usesProjectSource(),
        });
    if (compiled.passes.size() != 1) {
        throw std::runtime_error("OpenXR mirror output transform did not compile");
    }
    output_transform = std::move(compiled.passes.front());
    screen_ui = GET_MODULE(RenderingPassContainer).isFeatureEnabled("ui");
}

void XrMirrorSink::rebindSourceIfNeeded() {
    auto &fullscreen = GET_MODULE(FullscreenPassContainer);
    const auto bound =
        fullscreen.boundInputImageViewsForTesting(output_transform->pass_id);
    const auto expected = GET_MODULE(RenderTargetContainer).getImageView(source_id);
    if (bound.size() != 1 || bound.front() != expected) {
        const RenderTargetImageViewResolver views{GET_MODULE(RenderTargetContainer)};
        fullscreen.setInputTextures(output_transform->pass_id, {source_id}, views);
    }
}

void XrMirrorSink::reportProgress() {
    const auto attempts = stats.presented + stats.dropped + stats.failures;
    if (!first_outcome_reported) {
        first_outcome_reported = true;
        LOG_INFO(logger,
                 "OpenXR mirror diagnostic: milestone=first_frame attempts={} "
                 "presented={} dropped={} failures={}",
                 attempts, stats.presented, stats.dropped, stats.failures);
    }
    if (!first_present_reported && stats.presented != 0) {
        first_present_reported = true;
        LOG_INFO(logger,
                 "OpenXR mirror diagnostic: milestone=first_present attempts={} "
                 "presented={} dropped={} failures={}",
                 attempts, stats.presented, stats.dropped, stats.failures);
    }
    if (!thousand_frames_reported && attempts >= 1000) {
        thousand_frames_reported = true;
        LOG_INFO(logger,
                 "OpenXR mirror diagnostic: milestone=1000_xr_frames attempts={} "
                 "presented={} dropped={} failures={}",
                 attempts, stats.presented, stats.dropped, stats.failures);
    }
}

void XrMirrorSink::tryPresent() noexcept {
    if (disabled || !output_transform) {
        ++stats.dropped;
        reportProgress();
        return;
    }

    auto &target = GET_MODULE(RenderTarget);
    bool frame_begun = false;
    bool rendering_begun = false;
    vk::CommandBuffer cmd;
    try {
        auto frame = target.tryRenderBegin();
        if (!frame) {
            ++stats.dropped;
            reportProgress();
            return;
        }
        frame_begun = true;
        cmd = frame->cmd_buf;
        const auto source_extent =
            GET_MODULE(RenderTargetContainer).getMetadata(source_id).extent;
        const auto letterbox = mirrorLetterboxRect(source_extent, frame->extent);
        if (!letterbox) {
            frame_begun = false;
            target.render_end();
            ++stats.dropped;
            reportProgress();
            return;
        }
        rebindSourceIfNeeded();

        vk::RenderingAttachmentInfo attachment;
        attachment.imageView = frame->color_attachment;
        attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        attachment.loadOp = vk::AttachmentLoadOp::eClear;
        attachment.storeOp = vk::AttachmentStoreOp::eStore;
        attachment.clearValue.color =
            vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}};
        vk::RenderingInfo rendering;
        rendering.renderArea = vk::Rect2D{{0, 0}, frame->extent};
        rendering.layerCount = 1;
        rendering.setColorAttachments(attachment);
        cmd.beginRendering(rendering);
        rendering_begun = true;
        const auto &rect = *letterbox;
        cmd.setViewport(0, vk::Viewport{
                               static_cast<float>(rect.offset.x),
                               static_cast<float>(rect.offset.y),
                               static_cast<float>(rect.extent.width),
                               static_cast<float>(rect.extent.height), 0.0f, 1.0f});
        cmd.setScissor(0, rect);
        GET_MODULE(FullscreenPassRenderer)
            .render(cmd, output_transform->pass_id, output_transform->definition,
                    FullscreenPassRendererDependencies{
                        GET_MODULE(FullscreenPassContainer), GET_MODULE(FrameResources)});
        cmd.endRendering();
        rendering_begun = false;

        if (screen_ui) {
            GET_MODULE(UiRenderer).render(
                cmd,
                UiDrawRequest{frame->color_attachment, frame->extent,
                              target.getSwapchainFormat(), vk::AttachmentLoadOp::eLoad,
                              vk::AttachmentStoreOp::eStore,
                              vk::ClearColorValue{
                                  std::array{0.0f, 0.0f, 0.0f, 0.0f}}},
                UiRendererDependencies{GET_MODULE(UIContainer),
                                       GET_MODULE(ui::UiModule),
                                       GET_MODULE(FrameResources)});
        }
        frame_begun = false;
        target.render_end();
        ++stats.presented;
        reportProgress();
    } catch (const std::exception &e) {
        ++stats.failures;
        disabled = true;
        LOG_WARNING(logger, "OpenXR mirror disabled after presentation failure: {}", e.what());
        reportProgress();
        try {
            if (rendering_begun) cmd.endRendering();
            if (frame_begun) target.render_end();
        } catch (...) {
        }
    } catch (...) {
        ++stats.failures;
        disabled = true;
        LOG_WARNING(logger, "OpenXR mirror disabled after unknown presentation failure");
        reportProgress();
        try {
            if (rendering_begun) cmd.endRendering();
            if (frame_begun) target.render_end();
        } catch (...) {
        }
    }
}

} // namespace Pelican::OpenXr
