#include "inputstate.hpp"

#include <algorithm>
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

void InputStateCore::queueButtonEvent(KeyCode code, bool pressed) {
    queueEvent(InputEvent::button(code, pressed));
}

void InputStateCore::queueCursorMove(float x, float y) {
    queueEvent(InputEvent::cursorMove(x, y));
}

void InputStateCore::queueEvent(InputEvent event) {
    pending_events.push_back(event);
}

void InputStateCore::queueEvents(const std::vector<InputEvent> &events) {
    pending_events.insert(pending_events.end(), events.begin(), events.end());
}

void InputStateCore::beginFrame() {
    InputSnapshot next_snapshot{};
    const bool had_mouse_position = mouse_position_known;
    const float previous_mouse_x = mouse_x;
    const float previous_mouse_y = mouse_y;
    bool saw_mouse_move = false;

    for (const auto &event : pending_events) {
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
        }
    }

    next_snapshot.down = current_down;
    next_snapshot.mouse_x = mouse_x;
    next_snapshot.mouse_y = mouse_y;
    if (saw_mouse_move && had_mouse_position) {
        next_snapshot.mouse_delta_x = mouse_x - previous_mouse_x;
        next_snapshot.mouse_delta_y = mouse_y - previous_mouse_y;
    }

    pending_events.clear();
    snapshot = next_snapshot;
}

void InputStateCore::clear() {
    snapshot = {};
    current_down = {};
    pending_events.clear();
    mouse_x = 0.0f;
    mouse_y = 0.0f;
    mouse_position_known = false;
}

const InputSnapshot &InputStateCore::currentSnapshot() const noexcept {
    return snapshot;
}

std::size_t InputStateCore::pendingEventCount() const noexcept {
    return pending_events.size();
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

} // namespace Pelican
