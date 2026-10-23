#include "audioclock.hpp"

#include <algorithm>
#include <cmath>

namespace Pelican {

namespace {

// Until the stamps span this long, the line keeps the nominal rate as its
// slope and fits only where it stands: a slope fitted over a few chunks of
// jittered callbacks is worse than the device's own rate.
constexpr double minimumSlopeSpanSeconds = 0.25;
// The drift is measured from a point on a line at least this long: the
// callbacks' jitter puts a short line's reading a fraction of a millisecond
// off, and the drift is the difference of two readings.
constexpr double driftReferenceSpanSeconds = 1.0;
// A stamp this long after the one before belongs to another run of the
// device (it stopped, stalled or was rerouted): its line starts anew.
constexpr double newLineGapSeconds = 0.25;
// How far past its last stamp the line is read. A frame's head is normally
// within a chunk of it; a device that has gone quiet stops the clock here
// rather than letting it run on.
constexpr double maximumExtrapolationSeconds = 0.1;

} // namespace

void AudioClockRing::push(const AudioClockStamp &stamp) noexcept {
    const auto index = written_.load(std::memory_order_relaxed);
    auto &slot = slots_[index % capacity];
    slot.sequence.store(2 * index + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    slot.steady_ns.store(stamp.steady_ns, std::memory_order_relaxed);
    slot.frames.store(stamp.frames, std::memory_order_relaxed);
    slot.sequence.store(2 * index + 2, std::memory_order_release);
    written_.store(index + 1, std::memory_order_release);
}

AudioDeviceClock::AudioDeviceClock(double sample_rate, double latency_frames, double window_seconds)
    : sample_rate_{sample_rate > 0.0 ? sample_rate : 48000.0},
      latency_frames_{std::isfinite(latency_frames) && latency_frames > 0.0 ? latency_frames : 0.0},
      window_seconds_{window_seconds > 0.0 ? window_seconds : 1.5} {}

void AudioDeviceClock::startLine() {
    drift_before_ = last_.drift_seconds;
    drift_reference_.reset();
    points_.clear();
}

void AudioDeviceClock::add(double seconds, std::uint64_t frames) {
    if (!std::isfinite(seconds)) return;
    const auto count = static_cast<double>(frames);
    if (!points_.empty()) {
        const auto &last = points_.back();
        if (seconds < last.seconds) return;
        if (count < last.frames || seconds - last.seconds > newLineGapSeconds) startLine();
    }
    points_.push_back({seconds, count});
    while (points_.size() > 2 && points_.back().seconds - points_.front().seconds > window_seconds_)
        points_.pop_front();
}

void AudioDeviceClock::hold(bool held) {
    if (held == held_) return;
    held_ = held;
    // Started again: whatever the stamps say from here is a new run.
    if (!held) startLine();
}

AudioDeviceClock::Reading AudioDeviceClock::at(double seconds) {
    if (held_ || points_.empty() || !std::isfinite(seconds)) return last_;
    const auto &first = points_.front();
    const auto &last = points_.back();
    const auto when = (std::min)(seconds, last.seconds + maximumExtrapolationSeconds);
    // frames(t) = mean_frames + slope * (t - mean_t), least squares.
    double mean_seconds = 0.0;
    double mean_frames = 0.0;
    for (const auto &point : points_) {
        mean_seconds += point.seconds - first.seconds;
        mean_frames += point.frames - first.frames;
    }
    const auto count = static_cast<double>(points_.size());
    mean_seconds /= count;
    mean_frames /= count;
    auto slope = sample_rate_;
    const auto span = last.seconds - first.seconds;
    if (points_.size() >= 3 && span >= minimumSlopeSpanSeconds) {
        double seconds_squares = 0.0;
        double cross = 0.0;
        for (const auto &point : points_) {
            const auto dt = point.seconds - first.seconds - mean_seconds;
            seconds_squares += dt * dt;
            cross += dt * (point.frames - first.frames - mean_frames);
        }
        if (seconds_squares > 0.0) slope = cross / seconds_squares;
    }
    const auto frames = first.frames + mean_frames + slope * (when - first.seconds - mean_seconds);
    const auto heard = (frames - latency_frames_) / sample_rate_;
    if (!drift_reference_ && span >= driftReferenceSpanSeconds) drift_reference_.emplace(when, heard);
    Reading reading;
    reading.running = true;
    reading.frames = frames;
    reading.audio_time = (std::max)({last_.audio_time, heard, 0.0});
    reading.drift_seconds = drift_before_;
    if (drift_reference_)
        reading.drift_seconds += (heard - drift_reference_->second) - (when - drift_reference_->first);
    last_ = reading;
    return reading;
}

} // namespace Pelican
