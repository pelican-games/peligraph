#include "actionmap.hpp"
#include "inputstate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace {

constexpr std::string_view input_actions_schema = "pelican.input_actions";
constexpr int supported_input_actions_version = 1;
constexpr std::string_view input_profile_schema = "pelican.input_profile";
constexpr int supported_input_profile_version = 1;

enum class BindingKind {
    key,
    mouse_axis1,
    composite_axis2,
    gamepad_button,
    gamepad_axis1,
    gamepad_axis2,
    unresolved,
};

enum class CompositeKind {
    wasd,
    arrows,
};

enum class MouseAxis {
    delta_x,
    delta_y,
    wheel_x,
    wheel_y,
    count,
};

inline constexpr std::size_t mouse_axis_count = static_cast<std::size_t>(MouseAxis::count);

enum ModifierMask : std::uint8_t {
    modifier_none = 0,
    modifier_shift = 1 << 0,
    modifier_control = 1 << 1,
    modifier_alt = 1 << 2,
    modifier_super = 1 << 3,
};

struct ResolvedBinding {
    BindingKind kind = BindingKind::unresolved;
    std::string text;
    KeyCode key = KeyCode::Count;
    MouseAxis mouse_axis = MouseAxis::delta_x;
    CompositeKind composite = CompositeKind::wasd;
    GamepadButton gamepad_button = GamepadButton::Count;
    GamepadAxis gamepad_axis_x = GamepadAxis::Count;
    GamepadAxis gamepad_axis_y = GamepadAxis::Count;
    float deadzone = 0.0f;
    bool invert_x = false;
    bool invert_y = false;
    std::uint8_t modifiers = modifier_none;
};

struct ConsumedControls {
    std::array<std::uint8_t, key_code_count> keys{};
    std::array<std::uint8_t, mouse_axis_count> mouse_axes{};
    std::array<std::uint8_t, gamepad_button_count> gamepad_buttons{};
    std::array<std::uint8_t, gamepad_axis_count> gamepad_axes{};

    bool contains(KeyCode code) const noexcept {
        return isValidKeyCode(code) && keys[static_cast<std::size_t>(code)] != 0;
    }

    void mark(KeyCode code) noexcept {
        if (isValidKeyCode(code)) {
            keys[static_cast<std::size_t>(code)] = 1;
        }
    }

    void markMouseAxis(MouseAxis axis) noexcept {
        const auto index = static_cast<std::size_t>(axis);
        if (index < mouse_axes.size()) {
            mouse_axes[index] = 1;
        }
    }

    bool containsMouseAxis(MouseAxis axis) const noexcept {
        const auto index = static_cast<std::size_t>(axis);
        return index < mouse_axes.size() && mouse_axes[index] != 0;
    }

    void merge(const ConsumedControls &other) noexcept {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            keys[i] = static_cast<std::uint8_t>(keys[i] || other.keys[i]);
        }
        for (std::size_t i = 0; i < mouse_axes.size(); ++i) {
            mouse_axes[i] = static_cast<std::uint8_t>(mouse_axes[i] || other.mouse_axes[i]);
        }
        for (std::size_t i = 0; i < gamepad_buttons.size(); ++i) {
            gamepad_buttons[i] = static_cast<std::uint8_t>(gamepad_buttons[i] || other.gamepad_buttons[i]);
        }
        for (std::size_t i = 0; i < gamepad_axes.size(); ++i) {
            gamepad_axes[i] = static_cast<std::uint8_t>(gamepad_axes[i] || other.gamepad_axes[i]);
        }
    }
};

struct BindingSample {
    InputActionState state;
    ConsumedControls consumed;
    bool any_release = false;
};

struct ParsedChordControl {
    std::string_view primary;
    std::uint8_t modifiers = modifier_none;
};

struct MouseWheelDelta {
    float x = 0.0f;
    float y = 0.0f;
};

bool isIdentifier(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const char ch : value) {
        const bool is_digit = ch >= '0' && ch <= '9';
        const bool is_upper = ch >= 'A' && ch <= 'Z';
        const bool is_lower = ch >= 'a' && ch <= 'z';
        if (!is_digit && !is_upper && !is_lower && ch != '_' && ch != '.') {
            return false;
        }
    }
    return true;
}

void requireIdentifier(std::string_view value, std::string_view kind) {
    if (!isIdentifier(value)) {
        throw std::runtime_error(std::string{kind} +
                                 " must match [a-zA-Z0-9_.]: " +
                                 std::string{value});
    }
}

std::string actionContext(std::string_view action_name) {
    return " for action '" + std::string{action_name} + "'";
}

std::string requiredString(const nlohmann::json &object, const char *key, std::string_view context) {
    if (!object.contains(key) || !object.at(key).is_string()) {
        throw std::runtime_error("input action requires string '" + std::string{key} + "'" + std::string{context});
    }
    return object.at(key).get<std::string>();
}

