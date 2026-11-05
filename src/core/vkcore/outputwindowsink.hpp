#pragma once

#include "buf.hpp"
#include "../container.hpp"
#include "../launchconfig.hpp"
#include "../renderingpass/renderingpass.hpp"

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

inline constexpr std::string_view output_window_bridge_name =
    "output_window_bridge";

struct FrameRenderContext;
struct RendererRuntimeGeneration;
class OutputWindow;
class RenderTargetContainer;
class RenderTargetLayoutTracker;
class SwapchainFrameTarget;
class VulkanManageCore;
class VulkanUtils;

enum class OutputWindowLifecycleState {
    off,
    running,
    failed,
    closed,
};

struct OutputWindowStatus {
    OutputWindowLifecycleState state =
        OutputWindowLifecycleState::off;
    std::optional<std::uint32_t> monitor;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string present_mode;
    std::string source;
    std::string surface_format;
    std::uint64_t presented = 0;
    std::uint64_t dropped = 0;
    std::uint64_t capture_skipped = 0;
    std::uint64_t captures = 0;
    std::uint64_t capture_aborted = 0;
    std::uint64_t capture_fence_waits = 0;
    std::optional<std::string> last_error;
    double copy_gpu_ms_max = 0.0;
    double submit_ms_max = 0.0;
    double present_ms_max = 0.0;
    std::uint64_t copy_gpu_samples = 0;
};

std::string_view outputWindowLifecycleStateName(
    OutputWindowLifecycleState state) noexcept;

nlohmann::json outputWindowStatusJson(
    const OutputWindowStatus &status);
nlohmann::json outputWindowOffStatusJson();
std::optional<vk::Rect2D> outputWindowLetterboxRect(
    vk::Extent2D source, vk::Extent2D destination);

DECLARE_MODULE(OutputWindowSink) {
  public:
    OutputWindowSink();
    ~OutputWindowSink();

    OutputWindowSink(const OutputWindowSink &) = delete;
    OutputWindowSink &operator=(
        const OutputWindowSink &) = delete;

    void configure(
        const OutputWindowLaunchOptions &options);
    void presentFrame();
    void shutdown() noexcept;
    void beginDisplayCaptureFrame() noexcept;
    bool abortDisplayCaptureBeforeSubmitForTesting() noexcept;

    bool enabled() const noexcept {
        return options_.enabled;
    }
    bool running() const noexcept {
        return state_ ==
               OutputWindowLifecycleState::running;
    }
    bool wantsDisplayCapture() const noexcept {
        return running() && options_.enabled && !options_.pattern;
    }
    bool recordDisplayCapture(
        const FrameRenderContext &context,
        RenderTargetContainer &targets,
        VulkanUtils &vk_utils,
        RenderTargetLayoutTracker &layout_tracker,
        GlobalRenderTargetId display,
        vk::ImageLayout next_layout,
        std::shared_ptr<const RendererRuntimeGeneration>
            runtime_generation) noexcept;
    void publishDisplayCapture() noexcept;
    void cancelDisplayCapture() noexcept;
    void refuseDisplayCapture() noexcept;
    OutputWindowStatus status() const;
    nlohmann::json statusJson() const;

  private:
    void fail(std::string message) noexcept;
    void waitForCaptureForTesting();
    void pollCopyGpuTiming() noexcept;
    void initializeDisplayOutput();
    void recordDisplay(
        vk::CommandBuffer command_buffer,
        const FrameRenderContext &context);
    void recordPattern(
        vk::CommandBuffer command_buffer,
        const FrameRenderContext &context,
        std::uint64_t pattern_frame);

    struct PendingCapture;
    std::unique_ptr<PendingCapture> recordCapture(
        vk::CommandBuffer command_buffer,
        const FrameRenderContext &context,
        std::uint64_t pattern_frame);
    void finishCapture(
        std::unique_ptr<PendingCapture> capture);

    struct DisplaySubmissionLease {
        std::shared_ptr<const RendererRuntimeGeneration> generation;
        std::vector<std::shared_ptr<const void>> target_resources;
    };

    OutputWindowLaunchOptions options_;
    OutputWindowLifecycleState state_ =
        OutputWindowLifecycleState::off;
    std::optional<std::uint32_t> monitor_;
    std::string last_error_;
    std::uint64_t presented_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t capture_skipped_ = 0;
    std::uint64_t captures_ = 0;
    std::uint64_t capture_aborted_ = 0;
    std::uint64_t capture_fence_waits_ = 0;
    std::uint64_t forced_capture_busy_frames_ = 0;
    std::uint64_t forced_abort_before_submit_frames_ = 0;
    double copy_gpu_ms_max_ = 0.0;
    double submit_ms_max_ = 0.0;
    double present_ms_max_ = 0.0;
    std::uint64_t copy_gpu_samples_ = 0;
    vk::UniqueQueryPool copy_query_pool_;
    std::vector<bool> copy_query_pending_;
    std::uint32_t next_copy_query_slot_ = 0;
    std::int32_t active_copy_query_slot_ = -1;
    float timestamp_period_ns_ = 0.0F;
    std::uint64_t pattern_frame_ = 0;
    std::uint64_t observed_frames_ = 0;
    std::uint64_t forced_drop_frames_ = 0;
    std::uint64_t forced_drop_after_frames_ = 0;
    std::uint64_t close_after_frames_ = 0;
    std::uint64_t delayed_sink_submissions_ = 0;
    std::uint64_t sink_submission_delay_ms_ = 0;
    std::optional<BufferWrapper> sink_delay_buffer_;
    bool force_blocking_begin_ = false;
    std::string present_mode_ = "FIFO";
    std::uint32_t captured_width_ = 0;
    std::uint32_t captured_height_ = 0;
    bool pattern_capture_available_ = false;

    bool configured_ = false;
    std::unique_ptr<OutputWindow> window_;
    vk::UniqueFence slot_fence_for_testing_;
    std::unique_ptr<SwapchainFrameTarget> target_;
    VulkanManageCore *core_ = nullptr;

    std::unique_ptr<CompiledPass> output_transform_;
    GlobalRenderTargetId bridge_id_ = noRenderTargetId();
    GlobalRenderTargetId recorded_bridge_id_ = noRenderTargetId();
    std::shared_ptr<const RendererRuntimeGeneration>
        display_generation_;
    std::shared_ptr<const DisplaySubmissionLease>
        display_submission_lease_;
    vk::Extent2D display_source_extent_{};
    bool capture_recorded_ = false;
    bool capture_ready_ = false;

    std::string dump_directory_;
    std::uint64_t dump_count_ = 0;
    std::uint64_t dumps_written_ = 0;
    bool dump_skip_reported_ = false;
    std::vector<std::unique_ptr<PendingCapture>> retired_captures_;
};

} // namespace Pelican
