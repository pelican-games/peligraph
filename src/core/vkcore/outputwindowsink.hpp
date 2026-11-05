#pragma once

// MD-1a0: the diagnostic second-window sink. It runs once per flat interactive
// frame, after the main target has rendered and presented, and owns a small
// independently recovered swapchain target over an output-only GLFW window.
// It deliberately has no FrameResources, render-graph nodes, capture, or GPU
// timing ranges.

#include "../container.hpp"
#include "../launchconfig.hpp"

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct FrameRenderContext;
class OutputWindow;
class SwapchainFrameTarget;
class VulkanManageCore;

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
    std::uint64_t presented = 0;
    std::uint64_t dropped = 0;
    std::optional<std::string> last_error;
    double present_ms_max = 0.0;
};

std::string_view outputWindowLifecycleStateName(
    OutputWindowLifecycleState state) noexcept;

nlohmann::json outputWindowStatusJson(
    const OutputWindowStatus &status);
nlohmann::json outputWindowOffStatusJson();

DECLARE_MODULE(OutputWindowSink) {
  public:
    OutputWindowSink();
    ~OutputWindowSink();

    OutputWindowSink(const OutputWindowSink &) = delete;
    OutputWindowSink &operator=(
        const OutputWindowSink &) = delete;

    // Called once, after the runtime module graph is ready and before module
    // creation is frozen. A monitor that cannot be opened is recorded as
    // failed and leaves the engine running.
    void configure(
        const OutputWindowLaunchOptions &options);

    // Called on the main frame thread after the main flat render. A failed or
    // closed sink keeps this a no-op.
    void presentFrame();

    // finishLoopResources waits for the device, including this target's slot
    // and presentation fences. The target, surface and window then go away.
    void shutdown() noexcept;

    bool enabled() const noexcept {
        return options_.enabled;
    }
    bool running() const noexcept {
        return state_ ==
               OutputWindowLifecycleState::running;
    }
    OutputWindowStatus status() const;
    nlohmann::json statusJson() const;

  private:
    void fail(std::string message);
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

    OutputWindowLaunchOptions options_;
    OutputWindowLifecycleState state_ =
        OutputWindowLifecycleState::off;
    std::optional<std::uint32_t> monitor_;
    std::string last_error_;
    std::uint64_t presented_ = 0;
    std::uint64_t dropped_ = 0;
    double present_ms_max_ = 0.0;
    std::uint64_t pattern_frame_ = 0;
    std::uint64_t observed_frames_ = 0;
    std::uint64_t forced_drop_frames_ = 0;
    std::uint64_t forced_drop_after_frames_ = 0;
    std::uint64_t close_after_frames_ = 0;
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

    // Diagnostic dump setup. Empty means no dump. The count is consumed only
    // by successful frames, so a dropped start does not name a fixture file.
    std::string dump_directory_;
    std::uint64_t dump_count_ = 0;
    std::uint64_t dumps_written_ = 0;
    bool dump_skip_reported_ = false;
};

} // namespace Pelican
