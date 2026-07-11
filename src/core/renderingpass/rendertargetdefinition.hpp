#pragma once

#include <string>
#include <optional>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct RenderTargetDefinition {
    std::string name;
    std::string format_class;
    std::string role;
    float extent_scale = 1.0f;
    std::optional<vk::Extent2D> fixed_extent;
    vk::Format format;
    vk::ImageUsageFlags usage;
};

} // namespace Pelican