InputActionType parseActionType(std::string_view type, std::string_view action_name) {
    if (type == "button") {
        return InputActionType::button;
    }
    if (type == "axis1") {
        return InputActionType::axis1;
    }
    if (type == "axis2") {
        return InputActionType::axis2;
    }
    if (type == "pose") {
        return InputActionType::pose;
    }
    throw std::runtime_error("unknown input action type '" + std::string{type} + "'" + actionContext(action_name));
}

std::optional<KeyCode> keyboardControlToKey(std::string_view control) {
    if (control.size() == 1) {
        const char ch = control[0];
        if (ch >= 'a' && ch <= 'z') {
            return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + (ch - 'a'));
        }
        if (ch >= '0' && ch <= '9') {
            return static_cast<KeyCode>(static_cast<int>(KeyCode::Num0) + (ch - '0'));
        }
    }

    if (control == "space") {
        return KeyCode::Space;
    }
    if (control == "enter") {
        return KeyCode::Enter;
    }
    if (control == "escape" || control == "esc") {
        return KeyCode::Escape;
    }
    if (control == "tab") {
        return KeyCode::Tab;
    }
    if (control == "backspace") {
        return KeyCode::Backspace;
    }
    if (control == "left_shift") {
        return KeyCode::LeftShift;
    }
    if (control == "right_shift") {
        return KeyCode::RightShift;
    }
    if (control == "left_control" || control == "left_ctrl") {
        return KeyCode::LeftControl;
    }
    if (control == "right_control" || control == "right_ctrl") {
        return KeyCode::RightControl;
    }
    if (control == "left_alt") {
        return KeyCode::LeftAlt;
    }
    if (control == "right_alt") {
        return KeyCode::RightAlt;
    }
    if (control == "left_super") {
        return KeyCode::LeftSuper;
    }
    if (control == "right_super") {
        return KeyCode::RightSuper;
    }
    if (control == "arrow_up" || control == "up") {
        return KeyCode::ArrowUp;
    }
    if (control == "arrow_down" || control == "down") {
        return KeyCode::ArrowDown;
    }
    if (control == "arrow_left" || control == "left") {
        return KeyCode::ArrowLeft;
    }
    if (control == "arrow_right" || control == "right") {
        return KeyCode::ArrowRight;
    }
    if (control.size() >= 2 && control[0] == 'f') {
        int value = 0;
        bool numeric = true;
        for (std::size_t i = 1; i < control.size(); ++i) {
            const char ch = control[i];
            if (ch < '0' || ch > '9') {
                numeric = false;
                break;
            }
            value = value * 10 + (ch - '0');
        }
        if (numeric && value >= 1 && value <= 12) {
            return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + (value - 1));
        }
    }
    if (control.size() == 4 && control.substr(0, 3) == "num") {
        const char ch = control[3];
        if (ch >= '0' && ch <= '9') {
            return static_cast<KeyCode>(static_cast<int>(KeyCode::Num0) + (ch - '0'));
        }
    }

    return std::nullopt;
}

std::optional<KeyCode> mouseControlToKey(std::string_view control) {
    if (control == "left") {
        return KeyCode::MouseLeft;
    }
    if (control == "right") {
        return KeyCode::MouseRight;
    }
    if (control == "middle") {
        return KeyCode::MouseMiddle;
    }
    if (control == "button4") {
        return KeyCode::MouseButton4;
    }
    if (control == "button5") {
        return KeyCode::MouseButton5;
    }
    if (control == "button6") {
        return KeyCode::MouseButton6;
    }
    if (control == "button7") {
        return KeyCode::MouseButton7;
    }
    if (control == "button8") {
        return KeyCode::MouseButton8;
    }
    return std::nullopt;
}

std::optional<std::uint8_t> modifierMaskFromName(std::string_view name) noexcept {
    if (name == "shift") {
        return modifier_shift;
    }
    if (name == "ctrl" || name == "control") {
        return modifier_control;
    }
    if (name == "alt") {
        return modifier_alt;
    }
    if (name == "super") {
        return modifier_super;
    }
    return std::nullopt;
}

ParsedChordControl parseChordControl(std::string_view control, InputActionType action_type,
                                     std::string_view binding_text, std::string_view action_name) {
    if (control.find('+') == std::string_view::npos) {
        return {.primary = control};
    }
    if (action_type != InputActionType::button) {
        throw std::runtime_error("input chord binding '" + std::string{binding_text} +
                                 "' requires a button action" + actionContext(action_name));
    }

    ParsedChordControl parsed;
    std::size_t begin = 0;
    while (true) {
        const auto separator = control.find('+', begin);
        const auto part = control.substr(begin, separator == std::string_view::npos
                                                    ? std::string_view::npos
                                                    : separator - begin);
        if (part.empty()) {
            throw std::runtime_error("invalid input chord binding '" + std::string{binding_text} + "'" +
                                     actionContext(action_name));
        }
        if (separator == std::string_view::npos) {
            parsed.primary = part;
            break;
        }

        const auto modifier = modifierMaskFromName(part);
        if (!modifier) {
            throw std::runtime_error("unknown input chord modifier '" + std::string{part} + "'" +
                                     actionContext(action_name));
        }
        if ((parsed.modifiers & *modifier) != 0) {
            throw std::runtime_error("duplicate input chord modifier '" + std::string{part} + "'" +
                                     actionContext(action_name));
        }
        parsed.modifiers = static_cast<std::uint8_t>(parsed.modifiers | *modifier);
        begin = separator + 1;
    }
    return parsed;
}

