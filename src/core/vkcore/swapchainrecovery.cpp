#include "swapchainrecovery.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace Pelican {

void BasePresentRetirementTracker::
    noteSuccessorImageReacquired(
        SwapchainEpochId successor_epoch,
        bool image_had_pending_present) noexcept {
    if (!image_had_pending_present) return;
    successor_completion_watermark_ =
        std::max(
            successor_completion_watermark_,
            successor_epoch);
}

bool BasePresentRetirementTracker::mayRetire(
    SwapchainEpochId retired_epoch,
    bool ever_presented) const noexcept {
    return !ever_presented ||
           retired_epoch <
               successor_completion_watermark_;
}

WsiResultClass classifyWsiResult(
    vk::Result result) noexcept {
    switch (result) {
    case vk::Result::eSuccess:
        return WsiResultClass::ready;
    case vk::Result::eSuboptimalKHR:
        return WsiResultClass::refresh_advisory;
    case vk::Result::eTimeout:
    case vk::Result::eNotReady:
        return WsiResultClass::not_ready;
    case vk::Result::eErrorOutOfDateKHR:
        return WsiResultClass::swapchain_unavailable;
    case vk::Result::eErrorSurfaceLostKHR:
        return WsiResultClass::surface_unavailable;
    case vk::Result::eErrorDeviceLost:
        return WsiResultClass::device_lost;
    case vk::Result::eErrorOutOfHostMemory:
    case vk::Result::eErrorOutOfDeviceMemory:
        return WsiResultClass::retryable_failure;
    default:
        return WsiResultClass::fatal;
    }
}

WindowOutputRecoveryStateMachine::
    WindowOutputRecoveryStateMachine(
        SwapchainEpochId initial_epoch,
        SwapchainRecoveryKey initial_key)
    : state_{WindowOutputReady{
          initial_epoch, std::move(initial_key)}} {
    if (initial_epoch == 0) {
        throw std::invalid_argument(
            "window output recovery requires a non-zero initial epoch");
    }
}

WindowOutputStateKind
WindowOutputRecoveryStateMachine::kind() const noexcept {
    return std::visit(
        [](const auto &value) {
            using State =
                std::remove_cvref_t<decltype(value)>;
            if constexpr (
                std::is_same_v<State, WindowOutputReady>) {
                return WindowOutputStateKind::ready;
            } else if constexpr (
                std::is_same_v<
                    State,
                    WindowOutputRefreshPending>) {
                return WindowOutputStateKind::
                    refresh_pending;
            } else if constexpr (
                std::is_same_v<
                    State,
                    WindowOutputSuspendedZeroExtent>) {
                return WindowOutputStateKind::
                    suspended_zero_extent;
            } else if constexpr (
                std::is_same_v<
                    State, WindowOutputPreparing>) {
                return WindowOutputStateKind::preparing;
            } else if constexpr (
                std::is_same_v<
                    State,
                    WindowOutputUnavailableRetry>) {
                return WindowOutputStateKind::
                    unavailable_retry;
            } else if constexpr (
                std::is_same_v<
                    State, WindowOutputSurfaceLost>) {
                return WindowOutputStateKind::surface_lost;
            } else if constexpr (
                std::is_same_v<
                    State,
                    WindowOutputDeviceRebuildRequired>) {
                return WindowOutputStateKind::
                    device_rebuild_required;
            } else {
                return WindowOutputStateKind::fatal;
            }
        },
        state_);
}

WindowOutputRecoveryReason
WindowOutputRecoveryStateMachine::reason() const noexcept {
    return std::visit(
        [](const auto &value) {
            using State =
                std::remove_cvref_t<decltype(value)>;
            if constexpr (
                requires { value.reason; }) {
                return value.reason;
            } else if constexpr (
                std::is_same_v<
                    State,
                    WindowOutputSuspendedZeroExtent>) {
                return WindowOutputRecoveryReason::
                    zero_extent;
            } else if constexpr (
                std::is_same_v<
                    State, WindowOutputSurfaceLost>) {
                return WindowOutputRecoveryReason::
                    surface_lost;
            } else if constexpr (
                std::is_same_v<
                    State,
                    WindowOutputDeviceRebuildRequired>) {
                return WindowOutputRecoveryReason::
                    device_lost;
            } else if constexpr (
                std::is_same_v<State, WindowOutputFatal>) {
                return WindowOutputRecoveryReason::fatal;
            } else {
                return WindowOutputRecoveryReason::none;
            }
        },
        state_);
}

