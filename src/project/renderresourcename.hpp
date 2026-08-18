#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican {

inline constexpr std::string_view reservedSwapchainResourceName =
    "swapchain";
inline constexpr std::string_view reservedSwapchainNameViolation =
    "reserved_swapchain";

// Authored images and buffers share one logical resource namespace. The
// built-in frame target owns "swapchain", so no authored resource kind may
// claim that name.
inline void validateAuthoredRenderResourceName(
    std::string_view name, std::string_view resource_kind) {
    if (name != reservedSwapchainResourceName) {
        return;
    }
    throw std::runtime_error(
        "Render resource name violation '" +
        std::string{reservedSwapchainNameViolation} + "': " +
        std::string{resource_kind} +
        " name is reserved: " + std::string{name});
}

} // namespace Pelican
