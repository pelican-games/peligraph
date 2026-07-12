#pragma once

#include "drawcommands.hpp"

#include <array>
#include <cstddef>
#include <vulkan/vulkan.hpp>

namespace Pelican::ui {

struct QuadVertexLayout {
    vk::VertexInputBindingDescription binding;
    std::array<vk::VertexInputAttributeDescription, 3> attributes;
};

inline QuadVertexLayout quadVertexLayout() {
    return {
        {0, sizeof(QuadVertex), vk::VertexInputRate::eVertex},
        {{{0, 0, vk::Format::eR32G32Sfloat, static_cast<std::uint32_t>(offsetof(QuadVertex, position))},
          {1, 0, vk::Format::eR32G32Sfloat, static_cast<std::uint32_t>(offsetof(QuadVertex, uv))},
          {2, 0, vk::Format::eR8G8B8A8Unorm, static_cast<std::uint32_t>(offsetof(QuadVertex, color))}}},
    };
}

} // namespace Pelican::ui
