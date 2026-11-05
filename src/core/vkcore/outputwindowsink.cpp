#include "outputwindowsink.hpp"

#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"
#include "../os/outputwindow.hpp"
#include "../renderer/frameresources.hpp"
#include "../renderer/fullscreenpassrenderer.hpp"
#include "../renderingpass/computetask.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/renderingpassruntimecompiler.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../renderingpass/rendertargetimageviewresolver.hpp"
#include "../renderingpass/rendertargetmetadataresolver.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/core.hpp"
#include "render_target_layout_tracker.hpp"
#include "rendertarget.hpp"
#include "swapchainframetarget.hpp"
#include "windowsurface.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <system_error>
#include <thread>

namespace Pelican {
namespace {

std::optional<std::uint64_t> environmentUint(
    const char *name) {
    const char *text = std::getenv(name);
    if (text == nullptr || *text == '\0') {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto *end = text + std::char_traits<char>::length(text);
    const auto parsed =
        std::from_chars(text, end, value);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != end) {
        LOG_INFO(logger, "ignoring malformed {}={}",
                 name, text);
        return std::nullopt;
    }
    return value;
}

class OutputWindowDeviceLost final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

std::string_view shortPresentMode(
    vk::PresentModeKHR mode) noexcept {
    switch (mode) {
    case vk::PresentModeKHR::eMailbox:
        return "MAILBOX";
    case vk::PresentModeKHR::eFifo:
        return "FIFO";
    case vk::PresentModeKHR::eImmediate:
        return "IMMEDIATE";
    case vk::PresentModeKHR::eFifoRelaxed:
        return "FIFO_RELAXED";
    default:
        return "OTHER";
    }
}

} // namespace

std::string_view outputWindowLifecycleStateName(
    OutputWindowLifecycleState state) noexcept {
    switch (state) {
    case OutputWindowLifecycleState::off:
        return "off";
    case OutputWindowLifecycleState::running:
        return "running";
    case OutputWindowLifecycleState::failed:
        return "failed";
    case OutputWindowLifecycleState::closed:
        return "closed";
    }
    return "failed";
}

nlohmann::json outputWindowStatusJson(
    const OutputWindowStatus &status) {
    return nlohmann::json{
        {"state",
         outputWindowLifecycleStateName(
             status.state)},
        {"monitor",
         status.monitor
             ? nlohmann::json(*status.monitor)
             : nlohmann::json(nullptr)},
        {"width", status.width},
        {"height", status.height},
        {"present_mode",
         status.present_mode.empty()
             ? nlohmann::json(nullptr)
             : nlohmann::json(status.present_mode)},
        {"source", status.source},
        {"surface_format", status.surface_format},
        {"presented", status.presented},
        {"dropped", status.dropped},
        {"capture_skipped", status.capture_skipped},
        {"captures", status.captures},
        {"capture_aborted", status.capture_aborted},
        {"capture_fence_waits", status.capture_fence_waits},
        {"last_error",
         status.last_error
             ? nlohmann::json(*status.last_error)
             : nlohmann::json(nullptr)},
        {"copy_gpu_ms_max", status.copy_gpu_ms_max},
        {"copy_gpu_samples", status.copy_gpu_samples},
        {"submit_ms_max", status.submit_ms_max},
        {"present_ms_max", status.present_ms_max},
    };
}

nlohmann::json outputWindowOffStatusJson() {
    return outputWindowStatusJson(
        OutputWindowStatus{});
}

std::optional<vk::Rect2D> outputWindowLetterboxRect(
    vk::Extent2D source, vk::Extent2D destination) {
    if (source.width == 0 || source.height == 0 ||
        destination.width == 0 || destination.height == 0) {
        return std::nullopt;
    }
    const double scale = std::min(
        static_cast<double>(destination.width) / source.width,
        static_cast<double>(destination.height) / source.height);
    const auto width = std::max(1u, std::min(destination.width,
        static_cast<std::uint32_t>(std::floor(source.width * scale))));
    const auto height = std::max(1u, std::min(destination.height,
        static_cast<std::uint32_t>(std::floor(source.height * scale))));
    return vk::Rect2D{
        vk::Offset2D{
            static_cast<std::int32_t>((destination.width - width) / 2),
            static_cast<std::int32_t>((destination.height - height) / 2)},
        vk::Extent2D{width, height}};
}

struct OutputWindowSink::PendingCapture {
    BufferWrapper buffer;
    vk::Extent2D extent;
    vk::Format format = vk::Format::eUndefined;
    std::uint64_t pattern_frame = 0;
};

OutputWindowSink::OutputWindowSink() = default;

OutputWindowSink::~OutputWindowSink() {
    shutdown();
}

void OutputWindowSink::configure(
    const OutputWindowLaunchOptions &options) {
    if (configured_) {
        return;
    }
    configured_ = true;
    options_ = options;
    if (!options_.enabled) {
        state_ =
            OutputWindowLifecycleState::off;
        return;
    }
    monitor_ = options_.monitor;
    present_mode_ = "FIFO";

    if (const auto count = environmentUint(
            "PELICAN_MD1A_BEGIN_UNAVAILABLE_FRAMES")) {
        forced_drop_frames_ = *count;
    }
    if (const auto count = environmentUint(
            "PELICAN_MD1A_BEGIN_UNAVAILABLE_AFTER_FRAMES")) {
        forced_drop_after_frames_ = *count;
    }
    if (const auto count = environmentUint(
            "PELICAN_MD1A_REQUEST_CLOSE_AFTER_FRAMES")) {
        close_after_frames_ = *count;
    }
    forced_capture_busy_frames_ = environmentUint(
        "PELICAN_MD1A_CAPTURE_BUSY_FRAMES").value_or(0);
    forced_abort_before_submit_frames_ = environmentUint(
        "PELICAN_MD1A_ABORT_BEFORE_SUBMIT_FRAMES").value_or(0);
    sink_submission_delay_ms_ = environmentUint(
        "PELICAN_MD1A_DELAY_SINK_GPU_MS").value_or(0);
    force_blocking_begin_ =
        std::getenv(
            "PELICAN_MD1A_FORCE_BLOCKING_BEGIN") !=
        nullptr;
    if (const char *directory = std::getenv(
            "PELICAN_MD1A_PATTERN_DUMP_DIR");
        directory != nullptr && *directory != '\0') {
        dump_directory_ = directory;
        dump_count_ =
            environmentUint(
                "PELICAN_MD1A_PATTERN_DUMP_COUNT")
                .value_or(2);
    }

    core_ = &GET_MODULE(VulkanManageCore);
    try {
        if (options_.enabled && !options_.pattern) {
            const auto queue_families = core_->getPhysDevice().getQueueFamilyProperties();
            const auto queue_family = core_->getGraphicsQueueFamilyIndex();
            if (queue_family < queue_families.size() &&
                queue_families[queue_family].timestampValidBits != 0) {
                copy_query_pool_ = core_->getDevice().createQueryPoolUnique(
                    {vk::QueryPoolCreateFlags{}, vk::QueryType::eTimestamp, 256});
                copy_query_pending_.assign(128, false);
                timestamp_period_ns_ =
                    core_->getPhysDevice().getProperties().limits.timestampPeriod;
            }
        }
    } catch (const std::exception &error) {
        fail("output_window.config_invalid@1: copy timing initialization failed: " +
             std::string{error.what()});
        return;
    }
    try {
        window_ = std::make_unique<OutputWindow>(
            options_.monitor, options_.size);
    } catch (const OutputWindowMonitorUnavailable &error) {
        fail(error.what());
        return;
    } catch (const std::exception &error) {
        fail(
            "output_window.config_invalid@1: output window "
            "creation failed: " +
            std::string{error.what()});
        return;
    }

    try {
        auto surface = WindowSurfaceFactory{
            core_->getInstance(), *window_}
                           .create();
        const auto binding =
            querySurfacePresentationQueue(
                core_->getPhysDevice(),
                surface.surface.get(),
                core_
                    ->getBootstrapPresentationQueueFamilyIndex(),
                core_->createdQueueFamilyIndices());
        if (binding.kind !=
            SurfaceQueueBindingKind::compatible) {
            fail(
                "output_window.present_unsupported@1: output "
                "surface has no created presentation queue (" +
                std::string{
                    surfaceDeviceRebuildReasonName(
                        binding.rebuild_reason)} +
                ")");
            return;
        }
        // The diagnostic window never asks for IMMEDIATE. MAILBOX is the
        // preferred pacing mode; FIFO is the only fallback. A surface that
        // offers neither is a named failure instead of a fallback to a mode
        // that could tear or spin.
        const auto present_modes =
            core_->getPhysDevice()
                .getSurfacePresentModesKHR(
                    surface.surface.get());
        const bool has_mailbox =
            std::find(present_modes.begin(),
                      present_modes.end(),
                      vk::PresentModeKHR::eMailbox) !=
            present_modes.end();
        const bool has_fifo =
            std::find(present_modes.begin(),
                      present_modes.end(),
                      vk::PresentModeKHR::eFifo) !=
            present_modes.end();
        if (!has_mailbox && !has_fifo) {
            fail(
                "output_window.present_mode_unsupported@1: "
                "output surface offers neither MAILBOX nor "
                "FIFO");
            return;
        }
        if (std::getenv("PELICAN_MD1A_UNSIGNALED_SLOT_FENCE") != nullptr) {
            slot_fence_for_testing_ =
                core_->getDevice().createFenceUnique({});
        }
        target_ =
            std::make_unique<SwapchainFrameTarget>(
                ExternalWindowTargetConfig{
                    .window = window_.get(),
                    .surface = std::move(surface),
                    .presentation_queue_family =
                        binding.presentation_queue_family,
                    .slot_fence_override_for_testing =
                        slot_fence_for_testing_.get(),
                });
        const auto facts =
            target_->caps().compile_facts;
        captured_width_ = facts.extent.width;
        captured_height_ = facts.extent.height;
        pattern_capture_available_ =
            facts.capture_available;
        present_mode_ = std::string{
            shortPresentMode(
                target_->status().present_mode)};
        state_ =
            OutputWindowLifecycleState::running;
        if (!options_.pattern) {
            initializeDisplayOutput();
        }
        if (running()) {
            LOG_INFO(logger,
                     "output_window running: monitor={} source={} "
                     "extent={}x{} present_mode={}",
                     options_.monitor,
                     options_.pattern ? "pattern" : "display",
                     captured_width_, captured_height_, present_mode_);
        }
    } catch (const std::exception &error) {
        fail(
            "output_window.present_failed@1: output target "
            "creation failed: " +
            std::string{error.what()});
    }
}

void OutputWindowSink::fail(
    std::string message) noexcept {
    state_ = OutputWindowLifecycleState::failed;
    last_error_ = std::move(message);
    LOG_INFO(logger, "diagnostic output_window failed: {}",
             last_error_);
}

void OutputWindowSink::waitForCaptureForTesting() {
    ++capture_fence_waits_;
    core_->waitIdle();
}

void OutputWindowSink::initializeDisplayOutput() {
    const auto facts = target_->caps().compile_facts;
    const bool supported_format =
        facts.color_format == vk::Format::eR8G8B8A8Unorm ||
        facts.color_format == vk::Format::eR8G8B8A8Srgb ||
        facts.color_format == vk::Format::eB8G8R8A8Unorm ||
        facts.color_format == vk::Format::eB8G8R8A8Srgb;
    if (!supported_format ||
        facts.color_space != vk::ColorSpaceKHR::eSrgbNonlinear) {
        fail("output_window.format_unsupported@1: second-window output "
             "requires an 8-bit SDR RGBA/BGRA UNORM or SRGB surface");
        return;
    }

    auto &targets = GET_MODULE(RenderTargetContainer);
    bridge_id_ = targets.getRenderTargetIdByName(
        std::string{output_window_bridge_name});
    if (!isConcreteRenderTarget(bridge_id_)) {
        fail("output_window.source_unavailable@1: output_window_bridge "
             "was not materialized");
        return;
    }

    PassDefinition pass;
    pass.name = "output_transform";
    pass.output_color = {swapchainRenderTargetId()};
    pass.input_targets = {bridge_id_};
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
        "__output_window_sink", std::vector<PassDefinition>{std::move(pass)}};
    auto compiled = compileRenderingPassRuntime(
        definition,
        RenderingPassRuntimeDependencies{
            .render_target = &GET_MODULE(RenderTarget),
            .render_target_metadata = &metadata,
            .render_target_views = &views,
            .shader_library = &GET_MODULE(ShaderLibrary),
            .fullscreen_pass_container = &GET_MODULE(FullscreenPassContainer),
            .frame_graph_resources = &GET_MODULE(FrameGraphResourceContainer),
            .path_resolver = &GET_MODULE(PathResolver),
            .swapchain_format_override = facts.color_format,
        });
    if (compiled.passes.size() != 1) {
        fail("output_window.format_unsupported@1: output_transform did not "
             "compile for the second-window surface");
        return;
    }
    output_transform_ = std::make_unique<CompiledPass>(
        std::move(compiled.passes.front()));
}

