#include "renderdoccapture.hpp"

#include "../../third_party/renderdoc/renderdoc_app.h"

#include <algorithm>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace Pelican {
namespace {

constexpr std::string_view not_injected_reason = "renderdoc_not_injected";

bool isValidApiTable(const RenderDocApiTable &api) {
    return api.disable_capture_keys && api.get_num_captures && api.get_capture &&
           api.start_frame_capture && api.end_frame_capture &&
           api.discard_frame_capture && api.is_frame_capturing;
}

struct PassiveConnection {
    RenderDocApiTable api;
    std::string reason;
    std::string version;
};

PassiveConnection connectPassively() {
#ifdef _WIN32
    // Deliberately passive: never replace this with LoadLibrary. RenderDoc must
    // already have been injected before Vulkan instance creation.
    const auto module = GetModuleHandleA("renderdoc.dll");
    if (module == nullptr) {
        return {.reason = std::string{not_injected_reason}};
    }

    const auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(
        GetProcAddress(module, "RENDERDOC_GetAPI"));
    if (get_api == nullptr) {
        return {.reason = "renderdoc_get_api_missing"};
    }

    RENDERDOC_API_1_6_0 *upstream = nullptr;
    if (get_api(eRENDERDOC_API_Version_1_6_0,
                reinterpret_cast<void **>(&upstream)) != 1 ||
        upstream == nullptr) {
        return {.reason = "renderdoc_api_version_mismatch"};
    }

    int major = 0;
    int minor = 0;
    int patch = 0;
    upstream->GetAPIVersion(&major, &minor, &patch);

    PassiveConnection connection;
    connection.version = std::to_string(major) + "." + std::to_string(minor) +
                         "." + std::to_string(patch);
    connection.api.disable_capture_keys = [upstream] {
        // Pelican owns F11. Leaving RenderDoc's default binding active would
        // race TriggerCapture against the explicit Start/End frame boundary.
        upstream->SetCaptureKeys(nullptr, 0);
    };
    connection.api.get_num_captures = [upstream] {
        return upstream->GetNumCaptures();
    };
    connection.api.get_capture = [upstream](std::uint32_t index, char *filename,
                                             std::uint32_t *length,
                                             std::uint64_t *timestamp) {
        return upstream->GetCapture(index, filename, length, timestamp);
    };
    connection.api.start_frame_capture = [upstream](void *device, void *window) {
        upstream->StartFrameCapture(device, window);
    };
    connection.api.end_frame_capture = [upstream](void *device, void *window) {
        return upstream->EndFrameCapture(device, window);
    };
    connection.api.discard_frame_capture = [upstream](void *device, void *window) {
        return upstream->DiscardFrameCapture(device, window);
    };
    connection.api.is_frame_capturing = [upstream] {
        return upstream->IsFrameCapturing();
    };
    return connection;
#else
    return {.reason = "renderdoc_platform_unsupported"};
#endif
}

std::string errorMessage(std::string_view reason, std::string_view detail) {
    std::string message{reason};
    if (!detail.empty()) {
        message += ": ";
        message += detail;
    }
    return message;
}

} // namespace

std::string_view renderDocCaptureStateName(RenderDocCaptureState state) noexcept {
    switch (state) {
    case RenderDocCaptureState::unavailable:
        return "unavailable";
    case RenderDocCaptureState::idle:
        return "idle";
    case RenderDocCaptureState::armed:
        return "armed";
    case RenderDocCaptureState::capturing:
        return "capturing";
    case RenderDocCaptureState::completing:
        return "completing";
    case RenderDocCaptureState::failed:
        return "failed";
    }
    return "unknown";
}

std::string_view renderDocCaptureSourceName(RenderDocCaptureSource source) noexcept {
    switch (source) {
    case RenderDocCaptureSource::f11:
        return "f11";
    case RenderDocCaptureSource::rpc:
        return "rpc";
    }
    return "unknown";
}

RenderDocCaptureError::RenderDocCaptureError(std::string reason, std::string detail)
    : std::runtime_error(errorMessage(reason, detail)),
      failure_reason{std::move(reason)} {}

