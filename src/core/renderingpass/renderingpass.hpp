#pragma once

#include "../container.hpp"
#include "../handle.hpp"

namespace Pelican {

PELICAN_DEFINE_HANDLE(RenderingPassId, int)
PELICAN_DEFINE_HANDLE(PassId, int)

PELICAN_DEFINE_HANDLE(GlobalRenderTargetId, int)

inline constexpr int invalidRenderingPassIdValue = -1;
inline constexpr int invalidRenderTargetIdValue = -1;
inline constexpr int swapchainRenderTargetIdValue = -2;

inline constexpr RenderingPassId invalidRenderingPassId() {
    return RenderingPassId{invalidRenderingPassIdValue};
}

inline constexpr bool isValidRenderingPassId(RenderingPassId pass_id) {
    return pass_id.value >= 0;
}

inline constexpr GlobalRenderTargetId noRenderTargetId() {
    return GlobalRenderTargetId{invalidRenderTargetIdValue};
}

inline constexpr GlobalRenderTargetId swapchainRenderTargetId() {
    return GlobalRenderTargetId{swapchainRenderTargetIdValue};
}

inline constexpr bool isConcreteRenderTarget(GlobalRenderTargetId rt_id) {
    return rt_id.value >= 0;
}

inline constexpr bool isSpecialRenderTarget(GlobalRenderTargetId rt_id) {
    return rt_id.value < 0;
}

inline constexpr bool isSwapchainRenderTarget(GlobalRenderTargetId rt_id) {
    return rt_id.value == swapchainRenderTargetIdValue;
}

} // namespace Pelican
