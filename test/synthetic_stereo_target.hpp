#pragma once

#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/vkcore/util.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace Pelican::Test {

class VulkanSyntheticStereoTarget final : public ILogicalFrameTarget {
    static constexpr std::uint32_t stereo_view_count = 2;

    vk::Device device;
    vk::Queue queue;
    vk::Extent2D extent;
    vk::Format format;
    std::vector<CommandBufWrapper> command_buffers;
    std::array<ImageWrapper, stereo_view_count> color_images;
    std::array<vk::UniqueImageView, stereo_view_count> color_views;
    std::array<vk::ImageLayout, stereo_view_count> color_layouts{
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eUndefined,
    };
    std::array<bool, stereo_view_count> view_begun{};
    std::array<bool, stereo_view_count> view_ended{};
    std::uint32_t in_flight_frame_index = 0;
    bool logical_frame_begun = false;
    std::uint64_t logical_begin_count = 0;
    std::uint64_t logical_end_count = 0;
    std::uint64_t view_begin_count = 0;
    std::uint64_t view_end_count = 0;
    std::uint64_t submission_count = 0;

    static vk::UniqueImageView createColorView(vk::Device device,
                                                const ImageWrapper &image) {
        vk::ImageViewCreateInfo info;
        info.image = image.image.get();
        info.viewType = vk::ImageViewType::e2D;
        info.format = image.format;
        info.components = {
            vk::ComponentSwizzle::eR,
            vk::ComponentSwizzle::eG,
            vk::ComponentSwizzle::eB,
            vk::ComponentSwizzle::eA,
        };
        info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        return device.createImageViewUnique(info);
    }

    static VulkanUtils::ChangeImageLayoutInfo transitionInfo(
        vk::ImageLayout old_layout, vk::ImageLayout new_layout) {
        VulkanUtils::ChangeImageLayoutInfo result{
            .src_stage = vk::PipelineStageFlagBits::eTopOfPipe,
            .dst_stage = vk::PipelineStageFlagBits::eTopOfPipe,
            .src_access = {},
            .dst_access = {},
        };
        if (old_layout == vk::ImageLayout::eColorAttachmentOptimal) {
            result.src_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
            result.src_access = vk::AccessFlagBits::eColorAttachmentRead |
                                vk::AccessFlagBits::eColorAttachmentWrite;
        } else if (old_layout == vk::ImageLayout::eTransferSrcOptimal) {
            result.src_stage = vk::PipelineStageFlagBits::eTransfer;
            result.src_access = vk::AccessFlagBits::eTransferRead;
        }
        if (new_layout == vk::ImageLayout::eColorAttachmentOptimal) {
            result.dst_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
            result.dst_access = vk::AccessFlagBits::eColorAttachmentRead |
                                vk::AccessFlagBits::eColorAttachmentWrite;
        } else if (new_layout == vk::ImageLayout::eTransferSrcOptimal) {
            result.dst_stage = vk::PipelineStageFlagBits::eTransfer;
            result.dst_access = vk::AccessFlagBits::eTransferRead;
        }
        return result;
    }

    CommandBufWrapper &command(std::uint32_t view_index) {
        return command_buffers.at(static_cast<std::size_t>(in_flight_frame_index) *
                                      stereo_view_count +
                                  view_index);
    }

  public:
    VulkanSyntheticStereoTarget(vk::Extent2D extent, vk::Format format)
        : device{GET_MODULE(VulkanManageCore).getDevice()},
          queue{GET_MODULE(VulkanManageCore).getGraphicsQueue()}, extent{extent},
          format{format},
          command_buffers{GET_MODULE(VulkanManageCore).allocCmdBufs(
              in_flight_frames_num * stereo_view_count)} {
        if (format != vk::Format::eR8G8B8A8Srgb &&
            format != vk::Format::eR8G8B8A8Unorm) {
            throw std::runtime_error(
                "synthetic stereo target requires an RGBA8 frame-target format");
        }
        const vk::Extent3D image_extent{extent.width, extent.height, 1};
        for (std::uint32_t view = 0; view < stereo_view_count; ++view) {
            color_images[view] = GET_MODULE(VulkanManageCore).allocImage(
                image_extent, format,
                vk::ImageUsageFlagBits::eColorAttachment |
                    vk::ImageUsageFlagBits::eTransferSrc,
                vma::MemoryUsage::eAutoPreferDevice, {});
            color_views[view] = createColorView(device, color_images[view]);
        }
    }

