#pragma once

#include <vulkan/vulkan.hpp>

namespace Pelican {

// PSN-1: a depth attachment whose format has a stencil component is also the
// pass's stencil attachment, and every pipeline drawn into it names that
// format as its stencil attachment format. The two aspects share one layout.
inline bool vulkanFormatHasStencil(vk::Format format) noexcept {
    switch (format) {
    case vk::Format::eS8Uint:
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
        return true;
    default:
        return false;
    }
}

inline vk::ImageLayout vulkanDepthAttachmentLayout(
    vk::Format format) noexcept {
    return vulkanFormatHasStencil(format)
               ? vk::ImageLayout::eDepthStencilAttachmentOptimal
               : vk::ImageLayout::eDepthAttachmentOptimal;
}

} // namespace Pelican