RenderDocCapture::RenderDocCapture() {
    auto connection = connectPassively();
    if (!isValidApiTable(connection.api)) {
        unavailable_reason = connection.reason.empty()
                                 ? "renderdoc_api_table_invalid"
                                 : std::move(connection.reason);
        return;
    }
    api = std::move(connection.api);
    loaded_api_version = std::move(connection.version);
    api.disable_capture_keys();
    current_state = RenderDocCaptureState::idle;
}

RenderDocCapture::RenderDocCapture(RenderDocApiTable table, std::string api_version)
    : api{std::move(table)}, loaded_api_version{std::move(api_version)} {
    if (!isValidApiTable(api)) {
        unavailable_reason = "renderdoc_api_table_invalid";
        return;
    }
    api.disable_capture_keys();
    current_state = RenderDocCaptureState::idle;
}

RenderDocCapture::RenderDocCapture(std::string unavailable_reason_for_testing)
    : unavailable_reason{std::move(unavailable_reason_for_testing)} {
    if (unavailable_reason.empty()) unavailable_reason = std::string{not_injected_reason};
}

RenderDocCaptureStatus RenderDocCapture::status() const {
    RenderDocCaptureStatus result;
    result.state = current_state;
    result.api_version = loaded_api_version;
    result.source = active_source;
    if (current_state == RenderDocCaptureState::unavailable) {
        result.status = unavailable_reason == not_injected_reason ? "absent" : "unavailable";
        result.reason = unavailable_reason;
    } else {
        result.status = std::string{renderDocCaptureStateName(current_state)};
        result.reason = last_error;
    }
    return result;
}

[[noreturn]] void RenderDocCapture::fail(std::string reason, std::string detail) {
    current_state = RenderDocCaptureState::failed;
    last_error = reason;
    active_source.reset();
    throw RenderDocCaptureError{std::move(reason), std::move(detail)};
}

void RenderDocCapture::discardActiveCapture(void *device, void *window) noexcept {
    try {
        if (api.discard_frame_capture && api.is_frame_capturing &&
            api.is_frame_capturing() != 0) {
            (void)api.discard_frame_capture(device, window);
        }
    } catch (...) {
    }
}

void RenderDocCapture::request(RenderDocCaptureSource source, bool xr_active) {
    if (xr_active) {
        throw RenderDocCaptureError{
            "capture_xr_unsupported",
            std::string{renderDocCaptureSourceName(source)} +
                " capture rejected: XR capture is outside the v1 flat/headless boundary"};
    }
    if (shutting_down) {
        throw RenderDocCaptureError{
            "capture_shutdown",
            std::string{renderDocCaptureSourceName(source)} +
                " capture rejected while engine shutdown is in progress"};
    }
    if (current_state == RenderDocCaptureState::unavailable) {
        throw RenderDocCaptureError{unavailable_reason,
                                    std::string{renderDocCaptureSourceName(source)} +
                                        " capture requires an injected RenderDoc module"};
    }
    if (current_state == RenderDocCaptureState::armed ||
        current_state == RenderDocCaptureState::capturing ||
        current_state == RenderDocCaptureState::completing) {
        const auto owner = active_source
                               ? std::string{renderDocCaptureSourceName(*active_source)}
                               : std::string{"unknown"};
        throw RenderDocCaptureError{
            "capture_busy", std::string{renderDocCaptureSourceName(source)} +
                                " capture rejected while " + owner + " capture is " +
                                std::string{renderDocCaptureStateName(current_state)}};
    }
    if (current_state == RenderDocCaptureState::failed) {
        current_state = RenderDocCaptureState::idle;
        last_error.clear();
    }
    if (api.is_frame_capturing() != 0) {
        fail("capture_state_mismatch",
             "RenderDoc reports an active capture while Pelican is idle");
    }

    capture_count_before = api.get_num_captures();
    active_source = source;
    current_state = RenderDocCaptureState::armed;
}

