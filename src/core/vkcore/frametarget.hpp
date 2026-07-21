#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct FrameRenderContext;

struct FrameTargetCaps {
    vk::Format color_format;
    vk::Extent2D extent;
    bool presents;
    bool capture_available;
    std::string color_path;
};

class IFrameTarget {
  public:
    virtual ~IFrameTarget() = default;
    virtual FrameRenderContext render_begin() = 0;
    // Optional sinks use a zero-wait begin.  false means the frame is dropped;
    // it must not be treated as a rendering failure.
    virtual bool try_render_begin(FrameRenderContext &context) = 0;
    virtual void recordOutputTransformCopy(vk::CommandBuffer cmd_buf, vk::Image source,
                                           vk::Format source_format, vk::Extent2D source_extent) = 0;
    virtual void render_end() = 0;
    virtual FrameTargetCaps caps() const = 0;
    virtual bool consumeExtentChanged() = 0;
    // Optional/nonblocking presenters can leave a window swapchain stale.
    // Recovery is an explicit, potentially blocking operation performed
    // outside the latency-critical acquire path.
    virtual bool recoverSurfaceIfStale() = 0;
    virtual std::vector<uint8_t> readbackLastFrameRGBA8() = 0;
};

} // namespace Pelican
