#include <catch2/catch_test_macros.hpp>

#include "../src/core/openxr/openxraction.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

template <class Handle> Handle fakeHandle(std::uintptr_t value) {
    return reinterpret_cast<Handle>(value);
}

XrInstance fakeInstance() { return fakeHandle<XrInstance>(0x101); }
XrSession fakeSession() { return fakeHandle<XrSession>(0x301); }
XrSpace fakeBaseSpace() { return fakeHandle<XrSpace>(0x401); }
XrActionSet fakeActionSet(std::size_t index) {
    return fakeHandle<XrActionSet>(0x500 + index);
}
XrAction fakeAction(std::size_t index) { return fakeHandle<XrAction>(0x700 + index); }
XrSpace fakeActionSpace(std::size_t index) { return fakeHandle<XrSpace>(0x900 + index); }

struct CreatedAction {
    std::string name;
    XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::vector<XrPath> subactions;
};

struct CreatedSpace {
    XrAction action = XR_NULL_HANDLE;
    XrPath subaction = XR_NULL_PATH;
};

struct FakeRuntime {
    std::vector<std::string> calls;
    std::unordered_map<std::string, XrPath> paths;
    std::unordered_map<XrAction, CreatedAction> actions;
    std::unordered_map<XrActionSet, std::string> sets;
    std::unordered_map<XrSpace, CreatedSpace> spaces;
    std::vector<XrActionSuggestedBinding> suggestions;
    std::vector<XrActionSet> attached_sets;
    std::vector<XrActionSet> synced_sets;
    XrPath touch_profile = XR_NULL_PATH;
    std::size_t next_path = 1;
    std::size_t next_set = 1;
    std::size_t next_action = 1;
    std::size_t next_space = 1;
    XrSpaceLocationFlags location_flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                          XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                          XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                                          XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
};

thread_local FakeRuntime *active_fake = nullptr;

struct FakeScope {
    explicit FakeScope(FakeRuntime &fake) { active_fake = &fake; }
    ~FakeScope() { active_fake = nullptr; }
};

