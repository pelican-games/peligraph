#include "userinput.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../launchconfig.hpp"
#include "../os/actionmap.hpp"
#include "../os/inputstate.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <unordered_map>

namespace Pelican {

namespace {

constexpr std::string_view freeCameraActionsResource =
    "engine://input/free_camera_actions.json";
constexpr std::string_view freeCameraBlenderProfileResource =
    "engine://input/profiles/free_camera_blender.json";
constexpr std::string_view freeCameraUnityProfileResource =
    "engine://input/profiles/free_camera_unity.json";
constexpr std::string_view freeCameraBlenderProfileName = "blender";
constexpr std::string_view freeCameraUnityProfileName = "unity";

std::string_view freeCameraProfileName(EngineLaunchFreeCameraPreset preset) {
    switch (preset) {
    case EngineLaunchFreeCameraPreset::Blender:
        return freeCameraBlenderProfileName;
    case EngineLaunchFreeCameraPreset::Unity:
        return freeCameraUnityProfileName;
    }
    throw std::runtime_error("unknown runtime free camera preset");
}

class InputActionsRuntime : public ModuleBase<InputActionsRuntime> {
    std::optional<InputActionMap> action_definitions;
    std::optional<InputActionMap> action_map;
    std::unordered_map<std::string, InputBindingProfile> profiles;
    std::optional<std::string> active_profile;
    std::vector<std::string> action_set_stack;
    InputActionFrame current_frame;
    InputActionFrame backend_frame;
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
        auto frame_input = input_state.currentFrameInput();
        frame_input.snapshot = action_snapshot;
        current_frame = action_map ? evaluateInputActions(*action_map, frame_input, action_set_stack)
                                   : InputActionFrame{};
        if (action_map) mergeInputActionBackendFrame(current_frame, backend_frame);
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
        const auto &launch = GET_MODULE(EngineLaunchConfig);
        std::optional<std::string> input_actions_json;
        std::unordered_map<std::string, std::string> profile_jsons;
        std::optional<std::string> selected;

        if (launch.free_camera) {
            if (launch.input_profile) {
                throw std::runtime_error(
                    "runtime free camera cannot be combined with an input profile override");
            }
            auto &resolver = GET_MODULE(PathResolver);
            input_actions_json = resolver.loadText(freeCameraActionsResource);
            profile_jsons.emplace(
                freeCameraBlenderProfileName,
                resolver.loadText(freeCameraBlenderProfileResource));
            profile_jsons.emplace(
                freeCameraUnityProfileName,
                resolver.loadText(freeCameraUnityProfileResource));
            selected = std::string{freeCameraProfileName(launch.free_camera->preset)};
        } else {
            auto &config = GET_MODULE(ProjectBasicConfig);
            input_actions_json = config.inputActionsJson();
            profile_jsons = config.inputProfileJsons();
            selected = launch.input_profile ? launch.input_profile
                                            : config.defaultInputProfile();
        }

        if (!input_actions_json) {
            return;
        }

        action_definitions = parseInputActionsString(*input_actions_json);
        action_map = action_definitions;
        for (const auto &[name, profile_json] : profile_jsons) {
            auto profile = parseInputProfileString(profile_json, *action_definitions);
            if (profile.name != name) {
                throw std::runtime_error("input profile key '" + name + "' does not match document name '" +
                                         profile.name + "'");
            }
            profiles.emplace(name, std::move(profile));
        }
        if (selected) {
            selectProfile(*selected);
        }
        if (!action_map->actionSets().empty()) {
            action_set_stack.push_back(action_map->actionSets().front().name);
        }
    }

    bool isConfigured() const noexcept {
        return action_map.has_value();
    }

    void selectProfile(std::string_view name) {
        const auto it = profiles.find(std::string{name});
        if (it == profiles.end()) {
            throw std::runtime_error("unknown input profile: " + std::string{name});
        }
        action_map = applyInputProfile(*action_definitions, it->second);
        active_profile = it->first;
        frozen_generation = std::numeric_limits<std::uint64_t>::max();
    }

    bool pollsGamepad() const noexcept {
        return action_map && action_map->usesGamepad();
    }

    std::optional<std::string> activeProfile() const {
        return active_profile;
    }

    std::vector<std::string> availableProfiles() const {
        std::vector<std::string> result;
        result.reserve(profiles.size());
        for (const auto &[name, profile] : profiles) {
            (void)profile;
            result.push_back(name);
        }
        std::sort(result.begin(), result.end());
        return result;
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

    const InputActionMap *map() const noexcept {
        return action_map ? &*action_map : nullptr;
    }

    void setBackendFrame(InputActionFrame frame) {
        backend_frame = std::move(frame);
        frozen_generation = std::numeric_limits<std::uint64_t>::max();
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

void prepareInputActionsRuntime() {
    (void)GET_MODULE(InputActionsRuntime);
}

void freezeInputActionsFrame() {
    GET_MODULE(InputActionsRuntime).freezeFrame();
}

std::uint64_t inputActionsEvaluationCount() {
    return GET_MODULE(InputActionsRuntime).evaluationCount();
}

bool gamepadPollingEnabled() {
    return GET_MODULE(InputActionsRuntime).pollsGamepad();
}

std::optional<std::string> activeInputProfile() {
    return GET_MODULE(InputActionsRuntime).activeProfile();
}

std::vector<std::string> availableInputProfiles() {
    return GET_MODULE(InputActionsRuntime).availableProfiles();
}

void selectInputProfile(std::string_view profile_name) {
    GET_MODULE(InputActionsRuntime).selectProfile(profile_name);
}

const InputActionMap *inputActionMap() {
    return GET_MODULE(InputActionsRuntime).map();
}

std::vector<std::string> poseInputActionNames() {
    const auto *map = GET_MODULE(InputActionsRuntime).map();
    return map == nullptr ? std::vector<std::string>{} : map->poseActionNames();
}

void setInputActionBackendFrame(InputActionFrame frame) {
    GET_MODULE(InputActionsRuntime).setBackendFrame(std::move(frame));
}

} // namespace internal

} // namespace Pelican
