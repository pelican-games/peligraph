#pragma once

#include <string>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct RenderTargetDefinition {
    std::string name;
    vk::Extent2D extent;
    vk::Format format;
    vk::ImageUsageFlags usage;
};

} // namespace Pelican
