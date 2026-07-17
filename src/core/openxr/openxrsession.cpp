#include "openxrsession.hpp"

#include "../appflow/enginetime.hpp"
#include "../log.hpp"

#include <algorithm>
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

std::string_view sessionStateName(XrSessionState state) noexcept {
    switch (state) {
    case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    case XR_SESSION_STATE_MAX_ENUM: return "MAX_ENUM";
    }
    return "UNRECOGNIZED";
}

std::string_view shouldRenderName(const std::optional<bool> value) noexcept {
    if (!value) return "unknown";
    return *value ? "true" : "false";
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
    try {
        create(dependencies);
        if (dependencies.input_actions != nullptr &&
            dependencies.input_actions->actionCount() != 0) {
            action_runtime = std::make_unique<XrActionRuntime>(
                dependencies.get_instance_proc_addr, instance, session,
                *dependencies.input_actions);
        }
        logDiagnostic("configuration", "initial");
    } catch (...) {
        if (tracking_space != XR_NULL_HANDLE && api.destroy_space != nullptr) {
            (void)api.destroy_space(tracking_space);
            tracking_space = XR_NULL_HANDLE;
        }
        tracking_space_type = XR_REFERENCE_SPACE_TYPE_MAX_ENUM;
        if (session != XR_NULL_HANDLE && api.destroy_session != nullptr) {
            (void)api.destroy_session(session);
            session = XR_NULL_HANDLE;
        }
        throw;
    }
}

SessionRuntime::~SessionRuntime() {
    action_runtime.reset();
    if (tracking_space != XR_NULL_HANDLE && api.destroy_space != nullptr) {
        (void)api.destroy_space(tracking_space);
    }
    tracking_space = XR_NULL_HANDLE;
    tracking_space_type = XR_REFERENCE_SPACE_TYPE_MAX_ENUM;
    if (session != XR_NULL_HANDLE && api.destroy_session != nullptr) {
        (void)api.destroy_session(session);
    }
    session = XR_NULL_HANDLE;
    session_running = false;
}