void OutputWindowSink::pollCopyGpuTiming() noexcept {
    if (!copy_query_pool_ || core_ == nullptr) return;
    try {
        for (std::uint32_t slot = 0; slot < copy_query_pending_.size(); ++slot) {
            if (!copy_query_pending_[slot]) continue;
            std::array<std::uint64_t, 4> values{};
            (void)core_->getDevice().getQueryPoolResults(
                copy_query_pool_.get(), slot * 2, 2,
                values.size() * sizeof(values.front()), values.data(),
                2 * sizeof(values.front()),
                vk::QueryResultFlagBits::e64 |
                    vk::QueryResultFlagBits::eWithAvailability);
            if (values[3] == 0 || values[1] == 0) continue;
            const auto begin = values[0];
            const auto end = values[2];
            copy_query_pending_[slot] = false;
            if (end >= begin) {
                const auto elapsed = static_cast<double>(end - begin) *
                                     timestamp_period_ns_ / 1.0e6;
                copy_gpu_ms_max_ = std::max(copy_gpu_ms_max_, elapsed);
                ++copy_gpu_samples_;
            }
        }
    } catch (...) {
    }
}

bool OutputWindowSink::recordDisplayCapture(
    const FrameRenderContext &context,
    RenderTargetContainer &targets,
    VulkanUtils &vk_utils,
    RenderTargetLayoutTracker &layout_tracker,
    GlobalRenderTargetId display,
    vk::ImageLayout next_layout,
    std::shared_ptr<const RendererRuntimeGeneration>
        runtime_generation) noexcept {
    if (!wantsDisplayCapture()) {
        return false;
    }
    if (!output_transform_ || !target_ || !runtime_generation) {
        refuseDisplayCapture();
        return false;
    }
    if (capture_recorded_ || capture_ready_) {
        return false;
    }
    try {
        pollCopyGpuTiming();
        if (forced_capture_busy_frames_ > 0) {
            --forced_capture_busy_frames_;
            ++capture_skipped_;
            return false;
        }
        if (!target_->gpuSubmissionsComplete()) {
            ++capture_skipped_;
            return false;
        }
        if (!isConcreteRenderTarget(display)) {
            refuseDisplayCapture();
            return false;
        }
        const auto latest_bridge = targets.getRenderTargetIdByName(
            std::string{output_window_bridge_name});
        const auto source = targets.getMetadata(display);
        if (!isConcreteRenderTarget(latest_bridge)) {
            refuseDisplayCapture();
            return false;
        }
        const auto bridge = targets.getMetadata(latest_bridge);
        const vk::Extent2D source_extent{
            source.extent.width, source.extent.height};
        const bool source_valid =
            source.usage.contains(ImageUsageBit::transfer_src) &&
            !source.usage.contains(ImageUsageBit::transient_attachment) &&
            source.storage_mode == RenderTargetStorageMode::materialized &&
            !source.alias_group && source.samples == 1 &&
            source.mip_levels == 1 && source.array_layers == 1 &&
            source.dimension == ImageResourceDimension::two_d &&
            toVulkanFormat(source.format) != vk::Format::eUndefined &&
            source_extent.width != 0 && source_extent.height != 0;
        const bool bridge_valid =
            bridge.usage.contains(ImageUsageBit::transfer_dst) &&
            bridge.usage.contains(ImageUsageBit::sampled) &&
            !bridge.usage.contains(ImageUsageBit::transient_attachment) &&
            bridge.storage_mode == RenderTargetStorageMode::materialized &&
            !bridge.alias_group && bridge.samples == 1 &&
            bridge.mip_levels == 1 && bridge.array_layers == 1 &&
            bridge.dimension == ImageResourceDimension::two_d;
        if (!source_valid || !bridge_valid ||
            (next_layout != vk::ImageLayout::eColorAttachmentOptimal &&
             next_layout != vk::ImageLayout::eShaderReadOnlyOptimal)) {
            refuseDisplayCapture();
            return false;
        }
        if (bridge.format != source.format ||
            bridge.extent.width != source.extent.width ||
            bridge.extent.height != source.extent.height) {
            return false;
        }

        auto &fullscreen = GET_MODULE(FullscreenPassContainer);
        const auto expected = targets.getImageView(latest_bridge);
        const auto bound = fullscreen.boundInputImageViewsForTesting(
            output_transform_->pass_id);
        if (bound.size() != 1 || bound.front() != expected) {
            const RenderTargetImageViewResolver views{targets};
            fullscreen.setInputTextures(output_transform_->pass_id,
                                        {latest_bridge}, views);
        }
        bridge_id_ = latest_bridge;
        display_generation_ = std::move(runtime_generation);
        display_source_extent_ = source_extent;
        display_submission_lease_ =
            std::make_shared<DisplaySubmissionLease>(
                DisplaySubmissionLease{display_generation_,
                                       targets.resourceLeases()});
        active_copy_query_slot_ = -1;
        if (copy_query_pool_ && timestamp_period_ns_ > 0.0F) {
            for (std::uint32_t attempt = 0;
                 attempt < copy_query_pending_.size(); ++attempt) {
                const auto slot = (next_copy_query_slot_ + attempt) %
                                  copy_query_pending_.size();
                if (copy_query_pending_[slot]) continue;
                active_copy_query_slot_ = static_cast<std::int32_t>(slot);
                next_copy_query_slot_ = (slot + 1) % copy_query_pending_.size();
                context.cmd_buf.resetQueryPool(copy_query_pool_.get(), slot * 2, 2);
                context.cmd_buf.writeTimestamp(
                    vk::PipelineStageFlagBits::eTopOfPipe,
                    copy_query_pool_.get(), slot * 2);
                break;
            }
        }
        layout_tracker.transition(context.cmd_buf, targets, vk_utils,
                                  display, vk::ImageLayout::eTransferSrcOptimal);
        layout_tracker.transition(context.cmd_buf, targets, vk_utils,
                                  bridge_id_, vk::ImageLayout::eTransferDstOptimal,
                                  false, RenderTargetImageKind::resolved,
                                  vk::PipelineStageFlagBits::eFragmentShader);
        vk::ImageCopy copy;
        copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        copy.extent = vk::Extent3D{source_extent, 1};
        context.cmd_buf.copyImage(
            targets.getImage(display).image.get(),
            vk::ImageLayout::eTransferSrcOptimal,
            targets.getImage(bridge_id_).image.get(),
            vk::ImageLayout::eTransferDstOptimal, copy);
        layout_tracker.transition(context.cmd_buf, targets, vk_utils,
                                  bridge_id_,
                                  vk::ImageLayout::eShaderReadOnlyOptimal,
                                  false, RenderTargetImageKind::resolved,
                                  vk::PipelineStageFlagBits::eFragmentShader);
        layout_tracker.transition(context.cmd_buf, targets, vk_utils,
                                  display, next_layout, false,
                                  RenderTargetImageKind::resolved,
                                  vk::PipelineStageFlagBits::eFragmentShader);
        if (active_copy_query_slot_ >= 0) {
            const auto slot = static_cast<std::uint32_t>(active_copy_query_slot_);
            context.cmd_buf.writeTimestamp(
                vk::PipelineStageFlagBits::eBottomOfPipe,
                copy_query_pool_.get(), slot * 2 + 1);
            copy_query_pending_[slot] = true;
        }
        recorded_bridge_id_ = bridge_id_;
        capture_recorded_ = true;
        return true;
    } catch (...) {
        refuseDisplayCapture();
        return false;
    }
}

