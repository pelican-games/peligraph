#pragma once

#include "../os/framebuffersnapshot.hpp"
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vulkan/vulkan.hpp>

namespace Pelican {

using SwapchainEpochId = std::uint64_t;

// Base Vulkan has no presentation-complete primitive. After an image that was
// presented by a successor epoch is reacquired, every earlier swapchain can be
// retired without guessing about presentation-engine progress.
class BasePresentRetirementTracker {
    SwapchainEpochId successor_completion_watermark_ = 0;

  public:
    void noteSuccessorImageReacquired(
        SwapchainEpochId successor_epoch,
        bool image_had_pending_present) noexcept;
    bool mayRetire(
        SwapchainEpochId retired_epoch,
        bool ever_presented) const noexcept;
    SwapchainEpochId watermark() const noexcept {
        return successor_completion_watermark_;
    }
};

enum class WsiResultClass {
    ready,
    refresh_advisory,
    not_ready,
    swapchain_unavailable,
    surface_unavailable,
    device_lost,
    retryable_failure,
    fatal,
};

WsiResultClass classifyWsiResult(vk::Result result) noexcept;

struct SwapchainRecoveryKey {
    std::uint64_t framebuffer_revision = 0;
    vk::Extent2D framebuffer_extent{};
    std::uint64_t surface_support_fingerprint = 0;
    std::uint64_t output_facts_fingerprint = 0;
    std::uint64_t present_configuration_fingerprint = 0;

    bool operator==(const SwapchainRecoveryKey &) const = default;
};

enum class WindowOutputRecoveryReason {
    none,
    framebuffer_changed,
    suboptimal,
    out_of_date,
    abandoned_frame,
    prepare_failed,
    resource_pressure,
    zero_extent,
    surface_lost,
    device_lost,
    fatal,
};

enum class WindowOutputStateKind {
    ready,
    refresh_pending,
    suspended_zero_extent,
    preparing,
    unavailable_retry,
    surface_lost,
    device_rebuild_required,
    fatal,
};

struct WindowOutputReady {
    SwapchainEpochId epoch = 0;
    SwapchainRecoveryKey key;
};

struct WindowOutputRefreshPending {
    SwapchainEpochId old_epoch = 0;
    SwapchainRecoveryKey key;
    WindowOutputRecoveryReason reason =
        WindowOutputRecoveryReason::framebuffer_changed;
};

struct WindowOutputSuspendedZeroExtent {
    SwapchainEpochId old_epoch = 0;
    FramebufferExtentSnapshot framebuffer;
};

struct WindowOutputPreparing {
    SwapchainEpochId old_epoch = 0;
    SwapchainRecoveryKey key;
    WindowOutputRecoveryReason reason =
        WindowOutputRecoveryReason::framebuffer_changed;
    std::uint32_t attempt = 0;
};

struct WindowOutputUnavailableRetry {
    SwapchainRecoveryKey key;
    WindowOutputRecoveryReason reason =
        WindowOutputRecoveryReason::prepare_failed;
    std::uint64_t retry_after_tick = 0;
    std::uint32_t attempt = 0;
};

struct WindowOutputSurfaceLost {
    SwapchainEpochId old_epoch = 0;
};

struct WindowOutputDeviceRebuildRequired {};

struct WindowOutputFatal {};

using WindowOutputState =
    std::variant<WindowOutputReady,
                 WindowOutputRefreshPending,
                 WindowOutputSuspendedZeroExtent,
                 WindowOutputPreparing,
                 WindowOutputUnavailableRetry,
                 WindowOutputSurfaceLost,
                 WindowOutputDeviceRebuildRequired,
                 WindowOutputFatal>;

struct SwapchainPreparationRequest {
    SwapchainEpochId old_epoch = 0;
    SwapchainRecoveryKey key;
    WindowOutputRecoveryReason reason =
        WindowOutputRecoveryReason::framebuffer_changed;
    std::uint32_t attempt = 0;
};

// Pure, deterministic lifecycle policy. Vulkan object creation and publication
// stay in SwapchainFrameTarget; this class only decides whether one complete
// candidate may be prepared for a recovery key.
class WindowOutputRecoveryStateMachine {
    WindowOutputState state_;
    std::optional<SwapchainRecoveryKey>
        acknowledged_suboptimal_key_;

  public:
    WindowOutputRecoveryStateMachine(
        SwapchainEpochId initial_epoch,
        SwapchainRecoveryKey initial_key);

    const WindowOutputState &state() const noexcept {
        return state_;
    }
    WindowOutputStateKind kind() const noexcept;
    WindowOutputRecoveryReason reason() const noexcept;

    // Returns true only when a new refresh request was published. Duplicate
    // callbacks/results for the same key are coalesced.
    bool requestRefresh(
        SwapchainEpochId old_epoch,
        SwapchainRecoveryKey key,
        WindowOutputRecoveryReason reason,
        std::uint64_t tick);
    void observeZeroExtent(
        SwapchainEpochId old_epoch,
        FramebufferExtentSnapshot framebuffer);
    bool resumeFromPositiveExtent(
        SwapchainEpochId old_epoch,
        SwapchainRecoveryKey key,
        std::uint64_t tick);

    std::optional<SwapchainPreparationRequest>
    takePreparationRequest();
    bool retryIfDue(std::uint64_t tick);
    void preparationSucceeded(
        SwapchainEpochId new_epoch,
        SwapchainRecoveryKey key);
    void preparationFailed(
        std::uint64_t tick,
        std::uint64_t retry_delay_ticks);
    void deferRetry(
        SwapchainRecoveryKey key,
        WindowOutputRecoveryReason reason,
        std::uint64_t tick,
        std::uint64_t retry_delay_ticks,
        std::uint32_t attempt = 1);
    void markSurfaceLost(SwapchainEpochId old_epoch);
    void markDeviceLost();
    void markFatal();
};

std::string_view windowOutputStateName(
    WindowOutputStateKind state) noexcept;
std::string_view windowOutputRecoveryReasonName(
    WindowOutputRecoveryReason reason) noexcept;

} // namespace Pelican
