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
        info.src_access = vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (old_layout == vk::ImageLayout::eTransferSrcOptimal) {
        info.src_stage = vk::PipelineStageFlagBits::eTransfer;
        info.src_access = vk::AccessFlagBits::eTransferRead;
    }

    if (new_layout == vk::ImageLayout::eColorAttachmentOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        info.dst_access = vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (new_layout == vk::ImageLayout::eTransferSrcOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eTransfer;
        info.dst_access = vk::AccessFlagBits::eTransferRead;
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
      color_format{vk::Format::eR8G8B8A8Unorm}, color_layout{vk::ImageLayout::eUndefined} {
    auto &vkcore = GET_MODULE(VulkanManageCore);
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
    setViewportAndScissor(*cmd_buf, extent);

    return FrameRenderContext{
        .cmd_buf = *cmd_buf,
        .color_attachment = color_image_view.get(),
        .depth_attachment = depth_image_view.get(),
        .extent = extent,
        .image_prepared_semaphore = nullptr,
        .required_layout = vk::ImageLayout::eTransferSrcOptimal,
    };
}

void OffscreenFrameTarget::render_end() {
    const auto &cmd_buf = render_cmd_bufs[in_flight_frame_index];

    GET_MODULE(VulkanUtils)
        .changeImageLayoutCmd(*cmd_buf, color_image, color_layout, vk::ImageLayout::eTransferSrcOptimal,
                              transitionInfo(color_layout, vk::ImageLayout::eTransferSrcOptimal));
    color_layout = vk::ImageLayout::eTransferSrcOptimal;

    cmd_buf.recordEndSubmit();
    if (auto result = device.waitForFences({cmd_buf.getFence()}, VK_TRUE, UINT64_MAX);
        result != vk::Result::eSuccess) {
        LOG_WARNING(logger, "vkWaitForFences didn't succeed : {}", vk::to_string(result));
    }

    in_flight_frame_index++;
    in_flight_frame_index %= in_flight_frames_num;
}

FrameTargetCaps OffscreenFrameTarget::caps() const {
    return FrameTargetCaps{
        .color_format = color_format,
        .extent = extent,
        .presents = false,
    };
}

std::vector<uint8_t> OffscreenFrameTarget::readbackLastFrameRGBA8() {
    throw std::runtime_error("OffscreenFrameTarget readback is not implemented yet");
}

} // namespace Pelican