void requireNotPoseBinding(InputActionType type, std::string_view binding_text) {
    if (type == InputActionType::pose) {
        throw std::runtime_error("pose action binding '" + std::string{binding_text} +
                                 "' is reserved for OpenXR and cannot be resolved by kbd/mouse");
    }
}

ResolvedBinding parseBinding(std::string_view binding_text, InputActionType action_type,
                             std::string_view action_name) {
    const auto separator = binding_text.find(':');
    if (separator == std::string_view::npos || separator == 0 || separator == binding_text.size() - 1 ||
        separator != binding_text.rfind(':')) {
        throw std::runtime_error("invalid input action binding '" + std::string{binding_text} + "'" +
                                 actionContext(action_name));
    }

    const auto device = binding_text.substr(0, separator);
    const auto control = binding_text.substr(separator + 1);
    ResolvedBinding binding;
    binding.text = std::string{binding_text};

    if (device == "xr") {
        binding.kind = BindingKind::unresolved;
        return binding;
    }

    if (device == "pad") {
        requireNotPoseBinding(action_type, binding_text);
        if (action_type == InputActionType::button) {
            const auto button = gamepadButtonFromName(control);
            if (!button) {
                throw std::runtime_error("unknown gamepad button '" + std::string{control} + "'" +
                                         actionContext(action_name));
            }
            binding.kind = BindingKind::gamepad_button;
            binding.gamepad_button = *button;
            return binding;
        }
        if (action_type == InputActionType::axis1) {
            const auto axis = gamepadAxisFromName(control);
            if (!axis) {
                throw std::runtime_error("unknown gamepad axis '" + std::string{control} + "'" +
                                         actionContext(action_name));
            }
            binding.kind = BindingKind::gamepad_axis1;
            binding.gamepad_axis_x = *axis;
            return binding;
        }
        if (action_type == InputActionType::axis2) {
            if (control == "left_stick") {
                binding.gamepad_axis_x = GamepadAxis::LeftX;
                binding.gamepad_axis_y = GamepadAxis::LeftY;
            } else if (control == "right_stick") {
                binding.gamepad_axis_x = GamepadAxis::RightX;
                binding.gamepad_axis_y = GamepadAxis::RightY;
            } else {
                throw std::runtime_error("unknown gamepad axis2 control '" + std::string{control} + "'" +
                                         actionContext(action_name));
            }
            binding.kind = BindingKind::gamepad_axis2;
            return binding;
        }
    }

    if (device == "kbd") {
        requireNotPoseBinding(action_type, binding_text);
        const auto chord = parseChordControl(control, action_type, binding_text, action_name);
        if (chord.primary == "wasd" || chord.primary == "arrows") {
            if (action_type != InputActionType::axis2) {
                throw std::runtime_error("composite binding '" + std::string{binding_text} +
                                         "' requires an axis2 action" + actionContext(action_name));
            }
            binding.kind = BindingKind::composite_axis2;
            binding.composite = chord.primary == "wasd" ? CompositeKind::wasd : CompositeKind::arrows;
            return binding;
        }

        if (action_type == InputActionType::axis2) {
            throw std::runtime_error("axis2 action '" + std::string{action_name} +
                                     "' only supports kbd:wasd and kbd:arrows as built-in keyboard composites");
        }

        const auto key = keyboardControlToKey(chord.primary);
        if (!key) {
            throw std::runtime_error("unknown keyboard binding control '" + std::string{chord.primary} + "'" +
                                     actionContext(action_name));
        }
        binding.kind = BindingKind::key;
        binding.key = *key;
        binding.modifiers = chord.modifiers;
        return binding;
    }

    if (device == "mouse") {
        requireNotPoseBinding(action_type, binding_text);
        const auto chord = parseChordControl(control, action_type, binding_text, action_name);
        if (chord.primary == "delta_x" || chord.primary == "delta_y" ||
            chord.primary == "wheel_x" || chord.primary == "wheel_y") {
            if (action_type != InputActionType::axis1) {
                throw std::runtime_error("mouse axis binding '" + std::string{binding_text} +
                                         "' requires an axis1 action" + actionContext(action_name));
            }
            binding.kind = BindingKind::mouse_axis1;
            if (chord.primary == "delta_x") {
                binding.mouse_axis = MouseAxis::delta_x;
            } else if (chord.primary == "delta_y") {
                binding.mouse_axis = MouseAxis::delta_y;
            } else if (chord.primary == "wheel_x") {
                binding.mouse_axis = MouseAxis::wheel_x;
            } else {
                binding.mouse_axis = MouseAxis::wheel_y;
            }
            return binding;
        }
        if (action_type == InputActionType::axis2) {
            throw std::runtime_error("axis2 action '" + std::string{action_name} +
                                     "' does not support mouse button binding '" + std::string{binding_text} + "'");
        }

        const auto key = mouseControlToKey(chord.primary);
        if (!key) {
            throw std::runtime_error("unknown mouse binding control '" + std::string{chord.primary} + "'" +
                                     actionContext(action_name));
        }
        binding.kind = BindingKind::key;
        binding.key = *key;
        binding.modifiers = chord.modifiers;
        return binding;
    }

    throw std::runtime_error("unknown input binding device '" + std::string{device} + "'" +
                             actionContext(action_name));
}