void OutputWindowSink::beginDisplayCaptureFrame() noexcept {
    if (!wantsDisplayCapture()) return;
    capture_recorded_ = false;
    capture_ready_ = false;
    recorded_bridge_id_ = noRenderTargetId();
    display_generation_.reset();
    display_submission_lease_.reset();
}

bool OutputWindowSink::abortDisplayCaptureBeforeSubmitForTesting() noexcept {
    if (!capture_recorded_ || forced_abort_before_submit_frames_ == 0) return false;
    --forced_abort_before_submit_frames_;
    ++capture_aborted_;
    return true;
}

void OutputWindowSink::publishDisplayCapture() noexcept {
    if (!capture_recorded_) {
        return;
    }
    capture_recorded_ = false;
    capture_ready_ = true;
    active_copy_query_slot_ = -1;
    ++captures_;
}

void OutputWindowSink::cancelDisplayCapture() noexcept {
    if (active_copy_query_slot_ >= 0) {
        copy_query_pending_[static_cast<std::size_t>(active_copy_query_slot_)] = false;
        active_copy_query_slot_ = -1;
    }
    capture_recorded_ = false;
    recorded_bridge_id_ = noRenderTargetId();
}

void OutputWindowSink::refuseDisplayCapture() noexcept {
    cancelDisplayCapture();
    fail("output_window.source_unavailable@1: display is not a unique, "
         "copyable, materialized single-sample source at the pre-UI boundary");
}

