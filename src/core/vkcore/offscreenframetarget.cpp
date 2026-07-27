#include "offscreenframetarget.hpp"

#include "../launchconfig.hpp"
#include "../log.hpp"
#include "../renderer/camera.hpp"
#include "core.hpp"
#include "util.hpp"

#include <cassert>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

template <class T, size_t N> static std::array<T, N> vectorToArray(std::vector<T> v) {
    std::array<T, N> arr;
    assert(N == v.size());
    for (int i = 0; i < N; i++) {
        arr[i] = std::move(v[i]);
    }
    return arr;
}

vk::UniqueImageView createImageView(vk::Device device, const ImageWrapper &image, vk::ImageAspectFlags aspect) {
    vk::ImageViewCreateInfo create_info;
    create_info.image = image.image.get();
    create_info.viewType = vk::ImageViewType::e2D;
    create_info.format = image.format;
    create_info.components.r = vk::ComponentSwizzle::eR;
    create_info.components.g = vk::ComponentSwizzle::eG;
    create_info.components.b = vk::ComponentSwizzle::eB;
    create_info.components.a = vk::ComponentSwizzle::eA;
    create_info.subresourceRange.aspectMask = aspect;
    create_info.subresourceRange.baseMipLevel = 0;
    create_info.subresourceRange.levelCount = 1;
    create_info.subresourceRange.baseArrayLayer = 0;
    create_info.subresourceRange.layerCount = 1;

    return device.createImageViewUnique(create_info);
}