float clampAxis(float value) noexcept {
    return std::clamp(value, -1.0f, 1.0f);
}

void mergeSample(InputActionState &state, bool &any_release, const BindingSample &sample) noexcept {
    state.pressed = state.pressed || sample.state.pressed;
    state.held = state.held || sample.state.held;
    state.axis1 = clampAxis(state.axis1 + sample.state.axis1);
    state.axis2.x = clampAxis(state.axis2.x + sample.state.axis2.x);
    state.axis2.y = clampAxis(state.axis2.y + sample.state.axis2.y);
    any_release = any_release || sample.any_release;
}

struct ModifierKeyPair {
    std::uint8_t mask;
    KeyCode left;
    KeyCode right;
};

constexpr std::array<ModifierKeyPair, 4> modifier_key_pairs{{
    {modifier_shift, KeyCode::LeftShift, KeyCode::RightShift},
    {modifier_control, KeyCode::LeftControl, KeyCode::RightControl},
    {modifier_alt, KeyCode::LeftAlt, KeyCode::RightAlt},
    {modifier_super, KeyCode::LeftSuper, KeyCode::RightSuper},
}};

bool keyWasDown(KeyCode key, const InputSnapshot &snapshot) noexcept {
    return (snapshot.getKey(key) && !snapshot.isKeyPushed(key)) || snapshot.isKeyReleased(key);
}

bool availableKeyState(KeyCode key, const InputSnapshot &snapshot,
                       const ConsumedControls &already_consumed, bool previous) noexcept {
    if (already_consumed.contains(key)) {
        return false;
    }
    return previous ? keyWasDown(key, snapshot) : snapshot.getKey(key);
}

bool modifiersMatch(std::uint8_t required, const InputSnapshot &snapshot,
                    const ConsumedControls &already_consumed, bool previous) noexcept {
    for (const auto &pair : modifier_key_pairs) {
        if ((required & pair.mask) == 0) {
            continue;
        }
        if (!availableKeyState(pair.left, snapshot, already_consumed, previous) &&
            !availableKeyState(pair.right, snapshot, already_consumed, previous)) {
            return false;
        }
    }
    return true;
}

void markChordModifiers(ConsumedControls &consumed, std::uint8_t required,
                        const InputSnapshot &snapshot) noexcept {
    for (const auto &pair : modifier_key_pairs) {
        if ((required & pair.mask) == 0) {
            continue;
        }
        for (const auto key : {pair.left, pair.right}) {
            if (snapshot.getKey(key) || snapshot.isKeyPushed(key) || snapshot.isKeyReleased(key)) {
                consumed.mark(key);
            }
        }
    }
}

BindingSample readKeyBinding(const ResolvedBinding &binding, const InputSnapshot &snapshot,
                             const ConsumedControls &already_consumed, InputActionType action_type) {
    BindingSample sample;
    if (already_consumed.contains(binding.key)) {
        return sample;
    }

    if (binding.modifiers == modifier_none) {
        const bool pressed = snapshot.isKeyPushed(binding.key);
        const bool released = snapshot.isKeyReleased(binding.key);
        const bool held = snapshot.getKey(binding.key);
        if (pressed || released || held) {
            sample.consumed.mark(binding.key);
        }

        sample.state.pressed = pressed;
        sample.state.held = held;
        sample.any_release = released;
        if (action_type == InputActionType::axis1) {
            sample.state.axis1 = held ? 1.0f : 0.0f;
        }
        return sample;
    }

    const bool held = snapshot.getKey(binding.key) &&
                      modifiersMatch(binding.modifiers, snapshot, already_consumed, false);
    const bool was_held = keyWasDown(binding.key, snapshot) &&
                          modifiersMatch(binding.modifiers, snapshot, already_consumed, true);
    sample.state.pressed = held && !was_held;
    sample.state.held = held;
    sample.any_release = was_held && !held;
    if (sample.state.pressed || sample.state.held || sample.any_release) {
        sample.consumed.mark(binding.key);
        markChordModifiers(sample.consumed, binding.modifiers, snapshot);
    }
    return sample;
}

std::array<KeyCode, 4> compositeKeys(CompositeKind composite) noexcept {
    if (composite == CompositeKind::wasd) {
        return {KeyCode::A, KeyCode::D, KeyCode::S, KeyCode::W};
    }
    return {KeyCode::ArrowLeft, KeyCode::ArrowRight, KeyCode::ArrowDown, KeyCode::ArrowUp};
}

