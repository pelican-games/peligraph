#include "openxrsession.hpp"

#include "../appflow/enginetime.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace Pelican::OpenXr {
namespace {

constexpr XrViewConfigurationType primaryViewConfiguration =
    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

XrSessionDependencyProvider dependency_provider = nullptr;

XrSessionDependencies productionDependencies() {
    if (dependency_provider == nullptr) {
        throw std::runtime_error("OpenXR session dependency provider was not configured");
    }
    return dependency_provider();
}

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

} // namespace

void setSessionDependencyProvider(XrSessionDependencyProvider provider) noexcept {
    dependency_provider = provider;
}

SessionRuntime::SessionRuntime() : SessionRuntime(productionDependencies()) {}

SessionRuntime::SessionRuntime(const XrSessionDependencies &dependencies)
    : instance{dependencies.instance}, system_id{dependencies.system_id} {
    resolve(dependencies.get_instance_proc_addr);
    create(dependencies);
}

SessionRuntime::~SessionRuntime() {
    if (tracking_space != XR_NULL_HANDLE && api.destroy_space != nullptr) {
        (void)api.destroy_space(tracking_space);
    }
    tracking_space = XR_NULL_HANDLE;
    if (session != XR_NULL_HANDLE && api.destroy_session != nullptr) {
        (void)api.destroy_session(session);
    }
    session = XR_NULL_HANDLE;
    session_running = false;
}

void SessionRuntime::resolve(PFN_xrGetInstanceProcAddr get_instance_proc_addr) {
    if (get_instance_proc_addr == nullptr) {
        throw std::runtime_error("OpenXR session dispatch has no xrGetInstanceProcAddr");
    }
    // Keep this order stable: the protocol fixture treats it as the injection
    // boundary contract shared with discovery.
    resolveRequired(get_instance_proc_addr, instance, "xrCreateSession", api.create_session);
    resolveRequired(get_instance_proc_addr, instance, "xrDestroySession", api.destroy_session);
    resolveRequired(get_instance_proc_addr, instance, "xrPollEvent", api.poll_event);
    resolveRequired(get_instance_proc_addr, instance, "xrBeginSession", api.begin_session);
    resolveRequired(get_instance_proc_addr, instance, "xrEndSession", api.end_session);
    resolveRequired(get_instance_proc_addr, instance, "xrWaitFrame", api.wait_frame);
    resolveRequired(get_instance_proc_addr, instance, "xrBeginFrame", api.begin_frame);
    resolveRequired(get_instance_proc_addr, instance, "xrEndFrame", api.end_frame);
    resolveRequired(get_instance_proc_addr, instance, "xrLocateViews", api.locate_views);
    resolveRequired(get_instance_proc_addr, instance, "xrCreateReferenceSpace",
                    api.create_reference_space);
    resolveRequired(get_instance_proc_addr, instance, "xrDestroySpace", api.destroy_space);
}

void SessionRuntime::create(const XrSessionDependencies &dependencies) {
    instance = dependencies.instance;
    system_id = dependencies.system_id;
    if (instance == XR_NULL_HANDLE || system_id == XR_NULL_SYSTEM_ID ||
        dependencies.vulkan_instance == VK_NULL_HANDLE ||
        dependencies.vulkan_physical_device == VK_NULL_HANDLE ||
        dependencies.vulkan_device == VK_NULL_HANDLE) {
        throw std::runtime_error("OpenXR session dependencies are incomplete");
    }

    XrGraphicsBindingVulkan2KHR graphics_binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    graphics_binding.instance = dependencies.vulkan_instance;
    graphics_binding.physicalDevice = dependencies.vulkan_physical_device;
    graphics_binding.device = dependencies.vulkan_device;
    graphics_binding.queueFamilyIndex = dependencies.graphics_queue_family_index;
    graphics_binding.queueIndex = dependencies.graphics_queue_index;

    XrSessionCreateInfo create_info{XR_TYPE_SESSION_CREATE_INFO};
    create_info.next = &graphics_binding;
    create_info.systemId = system_id;
    const auto create_result = api.create_session(instance, &create_info, &session);
    if (XR_FAILED(create_result) || session == XR_NULL_HANDLE) {
        session = XR_NULL_HANDLE;
        throwFailure("xrCreateSession", create_result);
    }

    // XR2a.2 owns the final STAGE/LOCAL_FLOOR/LOCAL selection policy.  XR1b
    // needs a valid base space solely to retain xrLocateViews results, so use
    // the core LOCAL space without applying any world/floor interpretation.
    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.0F;
    const auto space_result = api.create_reference_space(session, &space_info, &tracking_space);
    if (XR_FAILED(space_result) || tracking_space == XR_NULL_HANDLE) {
        tracking_space = XR_NULL_HANDLE;
        (void)api.destroy_session(session);
        session = XR_NULL_HANDLE;
        throwFailure("xrCreateReferenceSpace", space_result);
    }
}

