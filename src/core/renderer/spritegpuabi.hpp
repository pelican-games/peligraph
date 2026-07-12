#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan.hpp>

namespace Pelican::sprite {

struct GpuVertex {
    std::array<float, 2> corner{};
    std::array<float, 2> uv{};
    std::array<float, 16> world{};
    std::array<float, 4> color{};
    std::uint32_t billboard = 0;
};

struct GpuVertexLayout {
    vk::VertexInputBindingDescription binding;
    std::array<vk::VertexInputAttributeDescription, 8> attributes;
};

inline GpuVertexLayout gpuVertexLayout() {
    return {
        {0, sizeof(GpuVertex), vk::VertexInputRate::eVertex},
        {{{0, 0, vk::Format::eR32G32Sfloat, static_cast<std::uint32_t>(offsetof(GpuVertex, corner))},
          {1, 0, vk::Format::eR32G32Sfloat, static_cast<std::uint32_t>(offsetof(GpuVertex, uv))},
          {2, 0, vk::Format::eR32G32B32A32Sfloat, static_cast<std::uint32_t>(offsetof(GpuVertex, world) + 0)},
          {3, 0, vk::Format::eR32G32B32A32Sfloat, static_cast<std::uint32_t>(offsetof(GpuVertex, world) + 16)},
          {4, 0, vk::Format::eR32G32B32A32Sfloat, static_cast<std::uint32_t>(offsetof(GpuVertex, world) + 32)},
          {5, 0, vk::Format::eR32G32B32A32Sfloat, static_cast<std::uint32_t>(offsetof(GpuVertex, world) + 48)},
          {6, 0, vk::Format::eR32G32B32A32Sfloat, static_cast<std::uint32_t>(offsetof(GpuVertex, color))},
          {7, 0, vk::Format::eR32Uint, static_cast<std::uint32_t>(offsetof(GpuVertex, billboard))}}},
    };
}

} // namespace Pelican::sprite
