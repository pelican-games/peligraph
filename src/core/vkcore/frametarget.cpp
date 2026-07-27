#include "frametarget.hpp"

#include <string>
#include <utility>

namespace Pelican {

FrameTargetFrame::FrameTargetFrame(
    FrameRenderContext context,
    std::shared_ptr<const RendererRuntimeGeneration>
        runtime_generation,
    GpuSubmissionLease submission_lease,
    GpuSubmissionLease target_epoch_lease,
    std::shared_ptr<FrameTargetFrameCleanup> cleanup,
    std::uint64_t serial)
    : context_{std::move(context)},
      runtime_generation_{std::move(runtime_generation)},
      submission_lease_{std::move(submission_lease)},
      target_epoch_lease_{
          std::move(target_epoch_lease)},
      cleanup_{std::move(cleanup)}, serial_{serial},
      state_{FrameTargetFrameState::recording} {}

void FrameTargetFrame::abandonIfActive() noexcept {
    if (state_ != FrameTargetFrameState::recording) {
        return;
    }
    state_ = FrameTargetFrameState::empty;
    if (cleanup_ != nullptr) cleanup_->abandon(serial_);
    runtime_generation_.reset();
    submission_lease_.reset();
    target_epoch_lease_.reset();
    cleanup_.reset();
}

FrameTargetFrame::~FrameTargetFrame() {
    abandonIfActive();
}

FrameTargetFrame::FrameTargetFrame(
    FrameTargetFrame &&other) noexcept
    : context_{std::move(other.context_)},
      runtime_generation_{
          std::move(other.runtime_generation_)},
      submission_lease_{
          std::move(other.submission_lease_)},
      target_epoch_lease_{
          std::move(other.target_epoch_lease_)},
      cleanup_{std::move(other.cleanup_)},
      serial_{other.serial_}, state_{other.state_} {
    other.state_ = FrameTargetFrameState::empty;
    other.serial_ = 0;
}

FrameTargetFrame &FrameTargetFrame::operator=(
    FrameTargetFrame &&other) noexcept {
    if (this == &other) return *this;
    abandonIfActive();
    context_ = std::move(other.context_);
    runtime_generation_ =
        std::move(other.runtime_generation_);
    submission_lease_ =
        std::move(other.submission_lease_);
    target_epoch_lease_ =
        std::move(other.target_epoch_lease_);
    cleanup_ = std::move(other.cleanup_);
    serial_ = other.serial_;
    state_ = other.state_;
    other.state_ = FrameTargetFrameState::empty;
    other.serial_ = 0;
    return *this;
}

FrameTargetFrame IFrameTarget::makeFrame(
    FrameRenderContext context,
    std::shared_ptr<const RendererRuntimeGeneration>
        runtime_generation,
    GpuSubmissionLease submission_lease,
    const std::shared_ptr<FrameTargetFrameCleanup> &cleanup,
    std::uint64_t serial,
    GpuSubmissionLease target_epoch_lease) {
    if (cleanup == nullptr) {
        throw std::logic_error(
            "frame target created a token without cleanup control");
    }
    return FrameTargetFrame{
        std::move(context), std::move(runtime_generation),
        std::move(submission_lease),
        std::move(target_epoch_lease), cleanup, serial};
}

void IFrameTarget::validateFrameTarget(
    const FrameTargetFrame &frame,
    const std::shared_ptr<FrameTargetFrameCleanup>
        &expected_cleanup,
    std::string_view target_name) {
    if (frame.state_ != FrameTargetFrameState::recording) {
        throw std::logic_error(
            std::string{target_name} +
            " received an already-consumed frame token");
    }
    if (frame.cleanup_ == nullptr ||
        frame.cleanup_ != expected_cleanup) {
        throw std::logic_error(
            std::string{target_name} +
            " received a frame token owned by another target");
    }
}

IFrameTarget::ConsumedFrame IFrameTarget::consumeFrame(
    FrameTargetFrame &&frame,
    const std::shared_ptr<FrameTargetFrameCleanup>
        &expected_cleanup,
    std::uint64_t expected_serial,
    std::string_view target_name) {
    validateFrameTarget(
        frame, expected_cleanup, target_name);
    if (frame.serial_ != expected_serial) {
        throw std::logic_error(
            std::string{target_name} +
            " received a stale frame token");
    }
    frame.state_ = FrameTargetFrameState::empty;
    auto result = ConsumedFrame{
        .context = std::move(frame.context_),
        .runtime_generation =
            std::move(frame.runtime_generation_),
        .submission_lease =
            std::move(frame.submission_lease_),
        .target_epoch_lease =
            std::move(frame.target_epoch_lease_),
        .serial = frame.serial_,
    };
    frame.cleanup_.reset();
    frame.serial_ = 0;
    return result;
}

void IFrameTarget::abandonFrame(
    FrameTargetFrame &&frame) noexcept {
    frame.abandonIfActive();
}

FrameTargetStatus IFrameTarget::status() const {
    return FrameTargetStatus{
        .output_facts_fingerprint =
            outputCompileFactsFingerprint(
                caps().compile_facts),
    };
}

std::string_view frameTargetLifecycleStateName(
    FrameTargetLifecycleState state) noexcept {
    switch (state) {
    case FrameTargetLifecycleState::ready:
        return "ready";
    case FrameTargetLifecycleState::preparing:
        return "preparing";
    case FrameTargetLifecycleState::
        preparing_surface:
        return "preparing_surface";
    case FrameTargetLifecycleState::
        suspended_zero_extent:
        return "suspended_zero_extent";
    case FrameTargetLifecycleState::
        unavailable_retry:
        return "unavailable_retry";
    case FrameTargetLifecycleState::surface_lost:
        return "surface_lost";
    case FrameTargetLifecycleState::
        device_rebuild_required:
        return "device_rebuild_required";
    case FrameTargetLifecycleState::fatal:
        return "fatal";
    }
    return "unknown";
}

std::string_view frameUnavailableReasonName(
    FrameUnavailableReason reason) noexcept {
    switch (reason) {
    case FrameUnavailableReason::none:
        return "none";
    case FrameUnavailableReason::zero_extent:
        return "zero_extent";
    case FrameUnavailableReason::acquire_not_ready:
        return "acquire_not_ready";
    case FrameUnavailableReason::output_out_of_date:
        return "output_out_of_date";
    case FrameUnavailableReason::surface_stale:
        return "surface_stale";
    case FrameUnavailableReason::output_preparing:
        return "output_preparing";
    case FrameUnavailableReason::retry_pending:
        return "retry_pending";
    case FrameUnavailableReason::output_reconfigured:
        return "output_reconfigured";
    case FrameUnavailableReason::surface_lost:
        return "surface_lost";
    case FrameUnavailableReason::device_lost:
        return "device_lost";
    case FrameUnavailableReason::
        device_rebuild_required:
        return "device_rebuild_required";
    case FrameUnavailableReason::fatal:
        return "fatal";
    }
    return "unknown";
}

} // namespace Pelican