[[noreturn]] void SessionRuntime::throwFailure(const char *operation, XrResult result) {
    throw std::runtime_error(std::string{operation} + " failed (XrResult " +
                             std::to_string(result) + ")");
}

void SessionRuntime::pollEvents() {
    while (true) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        const auto result = api.poll_event(instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) return;
        if (XR_FAILED(result)) throwFailure("xrPollEvent", result);

        if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            terminal_path = XrTerminalPath::instance_loss_pending;
            return;
        }
        if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;

        const auto &changed = reinterpret_cast<const XrEventDataSessionStateChanged &>(event);
        if (changed.session != session) continue;
        state = changed.state;

        switch (state) {
        case XR_SESSION_STATE_READY: {
            if (session_running) break;
            XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
            begin_info.primaryViewConfigurationType = primaryViewConfiguration;
            const auto begin_result = api.begin_session(session, &begin_info);
            if (XR_FAILED(begin_result)) throwFailure("xrBeginSession", begin_result);
            session_running = true;
            break;
        }
        case XR_SESSION_STATE_STOPPING: {
            if (!session_running) break;
            const auto end_result = api.end_session(session);
            // The running gate ends at the call, including an error return.
            session_running = false;
            frame_phase = FramePhase::idle;
            if (XR_FAILED(end_result)) throwFailure("xrEndSession", end_result);
            break;
        }
        case XR_SESSION_STATE_LOSS_PENDING:
            terminal_path = XrTerminalPath::loss_pending;
            return;
        case XR_SESSION_STATE_EXITING:
            terminal_path = XrTerminalPath::exiting;
            return;
        case XR_SESSION_STATE_UNKNOWN:
        case XR_SESSION_STATE_IDLE:
        case XR_SESSION_STATE_SYNCHRONIZED:
        case XR_SESSION_STATE_VISIBLE:
        case XR_SESSION_STATE_FOCUSED:
        case XR_SESSION_STATE_MAX_ENUM:
            break;
        }
    }
}

XrDisplayTiming SessionRuntime::waitFrame() {
    if (!session_running || frame_phase != FramePhase::idle) {
        throw std::logic_error("xrWaitFrame requires an idle running session");
    }
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    const auto result = api.wait_frame(session, &wait_info, &frame_state);
    if (XR_FAILED(result)) throwFailure("xrWaitFrame", result);
    frame_phase = FramePhase::waited;
    pending_display_time = frame_state.predictedDisplayTime;
    return {frame_state.predictedDisplayTime, frame_state.predictedDisplayPeriod,
            frame_state.shouldRender == XR_TRUE};
}

void SessionRuntime::beginFrame() {
    if (!session_running || frame_phase != FramePhase::waited) {
        throw std::logic_error("xrBeginFrame requires a waited running session");
    }
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    const auto result = api.begin_frame(session, &begin_info);
    if (XR_FAILED(result)) {
        frame_phase = FramePhase::idle;
        throwFailure("xrBeginFrame", result);
    }
    frame_phase = FramePhase::begun;
}

