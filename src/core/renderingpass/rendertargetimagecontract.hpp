#pragma once

#include "rendertargetdefinition.hpp"
#include <span>

namespace Pelican {

// Owns the queue index: copies/moves never leave pointers into another value.
// Registration and the maintenance4 query consume createInfo() unchanged.
struct RenderTargetImageContract {
    vk::ImageCreateInfo image;
    ImageUsage usage;
    std::optional<std::uint32_t> queue_family;

    vk::ImageCreateInfo createInfo() const {
        auto result = image;
        result.queueFamilyIndexCount = queue_family ? 1u : 0u;
        result.pQueueFamilyIndices = queue_family ? &*queue_family : nullptr;
        return result;
    }
};

// attachment selects the separate MSAA attachment. Live images may be supersets.
RenderTargetImageContract renderTargetImageContract(
    const RenderTargetDefinition &definition, vk::Extent2D base_extent,
    std::uint32_t graphics_queue_family, bool attachment = false,
    const RenderTargetImageContract *live = nullptr);

} // namespace Pelican