ActionPose syntheticHeadPose(const XrLocatedViews &located_views,
                             ActionPoseReferenceSpace reference_space) noexcept {
    ActionPose pose;
    pose.source = ActionPoseSource::synthetic_head;
    pose.reference_space = reference_space;
    if (located_views.views.empty()) return pose;

    for (const auto &view : located_views.views) {
        pose.position[0] += view.pose.position.x;
        pose.position[1] += view.pose.position.y;
        pose.position[2] += view.pose.position.z;
    }
    const auto divisor = static_cast<float>(located_views.views.size());
    pose.position[0] /= divisor;
    pose.position[1] /= divisor;
    pose.position[2] /= divisor;
    const auto &orientation = located_views.views.front().pose.orientation;
    pose.orientation[0] = orientation.x;
    pose.orientation[1] = orientation.y;
    pose.orientation[2] = orientation.z;
    pose.orientation[3] = orientation.w;
    pose.orientation_valid =
        (located_views.state_flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    pose.position_valid =
        (located_views.state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    pose.orientation_tracked =
        (located_views.state_flags & XR_VIEW_STATE_ORIENTATION_TRACKED_BIT) != 0;
    pose.position_tracked =
        (located_views.state_flags & XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0;
    pose.valid = pose.orientation_valid && pose.position_valid;
    return pose;
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
    resolveRequired(get_instance_proc_addr, instance, "xrEnumerateReferenceSpaces",
                    api.enumerate_reference_spaces);
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

    uint32_t supported_space_count = 0;
    auto enumerate_result = api.enumerate_reference_spaces(
        session, 0, &supported_space_count, nullptr);
    if (XR_FAILED(enumerate_result)) {
        throwFailure("xrEnumerateReferenceSpaces", enumerate_result);
    }
    const auto supported_space_capacity = supported_space_count;
    std::vector<XrReferenceSpaceType> supported_spaces(supported_space_capacity);
    enumerate_result = api.enumerate_reference_spaces(
        session, supported_space_capacity, &supported_space_count,
        supported_spaces.data());
    if (XR_FAILED(enumerate_result)) {
        throwFailure("xrEnumerateReferenceSpaces", enumerate_result);
    }
    if (supported_space_count > supported_space_capacity) {
        throw std::runtime_error(
            "xrEnumerateReferenceSpaces returned more spaces than its reported capacity");
    }
    supported_spaces.resize(supported_space_count);
    tracking_space_type = selectReferenceSpace(supported_spaces);

    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = tracking_space_type;
    space_info.poseInReferenceSpace.orientation.w = 1.0F;
    const auto space_result = api.create_reference_space(session, &space_info, &tracking_space);
    if (XR_FAILED(space_result) || tracking_space == XR_NULL_HANDLE) {
        tracking_space = XR_NULL_HANDLE;
        tracking_space_type = XR_REFERENCE_SPACE_TYPE_MAX_ENUM;
        (void)api.destroy_session(session);
        session = XR_NULL_HANDLE;
        throwFailure("xrCreateReferenceSpace", space_result);
    }
}

XrReferenceSpaceStatus SessionRuntime::referenceSpaceStatus() const {
    if (tracking_space == XR_NULL_HANDLE) {
        throw std::logic_error("OpenXR reference-space status requires a live tracking space");
    }
    return describeReferenceSpace(tracking_space_type);
}

XrDiagnosticStatus SessionRuntime::diagnosticStatus() const {
    return {
        .session_state = sessionStateName(state),
        .view_configuration = "PRIMARY_STEREO",
        .reference_space = referenceSpaceStatus(),
        .should_render = last_should_render,
    };
}

void SessionRuntime::logDiagnostic(std::string_view event,
                                   std::string_view transition) const {
    if (logger == nullptr || tracking_space == XR_NULL_HANDLE) return;
    const auto status = diagnosticStatus();
    LOG_INFO(logger,
             "OpenXR diagnostic: event={} transition={} session_state={} view_configuration={} reference_space={} shouldRender={}",
             event, transition, status.session_state, status.view_configuration,
             status.reference_space.reference_space,
             shouldRenderName(status.should_render));
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
        const auto previous_state = state;
        state = changed.state;
        const auto transition = std::string{sessionStateName(previous_state)} + "->" +
                                std::string{sessionStateName(state)};
        logDiagnostic("session_state", transition);

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
    input_located_views = {};
    const bool should_render = frame_state.shouldRender == XR_TRUE;
    if (!last_should_render || *last_should_render != should_render) {
        const auto transition = std::string{shouldRenderName(last_should_render)} + "->" +
                                std::string{should_render ? "true" : "false"};
        last_should_render = should_render;
        logDiagnostic("shouldRender", transition);
    }
    return {frame_state.predictedDisplayTime, frame_state.predictedDisplayPeriod,
            should_render};
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
    input_located_views = {view_state.viewStateFlags,
                           std::vector<XrView>{views.begin(), views.begin() + view_count}};
    return input_located_views;
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

XrInputFrame SessionRuntime::syncActions(
    const std::vector<std::string> &active_action_set_stack) {
    if (action_runtime == nullptr) return {};
    auto frame = action_runtime->sync(active_action_set_stack, isInputEligible(), tracking_space,
                                      pending_display_time, tracking_space_identity);
    const auto head = std::find_if(frame.pose_samples.begin(), frame.pose_samples.end(),
                                   [](const auto &sample) {
                                       return sample.action_name == "head";
                                   });
    if (head != frame.pose_samples.end() && isInputEligible()) {
        head->pose = syntheticHeadPose(input_located_views, tracking_space_identity);
    }
    return frame;
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
    runtime.beginFrame();
    XrLocatedViews located_views;
    if (display_timing.shouldRender()) {
        located_views = runtime.locateViews(display_timing);
    }
    update();
    runtime.endFrame(display_timing);
    return {display_timing, std::move(located_views)};
}

} // namespace Pelican::OpenXr
