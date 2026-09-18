#pragma once

#include "openxrcompositiontarget.hpp"

#include <memory>
#include <stdexcept>
#include <string>

namespace Pelican::OpenXr {

// The layout an OpenXR swapchain image is handed back to the runtime in.
// Shared by the composition target (which passes it into the graphics config)
// and by the production bridge (which records the barriers).
constexpr vk::ImageLayout runtime_release_layout =
    vk::ImageLayout::eColorAttachmentOptimal;

class XrCompositionError final : public std::runtime_error {
    XrResult xr_result;

  public:
    XrCompositionError(const char *operation, XrResult result)
        : std::runtime_error(std::string{operation} + " failed (XrResult " +
                             std::to_string(result) + ")"),
          xr_result{result} {}

    XrResult result() const noexcept { return xr_result; }
};

inline void requireSuccess(const char *operation, XrResult result) {
    if (XR_FAILED(result) || result == XR_SESSION_LOSS_PENDING) {
        throw XrCompositionError{operation, result};
    }
}

inline bool isLossResult(XrResult result) {
    return result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST ||
           result == XR_SESSION_LOSS_PENDING;
}

// The production Vulkan bridge (images, views, command buffers, fences) lives
// in openxrcompositiongraphics.cpp so that the frame protocol in
// openxrcompositiontarget.cpp is readable on its own.
std::unique_ptr<IXrCompositionGraphics>
makeProductionGraphics(const XrCompositionDependencies &dependencies);

} // namespace Pelican::OpenXr
