#pragma once

#include "../container.hpp"
#include "cmdbuf.hpp"
#include "frametarget.hpp"
#include "image.hpp"
#include <cstddef>
#include <filesystem>
#include <memory>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct FrameRenderContext {
    vk::CommandBuffer cmd_buf;
    vk::ImageView color_attachment, depth_attachment;
    vk::Extent2D extent;
    vk::Semaphore image_prepared_semaphore;
    vk::ImageLayout required_layout;
};

constexpr size_t in_flight_frames_num = 2;

DECLARE_MODULE(RenderTarget) {
    std::unique_ptr<IFrameTarget> impl;

  public:
    RenderTarget();
    ~RenderTarget();

    FrameRenderContext render_begin();
    void recordOutputTransformCopy(vk::CommandBuffer cmd_buf, vk::Image source,
                                   vk::Format source_format, vk::Extent2D source_extent);
    void render_end();

    vk::Format getSwapchainFormat() const;
    vk::Extent2D getExtent() const;
    bool consumeExtentChanged();
    std::vector<uint8_t> readbackLastFrameRGBA8();
    void captureLastFrameToPng(const std::filesystem::path &path);
};

} // namespace Pelican
