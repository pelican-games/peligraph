#pragma once

#include "openxraction.hpp"
#include "openxrdiscovery.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace Pelican {

class EngineTime;

namespace OpenXr {

// A display prediction belongs to exactly one OpenXR frame.  It deliberately
// has no mutator and is not a module: simulation time remains owned by
// EngineTime and cannot be replaced with the runtime's predicted display time.
class XrDisplayTiming {
    const XrTime predicted_display_time;
    const XrDuration predicted_display_period;
    const bool should_render;

  public:
    XrDisplayTiming(XrTime display_time, XrDuration display_period, bool render) noexcept
        : predicted_display_time{display_time}, predicted_display_period{display_period},
          should_render{render} {}

    XrTime predictedDisplayTime() const noexcept { return predicted_display_time; }
    XrDuration predictedDisplayPeriod() const noexcept { return predicted_display_period; }
    bool shouldRender() const noexcept { return should_render; }
};

struct XrLocatedViews {
    XrViewStateFlags state_flags{};
    std::vector<XrView> views;
};

struct XrFrameResult {
    XrDisplayTiming display_timing;
    XrLocatedViews located_views;
};

ActionPose syntheticHeadPose(const XrLocatedViews &located_views,
                             ActionPoseReferenceSpace reference_space) noexcept;

enum class XrTerminalPath {
    none,
    loss_pending,
    exiting,
    instance_loss_pending,
};

// Session and frame commands are resolved through the discovery injection
// point.  No loader symbol is called directly after instance creation.
struct XrSessionApi {
    PFN_xrCreateSession create_session = nullptr;
    PFN_xrDestroySession destroy_session = nullptr;
    PFN_xrPollEvent poll_event = nullptr;
    PFN_xrBeginSession begin_session = nullptr;
    PFN_xrEndSession end_session = nullptr;
    PFN_xrWaitFrame wait_frame = nullptr;
    PFN_xrBeginFrame begin_frame = nullptr;
    PFN_xrEndFrame end_frame = nullptr;
    PFN_xrLocateViews locate_views = nullptr;
    PFN_xrCreateReferenceSpace create_reference_space = nullptr;
    PFN_xrDestroySpace destroy_space = nullptr;
};

struct XrSessionDependencies {
    PFN_xrGetInstanceProcAddr get_instance_proc_addr = nullptr;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    VkInstance vulkan_instance = VK_NULL_HANDLE;
    VkPhysicalDevice vulkan_physical_device = VK_NULL_HANDLE;
    VkDevice vulkan_device = VK_NULL_HANDLE;
    uint32_t graphics_queue_family_index = 0;
    uint32_t graphics_queue_index = 0;
    const InputActionMap *input_actions = nullptr;
};

using XrSessionDependencyProvider = XrSessionDependencies (*)();
void setSessionDependencyProvider(XrSessionDependencyProvider provider) noexcept;

DECLARE_MODULE(SessionRuntime) {
    enum class FramePhase {
        idle,
        waited,
        begun,
    };

    XrSessionApi api;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace tracking_space = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    XrTerminalPath terminal_path = XrTerminalPath::none;
    std::unique_ptr<XrActionRuntime> action_runtime;
    XrLocatedViews input_located_views;
    ActionPoseReferenceSpace tracking_space_identity = ActionPoseReferenceSpace::local;
    bool session_running = false;
    FramePhase frame_phase = FramePhase::idle;
    XrTime pending_display_time = 0;

    void resolve(PFN_xrGetInstanceProcAddr get_instance_proc_addr);
    void create(const XrSessionDependencies &dependencies);
    [[noreturn]] static void throwFailure(const char *operation, XrResult result);

  public:
    SessionRuntime();
    explicit SessionRuntime(const XrSessionDependencies &dependencies);
    ~SessionRuntime();

    SessionRuntime(const SessionRuntime &) = delete;
    SessionRuntime &operator=(const SessionRuntime &) = delete;

    void pollEvents();
    XrDisplayTiming waitFrame();
    void beginFrame();
    XrLocatedViews locateViews(const XrDisplayTiming &display_timing);
    void endFrame(const XrDisplayTiming &display_timing);
    XrInputFrame syncActions(const std::vector<std::string> &active_action_set_stack);
    void endFrame(const XrDisplayTiming &display_timing,
                  const XrCompositionLayerBaseHeader &layer);
    void reportCompositionLoss(XrResult result) noexcept;

    bool isSessionRunning() const noexcept { return session_running; }
    bool isInputEligible() const noexcept {
        return session_running && state == XR_SESSION_STATE_FOCUSED;
    }
    XrSessionState sessionState() const noexcept { return state; }
    XrTerminalPath terminalPath() const noexcept { return terminal_path; }
    bool hasTerminalPath() const noexcept { return terminal_path != XrTerminalPath::none; }
    XrInstance instanceHandle() const noexcept { return instance; }
    XrSystemId systemId() const noexcept { return system_id; }
    XrSession sessionHandle() const noexcept { return session; }
    XrSpace trackingSpace() const noexcept { return tracking_space; }
    ActionPoseReferenceSpace trackingSpaceIdentity() const noexcept {
        return tracking_space_identity;
    }
};

// Executes the XR1b frame contract.  The callback is the existing simulation
// update; rendering and composition remain outside this work package.
XrFrameResult runSessionFrame(SessionRuntime &runtime, EngineTime &engine_time,
                              const std::function<void()> &update);

} // namespace OpenXr
} // namespace Pelican