void OutputWindowSink::presentFrame() {
    if (state_ !=
            OutputWindowLifecycleState::running ||
        !window_ || !target_ || core_ == nullptr) {
        return;
    }
    ++observed_frames_;
    if (close_after_frames_ > 0 &&
        observed_frames_ >= close_after_frames_) {
        window_->requestClose();
        state_ = OutputWindowLifecycleState::closed;
        LOG_INFO(logger,
                 "diagnostic output_window close requested");
        return;
    }
    if (window_->shouldClose()) {
        state_ = OutputWindowLifecycleState::closed;
        return;
    }
    window_->process();
    if (target_->gpuSubmissionsComplete()) {
        retired_captures_.clear();
    }
    std::unique_ptr<PendingCapture> capture;

    if (forced_drop_frames_ > 0 &&
        observed_frames_ > forced_drop_after_frames_) {
        --forced_drop_frames_;
        ++dropped_;
        return;
    }
    if (!options_.pattern && !capture_ready_) {
        ++dropped_;
        return;
    }
    try {
        const auto begin_mode =
            force_blocking_begin_
                ? FrameBeginMode::blocking
                : FrameBeginMode::nonblocking;
        auto begin = target_->beginFrame(
            options_.pattern ? nullptr : display_generation_,
            options_.pattern ? GpuSubmissionLease{} :
                               GpuSubmissionLease{display_submission_lease_},
            begin_mode);
        if (begin.disposition ==
            FrameBeginDisposition::unavailable) {
            if (begin.reason ==
                    FrameUnavailableReason::surface_lost ||
                begin.reason ==
                    FrameUnavailableReason::no_surface) {
                fail("output_window.present_failed@1: output window lost its surface");
                return;
            }
            ++dropped_;
            return;
        }
        if (begin.disposition ==
                FrameBeginDisposition::fatal ||
            begin.disposition ==
                FrameBeginDisposition::
                    device_rebuild_required) {
            fail("output_window.present_failed@1: output "
                 "target begin returned " +
                 std::string{
                     frameTargetLifecycleStateName(
                         target_->status().state)} +
                 " (" +
                 std::string{
                     frameUnavailableReasonName(
                         begin.reason)} +
                 ")");
            if (begin.disposition ==
                FrameBeginDisposition::
                    device_rebuild_required) {
                throw OutputWindowDeviceLost{last_error_};
            }
            return;
        }
        if (!begin.frame) {
            ++dropped_;
            return;
        }

        auto frame = std::move(*begin.frame);
        if (!options_.pattern && sink_submission_delay_ms_ > 0 &&
            delayed_sink_submissions_ == 0) {
            constexpr vk::DeviceSize size = 64 * 1024 * 1024;
            sink_delay_buffer_ = core_->allocBuf(
                size, vk::BufferUsageFlagBits::eTransferDst,
                vma::MemoryUsage::eGpuOnly, {});
            for (std::uint64_t i = 0; i < sink_submission_delay_ms_; ++i) {
                frame.context().cmd_buf.fillBuffer(
                    sink_delay_buffer_->buffer.get(), 0, size, 0);
                vk::BufferMemoryBarrier dependency{
                    vk::AccessFlagBits::eTransferWrite,
                    vk::AccessFlagBits::eTransferWrite, VK_QUEUE_FAMILY_IGNORED,
                    VK_QUEUE_FAMILY_IGNORED, sink_delay_buffer_->buffer.get(), 0,
                    size};
                frame.context().cmd_buf.pipelineBarrier(
                    vk::PipelineStageFlagBits::eTransfer,
                    vk::PipelineStageFlagBits::eTransfer, {}, {}, dependency, {});
            }
            ++delayed_sink_submissions_;
            LOG_INFO(logger,
                     "PELICAN_MD1A_SINK_SUBMISSION_DELAY: real submission pending");
        }
        const auto pattern_frame = pattern_frame_;
        if (options_.pattern) {
            recordPattern(frame.context().cmd_buf, frame.context(),
                          pattern_frame);
            ++pattern_frame_;
        } else {
            recordDisplay(frame.context().cmd_buf, frame.context());
            capture_ready_ = false;
        }

        if (!dump_directory_.empty() &&
            dumps_written_ < dump_count_) {
            capture = recordCapture(
                frame.context().cmd_buf, frame.context(),
                options_.pattern ? pattern_frame : captures_ - 1);
        }

        const auto result = target_->submit(std::move(frame));
        if (!options_.pattern) {
            display_submission_lease_.reset();
            display_generation_.reset();
        }
        const auto target_status = target_->status();
        submit_ms_max_ = target_status.submit_ms_max;
        present_ms_max_ = target_status.present_ms_max;
        if (target_status.last_wsi_result == vk::Result::eSuccess ||
            target_status.last_wsi_result ==
                vk::Result::eSuboptimalKHR) {
            ++presented_;
        }
        if (result.disposition ==
            FrameSubmitDisposition::presented) {
            if (capture) {
                finishCapture(
                    std::move(capture));
                ++dumps_written_;
            }
            return;
        }
        if (result.disposition ==
            FrameSubmitDisposition::output_stale) {
            if (capture) {
                retired_captures_.push_back(std::move(capture));
            }
            if (target_status.last_wsi_result ==
                vk::Result::eErrorDeviceLost) {
                fail("output_window.device_lost@1: output target present lost the device");
                throw OutputWindowDeviceLost{last_error_};
            }
            if (target_status.last_wsi_result ==
                vk::Result::eErrorSurfaceLostKHR) {
                fail("output_window.present_failed@1: output window lost its surface");
                return;
            }
            return;
        }
        fail("output_window.present_failed@1: queue present "
             "was not issued");
    } catch (const OutputWindowDeviceLost &) {
        if (capture) {
            retired_captures_.push_back(std::move(capture));
        }
        // Match the main target's terminal device-loss path instead of
        // continuing to submit work on the lost device.
        throw;
    } catch (const std::exception &error) {
        if (capture) {
            retired_captures_.push_back(std::move(capture));
        }
        fail(
            "output_window.present_failed@1: " +
            std::string{error.what()});
    }
}

