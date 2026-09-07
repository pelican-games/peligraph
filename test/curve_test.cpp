#include "../src/core/userpublic/animation/curve.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

using namespace Pelican::Animation;
using Catch::Approx;

TEST_CASE("scalar curves preserve slope units and step key boundaries", "[animation][curve]") {
    // The Hermite segment represents y = (time - 2)^2 over four seconds.
    const ScalarCurve parabola{{
        {.time = 2, .value = 0, .out_tangent = 0, .interpolation = CurveInterpolation::cubic},
        {.time = 6, .value = 16, .in_tangent = 8},
    }};
    REQUIRE(parabola.sample(3) == Approx(1));
    REQUIRE(parabola.sample(4) == Approx(4));
    REQUIRE(parabola.sample(5) == Approx(9));
    REQUIRE(parabola.sample(-1) == 0);
    REQUIRE(parabola.sample(9) == 16);

    const ScalarCurve steps{{
        {.time = 0, .value = 3, .interpolation = CurveInterpolation::step},
        {.time = 2, .value = 7, .interpolation = CurveInterpolation::step},
        {.time = 5, .value = 11},
    }};
    REQUIRE(steps.sample(std::nextafter(2.0, 0.0)) == 3);
    REQUIRE(steps.sample(2) == 7);
    REQUIRE(steps.sample(std::nextafter(5.0, 0.0)) == 7);
    REQUIRE(steps.sample(5) == 11);
    const ScalarCurve constant{{{.time = 4, .value = 9}}};
    REQUIRE(constant.sample(-100) == 9);
}

TEST_CASE("curve playback reuses shape with source range scale reverse and wrapping", "[animation][curve]") {
    const ScalarCurve curve{{{.time = 0, .value = 0}, {.time = 10, .value = 100}}};
    CurvePlaybackSettings settings{.source_begin = 2, .source_end = 8, .duration = 4,
                                   .speed = 2, .scale = 0.5, .offset = -3};
    auto sample = sampleCurve(curve, settings, 0.5);
    REQUIRE(sample.source_time == Approx(3.5));
    REQUIRE(sample.value == Approx(14.5));
    REQUIRE(sample.active);
    REQUIRE_FALSE(sample.completed);
    sample = sampleCurve(curve, settings, 2);
    REQUIRE(sample.value == 37);
    REQUIRE(sample.completed);
    REQUIRE_FALSE(sample.active);

    settings.speed = -2;
    REQUIRE(sampleCurve(curve, settings, 0).source_time == 8);
    REQUIRE(sampleCurve(curve, settings, 0.5).source_time == Approx(6.5));
    REQUIRE(sampleCurve(curve, settings, 3).source_time == 2);
    settings.wrap = CurveWrap::repeat;
    REQUIRE(sampleCurve(curve, settings, 2).source_time == 8);
    sample = sampleCurve(curve, settings, 2.5);
    REQUIRE(sample.source_time == Approx(6.5));
    REQUIRE(sample.active);
    REQUIRE_FALSE(sample.completed);
    settings.speed = 2;
    settings.wrap = CurveWrap::ping_pong;
    REQUIRE(sampleCurve(curve, settings, 2).source_time == 8);
    REQUIRE(sampleCurve(curve, settings, 3).source_time == 5);
    REQUIRE(sampleCurve(curve, settings, 4).source_time == 2);
}

TEST_CASE("curve triggers include delay in retrigger lifetime and restart after completion", "[animation][curve]") {
    const ScalarCurve curve{{{.time = 0, .value = 0}, {.time = 1, .value = 10}}};
    CurveTriggerClock clock{{.duration = 2, .delay = 1}};
    REQUIRE_FALSE(clock.sample(curve, 100).active);
    REQUIRE_FALSE(clock.sample(curve, 100).completed);
    REQUIRE(clock.trigger(10));
    REQUIRE_FALSE(clock.sample(curve, 10.5).active);
    REQUIRE_FALSE(clock.trigger(10.5, CurveRetrigger::ignore_active));
    REQUIRE(clock.sample(curve, 11).active);
    REQUIRE(clock.sample(curve, 12).value == 5);
    REQUIRE_FALSE(clock.trigger(12, CurveRetrigger::ignore_active));
    REQUIRE(clock.trigger(12, CurveRetrigger::restart));
    REQUIRE_FALSE(clock.sample(curve, 12.5).active);
    REQUIRE(clock.sample(curve, 14).value == 5);
    REQUIRE(clock.sample(curve, 15).completed);
    REQUIRE(clock.trigger(15, CurveRetrigger::ignore_active));
    clock.stop();
    REQUIRE_FALSE(clock.sample(curve, 100).active);
    REQUIRE_FALSE(clock.sample(curve, 100).completed);
    REQUIRE(clock.trigger(100, CurveRetrigger::ignore_active));
    REQUIRE(clock.sample(curve, 102).value == 5);
}

TEST_CASE("fixed curve sampling is independent of calls and includes final value and LUT endpoints", "[animation][curve]") {
    const ScalarCurve curve{{{.time = 0, .value = 0}, {.time = 1, .value = 10}}};
    const CurvePlaybackSettings settings{.duration = 1, .delay = 0.5, .sample_interval = 0.3};
    REQUIRE(sampleCurve(curve, settings, 0.79).value == 0);
    REQUIRE(sampleCurve(curve, settings, 0.99).value == Approx(3));
    REQUIRE(sampleCurve(curve, settings, 1.49).value == Approx(9));
    REQUIRE(sampleCurve(curve, settings, 0.99).value == Approx(3));
    const auto completed = sampleCurve(curve, settings, 1.5);
    REQUIRE(completed.value == 10);
    REQUIRE(completed.completed);
    const auto lut = curve.sampleUniform(0.2, 0.8, 4);
    REQUIRE(lut.size() == 4);
    REQUIRE(lut[0] == Approx(2));
    REQUIRE(lut[1] == Approx(4));
    REQUIRE(lut[2] == Approx(6));
    REQUIRE(lut[3] == Approx(8));
}

TEST_CASE("curve input rejects ambiguous keys and invalid playback settings", "[animation][curve]") {
    REQUIRE_THROWS_AS(ScalarCurve(std::vector<CurveKey>{}), std::invalid_argument);
    const std::vector<CurveKey> duplicates{{.time = 1}, {.time = 1}};
    REQUIRE_THROWS_AS(ScalarCurve(duplicates), std::invalid_argument);
    const ScalarCurve curve{{{.time = 0, .value = 1}}};
    REQUIRE_THROWS_AS(curve.sample(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
    REQUIRE_THROWS_AS(curve.sampleUniform(0, 1, 1), std::invalid_argument);
    CurvePlaybackSettings settings;
    settings.duration = 0;
    REQUIRE_THROWS_AS(sampleCurve(curve, settings, 0), std::invalid_argument);
}
