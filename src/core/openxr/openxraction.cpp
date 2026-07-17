#include "openxraction.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace Pelican::OpenXr {
namespace {

constexpr std::string_view touchInteractionProfile =
    "/interaction_profiles/oculus/touch_controller";
constexpr std::string_view xrBindingPrefix = "xr:";
constexpr std::string_view leftHandPath = "/user/hand/left";
constexpr std::string_view rightHandPath = "/user/hand/right";

template <class Function>
void resolveRequired(PFN_xrGetInstanceProcAddr get_instance_proc_addr, XrInstance instance,
                     const char *name, Function &destination) {
    PFN_xrVoidFunction function = nullptr;
    const auto result = get_instance_proc_addr(instance, name, &function);
    if (XR_FAILED(result) || function == nullptr) {
        throw std::runtime_error(std::string{"xrGetInstanceProcAddr could not resolve "} + name);
    }
    destination = reinterpret_cast<Function>(function);
}

template <std::size_t Size>
void copyName(char (&destination)[Size], std::string_view value, std::string_view kind) {
    if (value.empty() || value.size() >= Size) {
        throw std::runtime_error("OpenXR " + std::string{kind} + " name is empty or too long: " +
                                 std::string{value});
    }
    std::memcpy(destination, value.data(), value.size());
    destination[value.size()] = '\0';
}

std::string xrName(std::string_view project_name, std::size_t index) {
    std::string result;
    result.reserve(project_name.size() + 16);
    for (const auto ch : project_name) {
        if (ch >= 'A' && ch <= 'Z') {
            result.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            result.push_back(ch);
        }
    }
    // The project schema permits case-distinct identifiers, while OpenXR path
    // names are lower-case. The stable suffix prevents every collision after
    // conversion (including Foo versus foo_0).
    result += "_" + std::to_string(index);
    return result;
}

std::optional<XrActionType> xrActionType(InputActionType type) {
    switch (type) {
    case InputActionType::button:
        return XR_ACTION_TYPE_BOOLEAN_INPUT;
    case InputActionType::axis1:
        return XR_ACTION_TYPE_FLOAT_INPUT;
    case InputActionType::axis2:
        return XR_ACTION_TYPE_VECTOR2F_INPUT;
    case InputActionType::pose:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string_view> xrBindingPath(const InputActionBinding &binding) {
    if (!binding.text.starts_with(xrBindingPrefix)) return std::nullopt;
    const auto value = std::string_view{binding.text}.substr(xrBindingPrefix.size());
    if (!value.starts_with("/user/hand/")) {
        throw std::runtime_error("OpenXR Touch binding must start with xr:/user/hand/: " +
                                 binding.text);
    }
    return value;
}

std::optional<std::string_view> handPath(std::string_view binding_path) {
    if (binding_path.starts_with(std::string{leftHandPath} + "/")) return leftHandPath;
    if (binding_path.starts_with(std::string{rightHandPath} + "/")) return rightHandPath;
    return std::nullopt;
}

float clampAxis(float value) noexcept {
    return std::clamp(value, -1.0F, 1.0F);
}

void mergeState(InputActionState &destination, const InputActionState &source) noexcept {
    const bool released = destination.released || source.released;
    destination.pressed = destination.pressed || source.pressed;
    destination.held = destination.held || source.held;
    destination.released = released && !destination.held;
    destination.axis1 = clampAxis(destination.axis1 + source.axis1);
    destination.axis2.x = clampAxis(destination.axis2.x + source.axis2.x);
    destination.axis2.y = clampAxis(destination.axis2.y + source.axis2.y);
}

bool nonZero(float value) noexcept {
    return std::abs(value) > 0.0F;
}

bool nonZero(XrVector2f value) noexcept {
    return nonZero(value.x) || nonZero(value.y);
}

} // namespace

XrActionRuntime::XrActionRuntime(PFN_xrGetInstanceProcAddr get_instance_proc_addr,
                                 XrInstance xr_instance, XrSession xr_session,
                                 const InputActionMap &input_actions)
    : instance{xr_instance}, session{xr_session} {
    if (instance == XR_NULL_HANDLE || session == XR_NULL_HANDLE) {
        throw std::runtime_error("OpenXR action runtime requires an instance and session");
    }
    resolve(get_instance_proc_addr);
    try {
        create(input_actions);
    } catch (...) {
        destroy();
        throw;
    }
}

XrActionRuntime::~XrActionRuntime() {
    destroy();
}

void XrActionRuntime::resolve(PFN_xrGetInstanceProcAddr get_instance_proc_addr) {
    if (get_instance_proc_addr == nullptr) {
        throw std::runtime_error("OpenXR action dispatch has no xrGetInstanceProcAddr");
    }
    resolveRequired(get_instance_proc_addr, instance, "xrStringToPath", api.string_to_path);
    resolveRequired(get_instance_proc_addr, instance, "xrCreateActionSet", api.create_action_set);
    resolveRequired(get_instance_proc_addr, instance, "xrDestroyActionSet", api.destroy_action_set);
    resolveRequired(get_instance_proc_addr, instance, "xrCreateAction", api.create_action);
    resolveRequired(get_instance_proc_addr, instance, "xrDestroyAction", api.destroy_action);
    resolveRequired(get_instance_proc_addr, instance, "xrSuggestInteractionProfileBindings",
                    api.suggest_bindings);
    resolveRequired(get_instance_proc_addr, instance, "xrAttachSessionActionSets",
                    api.attach_action_sets);
    resolveRequired(get_instance_proc_addr, instance, "xrSyncActions", api.sync_actions);
    resolveRequired(get_instance_proc_addr, instance, "xrGetActionStateBoolean", api.get_boolean);
    resolveRequired(get_instance_proc_addr, instance, "xrGetActionStateFloat", api.get_float);
    resolveRequired(get_instance_proc_addr, instance, "xrGetActionStateVector2f", api.get_vector2);
}

XrPath XrActionRuntime::path(std::string_view value) const {
    XrPath result = XR_NULL_PATH;
    const std::string terminated{value};
    const auto xr_result = api.string_to_path(instance, terminated.c_str(), &result);
    if (XR_FAILED(xr_result) || result == XR_NULL_PATH) {
        throwFailure("xrStringToPath(" + terminated + ")", xr_result);
    }
    return result;
}

void XrActionRuntime::create(const InputActionMap &input_actions) {
    std::vector<XrActionSuggestedBinding> suggested_bindings;
    std::vector<XrActionSet> attach_sets;
    std::size_t action_index = 0;

    action_sets.reserve(input_actions.actionSets().size());
    attach_sets.reserve(input_actions.actionSets().size());
    for (std::size_t set_index = 0; set_index < input_actions.actionSets().size(); ++set_index) {
        const auto &input_set = input_actions.actionSets()[set_index];
        ActionSetRecord record;
        record.name = input_set.name;

        XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
        copyName(set_info.actionSetName, xrName(input_set.name, set_index), "action set");
        copyName(set_info.localizedActionSetName, input_set.name, "localized action set");
        set_info.priority = static_cast<uint32_t>(set_index);
        const auto set_result = api.create_action_set(instance, &set_info, &record.action_set);
        if (XR_FAILED(set_result) || record.action_set == XR_NULL_HANDLE) {
            throwFailure("xrCreateActionSet(" + input_set.name + ")", set_result);
        }
        action_sets.push_back(std::move(record));
        auto &stored_set = action_sets.back();

        for (const auto &input_action : input_set.actions) {
            inactive_frame.action_types.emplace(input_action.name, input_action.type);
            inactive_frame.actions.emplace(input_action.name, InputActionState{});
            const auto action_type = xrActionType(input_action.type);
            if (!action_type) continue; // XR3b owns pose actions and spaces.

            ActionRecord action_record;
            action_record.name = input_action.name;
            action_record.type = input_action.type;

            std::vector<XrPath> suggested_paths;
            std::unordered_set<XrPath> unique_subactions;
            for (const auto &binding : input_action.bindings) {
                const auto binding_path = xrBindingPath(binding);
                if (!binding_path) continue;
                suggested_paths.push_back(path(*binding_path));
                if (const auto hand = handPath(*binding_path)) {
                    const auto subaction = path(*hand);
                    if (unique_subactions.insert(subaction).second) {
                        action_record.subaction_paths.push_back(subaction);
                    }
                }
            }

            XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
            copyName(action_info.actionName, xrName(input_action.name, action_index), "action");
            copyName(action_info.localizedActionName, input_action.name, "localized action");
            action_info.actionType = *action_type;
            action_info.countSubactionPaths =
                static_cast<uint32_t>(action_record.subaction_paths.size());
            action_info.subactionPaths = action_record.subaction_paths.data();
            const auto action_result =
                api.create_action(stored_set.action_set, &action_info, &action_record.action);
            if (XR_FAILED(action_result) || action_record.action == XR_NULL_HANDLE) {
                throwFailure("xrCreateAction(" + input_action.name + ")", action_result);
            }
            for (const auto suggested_path : suggested_paths) {
                suggested_bindings.push_back({action_record.action, suggested_path});
            }
            stored_set.actions.push_back(std::move(action_record));
            ++action_index;
        }

        set_lookup.emplace(stored_set.name, action_sets.size() - 1);
        attach_sets.push_back(stored_set.action_set);
    }

    if (!suggested_bindings.empty()) {
        XrInteractionProfileSuggestedBinding suggested{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = path(touchInteractionProfile);
        suggested.countSuggestedBindings = static_cast<uint32_t>(suggested_bindings.size());
        suggested.suggestedBindings = suggested_bindings.data();
        const auto result = api.suggest_bindings(instance, &suggested);
        if (XR_FAILED(result)) {
            throwFailure("xrSuggestInteractionProfileBindings(Touch)", result);
        }
    }

    if (!attach_sets.empty()) {
        XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach_info.countActionSets = static_cast<uint32_t>(attach_sets.size());
        attach_info.actionSets = attach_sets.data();
        const auto result = api.attach_action_sets(session, &attach_info);
        if (XR_FAILED(result)) throwFailure("xrAttachSessionActionSets", result);
    }
}

void XrActionRuntime::destroy() noexcept {
    // xrDestroyActionSet destroys all actions in that set. Avoid redundant
    // xrDestroyAction calls during normal teardown while still keeping the
    // dispatch entry available for the explicit ownership contract.
    for (auto set_it = action_sets.rbegin(); set_it != action_sets.rend(); ++set_it) {
        if (set_it->action_set != XR_NULL_HANDLE && api.destroy_action_set != nullptr) {
            (void)api.destroy_action_set(set_it->action_set);
            set_it->action_set = XR_NULL_HANDLE;
        }
    }
    action_sets.clear();
    set_lookup.clear();
}

InputActionFrame XrActionRuntime::sync(
    const std::vector<std::string> &active_action_set_stack, bool input_eligible) {
    if (!input_eligible) return inactive_frame;

    std::vector<XrActiveActionSet> active_sets;
    active_sets.reserve(active_action_set_stack.size());
    for (const auto &set_name : active_action_set_stack) {
        const auto found = set_lookup.find(set_name);
        if (found == set_lookup.end()) {
            throw std::runtime_error("unknown OpenXR input action set: " + set_name);
        }
        active_sets.push_back({action_sets[found->second].action_set, XR_NULL_PATH});
    }

    XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
    sync_info.countActiveActionSets = static_cast<uint32_t>(active_sets.size());
    sync_info.activeActionSets = active_sets.data();
    const auto sync_result = api.sync_actions(session, &sync_info);
    if (XR_FAILED(sync_result)) throwFailure("xrSyncActions", sync_result);

    InputActionFrame frame = inactive_frame;
    for (const auto &active : active_sets) {
        const auto set_it = std::find_if(action_sets.begin(), action_sets.end(), [&](const auto &set) {
            return set.action_set == active.actionSet;
        });
        if (set_it == action_sets.end()) continue;

        for (const auto &action : set_it->actions) {
            auto &destination = frame.actions.at(action.name);
            const std::array<XrPath, 1> global_path{XR_NULL_PATH};
            const auto *query_paths = action.subaction_paths.empty() ? global_path.data()
                                                                     : action.subaction_paths.data();
            const auto query_count = action.subaction_paths.empty() ? global_path.size()
                                                                     : action.subaction_paths.size();
            for (std::size_t i = 0; i < query_count; ++i) {
                XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
                get_info.action = action.action;
                get_info.subactionPath = query_paths[i];
                InputActionState sample;
                switch (action.type) {
                case InputActionType::button: {
                    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
                    const auto result = api.get_boolean(session, &get_info, &state);
                    if (XR_FAILED(result)) {
                        throwFailure("xrGetActionStateBoolean(" + action.name + ")", result);
                    }
                    if (state.isActive == XR_TRUE) {
                        sample.held = state.currentState == XR_TRUE;
                        sample.pressed = state.changedSinceLastSync == XR_TRUE && sample.held;
                        sample.released = state.changedSinceLastSync == XR_TRUE && !sample.held;
                    }
                    break;
                }
                case InputActionType::axis1: {
                    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
                    const auto result = api.get_float(session, &get_info, &state);
                    if (XR_FAILED(result)) {
                        throwFailure("xrGetActionStateFloat(" + action.name + ")", result);
                    }
                    if (state.isActive == XR_TRUE) {
                        sample.axis1 = clampAxis(state.currentState);
                        sample.held = nonZero(sample.axis1);
                        sample.pressed = state.changedSinceLastSync == XR_TRUE && sample.held;
                        sample.released = state.changedSinceLastSync == XR_TRUE && !sample.held;
                    }
                    break;
                }
                case InputActionType::axis2: {
                    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
                    const auto result = api.get_vector2(session, &get_info, &state);
                    if (XR_FAILED(result)) {
                        throwFailure("xrGetActionStateVector2f(" + action.name + ")", result);
                    }
                    if (state.isActive == XR_TRUE) {
                        sample.axis2 = {clampAxis(state.currentState.x),
                                        clampAxis(state.currentState.y)};
                        sample.held = nonZero(state.currentState);
                        sample.pressed = state.changedSinceLastSync == XR_TRUE && sample.held;
                        sample.released = state.changedSinceLastSync == XR_TRUE && !sample.held;
                    }
                    break;
                }
                case InputActionType::pose:
                    break;
                }
                mergeState(destination, sample);
            }
        }
    }
    return frame;
}

[[noreturn]] void XrActionRuntime::throwFailure(const std::string &operation, XrResult result) {
    throw std::runtime_error(operation + " failed (XrResult " + std::to_string(result) + ")");
}

} // namespace Pelican::OpenXr
