#include "openxrmirrorsink.hpp"

#include "openxrcompositiontarget.hpp"
#include "../appflow/enginetime.hpp"
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
#include "../vkcore/core.hpp"
#include "../vkcore/debugutils.hpp"
#include "../vkcore/rendertiming.hpp"
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

XrMirrorBeginAction classifyMirrorBeginResult(
    const FrameBeginResult &result) noexcept {
    switch (result.disposition) {
    case FrameBeginDisposition::ready:
        return XrMirrorBeginAction::present;
    case FrameBeginDisposition::unavailable:
        return XrMirrorBeginAction::drop;
    case FrameBeginDisposition::device_rebuild_required:
    case FrameBeginDisposition::fatal:
        return XrMirrorBeginAction::disable;
    }
    return XrMirrorBeginAction::disable;
}

namespace {

// tryPresent() used to track its unwinding with four locals (`frame_token`,
// `rendering_begun`, `timing_begun`, `submission_lease`) across eight exit
// points, and to repeat the same 14-line cleanup in both catch handlers.
// These three guards own that unwinding instead: each undoes its own step
// unless the body hands it off, and each destructor swallows -- tryPresent()
// is noexcept, and the old handlers wrapped their cleanup in catch(...) too.
//
// Declaration order in tryPresent() is token -> timing -> rendering, so the
// unwind ends the rendering scope before it abandons the frame, which is the
// order the old handlers used. cancelGpuRange() records nothing into the
// command buffer, so where it falls in that sequence is not observable.

// The acquired frame. submit() takes it through release(); leaving the scope
// any other way abandons it.
class ScopedFrameToken {
    RenderTarget *target_;
    std::optional<FrameTargetFrame> frame_;

  public:
    ScopedFrameToken(RenderTarget &target, FrameTargetFrame &&frame)
        : target_{&target}, frame_{std::move(frame)} {}
    ScopedFrameToken(const ScopedFrameToken &) = delete;
    ScopedFrameToken &operator=(const ScopedFrameToken &) = delete;
    ~ScopedFrameToken() {
        if (!frame_) return;
        try {
            target_->abandon(release());
        } catch (...) {
        }
    }

    const FrameRenderContext &context() const { return frame_->context(); }
    FrameTargetFrame release() {
        auto token = std::move(*frame_);
        frame_.reset();
        return token;
    }
};

// The GPU timing range opened by beginGpuRange(). end() closes it; leaving the
// scope any other way cancels it. A null RenderTiming means the range was
// never opened and the guard is inert.
class ScopedGpuTimingRange {
    RenderTiming *timing_;

  public:
    explicit ScopedGpuTimingRange(RenderTiming *timing) noexcept : timing_{timing} {}
    ScopedGpuTimingRange(const ScopedGpuTimingRange &) = delete;
    ScopedGpuTimingRange &operator=(const ScopedGpuTimingRange &) = delete;
    ~ScopedGpuTimingRange() {
        if (timing_ == nullptr) return;
        try {
            timing_->cancelGpuRange();
        } catch (...) {
        }
    }
    void end() {
        if (auto *timing = std::exchange(timing_, nullptr); timing != nullptr)
            timing->endGpuRange();
    }
};

// cmd.beginRendering() ... cmd.endRendering().
class ScopedRendering {
    vk::CommandBuffer cmd_;
    bool begun_ = false;