VulkanUtils::ChangeImageLayoutInfo transitionInfo(vk::ImageLayout old_layout, vk::ImageLayout new_layout) {
    VulkanUtils::ChangeImageLayoutInfo info{
        .src_stage = vk::PipelineStageFlagBits::eTopOfPipe,
        .dst_stage = vk::PipelineStageFlagBits::eTopOfPipe,
        .src_access = {},
        .dst_access = {},
    };

    if (old_layout == vk::ImageLayout::eColorAttachmentOptimal) {
        info.src_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        info.src_access = vk::AccessFlagBits::eColorAttachmentRead |
                          vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (old_layout == vk::ImageLayout::eTransferSrcOptimal) {
        info.src_stage = vk::PipelineStageFlagBits::eTransfer;
        info.src_access = vk::AccessFlagBits::eTransferRead;
    } else if (old_layout == vk::ImageLayout::eTransferDstOptimal) {
        info.src_stage = vk::PipelineStageFlagBits::eTransfer;
        info.src_access = vk::AccessFlagBits::eTransferWrite;
    }

    if (new_layout == vk::ImageLayout::eColorAttachmentOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        info.dst_access = vk::AccessFlagBits::eColorAttachmentRead |
                          vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (new_layout == vk::ImageLayout::eTransferSrcOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eTransfer;
        info.dst_access = vk::AccessFlagBits::eTransferRead;
    } else if (new_layout == vk::ImageLayout::eTransferDstOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eTransfer;
        info.dst_access = vk::AccessFlagBits::eTransferWrite;
    }

    return info;
}

void setViewportAndScissor(vk::CommandBuffer cmd_buf, vk::Extent2D extent) {
    vk::Viewport viewport;
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    cmd_buf.setViewport(0, {viewport});

    vk::Rect2D scissor;
    scissor.offset = vk::Offset2D{0, 0};
    scissor.extent = extent;
    cmd_buf.setScissor(0, {scissor});
}

} // namespace

OffscreenFrameTarget::OffscreenFrameTarget()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      render_cmd_bufs{vectorToArray<CommandBufWrapper, in_flight_frames_num>(
          GET_MODULE(VulkanManageCore).allocCmdBufs(in_flight_frames_num))},
      in_flight_frame_index{0}, extent{GET_MODULE(EngineLaunchConfig).headless_extent},
      color_format{vk::Format::eR8G8B8A8Srgb}, color_layout{vk::ImageLayout::eUndefined},
      has_rendered_frame{false} {
    frame_cleanup =
        std::make_shared<FrameTargetFrameCleanup>(
            this, &OffscreenFrameTarget::
                      cleanupAbandonedFrame);
    auto &vkcore = GET_MODULE(VulkanManageCore);
    auto format_features = vkcore.getPhysDevice().getFormatProperties(color_format).optimalTilingFeatures;
    const auto required_features = vk::FormatFeatureFlagBits::eColorAttachment |
                                   vk::FormatFeatureFlagBits::eTransferSrc;
    if (GET_MODULE(EngineLaunchConfig).force_unorm_color_path_for_testing ||
        (format_features & required_features) != required_features) {
        color_format = vk::Format::eR8G8B8A8Unorm;
        format_features = vkcore.getPhysDevice().getFormatProperties(color_format).optimalTilingFeatures;
        if ((format_features & required_features) != required_features) {
            throw std::runtime_error(
                "offscreen frame target lacks COLOR_ATTACHMENT/TRANSFER_SRC support");
        }
    }
    const vk::Extent3D image_extent{extent.width, extent.height, 1};

    color_image = vkcore.allocImage(image_extent, color_format,
                                    vk::ImageUsageFlagBits::eColorAttachment |
                                        vk::ImageUsageFlagBits::eTransferSrc,
                                    vma::MemoryUsage::eAutoPreferDevice, {});
    color_image_view = createImageView(device, color_image, vk::ImageAspectFlagBits::eColor);

    depth_image = vkcore.allocImage(image_extent, vk::Format::eD32Sfloat,
                                    vk::ImageUsageFlagBits::eDepthStencilAttachment,
                                    vma::MemoryUsage::eAutoPreferDevice, {});
    depth_image_view = createImageView(device, depth_image, vk::ImageAspectFlagBits::eDepth);

    const auto &debug_utils = vkcore.getDebugUtils();
    debug_utils.nameImage(color_image.image.get(), "offscreen/color/image");
    debug_utils.nameImageView(color_image_view.get(), "offscreen/color/view");
    debug_utils.nameImage(depth_image.image.get(), "offscreen/depth/image");
    debug_utils.nameImageView(depth_image_view.get(), "offscreen/depth/view");

    GET_MODULE(Camera).setScreenSize(extent.width, extent.height);
    LOG_INFO(logger, "offscreen frame target initialized");
}

OffscreenFrameTarget::~OffscreenFrameTarget() {
    frame_cleanup->detach(this);
}

FrameBeginResult OffscreenFrameTarget::beginFrame(
    std::shared_ptr<const RendererRuntimeGeneration>
        runtime_generation,
    GpuSubmissionLease submission_lease,
    FrameBeginMode mode) {
    if (active_frame) {
        throw std::logic_error(
            "offscreen frame target begin called with an unfinished frame");
    }
    const auto &cmd_buf = render_cmd_bufs[in_flight_frame_index];

    const auto timeout =
        mode == FrameBeginMode::nonblocking ? 0 : UINT64_MAX;
    const auto fence_result = device.waitForFences(
        {cmd_buf.getFence()}, VK_TRUE, timeout);
    if (mode == FrameBeginMode::nonblocking &&
        fence_result == vk::Result::eTimeout) {
        return FrameBeginResult{
            .disposition =
                FrameBeginDisposition::unavailable,
            .reason =
                FrameUnavailableReason::acquire_not_ready,
        };
    }
    if (fence_result != vk::Result::eSuccess) {
        throw std::runtime_error(
            "failed to wait for offscreen submission fence: " +
            vk::to_string(fence_result));
    }
    submission_leases.complete(
        in_flight_frame_index);

    if (next_frame_serial ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "offscreen frame token serial space exhausted");
    }
    const auto serial = next_frame_serial++;
    const auto recording_start_color_layout =
        color_layout;
    try {
        cmd_buf.recordBegin();
        active_frame = ActiveFrame{
            .serial = serial,
            .slot = in_flight_frame_index,
            .recording_start_color_layout =
                recording_start_color_layout,
        };
        GET_MODULE(VulkanUtils)
            .changeImageLayoutCmd(*cmd_buf, color_image, color_layout,
                                  vk::ImageLayout::eColorAttachmentOptimal,
                                  transitionInfo(
                                      color_layout,
                                      vk::ImageLayout::eColorAttachmentOptimal));
        color_layout = vk::ImageLayout::eColorAttachmentOptimal;
        setViewportAndScissor(*cmd_buf, extent);
    } catch (...) {
        abandonFrameSerial(serial);
        throw;
    }

    auto frame = makeFrame(
        FrameRenderContext{
        .cmd_buf = *cmd_buf,
        .color_image = color_image.image.get(),
        .color_attachment = color_image_view.get(),
        .depth_attachment = depth_image_view.get(),
        .extent = extent,
        .image_prepared_semaphore = nullptr,
        .required_layout = vk::ImageLayout::eTransferSrcOptimal,
        .in_flight_frame_index = in_flight_frame_index,
        },
        std::move(runtime_generation),
        std::move(submission_lease), frame_cleanup,
        serial);
    FrameBeginResult result;
    result.disposition = FrameBeginDisposition::ready;
    result.reason = FrameUnavailableReason::none;
    result.frame.emplace(std::move(frame));
    return result;
}

