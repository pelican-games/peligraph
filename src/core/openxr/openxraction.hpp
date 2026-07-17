#pragma once

#include "../os/actionmap.hpp"
#include "../os/inputstate.hpp"
#include "openxrdiscovery.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Pelican::OpenXr {

struct XrActionApi {
    PFN_xrStringToPath string_to_path = nullptr;
    PFN_xrCreateActionSet create_action_set = nullptr;
    PFN_xrDestroyActionSet destroy_action_set = nullptr;
    PFN_xrCreateAction create_action = nullptr;
    PFN_xrDestroyAction destroy_action = nullptr;
    PFN_xrSuggestInteractionProfileBindings suggest_bindings = nullptr;
    PFN_xrAttachSessionActionSets attach_action_sets = nullptr;
    PFN_xrSyncActions sync_actions = nullptr;
    PFN_xrGetActionStateBoolean get_boolean = nullptr;
    PFN_xrGetActionStateFloat get_float = nullptr;
    PFN_xrGetActionStateVector2f get_vector2 = nullptr;
    PFN_xrGetActionStatePose get_pose = nullptr;
    PFN_xrCreateActionSpace create_action_space = nullptr;
    PFN_xrLocateSpace locate_space = nullptr;
    PFN_xrDestroySpace destroy_space = nullptr;
};

struct XrInputFrame {
    InputActionFrame actions;
    std::vector<InputPoseSample> pose_samples;
};

// Owns the project action mirror attached to one OpenXR session. All action
// sets and actions are created in the constructor before the single
// xrAttachSessionActionSets call; no runtime path can add an action later.
class XrActionRuntime {
    struct PoseSpaceRecord {
        XrPath subaction_path = XR_NULL_PATH;
        XrSpace space = XR_NULL_HANDLE;
        ActionPoseHand hand = ActionPoseHand::none;
    };

    struct ActionRecord {
        std::string name;
        InputActionType type = InputActionType::button;
        XrAction action = XR_NULL_HANDLE;
        std::vector<XrPath> subaction_paths;
        std::vector<PoseSpaceRecord> pose_spaces;
        ActionPoseHand hand = ActionPoseHand::none;
    };

    struct ActionSetRecord {
        std::string name;
        XrActionSet action_set = XR_NULL_HANDLE;
        std::vector<ActionRecord> actions;
    };

    XrActionApi api;
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    std::vector<ActionSetRecord> action_sets;
    std::unordered_map<std::string, std::size_t> set_lookup;
    InputActionFrame inactive_frame;
    std::vector<InputPoseSample> inactive_pose_samples;

    void resolve(PFN_xrGetInstanceProcAddr get_instance_proc_addr);
    void create(const InputActionMap &input_actions);
    void destroy() noexcept;
    XrPath path(std::string_view value) const;
    [[noreturn]] static void throwFailure(const std::string &operation, XrResult result);

  public:
    XrActionRuntime(PFN_xrGetInstanceProcAddr get_instance_proc_addr, XrInstance instance,
                    XrSession session, const InputActionMap &input_actions);
    ~XrActionRuntime();

    XrActionRuntime(const XrActionRuntime &) = delete;
    XrActionRuntime &operator=(const XrActionRuntime &) = delete;

    // A non-focused session deliberately yields a typed all-inactive frame and
    // does not call xrSyncActions (which runtimes may reject outside FOCUSED).
    XrInputFrame sync(const std::vector<std::string> &active_action_set_stack,
                      bool input_eligible, XrSpace base_space, XrTime display_time,
                      ActionPoseReferenceSpace reference_space);
};

} // namespace Pelican::OpenXr
