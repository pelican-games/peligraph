#pragma once

#include "../userpublic/userinput.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Pelican {

struct InputSnapshot;

enum class InputActionType : std::uint8_t {
    button,
    axis1,
    axis2,
    pose,
};

struct InputActionState {
    bool pressed = false;
    bool released = false;
    bool held = false;
    float axis1 = 0.0f;
    ActionAxis2 axis2{};
};

struct InputActionBinding {
    std::string text;
    float deadzone = 0.0f;
    bool invert_x = false;
    bool invert_y = false;
};

struct InputProfileBinding {
    std::string action;
    InputActionBinding binding;
};

struct InputBindingProfile {
    std::string name;
    std::vector<InputProfileBinding> bindings;
    bool uses_gamepad = false;
};

struct InputActionDefinition {
    std::string set_name;
    std::string name;
    InputActionType type = InputActionType::button;
    std::vector<InputActionBinding> bindings;
};

struct InputActionSet {
    std::string name;
    std::vector<InputActionDefinition> actions;
};

class InputActionMap {
    struct ActionLocation {
        std::size_t set_index = 0;
        std::size_t action_index = 0;
    };

    std::vector<InputActionSet> action_sets;
    std::unordered_map<std::string, ActionLocation> action_lookup;
    std::unordered_map<std::string, std::size_t> set_lookup;

    void rebuildLookup();

  public:
    const std::vector<InputActionSet> &actionSets() const noexcept;
    const InputActionSet *findActionSet(std::string_view name) const;
    const InputActionDefinition *findAction(std::string_view name) const;
    bool hasActionSet(std::string_view name) const;
    std::size_t actionCount() const noexcept;
    bool usesGamepad() const noexcept;

    friend InputActionMap parseInputActionsJson(const nlohmann::json &document);
    friend InputActionMap applyInputProfile(InputActionMap map, const InputBindingProfile &profile);
};

struct InputActionFrame {
    std::unordered_map<std::string, InputActionState> actions;
    std::unordered_map<std::string, InputActionType> action_types;

    const InputActionState *find(std::string_view action_name) const;
    InputActionState get(std::string_view action_name) const;
    ActionPose pose(std::string_view action_name) const;
};

InputActionMap parseInputActionsJson(const nlohmann::json &document);
InputActionMap parseInputActionsString(std::string_view document);
InputBindingProfile parseInputProfileJson(const nlohmann::json &document, const InputActionMap &actions);
InputBindingProfile parseInputProfileString(std::string_view document, const InputActionMap &actions);
InputActionMap applyInputProfile(InputActionMap map, const InputBindingProfile &profile);
InputActionFrame evaluateInputActions(const InputActionMap &map, const InputSnapshot &snapshot,
                                      const std::vector<std::string> &action_set_stack);

} // namespace Pelican
