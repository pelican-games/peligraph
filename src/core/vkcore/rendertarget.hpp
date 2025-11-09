#pragma once

#include "../container.hpp"
#include "cmdbuf.hpp"
#include <initializer_list>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct SwapchainWithFmt {
    vk::UniqueSwapchainKHR swapchain;
    vk::Format format;
};

struct FrameRenderContext {
    vk::CommandBuffer cmd_buf;
    vk::ImageView color_attachment;
    vk::Extent2D extent;
    vk::Semaphore image_prepared_semaphore;
    vk::ImageLayout required_layout;
};

constexpr size_t in_flight_frames_num = 2;

class RenderTarget {
    vk::Device device;
    vk::Queue presen_queue;
    SwapchainWithFmt swapchain;
    std::vector<vk::Image> images;
    std::vector<vk::UniqueImageView> image_views;
    std::vector<vk::UniqueSemaphore> image_acquire_semaphores, rendered_semaphores;
    std::array<CommandBufWrapper, in_flight_frames_num> render_cmd_bufs;

    vk::Extent2D extent;
    uint32_t current_image_index, in_flight_frame_index;

  public:
    RenderTarget(DependencyContainer &container);
    ~RenderTarget();

    FrameRenderContext render_begin();
    void render_end();
};

} // namespace Pelican