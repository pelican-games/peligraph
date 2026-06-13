#pragma once

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct FrameRenderContext;

struct FrameTargetCaps {
    vk::Format color_format;
    vk::Extent2D extent;
    bool presents;
};

class IFrameTarget {
  public:
    virtual ~IFrameTarget() = default;
    virtual FrameRenderContext render_begin() = 0;
    virtual void render_end() = 0;
    virtual FrameTargetCaps caps() const = 0;
    virtual std::vector<uint8_t> readbackLastFrameRGBA8() = 0;
};

} // namespace Pelican
