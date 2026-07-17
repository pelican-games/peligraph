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

XrActionType xrActionType(InputActionType type) {
    switch (type) {
    case InputActionType::button:
        return XR_ACTION_TYPE_BOOLEAN_INPUT;
    case InputActionType::axis1:
        return XR_ACTION_TYPE_FLOAT_INPUT;
    case InputActionType::axis2:
        return XR_ACTION_TYPE_VECTOR2F_INPUT;
    case InputActionType::pose:
        return XR_ACTION_TYPE_POSE_INPUT;
    }
    return XR_ACTION_TYPE_BOOLEAN_INPUT;
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

ActionPoseHand handIdentity(std::string_view value) noexcept {
    if (value == leftHandPath || value.ends_with("_left")) return ActionPoseHand::left;
    if (value == rightHandPath || value.ends_with("_right")) return ActionPoseHand::right;
    return ActionPoseHand::none;
}

std::string_view handPath(ActionPoseHand hand) noexcept {
    switch (hand) {
    case ActionPoseHand::left:
        return leftHandPath;
    case ActionPoseHand::right:
        return rightHandPath;
    case ActionPoseHand::none:
        return {};
    }
    return {};
}

ActionPose poseIdentity(std::string_view action_name, ActionPoseReferenceSpace reference_space) {
    ActionPose pose;
    pose.source = action_name == "head" ? ActionPoseSource::synthetic_head
                                        : ActionPoseSource::action_space;
    pose.reference_space = reference_space;
    pose.hand = handIdentity(action_name);
    return pose;
}

void copyLocatedPose(ActionPose &destination, const XrSpaceLocation &location) noexcept {
    destination.position[0] = location.pose.position.x;
    destination.position[1] = location.pose.position.y;
    destination.position[2] = location.pose.position.z;
    destination.orientation[0] = location.pose.orientation.x;
    destination.orientation[1] = location.pose.orientation.y;
    destination.orientation[2] = location.pose.orientation.z;
    destination.orientation[3] = location.pose.orientation.w;
    destination.orientation_valid =
        (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
    destination.position_valid =
        (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    destination.orientation_tracked =
        (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
    destination.position_tracked =
        (location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
    destination.valid = destination.orientation_valid && destination.position_valid;
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
    resolveRequired(get_instance_proc_addr, instance, "xrGetActionStatePose", api.get_pose);
    resolveRequired(get_instance_proc_addr, instance, "xrCreateActionSpace", api.create_action_space);
    resolveRequired(get_instance_proc_addr, instance, "xrLocateSpace", api.locate_space);
    resolveRequired(get_instance_proc_addr, instance, "xrDestroySpace", api.destroy_space);
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
            if (input_action.type == InputActionType::pose) {
                inactive_frame.poses.emplace(input_action.name, ActionPose{});
                inactive_pose_samples.push_back(
                    {input_action.name,
                     poseIdentity(input_action.name, ActionPoseReferenceSpace::unknown)});
                if (input_action.name == "head") {
                    if (!input_action.bindings.empty()) {
                        throw std::runtime_error(
                            "synthetic head pose action must not have an OpenXR binding: head");
                    }
                    continue;
                }
            }
            const auto action_type = xrActionType(input_action.type);

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
                    const auto identity = handIdentity(*hand);
                    if (input_action.type == InputActionType::pose &&
                        action_record.hand != ActionPoseHand::none &&
                        action_record.hand != identity) {
                        throw std::runtime_error(
                            "OpenXR pose action must use a single left/right identity: " +
                            input_action.name);
                    }
                    if (input_action.type == InputActionType::pose) action_record.hand = identity;
                    if (unique_subactions.insert(subaction).second) {
                        action_record.subaction_paths.push_back(subaction);
                    }
                }
            }
            if (input_action.type == InputActionType::pose &&
                action_record.subaction_paths.empty()) {
                const auto identity = handIdentity(input_action.name);
                const auto default_path = handPath(identity);
                if (!default_path.empty()) {
                    action_record.hand = identity;
                    action_record.subaction_paths.push_back(path(default_path));
                }
            }
            const auto named_hand = handIdentity(input_action.name);
            if (input_action.type == InputActionType::pose &&
                named_hand != ActionPoseHand::none &&
                action_record.hand != ActionPoseHand::none &&
                named_hand != action_record.hand) {
                throw std::runtime_error("OpenXR pose binding hand does not match action name: " +
                                         input_action.name);
            }
            if (input_action.type == InputActionType::pose &&
                action_record.subaction_paths.size() > 1) {
                throw std::runtime_error("OpenXR pose action must use a single left/right identity: " +
                                         input_action.name);
            }

            XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
            copyName(action_info.actionName, xrName(input_action.name, action_index), "action");
            copyName(action_info.localizedActionName, input_action.name, "localized action");
            action_info.actionType = action_type;
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
            if (input_action.type == InputActionType::pose) {
                const auto inactive = std::find_if(
                    inactive_pose_samples.begin(), inactive_pose_samples.end(),
                    [&](const auto &sample) { return sample.action_name == input_action.name; });
                if (inactive != inactive_pose_samples.end()) inactive->pose.hand = action_record.hand;
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

    for (auto &set : action_sets) {
        for (auto &action : set.actions) {
            if (action.type != InputActionType::pose) continue;
            const std::array<XrPath, 1> global_path{XR_NULL_PATH};
            const auto *space_paths = action.subaction_paths.empty() ? global_path.data()
                                                                     : action.subaction_paths.data();
            const auto space_count = action.subaction_paths.empty() ? global_path.size()
                                                                     : action.subaction_paths.size();
            for (std::size_t index = 0; index < space_count; ++index) {
                XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
                space_info.action = action.action;
                space_info.subactionPath = space_paths[index];
                space_info.poseInActionSpace.orientation.w = 1.0F;
                PoseSpaceRecord pose_space;
                pose_space.subaction_path = space_paths[index];
                pose_space.hand = action.hand;
                const auto result = api.create_action_space(session, &space_info, &pose_space.space);
                if (XR_FAILED(result) || pose_space.space == XR_NULL_HANDLE) {
                    throwFailure("xrCreateActionSpace(" + action.name + ")", result);
                }
                action.pose_spaces.push_back(pose_space);
            }
        }
    }
}

void XrActionRuntime::destroy() noexcept {
    for (auto set_it = action_sets.rbegin(); set_it != action_sets.rend(); ++set_it) {
        for (auto action_it = set_it->actions.rbegin(); action_it != set_it->actions.rend();
             ++action_it) {
            for (auto space_it = action_it->pose_spaces.rbegin();
                 space_it != action_it->pose_spaces.rend(); ++space_it) {
                if (space_it->space != XR_NULL_HANDLE && api.destroy_space != nullptr) {
                    (void)api.destroy_space(space_it->space);
                    space_it->space = XR_NULL_HANDLE;
                }
            }
        }
    }
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

XrInputFrame XrActionRuntime::sync(
    const std::vector<std::string> &active_action_set_stack, bool input_eligible,
    XrSpace base_space, XrTime display_time, ActionPoseReferenceSpace reference_space) {
    XrInputFrame result{inactive_frame, inactive_pose_samples};
    for (auto &sample : result.pose_samples) sample.pose.reference_space = reference_space;
    if (!input_eligible) return result;

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

    auto &frame = result.actions;
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
                case InputActionType::pose: {
                    XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
                    const auto state_result = api.get_pose(session, &get_info, &state);
                    if (XR_FAILED(state_result)) {
                        throwFailure("xrGetActionStatePose(" + action.name + ")", state_result);
                    }
                    if (state.isActive != XR_TRUE) break;
                    if (base_space == XR_NULL_HANDLE) {
                        throw std::runtime_error("xrLocateSpace requires a reference space for pose action '" +
                                                 action.name + "'");
                    }
                    const auto pose_space = std::find_if(
                        action.pose_spaces.begin(), action.pose_spaces.end(),
                        [&](const auto &space) { return space.subaction_path == query_paths[i]; });
                    if (pose_space == action.pose_spaces.end()) {
                        throw std::runtime_error("OpenXR pose action has no action space: " +
                                                 action.name);
                    }
                    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
                    const auto locate_result =
                        api.locate_space(pose_space->space, base_space, display_time, &location);
                    if (XR_FAILED(locate_result)) {
                        throwFailure("xrLocateSpace(" + action.name + ")", locate_result);
                    }
                    const auto destination = std::find_if(
                        result.pose_samples.begin(), result.pose_samples.end(),
                        [&](const auto &pose) { return pose.action_name == action.name; });
                    if (destination == result.pose_samples.end()) {
                        throw std::logic_error("OpenXR pose sample identity is missing: " +
                                               action.name);
                    }
                    destination->pose = poseIdentity(action.name, reference_space);
                    destination->pose.hand = pose_space->hand;
                    copyLocatedPose(destination->pose, location);
                    break;
                }
                }
                mergeState(destination, sample);
            }
        }
    }
    return result;
}

[[noreturn]] void XrActionRuntime::throwFailure(const std::string &operation, XrResult result) {
    throw std::runtime_error(operation + " failed (XrResult " + std::to_string(result) + ")");
}

} // namespace Pelican::OpenXr