BindingSample readCompositeBinding(CompositeKind composite, const InputSnapshot &snapshot,
                                   const ConsumedControls &already_consumed) {
    BindingSample sample;
    const auto keys = compositeKeys(composite);
    const auto read_key = [&](KeyCode key) {
        if (already_consumed.contains(key)) {
            return std::array<bool, 3>{false, false, false};
        }
        const bool pressed = snapshot.isKeyPushed(key);
        const bool released = snapshot.isKeyReleased(key);
        const bool held = snapshot.getKey(key);
        if (pressed || released || held) {
            sample.consumed.mark(key);
        }
        return std::array<bool, 3>{pressed, released, held};
    };

    const auto left = read_key(keys[0]);
    const auto right = read_key(keys[1]);
    const auto down = read_key(keys[2]);
    const auto up = read_key(keys[3]);

    sample.state.axis2.x = (right[2] ? 1.0f : 0.0f) - (left[2] ? 1.0f : 0.0f);
    sample.state.axis2.y = (up[2] ? 1.0f : 0.0f) - (down[2] ? 1.0f : 0.0f);
    sample.state.held = left[2] || right[2] || down[2] || up[2];
    sample.state.pressed = left[0] || right[0] || down[0] || up[0];
    sample.any_release = left[1] || right[1] || down[1] || up[1];
    return sample;
}

BindingSample readMouseAxisBinding(MouseAxis axis, const InputSnapshot &snapshot,
                                   const MouseWheelDelta &wheel,
                                   const ConsumedControls &already_consumed) {
    BindingSample sample;
    if (already_consumed.containsMouseAxis(axis)) {
        return sample;
    }
    float value = 0.0f;
    switch (axis) {
    case MouseAxis::delta_x:
        value = snapshot.mouse_delta_x;
        break;
    case MouseAxis::delta_y:
        value = snapshot.mouse_delta_y;
        break;
    case MouseAxis::wheel_x:
        value = wheel.x;
        break;
    case MouseAxis::wheel_y:
        value = wheel.y;
        break;
    case MouseAxis::count:
        break;
    }
    if (value != 0.0f) {
        sample.consumed.markMouseAxis(axis);
        sample.state.axis1 = value;
        sample.state.pressed = true;
        sample.state.held = true;
    }
    return sample;
}

float applyDeadzone(float value, float deadzone) noexcept {
    const auto magnitude = std::abs(value);
    if (magnitude <= deadzone) {
        return 0.0f;
    }
    return std::copysign((magnitude - deadzone) / (1.0f - deadzone), value);
}

BindingSample readGamepadButtonBinding(const ResolvedBinding &binding, const InputSnapshot &snapshot,
                                       const ConsumedControls &already_consumed) {
    BindingSample sample;
    const auto index = static_cast<std::size_t>(binding.gamepad_button);
    if (index >= gamepad_button_count || already_consumed.gamepad_buttons[index] != 0) {
        return sample;
    }
    for (std::size_t pad = 0; pad < gamepad_slot_count; ++pad) {
        sample.state.pressed = sample.state.pressed || snapshot.isGamepadButtonPushed(pad, binding.gamepad_button);
        sample.state.held = sample.state.held || snapshot.getGamepadButton(pad, binding.gamepad_button);
        sample.any_release = sample.any_release || snapshot.isGamepadButtonReleased(pad, binding.gamepad_button);
    }
    if (sample.state.pressed || sample.state.held || sample.any_release) {
        sample.consumed.gamepad_buttons[index] = 1;
    }
    return sample;
}

float strongestGamepadAxis(const InputSnapshot &snapshot, GamepadAxis axis) noexcept {
    float result = 0.0f;
    for (std::size_t pad = 0; pad < gamepad_slot_count; ++pad) {
        const auto candidate = snapshot.getGamepadAxis(pad, axis);
        if (std::abs(candidate) > std::abs(result)) {
            result = candidate;
        }
    }
    return result;
}

BindingSample readGamepadAxisBinding(const ResolvedBinding &binding, const InputSnapshot &snapshot,
                                     const ConsumedControls &already_consumed, bool axis2) {
    BindingSample sample;
    const auto x_index = static_cast<std::size_t>(binding.gamepad_axis_x);
    const auto y_index = static_cast<std::size_t>(binding.gamepad_axis_y);
    if (x_index >= gamepad_axis_count || already_consumed.gamepad_axes[x_index] != 0 ||
        (axis2 && (y_index >= gamepad_axis_count || already_consumed.gamepad_axes[y_index] != 0))) {
        return sample;
    }
    float x = strongestGamepadAxis(snapshot, binding.gamepad_axis_x);
    float y = axis2 ? strongestGamepadAxis(snapshot, binding.gamepad_axis_y) : 0.0f;
    if (axis2) {
        const float magnitude = std::sqrt(x * x + y * y);
        if (magnitude <= binding.deadzone) {
            x = 0.0f;
            y = 0.0f;
        } else if (magnitude > 0.0f) {
            const float scaled = std::min(1.0f, (magnitude - binding.deadzone) / (1.0f - binding.deadzone));
            x = x / magnitude * scaled;
            y = y / magnitude * scaled;
        }
    } else {
        x = applyDeadzone(x, binding.deadzone);
    }
    if (binding.invert_x) {
        x = -x;
    }
    if (binding.invert_y) {
        y = -y;
    }
    if (x != 0.0f || y != 0.0f) {
        sample.consumed.gamepad_axes[x_index] = 1;
        if (axis2) {
            sample.consumed.gamepad_axes[y_index] = 1;
        }
        sample.state.pressed = true;
        sample.state.held = true;
    }
    if (axis2) {
        sample.state.axis2 = {x, y};
    } else {
        sample.state.axis1 = x;
    }
    return sample;
}

