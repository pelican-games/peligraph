#pragma once

#include "../container.hpp"
#include "../userpublic/userinput.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace Pelican {

bool isValidKeyCode(KeyCode code) noexcept;

struct InputSnapshot {
    std::array<std::uint8_t, key_code_count> down{};
    std::array<std::uint8_t, key_code_count> pushed{};
    std::array<std::uint8_t, key_code_count> released{};
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    float mouse_delta_x = 0.0f;
    float mouse_delta_y = 0.0f;

    bool getKey(KeyCode code) const noexcept;
    bool isKeyPushed(KeyCode code) const noexcept;
    bool isKeyReleased(KeyCode code) const noexcept;
    std::size_t downCount() const noexcept;
    std::size_t pushedCount() const noexcept;
    std::size_t releasedCount() const noexcept;
};

struct InputEvent {
    enum class Type : std::uint8_t {
        button,
        cursor_move,
        axis,
    };

    Type type = Type::button;
    KeyCode code = KeyCode::Count;
    std::uint8_t pressed = 0;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    float axis_x = 0.0f;
    float axis_y = 0.0f;

    static InputEvent button(KeyCode code, bool pressed) noexcept;
    static InputEvent cursorMove(float x, float y) noexcept;
    static InputEvent axis(float x, float y) noexcept;
};

static_assert(std::is_trivially_copyable_v<InputSnapshot>);
static_assert(std::is_standard_layout_v<InputSnapshot>);
static_assert(std::is_trivially_copyable_v<InputEvent>);
static_assert(std::is_standard_layout_v<InputEvent>);

class InputStateCore {
    InputSnapshot snapshot{};
    std::array<std::uint8_t, key_code_count> current_down{};
    std::vector<InputEvent> pending_events;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    bool mouse_position_known = false;

  public:
    void queueButtonEvent(KeyCode code, bool pressed);
    void queueCursorMove(float x, float y);
    void queueAxisEvent(float x, float y);
    void queueEvent(InputEvent event);
    void queueEvents(const std::vector<InputEvent> &events);
    void beginFrame();
    void clear();

    const InputSnapshot &currentSnapshot() const noexcept;
    std::size_t pendingEventCount() const noexcept;
};

DECLARE_MODULE(InputState) {
    InputStateCore core;

  public:
    void queueEvent(InputEvent event);
    void queueEvents(const std::vector<InputEvent> &events);
    void beginFrame();
    void clear();
    const InputSnapshot &currentSnapshot() const noexcept;
};

} // namespace Pelican
