#include "swapchainrecovery.hpp"

#include <algorithm>
#include <limits>
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

std::string_view wsiResultClassName(
    WsiResultClass classification) noexcept {
    switch (classification) {
    case WsiResultClass::ready:
        return "ready";
    case WsiResultClass::refresh_advisory:
        return "refresh_advisory";
    case WsiResultClass::not_ready:
        return "not_ready";
    case WsiResultClass::swapchain_unavailable:
        return "swapchain_unavailable";
    case WsiResultClass::surface_unavailable:
        return "surface_unavailable";
    case WsiResultClass::device_lost:
        return "device_lost";
    case WsiResultClass::retryable_failure:
        return "retryable_failure";
    case WsiResultClass::fatal:
        return "fatal";
    }
    return "unknown";
}

WindowWsiRecoveryDecision decideWindowWsiRecovery(
    WindowWsiCallSite site,
    vk::Result result) noexcept {
    const auto classification =
        classifyWsiResult(result);
    const bool frame_call =
        site == WindowWsiCallSite::acquire ||
        site == WindowWsiCallSite::present;

    switch (classification) {
    case WsiResultClass::ready:
        return {
            classification,
            WindowWsiRecoveryAction::proceed};
    case WsiResultClass::refresh_advisory:
        return {
            classification,
            frame_call
                ? WindowWsiRecoveryAction::
                      proceed_and_refresh
                : WindowWsiRecoveryAction::
                      retry_later};
    case WsiResultClass::not_ready:
        return {
            classification,
            site == WindowWsiCallSite::acquire
                ? WindowWsiRecoveryAction::drop_frame
                : WindowWsiRecoveryAction::retry_later};
    case WsiResultClass::swapchain_unavailable:
        return {
            classification,
            site == WindowWsiCallSite::
                            surface_create ||
                    site == WindowWsiCallSite::
                                surface_support_query
                ? WindowWsiRecoveryAction::
                      replace_surface
                : WindowWsiRecoveryAction::
                      replace_swapchain};
    case WsiResultClass::surface_unavailable:
        return {
            classification,
            WindowWsiRecoveryAction::replace_surface};
    case WsiResultClass::device_lost:
        return {
            classification,
            WindowWsiRecoveryAction::rebuild_device};
    case WsiResultClass::retryable_failure:
        return {
            classification,
            WindowWsiRecoveryAction::retry_later};
    case WsiResultClass::fatal:
        // Surface/swapchain preparation is failure-contained: it has no
        // partially-published object, so keep the output completely
        // unavailable and retry with backoff. Runtime acquire/present errors
        // cannot be hidden behind that transaction and remain terminal.
        return {
            classification,
            frame_call
                ? WindowWsiRecoveryAction::fatal
                : WindowWsiRecoveryAction::
                      retry_later};
    }
    return {
        classification,
        WindowWsiRecoveryAction::fatal};
}

std::string_view windowWsiCallSiteName(
    WindowWsiCallSite site) noexcept {
    switch (site) {
    case WindowWsiCallSite::acquire:
        return "acquire";
    case WindowWsiCallSite::present:
        return "present";
    case WindowWsiCallSite::surface_create:
        return "surface_create";
    case WindowWsiCallSite::surface_support_query:
        return "surface_support_query";
    case WindowWsiCallSite::swapchain_create:
        return "swapchain_create";
    case WindowWsiCallSite::dependent_resources:
        return "dependent_resources";
    }
    return "unknown";
}

std::string_view windowWsiRecoveryActionName(
    WindowWsiRecoveryAction action) noexcept {
    switch (action) {
    case WindowWsiRecoveryAction::proceed:
        return "proceed";
    case WindowWsiRecoveryAction::
        proceed_and_refresh:
        return "proceed_and_refresh";
    case WindowWsiRecoveryAction::drop_frame:
        return "drop_frame";
    case WindowWsiRecoveryAction::
        replace_swapchain:
        return "replace_swapchain";
    case WindowWsiRecoveryAction::replace_surface:
        return "replace_surface";
    case WindowWsiRecoveryAction::rebuild_device:
        return "rebuild_device";
    case WindowWsiRecoveryAction::retry_later:
        return "retry_later";
    case WindowWsiRecoveryAction::fatal:
        return "fatal";
    }
    return "unknown";
}

