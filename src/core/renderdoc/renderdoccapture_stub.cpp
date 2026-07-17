#include "renderdoccapture.hpp"

#include <utility>

namespace Pelican {

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
    return source == RenderDocCaptureSource::f11 ? "f11" : "rpc";
}

RenderDocCaptureError::RenderDocCaptureError(std::string reason, std::string detail)
    : std::runtime_error(reason + (detail.empty() ? "" : ": " + detail)),
      failure_reason{std::move(reason)} {}

RenderDocCapture::RenderDocCapture()
    : unavailable_reason{"renderdoc_build_disabled"} {}

RenderDocCapture::RenderDocCapture(RenderDocApiTable, std::string)
    : unavailable_reason{"renderdoc_build_disabled"} {}

RenderDocCapture::RenderDocCapture(std::string)
    : unavailable_reason{"renderdoc_build_disabled"} {}

RenderDocCaptureStatus RenderDocCapture::status() const {
    return {
        .status = "disabled",
        .state = RenderDocCaptureState::unavailable,
        .reason = unavailable_reason,
    };
}

[[noreturn]] void RenderDocCapture::fail(std::string reason, std::string detail) {
    throw RenderDocCaptureError{std::move(reason), std::move(detail)};
}

void RenderDocCapture::discardActiveCapture(void *, void *) noexcept {}

void RenderDocCapture::request(RenderDocCaptureSource source, bool xr_active) {
    if (xr_active) {
        throw RenderDocCaptureError{"capture_xr_unsupported",
                                    std::string{renderDocCaptureSourceName(source)}};
    }
    throw RenderDocCaptureError{"renderdoc_build_disabled",
                                "This binary was built with PELICAN_WITH_RENDERDOC=OFF"};
}

RenderDocCaptureResult RenderDocCapture::captureArmedFrame(
    std::uint64_t, void *, void *, const std::function<void()> &) {
    throw RenderDocCaptureError{"renderdoc_build_disabled",
                                "This binary was built with PELICAN_WITH_RENDERDOC=OFF"};
}

void RenderDocCapture::beginShutdown() noexcept {
    shutting_down = true;
}

void *renderDocDevicePointerFromVulkanInstance(void *) noexcept {
    return nullptr;
}

} // namespace Pelican
