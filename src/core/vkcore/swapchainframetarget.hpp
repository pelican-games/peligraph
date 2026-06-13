#pragma once

#include "frametarget.hpp"
#include "image.hpp"
#include "rendertarget.hpp"

#include <array>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct SwapchainWithFmt {
    vk::UniqueSwapchainKHR swapchain;
    vk::Format format;
    vk::Extent2D extent;
};

class SwapchainFrameTarget : public IFrameTarget {
    vk::Device device;
    std::vector<vk::UniqueSemaphore> image_acquire_semaphores, rendered_semaphores;
    std::array<CommandBufWrapper, in_flight_frames_num> render_cmd_bufs;

    uint32_t current_image_index, in_flight_frame_index;

    // surface dependants
    vk::Extent2D extent;
    vk::Queue presen_queue;
    SwapchainWithFmt swapchain;
    std::vector<vk::Image> swapchain_images;
    std::vector<vk::UniqueImageView> swapchain_image_views;
    ImageWrapper depth_image;
    vk::UniqueImageView depth_image_view;

    void releaseSurfaceDependants();
    void surfaceDependantsSetup();
    void recreateSurfaceDependants();

  public:
    SwapchainFrameTarget();
    ~SwapchainFrameTarget() override;

    FrameRenderContext render_begin() override;
    void render_end() override;
    FrameTargetCaps caps() const override;
    std::vector<uint8_t> readbackLastFrameRGBA8() override;
};

} // namespace Pelican
