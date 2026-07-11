#include "userinput.hpp"
#include "../loader/basicconfig.hpp"
#include "../os/actionmap.hpp"
#include "../os/inputstate.hpp"

#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

class InputActionsRuntime : public ModuleBase<InputActionsRuntime> {
    std::optional<InputActionMap> action_map;
    std::vector<std::string> action_set_stack;
    InputActionFrame current_frame;
    std::uint64_t frozen_generation = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t evaluation_count = 0;

    const InputActionDefinition *requireAction(std::string_view action_name) const {
        if (!action_map) {
            return nullptr;
        }
        const auto *action = action_map->findAction(action_name);
        if (action == nullptr) {
            throw std::runtime_error("unknown input action: " + std::string{action_name});
        }
        return action;
    }

    const InputActionDefinition *requireNonPoseAction(std::string_view action_name) const {
        const auto *action = requireAction(action_name);
        if (action != nullptr && action->type == InputActionType::pose) {
            throw std::runtime_error("input action is a pose action and has no button/axis state: " +
                                     std::string{action_name});
        }
        return action;
    }

    const InputActionDefinition *requireTypedAction(std::string_view action_name, InputActionType expected,
                                                    std::string_view value_name) const {
        const auto *action = requireAction(action_name);
        if (action != nullptr && action->type != expected) {
            throw std::runtime_error("input action '" + std::string{action_name} + "' is not an " +
                                     std::string{value_name} + " action");
        }
        return action;
    }

    void ensureFrame() {
        auto &input_state = GET_MODULE(InputState);
        const auto generation = input_state.frameGeneration();
        if (frozen_generation == generation) {
            return;
        }

        const auto action_snapshot = input_state.freezeActionsSnapshot();
        current_frame = action_map ? evaluateInputActions(*action_map, action_snapshot, action_set_stack)
                                   : InputActionFrame{};
        frozen_generation = generation;
        ++evaluation_count;
    }

    void validateActionSetStack(const std::vector<std::string> &stack) const {
        if (!action_map) {
            return;
        }
        for (const auto &set_name : stack) {
            if (!action_map->hasActionSet(set_name)) {
                throw std::runtime_error("unknown input action set: " + set_name);
            }
        }
    }

  public:
    InputActionsRuntime() {
        const auto input_actions_json = GET_MODULE(ProjectBasicConfig).inputActionsJson();
        if (!input_actions_json) {
            return;
        }

        action_map = parseInputActionsString(*input_actions_json);
        if (!action_map->actionSets().empty()) {
            action_set_stack.push_back(action_map->actionSets().front().name);
        }
    }

    bool isConfigured() const noexcept {
        return action_map.has_value();
    }

    void freezeFrame() {
        ensureFrame();
    }

    std::uint64_t evaluationCount() const noexcept {
        return evaluation_count;
    }

    InputActionState state(std::string_view action_name) {
        if (requireNonPoseAction(action_name) == nullptr) {
            return {};
        }
        ensureFrame();
        return current_frame.get(action_name);
    }

    float axis1(std::string_view action_name) {
        if (requireTypedAction(action_name, InputActionType::axis1, "axis1") == nullptr) {
            return 0.0f;
        }
        ensureFrame();
        return current_frame.get(action_name).axis1;
    }

    ActionAxis2 axis2(std::string_view action_name) {
        if (requireTypedAction(action_name, InputActionType::axis2, "axis2") == nullptr) {
            return {};
        }
        ensureFrame();
        return current_frame.get(action_name).axis2;
    }

    ActionPose pose(std::string_view action_name) {
        if (requireTypedAction(action_name, InputActionType::pose, "pose") == nullptr) {
            return {};
        }
        ensureFrame();
        return current_frame.pose(action_name);
    }

    void setStack(const std::vector<std::string> &stack) {
        validateActionSetStack(stack);
        action_set_stack = stack;
    }

    void pushSet(std::string set_name) {
        std::vector<std::string> next_stack = action_set_stack;
        next_stack.push_back(std::move(set_name));
        validateActionSetStack(next_stack);
        action_set_stack = std::move(next_stack);
    }

    bool popSet() {
        if (action_set_stack.empty()) {
            return false;
        }
        action_set_stack.pop_back();
        return true;
    }

    void clearStack() {
        action_set_stack.clear();
    }

    std::vector<std::string> stack() const {
        return action_set_stack;
    }
};

} // namespace

bool UserInput::getKey(KeyCode code) {
    return GET_MODULE(InputState).currentSnapshot().getKey(code);
}

bool UserInput::isKeyPushed(KeyCode code) {
    return GET_MODULE(InputState).currentSnapshot().isKeyPushed(code);
}

bool UserInput::isKeyReleased(KeyCode code) {
    return GET_MODULE(InputState).currentSnapshot().isKeyReleased(code);
}

bool Actions::isConfigured() {
    return GET_MODULE(InputActionsRuntime).isConfigured();
}

bool Actions::isPressed(std::string_view action_name) {
    return GET_MODULE(InputActionsRuntime).state(action_name).pressed;
}

bool Actions::isReleased(std::string_view action_name) {
    return GET_MODULE(InputActionsRuntime).state(action_name).released;
}

bool Actions::isHeld(std::string_view action_name) {
    return GET_MODULE(InputActionsRuntime).state(action_name).held;
}

float Actions::axis1(std::string_view action_name) {
    return GET_MODULE(InputActionsRuntime).axis1(action_name);
}

ActionAxis2 Actions::axis2(std::string_view action_name) {
    return GET_MODULE(InputActionsRuntime).axis2(action_name);
}

ActionPose Actions::pose(std::string_view action_name) {
    return GET_MODULE(InputActionsRuntime).pose(action_name);
}

void Actions::setActionSetStack(const std::vector<std::string> &action_set_stack) {
    GET_MODULE(InputActionsRuntime).setStack(action_set_stack);
}

void Actions::pushActionSet(std::string action_set_name) {
    GET_MODULE(InputActionsRuntime).pushSet(std::move(action_set_name));
}

bool Actions::popActionSet() {
    return GET_MODULE(InputActionsRuntime).popSet();
}

void Actions::clearActionSetStack() {
    GET_MODULE(InputActionsRuntime).clearStack();
}

std::vector<std::string> Actions::actionSetStack() {
    return GET_MODULE(InputActionsRuntime).stack();
}

namespace internal {

void freezeInputActionsFrame() {
    GET_MODULE(InputActionsRuntime).freezeFrame();
}

std::uint64_t inputActionsEvaluationCount() {
    return GET_MODULE(InputActionsRuntime).evaluationCount();
}

} // namespace internal

} // namespace Pelican
