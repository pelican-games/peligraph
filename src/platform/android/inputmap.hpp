#pragma once
// NDK-independent Android stable input constants and conversions, also built by CPU tests.
#include "../../core/os/inputstate.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
namespace Pelican::Android {
inline constexpr int source_keyboard = 0x101, source_gamepad = 0x401, source_joystick = 0x1000010;
inline constexpr int source_touchscreen = 0x1002, source_mouse = 0x2002;
constexpr bool isPadSource(int source) noexcept {
    return (source & source_gamepad) == source_gamepad || (source & source_joystick) == source_joystick;
}
inline std::optional<KeyCode> keyCode(int code) noexcept {
    if (code >= 29 && code <= 54) return static_cast<KeyCode>(int(KeyCode::A) + code - 29);
    if (code >= 7 && code <= 16) return static_cast<KeyCode>(int(KeyCode::Num0) + code - 7);
    if (code >= 131 && code <= 142) return static_cast<KeyCode>(int(KeyCode::F1) + code - 131);
    switch (code) {
    case 19: return KeyCode::ArrowUp;
    case 20: return KeyCode::ArrowDown;
    case 21: return KeyCode::ArrowLeft;
    case 22: return KeyCode::ArrowRight;
    case 62: return KeyCode::Space;
    case 66: return KeyCode::Enter;
    case 111: return KeyCode::Escape;
    case 61: return KeyCode::Tab;
    case 67: return KeyCode::Backspace;
    case 59: return KeyCode::LeftShift;
    case 60: return KeyCode::RightShift;
    case 113: return KeyCode::LeftControl;
    case 114: return KeyCode::RightControl;
    case 57: return KeyCode::LeftAlt;
    case 58: return KeyCode::RightAlt;
    case 117: return KeyCode::LeftSuper;
    case 118: return KeyCode::RightSuper;
    default: return std::nullopt;
    }
}
inline std::optional<GamepadButton> padButton(int code) noexcept {
    switch (code) {
    case 96: return GamepadButton::A; case 97: return GamepadButton::B;
    case 99: return GamepadButton::X; case 100: return GamepadButton::Y;
    case 102: return GamepadButton::LeftBumper; case 103: return GamepadButton::RightBumper;
    case 109: return GamepadButton::Back; case 108: return GamepadButton::Start;
    case 110: return GamepadButton::Guide; case 106: return GamepadButton::LeftThumb;
    case 107: return GamepadButton::RightThumb;
    case 19: return GamepadButton::DpadUp; case 22: return GamepadButton::DpadRight;
    case 20: return GamepadButton::DpadDown; case 21: return GamepadButton::DpadLeft;
    default: return std::nullopt;
    }
}
struct PadAxes { float x=0, y=0, z=0, rz=0, left_trigger=0, right_trigger=0, brake=0, gas=0, hat_x=0, hat_y=0; };
inline float finiteClamp(float value, float lo, float hi) noexcept {
    return std::isfinite(value) ? std::clamp(value, lo, hi) : lo;
}
inline float trigger(float primary, float fallback) noexcept {
    return 2.0f * std::max(finiteClamp(primary, 0, 1), finiteClamp(fallback, 0, 1)) - 1.0f;
}
inline GamepadState padState(const GamepadState &keys, const PadAxes &axes) noexcept {
    auto state = keys;
    state.axes = {finiteClamp(axes.x,-1,1), finiteClamp(axes.y,-1,1),
                  finiteClamp(axes.z,-1,1), finiteClamp(axes.rz,-1,1),
                  trigger(axes.left_trigger,axes.brake), trigger(axes.right_trigger,axes.gas)};
    const auto mark = [&](GamepadButton button, bool held) { state.buttons[std::size_t(button)] = static_cast<std::uint8_t>(state.buttons[std::size_t(button)] || held); };
    mark(GamepadButton::DpadUp, axes.hat_y < -0.5f); mark(GamepadButton::DpadDown, axes.hat_y > 0.5f);
    mark(GamepadButton::DpadLeft, axes.hat_x < -0.5f); mark(GamepadButton::DpadRight, axes.hat_x > 0.5f);
    return state;
}
// Android motion actions: POINTER_DOWN/UP applies only to encoded pointer index;
// MOVE and CANCEL apply to every pointer. Unknown actions remain unhandled.
inline std::optional<TouchPhase> touchPhase(int action, std::size_t pointer_index) noexcept {
    switch (action & 0xff) {
    case 0: return pointer_index == 0 ? std::optional{TouchPhase::down} : std::nullopt;
    case 1: return pointer_index == 0 ? std::optional{TouchPhase::up} : std::nullopt;
    case 5: return pointer_index == std::size_t((action >> 8) & 0xff) ? std::optional{TouchPhase::down} : std::nullopt;
    case 6: return pointer_index == std::size_t((action >> 8) & 0xff) ? std::optional{TouchPhase::up} : std::nullopt;
    case 2: return TouchPhase::move;
    case 3: return TouchPhase::cancel;
    default: return std::nullopt;
    }
}
class PadSlots {
    std::array<std::optional<int>, gamepad_slot_count> devices{};
  public:
    std::optional<std::size_t> assign(int id) noexcept {
        for (std::size_t i=0; i<devices.size(); ++i) if (devices[i] == id) return i;
        for (std::size_t i=0; i<devices.size(); ++i) if (!devices[i]) { devices[i]=id; return i; }
        return std::nullopt;
    }
};
} // namespace Pelican::Android
