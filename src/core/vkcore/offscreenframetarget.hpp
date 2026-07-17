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
    bool output_transform_recorded = false;

  public:
    OffscreenFrameTarget();
    ~OffscreenFrameTarget() override;

    FrameRenderContext render_begin() override;
    bool try_render_begin(FrameRenderContext &context) override;
    void recordOutputTransformCopy(vk::CommandBuffer cmd_buf, vk::Image source,
                                   vk::Format source_format, vk::Extent2D source_extent) override;
    void render_end() override;
    FrameTargetCaps caps() const override;
    bool consumeExtentChanged() override;
    std::vector<uint8_t> readbackLastFrameRGBA8() override;
};

} // namespace Pelican
