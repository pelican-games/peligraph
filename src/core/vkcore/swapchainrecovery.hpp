#pragma once

#include "../os/framebuffersnapshot.hpp"
#include <cstdint>
#include <memory>
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

// A fresh surface cannot bind the same native window while an unretired
// swapchain is still associated with it. Without an exact present-completion
// primitive there is no safe same-device ordering for that case.
bool surfaceLossRequiresDeviceRebuild(
    bool exact_present_retirement,
    bool has_unproven_present) noexcept;

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
std::string_view wsiResultClassName(
    WsiResultClass classification) noexcept;

// The same VkResult has different recovery meaning depending on where it was
// observed. For example, NOT_READY is a routine frame drop at acquire, while a
// failed preparation call must enter bounded retry. Keep this policy pure so
// the real WSI path and protocol-fake tests share one decision table.
enum class WindowWsiCallSite {
    acquire,
    present,
    surface_create,
    surface_support_query,
    swapchain_create,
    dependent_resources,
};

enum class WindowWsiRecoveryAction {
    proceed,
    proceed_and_refresh,
    drop_frame,
    replace_swapchain,
    replace_surface,
    rebuild_device,
    retry_later,
    fatal,
};

struct WindowWsiRecoveryDecision {
    WsiResultClass classification =
        WsiResultClass::fatal;
    WindowWsiRecoveryAction action =
        WindowWsiRecoveryAction::fatal;
};

WindowWsiRecoveryDecision decideWindowWsiRecovery(
    WindowWsiCallSite site,
    vk::Result result) noexcept;

std::string_view windowWsiCallSiteName(
    WindowWsiCallSite site) noexcept;
std::string_view windowWsiRecoveryActionName(
    WindowWsiRecoveryAction action) noexcept;

struct WindowWsiFaultContext {
    WindowWsiCallSite site =
        WindowWsiCallSite::acquire;
    std::uint64_t surface_epoch = 0;
    SwapchainEpochId swapchain_epoch = 0;
    std::uint32_t recovery_attempt = 0;
};

// Ordered Debug-only protocol driver. A rule is consumed only when its call
// site becomes the next observed WSI boundary, so worker/main-thread ordering
// is deterministic without sleeping or replacing Vulkan handles with fakes.
// Production construction never creates this object.
class WindowWsiFaultScriptForTesting {
    struct State;
    std::unique_ptr<State> state_;

    explicit WindowWsiFaultScriptForTesting(
        std::unique_ptr<State> state) noexcept;

  public:
    ~WindowWsiFaultScriptForTesting();

    WindowWsiFaultScriptForTesting(
        const WindowWsiFaultScriptForTesting &) =
        delete;
    WindowWsiFaultScriptForTesting &operator=(
        const WindowWsiFaultScriptForTesting &) =
        delete;

    static std::unique_ptr<
        WindowWsiFaultScriptForTesting>
    parse(std::string_view script);

    std::optional<vk::Result> take(
        WindowWsiFaultContext context) noexcept;
    std::size_t injectedCount() const noexcept;
    std::size_t remainingCount() const noexcept;
};

// vkCreateSwapchainKHR success may retire oldSwapchain even when dependent
// resources fail later. The successfully-created replacement is then the only
// legal retry anchor; before that cutover the previous swapchain remains the
// anchor.
enum class SwapchainRecoveryAnchorKind {
    none,
    previous,
    replacement,
};

SwapchainRecoveryAnchorKind
selectSwapchainRecoveryAnchor(
    bool replacement_available,
    bool previous_available) noexcept;

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
    preparing_surface,
    unavailable_retry,
    surface_lost,
    device_rebuild_required,
    fatal,
};

enum class WindowOutputPreparationKind {
    swapchain,
    surface,
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
    WindowOutputPreparationKind preparation_kind =
        WindowOutputPreparationKind::swapchain;
    std::uint32_t attempt = 0;
};

struct WindowOutputPreparing {
    SwapchainEpochId old_epoch = 0;
    SwapchainRecoveryKey key;
    WindowOutputRecoveryReason reason =
        WindowOutputRecoveryReason::framebuffer_changed;
    std::uint32_t attempt = 0;
    WindowOutputPreparationKind preparation_kind =
        WindowOutputPreparationKind::swapchain;
};

struct WindowOutputUnavailableRetry {
    SwapchainEpochId old_epoch = 0;
    SwapchainRecoveryKey key;
    WindowOutputRecoveryReason reason =
        WindowOutputRecoveryReason::prepare_failed;
    std::uint64_t retry_after_tick = 0;
    std::uint32_t attempt = 0;
    WindowOutputPreparationKind preparation_kind =
        WindowOutputPreparationKind::swapchain;
};

struct WindowOutputSurfaceLost {
    SwapchainEpochId old_epoch = 0;
    std::uint32_t attempt = 0;
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
    WindowOutputPreparationKind preparation_kind =
        WindowOutputPreparationKind::swapchain;
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
    std::uint32_t attempt() const noexcept;

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
    std::optional<SwapchainPreparationRequest>
    takeSurfacePreparationRequest(
        SwapchainRecoveryKey key);
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
        std::uint32_t attempt = 1,
        WindowOutputPreparationKind
            preparation_kind =
                WindowOutputPreparationKind::
                    swapchain,
        SwapchainEpochId old_epoch = 0);
    void markSurfaceLost(SwapchainEpochId old_epoch);
    void markDeviceLost();
    void markFatal();
};

std::string_view windowOutputStateName(
    WindowOutputStateKind state) noexcept;
std::string_view windowOutputRecoveryReasonName(
    WindowOutputRecoveryReason reason) noexcept;

} // namespace Pelican
