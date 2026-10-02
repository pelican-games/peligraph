// IL-1: pil_eval (the intro link client's float evaluator, shared by the
// development and release forms) against the engine's ScalarCurve::sample.
// curve.cpp is compiled in directly; pelican_core is not linked.

#include "animation/curve.hpp"

#include "pelican_intro_link.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using Pelican::Animation::CurveInterpolation;
using Pelican::Animation::CurveKey;
using Pelican::Animation::ScalarCurve;

CurveInterpolation engineInterpolation(std::uint32_t wire) {
    if (wire == PIL_INTERPOLATION_STEP) return CurveInterpolation::step;
    if (wire == PIL_INTERPOLATION_CUBIC) return CurveInterpolation::cubic;
    return CurveInterpolation::linear;
}

int check(const char *label, const std::vector<pil_key> &keys) {
    std::vector<CurveKey> engine_keys;
    for (const auto &key : keys) {
        engine_keys.push_back({key.time, key.value, key.in_tangent, key.out_tangent,
                               engineInterpolation(key.interpolation)});
    }
    const ScalarCurve curve{engine_keys};

    // Before, at, just around and between every key, and past the end.
    std::vector<float> times;
    const float first = keys.front().time;
    const float last = keys.back().time;
    for (float t = first - 1.0f; t <= last + 1.0f; t += 0.0625f) times.push_back(t);
    for (const auto &key : keys) {
        times.push_back(key.time);
        times.push_back(std::nextafter(key.time, -1e9f));
        times.push_back(std::nextafter(key.time, 1e9f));
        times.push_back(key.time - 1e-3f);
        times.push_back(key.time + 1e-3f);
    }
    times.push_back(-100.0f);
    times.push_back(100.0f);

    int failures = 0;
    for (const float t : times) {
        const float actual = pil_eval(keys.data(), static_cast<std::uint32_t>(keys.size()), t);
        const double expected = curve.sample(static_cast<double>(t));
        const double tolerance = 1e-5 * std::max(1.0, std::abs(expected));
        if (!(std::abs(static_cast<double>(actual) - expected) <= tolerance)) {
            std::fprintf(stderr, "%s t=%.9g pil_eval=%.9g engine=%.9g\n", label, t, actual, expected);
            ++failures;
        }
    }
    std::printf("%s: %zu times, %d mismatches\n", label, times.size(), failures);
    return failures;
}

} // namespace

int main() {
    int failures = 0;
    failures += check("linear", {{0.0f, 0.0f, 0.0f, 0.0f, PIL_INTERPOLATION_LINEAR},
                                 {1.0f, 1.0f, 0.0f, 0.0f, PIL_INTERPOLATION_LINEAR},
                                 {3.0f, 0.25f, 0.0f, 0.0f, PIL_INTERPOLATION_LINEAR},
                                 {3.5f, -2.0f, 0.0f, 0.0f, PIL_INTERPOLATION_LINEAR}});
    failures += check("step", {{0.0f, 0.0f, 0.0f, 0.0f, PIL_INTERPOLATION_STEP},
                               {0.5f, 1.0f, 0.0f, 0.0f, PIL_INTERPOLATION_STEP},
                               {2.0f, 0.25f, 0.0f, 0.0f, PIL_INTERPOLATION_STEP}});
    failures += check("cubic", {{0.0f, 0.0f, 0.0f, 2.0f, PIL_INTERPOLATION_CUBIC},
                                {1.5f, 1.0f, 0.0f, -1.0f, PIL_INTERPOLATION_CUBIC},
                                {3.0f, -0.5f, 0.5f, 0.0f, PIL_INTERPOLATION_CUBIC},
                                {4.25f, 3.0f, -4.0f, 1.0f, PIL_INTERPOLATION_CUBIC}});
    // One curve whose segments switch interpolation (the left key decides).
    failures += check("mixed", {{-1.0f, 2.0f, 0.0f, 0.0f, PIL_INTERPOLATION_STEP},
                                {0.0f, -1.0f, 0.0f, 3.0f, PIL_INTERPOLATION_CUBIC},
                                {0.75f, 0.5f, 1.0f, 0.0f, PIL_INTERPOLATION_LINEAR},
                                {2.0f, 4.0f, 0.0f, 0.0f, PIL_INTERPOLATION_LINEAR}});
    failures += check("single", {{1.0f, 0.75f, 0.0f, 0.0f, PIL_INTERPOLATION_CUBIC}});
    if (failures != 0) {
        std::fprintf(stderr, "intro_link_eval_matches_engine: %d mismatches\n", failures);
        return 1;
    }
    std::printf("intro_link_eval_matches_engine: pil_eval matches ScalarCurve::sample\n");
    return 0;
}