bool WindowOutputRecoveryStateMachine::requestRefresh(
    SwapchainEpochId old_epoch,
    SwapchainRecoveryKey key,
    WindowOutputRecoveryReason request_reason,
    std::uint64_t tick) {
    if (request_reason ==
            WindowOutputRecoveryReason::suboptimal &&
        acknowledged_suboptimal_key_ == key) {
        return false;
    }

    if (const auto *pending =
            std::get_if<WindowOutputRefreshPending>(
                &state_);
        pending != nullptr && pending->key == key) {
        return false;
    }
    if (std::holds_alternative<
            WindowOutputPreparing>(state_)) {
        // A driver-blocking candidate is already in flight. The owner will
        // compare it with the latest framebuffer snapshot on completion and
        // enqueue one replacement if it became stale.
        return false;
    }
    if (const auto *retry =
            std::get_if<WindowOutputUnavailableRetry>(
                &state_);
        retry != nullptr && retry->key == key &&
        tick < retry->retry_after_tick) {
        return false;
    }
    if (std::holds_alternative<
            WindowOutputDeviceRebuildRequired>(state_) ||
        std::holds_alternative<WindowOutputFatal>(state_) ||
        std::holds_alternative<WindowOutputSurfaceLost>(
            state_)) {
        return false;
    }

    state_ = WindowOutputRefreshPending{
        old_epoch, std::move(key), request_reason};
    return true;
}

void WindowOutputRecoveryStateMachine::observeZeroExtent(
    SwapchainEpochId old_epoch,
    FramebufferExtentSnapshot framebuffer) {
    if (framebuffer.extent.width != 0 &&
        framebuffer.extent.height != 0) {
        throw std::invalid_argument(
            "zero-extent transition received a positive framebuffer");
    }
    state_ = WindowOutputSuspendedZeroExtent{
        old_epoch, framebuffer};
}

bool WindowOutputRecoveryStateMachine::
    resumeFromPositiveExtent(
        SwapchainEpochId old_epoch,
        SwapchainRecoveryKey key,
        std::uint64_t tick) {
    if (key.framebuffer_extent.width == 0 ||
        key.framebuffer_extent.height == 0) {
        return false;
    }
    if (!std::holds_alternative<
            WindowOutputSuspendedZeroExtent>(state_)) {
        return requestRefresh(
            old_epoch, std::move(key),
            WindowOutputRecoveryReason::
                framebuffer_changed,
            tick);
    }
    state_ = WindowOutputRefreshPending{
        old_epoch, std::move(key),
        WindowOutputRecoveryReason::
            framebuffer_changed};
    return true;
}

std::optional<SwapchainPreparationRequest>
WindowOutputRecoveryStateMachine::
    takePreparationRequest() {
    const auto *pending =
        std::get_if<WindowOutputRefreshPending>(
            &state_);
    if (pending == nullptr) return std::nullopt;
    auto request = SwapchainPreparationRequest{
        .old_epoch = pending->old_epoch,
        .key = pending->key,
        .reason = pending->reason,
        .attempt = 1,
    };
    state_ = WindowOutputPreparing{
        request.old_epoch, request.key,
        request.reason, request.attempt};
    return request;
}

bool WindowOutputRecoveryStateMachine::retryIfDue(
    std::uint64_t tick) {
    const auto *retry =
        std::get_if<WindowOutputUnavailableRetry>(
            &state_);
    if (retry == nullptr ||
        tick < retry->retry_after_tick) {
        return false;
    }
    state_ = WindowOutputRefreshPending{
        0, retry->key, retry->reason};
    return true;
}

