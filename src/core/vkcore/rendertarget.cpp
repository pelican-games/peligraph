#include "rendertarget.hpp"
#include "../log.hpp"
#include "core.hpp"
#include <stdexcept>

namespace Pelican {

static SwapchainWithFmt createSwapchain(vk::Device device, const vk::PhysicalDevice &phys_device,
                                        vk::SurfaceKHR surface) {
    LOG_INFO(logger, "vulkan swapchain creating...");

    vk::SwapchainCreateInfoKHR create_info;

    create_info.surface = surface;

    const auto surface_cap = phys_device.getSurfaceCapabilitiesKHR(surface);
    auto surface_fmts = phys_device.getSurfaceFormatsKHR(surface);
    auto surface_presentmodes = phys_device.getSurfacePresentModesKHR(surface);

    const auto pred_fmt = [](const vk::SurfaceFormatKHR &format1, const vk::SurfaceFormatKHR &format2) {
        const auto score_func = [](vk::SurfaceFormatKHR format) {
            if ((format.format == vk::Format::eR8G8B8A8Unorm || format.format == vk::Format::eB8G8R8A8Unorm) &&
                format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
                return 10;
            return 0;
        };

        return score_func(format1) > score_func(format2);
    };
    std::stable_sort(surface_fmts.begin(), surface_fmts.end(), pred_fmt);

    const auto pred_mode = [](vk::PresentModeKHR mode1, vk::PresentModeKHR mode2) {
        const auto score_func = [](vk::PresentModeKHR mode) { return mode == vk::PresentModeKHR::eMailbox ? 10 : 0; };

        return score_func(mode1) > score_func(mode2);
    };
    std::stable_sort(surface_presentmodes.begin(), surface_presentmodes.end(), pred_mode);

    create_info.minImageCount = surface_cap.minImageCount + 1;
    create_info.imageFormat = surface_fmts[0].format;
    create_info.imageColorSpace = surface_fmts[0].colorSpace;
    create_info.imageExtent = surface_cap.currentExtent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
    create_info.imageSharingMode = vk::SharingMode::eExclusive;
    create_info.preTransform = surface_cap.currentTransform;
    create_info.presentMode = surface_presentmodes[0];
    create_info.clipped = VK_TRUE;

    return SwapchainWithFmt{device.createSwapchainKHRUnique(create_info), surface_fmts[0].format};
}

static std::vector<vk::Image> getImageFromSwapchain(vk::Device device, vk::SwapchainKHR swapchain) {
    LOG_INFO(logger, "vulkan swapchain image obtaining...");
    return device.getSwapchainImagesKHR(swapchain);
}

static std::vector<vk::UniqueImageView>
createImageViewsFromImages(vk::Device device, const std::vector<vk::Image> &images, vk::Format format) {
    LOG_INFO(logger, "vulkan imageview creating...");

    std::vector<vk::UniqueImageView> image_views{images.size()};

    for (int i = 0; i < images.size(); i++) {
        vk::ImageViewCreateInfo create_info;
        create_info.image = images[i];
        create_info.viewType = vk::ImageViewType::e2D;
        create_info.format = format;
        create_info.components.r = vk::ComponentSwizzle::eR;
        create_info.components.g = vk::ComponentSwizzle::eG;
        create_info.components.b = vk::ComponentSwizzle::eB;
        create_info.components.a = vk::ComponentSwizzle::eA;
        create_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        create_info.subresourceRange.baseMipLevel = 0;
        create_info.subresourceRange.levelCount = 1;
        create_info.subresourceRange.baseArrayLayer = 0;
        create_info.subresourceRange.layerCount = 1;

        image_views[i] = device.createImageViewUnique(create_info);
    }

    return image_views;
}

static vk::Extent2D getSurfaceExtent(vk::PhysicalDevice phys_device, vk::SurfaceKHR surface) {
    return phys_device.getSurfaceCapabilitiesKHR(surface).currentExtent;
}

RenderTarget::RenderTarget(DependencyContainer &container)
    : device{container.get<VulkanManageCore>().getDevice()},
      presen_queue{container.get<VulkanManageCore>().getPresentationQueue()},
      swapchain{createSwapchain(container.get<VulkanManageCore>().getDevice(),
                                container.get<VulkanManageCore>().getPhysDevice(),
                                container.get<VulkanManageCore>().getSurface())},
      images{getImageFromSwapchain(device, swapchain.swapchain.get())},
      image_views{createImageViewsFromImages(device, images, swapchain.format)},
      image_acquire_semaphores{container.get<VulkanManageCore>().createSemaphores(in_flight_frames_num)},
      rendered_semaphores{container.get<VulkanManageCore>().createSemaphores(in_flight_frames_num)}, render_cmd_bufs{},
      extent{getSurfaceExtent(container.get<VulkanManageCore>().getPhysDevice(),
                              container.get<VulkanManageCore>().getSurface())},
      in_flight_frame_index{0} {
    const auto &vkcore = container.get<VulkanManageCore>();
    {
        auto tmp_cmd_bufs = vkcore.allocCmdBufs(in_flight_frames_num);
        assert(tmp_cmd_bufs.size() == in_flight_frames_num);
        for (int i = 0; i < in_flight_frames_num; i++) {
            render_cmd_bufs[i] = std::move(tmp_cmd_bufs[i]);
        }
    }

    LOG_INFO(logger, "rendertarget initialized");
}

RenderTarget::~RenderTarget() {
    std::array<vk::Fence, in_flight_frames_num> fences;
    for (int i = 0; i < in_flight_frames_num; i++) {
        fences[i] = render_cmd_bufs[i].getFence();
    }
    if (auto result = device.waitForFences(fences, VK_TRUE, 500'000'000); result != vk::Result::eSuccess) {
        LOG_WARNING(logger, "vkWaitForFences didn't succeed : {}", vk::to_string(result));
    }
}

FrameRenderContext RenderTarget::render_begin() {
    const auto image_prepared_semaphore = image_acquire_semaphores[in_flight_frame_index].get();
    const auto &cmd_buf = render_cmd_bufs[in_flight_frame_index];

    if (auto result = device.waitForFences({cmd_buf.getFence()}, VK_TRUE, UINT64_MAX); result != vk::Result::eSuccess) {
        LOG_WARNING(logger, "vkWaitForFences didn't succeed : {}", vk::to_string(result));
    }
    device.resetFences({cmd_buf.getFence()});

    auto image_acquire_result =
        device.acquireNextImageKHR(swapchain.swapchain.get(), UINT64_MAX, image_prepared_semaphore);
    if (image_acquire_result.result != vk::Result::eSuccess) {
        throw std::runtime_error("failed on vkAcquireNextImageKHR : " + vk::to_string(image_acquire_result.result));
    }
    current_image_index = image_acquire_result.value;

    cmd_buf.recordBegin();

    {
        vk::Viewport viewport;
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        cmd_buf->setViewport(0, {viewport});

        vk::Rect2D scissor;
        scissor.offset = vk::Offset2D{0, 0};
        scissor.extent = extent;
        cmd_buf->setScissor(0, {scissor});
    }

    return FrameRenderContext{
        .cmd_buf = *cmd_buf,
        .color_attachment = image_views[image_acquire_result.value].get(),
        .extent = extent,
        .image_prepared_semaphore = image_prepared_semaphore,
        .required_layout = vk::ImageLayout::ePresentSrcKHR,
    };
}

void RenderTarget::render_end() {
    const auto &cmd_buf = render_cmd_bufs[in_flight_frame_index];

    vk::ImageMemoryBarrier barrier;
    barrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    barrier.oldLayout = vk::ImageLayout::eUndefined;
    barrier.newLayout = vk::ImageLayout::ePresentSrcKHR;
    barrier.image = images[current_image_index];
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    cmd_buf->pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                             vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, {barrier});

    cmd_buf.recordEndSubmit({rendered_semaphores[in_flight_frame_index].get()},
                            {image_acquire_semaphores[in_flight_frame_index].get()},
                            {vk::PipelineStageFlagBits::eColorAttachmentOutput});

    vk::PresentInfoKHR presen_info;
    presen_info.setSwapchains(swapchain.swapchain.get());
    presen_info.setImageIndices(current_image_index);
    presen_info.setWaitSemaphores(rendered_semaphores[in_flight_frame_index].get());

    if (auto result = presen_queue.presentKHR(presen_info); result != vk::Result::eSuccess) {
        throw std::runtime_error("failed on vkQueuePresentKHR : " + vk::to_string(result));
    }

    in_flight_frame_index++;
    in_flight_frame_index %= in_flight_frames_num;
}

} // namespace Pelican