XrLocatedViews SessionRuntime::locateViews(const XrDisplayTiming &display_timing) {
    if (!session_running || frame_phase != FramePhase::begun ||
        display_timing.predictedDisplayTime() != pending_display_time) {
        throw std::logic_error("xrLocateViews requires the current begun frame timing");
    }
    if (!display_timing.shouldRender()) {
        throw std::logic_error("xrLocateViews must not run when shouldRender is false");
    }

    XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    locate_info.viewConfigurationType = primaryViewConfiguration;
    locate_info.displayTime = display_timing.predictedDisplayTime();
    locate_info.space = tracking_space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
    uint32_t view_count = 0;
    const auto result = api.locate_views(session, &locate_info, &view_state,
                                         static_cast<uint32_t>(views.size()), &view_count,
                                         views.data());
    if (XR_FAILED(result)) throwFailure("xrLocateViews", result);
    if (view_count > views.size()) {
        throw std::runtime_error("xrLocateViews returned more than two PRIMARY_STEREO views");
    }
    return {view_state.viewStateFlags,
            std::vector<XrView>{views.begin(), views.begin() + view_count}};
}

void SessionRuntime::endFrame(const XrDisplayTiming &display_timing) {
    if (!session_running || frame_phase != FramePhase::begun ||
        display_timing.predictedDisplayTime() != pending_display_time) {
        throw std::logic_error("xrEndFrame requires the current begun frame timing");
    }
    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = display_timing.predictedDisplayTime();
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount = 0;
    end_info.layers = nullptr;
    const auto result = api.end_frame(session, &end_info);
    frame_phase = FramePhase::idle;
    pending_display_time = 0;
    if (result == XR_SESSION_LOSS_PENDING || result == XR_ERROR_SESSION_LOST ||
        result == XR_ERROR_INSTANCE_LOST) {
        reportCompositionLoss(result);
        throwFailure("xrEndFrame", result);
    }
    if (XR_FAILED(result)) throwFailure("xrEndFrame", result);
}

void SessionRuntime::endFrame(const XrDisplayTiming &display_timing,
                              const XrCompositionLayerBaseHeader &layer) {
    if (!session_running || frame_phase != FramePhase::begun ||
        display_timing.predictedDisplayTime() != pending_display_time) {
        throw std::logic_error("xrEndFrame requires the current begun frame timing");
    }
    const XrCompositionLayerBaseHeader *layers[] = {&layer};
    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = display_timing.predictedDisplayTime();
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount = 1;
    end_info.layers = layers;
    const auto result = api.end_frame(session, &end_info);
    frame_phase = FramePhase::idle;
    pending_display_time = 0;
    if (result == XR_SESSION_LOSS_PENDING || result == XR_ERROR_SESSION_LOST ||
        result == XR_ERROR_INSTANCE_LOST) {
        reportCompositionLoss(result);
        throwFailure("xrEndFrame", result);
    }
    if (XR_FAILED(result)) throwFailure("xrEndFrame", result);
}

void SessionRuntime::reportCompositionLoss(XrResult result) noexcept {
    if (result == XR_ERROR_INSTANCE_LOST) {
        terminal_path = XrTerminalPath::instance_loss_pending;
    } else {
        terminal_path = XrTerminalPath::loss_pending;
    }
    session_running = false;
    frame_phase = FramePhase::idle;
    pending_display_time = 0;
}

XrFrameResult runSessionFrame(SessionRuntime &runtime, EngineTime &engine_time,
                              const std::function<void()> &update) {
    const auto display_timing = runtime.waitFrame();
    engine_time.advance();
    update();
    runtime.beginFrame();
    XrLocatedViews located_views;
    if (display_timing.shouldRender()) {
        located_views = runtime.locateViews(display_timing);
    }
    runtime.endFrame(display_timing);
    return {display_timing, std::move(located_views)};
}

} // namespace Pelican::OpenXr
