#pragma once

#include "rendertargetstoragemode.hpp"

#include <cstdint>
#include <string>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct RenderTargetMetadata {
    std::string name;
    vk::ImageUsageFlags usage;
    vk::Format format;
    vk::Extent2D extent;
    bool history = false;
    std::uint32_t samples = 1;
    std::uint32_t array_layers = 1;
    RenderTargetStorageMode storage_mode =
        RenderTargetStorageMode::materialized;
};

} // namespace Pelican