RenderDocCaptureResult RenderDocCapture::captureArmedFrame(
    std::uint64_t frame_index, void *device, void *window,
    const std::function<void()> &render_once) {
    if (current_state != RenderDocCaptureState::armed || !active_source) {
        throw RenderDocCaptureError{
            "capture_not_armed", "captureArmedFrame requires exactly one armed request"};
    }
    if (api.get_num_captures() != capture_count_before) {
        fail("capture_count_changed_before_start",
             "RenderDoc capture count changed after the request was armed");
    }
    if (api.is_frame_capturing() != 0) {
        fail("capture_state_mismatch",
             "RenderDoc reports an active capture before StartFrameCapture");
    }

    api.start_frame_capture(device, window);
    if (api.is_frame_capturing() == 0) {
        fail("capture_start_failed",
             "StartFrameCapture did not enter RenderDoc capture state");
    }
    current_state = RenderDocCaptureState::capturing;

    try {
        render_once();
    } catch (const std::exception &error) {
        discardActiveCapture(device, window);
        fail("capture_render_failed", error.what());
    } catch (...) {
        discardActiveCapture(device, window);
        fail("capture_render_failed", "render raised a non-standard exception");
    }

    if (api.is_frame_capturing() == 0) {
        fail("capture_state_mismatch",
             "RenderDoc stopped capture before EndFrameCapture");
    }
    current_state = RenderDocCaptureState::completing;
    if (api.end_frame_capture(device, window) == 0) {
        discardActiveCapture(device, window);
        fail("capture_end_failed", "EndFrameCapture returned zero");
    }
    if (api.is_frame_capturing() != 0) {
        discardActiveCapture(device, window);
        fail("capture_state_mismatch",
             "RenderDoc still reports an active capture after EndFrameCapture");
    }

    const auto capture_count_after = api.get_num_captures();
    if (capture_count_after <= capture_count_before) {
        fail("capture_count_not_increased",
             "EndFrameCapture succeeded but GetNumCaptures did not increase");
    }
    if (capture_count_after != capture_count_before + 1) {
        fail("capture_count_unexpected",
             "GetNumCaptures increased by more than the single requested frame");
    }
    const auto capture_index = capture_count_before;

    std::uint32_t path_length = 0;
    std::uint64_t timestamp = 0;
    if (api.get_capture(capture_index, nullptr, &path_length, &timestamp) == 0 ||
        path_length < 2) {
        fail("capture_path_unavailable",
             "GetCapture length query failed for the new capture index");
    }
    std::vector<char> path_buffer(path_length, '\0');
    auto supplied_length = path_length;
    if (api.get_capture(capture_index, path_buffer.data(), &supplied_length,
                        &timestamp) == 0 ||
        supplied_length == 0 || supplied_length > path_buffer.size() ||
        std::find(path_buffer.begin(), path_buffer.end(), '\0') == path_buffer.end()) {
        fail("capture_path_unavailable",
             "GetCapture path query failed for the new capture index");
    }

    std::filesystem::path capture_path;
    try {
        capture_path = std::filesystem::u8path(path_buffer.data());
        if (capture_path.is_relative()) {
            capture_path = std::filesystem::current_path() / capture_path;
        }
    } catch (const std::exception &error) {
        fail("capture_path_invalid", error.what());
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(capture_path, ec) || ec) {
        fail("capture_file_missing",
             "GetCapture returned a path that is not a regular file: " +
                 capture_path.generic_string());
    }
    const auto file_size = std::filesystem::file_size(capture_path, ec);
    if (ec || file_size == 0) {
        fail("capture_file_empty",
             "GetCapture returned an empty capture file: " +
                 capture_path.generic_string());
    }
    const auto canonical_path = std::filesystem::canonical(capture_path, ec);
    if (ec || !canonical_path.is_absolute()) {
        fail("capture_path_invalid",
             "capture path could not be canonicalized: " +
                 capture_path.generic_string());
    }

    RenderDocCaptureResult result{
        .path = canonical_path,
        .capture_index = capture_index,
        .frame_index = frame_index,
        .timestamp = timestamp,
    };
    current_state = RenderDocCaptureState::idle;
    active_source.reset();
    last_error.clear();
    return result;
}

void RenderDocCapture::beginShutdown() noexcept {
    shutting_down = true;
}

void *renderDocDevicePointerFromVulkanInstance(void *instance) noexcept {
    if (instance == nullptr) return nullptr;
    return RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance);
}

} // namespace Pelican
