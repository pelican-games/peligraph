#pragma once

#include <cstdint>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct FramebufferExtentSnapshot {
    vk::Extent2D extent{};
    std::uint64_t revision = 0;

    bool operator==(const FramebufferExtentSnapshot &) const = default;
};

} // namespace Pelican
