#pragma once

#include "openxraction.hpp"
#include "openxrdiscovery.hpp"
#include "openxrviewspace.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
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

enum class XrBeginFrameResult {
    ready,
    discarded,
    session_loss_pending,
};

struct XrFrameResult {
    XrDisplayTiming display_timing;
    XrLocatedViews located_views;
    XrBeginFrameResult begin_result = XrBeginFrameResult::ready;
};

struct XrTimingMarginStatus {
    std::uint64_t count = 0;
    std::optional<double> minimum_ms;
    std::optional<double> median_ms;
    std::optional<double> p95_ms;
};

struct XrTimingDiagnosticStatus {
    std::uint64_t wait_frame_count = 0;
    std::uint64_t should_render_false_count = 0;
    double should_render_false_rate = 0.0;
    std::uint64_t begin_frame_discarded_count = 0;
    std::uint64_t session_loss_pending_count = 0;
    std::uint64_t mirror_presented = 0;
    std::uint64_t mirror_dropped = 0;
    std::uint64_t mirror_failures = 0;
    bool time_conversion_available = false;
    std::string_view time_conversion_reason;
    std::uint64_t time_conversion_failures = 0;
    XrTimingMarginStatus after_wait_margin;
    XrTimingMarginStatus before_submit_margin;
    XrTimingMarginStatus after_end_frame_margin;
};

struct XrDiagnosticStatus {
    std::string_view session_state;
    std::string_view view_configuration;
    XrReferenceSpaceStatus reference_space;
    std::optional<bool> should_render;
    XrTimingDiagnosticStatus timing;
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
    PFN_xrEnumerateReferenceSpaces enumerate_reference_spaces = nullptr;
    PFN_xrCreateReferenceSpace create_reference_space = nullptr;
    PFN_xrDestroySpace destroy_space = nullptr;
#ifdef _WIN32
    PFN_xrConvertTimeToWin32PerformanceCounterKHR convert_time_to_qpc = nullptr;
#endif
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
    bool win32_time_conversion_enabled = false;
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
    XrReferenceSpaceType tracking_space_type = XR_REFERENCE_SPACE_TYPE_MAX_ENUM;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    XrTerminalPath terminal_path = XrTerminalPath::none;
    std::unique_ptr<XrActionRuntime> action_runtime;
    XrLocatedViews input_located_views;
    ActionPoseReferenceSpace tracking_space_identity = ActionPoseReferenceSpace::local;
    bool session_running = false;
    FramePhase frame_phase = FramePhase::idle;
    XrTime pending_display_time = 0;
    std::optional<bool> last_should_render;
    std::uint64_t wait_frame_count = 0;
    std::uint64_t should_render_false_count = 0;
    std::uint64_t begin_frame_discarded_count = 0;
    std::uint64_t session_loss_pending_count = 0;
    std::uint64_t mirror_presented = 0;
    std::uint64_t mirror_dropped = 0;
    std::uint64_t mirror_failures = 0;
    bool time_conversion_enabled = false;
    std::uint64_t time_conversion_failures = 0;
    std::deque<double> after_wait_margins_ms;
    std::deque<double> before_submit_margins_ms;
    std::deque<double> after_end_frame_margins_ms;

    void resolve(PFN_xrGetInstanceProcAddr get_instance_proc_addr);
    void create(const XrSessionDependencies &dependencies);
    void logDiagnostic(std::string_view event, std::string_view transition) const;
    [[noreturn]] static void throwFailure(const char *operation, XrResult result);
    void recordPredictedDisplayMargin(std::deque<double> &samples,
                                      XrTime predicted_display_time) noexcept;
    void logTimingProgress() const;

  public:
    SessionRuntime();
    explicit SessionRuntime(const XrSessionDependencies &dependencies);
    ~SessionRuntime();

    SessionRuntime(const SessionRuntime &) = delete;
    SessionRuntime &operator=(const SessionRuntime &) = delete;

    void pollEvents();
    XrDisplayTiming waitFrame();
    XrBeginFrameResult beginFrame();
    XrLocatedViews locateViews(const XrDisplayTiming &display_timing);
    void endFrame(const XrDisplayTiming &display_timing);
    XrInputFrame syncActions(const std::vector<std::string> &active_action_set_stack);
    void endFrame(const XrDisplayTiming &display_timing,
                  const XrCompositionLayerBaseHeader &layer);
    void reportCompositionLoss(XrResult result) noexcept;
    void recordMirrorStatistics(std::uint64_t presented, std::uint64_t dropped,
                                std::uint64_t failures) noexcept;

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
    XrReferenceSpaceStatus referenceSpaceStatus() const;
    XrDiagnosticStatus diagnosticStatus() const;
};

// Executes the XR1b frame contract.  The callback is the existing simulation
// update; rendering and composition remain outside this work package.
XrFrameResult runSessionFrame(SessionRuntime &runtime, EngineTime &engine_time,
                              const std::function<void()> &update);

} // namespace OpenXr
} // namespace Pelican
