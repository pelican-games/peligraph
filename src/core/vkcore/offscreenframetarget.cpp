#include "offscreenframetarget.hpp"

#include "../launchconfig.hpp"
#include "../log.hpp"
#include "../renderer/camera.hpp"
#include "core.hpp"
#include "util.hpp"

#include <cassert>
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

    GET_MODULE(Camera).setScreenSize(extent.width, extent.height);
    LOG_INFO(logger, "offscreen frame target initialized");
}

OffscreenFrameTarget::~OffscreenFrameTarget() {}

FrameRenderContext OffscreenFrameTarget::render_begin() {
    const auto &cmd_buf = render_cmd_bufs[in_flight_frame_index];

    if (auto result = device.waitForFences({cmd_buf.getFence()}, VK_TRUE, UINT64_MAX);
        result != vk::Result::eSuccess) {
        LOG_WARNING(logger, "vkWaitForFences didn't succeed : {}", vk::to_string(result));
    }

    cmd_buf.recordBegin();
    GET_MODULE(VulkanUtils)
        .changeImageLayoutCmd(*cmd_buf, color_image, color_layout, vk::ImageLayout::eColorAttachmentOptimal,
                              transitionInfo(color_layout, vk::ImageLayout::eColorAttachmentOptimal));
    color_layout = vk::ImageLayout::eColorAttachmentOptimal;
    output_transform_recorded = false;
    setViewportAndScissor(*cmd_buf, extent);

    return FrameRenderContext{
        .cmd_buf = *cmd_buf,
        .color_attachment = color_image_view.get(),
        .depth_attachment = depth_image_view.get(),
        .extent = extent,
        .image_prepared_semaphore = nullptr,
        .required_layout = vk::ImageLayout::eTransferSrcOptimal,
        .in_flight_frame_index = in_flight_frame_index,
    };
}

void OffscreenFrameTarget::recordOutputTransformCopy(vk::CommandBuffer cmd_buf, vk::Image source,
                                                     vk::Format source_format,
                                                     vk::Extent2D source_extent) {
    if (output_transform_recorded) {
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
    output_transform_recorded = true;
}

void OffscreenFrameTarget::render_end() {
    const auto &cmd_buf = render_cmd_bufs[in_flight_frame_index];

    if (!output_transform_recorded) {
        GET_MODULE(VulkanUtils)
            .changeImageLayoutCmd(*cmd_buf, color_image, color_layout, vk::ImageLayout::eTransferSrcOptimal,
                                  transitionInfo(color_layout, vk::ImageLayout::eTransferSrcOptimal));
        color_layout = vk::ImageLayout::eTransferSrcOptimal;
    }

    cmd_buf.recordEndSubmit();
    if (auto result = device.waitForFences({cmd_buf.getFence()}, VK_TRUE, UINT64_MAX);
        result != vk::Result::eSuccess) {
        LOG_WARNING(logger, "vkWaitForFences didn't succeed : {}", vk::to_string(result));
    }
    has_rendered_frame = true;

    in_flight_frame_index++;
    in_flight_frame_index %= in_flight_frames_num;
}

FrameTargetCaps OffscreenFrameTarget::caps() const {
    return FrameTargetCaps{
        .color_format = color_format,
        .extent = extent,
        .presents = false,
        .capture_available = true,
        .color_path = color_format == vk::Format::eR8G8B8A8Srgb ? "srgb" : "unorm_fallback",
    };
}

bool OffscreenFrameTarget::consumeExtentChanged() { return false; }

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
