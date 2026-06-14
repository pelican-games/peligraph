#pragma once

#include "frametarget.hpp"
#include "image.hpp"
#include "rendertarget.hpp"

#include <array>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class OffscreenFrameTarget : public IFrameTarget {
    vk::Device device;
    std::array<CommandBufWrapper, in_flight_frames_num> render_cmd_bufs;
    uint32_t in_flight_frame_index;

    vk::Extent2D extent;
    vk::Format color_format;
    ImageWrapper color_image;
    vk::UniqueImageView color_image_view;
    ImageWrapper depth_image;
    vk::UniqueImageView depth_image_view;
    vk::ImageLayout color_layout;
    bool has_rendered_frame;

  public:
    OffscreenFrameTarget();
    ~OffscreenFrameTarget() override;

    FrameRenderContext render_begin() override;
    void render_end() override;
    FrameTargetCaps caps() const override;
    bool consumeExtentChanged() override;
    std::vector<uint8_t> readbackLastFrameRGBA8() override;
};

} // namespace Pelican
