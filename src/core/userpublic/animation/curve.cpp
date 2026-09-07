#include "curve.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace Pelican::Animation {
namespace {
void finite(double value) {
    if (!std::isfinite(value)) throw std::invalid_argument("curve values must be finite");
}
void validateTime(const CurvePlaybackSettings &s) {
    for (double value : {s.source_begin, s.source_end, s.duration, s.speed,
                         s.delay, s.sample_interval}) finite(value);
    if (s.source_end < s.source_begin || s.duration <= 0 || s.speed == 0 ||
        s.delay < 0 || s.sample_interval < 0)
        throw std::invalid_argument("invalid curve playback range, duration, speed or sampling interval");
    if (s.wrap != CurveWrap::once && s.wrap != CurveWrap::repeat && s.wrap != CurveWrap::ping_pong)
        throw std::invalid_argument("unknown curve wrap mode");
}
void validate(const CurvePlaybackSettings &s) {
    validateTime(s);
    finite(s.scale);
    finite(s.offset);
}
double progress(const CurvePlaybackSettings &s, double local_time) {
    const auto result = std::max(0.0, local_time) / s.duration * std::abs(s.speed);
    if (!std::isfinite(result)) throw std::out_of_range("curve playback time overflow");
    return result;
}
}

ScalarCurve::ScalarCurve(std::vector<CurveKey> keys) : keys_(std::move(keys)) {
    if (keys_.empty()) throw std::invalid_argument("curve requires at least one key");
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        const auto &key = keys_[i];
        for (double value : {key.time, key.value, key.in_tangent, key.out_tangent}) finite(value);
        if (i && !(keys_[i - 1].time < key.time))
            throw std::invalid_argument("curve key times must be strictly increasing");
        if (key.interpolation != CurveInterpolation::step &&
            key.interpolation != CurveInterpolation::linear &&
            key.interpolation != CurveInterpolation::cubic)
            throw std::invalid_argument("unknown curve interpolation");
    }
}

double ScalarCurve::sample(double time) const {
    finite(time);
    if (time <= keys_.front().time) return keys_.front().value;
    if (time >= keys_.back().time) return keys_.back().value;
    const auto next = std::upper_bound(keys_.begin(), keys_.end(), time,
        [](double t, const CurveKey &key) { return t < key.time; });
    const auto &a = *std::prev(next);
    const auto &b = *next;
    if (a.interpolation == CurveInterpolation::step) return a.value;
    const auto width = b.time - a.time;
    const auto t = (time - a.time) / width;
    if (a.interpolation == CurveInterpolation::linear) return std::lerp(a.value, b.value, t);
    const auto t2 = t * t;
    const auto t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * a.value + (t3 - 2 * t2 + t) * width * a.out_tangent +
           (-2 * t3 + 3 * t2) * b.value + (t3 - t2) * width * b.in_tangent;
}

std::vector<double> ScalarCurve::sampleUniform(double begin, double end, unsigned count) const {
    finite(begin);
    finite(end);
    if (end < begin || count < 2) throw std::invalid_argument("curve LUT requires an ordered range and at least two samples");
    std::vector<double> result(count);
    for (unsigned i = 0; i < count; ++i)
        result[i] = sample(std::lerp(begin, end, static_cast<double>(i) / (count - 1)));
    return result;
}

CurveTimeSample mapCurveTime(const CurvePlaybackSettings &s, double elapsed) {
    validateTime(s);
    finite(elapsed);
    const auto raw_progress = progress(s, elapsed - s.delay);
    const bool waiting = elapsed < s.delay;
    const bool completed = !waiting && s.wrap == CurveWrap::once && raw_progress >= 1;
    auto local = std::max(0.0, elapsed - s.delay);
    if (s.sample_interval > 0) local = std::floor(local / s.sample_interval) * s.sample_interval;
    auto phase = progress(s, local);
    if (s.wrap == CurveWrap::once) phase = completed ? 1.0 : std::min(phase, 1.0);
    else if (s.wrap == CurveWrap::repeat) phase = std::fmod(phase, 1.0);
    else {
        phase = std::fmod(phase, 2.0);
        if (phase > 1.0) phase = 2.0 - phase;
    }
    if (s.speed < 0) phase = 1.0 - phase;
    const auto source_time = std::lerp(s.source_begin, s.source_end, phase);
    return {source_time, !waiting && !completed, completed};
}

CurveSample sampleCurve(const ScalarCurve &curve, const CurvePlaybackSettings &s, double elapsed) {
    finite(s.scale);
    finite(s.offset);
    const auto time = mapCurveTime(s, elapsed);
    return {time.source_time, curve.sample(time.source_time) * s.scale + s.offset, time.active, time.completed};
}

CurveTriggerClock::CurveTriggerClock(CurvePlaybackSettings settings) : settings_(settings) { validate(settings_); }
bool CurveTriggerClock::trigger(double time, CurveRetrigger policy) {
    finite(time);
    if (policy != CurveRetrigger::restart && policy != CurveRetrigger::ignore_active)
        throw std::invalid_argument("unknown curve retrigger policy");
    if (triggered_ && policy == CurveRetrigger::ignore_active &&
        (settings_.wrap != CurveWrap::once || elapsed(time) < settings_.delay ||
         progress(settings_, elapsed(time) - settings_.delay) < 1)) return false;
    started_ = time;
    triggered_ = true;
    paused_at_.reset();
    return true;
}
void CurveTriggerClock::pause(double time) {
    finite(time);
    if (triggered_ && !paused_at_) paused_at_ = time;
}
void CurveTriggerClock::resume(double time) {
    finite(time);
    if (paused_at_) {
        started_ += time - *paused_at_;
        paused_at_.reset();
    }
}
double CurveTriggerClock::elapsed(double time) const {
    finite(time);
    return triggered_ ? paused_at_.value_or(time) - started_ : 0.0;
}
CurveSample CurveTriggerClock::sample(const ScalarCurve &curve, double time) const {
    finite(time);
    auto result = sampleCurve(curve, settings_, elapsed(time));
    if (!triggered_) { result.active = false; result.completed = false; }
    return result;
}

} // namespace Pelican::Animation