void OffscreenFrameTarget::recordOutputTransformCopy(vk::CommandBuffer cmd_buf, vk::Image source,
                                                     vk::Format source_format,
                                                     vk::Extent2D source_extent) {
    if (!active_frame ||
        active_frame->phase !=
            ActiveFramePhase::recording) {
        throw std::logic_error(
            "offscreen output transform requires an active frame");
    }
    if (active_frame->output_transform_recorded) {
        throw std::runtime_error("output_transform was recorded more than once");
    }
    if (source_format != color_format || source_extent.width != extent.width ||
        source_extent.height != extent.height) {
        throw std::runtime_error(
            "output_transform resolver v1 requires identical format, channel order, extent, and sample count");
    }

    auto &vk_utils = GET_MODULE(VulkanUtils);
    vk_utils.changeImageLayoutCmd(cmd_buf, color_image, color_layout,
                                  vk::ImageLayout::eTransferDstOptimal,
                                  transitionInfo(color_layout, vk::ImageLayout::eTransferDstOptimal));
    color_layout = vk::ImageLayout::eTransferDstOptimal;

    vk::ImageCopy region;
    region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.extent = vk::Extent3D{extent.width, extent.height, 1};
    cmd_buf.copyImage(source, vk::ImageLayout::eTransferSrcOptimal, color_image.image.get(),
                      vk::ImageLayout::eTransferDstOptimal, {region});

    vk_utils.changeImageLayoutCmd(cmd_buf, color_image, color_layout,
                                  vk::ImageLayout::eTransferSrcOptimal,
                                  transitionInfo(color_layout, vk::ImageLayout::eTransferSrcOptimal));
    color_layout = vk::ImageLayout::eTransferSrcOptimal;
    active_frame->output_transform_recorded = true;
}

FrameSubmitResult OffscreenFrameTarget::submit(
    FrameTargetFrame frame) {
    validateFrameTarget(
        frame, frame_cleanup,
        "offscreen frame target");
    if (!active_frame) {
        throw std::logic_error(
            "offscreen frame target submit called without an active frame");
    }
    const auto serial = active_frame->serial;
    auto consumed = consumeFrame(
        std::move(frame), frame_cleanup, serial,
        "offscreen frame target");
    const auto &cmd_buf = render_cmd_bufs[in_flight_frame_index];
    if (active_frame->phase !=
        ActiveFramePhase::recording) {
        throw std::logic_error(
            "offscreen frame target submit called without a recording frame");
    }

    try {
        if (!active_frame->output_transform_recorded) {
            GET_MODULE(VulkanUtils)
                .changeImageLayoutCmd(
                    *cmd_buf, color_image, color_layout,
                    vk::ImageLayout::eTransferSrcOptimal,
                    transitionInfo(
                        color_layout,
                        vk::ImageLayout::
                            eTransferSrcOptimal));
            color_layout =
                vk::ImageLayout::eTransferSrcOptimal;
        }

        cmd_buf.recordEndSubmit();
        active_frame->phase =
            ActiveFramePhase::submitted;
        auto lease =
            consumed.submission_lease != nullptr
                ? std::move(consumed.submission_lease)
                : GpuSubmissionLease{
                      std::move(
                          consumed.runtime_generation)};
        submission_leases.submitted(
            in_flight_frame_index, std::move(lease));
        if (auto result = device.waitForFences(
                {cmd_buf.getFence()}, VK_TRUE,
                UINT64_MAX);
            result != vk::Result::eSuccess) {
            throw std::runtime_error(
                "failed to wait for offscreen submission fence: " +
                vk::to_string(result));
        }
        submission_leases.complete(
            in_flight_frame_index);
        active_frame.reset();
        has_rendered_frame = true;

        in_flight_frame_index++;
        in_flight_frame_index %= in_flight_frames_num;
        return FrameSubmitResult{
            .disposition =
                FrameSubmitDisposition::submitted,
        };
    } catch (...) {
        abandonFrameSerial(serial);
        throw;
    }
}

