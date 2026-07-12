#include "inputstate.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <type_traits>

namespace Pelican {

namespace {

std::size_t keyCodeIndex(KeyCode code) noexcept {
    return static_cast<std::size_t>(code);
}

std::size_t countActive(const std::array<std::uint8_t, key_code_count> &values) noexcept {
    return static_cast<std::size_t>(std::count(values.begin(), values.end(), std::uint8_t{1}));
}

} // namespace

bool isValidKeyCode(KeyCode code) noexcept {
    using KeyCodeValue = std::underlying_type_t<KeyCode>;
    return static_cast<KeyCodeValue>(code) < static_cast<KeyCodeValue>(KeyCode::Count);
}

bool InputSnapshot::getKey(KeyCode code) const noexcept {
    return isValidKeyCode(code) && down[keyCodeIndex(code)] != 0;
}

bool InputSnapshot::isKeyPushed(KeyCode code) const noexcept {
    return isValidKeyCode(code) && pushed[keyCodeIndex(code)] != 0;
}

bool InputSnapshot::isKeyReleased(KeyCode code) const noexcept {
    return isValidKeyCode(code) && released[keyCodeIndex(code)] != 0;
}

std::size_t InputSnapshot::downCount() const noexcept {
    return countActive(down);
}

std::size_t InputSnapshot::pushedCount() const noexcept {
    return countActive(pushed);
}

std::size_t InputSnapshot::releasedCount() const noexcept {
    return countActive(released);
}

InputEvent InputEvent::button(KeyCode code, bool pressed) noexcept {
    return InputEvent{
        .type = Type::button,
        .code = code,
        .pressed = static_cast<std::uint8_t>(pressed ? 1 : 0),
    };
}

InputEvent InputEvent::cursorMove(float x, float y) noexcept {
    return InputEvent{
        .type = Type::cursor_move,
        .mouse_x = x,
        .mouse_y = y,
    };
}

InputEvent InputEvent::axis(float x, float y) noexcept {
    return InputEvent{
        .type = Type::axis,
        .axis_x = x,
        .axis_y = y,
    };
}

InputEvent InputEvent::scroll(float x, float y) noexcept {
    return InputEvent{
        .type = Type::scroll,
        .axis_x = x,
        .axis_y = y,
    };
}

InputEvent InputEvent::character(std::uint32_t codepoint) noexcept {
    return InputEvent{
        .type = Type::character,
        .codepoint = codepoint,
    };
}

void InputConsumptionMask::consumeControl(KeyCode code) noexcept {
    if (isValidKeyCode(code)) {
        controls[keyCodeIndex(code)] = 1;
    }
}

void InputConsumptionMask::consumePointerMotion() noexcept {
    pointer_motion = true;
}

void InputConsumptionMask::consumePointer() noexcept {
    for (auto code = static_cast<std::underlying_type_t<KeyCode>>(KeyCode::MouseLeft);
         code <= static_cast<std::underlying_type_t<KeyCode>>(KeyCode::MouseButton8); ++code) {
        consumeControl(static_cast<KeyCode>(code));
    }
    consumePointerMotion();
}

bool InputConsumptionMask::consumesControl(KeyCode code) const noexcept {
    return isValidKeyCode(code) && controls[keyCodeIndex(code)] != 0;
}

InputSnapshot InputConsumptionMask::apply(const InputSnapshot &source) const noexcept {
    auto result = source;
    for (std::size_t index = 0; index < controls.size(); ++index) {
        if (controls[index] == 0) {
            continue;
        }
        result.down[index] = 0;
        result.pushed[index] = 0;
        result.released[index] = 0;
    }
    if (pointer_motion) {
        result.mouse_delta_x = 0.0f;
        result.mouse_delta_y = 0.0f;
    }
    return result;
}

FrameInput::FrameInput(std::span<const InputEvent> events, const InputSnapshot &frame_snapshot,
                       std::shared_ptr<internal::FrameInputBorrowState> state) noexcept
    : ordered_events(events), snapshot(frame_snapshot), borrow_state(std::move(state)),
      borrowed_generation(borrow_state ? borrow_state->generation : 0) {
    acquire();
}

FrameInput::FrameInput(const FrameInput &other) noexcept
    : ordered_events(other.ordered_events), snapshot(other.snapshot), borrow_state(other.borrow_state),
      borrowed_generation(other.borrowed_generation) {
    acquire();
}

FrameInput::FrameInput(FrameInput &&other) noexcept
    : ordered_events(other.ordered_events), snapshot(other.snapshot), borrow_state(std::move(other.borrow_state)),
      borrowed_generation(other.borrowed_generation) {
    other.ordered_events = {};
    other.borrowed_generation = 0;
}

FrameInput &FrameInput::operator=(const FrameInput &other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    ordered_events = other.ordered_events;
    snapshot = other.snapshot;
    borrow_state = other.borrow_state;
    borrowed_generation = other.borrowed_generation;
    acquire();
    return *this;
}

FrameInput &FrameInput::operator=(FrameInput &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    ordered_events = other.ordered_events;
    snapshot = other.snapshot;
    borrow_state = std::move(other.borrow_state);
    borrowed_generation = other.borrowed_generation;
    other.ordered_events = {};
    other.borrowed_generation = 0;
    return *this;
}

FrameInput::~FrameInput() {
    release();
}

bool FrameInput::isValid() const noexcept {
    return borrow_state != nullptr && borrow_state->active && borrow_state->generation == borrowed_generation;
}

std::uint64_t FrameInput::generation() const noexcept {
    return borrowed_generation;
}

void FrameInput::acquire() noexcept {
    if (borrow_state != nullptr) {
        ++borrow_state->borrowers;
    }
}

void FrameInput::release() noexcept {
    if (borrow_state != nullptr) {
        assert(borrow_state->borrowers > 0);
        --borrow_state->borrowers;
        borrow_state.reset();
    }
}

InputStateCore::~InputStateCore() {
    borrow_state->active = false;
    assert(borrow_state->borrowers == 0 && "FrameInput must not outlive its InputState owner");
}

void InputStateCore::queueButtonEvent(KeyCode code, bool pressed) {
    queueEvent(InputEvent::button(code, pressed));
}

void InputStateCore::queueCursorMove(float x, float y) {
    queueEvent(InputEvent::cursorMove(x, y));
}

void InputStateCore::queueAxisEvent(float x, float y) {
    queueEvent(InputEvent::axis(x, y));
}

void InputStateCore::queueEvent(InputEvent event) {
    if (event.event_seq == InputEvent::unassigned_sequence) {
        if (next_event_seq == InputEvent::unassigned_sequence) {
            throw std::overflow_error("input event sequence exhausted");
        }
        event.event_seq = next_event_seq++;
    } else {
        if (event.event_seq < next_event_seq) {
            throw std::runtime_error("recorded input event sequence is not monotonic");
        }
        if (event.event_seq == InputEvent::unassigned_sequence - 1) {
            next_event_seq = InputEvent::unassigned_sequence;
        } else {
            next_event_seq = event.event_seq + 1;
        }
    }
    pending_events.push_back(event);
}

void InputStateCore::queueEvents(const std::vector<InputEvent> &events) {
    for (const auto &event : events) {
        queueEvent(event);
    }
}

void InputStateCore::beginFrame() {
    borrow_state->active = false;
    assert(borrow_state->borrowers == 0 && "FrameInput must not be retained across frames");
    ++frame_generation;
    borrow_state->generation = frame_generation;
    borrow_state->active = true;
    frame_active = true;
    actions_frozen = false;
    consumption_mask = {};
    frame_events.clear();
    frame_events.swap(pending_events);

    InputSnapshot next_snapshot{};
    const bool had_mouse_position = mouse_position_known;
    const float previous_mouse_x = mouse_x;
    const float previous_mouse_y = mouse_y;
    bool saw_mouse_move = false;
    float axis_delta_x = 0.0f;
    float axis_delta_y = 0.0f;

    for (const auto &event : frame_events) {
        switch (event.type) {
        case InputEvent::Type::button: {
            if (!isValidKeyCode(event.code)) {
                break;
            }

            const auto index = keyCodeIndex(event.code);
            const bool was_down = current_down[index] != 0;
            const bool is_down = event.pressed != 0;
            if (is_down && !was_down) {
                next_snapshot.pushed[index] = 1;
            } else if (!is_down && was_down) {
                next_snapshot.released[index] = 1;
            }
            current_down[index] = static_cast<std::uint8_t>(is_down ? 1 : 0);
            break;
        }
        case InputEvent::Type::cursor_move:
            saw_mouse_move = true;
            mouse_x = event.mouse_x;
            mouse_y = event.mouse_y;
            mouse_position_known = true;
            break;
        case InputEvent::Type::axis:
            axis_delta_x += event.axis_x;
            axis_delta_y += event.axis_y;
            break;
        case InputEvent::Type::scroll:
        case InputEvent::Type::character:
            break;
        }
    }

    next_snapshot.down = current_down;
    next_snapshot.mouse_x = mouse_x;
    next_snapshot.mouse_y = mouse_y;
    next_snapshot.mouse_delta_x = axis_delta_x;
    next_snapshot.mouse_delta_y = axis_delta_y;
    if (saw_mouse_move && had_mouse_position) {
        next_snapshot.mouse_delta_x += mouse_x - previous_mouse_x;
        next_snapshot.mouse_delta_y += mouse_y - previous_mouse_y;
    }

    snapshot = next_snapshot;
}

void InputStateCore::clear() {
    borrow_state->active = false;
    assert(borrow_state->borrowers == 0 && "FrameInput must not be retained while input state is cleared");
    ++frame_generation;
    borrow_state->generation = frame_generation;
    snapshot = {};
    current_down = {};
    pending_events.clear();
    frame_events.clear();
    consumption_mask = {};
    mouse_x = 0.0f;
    mouse_y = 0.0f;
    mouse_position_known = false;
    frame_active = false;
    actions_frozen = false;
}

const InputSnapshot &InputStateCore::currentSnapshot() const noexcept {
    return snapshot;
}

FrameInput InputStateCore::currentFrameInput() const noexcept {
    assert(frame_active && "FrameInput is only available after beginFrame");
    return FrameInput{frame_events, snapshot, borrow_state};
}

void InputStateCore::consumeControlForActions(KeyCode code) noexcept {
    assert(frame_active && !actions_frozen && "input controls must be consumed before Actions are frozen");
    consumption_mask.consumeControl(code);
}

void InputStateCore::consumePointerMotionForActions() noexcept {
    assert(frame_active && !actions_frozen && "pointer motion must be consumed before Actions are frozen");
    consumption_mask.consumePointerMotion();
}

void InputStateCore::consumePointerForActions() noexcept {
    assert(frame_active && !actions_frozen && "pointer input must be consumed before Actions are frozen");
    consumption_mask.consumePointer();
}

const InputConsumptionMask &InputStateCore::consumptionMask() const noexcept {
    return consumption_mask;
}

InputSnapshot InputStateCore::freezeActionsSnapshot() noexcept {
    actions_frozen = true;
    return consumption_mask.apply(snapshot);
}

std::uint64_t InputStateCore::frameGeneration() const noexcept {
    return frame_generation;
}

std::size_t InputStateCore::frameInputBorrowCount() const noexcept {
    return borrow_state->borrowers;
}

std::size_t InputStateCore::pendingEventCount() const noexcept {
    return pending_events.size();
}

std::uint64_t InputStateCore::nextEventSequence() const noexcept {
    return next_event_seq;
}

void InputState::queueEvent(InputEvent event) {
    core.queueEvent(event);
}

void InputState::queueEvents(const std::vector<InputEvent> &events) {
    core.queueEvents(events);
}

void InputState::beginFrame() {
    core.beginFrame();
}

void InputState::clear() {
    core.clear();
}

const InputSnapshot &InputState::currentSnapshot() const noexcept {
    return core.currentSnapshot();
}

FrameInput InputState::currentFrameInput() const noexcept {
    return core.currentFrameInput();
}

void InputState::consumeControlForActions(KeyCode code) noexcept {
    core.consumeControlForActions(code);
}

void InputState::consumePointerMotionForActions() noexcept {
    core.consumePointerMotionForActions();
}

void InputState::consumePointerForActions() noexcept {
    core.consumePointerForActions();
}

const InputConsumptionMask &InputState::consumptionMask() const noexcept {
    return core.consumptionMask();
}

InputSnapshot InputState::freezeActionsSnapshot() noexcept {
    return core.freezeActionsSnapshot();
}

std::uint64_t InputState::frameGeneration() const noexcept {
    return core.frameGeneration();
}

std::uint64_t InputState::nextEventSequence() const noexcept {
    return core.nextEventSequence();
}

std::size_t InputState::pendingEventCount() const noexcept {
    return core.pendingEventCount();
}

} // namespace Pelican
