#include "frametarget.hpp"

#include <string>
#include <utility>

namespace Pelican {

FrameTargetFrame::FrameTargetFrame(
    FrameRenderContext context,
    std::shared_ptr<const RendererRuntimeGeneration>
        runtime_generation,
    GpuSubmissionLease submission_lease,
    std::shared_ptr<FrameTargetFrameCleanup> cleanup,
    std::uint64_t serial)
    : context_{std::move(context)},
      runtime_generation_{std::move(runtime_generation)},
      submission_lease_{std::move(submission_lease)},
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
    std::uint64_t serial) {
    if (cleanup == nullptr) {
        throw std::logic_error(
            "frame target created a token without cleanup control");
    }
    return FrameTargetFrame{
        std::move(context), std::move(runtime_generation),
        std::move(submission_lease), cleanup, serial};
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

} // namespace Pelican
