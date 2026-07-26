#pragma once

#include "frametarget.hpp"
#include "image.hpp"
#include "rendertarget.hpp"

#include <array>
#include <optional>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct SwapchainWithFmt {
    vk::UniqueSwapchainKHR swapchain;
    vk::Format format;
    vk::Extent2D extent;
    bool capture_available;
};

class SwapchainFrameTarget : public IFrameTarget {
    vk::Device device;
    std::vector<vk::UniqueSemaphore> image_acquire_semaphores, rendered_semaphores;
    std::array<CommandBufWrapper, in_flight_frames_num> render_cmd_bufs;
    GpuSubmissionLeaseSlots<in_flight_frames_num>
        submission_leases;

    uint32_t current_image_index = 0;
    uint32_t in_flight_frame_index;

    // surface dependants
    vk::Extent2D extent;
    vk::Queue presen_queue;
    SwapchainWithFmt swapchain;
    std::vector<vk::Image> swapchain_images;
    std::vector<vk::UniqueImageView> swapchain_image_views;
    ImageWrapper depth_image;
    vk::UniqueImageView depth_image_view;
    bool extent_changed = false;
    bool surface_stale = false;
    bool output_transform_recorded = false;
    bool has_rendered_frame = false;
    bool current_frame_nonblocking = false;
    bool frame_acquired = false;
    bool frame_recording = false;
    bool frame_submitted = false;

    std::optional<FrameRenderContext> beginFrame(bool nonblocking);
    void releaseSurfaceDependants();
    void surfaceDependantsSetup();
    void recreateSurfaceDependants();

  public:
    SwapchainFrameTarget();
    ~SwapchainFrameTarget() override;

    FrameRenderContext render_begin() override;
    bool try_render_begin(FrameRenderContext &context) override;
    void recordOutputTransformCopy(vk::CommandBuffer cmd_buf, vk::Image source,
                                   vk::Format source_format, vk::Extent2D source_extent) override;
    void render_end(GpuSubmissionLease lease) override;
    void abort_render() noexcept override;
    FrameTargetCaps caps() const override;
    bool consumeExtentChanged() override;
    bool recoverSurfaceIfStale() override;
    std::vector<uint8_t> readbackLastFrameRGBA8() override;
};

} // namespace Pelican