void OutputWindowSink::recordDisplay(
    vk::CommandBuffer command_buffer,
    const FrameRenderContext &context) {
    if (!output_transform_ ||
        !isConcreteRenderTarget(bridge_id_)) {
        throw std::runtime_error("output window has no captured display source");
    }
    const auto letterbox = outputWindowLetterboxRect(
        display_source_extent_, context.extent);
    if (!letterbox) {
        throw std::runtime_error("output window display has a zero extent");
    }
    vk::RenderingAttachmentInfo attachment;
    attachment.imageView = context.color_attachment;
    attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    attachment.loadOp = vk::AttachmentLoadOp::eClear;
    attachment.storeOp = vk::AttachmentStoreOp::eStore;
    attachment.clearValue.color = vk::ClearColorValue{
        std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
    vk::RenderingInfo rendering;
    rendering.renderArea = vk::Rect2D{{0, 0}, context.extent};
    rendering.layerCount = 1;
    rendering.setColorAttachments(attachment);
    command_buffer.beginRendering(rendering);
    command_buffer.setViewport(0, vk::Viewport{
        static_cast<float>(letterbox->offset.x),
        static_cast<float>(letterbox->offset.y),
        static_cast<float>(letterbox->extent.width),
        static_cast<float>(letterbox->extent.height), 0.0f, 1.0f});
    command_buffer.setScissor(0, *letterbox);
    GET_MODULE(FullscreenPassRenderer).render(
        command_buffer, output_transform_->pass_id,
        output_transform_->definition,
        FullscreenPassRendererDependencies{
            GET_MODULE(FullscreenPassContainer), GET_MODULE(FrameResources)});
    command_buffer.endRendering();
}

void OutputWindowSink::recordPattern(
    vk::CommandBuffer command_buffer,
    const FrameRenderContext &context,
    std::uint64_t pattern_frame) {
    if (context.extent.width == 0 ||
        context.extent.height == 0) {
        throw std::runtime_error(
            "output window has a zero extent");
    }

    vk::RenderingAttachmentInfo attachment;
    attachment.imageView =
        context.color_attachment;
    attachment.imageLayout =
        vk::ImageLayout::eColorAttachmentOptimal;
    attachment.loadOp =
        vk::AttachmentLoadOp::eClear;
    attachment.storeOp =
        vk::AttachmentStoreOp::eStore;
    attachment.clearValue.color =
        vk::ClearColorValue{std::array<float, 4>{
            0.0f, 0.0f, 0.0f, 1.0f}};

    vk::RenderingInfo info;
    info.renderArea =
        vk::Rect2D{{0, 0}, context.extent};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &attachment;
    command_buffer.beginRendering(info);

    // Eight vertical bars. The phase is the frame number, so consecutive
    // frames invert every bar. Black and white are exactly representable in
    // both UNORM and sRGB swapchain formats.
    constexpr std::uint32_t kBarCount = 8;
    const auto bar_width =
        (context.extent.width + kBarCount - 1) /
        kBarCount;
    std::array<vk::ClearRect, kBarCount> rects{};
    std::uint32_t white_bar_count = 0;
    for (std::uint32_t bar = 0; bar < kBarCount; ++bar) {
        if (((bar + pattern_frame) & 1u) != 0) {
            continue;
        }
        const auto left = bar * bar_width;
        const auto right = std::min(
            (bar + 1) * bar_width,
            context.extent.width);
        if (left >= right) {
            continue;
        }
        auto &rect = rects[white_bar_count++];
        rect.rect = vk::Rect2D{
            {static_cast<std::int32_t>(left), 0},
            {right - left, context.extent.height}};
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
    }
    if (white_bar_count > 0) {
        auto clear = vk::ClearAttachment{};
        clear.aspectMask =
            vk::ImageAspectFlagBits::eColor;
        clear.colorAttachment = 0;
        clear.clearValue.color =
            vk::ClearColorValue{std::array<float, 4>{
                1.0f, 1.0f, 1.0f, 1.0f}};
        command_buffer.clearAttachments(
            std::array<vk::ClearAttachment, 1>{clear},
            std::span<const vk::ClearRect>{
                rects.data(), white_bar_count});
    }
    command_buffer.endRendering();
}

std::unique_ptr<OutputWindowSink::PendingCapture>
OutputWindowSink::recordCapture(
    vk::CommandBuffer command_buffer,
    const FrameRenderContext &context,
    std::uint64_t pattern_frame) {
    if (!pattern_capture_available_) {
        if (!dump_skip_reported_) {
            dump_skip_reported_ = true;
            LOG_INFO(
                logger,
                "PELICAN_MD1A_PATTERN_CAPTURE_SKIPPED: "
                "capture unavailable_windowed: surface lacks "
                "TRANSFER_SRC support");
        }
        return nullptr;
    }
    const auto bytes =
        static_cast<vk::DeviceSize>(
            context.extent.width) *
        context.extent.height * 4;
    auto staging = core_->allocBuf(
        bytes,
        vk::BufferUsageFlagBits::eTransferDst,
        vma::MemoryUsage::eAutoPreferHost,
        vma::AllocationCreateFlagBits::
            eHostAccessRandom);

    vk::ImageMemoryBarrier to_transfer;
    to_transfer.srcAccessMask =
        vk::AccessFlagBits::eColorAttachmentRead |
        vk::AccessFlagBits::eColorAttachmentWrite;
    to_transfer.dstAccessMask =
        vk::AccessFlagBits::eTransferRead;
    to_transfer.oldLayout =
        vk::ImageLayout::eColorAttachmentOptimal;
    to_transfer.newLayout =
        vk::ImageLayout::eTransferSrcOptimal;
    to_transfer.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = context.color_image;
    to_transfer.subresourceRange = {
        vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    command_buffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eTransfer,
        {}, {}, {}, {to_transfer});

    vk::BufferImageCopy region;
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {
        vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.imageOffset = vk::Offset3D(0, 0, 0);
    region.imageExtent =
        vk::Extent3D{context.extent, 1};
    command_buffer.copyImageToBuffer(
        context.color_image,
        vk::ImageLayout::eTransferSrcOptimal,
        staging.buffer.get(),
        std::array<vk::BufferImageCopy, 1>{region});

    vk::ImageMemoryBarrier to_color;
    to_color.srcAccessMask =
        vk::AccessFlagBits::eTransferRead;
    to_color.dstAccessMask =
        vk::AccessFlagBits::eColorAttachmentRead |
        vk::AccessFlagBits::eColorAttachmentWrite;
    to_color.oldLayout =
        vk::ImageLayout::eTransferSrcOptimal;
    to_color.newLayout =
        vk::ImageLayout::eColorAttachmentOptimal;
    to_color.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    to_color.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    to_color.image = context.color_image;
    to_color.subresourceRange = {
        vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    command_buffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        {}, {}, {}, {to_color});

    return std::make_unique<PendingCapture>(
        PendingCapture{
            .buffer = std::move(staging),
            .extent = context.extent,
            .format =
                target_->caps().compile_facts.color_format,
            .pattern_frame = pattern_frame,
        });
}

void OutputWindowSink::finishCapture(
    std::unique_ptr<PendingCapture> capture) {
    // Diagnostic reads only. Waiting here is what makes the staging buffer's
    // host read safe, and the dump path is never used by the normal frame
    // loop.
    core_->waitIdle();
    const auto pixels = core_->readBuf(
        capture->buffer,
        static_cast<vk::DeviceSize>(
            capture->extent.width) *
            capture->extent.height * 4);
    const auto expected =
        static_cast<std::size_t>(
            capture->extent.width) *
        capture->extent.height * 4;
    if (pixels.size() < expected) {
        throw std::runtime_error(
            "diagnostic capture returned " +
            std::to_string(pixels.size()) +
            " bytes, expected " +
            std::to_string(expected));
    }

    const auto bgr =
        capture->format ==
            vk::Format::eB8G8R8A8Unorm ||
        capture->format ==
            vk::Format::eB8G8R8A8Srgb;
    std::string ppm;
    ppm.reserve(
        expected * 3 / 4 + 64);
    ppm += "P6\n" +
           std::to_string(capture->extent.width) +
           " " +
           std::to_string(capture->extent.height) +
           "\n255\n";
    for (std::size_t offset = 0;
         offset < expected; offset += 4) {
        const auto red = bgr ? pixels[offset + 2]
                             : pixels[offset + 0];
        const auto green = pixels[offset + 1];
        const auto blue = bgr ? pixels[offset + 0]
                              : pixels[offset + 2];
        ppm.push_back(static_cast<char>(red));
        ppm.push_back(static_cast<char>(green));
        ppm.push_back(static_cast<char>(blue));
    }

    std::filesystem::create_directories(
        dump_directory_);
    const auto path =
        std::filesystem::path{dump_directory_} /
        ("pattern_" +
         std::to_string(capture->pattern_frame) +
         ".ppm");
    std::ofstream stream{
        path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        throw std::runtime_error(
            "could not open diagnostic pattern dump " +
            path.string());
    }
    stream.write(
        ppm.data(),
        static_cast<std::streamsize>(ppm.size()));
    if (!stream) {
        throw std::runtime_error(
            "could not write diagnostic pattern dump " +
            path.string());
    }
}

void OutputWindowSink::shutdown() noexcept {
    if (target_) {
        if (core_ != nullptr) {
            try {
                core_->waitIdle();
            } catch (...) {
            }
        }
        target_.reset();
    }
    copy_query_pool_.reset();
    copy_query_pending_.clear();
    window_.reset();
    core_ = nullptr;
    if (state_ ==
        OutputWindowLifecycleState::running) {
        state_ =
            OutputWindowLifecycleState::closed;
    }
}

OutputWindowStatus OutputWindowSink::status() const {
    OutputWindowStatus status;
    status.state = state_;
    status.monitor = monitor_;
    status.width = captured_width_;
    status.height = captured_height_;
    status.source = options_.enabled
        ? (options_.pattern ? "pattern" : "display")
        : "";
    status.presented = presented_;
    status.dropped = dropped_;
    status.capture_skipped = capture_skipped_;
    status.captures = captures_;
    status.capture_aborted = capture_aborted_;
    status.capture_fence_waits = capture_fence_waits_;
    status.copy_gpu_ms_max = copy_gpu_ms_max_;
    status.copy_gpu_samples = copy_gpu_samples_;
    status.submit_ms_max = submit_ms_max_;
    status.present_ms_max = present_ms_max_;
    if (!last_error_.empty()) {
        status.last_error = last_error_;
    }
    if (target_) {
        status.present_mode =
            std::string{shortPresentMode(
                target_->status().present_mode)};
        status.width =
            target_->caps().compile_facts.extent.width;
        status.height =
            target_->caps().compile_facts.extent.height;
        const auto format = target_->caps().compile_facts.color_format;
        status.surface_format =
            format == vk::Format::eR8G8B8A8Srgb ||
                    format == vk::Format::eB8G8R8A8Srgb
                ? "SRGB"
                : "UNORM";
    } else if (
        state_ !=
        OutputWindowLifecycleState::off) {
        status.present_mode = present_mode_;
    }
    return status;
}

nlohmann::json OutputWindowSink::statusJson() const {
    return outputWindowStatusJson(status());
}

} // namespace Pelican