void OffscreenFrameTarget::cleanupAbandonedFrame(
    void *owner, std::uint64_t serial) noexcept {
    static_cast<OffscreenFrameTarget *>(owner)
        ->abandonFrameSerial(serial);
}

void OffscreenFrameTarget::abandonFrameSerial(
    std::uint64_t serial) noexcept {
    if (!active_frame ||
        active_frame->serial != serial) {
        return;
    }
    const auto abandoned = *active_frame;
    if (abandoned.phase ==
        ActiveFramePhase::submitted) {
        try {
            device.waitIdle();
            submission_leases.complete(
                abandoned.slot);
        } catch (...) {
            // The device is no longer recoverable, but abort must preserve the
            // original render exception.
        }
    } else {
        render_cmd_bufs[abandoned.slot]
            .abortRecording();
        color_layout =
            abandoned.recording_start_color_layout;
    }
    active_frame.reset();
}

void OffscreenFrameTarget::abandon(
    FrameTargetFrame frame) noexcept {
    abandonFrame(std::move(frame));
}

FrameTargetCaps OffscreenFrameTarget::caps() const {
    const auto graphics_queue_family =
        GET_MODULE(VulkanManageCore)
            .getGraphicsQueueFamilyIndex();
    return FrameTargetCaps{
        .compile_facts =
            OutputCompileFacts{
                .target_kind =
                    OutputTargetKind::offscreen,
                .extent = extent,
                .color_format = color_format,
                .color_space =
                    vk::ColorSpaceKHR::eSrgbNonlinear,
                .encoding_path =
                    color_format ==
                            vk::Format::eR8G8B8A8Srgb
                        ? OutputEncodingPath::srgb_hardware
                        : OutputEncodingPath::
                              srgb_shader_unorm,
                .selected_usage =
                    vk::ImageUsageFlagBits::eColorAttachment |
                    vk::ImageUsageFlagBits::eTransferSrc,
                .capture_available = true,
                .surface_transform =
                    vk::SurfaceTransformFlagBitsKHR::
                        eIdentity,
                .graphics_queue_family =
                    graphics_queue_family,
                .presentation_queue_family =
                    graphics_queue_family,
            },
    };
}

std::vector<uint8_t> OffscreenFrameTarget::readbackLastFrameRGBA8() {
    if (!has_rendered_frame) {
        throw std::runtime_error("OffscreenFrameTarget has no rendered frame to read back");
    }
    if (color_layout != vk::ImageLayout::eTransferSrcOptimal) {
        throw std::runtime_error("OffscreenFrameTarget color image is not ready for readback");
    }

    auto &vkcore = GET_MODULE(VulkanManageCore);
    const vk::DeviceSize bytes_num = static_cast<vk::DeviceSize>(extent.width) * extent.height * 4;
    auto staging = vkcore.allocBuf(bytes_num, vk::BufferUsageFlagBits::eTransferDst,
                                   vma::MemoryUsage::eAutoPreferHost,
                                   vma::AllocationCreateFlagBits::eHostAccessRandom);

    vk::BufferImageCopy copy_region;
    copy_region.bufferOffset = 0;
    copy_region.bufferRowLength = 0;
    copy_region.bufferImageHeight = 0;
    copy_region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    copy_region.imageSubresource.mipLevel = 0;
    copy_region.imageSubresource.baseArrayLayer = 0;
    copy_region.imageSubresource.layerCount = 1;
    copy_region.imageOffset = vk::Offset3D{0, 0, 0};
    copy_region.imageExtent = vk::Extent3D{extent.width, extent.height, 1};

    GET_MODULE(VulkanUtils)
        .executeOneTimeCmd(
            [&](vk::CommandBuffer cmd_buf) {
                cmd_buf.copyImageToBuffer(color_image.image.get(), vk::ImageLayout::eTransferSrcOptimal,
                                          staging.buffer.get(), {copy_region});
            },
            true);

    return vkcore.readBuf(staging, bytes_num);
}

} // namespace Pelican
