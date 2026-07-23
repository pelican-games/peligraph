#pragma once

#include <array>
#include <cstdint>
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
    bool history = false;
    vk::ClearColorValue history_clear_color =
        vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 0.0f}};
    std::uint32_t samples = 1;
};

} // namespace Pelican
