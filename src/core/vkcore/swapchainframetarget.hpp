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
    vk::ColorSpaceKHR color_space;
    vk::Extent2D extent;
    vk::ImageUsageFlags selected_usage;
    vk::SurfaceTransformFlagBitsKHR surface_transform;
    WsiPresentConfiguration present_configuration;
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
    bool surface_stale = false;
    bool has_rendered_frame = false;

    enum class ActiveFramePhase {
        acquired,
        recording,
        submitted,
    };
    struct ActiveFrame {
        std::uint64_t serial = 0;
        std::uint32_t slot = 0;
        std::uint32_t image_index = 0;
        FrameBeginMode begin_mode =
            FrameBeginMode::blocking;
        ActiveFramePhase phase =
            ActiveFramePhase::acquired;
        bool output_transform_recorded = false;
    };
    std::optional<ActiveFrame> active_frame;
    std::uint64_t next_frame_serial = 1;
    std::shared_ptr<FrameTargetFrameCleanup>
        frame_cleanup;

    static void cleanupAbandonedFrame(
        void *owner, std::uint64_t serial) noexcept;
    void abandonFrameSerial(
        std::uint64_t serial) noexcept;
    void releaseSurfaceDependants();
    void surfaceDependantsSetup();
    void recreateSurfaceDependants();

  public:
    SwapchainFrameTarget();
    ~SwapchainFrameTarget() override;

    FrameBeginResult beginFrame(
        std::shared_ptr<const RendererRuntimeGeneration>
            runtime_generation,
        GpuSubmissionLease submission_lease,
        FrameBeginMode mode) override;
    void recordOutputTransformCopy(vk::CommandBuffer cmd_buf, vk::Image source,
                                   vk::Format source_format, vk::Extent2D source_extent) override;
    FrameSubmitResult submit(
        FrameTargetFrame frame) override;
    void abandon(FrameTargetFrame frame) noexcept override;
    FrameTargetCaps caps() const override;
    std::vector<uint8_t> readbackLastFrameRGBA8() override;
};

} // namespace Pelican
