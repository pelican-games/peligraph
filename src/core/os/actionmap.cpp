#include "actionmap.hpp"
#include "inputstate.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace {

constexpr std::string_view input_actions_schema = "pelican.input_actions";
constexpr int supported_input_actions_version = 1;

enum class BindingKind {
    key,
    mouse_axis1,
    composite_axis2,
    unresolved,
};

enum class CompositeKind {
    wasd,
    arrows,
};

enum class MouseAxis {
    delta_x,
    delta_y,
};

struct ResolvedBinding {
    BindingKind kind = BindingKind::unresolved;
    std::string text;
    KeyCode key = KeyCode::Count;
    MouseAxis mouse_axis = MouseAxis::delta_x;
    CompositeKind composite = CompositeKind::wasd;
};

struct ConsumedControls {
    std::array<std::uint8_t, key_code_count> keys{};
    bool mouse_delta_x = false;
    bool mouse_delta_y = false;

    bool contains(KeyCode code) const noexcept {
        return isValidKeyCode(code) && keys[static_cast<std::size_t>(code)] != 0;
    }

    void mark(KeyCode code) noexcept {
        if (isValidKeyCode(code)) {
            keys[static_cast<std::size_t>(code)] = 1;
        }
    }

    void markMouseAxis(MouseAxis axis) noexcept {
        if (axis == MouseAxis::delta_x) {
            mouse_delta_x = true;
        } else {
            mouse_delta_y = true;
        }
    }

    bool containsMouseAxis(MouseAxis axis) const noexcept {
        return axis == MouseAxis::delta_x ? mouse_delta_x : mouse_delta_y;
    }

    void merge(const ConsumedControls &other) noexcept {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            keys[i] = static_cast<std::uint8_t>(keys[i] || other.keys[i]);
        }
        mouse_delta_x = mouse_delta_x || other.mouse_delta_x;
        mouse_delta_y = mouse_delta_y || other.mouse_delta_y;
    }
};

struct BindingSample {
    InputActionState state;
    ConsumedControls consumed;
    bool any_release = false;
};

bool isIdentifier(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const char ch : value) {
        const bool is_digit = ch >= '0' && ch <= '9';
        const bool is_upper = ch >= 'A' && ch <= 'Z';
        const bool is_lower = ch >= 'a' && ch <= 'z';
        if (!is_digit && !is_upper && !is_lower && ch != '_') {
            return false;
        }
    }
    return true;
}