    ~VulkanSyntheticStereoTarget() override {
        if (device) {
            device.waitIdle();
        }
    }

    void beginLogicalFrame(
        std::uint32_t view_count,
        LogicalFrameRuntime = {}) override {
        if (logical_frame_begun || view_count != stereo_view_count) {
            throw std::runtime_error(
                "synthetic stereo target requires one non-nested two-view logical frame");
        }
        const auto &logical_fence = command(0).getFence();
        if (device.waitForFences({logical_fence}, VK_TRUE, UINT64_MAX) !=
            vk::Result::eSuccess) {
            throw std::runtime_error("synthetic stereo target fence wait failed");
        }
        view_begun.fill(false);
        view_ended.fill(false);
        logical_frame_begun = true;
        ++logical_begin_count;
    }

    FrameRenderContext beginView(std::uint32_t view_index) override {
        if (!logical_frame_begun || view_index >= stereo_view_count ||
            view_begun[view_index]) {
            throw std::runtime_error("synthetic stereo target view begin is out of order");
        }
        auto &cmd = command(view_index);
        cmd.recordBegin();
        GET_MODULE(VulkanUtils).changeImageLayoutCmd(
            *cmd, color_images[view_index], color_layouts[view_index],
            vk::ImageLayout::eColorAttachmentOptimal,
            transitionInfo(color_layouts[view_index],
                           vk::ImageLayout::eColorAttachmentOptimal));
        color_layouts[view_index] = vk::ImageLayout::eColorAttachmentOptimal;
        view_begun[view_index] = true;
        ++view_begin_count;
        return FrameRenderContext{
            .cmd_buf = *cmd,
            .color_image = color_images[view_index].image.get(),
            .color_attachment = color_views[view_index].get(),
            .depth_attachment = {},
            .extent = extent,
            .image_prepared_semaphore = {},
            .required_layout = vk::ImageLayout::eTransferSrcOptimal,
            .in_flight_frame_index = in_flight_frame_index,
        };
    }

    void endView(
        std::uint32_t view_index,
        GpuSubmissionLease) override {
        if (!logical_frame_begun || view_index >= stereo_view_count ||
            !view_begun[view_index] || view_ended[view_index]) {
            throw std::runtime_error("synthetic stereo target view end is out of order");
        }
        auto &cmd = command(view_index);
        GET_MODULE(VulkanUtils).changeImageLayoutCmd(
            *cmd, color_images[view_index], color_layouts[view_index],
            vk::ImageLayout::eTransferSrcOptimal,
            transitionInfo(color_layouts[view_index],
                           vk::ImageLayout::eTransferSrcOptimal));
        color_layouts[view_index] = vk::ImageLayout::eTransferSrcOptimal;
        cmd.recordEnd();
        view_ended[view_index] = true;
        ++view_end_count;
    }

    void endLogicalFrame(GpuSubmissionLease lease) override {
        // This fixture waits synchronously below; the parameter keeps the
        // submitted generation alive through that wait.
        (void)lease;
        if (!logical_frame_begun || !view_ended[0] || !view_ended[1]) {
            throw std::runtime_error(
                "synthetic stereo target logical frame ended before both views");
        }
        const std::array command_handles{*command(0), *command(1)};
        vk::SubmitInfo submit;
        submit.setCommandBuffers(command_handles);
        const auto &logical_fence = command(0).getFence();
        device.resetFences({logical_fence});
        queue.submit({submit}, logical_fence);
        ++submission_count;
        if (device.waitForFences({logical_fence}, VK_TRUE, UINT64_MAX) !=
            vk::Result::eSuccess) {
            throw std::runtime_error("synthetic stereo target submit wait failed");
        }
        logical_frame_begun = false;
        ++logical_end_count;
        in_flight_frame_index =
            (in_flight_frame_index + 1) % in_flight_frames_num;
    }

    void abortLogicalFrame() noexcept override {
        if (!logical_frame_begun) return;
        for (std::uint32_t view = 0; view < stereo_view_count; ++view) {
            if (view_begun[view] && !view_ended[view]) {
                command(view).abortRecording();
            }
        }
        logical_frame_begun = false;
    }

    vk::Format colorFormat(std::uint32_t view_index) const override {
        if (view_index >= stereo_view_count) {
            throw std::runtime_error(
                "synthetic stereo target color format view is out of range");
        }
        return format;
    }


