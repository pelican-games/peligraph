#pragma once

#include <string>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct RenderTargetDefinition {
    std::string name;
    float extent_scale = 1.0f;
    vk::Format format;
    vk::ImageUsageFlags usage;
};

} // namespace Pelican
