#pragma once

#include "../export.hpp"
#include <span>
#include <optional>
#include <vector>

namespace Pelican::Animation {

enum class CurveInterpolation { step, linear, cubic };
struct CurveKey {
    double time{};
    double value{};
    // Slopes in value / curve-time units, not Bezier control point positions.
    double in_tangent{};
    double out_tangent{};
    CurveInterpolation interpolation{CurveInterpolation::linear};
};

// Immutable shape. A single key is a constant; samples outside the key range
// clamp to its endpoints. Playback clocks and output scaling live separately.
class PELICAN_API ScalarCurve {
  public:
    explicit ScalarCurve(std::vector<CurveKey> keys);
    std::span<const CurveKey> keys() const noexcept { return keys_; }
    double sample(double time) const;
    std::vector<double> sampleUniform(double begin, double end, unsigned count) const;
  private:
    std::vector<CurveKey> keys_;
};

enum class CurveWrap { once, repeat, ping_pong };
struct CurvePlaybackSettings {
    double source_begin{0.0};
    double source_end{1.0};
    double duration{1.0};
    double speed{1.0};
    double delay{};
    double scale{1.0};
    double offset{};
    // Zero evaluates continuously; positive values hold samples on a grid
    // measured in elapsed seconds after delay (independent of frame rate).
    double sample_interval{};
    CurveWrap wrap{CurveWrap::once};
};
struct CurveSample {
    double source_time{};
    double value{};
    bool active{};
    bool completed{};
};

struct CurveTimeSample {
    double source_time{};
    bool active{};
    bool completed{};
};
// Time mapping only; scale and offset belong to the later value transform.
PELICAN_API CurveTimeSample mapCurveTime(const CurvePlaybackSettings &settings,
                                        double elapsed);

// Stateless evaluation supports both a trigger clock and externally supplied
// time. Negative elapsed time/delay returns the initial value but is inactive.
PELICAN_API CurveSample sampleCurve(const ScalarCurve &curve,
                                   const CurvePlaybackSettings &settings,
                                   double elapsed);

enum class CurveRetrigger { restart, ignore_active };
enum class CurvePlaybackCommand { start, pause, resume, reset };
class PELICAN_API CurveTriggerClock {
  public:
    explicit CurveTriggerClock(CurvePlaybackSettings settings = {});
    bool trigger(double time, CurveRetrigger policy = CurveRetrigger::restart);
    void stop() noexcept { triggered_ = false; paused_at_.reset(); }
    void pause(double time);
    void resume(double time);
    double elapsed(double time) const;
    CurveSample sample(const ScalarCurve &curve, double time) const;
  private:
    CurvePlaybackSettings settings_;
    double started_{};
    bool triggered_{};
    std::optional<double> paused_at_;
};

} // namespace Pelican::Animation
