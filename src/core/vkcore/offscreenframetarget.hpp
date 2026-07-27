#pragma once

#include "frametarget.hpp"
#include "image.hpp"
#include "rendertarget.hpp"

#include <array>
#include <optional>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class OffscreenFrameTarget : public IFrameTarget {
    vk::Device device;
    std::array<CommandBufWrapper, in_flight_frames_num> render_cmd_bufs;
    GpuSubmissionLeaseSlots<in_flight_frames_num>
        submission_leases;
    uint32_t in_flight_frame_index;

    vk::Extent2D extent;
    vk::Format color_format;
    ImageWrapper color_image;
    vk::UniqueImageView color_image_view;
    ImageWrapper depth_image;
    vk::UniqueImageView depth_image_view;
    vk::ImageLayout color_layout;
    bool has_rendered_frame;

    enum class ActiveFramePhase {
        recording,
        submitted,
    };
    struct ActiveFrame {
        std::uint64_t serial = 0;
        std::uint32_t slot = 0;
        vk::ImageLayout recording_start_color_layout =
            vk::ImageLayout::eUndefined;
        bool output_transform_recorded = false;
        ActiveFramePhase phase =
            ActiveFramePhase::recording;
    };
    std::optional<ActiveFrame> active_frame;
    std::uint64_t next_frame_serial = 1;
    std::shared_ptr<FrameTargetFrameCleanup>
        frame_cleanup;

    static void cleanupAbandonedFrame(
        void *owner, std::uint64_t serial) noexcept;
    void abandonFrameSerial(
        std::uint64_t serial) noexcept;

  public:
    OffscreenFrameTarget();
    ~OffscreenFrameTarget() override;

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