    std::vector<std::uint8_t> readback(std::uint32_t view_index) const {
        if (view_index >= stereo_view_count ||
            color_layouts[view_index] != vk::ImageLayout::eTransferSrcOptimal) {
            throw std::runtime_error(
                "synthetic stereo target view is not ready for readback");
        }
        const auto byte_count =
            static_cast<vk::DeviceSize>(extent.width) * extent.height * 4;
        auto staging = GET_MODULE(VulkanManageCore).allocBuf(
            byte_count, vk::BufferUsageFlagBits::eTransferDst,
            vma::MemoryUsage::eAutoPreferHost,
            vma::AllocationCreateFlagBits::eHostAccessRandom);
        vk::BufferImageCopy copy;
        copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        copy.imageExtent = vk::Extent3D{extent.width, extent.height, 1};
        GET_MODULE(VulkanUtils).executeOneTimeCmd(
            [&](vk::CommandBuffer cmd) {
                cmd.copyImageToBuffer(color_images[view_index].image.get(),
                                      vk::ImageLayout::eTransferSrcOptimal,
                                      staging.buffer.get(), {copy});
            },
            true);
        return GET_MODULE(VulkanManageCore).readBuf(staging, byte_count);
    }

    std::uint64_t logicalBeginCount() const { return logical_begin_count; }
    std::uint64_t logicalEndCount() const { return logical_end_count; }
    std::uint64_t viewBeginCount() const { return view_begin_count; }
    std::uint64_t viewEndCount() const { return view_end_count; }
    std::uint64_t submissionCount() const { return submission_count; }
};

