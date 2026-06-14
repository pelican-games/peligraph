#include "../src/core/appflow/enginetime.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE("EngineTime advances fixed-step time deterministically", "[enginetime]") {
    EngineTime time;
    time.setup(EngineTime::Mode::fixed_step, 0.25);

    REQUIRE(time.now() == Catch::Approx(0.0));
    REQUIRE(time.dt() == Catch::Approx(0.0));
    REQUIRE(time.frameIndex() == 0);

    time.advance();
    REQUIRE(time.now() == Catch::Approx(0.25));
    REQUIRE(time.dt() == Catch::Approx(0.25));
    REQUIRE(time.frameIndex() == 1);

    time.advance();
    time.advance();
    REQUIRE(time.now() == Catch::Approx(0.75));
    REQUIRE(time.dt() == Catch::Approx(0.25));
    REQUIRE(time.frameIndex() == 3);
}

TEST_CASE("EngineTime setTime updates the current time without advancing frames", "[enginetime]") {
    EngineTime time;
    time.setup(EngineTime::Mode::fixed_step, 0.5);
    time.advance();

    time.setTime(10.0);

    REQUIRE(time.now() == Catch::Approx(10.0));
    REQUIRE(time.dt() == Catch::Approx(0.0));
    REQUIRE(time.frameIndex() == 1);
}

TEST_CASE("EngineTime setup resets accumulated state", "[enginetime]") {
    EngineTime time;
    time.setup(EngineTime::Mode::fixed_step, 0.5);
    time.advance();
    time.setTime(3.0);

    time.setup(EngineTime::Mode::fixed_step, 0.125);

    REQUIRE(time.now() == Catch::Approx(0.0));
    REQUIRE(time.dt() == Catch::Approx(0.0));
    REQUIRE(time.frameIndex() == 0);

    time.advance();
    REQUIRE(time.now() == Catch::Approx(0.125));
    REQUIRE(time.dt() == Catch::Approx(0.125));
    REQUIRE(time.frameIndex() == 1);
}

} // namespace Pelican