BindingSample readBinding(const ResolvedBinding &binding, const InputSnapshot &snapshot,
                          const MouseWheelDelta &wheel,
                          const ConsumedControls &already_consumed, InputActionType action_type) {
    switch (binding.kind) {
    case BindingKind::key:
        return readKeyBinding(binding, snapshot, already_consumed, action_type);
    case BindingKind::mouse_axis1:
        return readMouseAxisBinding(binding.mouse_axis, snapshot, wheel, already_consumed);
    case BindingKind::composite_axis2:
        return readCompositeBinding(binding.composite, snapshot, already_consumed);
    case BindingKind::gamepad_button:
        return readGamepadButtonBinding(binding, snapshot, already_consumed);
    case BindingKind::gamepad_axis1:
        return readGamepadAxisBinding(binding, snapshot, already_consumed, false);
    case BindingKind::gamepad_axis2:
        return readGamepadAxisBinding(binding, snapshot, already_consumed, true);
    case BindingKind::unresolved:
        return {};
    }
    return {};
}

void validateEnvelope(const nlohmann::json &document) {
    if (!document.is_object()) {
        throw std::runtime_error("input actions document must be an object");
    }
    if (document.value("schema", std::string{}) != input_actions_schema) {
        throw std::runtime_error("input actions schema is not supported");
    }
    if (!document.contains("version") || !document.at("version").is_number_integer()) {
        throw std::runtime_error("input actions requires numeric version");
    }
    const auto version = document.at("version").get<int>();
    if (version != supported_input_actions_version) {
        throw std::runtime_error("input actions version is not supported");
    }
    if (!document.contains("action_sets") || !document.at("action_sets").is_array()) {
        throw std::runtime_error("input actions requires action_sets array");
    }
}

} // namespace

void InputActionMap::rebuildLookup() {
    action_lookup.clear();
    set_lookup.clear();

    for (std::size_t set_index = 0; set_index < action_sets.size(); ++set_index) {
        const auto &set = action_sets[set_index];
        set_lookup.emplace(set.name, set_index);
        for (std::size_t action_index = 0; action_index < set.actions.size(); ++action_index) {
            action_lookup.emplace(set.actions[action_index].name, ActionLocation{set_index, action_index});
        }
    }
}

const std::vector<InputActionSet> &InputActionMap::actionSets() const noexcept {
    return action_sets;
}

const InputActionSet *InputActionMap::findActionSet(std::string_view name) const {
    const auto it = set_lookup.find(std::string{name});
    if (it == set_lookup.end()) {
        return nullptr;
    }
    return &action_sets[it->second];
}

const InputActionDefinition *InputActionMap::findAction(std::string_view name) const {
    const auto it = action_lookup.find(std::string{name});
    if (it == action_lookup.end()) {
        return nullptr;
    }
    const auto location = it->second;
    return &action_sets[location.set_index].actions[location.action_index];
}

bool InputActionMap::hasActionSet(std::string_view name) const {
    return set_lookup.find(std::string{name}) != set_lookup.end();
}