class VulkanSyntheticViewFamilyTarget final
    : public ILogicalFrameTarget {
    static constexpr std::uint32_t stereo_view_count = 2;

    vk::Device device;
    vk::Queue queue;
    vk::Extent2D extent;
    vk::Format format;
    std::vector<CommandBufWrapper> command_buffers;
    ImageWrapper color_image;
    vk::UniqueImageView color_array_view;
    std::array<vk::UniqueImageView, stereo_view_count>
        color_layer_views;
    vk::ImageLayout color_layout =
        vk::ImageLayout::eUndefined;
    std::uint32_t in_flight_frame_index = 0;
    bool logical_frame_begun = false;
    bool view_family_begun = false;
    bool view_family_ended = false;
    std::uint64_t logical_begin_count = 0;
    std::uint64_t logical_end_count = 0;
    std::uint64_t view_family_begin_count = 0;
    std::uint64_t view_family_end_count = 0;
    std::uint64_t submission_count = 0;

    static vk::UniqueImageView createColorView(
        vk::Device device, const ImageWrapper &image,
        vk::ImageViewType type,
        std::uint32_t base_layer,
        std::uint32_t layer_count) {
        if (layer_count == 0 ||
            base_layer >= image.array_layers ||
            layer_count >
                image.array_layers - base_layer) {
            throw std::runtime_error(
                "synthetic view-family target image view range is invalid");
        }
        vk::ImageViewCreateInfo info;
        info.image = image.image.get();
        info.viewType = type;
        info.format = image.format;
        info.components = {
            vk::ComponentSwizzle::eR,
            vk::ComponentSwizzle::eG,
            vk::ComponentSwizzle::eB,
            vk::ComponentSwizzle::eA,
        };
        info.subresourceRange = {
            vk::ImageAspectFlagBits::eColor,
            0,
            image.mip_levels,
            base_layer,
            layer_count,
        };
        return device.createImageViewUnique(info);
    }

    static VulkanUtils::ChangeImageLayoutInfo
    transitionInfo(
        vk::ImageLayout old_layout,
        vk::ImageLayout new_layout) {
        VulkanUtils::ChangeImageLayoutInfo result{
            .src_stage =
                vk::PipelineStageFlagBits::eTopOfPipe,
            .dst_stage =
                vk::PipelineStageFlagBits::eTopOfPipe,
            .src_access = {},
            .dst_access = {},
        };
        if (old_layout ==
            vk::ImageLayout::eColorAttachmentOptimal) {
            result.src_stage =
                vk::PipelineStageFlagBits::
                    eColorAttachmentOutput;
            result.src_access =
                vk::AccessFlagBits::
                    eColorAttachmentRead |
                vk::AccessFlagBits::
                    eColorAttachmentWrite;
        } else if (
            old_layout ==
            vk::ImageLayout::eTransferSrcOptimal) {
            result.src_stage =
                vk::PipelineStageFlagBits::eTransfer;
            result.src_access =
                vk::AccessFlagBits::eTransferRead;
        }
        if (new_layout ==
            vk::ImageLayout::eColorAttachmentOptimal) {
            result.dst_stage =
                vk::PipelineStageFlagBits::
                    eColorAttachmentOutput;
            result.dst_access =
                vk::AccessFlagBits::
                    eColorAttachmentRead |
                vk::AccessFlagBits::
                    eColorAttachmentWrite;
        } else if (
            new_layout ==
            vk::ImageLayout::eTransferSrcOptimal) {
            result.dst_stage =
                vk::PipelineStageFlagBits::eTransfer;
            result.dst_access =
                vk::AccessFlagBits::eTransferRead;
        }
        return result;
    }

    CommandBufWrapper &command() {
        return command_buffers.at(
            in_flight_frame_index);
    }

  public:
    VulkanSyntheticViewFamilyTarget(
        vk::Extent2D extent, vk::Format format)
        : device{
              GET_MODULE(VulkanManageCore)
                  .getDevice()},
          queue{
              GET_MODULE(VulkanManageCore)
                  .getGraphicsQueue()},
          extent{extent}, format{format},
          command_buffers{
              GET_MODULE(VulkanManageCore)
                  .allocCmdBufs(
                      in_flight_frames_num)},
          color_image{
              GET_MODULE(VulkanManageCore)
                  .allocImage(
                      {extent.width,
                       extent.height, 1},
                      format,
                      vk::ImageUsageFlagBits::
                              eColorAttachment |
                          vk::ImageUsageFlagBits::
                              eTransferSrc,
                      vma::MemoryUsage::
                          eAutoPreferDevice,
                      {},
                      VulkanProcessType::graphics,
                      {}, 1,
                      vk::SampleCountFlagBits::e1,
                      stereo_view_count)} {
        if (format !=
                vk::Format::eR8G8B8A8Srgb &&
            format !=
                vk::Format::eR8G8B8A8Unorm) {
            throw std::runtime_error(
                "synthetic view-family target requires an RGBA8 frame-target format");
        }
        color_array_view = createColorView(
            device, color_image,
            vk::ImageViewType::e2DArray, 0,
            stereo_view_count);
        for (std::uint32_t view = 0;
             view < stereo_view_count; ++view) {
            color_layer_views[view] =
                createColorView(
                    device, color_image,
                    vk::ImageViewType::e2D,
                    view, 1);
        }
    }

    ~VulkanSyntheticViewFamilyTarget() override {
        if (device) {
            device.waitIdle();
        }
    }

    void beginLogicalFrame(
        std::uint32_t view_count,
        LogicalFrameRuntime = {}) override {
        if (logical_frame_begun ||
            view_count != stereo_view_count) {
            throw std::runtime_error(
                "synthetic view-family target requires one non-nested two-view logical frame");
        }
        const auto &fence = command().getFence();
        if (device.waitForFences(
                {fence}, VK_TRUE, UINT64_MAX) !=
            vk::Result::eSuccess) {
            throw std::runtime_error(
                "synthetic view-family target fence wait failed");
        }
        view_family_begun = false;
        view_family_ended = false;
        logical_frame_begun = true;
        ++logical_begin_count;
    }

    FrameRenderContext beginView(
        std::uint32_t) override {
        throw std::runtime_error(
            "synthetic view-family target requires beginViewFamily");
    }

    bool supportsViewFamilyExecution()
        const noexcept override {
        return true;
    }

    FrameRenderContext beginViewFamily(
        std::uint32_t view_count) override {
        if (!logical_frame_begun ||
            view_family_begun ||
            view_count != stereo_view_count) {
            throw std::runtime_error(
                "synthetic view-family target family begin is out of order");
        }
        auto &cmd = command();
        cmd.recordBegin();
        GET_MODULE(VulkanUtils)
            .changeImageLayoutCmd(
                *cmd, color_image,
                color_layout,
                vk::ImageLayout::
                    eColorAttachmentOptimal,
                transitionInfo(
                    color_layout,
                    vk::ImageLayout::
                        eColorAttachmentOptimal));
        color_layout =
            vk::ImageLayout::
                eColorAttachmentOptimal;
        view_family_begun = true;
        ++view_family_begin_count;
        return FrameRenderContext{
            .cmd_buf = *cmd,
            .color_image =
                color_image.image.get(),
            .color_attachment =
                color_array_view.get(),
            .depth_attachment = {},
            .color_layer_attachments = {
                color_layer_views[0].get(),
                color_layer_views[1].get(),
            },
            .color_base_array_layer = 0,
            .color_array_layers =
                stereo_view_count,
            .extent = extent,
            .image_prepared_semaphore = {},
            .required_layout =
                vk::ImageLayout::
                    eTransferSrcOptimal,
            .in_flight_frame_index =
                in_flight_frame_index,
        };
    }

    void endView(
        std::uint32_t,
        GpuSubmissionLease) override {
        throw std::runtime_error(
            "synthetic view-family target requires endViewFamily");
    }

    void endViewFamily(
        GpuSubmissionLease) override {
        if (!logical_frame_begun ||
            !view_family_begun ||
            view_family_ended) {
            throw std::runtime_error(
                "synthetic view-family target family end is out of order");
        }
        auto &cmd = command();
        GET_MODULE(VulkanUtils)
            .changeImageLayoutCmd(
                *cmd, color_image,
                color_layout,
                vk::ImageLayout::
                    eTransferSrcOptimal,
                transitionInfo(
                    color_layout,
                    vk::ImageLayout::
                        eTransferSrcOptimal));
        color_layout =
            vk::ImageLayout::
                eTransferSrcOptimal;
        cmd.recordEnd();
        view_family_ended = true;
        ++view_family_end_count;
    }

    void endLogicalFrame(
        GpuSubmissionLease lease) override {
        // The fixture waits synchronously, so retaining the lease
        // past this function is unnecessary.
        (void)lease;
        if (!logical_frame_begun ||
            !view_family_ended) {
            throw std::runtime_error(
                "synthetic view-family target logical frame ended before its view family");
        }
        const std::array command_handles{
            *command()};
        vk::SubmitInfo submit;
        submit.setCommandBuffers(
            command_handles);
        const auto &fence =
            command().getFence();
        device.resetFences({fence});
        queue.submit({submit}, fence);
        ++submission_count;
        if (device.waitForFences(
                {fence}, VK_TRUE,
                UINT64_MAX) !=
            vk::Result::eSuccess) {
            throw std::runtime_error(
                "synthetic view-family target submit wait failed");
        }
        logical_frame_begun = false;
        ++logical_end_count;
        in_flight_frame_index =
            (in_flight_frame_index + 1) %
            in_flight_frames_num;
    }

    void abortLogicalFrame()
        noexcept override {
        if (!logical_frame_begun) {
            return;
        }
        if (view_family_begun &&
            !view_family_ended) {
            command().abortRecording();
        }
        logical_frame_begun = false;
    }

    vk::Format colorFormat(
        std::uint32_t view_index)
        const override {
        if (view_index >=
            stereo_view_count) {
            throw std::runtime_error(
                "synthetic view-family target color format view is out of range");
        }
        return format;
    }

    std::vector<std::uint8_t> readback(
        std::uint32_t view_index) const {
        if (view_index >=
                stereo_view_count ||
            color_layout !=
                vk::ImageLayout::
                    eTransferSrcOptimal) {
            throw std::runtime_error(
                "synthetic view-family target view is not ready for readback");
        }
        const auto byte_count =
            static_cast<vk::DeviceSize>(
                extent.width) *
            extent.height * 4;
        auto staging =
            GET_MODULE(VulkanManageCore)
                .allocBuf(
                    byte_count,
                    vk::BufferUsageFlagBits::
                        eTransferDst,
                    vma::MemoryUsage::
                        eAutoPreferHost,
                    vma::AllocationCreateFlagBits::
                        eHostAccessRandom);
        vk::BufferImageCopy copy;
        copy.imageSubresource = {
            vk::ImageAspectFlagBits::eColor,
            0,
            view_index,
            1,
        };
        copy.imageExtent = vk::Extent3D{
            extent.width, extent.height, 1};
        GET_MODULE(VulkanUtils)
            .executeOneTimeCmd(
                [&](vk::CommandBuffer cmd) {
                    cmd.copyImageToBuffer(
                        color_image.image.get(),
                        vk::ImageLayout::
                            eTransferSrcOptimal,
                        staging.buffer.get(),
                        {copy});
                },
                true);
        return GET_MODULE(VulkanManageCore)
            .readBuf(staging, byte_count);
    }

    std::uint64_t logicalBeginCount()
        const {
        return logical_begin_count;
    }
    std::uint64_t logicalEndCount()
        const {
        return logical_end_count;
    }
    std::uint64_t viewFamilyBeginCount()
        const {
        return view_family_begin_count;
    }
    std::uint64_t viewFamilyEndCount()
        const {
        return view_family_end_count;
    }
    std::uint64_t submissionCount()
        const {
        return submission_count;
    }
};

} // namespace Pelican::Test
