#include "../src/core/os/inputstate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace Pelican {

TEST_CASE("InputSnapshot is a serializable value shape", "[inputstate]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<InputSnapshot>);
    STATIC_REQUIRE(std::is_standard_layout_v<InputSnapshot>);
}

TEST_CASE("InputStateCore reports pushed and released edges once per frame", "[inputstate]") {
    InputStateCore input;

    input.queueButtonEvent(KeyCode::A, true);
    input.beginFrame();
    REQUIRE(input.currentSnapshot().getKey(KeyCode::A));
    REQUIRE(input.currentSnapshot().isKeyPushed(KeyCode::A));
    REQUIRE_FALSE(input.currentSnapshot().isKeyReleased(KeyCode::A));

    input.beginFrame();
    REQUIRE(input.currentSnapshot().getKey(KeyCode::A));
    REQUIRE_FALSE(input.currentSnapshot().isKeyPushed(KeyCode::A));
    REQUIRE_FALSE(input.currentSnapshot().isKeyReleased(KeyCode::A));

    input.queueButtonEvent(KeyCode::A, false);
    input.beginFrame();
    REQUIRE_FALSE(input.currentSnapshot().getKey(KeyCode::A));
    REQUIRE_FALSE(input.currentSnapshot().isKeyPushed(KeyCode::A));
    REQUIRE(input.currentSnapshot().isKeyReleased(KeyCode::A));

    input.beginFrame();
    REQUIRE_FALSE(input.currentSnapshot().getKey(KeyCode::A));
    REQUIRE_FALSE(input.currentSnapshot().isKeyPushed(KeyCode::A));
    REQUIRE_FALSE(input.currentSnapshot().isKeyReleased(KeyCode::A));
}

TEST_CASE("InputStateCore derives edges from ordered events within one frame", "[inputstate]") {
    InputStateCore input;

    input.queueButtonEvent(KeyCode::MouseLeft, true);
    input.queueButtonEvent(KeyCode::MouseLeft, false);
    input.beginFrame();

    REQUIRE_FALSE(input.currentSnapshot().getKey(KeyCode::MouseLeft));
    REQUIRE(input.currentSnapshot().isKeyPushed(KeyCode::MouseLeft));
    REQUIRE(input.currentSnapshot().isKeyReleased(KeyCode::MouseLeft));
}

TEST_CASE("InputStateCore stores mouse position and per-frame delta", "[inputstate]") {
    InputStateCore input;

    input.queueCursorMove(10.0f, 20.0f);
    input.beginFrame();
    REQUIRE(input.currentSnapshot().mouse_x == 10.0f);
    REQUIRE(input.currentSnapshot().mouse_y == 20.0f);
    REQUIRE(input.currentSnapshot().mouse_delta_x == 0.0f);
    REQUIRE(input.currentSnapshot().mouse_delta_y == 0.0f);

    input.queueCursorMove(12.5f, 17.0f);
    input.queueCursorMove(15.0f, 19.0f);
    input.beginFrame();
    REQUIRE(input.currentSnapshot().mouse_x == 15.0f);
    REQUIRE(input.currentSnapshot().mouse_y == 19.0f);
    REQUIRE(input.currentSnapshot().mouse_delta_x == 5.0f);
    REQUIRE(input.currentSnapshot().mouse_delta_y == -1.0f);
}

TEST_CASE("InputStateCore applies injected axis deltas for one frame", "[inputstate]") {
    InputStateCore input;

    input.queueAxisEvent(3.0f, -2.0f);
    input.beginFrame();
    REQUIRE(input.currentSnapshot().mouse_delta_x == 3.0f);
    REQUIRE(input.currentSnapshot().mouse_delta_y == -2.0f);

    input.beginFrame();
    REQUIRE(input.currentSnapshot().mouse_delta_x == 0.0f);
    REQUIRE(input.currentSnapshot().mouse_delta_y == 0.0f);
}

TEST_CASE("InputStateCore clear returns the snapshot to empty headless state", "[inputstate]") {
    InputStateCore input;

    input.queueButtonEvent(KeyCode::F1, true);
    input.queueCursorMove(3.0f, 4.0f);
    input.beginFrame();
    input.clear();

    REQUIRE_FALSE(input.currentSnapshot().getKey(KeyCode::F1));
    REQUIRE_FALSE(input.currentSnapshot().isKeyPushed(KeyCode::F1));
    REQUIRE_FALSE(input.currentSnapshot().isKeyReleased(KeyCode::F1));
    REQUIRE(input.currentSnapshot().mouse_x == 0.0f);
    REQUIRE(input.currentSnapshot().mouse_y == 0.0f);
    REQUIRE(input.pendingEventCount() == 0);
}

} // namespace Pelican