bool InputActionMap::usesGamepad() const noexcept {
    for (const auto &set : action_sets) {
        for (const auto &action : set.actions) {
            for (const auto &binding : action.bindings) {
                if (binding.text.starts_with("pad:")) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::vector<std::string> InputActionMap::poseActionNames() const {
    std::vector<std::string> result;
    for (const auto &set : action_sets) {
        for (const auto &action : set.actions) {
            if (action.type == InputActionType::pose) result.push_back(action.name);
        }
    }
    return result;
}

std::size_t InputActionMap::actionCount() const noexcept {
    return action_lookup.size();
}

const InputActionState *InputActionFrame::find(std::string_view action_name) const {
    const auto it = actions.find(std::string{action_name});
    if (it == actions.end()) {
        return nullptr;
    }
    return &it->second;
}

InputActionState InputActionFrame::get(std::string_view action_name) const {
    const auto *state = find(action_name);
    if (state == nullptr) {
        throw std::runtime_error("unknown input action: " + std::string{action_name});
    }
    return *state;
}

ActionPose InputActionFrame::pose(std::string_view action_name) const {
    const auto type = action_types.find(std::string{action_name});
    if (type == action_types.end()) {
        throw std::runtime_error("unknown input action: " + std::string{action_name});
    }
    if (type->second != InputActionType::pose) {
        throw std::runtime_error("input action is not a pose action: " + std::string{action_name});
    }
    const auto found = poses.find(std::string{action_name});
    return found == poses.end() ? ActionPose{} : found->second;
}

InputActionMap parseInputActionsJson(const nlohmann::json &document) {
    validateEnvelope(document);

    InputActionMap map;
    std::unordered_set<std::string> action_names;
    std::unordered_set<std::string> action_set_names;

    for (const auto &set_json : document.at("action_sets")) {
        if (!set_json.is_object()) {
            throw std::runtime_error("input action set entries must be objects");
        }
        auto set_name = requiredString(set_json, "name", " for action set");
        requireIdentifier(set_name, "input action set name");
        if (!action_set_names.insert(set_name).second) {
            throw std::runtime_error("duplicate input action set name: " + set_name);
        }
        if (!set_json.contains("actions") || !set_json.at("actions").is_array()) {
            throw std::runtime_error("input action set '" + set_name + "' requires actions array");
        }

        InputActionSet set;
        set.name = set_name;
        for (const auto &action_json : set_json.at("actions")) {
            if (!action_json.is_object()) {
                throw std::runtime_error("input action entries must be objects in set '" + set_name + "'");
            }

            InputActionDefinition action;
            action.set_name = set_name;
            action.name = requiredString(action_json, "name", " in set '" + set_name + "'");
            requireIdentifier(action.name, "input action name");
            if (!action_names.insert(action.name).second) {
                throw std::runtime_error("duplicate input action name: " + action.name);
            }
            action.type = parseActionType(requiredString(action_json, "type", actionContext(action.name)),
                                          action.name);

            if (action_json.contains("bindings")) {
                throw std::runtime_error("input action '" + action.name +
                                         "' must define bindings in a pelican.input_profile");
            }
            set.actions.push_back(std::move(action));
        }
        map.action_sets.push_back(std::move(set));
    }

    map.rebuildLookup();
    return map;
}

InputActionMap parseInputActionsString(std::string_view document) {
    return parseInputActionsJson(nlohmann::json::parse(document));
}

InputBindingProfile parseInputProfileJson(const nlohmann::json &document, const InputActionMap &actions) {
    if (!document.is_object() || document.value("schema", std::string{}) != input_profile_schema) {
        throw std::runtime_error("input profile schema is not supported");
    }
    if (document.value("version", 0) != supported_input_profile_version) {
        throw std::runtime_error("input profile version is not supported");
    }
    InputBindingProfile profile;
    profile.name = requiredString(document, "name", " for input profile");
    requireIdentifier(profile.name, "input profile name");
    if (!document.contains("bindings") || !document.at("bindings").is_array()) {
        throw std::runtime_error("input profile '" + profile.name + "' requires bindings array");
    }
    for (const auto &entry : document.at("bindings")) {
        if (!entry.is_object()) {
            throw std::runtime_error("input profile '" + profile.name + "' binding entries must be objects");
        }
        InputProfileBinding parsed;
        parsed.action = requiredString(entry, "action", " in input profile '" + profile.name + "'");
        const auto *action = actions.findAction(parsed.action);
        if (action == nullptr) {
            throw std::runtime_error("input profile '" + profile.name + "' references unknown action '" +
                                     parsed.action + "'");
        }
        parsed.binding.text = requiredString(entry, "binding", " for action '" + parsed.action + "'");
        auto resolved = parseBinding(parsed.binding.text, action->type, parsed.action);
        if (entry.contains("deadzone")) {
            if (!entry.at("deadzone").is_number()) {
                throw std::runtime_error("input profile deadzone must be numeric for action '" + parsed.action + "'");
            }
            parsed.binding.deadzone = entry.at("deadzone").get<float>();
            if (!std::isfinite(parsed.binding.deadzone) || parsed.binding.deadzone < 0.0f ||
                parsed.binding.deadzone >= 1.0f) {
                throw std::runtime_error("input profile deadzone must be in [0,1) for action '" + parsed.action + "'");
            }
        }
        const auto read_invert = [&](const char *field) {
            if (!entry.contains(field)) {
                return false;
            }
            if (!entry.at(field).is_boolean()) {
                throw std::runtime_error("input profile " + std::string{field} +
                                         " must be boolean for action '" + parsed.action + "'");
            }
            return entry.at(field).get<bool>();
        };
        parsed.binding.invert_x = read_invert("invert_x");
        parsed.binding.invert_y = read_invert("invert_y");
        const bool is_pad_axis = resolved.kind == BindingKind::gamepad_axis1 ||
                                 resolved.kind == BindingKind::gamepad_axis2;
        if ((parsed.binding.deadzone != 0.0f || parsed.binding.invert_x || parsed.binding.invert_y) &&
            !is_pad_axis) {
            throw std::runtime_error("input profile deadzone/invert attributes require a gamepad axis binding for action '" +
                                     parsed.action + "'");
        }
        if (parsed.binding.invert_y && resolved.kind != BindingKind::gamepad_axis2) {
            throw std::runtime_error("input profile invert_y requires a gamepad axis2 binding for action '" +
                                     parsed.action + "'");
        }
        profile.uses_gamepad = profile.uses_gamepad || parsed.binding.text.starts_with("pad:");
        profile.bindings.push_back(std::move(parsed));
    }
    return profile;
}

InputBindingProfile parseInputProfileString(std::string_view document, const InputActionMap &actions) {
    return parseInputProfileJson(nlohmann::json::parse(document), actions);
}

InputActionMap applyInputProfile(InputActionMap map, const InputBindingProfile &profile) {
    for (auto &set : map.action_sets) {
        for (auto &action : set.actions) {
            action.bindings.clear();
        }
    }
    for (const auto &entry : profile.bindings) {
        const auto location = map.action_lookup.find(entry.action);
        if (location == map.action_lookup.end()) {
            throw std::runtime_error("input profile '" + profile.name + "' references unknown action '" +
                                     entry.action + "'");
        }
        const auto where = location->second;
        map.action_sets[where.set_index].actions[where.action_index].bindings.push_back(entry.binding);
    }
    return map;
}

namespace {

InputActionFrame evaluateInputActionsImpl(const InputActionMap &map, const InputSnapshot &snapshot,
                                          const MouseWheelDelta &wheel,
                                          const std::vector<std::string> &action_set_stack) {
    InputActionFrame frame;
    for (const auto &set : map.actionSets()) {
        for (const auto &action : set.actions) {
            frame.action_types.emplace(action.name, action.type);
            frame.actions.emplace(action.name, InputActionState{});
            if (action.type == InputActionType::pose) frame.poses.emplace(action.name, ActionPose{});
        }
    }

    ConsumedControls consumed;
    for (auto stack_it = action_set_stack.rbegin(); stack_it != action_set_stack.rend(); ++stack_it) {
        const auto *set = map.findActionSet(*stack_it);
        if (set == nullptr) {
            throw std::runtime_error("unknown input action set on stack: " + *stack_it);
        }

        ConsumedControls consumed_by_set;
        for (const auto &action : set->actions) {
            if (action.type == InputActionType::pose) {
                continue;
            }

            InputActionState action_state;
            bool any_release = false;
            ConsumedControls consumed_by_action;
            for (const auto &binding : action.bindings) {
                auto resolved = parseBinding(binding.text, action.type, action.name);
                resolved.deadzone = binding.deadzone;
                resolved.invert_x = binding.invert_x;
                resolved.invert_y = binding.invert_y;
                const auto sample = readBinding(resolved, snapshot, wheel, consumed, action.type);
                mergeSample(action_state, any_release, sample);
                consumed_by_action.merge(sample.consumed);
            }

            action_state.released = any_release && !action_state.held;
            frame.actions[action.name] = action_state;
            consumed_by_set.merge(consumed_by_action);
        }
        consumed.merge(consumed_by_set);
    }

    return frame;
}

MouseWheelDelta collectMouseWheelDelta(const FrameInput &frame_input) noexcept {
    MouseWheelDelta result;
    for (const auto &event : frame_input.ordered_events) {
        if (event.type != InputEvent::Type::scroll) {
            continue;
        }
        if (std::isfinite(event.axis_x)) {
            result.x += event.axis_x;
        }
        if (std::isfinite(event.axis_y)) {
            result.y += event.axis_y;
        }
    }
    return result;
}

} // namespace

InputActionFrame evaluateInputActions(const InputActionMap &map, const InputSnapshot &snapshot,
                                      const std::vector<std::string> &action_set_stack) {
    return evaluateInputActionsImpl(map, snapshot, {}, action_set_stack);
}

InputActionFrame evaluateInputActions(const InputActionMap &map, const FrameInput &frame_input,
                                      const std::vector<std::string> &action_set_stack) {
    auto frame = evaluateInputActionsImpl(map, frame_input.snapshot, collectMouseWheelDelta(frame_input),
                                          action_set_stack);
    std::unordered_set<std::string> active_pose_actions;
    for (const auto &set_name : action_set_stack) {
        const auto *set = map.findActionSet(set_name);
        if (set == nullptr) {
            throw std::runtime_error("unknown input action set on stack: " + set_name);
        }
        for (const auto &action : set->actions) {
            if (action.type == InputActionType::pose) active_pose_actions.insert(action.name);
        }
    }
    for (const auto &sample : frame_input.pose_samples) {
        if (active_pose_actions.contains(sample.action_name)) {
            frame.poses.at(sample.action_name) = sample.pose;
        }
    }
    return frame;
}

void mergeInputActionBackendFrame(InputActionFrame &destination,
                                  const InputActionFrame &backend_frame) {
    for (const auto &[action_name, source] : backend_frame.actions) {
        const auto destination_type = destination.action_types.find(action_name);
        const auto source_type = backend_frame.action_types.find(action_name);
        if (destination_type == destination.action_types.end() ||
            source_type == backend_frame.action_types.end()) {
            continue;
        }
        if (destination_type->second != source_type->second) {
            throw std::runtime_error("input backend type mismatch for action: " + action_name);
        }
        auto &target = destination.actions.at(action_name);
        const bool any_release = target.released || source.released;
        target.pressed = target.pressed || source.pressed;
        target.held = target.held || source.held;
        target.released = any_release && !target.held;
        target.axis1 = clampAxis(target.axis1 + source.axis1);
        target.axis2.x = clampAxis(target.axis2.x + source.axis2.x);
        target.axis2.y = clampAxis(target.axis2.y + source.axis2.y);
    }
}

} // namespace Pelican
