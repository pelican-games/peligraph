#pragma once

#include <array>
#include <vulkan/vulkan.hpp>

namespace Pelican {

inline constexpr std::array<vk::Format, 5> materialPassColorAttachmentFormats = {
    vk::Format::eB8G8R8A8Unorm,
    vk::Format::eR16G16B16A16Sfloat,
    vk::Format::eR8G8B8A8Unorm,
    vk::Format::eR16G16B16A16Sfloat,
    vk::Format::eR8G8B8A8Unorm,
};

inline constexpr vk::Format materialPassDepthAttachmentFormat = vk::Format::eD32Sfloat;

} // namespace Pelican