void requireIdentifier(std::string_view value, std::string_view kind) {
    if (!isIdentifier(value)) {
        throw std::runtime_error(std::string{kind} + " must match [a-zA-Z0-9_]: " + std::string{value});
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

    if (device == "pad" || device == "xr") {
        binding.kind = BindingKind::unresolved;
        return binding;
    }

    if (device == "kbd") {
        requireNotPoseBinding(action_type, binding_text);
        if (control == "wasd" || control == "arrows") {
            if (action_type != InputActionType::axis2) {
                throw std::runtime_error("composite binding '" + std::string{binding_text} +
                                         "' requires an axis2 action" + actionContext(action_name));
            }
            binding.kind = BindingKind::composite_axis2;
            binding.composite = control == "wasd" ? CompositeKind::wasd : CompositeKind::arrows;
            return binding;
        }

        if (action_type == InputActionType::axis2) {
            throw std::runtime_error("axis2 action '" + std::string{action_name} +
                                     "' only supports kbd:wasd and kbd:arrows as built-in keyboard composites");
        }

        const auto key = keyboardControlToKey(control);
        if (!key) {
            throw std::runtime_error("unknown keyboard binding control '" + std::string{control} + "'" +
                                     actionContext(action_name));
        }
        binding.kind = BindingKind::key;
        binding.key = *key;
        return binding;
    }

    if (device == "mouse") {
        requireNotPoseBinding(action_type, binding_text);
        if (control == "delta_x" || control == "delta_y") {
            if (action_type != InputActionType::axis1) {
                throw std::runtime_error("mouse delta binding '" + std::string{binding_text} +
                                         "' requires an axis1 action" + actionContext(action_name));
            }
            binding.kind = BindingKind::mouse_axis1;
            binding.mouse_axis = control == "delta_x" ? MouseAxis::delta_x : MouseAxis::delta_y;
            return binding;
        }
        if (action_type == InputActionType::axis2) {
            throw std::runtime_error("axis2 action '" + std::string{action_name} +
                                     "' does not support mouse button binding '" + std::string{binding_text} + "'");
        }

        const auto key = mouseControlToKey(control);
        if (!key) {
            throw std::runtime_error("unknown mouse binding control '" + std::string{control} + "'" +
                                     actionContext(action_name));
        }
        binding.kind = BindingKind::key;
        binding.key = *key;
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

BindingSample readKeyBinding(KeyCode key, const InputSnapshot &snapshot, const ConsumedControls &already_consumed,
                             InputActionType action_type) {
    BindingSample sample;
    if (already_consumed.contains(key)) {
        return sample;
    }

    const bool pressed = snapshot.isKeyPushed(key);
    const bool released = snapshot.isKeyReleased(key);
    const bool held = snapshot.getKey(key);
    if (pressed || released || held) {
        sample.consumed.mark(key);
    }

    sample.state.pressed = pressed;
    sample.state.held = held;
    sample.any_release = released;
    if (action_type == InputActionType::axis1) {
        sample.state.axis1 = held ? 1.0f : 0.0f;
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
                                   const ConsumedControls &already_consumed) {
    BindingSample sample;
    if (already_consumed.containsMouseAxis(axis)) {
        return sample;
    }
    const auto value = axis == MouseAxis::delta_x ? snapshot.mouse_delta_x : snapshot.mouse_delta_y;
    if (value != 0.0f) {
        sample.consumed.markMouseAxis(axis);
        sample.state.axis1 = value;
        sample.state.pressed = true;
        sample.state.held = true;
    }
    return sample;
}

BindingSample readBinding(const ResolvedBinding &binding, const InputSnapshot &snapshot,
                          const ConsumedControls &already_consumed, InputActionType action_type) {
    switch (binding.kind) {
    case BindingKind::key:
        return readKeyBinding(binding.key, snapshot, already_consumed, action_type);
    case BindingKind::mouse_axis1:
        return readMouseAxisBinding(binding.mouse_axis, snapshot, already_consumed);
    case BindingKind::composite_axis2:
        return readCompositeBinding(binding.composite, snapshot, already_consumed);
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
    throw std::runtime_error("OpenXR pose binding resolution is not implemented for input action '" +
                             std::string{action_name} + "'");
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

            if (!action_json.contains("bindings") || !action_json.at("bindings").is_array()) {
                throw std::runtime_error("input action '" + action.name + "' requires bindings array");
            }
            for (const auto &binding_json : action_json.at("bindings")) {
                if (!binding_json.is_string()) {
                    throw std::runtime_error("input action binding must be a string" + actionContext(action.name));
                }
                const auto binding_text = binding_json.get<std::string>();
                (void)parseBinding(binding_text, action.type, action.name);
                action.bindings.push_back(InputActionBinding{binding_text});
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

InputActionFrame evaluateInputActions(const InputActionMap &map, const InputSnapshot &snapshot,
                                      const std::vector<std::string> &action_set_stack) {
    InputActionFrame frame;
    for (const auto &set : map.actionSets()) {
        for (const auto &action : set.actions) {
            frame.action_types.emplace(action.name, action.type);
            frame.actions.emplace(action.name, InputActionState{});
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
                const auto resolved = parseBinding(binding.text, action.type, action.name);
                const auto sample = readBinding(resolved, snapshot, consumed, action.type);
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

} // namespace Pelican
