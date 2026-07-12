#include "../src/core/os/inputstate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace Pelican {

TEST_CASE("InputSnapshot is a serializable value shape", "[inputstate]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<InputSnapshot>);
    STATIC_REQUIRE(std::is_standard_layout_v<InputSnapshot>);
}

TEST_CASE("Input events receive a process-lifetime monotonic sequence when queued", "[inputstate][frame-input]") {
    STATIC_REQUIRE(std::is_same_v<decltype(InputEvent::event_seq), std::uint64_t>);

    InputStateCore input;
    input.queueButtonEvent(KeyCode::A, true);
    input.queueCursorMove(10.0f, 20.0f);
    input.beginFrame();

    {
        const auto frame = input.currentFrameInput();
        REQUIRE(frame.ordered_events.size() == 2);
        REQUIRE(frame.ordered_events[0].type == InputEvent::Type::button);
        REQUIRE(frame.ordered_events[0].event_seq == 0);
        REQUIRE(frame.ordered_events[1].type == InputEvent::Type::cursor_move);
        REQUIRE(frame.ordered_events[1].event_seq == 1);
    }

    input.clear();
    input.queueAxisEvent(1.0f, -1.0f);
    input.beginFrame();
    const auto frame = input.currentFrameInput();
    REQUIRE(frame.ordered_events.size() == 1);
    REQUIRE(frame.ordered_events[0].event_seq == 2);
}

TEST_CASE("Recorded input sequences use the canonical queue without renumbering", "[inputstate][frame-input]") {
    InputStateCore input;
    auto recorded = InputEvent::button(KeyCode::A, true);
    recorded.event_seq = 40;
    input.queueEvent(recorded);
    input.queueCursorMove(1.0f, 2.0f);
    input.beginFrame();

    const auto frame = input.currentFrameInput();
    REQUIRE(frame.ordered_events[0].event_seq == 40);
    REQUIRE(frame.ordered_events[1].event_seq == 41);
}

TEST_CASE("FrameInput borrow is scoped to one frame generation", "[inputstate][frame-input]") {
    InputStateCore input;
    input.queueButtonEvent(KeyCode::A, true);
    input.beginFrame();

    {
        auto frame = input.currentFrameInput();
        REQUIRE(frame.isValid());
        REQUIRE(frame.generation() == input.frameGeneration());
        REQUIRE(input.frameInputBorrowCount() == 1);

        const auto copied_frame = frame;
        REQUIRE(copied_frame.isValid());
        REQUIRE(input.frameInputBorrowCount() == 2);
    }

    REQUIRE(input.frameInputBorrowCount() == 0);
    input.beginFrame();
    REQUIRE(input.currentFrameInput().ordered_events.empty());
}

TEST_CASE("Actions consumption mask does not mutate the public snapshot", "[inputstate][frame-input]") {
    InputStateCore input;
    input.queueButtonEvent(KeyCode::MouseLeft, true);
    input.queueAxisEvent(3.0f, -2.0f);
    input.beginFrame();

    input.consumePointerForActions();
    const auto actions_snapshot = input.freezeActionsSnapshot();
    REQUIRE_FALSE(actions_snapshot.getKey(KeyCode::MouseLeft));
    REQUIRE_FALSE(actions_snapshot.isKeyPushed(KeyCode::MouseLeft));
    REQUIRE(actions_snapshot.mouse_delta_x == 0.0f);
    REQUIRE(actions_snapshot.mouse_delta_y == 0.0f);

    REQUIRE(input.currentSnapshot().getKey(KeyCode::MouseLeft));
    REQUIRE(input.currentSnapshot().isKeyPushed(KeyCode::MouseLeft));
    REQUIRE(input.currentSnapshot().mouse_delta_x == 3.0f);
    REQUIRE(input.currentSnapshot().mouse_delta_y == -2.0f);
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

TEST_CASE("Gamepad poller is inactive without a bound profile and emits ordered changes", "[inputstate][gamepad]") {
    class FakeSource final : public GamepadStateSource {
      public:
        std::size_t reads = 0;
        bool connected = true;
        GamepadState state{};

        bool read(std::size_t slot, GamepadState &output) override {
            ++reads;
            if (slot != 0 || !connected) {
                return false;
            }
            output = state;
            return true;
        }
    } source;

    GamepadEventPoller poller;
    REQUIRE(poller.poll(source, false).empty());
    REQUIRE(source.reads == 0);

    source.state.buttons[static_cast<std::size_t>(GamepadButton::A)] = 1;
    source.state.axes[static_cast<std::size_t>(GamepadAxis::LeftX)] = 0.5f;
    const auto events = poller.poll(source, true);
    REQUIRE(source.reads == gamepad_slot_count);
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].type == InputEvent::Type::gamepad_button);
    REQUIRE(events[1].type == InputEvent::Type::gamepad_axis);

    source.connected = false;
    const auto released = poller.poll(source, true);
    REQUIRE(released.size() == 2);
    REQUIRE(released[0].pressed == 0);
    REQUIRE(released[1].pad_value == 0.0f);
}

} // namespace Pelican
