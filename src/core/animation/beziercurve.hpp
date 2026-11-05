#pragma once

// CB-1: the engine's Bezier curve. Not a public userpublic type: the frozen
// Animation::ScalarCurve cannot grow handles, so value.curve nodes that use a
// Bezier key get this immutable shape instead. It owns its segment-kind enum
// because the frozen CurveInterpolation must not change.

#include "../userpublic/animation/beziersegment.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Pelican::Animation {

enum class BezierSegmentKind { step, linear, cubic, bezier };

struct BezierCurveKey {
    double time{};
    double value{};
    // Slopes in value / curve-time units, like CurveKey.
    double in_tangent{};
    double out_tangent{};
    // The interpolation from this key to the next. b's kind does not describe
    // a segment; it describes the segment b starts.
    BezierSegmentKind kind{BezierSegmentKind::linear};
    // Handles are (time delta, value delta) from the owning key. A missing
    // handle is not an explicit [0, 0]: it takes the cubic-equivalent default.
    std::optional<std::array<double, 2>> out_handle;
    std::optional<std::array<double, 2>> in_handle;
};

// Immutable shape. A single key is a constant; samples outside the key range
// clamp to its endpoints. First/last in/out handles and handles on non-Bezier
// segments are unused by sampling but are kept and validated all the same.
class BezierCurve {
  public:
    explicit BezierCurve(std::vector<BezierCurveKey> keys) : keys_(std::move(keys)) {
        if (keys_.empty()) throw std::invalid_argument("curve requires at least one key");
        for (std::size_t index = 0; index < keys_.size(); ++index) {
            const auto &key = keys_[index];
            for (double value : {key.time, key.value, key.in_tangent, key.out_tangent}) {
                if (!std::isfinite(value))
                    throw std::invalid_argument("curve values must be finite");
            }
            if (key.out_handle) {
                if (!std::isfinite((*key.out_handle)[0]) || !std::isfinite((*key.out_handle)[1]))
                    throw std::invalid_argument("curve values must be finite");
                if ((*key.out_handle)[0] < 0.0)
                    throw std::invalid_argument("curve handle time deltas have the wrong sign");
            }
            if (key.in_handle) {
                if (!std::isfinite((*key.in_handle)[0]) || !std::isfinite((*key.in_handle)[1]))
                    throw std::invalid_argument("curve values must be finite");
                if ((*key.in_handle)[0] > 0.0)
                    throw std::invalid_argument("curve handle time deltas have the wrong sign");
            }
            if (index && !(keys_[index - 1].time < key.time))
                throw std::invalid_argument("curve key times must be strictly increasing");
            if (key.kind != BezierSegmentKind::step && key.kind != BezierSegmentKind::linear &&
                key.kind != BezierSegmentKind::cubic && key.kind != BezierSegmentKind::bezier)
                throw std::invalid_argument("unknown curve interpolation");
        }
    }

    std::span<const BezierCurveKey> keys() const noexcept { return keys_; }

    double sample(double time) const {
        if (!std::isfinite(time)) throw std::invalid_argument("curve values must be finite");
        if (time <= keys_.front().time) return keys_.front().value;
        if (time >= keys_.back().time) return keys_.back().value;
        const auto next = std::upper_bound(keys_.begin(), keys_.end(), time,
            [](double at, const BezierCurveKey &key) { return at < key.time; });
        const auto &a = *std::prev(next);
        const auto &b = *next;
        const double width = b.time - a.time;
        switch (a.kind) {
        case BezierSegmentKind::step:
            return a.value;
        case BezierSegmentKind::linear:
            return std::lerp(a.value, b.value, (time - a.time) / width);
        case BezierSegmentKind::cubic: {
            const double t = (time - a.time) / width;
            const double t2 = t * t;
            const double t3 = t2 * t;
            return (2.0 * t3 - 3.0 * t2 + 1.0) * a.value +
                   (t3 - 2.0 * t2 + t) * width * a.out_tangent +
                   (-2.0 * t3 + 3.0 * t2) * b.value +
                   (t3 - t2) * width * b.in_tangent;
        }
        case BezierSegmentKind::bezier: {
            // A missing side takes its own default; either side can be
            // explicit independently.
            const std::array<double, 2> out =
                a.out_handle ? *a.out_handle
                             : std::array<double, 2>{width / 3.0, a.out_tangent * width / 3.0};
            const std::array<double, 2> in =
                b.in_handle ? *b.in_handle
                            : std::array<double, 2>{-width / 3.0, -b.in_tangent * width / 3.0};
            return bezierSegmentValue(a.time, a.value, out[0], out[1], b.time, b.value, in[0],
                                      in[1], time);
        }
        }
        throw std::invalid_argument("unknown curve interpolation");
    }

    std::vector<double> sampleUniform(double begin, double end, unsigned count) const {
        if (!std::isfinite(begin) || !std::isfinite(end))
            throw std::invalid_argument("curve values must be finite");
        if (end < begin || count < 2)
            throw std::invalid_argument(
                "curve LUT requires an ordered range and at least two samples");
        std::vector<double> result(count);
        for (unsigned index = 0; index < count; ++index)
            result[index] = sample(
                std::lerp(begin, end, static_cast<double>(index) / (count - 1)));
        return result;
    }

  private:
    std::vector<BezierCurveKey> keys_;
};

} // namespace Pelican::Animation
