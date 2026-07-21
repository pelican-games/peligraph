#pragma once

#include <array>
#include <vulkan/vulkan.hpp>

namespace Pelican {

inline constexpr std::array<vk::Format, 5> materialPassColorAttachmentFormatsSdr = {
    vk::Format::eB8G8R8A8Srgb,
    vk::Format::eR16G16B16A16Sfloat,
    vk::Format::eR8G8B8A8Unorm,
    vk::Format::eR16G16B16A16Sfloat,
    vk::Format::eB8G8R8A8Srgb,
};

inline constexpr std::array<vk::Format, 5> materialPassColorAttachmentFormatsHdr = {
    vk::Format::eR16G16B16A16Sfloat,
    vk::Format::eR16G16B16A16Sfloat,
    vk::Format::eR8G8B8A8Unorm,
    vk::Format::eR16G16B16A16Sfloat,
    vk::Format::eR16G16B16A16Sfloat,
};

inline constexpr const std::array<vk::Format, 5> &materialPassColorAttachmentFormats(bool hdr) {
    return hdr ? materialPassColorAttachmentFormatsHdr : materialPassColorAttachmentFormatsSdr;
}

// Routed forward materials are composed with deferred lighting before the
// single presentation tone curve.  Keep this ABI scene-linear and capable of
// preserving values above 1.0 regardless of whether the display HDR feature
// is enabled.
inline constexpr vk::Format forwardMaterialPassColorAttachmentFormat =
    vk::Format::eR16G16B16A16Sfloat;

inline constexpr vk::Format materialPassDepthAttachmentFormat = vk::Format::eD32Sfloat;

} // namespace Pelican