XrResult XRAPI_CALL fakeStringToPath(XrInstance instance, const char *value, XrPath *path) {
    REQUIRE(instance == fakeInstance());
    const std::string text{value};
    active_fake->calls.push_back("path:" + text);
    const auto [found, inserted] = active_fake->paths.emplace(text, active_fake->next_path);
    if (inserted) ++active_fake->next_path;
    *path = found->second;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeCreateActionSet(XrInstance instance, const XrActionSetCreateInfo *info,
                                        XrActionSet *action_set) {
    REQUIRE(instance == fakeInstance());
    active_fake->calls.push_back("create_set:" + std::string{info->actionSetName});
    *action_set = fakeActionSet(active_fake->next_set++);
    active_fake->sets.emplace(*action_set, info->localizedActionSetName);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeDestroyActionSet(XrActionSet action_set) {
    active_fake->calls.push_back("destroy_set:" + active_fake->sets.at(action_set));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeCreateAction(XrActionSet action_set, const XrActionCreateInfo *info,
                                     XrAction *action) {
    REQUIRE(active_fake->sets.contains(action_set));
    active_fake->calls.push_back("create_action:" + std::string{info->localizedActionName});
    *action = fakeAction(active_fake->next_action++);
    active_fake->actions.emplace(
        *action, CreatedAction{info->localizedActionName, info->actionType,
                               {info->subactionPaths,
                                info->subactionPaths + info->countSubactionPaths}});
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeDestroyAction(XrAction) {
    active_fake->calls.push_back("destroy_action");
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeSuggestBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding *suggested) {
    REQUIRE(instance == fakeInstance());
    active_fake->calls.push_back("suggest_touch");
    active_fake->touch_profile = suggested->interactionProfile;
    active_fake->suggestions.assign(suggested->suggestedBindings,
                                    suggested->suggestedBindings +
                                        suggested->countSuggestedBindings);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeAttachActionSets(XrSession session,
                                         const XrSessionActionSetsAttachInfo *info) {
    REQUIRE(session == fakeSession());
    active_fake->calls.push_back("attach");
    active_fake->attached_sets.assign(info->actionSets,
                                      info->actionSets + info->countActionSets);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeSyncActions(XrSession session, const XrActionsSyncInfo *info) {
    REQUIRE(session == fakeSession());
    active_fake->calls.push_back("sync");
    active_fake->synced_sets.clear();
    for (uint32_t i = 0; i < info->countActiveActionSets; ++i) {
        CHECK(info->activeActionSets[i].subactionPath == XR_NULL_PATH);
        active_fake->synced_sets.push_back(info->activeActionSets[i].actionSet);
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeGetBoolean(XrSession session, const XrActionStateGetInfo *info,
                                   XrActionStateBoolean *state) {
    REQUIRE(session == fakeSession());
    const auto &action = active_fake->actions.at(info->action);
    active_fake->calls.push_back("get_boolean:" + action.name);
    REQUIRE(action.name == "select");
    const auto right = active_fake->paths.at("/user/hand/right");
    state->isActive = XR_TRUE;
    state->currentState = info->subactionPath == right ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = info->subactionPath == right ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeGetFloat(XrSession session, const XrActionStateGetInfo *info,
                                 XrActionStateFloat *state) {
    REQUIRE(session == fakeSession());
    const auto &action = active_fake->actions.at(info->action);
    active_fake->calls.push_back("get_float:" + action.name);
    REQUIRE(action.name == "throttle");
    CHECK(info->subactionPath == active_fake->paths.at("/user/hand/right"));
    state->isActive = XR_TRUE;
    state->currentState = 0.75F;
    state->changedSinceLastSync = XR_TRUE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeGetVector2(XrSession session, const XrActionStateGetInfo *info,
                                   XrActionStateVector2f *state) {
    REQUIRE(session == fakeSession());
    const auto &action = active_fake->actions.at(info->action);
    active_fake->calls.push_back("get_vector2:" + action.name);
    REQUIRE(action.name == "move");
    CHECK(info->subactionPath == active_fake->paths.at("/user/hand/left"));
    state->isActive = XR_TRUE;
    state->currentState = {0.25F, -0.5F};
    state->changedSinceLastSync = XR_TRUE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeGetPose(XrSession session, const XrActionStateGetInfo *info,
                                XrActionStatePose *state) {
    REQUIRE(session == fakeSession());
    const auto &action = active_fake->actions.at(info->action);
    active_fake->calls.push_back("get_pose:" + action.name);
    state->isActive = XR_TRUE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeCreateActionSpace(XrSession session,
                                          const XrActionSpaceCreateInfo *info,
                                          XrSpace *space) {
    REQUIRE(session == fakeSession());
    const auto &action = active_fake->actions.at(info->action);
    active_fake->calls.push_back("create_space:" + action.name);
    CHECK(info->poseInActionSpace.orientation.w == 1.0F);
    *space = fakeActionSpace(active_fake->next_space++);
    active_fake->spaces.emplace(*space, CreatedSpace{info->action, info->subactionPath});
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeLocateSpace(XrSpace space, XrSpace base_space, XrTime time,
                                    XrSpaceLocation *location) {
    const auto &created = active_fake->spaces.at(space);
    const auto &action = active_fake->actions.at(created.action);
    active_fake->calls.push_back("locate_space:" + action.name);
    CHECK(base_space == fakeBaseSpace());
    CHECK(time == 1234);
    location->locationFlags = active_fake->location_flags;
    location->pose.position = {1.0F, 2.0F, 3.0F};
    location->pose.orientation = {0.1F, 0.2F, 0.3F, 0.9F};
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeDestroySpace(XrSpace space) {
    REQUIRE(active_fake->spaces.contains(space));
    active_fake->calls.push_back("destroy_space");
    return XR_SUCCESS;
}

PFN_xrVoidFunction fakeFunction(const std::string &name) {
    if (name == "xrStringToPath") return reinterpret_cast<PFN_xrVoidFunction>(&fakeStringToPath);
    if (name == "xrCreateActionSet")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeCreateActionSet);
    if (name == "xrDestroyActionSet")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeDestroyActionSet);
    if (name == "xrCreateAction") return reinterpret_cast<PFN_xrVoidFunction>(&fakeCreateAction);
    if (name == "xrDestroyAction") return reinterpret_cast<PFN_xrVoidFunction>(&fakeDestroyAction);
    if (name == "xrSuggestInteractionProfileBindings")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeSuggestBindings);
    if (name == "xrAttachSessionActionSets")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeAttachActionSets);
    if (name == "xrSyncActions") return reinterpret_cast<PFN_xrVoidFunction>(&fakeSyncActions);
    if (name == "xrGetActionStateBoolean")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeGetBoolean);
    if (name == "xrGetActionStateFloat")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeGetFloat);
    if (name == "xrGetActionStateVector2f")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeGetVector2);
    if (name == "xrGetActionStatePose")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeGetPose);
    if (name == "xrCreateActionSpace")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeCreateActionSpace);
    if (name == "xrLocateSpace") return reinterpret_cast<PFN_xrVoidFunction>(&fakeLocateSpace);
    if (name == "xrDestroySpace") return reinterpret_cast<PFN_xrVoidFunction>(&fakeDestroySpace);
    return nullptr;
}

XrResult XRAPI_CALL fakeGetInstanceProcAddr(XrInstance instance, const char *name,
                                            PFN_xrVoidFunction *function) {
    REQUIRE(instance == fakeInstance());
    active_fake->calls.push_back("resolve:" + std::string{name});
    *function = fakeFunction(name);
    return *function == nullptr ? XR_ERROR_FUNCTION_UNSUPPORTED : XR_SUCCESS;
}

Pelican::InputActionMap inputActions() {
    const auto actions = Pelican::parseInputActionsString(R"json({
      "schema": "pelican.input_actions",
      "version": 1,
      "action_sets": [
        {"name": "gameplay", "actions": [
          {"name": "select", "type": "button"},
          {"name": "throttle", "type": "axis1"},
          {"name": "move", "type": "axis2"}
        ]},
        {"name": "tracking", "actions": [
          {"name": "aim_left", "type": "pose"},
          {"name": "aim_right", "type": "pose"},
          {"name": "grip_left", "type": "pose"},
          {"name": "grip_right", "type": "pose"},
          {"name": "head", "type": "pose"}
        ]}
      ]
    })json");
    const auto touch = Pelican::parseInputProfileString(R"json({
      "schema": "pelican.input_profile",
      "version": 1,
      "name": "touch",
      "bindings": [
        {"action": "select", "binding": "xr:/user/hand/left/input/x/click"},
        {"action": "select", "binding": "xr:/user/hand/right/input/a/click"},
        {"action": "throttle", "binding": "xr:/user/hand/right/input/trigger/value"},
        {"action": "move", "binding": "xr:/user/hand/left/input/thumbstick"},
        {"action": "aim_left", "binding": "xr:/user/hand/left/input/aim/pose"},
        {"action": "aim_right", "binding": "xr:/user/hand/right/input/aim/pose"},
        {"action": "grip_left", "binding": "xr:/user/hand/left/input/grip/pose"},
        {"action": "grip_right", "binding": "xr:/user/hand/right/input/grip/pose"}
      ]
    })json", actions);
    return Pelican::applyInputProfile(actions, touch);
}

const CreatedAction &findAction(const FakeRuntime &fake, const std::string &name) {
    const auto found = std::find_if(fake.actions.begin(), fake.actions.end(),
                                    [&](const auto &entry) { return entry.second.name == name; });
    REQUIRE(found != fake.actions.end());
    return found->second;
}

const Pelican::InputPoseSample &findPose(const Pelican::OpenXr::XrInputFrame &frame,
                                         const std::string &name) {
    const auto found = std::find_if(frame.pose_samples.begin(), frame.pose_samples.end(),
                                    [&](const auto &sample) {
                                        return sample.action_name == name;
                                    });
    REQUIRE(found != frame.pose_samples.end());
    return *found;
}

} // namespace

TEST_CASE("OpenXR actions are created and Touch bindings suggested before the one attach",
          "[openxr][actions][protocol]") {
    FakeRuntime fake;
    FakeScope scope{fake};
    const auto actions = inputActions();
    {
        Pelican::OpenXr::XrActionRuntime runtime{&fakeGetInstanceProcAddr, fakeInstance(),
                                                 fakeSession(), actions};

        REQUIRE(fake.attached_sets.size() == 2);
        REQUIRE(fake.suggestions.size() == 8);
        CHECK(fake.touch_profile ==
              fake.paths.at("/interaction_profiles/oculus/touch_controller"));

        const auto &select = findAction(fake, "select");
        const auto &throttle = findAction(fake, "throttle");
        const auto &move = findAction(fake, "move");
        const auto &aim_left = findAction(fake, "aim_left");
        CHECK(select.type == XR_ACTION_TYPE_BOOLEAN_INPUT);
        CHECK(throttle.type == XR_ACTION_TYPE_FLOAT_INPUT);
        CHECK(move.type == XR_ACTION_TYPE_VECTOR2F_INPUT);
        CHECK(aim_left.type == XR_ACTION_TYPE_POSE_INPUT);
        CHECK(select.subactions ==
              std::vector<XrPath>{fake.paths.at("/user/hand/left"),
                                  fake.paths.at("/user/hand/right")});
        CHECK(throttle.subactions ==
              std::vector<XrPath>{fake.paths.at("/user/hand/right")});
        CHECK(move.subactions == std::vector<XrPath>{fake.paths.at("/user/hand/left")});
        CHECK(fake.actions.size() == 7); // head is synthetic and is not an XrAction.
        CHECK(fake.spaces.size() == 4);

        const auto attach = std::find(fake.calls.begin(), fake.calls.end(), "attach");
        const auto suggest = std::find(fake.calls.begin(), fake.calls.end(), "suggest_touch");
        REQUIRE(attach != fake.calls.end());
        REQUIRE(suggest != fake.calls.end());
        CHECK(suggest < attach);
        CHECK(std::none_of(attach + 1, fake.calls.end(), [](const auto &call) {
            return call.starts_with("create_set:") || call.starts_with("create_action:");
        }));

        const auto calls_before_focus_loss = fake.calls.size();
        const auto inactive = runtime.sync({"gameplay", "tracking"}, false,
                                           fakeBaseSpace(), 1234,
                                           Pelican::ActionPoseReferenceSpace::local);
        CHECK(fake.calls.size() == calls_before_focus_loss);
        CHECK_FALSE(inactive.actions.get("select").held);
        CHECK(inactive.actions.get("throttle").axis1 == 0.0F);
        CHECK(inactive.actions.get("move").axis2.x == 0.0F);
        CHECK_FALSE(findPose(inactive, "aim_left").pose.valid);
        CHECK(findPose(inactive, "aim_left").pose.hand == Pelican::ActionPoseHand::left);
        CHECK(findPose(inactive, "head").pose.source ==
              Pelican::ActionPoseSource::synthetic_head);

        const auto frame = runtime.sync({"gameplay", "tracking"}, true,
                                        fakeBaseSpace(), 1234,
                                        Pelican::ActionPoseReferenceSpace::local);
        REQUIRE(fake.synced_sets.size() == 2);
        CHECK(frame.actions.get("select").pressed);
        CHECK(frame.actions.get("select").held);
        CHECK_FALSE(frame.actions.get("select").released);
        CHECK(frame.actions.get("throttle").axis1 == 0.75F);
        CHECK(frame.actions.get("move").axis2.x == 0.25F);
        CHECK(frame.actions.get("move").axis2.y == -0.5F);
        const auto &aim = findPose(frame, "aim_left").pose;
        CHECK(aim.valid);
        CHECK(aim.orientation_valid);
        CHECK(aim.position_valid);
        CHECK(aim.orientation_tracked);
        CHECK(aim.position_tracked);
        CHECK(aim.position[0] == 1.0F);
        CHECK(aim.source == Pelican::ActionPoseSource::action_space);
        CHECK(aim.reference_space == Pelican::ActionPoseReferenceSpace::local);
        CHECK(aim.hand == Pelican::ActionPoseHand::left);

        fake.location_flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                              XR_SPACE_LOCATION_POSITION_VALID_BIT |
                              XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        const auto tracking_loss = runtime.sync({"tracking"}, true, fakeBaseSpace(), 1234,
                                                Pelican::ActionPoseReferenceSpace::local);
        const auto &loss = findPose(tracking_loss, "aim_left").pose;
        CHECK(loss.valid);
        CHECK(loss.orientation_tracked);
        CHECK_FALSE(loss.position_tracked);
    }

    CHECK(std::count_if(fake.calls.begin(), fake.calls.end(), [](const auto &call) {
              return call.starts_with("destroy_set:");
          }) == 2);
    CHECK(std::count(fake.calls.begin(), fake.calls.end(), "destroy_space") == 4);
}