void WindowOutputRecoveryStateMachine::
    preparationSucceeded(
        SwapchainEpochId new_epoch,
        SwapchainRecoveryKey key) {
    const auto *preparing =
        std::get_if<WindowOutputPreparing>(&state_);
    if (preparing == nullptr) {
        throw std::logic_error(
            "window output published a candidate outside preparation");
    }
    if (preparing->reason ==
        WindowOutputRecoveryReason::suboptimal) {
        acknowledged_suboptimal_key_ = key;
    }
    state_ =
        WindowOutputReady{new_epoch, std::move(key)};
}

void WindowOutputRecoveryStateMachine::
    preparationFailed(
        std::uint64_t tick,
        std::uint64_t retry_delay_ticks) {
    const auto *preparing =
        std::get_if<WindowOutputPreparing>(&state_);
    if (preparing == nullptr) {
        throw std::logic_error(
            "window output failed a candidate outside preparation");
    }
    const auto delay =
        std::max<std::uint64_t>(retry_delay_ticks, 1);
    state_ = WindowOutputUnavailableRetry{
        preparing->key,
        WindowOutputRecoveryReason::prepare_failed,
        tick + delay,
        preparing->attempt,
    };
}

void WindowOutputRecoveryStateMachine::deferRetry(
    SwapchainRecoveryKey key,
    WindowOutputRecoveryReason retry_reason,
    std::uint64_t tick,
    std::uint64_t retry_delay_ticks,
    std::uint32_t attempt) {
    const auto delay =
        std::max<std::uint64_t>(
            retry_delay_ticks, 1);
    state_ = WindowOutputUnavailableRetry{
        std::move(key), retry_reason,
        tick + delay, attempt,
    };
}

void WindowOutputRecoveryStateMachine::markSurfaceLost(
    SwapchainEpochId old_epoch) {
    state_ = WindowOutputSurfaceLost{old_epoch};
}

void WindowOutputRecoveryStateMachine::markDeviceLost() {
    state_ = WindowOutputDeviceRebuildRequired{};
}

void WindowOutputRecoveryStateMachine::markFatal() {
    state_ = WindowOutputFatal{};
}

std::string_view windowOutputStateName(
    WindowOutputStateKind state) noexcept {
    switch (state) {
    case WindowOutputStateKind::ready: return "ready";
    case WindowOutputStateKind::refresh_pending:
        return "refresh_pending";
    case WindowOutputStateKind::suspended_zero_extent:
        return "suspended_zero_extent";
    case WindowOutputStateKind::preparing:
        return "preparing";
    case WindowOutputStateKind::unavailable_retry:
        return "unavailable_retry";
    case WindowOutputStateKind::surface_lost:
        return "surface_lost";
    case WindowOutputStateKind::device_rebuild_required:
        return "device_rebuild_required";
    case WindowOutputStateKind::fatal: return "fatal";
    }
    return "unknown";
}

std::string_view windowOutputRecoveryReasonName(
    WindowOutputRecoveryReason reason) noexcept {
    switch (reason) {
    case WindowOutputRecoveryReason::none: return "none";
    case WindowOutputRecoveryReason::framebuffer_changed:
        return "framebuffer_changed";
    case WindowOutputRecoveryReason::suboptimal:
        return "suboptimal";
    case WindowOutputRecoveryReason::out_of_date:
        return "out_of_date";
    case WindowOutputRecoveryReason::abandoned_frame:
        return "abandoned_frame";
    case WindowOutputRecoveryReason::prepare_failed:
        return "prepare_failed";
    case WindowOutputRecoveryReason::resource_pressure:
        return "resource_pressure";
    case WindowOutputRecoveryReason::zero_extent:
        return "zero_extent";
    case WindowOutputRecoveryReason::surface_lost:
        return "surface_lost";
    case WindowOutputRecoveryReason::device_lost:
        return "device_lost";
    case WindowOutputRecoveryReason::fatal:
        return "fatal";
    }
    return "unknown";
}

} // namespace Pelican