  public:
    ScopedRendering(vk::CommandBuffer cmd, const vk::RenderingInfo &rendering)
        : cmd_{cmd} {
        cmd_.beginRendering(rendering);
        begun_ = true;
    }
    ScopedRendering(const ScopedRendering &) = delete;
    ScopedRendering &operator=(const ScopedRendering &) = delete;
    ~ScopedRendering() {
        if (!begun_) return;
        try {
            cmd_.endRendering();
        } catch (...) {
        }
    }
    void end() {
        if (!std::exchange(begun_, false)) return;
        cmd_.endRendering();
    }
};

} // namespace

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
    if (target.caps().compile_facts.target_kind !=
        OutputTargetKind::window) {
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
    const auto latest =
        GET_MODULE(RenderTargetContainer)
            .getRenderTargetIdByName(
                std::string{xr_mirror_intermediate_name});
    if (!isConcreteRenderTarget(latest)) {
        throw std::runtime_error(
            "engine-owned OpenXR mirror intermediate is unavailable");
    }
    source_id = latest;
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
    GpuSubmissionLease submission_lease;
    RenderTiming *render_timing = FastModuleContainer::tryGet<RenderTiming>();
    try {
        // The module lookups used to be spread over eleven sites inside the
        // body. They are resolved once here, inside the try so a lookup
        // failure still disables the mirror rather than terminating this
        // noexcept function. The two UI modules stay behind the `screen_ui`
        // test below: they exist only when the ui feature is enabled.
        auto &frame_graph_runtime = GET_MODULE(FrameGraphRuntimeContainer);
        auto &render_targets = GET_MODULE(RenderTargetContainer);
        const auto &debug_utils = GET_MODULE(VulkanManageCore).getDebugUtils();
        const auto frame_index = GET_MODULE(EngineTime).evaluationIndex();
        auto &fullscreen_renderer = GET_MODULE(FullscreenPassRenderer);
        auto &fullscreen_passes = GET_MODULE(FullscreenPassContainer);
        auto &frame_resources = GET_MODULE(FrameResources);

        const auto runtime_generation = frame_graph_runtime.snapshot();
        submission_lease = runtime_generation;
        auto begun = target.beginFrame(
            runtime_generation,
            submission_lease,
            FrameBeginMode::nonblocking);
        const auto action =
            classifyMirrorBeginResult(begun);
        if (action == XrMirrorBeginAction::drop) {
            ++stats.dropped;
            stats.last_drop_reason = begun.reason;
            reportProgress();
            return;
        }
        if (action == XrMirrorBeginAction::disable || !begun.frame) {
            ++stats.failures;
            disabled = true;
            LOG_WARNING(
                logger,
                "OpenXR mirror disabled after terminal frame-begin result: "
                "disposition={} reason={}",
                static_cast<int>(begun.disposition),
                frameUnavailableReasonName(begun.reason));
            reportProgress();
            return;
        }
        ScopedFrameToken frame_token{target, std::move(*begun.frame)};
        const auto &frame = frame_token.context();
        const vk::CommandBuffer cmd = frame.cmd_buf;
        rebindSourceIfNeeded();
        const auto source_extent = render_targets.getMetadata(source_id).extent;
        const auto letterbox = mirrorLetterboxRect(source_extent, frame.extent);
        if (!letterbox) {
            target.submit(frame_token.release());
            ++stats.dropped;
            reportProgress();
            return;
        }
        std::string node_debug_name;
        if (debug_utils.commandLabelsEnabled()) {
            node_debug_name = makeFrameGraphDebugLabel(FrameGraphDebugLabelIdentity{
                frame_index, "xr", xr_stereo_view_count, 0,
                "mirror", "output_transform"});
        }
        if (render_timing != nullptr) {
            render_timing->beginGpuRange(
                cmd, frame.in_flight_frame_index, 3,
                GpuTimingRangeIdentity{frame_index, "xr", xr_stereo_view_count},
                {GpuTimingNodeDescriptor{0, "mirror", "output_transform", true}});
        }
        // Armed only once beginGpuRange has returned, which is what the
        // `timing_begun` flag used to say.
        ScopedGpuTimingRange timing_range{render_timing};
        ScopedCommandDebugLabel node_label{debug_utils, cmd, node_debug_name.c_str()};
        // The barrier subrange is still timed even though the pass that used
        // to record barriers here is gone; the empty debug-label scope that
        // wrapped nothing between these two writes was deleted with it.
        if (render_timing != nullptr) {
            render_timing->writeNodeSubrangeStart(
                cmd, 0, GpuTimingSubrange::barriers);
            render_timing->writeNodeSubrangeEnd(
                cmd, 0, GpuTimingSubrange::barriers);
        }
        ScopedCommandDebugLabel body_label{debug_utils, cmd, "body"};
        if (render_timing != nullptr) {
            render_timing->writeNodeSubrangeStart(cmd, 0, GpuTimingSubrange::body);
        }

        {
            vk::RenderingAttachmentInfo attachment;
            attachment.imageView = frame.color_attachment;
            attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
            attachment.loadOp = vk::AttachmentLoadOp::eClear;
            attachment.storeOp = vk::AttachmentStoreOp::eStore;
            attachment.clearValue.color =
                vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}};
            vk::RenderingInfo rendering;
            rendering.renderArea = vk::Rect2D{{0, 0}, frame.extent};
            rendering.layerCount = 1;
            rendering.setColorAttachments(attachment);
            ScopedRendering rendering_scope{cmd, rendering};
            const auto &rect = *letterbox;
            cmd.setViewport(0, vk::Viewport{
                                   static_cast<float>(rect.offset.x),
                                   static_cast<float>(rect.offset.y),
                                   static_cast<float>(rect.extent.width),
                                   static_cast<float>(rect.extent.height), 0.0f, 1.0f});
            cmd.setScissor(0, rect);
            fullscreen_renderer.render(
                cmd, output_transform->pass_id, output_transform->definition,
                FullscreenPassRendererDependencies{fullscreen_passes,
                                                   frame_resources});
            rendering_scope.end();
        }

        if (screen_ui) {
            GET_MODULE(UiRenderer).render(
                cmd,
                UiDrawRequest{
                    .target_view = frame.color_attachment,
                    .target_extent = frame.extent,
                    .target_format = target.getSwapchainFormat(),
                    .load_op = vk::AttachmentLoadOp::eLoad,
                    .store_op = vk::AttachmentStoreOp::eStore,
                    .clear_color =
                        vk::ClearColorValue{
                            std::array{0.0f, 0.0f, 0.0f, 0.0f}},
                },
                UiRendererDependencies{GET_MODULE(UIContainer),
                                       GET_MODULE(ui::UiModule),
                                       frame_resources});
        }
        if (render_timing != nullptr) {
            render_timing->writeNodeSubrangeEnd(cmd, 0, GpuTimingSubrange::body);
        }
        body_label.end();
        node_label.end();
        target.submit(frame_token.release());
        timing_range.end();
        ++stats.presented;
        reportProgress();
    } catch (...) {
        // One handler, not two: the guards above have already ended the
        // rendering scope, cancelled the timing range and abandoned the frame
        // by the time control arrives here, so all that is left is the
        // bookkeeping the two old copies shared and the one line they did not.
        ++stats.failures;
        disabled = true;
        try {
            throw;
        } catch (const std::exception &e) {
            LOG_WARNING(logger, "OpenXR mirror disabled after presentation failure: {}",
                        e.what());
        } catch (...) {
            LOG_WARNING(logger, "OpenXR mirror disabled after unknown presentation failure");
        }
        reportProgress();
    }
}

} // namespace Pelican::OpenXr
