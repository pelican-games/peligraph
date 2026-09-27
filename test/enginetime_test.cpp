#include "../src/core/appflow/enginetime.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

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

// PM-1a acceptance (default launch): playing, the evaluation index and the
// game frame are the same number and the game dt is the frame's own dt.
TEST_CASE("PM-1a playing advances the evaluation index and the game frame together",
          "[enginetime][pm1a]") {
    EngineTime time;
    time.setup(EngineTime::Mode::fixed_step, 0.25);
    REQUIRE(time.playState() == EngineTime::PlayState::playing);
    for (int frame = 1; frame <= 5; ++frame) {
        time.advance();
        REQUIRE(time.gameStepping());
        REQUIRE(time.evaluationIndex() == static_cast<std::uint64_t>(frame));
        REQUIRE(time.frameIndex() == time.evaluationIndex());
        REQUIRE(time.dt() == Catch::Approx(0.25));
        REQUIRE(time.editorDt() == Catch::Approx(0.25));
    }
    REQUIRE(time.now() == Catch::Approx(1.25));
}

// PM-1a acceptance 1 and 2, the clock half: paused, every frame is still an
// evaluation, but the game frame and the game clock stay where they stopped
// (dt 0); requested steps move them one per frame by the step delta.
TEST_CASE("PM-1a paused frames evaluate without moving the game, and steps move it by 1/fps",
          "[enginetime][pm1a]") {
    EngineTime time;
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    time.advance();
    time.advance();
    const auto game_frame = time.frameIndex();
    const auto game_time = time.now();

    REQUIRE(time.setPlayState(EngineTime::PlayState::paused) == EngineTime::PlayState::playing);
    REQUIRE(time.setPlayState(EngineTime::PlayState::paused) == EngineTime::PlayState::paused);
    for (int frame = 0; frame < 4; ++frame) {
        const auto evaluation = time.evaluationIndex();
        time.advance();
        REQUIRE_FALSE(time.gameStepping());
        REQUIRE(time.evaluationIndex() == evaluation + 1);
        REQUIRE(time.frameIndex() == game_frame);
        REQUIRE(time.now() == game_time);
        REQUIRE(time.dt() == 0.0);
        REQUIRE(time.editorDt() == Catch::Approx(1.0 / 60.0));
    }

    time.requestSteps(3);
    REQUIRE(time.pendingSteps() == 3);
    for (int step = 1; step <= 3; ++step) {
        time.advance();
        REQUIRE(time.gameStepping());
        REQUIRE(time.dt() == Catch::Approx(1.0 / 60.0));
        REQUIRE(time.frameIndex() == game_frame + static_cast<std::uint64_t>(step));
        REQUIRE(time.pendingSteps() == static_cast<std::uint64_t>(3 - step));
    }
    REQUIRE(time.now() == Catch::Approx(game_time + 3.0 / 60.0));
    time.advance();
    REQUIRE_FALSE(time.gameStepping());
    REQUIRE(time.frameIndex() == game_frame + 3);
    REQUIRE(time.playState() == EngineTime::PlayState::paused);
    REQUIRE(time.evaluationIndex() == 2 + 4 + 3 + 1);

    // Steps are a paused game's; playing again drops any not yet taken.
    time.requestSteps(2);
    REQUIRE(time.setPlayState(EngineTime::PlayState::playing) == EngineTime::PlayState::paused);
    REQUIRE(time.pendingSteps() == 0);
    REQUIRE_THROWS(time.requestSteps(1));
}

// PM-1a acceptance 4. The resume's first frame measures from the resume, not
// from the last frame drawn before it: however long the game sat between the
// last paused frame and the resume, the first playing dt does not jump.
TEST_CASE("PM-1a the first realtime frame after a resume does not measure the pause",
          "[enginetime][pm1a]") {
    EngineTime time;
    time.setup(EngineTime::Mode::realtime, 1.0 / 60.0);
    time.advance();
    time.setPlayState(EngineTime::PlayState::paused);
    time.advance();
    const auto paused_at = time.now();
    // The gap a Pause button leaves between the last frame and Play.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    time.setPlayState(EngineTime::PlayState::playing);
    time.advance();
    REQUIRE(time.gameStepping());
    REQUIRE(time.dt() < 0.04);
    REQUIRE(time.now() - paused_at < 0.04);
}

} // namespace Pelican
