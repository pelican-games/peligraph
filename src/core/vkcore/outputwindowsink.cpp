#include "outputwindowsink.hpp"

#include "../log.hpp"
#include "../os/outputwindow.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/core.hpp"
#include "swapchainframetarget.hpp"
#include "windowsurface.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <system_error>

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
        {"presented", status.presented},
        {"dropped", status.dropped},
        {"last_error",
         status.last_error
             ? nlohmann::json(*status.last_error)
             : nlohmann::json(nullptr)},
        {"present_ms_max", status.present_ms_max},
    };
}

nlohmann::json outputWindowOffStatusJson() {
    return outputWindowStatusJson(
        OutputWindowStatus{});
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

    if (!options_.pattern) {
        fail("output_window.config_invalid@1: real output is not "
             "implemented yet; --output-window-pattern is required");
        return;
    }

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
        LOG_INFO(logger,
                 "diagnostic output_window running: monitor={} "
                 "extent={}x{} present_mode={}",
                 options_.monitor, captured_width_,
                 captured_height_, present_mode_);
    } catch (const std::exception &error) {
        fail(
            "output_window.present_failed@1: output target "
            "creation failed: " +
            std::string{error.what()});
    }
}

void OutputWindowSink::fail(
    std::string message) {
    state_ = OutputWindowLifecycleState::failed;
    last_error_ = std::move(message);
    LOG_INFO(logger, "diagnostic output_window failed: {}",
             last_error_);
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

    if (forced_drop_frames_ > 0 &&
        observed_frames_ > forced_drop_after_frames_) {
        --forced_drop_frames_;
        ++dropped_;
        return;
    }
    try {
        const auto begin_mode =
            force_blocking_begin_
                ? FrameBeginMode::blocking
                : FrameBeginMode::nonblocking;
        auto begin = target_->beginFrame(
            {}, {}, begin_mode);
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
        const auto pattern_frame = pattern_frame_;
        recordPattern(
            frame.context().cmd_buf,
            frame.context(), pattern_frame);
        ++pattern_frame_;

        std::unique_ptr<PendingCapture> capture;
        if (!dump_directory_.empty() &&
            dumps_written_ < dump_count_) {
            capture = recordCapture(
                      frame.context().cmd_buf,
                          frame.context(),
                          pattern_frame);
        }

        const auto result =
            target_->submit(std::move(frame));
        const auto target_status = target_->status();
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
            // The target's own WSI recovery owns the old swapchain and its
            // present semaphores. The sink only has to retire a diagnostic
            // staging read before dropping it.
            if (capture) {
                core_->waitIdle();
                capture.reset();
            }
            return;
        }
        if (capture) {
            core_->waitIdle();
        }
        fail("output_window.present_failed@1: queue present "
             "was not issued");
    } catch (const OutputWindowDeviceLost &) {
        // Match the main target's terminal device-loss path instead of
        // continuing to submit work on the lost device.
        throw;
    } catch (const std::exception &error) {
        if (core_ != nullptr) {
            try {
                core_->waitIdle();
            } catch (...) {
            }
        }
        fail(
            "output_window.present_failed@1: " +
            std::string{error.what()});
    }
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
    status.presented = presented_;
    status.dropped = dropped_;
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