SwapchainRecoveryAnchorKind
selectSwapchainRecoveryAnchor(
    bool replacement_available,
    bool previous_available) noexcept {
    if (replacement_available) {
        return SwapchainRecoveryAnchorKind::replacement;
    }
    if (previous_available) {
        return SwapchainRecoveryAnchorKind::previous;
    }
    return SwapchainRecoveryAnchorKind::none;
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
                return value.preparation_kind ==
                               WindowOutputPreparationKind::
                                   surface
                           ? WindowOutputStateKind::
                                 preparing_surface
                           : WindowOutputStateKind::
                                 preparing;
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

std::uint32_t
WindowOutputRecoveryStateMachine::attempt() const noexcept {
    return std::visit(
        [](const auto &value) -> std::uint32_t {
            if constexpr (
                requires { value.attempt; }) {
                return value.attempt;
            } else {
                return 0;
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
    if (const auto *retry =
            std::get_if<WindowOutputUnavailableRetry>(
                &state_);
        retry != nullptr &&
        retry->preparation_kind ==
            WindowOutputPreparationKind::surface) {
        state_ = WindowOutputSurfaceLost{
            retry->old_epoch, retry->attempt};
        return true;
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
    auto preparation_kind =
        WindowOutputPreparationKind::swapchain;
    std::uint32_t attempt = 0;
    if (const auto *preparing =
            std::get_if<WindowOutputPreparing>(
                &state_);
        preparing != nullptr) {
        preparation_kind =
            preparing->preparation_kind;
        attempt = preparing->attempt;
    } else if (const auto *retry =
                   std::get_if<
                       WindowOutputUnavailableRetry>(
                       &state_);
               retry != nullptr) {
        preparation_kind =
            retry->preparation_kind;
        attempt = retry->attempt;
    } else if (const auto *lost =
                   std::get_if<
                       WindowOutputSurfaceLost>(
                       &state_);
               lost != nullptr) {
        preparation_kind =
            WindowOutputPreparationKind::surface;
        attempt = lost->attempt;
    }
    state_ = WindowOutputSuspendedZeroExtent{
        old_epoch, framebuffer,
        preparation_kind, attempt};
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
    const auto *suspended =
        std::get_if<
            WindowOutputSuspendedZeroExtent>(
            &state_);
    if (suspended == nullptr) {
        return requestRefresh(
            old_epoch, std::move(key),
            WindowOutputRecoveryReason::
                framebuffer_changed,
            tick);
    }
    if (suspended->preparation_kind ==
        WindowOutputPreparationKind::surface) {
        state_ = WindowOutputSurfaceLost{
            suspended->old_epoch,
            suspended->attempt};
        return true;
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
        request.reason, request.attempt,
        request.preparation_kind};
    return request;
}

std::optional<SwapchainPreparationRequest>
WindowOutputRecoveryStateMachine::
    takeSurfacePreparationRequest(
        SwapchainRecoveryKey key) {
    const auto *lost =
        std::get_if<WindowOutputSurfaceLost>(
            &state_);
    if (lost == nullptr) return std::nullopt;
    auto request = SwapchainPreparationRequest{
        .old_epoch = lost->old_epoch,
        .key = std::move(key),
        .reason =
            WindowOutputRecoveryReason::surface_lost,
        .attempt =
            lost->attempt ==
                    std::numeric_limits<
                        std::uint32_t>::max()
                ? lost->attempt
                : lost->attempt + 1,
        .preparation_kind =
            WindowOutputPreparationKind::surface,
    };
    state_ = WindowOutputPreparing{
        request.old_epoch, request.key,
        request.reason, request.attempt,
        request.preparation_kind};
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
    if (retry->preparation_kind ==
        WindowOutputPreparationKind::surface) {
        state_ = WindowOutputSurfaceLost{
            retry->old_epoch, retry->attempt};
    } else {
        state_ = WindowOutputRefreshPending{
            retry->old_epoch,
            retry->key, retry->reason};
    }
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
        preparing->old_epoch,
        preparing->key,
        preparing->preparation_kind ==
                WindowOutputPreparationKind::surface
            ? WindowOutputRecoveryReason::
                  surface_lost
            : WindowOutputRecoveryReason::
                  prepare_failed,
        tick + delay,
        preparing->attempt,
        preparing->preparation_kind,
    };
}

void WindowOutputRecoveryStateMachine::deferRetry(
    SwapchainRecoveryKey key,
    WindowOutputRecoveryReason retry_reason,
    std::uint64_t tick,
    std::uint64_t retry_delay_ticks,
    std::uint32_t attempt,
    WindowOutputPreparationKind
        preparation_kind,
    SwapchainEpochId old_epoch) {
    const auto delay =
        std::max<std::uint64_t>(
            retry_delay_ticks, 1);
    state_ = WindowOutputUnavailableRetry{
        old_epoch, std::move(key), retry_reason,
        tick + delay, attempt, preparation_kind,
    };
}

void WindowOutputRecoveryStateMachine::markSurfaceLost(
    SwapchainEpochId old_epoch) {
    std::uint32_t attempt = 0;
    if (const auto *preparing =
            std::get_if<WindowOutputPreparing>(
                &state_);
        preparing != nullptr &&
        preparing->preparation_kind ==
            WindowOutputPreparationKind::surface) {
        attempt = preparing->attempt;
    } else if (const auto *retry =
                   std::get_if<
                       WindowOutputUnavailableRetry>(
                       &state_);
               retry != nullptr &&
               retry->preparation_kind ==
                   WindowOutputPreparationKind::
                       surface) {
        attempt = retry->attempt;
    } else if (const auto *lost =
                   std::get_if<
                       WindowOutputSurfaceLost>(
                       &state_);
               lost != nullptr) {
        attempt = lost->attempt;
    }
    state_ = WindowOutputSurfaceLost{
        old_epoch, attempt};
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
    case WindowOutputStateKind::preparing_surface:
        return "preparing_surface";
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
