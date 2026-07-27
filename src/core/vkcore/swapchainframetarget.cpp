#include "swapchainframetarget.hpp"

#include "../log.hpp"
#include "../os/window.hpp"
#include "../renderer/camera.hpp"
#include "core.hpp"
#include "util.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace Pelican {

static uint32_t chooseSwapchainImageCount(const vk::SurfaceCapabilitiesKHR &surface_cap) {
    uint32_t image_count = surface_cap.minImageCount + 1;
    if (surface_cap.maxImageCount > 0) {
        image_count = std::min(image_count, surface_cap.maxImageCount);
    }
    return image_count;
}

static vk::Extent2D chooseSwapchainExtent(const vk::SurfaceCapabilitiesKHR &surface_cap,
                                          vk::Extent2D framebuffer_extent) {
    if (surface_cap.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return surface_cap.currentExtent;
    }

    return vk::Extent2D{
        std::clamp(framebuffer_extent.width, surface_cap.minImageExtent.width, surface_cap.maxImageExtent.width),
        std::clamp(framebuffer_extent.height, surface_cap.minImageExtent.height, surface_cap.maxImageExtent.height),
    };
}

static vk::CompositeAlphaFlagBitsKHR chooseCompositeAlpha(
    vk::CompositeAlphaFlagsKHR supported) {
    constexpr std::array candidates{
        vk::CompositeAlphaFlagBitsKHR::eOpaque,
        vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
        vk::CompositeAlphaFlagBitsKHR::ePostMultiplied,
        vk::CompositeAlphaFlagBitsKHR::eInherit,
    };
    for (const auto candidate : candidates) {
        if (supported & candidate) return candidate;
    }
    throw std::runtime_error(
        "No Vulkan composite alpha mode is available");
}

static SwapchainWithFmt createSwapchain(vk::Device device, const vk::PhysicalDevice &phys_device,
                                        vk::SurfaceKHR surface, vk::Extent2D framebuffer_extent,
                                        uint32_t graphics_queue_family,
                                        uint32_t presentation_queue_family) {
    LOG_INFO(logger, "vulkan swapchain creating...");

    vk::SwapchainCreateInfoKHR create_info;

    create_info.surface = surface;

    const auto surface_cap = phys_device.getSurfaceCapabilitiesKHR(surface);
    auto surface_fmts = phys_device.getSurfaceFormatsKHR(surface);
    auto surface_presentmodes = phys_device.getSurfacePresentModesKHR(surface);
    if (surface_fmts.empty()) {
        throw std::runtime_error("No Vulkan surface formats available");
    }
    if (surface_presentmodes.empty()) {
        throw std::runtime_error("No Vulkan present modes available");
    }

    const auto pred_fmt = [](const vk::SurfaceFormatKHR &format1, const vk::SurfaceFormatKHR &format2) {
        const auto score_func = [](vk::SurfaceFormatKHR format) {
            if ((format.format == vk::Format::eR8G8B8A8Srgb || format.format == vk::Format::eB8G8R8A8Srgb) &&
                format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
                return 20;
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

    const auto swapchain_extent = chooseSwapchainExtent(surface_cap, framebuffer_extent);

    create_info.minImageCount = chooseSwapchainImageCount(surface_cap);
    create_info.imageFormat = surface_fmts[0].format;
    create_info.imageColorSpace = surface_fmts[0].colorSpace;
    create_info.imageExtent = swapchain_extent;
    create_info.imageArrayLayers = 1;
    const auto selected_format_features =
        phys_device.getFormatProperties(surface_fmts[0].format).optimalTilingFeatures;
    const bool capture_available =
        static_cast<bool>(surface_cap.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferSrc) &&
        static_cast<bool>(selected_format_features & vk::FormatFeatureFlagBits::eTransferSrc);
    create_info.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
    if (capture_available) {
        create_info.imageUsage |= vk::ImageUsageFlagBits::eTransferSrc;
    }
    const std::array queue_families{graphics_queue_family, presentation_queue_family};
    if (graphics_queue_family == presentation_queue_family) {
        create_info.imageSharingMode = vk::SharingMode::eExclusive;
    } else {
        create_info.imageSharingMode = vk::SharingMode::eConcurrent;
        create_info.setQueueFamilyIndices(queue_families);
    }
    create_info.preTransform = surface_cap.currentTransform;
    create_info.presentMode = surface_presentmodes[0];
    create_info.compositeAlpha =
        chooseCompositeAlpha(
            surface_cap.supportedCompositeAlpha);
    create_info.clipped = VK_TRUE;

    return SwapchainWithFmt{
        .swapchain =
            device.createSwapchainKHRUnique(create_info),
        .format = surface_fmts[0].format,
        .color_space = surface_fmts[0].colorSpace,
        .extent = swapchain_extent,
        .selected_usage = create_info.imageUsage,
        .surface_transform = create_info.preTransform,
        .present_configuration =
            WsiPresentConfiguration{
                .present_mode = create_info.presentMode,
                .image_count = create_info.minImageCount,
                .composite_alpha =
                    create_info.compositeAlpha,
                .clipped =
                    create_info.clipped == VK_TRUE,
            },
        .capture_available = capture_available,
    };
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

static vk::UniqueImageView createImageViewsForDepth(vk::Device device, const ImageWrapper &image) {
    vk::ImageViewCreateInfo create_info;
    create_info.image = image.image.get();
    create_info.viewType = vk::ImageViewType::e2D;
    create_info.format = image.format;
    create_info.components.r = vk::ComponentSwizzle::eR;
    create_info.components.g = vk::ComponentSwizzle::eG;
    create_info.components.b = vk::ComponentSwizzle::eB;
    create_info.components.a = vk::ComponentSwizzle::eA;
    create_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
    create_info.subresourceRange.baseMipLevel = 0;
    create_info.subresourceRange.levelCount = 1;
    create_info.subresourceRange.baseArrayLayer = 0;
    create_info.subresourceRange.layerCount = 1;

    return device.createImageViewUnique(create_info);
}

void SwapchainFrameTarget::releaseSurfaceDependants() {
    rendered_semaphores.clear();
    image_acquire_semaphores.clear();
    depth_image_view.reset();
    depth_image = ImageWrapper{};
    swapchain_image_views.clear();
    swapchain_images.clear();
    swapchain = SwapchainWithFmt{};
}

void SwapchainFrameTarget::surfaceDependantsSetup() {
    const auto framebuffer_extent = GET_MODULE(Window).waitFramebufferExtent();
    releaseSurfaceDependants();
    auto &vkcore = GET_MODULE(VulkanManageCore);
    swapchain = createSwapchain(vkcore.getDevice(), vkcore.getPhysDevice(), vkcore.getSurface(),
                                framebuffer_extent, vkcore.getGraphicsQueueFamilyIndex(),
                                vkcore.getPresentationQueueFamilyIndex());
    extent = swapchain.extent;
    GET_MODULE(Camera).setScreenSize(extent.width, extent.height);
    presen_queue = GET_MODULE(VulkanManageCore).getPresentationQueue();
    swapchain_images = getImageFromSwapchain(device, swapchain.swapchain.get());
    image_acquire_semaphores =
        GET_MODULE(VulkanManageCore)
            .createSemaphores(in_flight_frames_num);
    // A present wait semaphore is reusable only after the corresponding
    // swapchain image is acquired again. Frame-slot fences do not prove that
    // the presentation engine consumed the previous binary semaphore signal.
    rendered_semaphores =
        GET_MODULE(VulkanManageCore)
            .createSemaphores(swapchain_images.size());
    swapchain_image_views = createImageViewsFromImages(device, swapchain_images, swapchain.format);
    depth_image =
        GET_MODULE(VulkanManageCore)
            .allocImage(vk::Extent3D{extent, 1}, vk::Format::eD32Sfloat,
                        vk::ImageUsageFlagBits::eDepthStencilAttachment, vma::MemoryUsage::eAutoPreferDevice, {});
    depth_image_view = createImageViewsForDepth(device, depth_image);

    const auto &debug_utils = vkcore.getDebugUtils();
    debug_utils.nameSwapchain(swapchain.swapchain.get(), "swapchain");
    for (std::size_t image_index = 0; image_index < swapchain_images.size();
         ++image_index) {
        const auto base = "swapchain/image/" + std::to_string(image_index);
        debug_utils.nameImage(swapchain_images[image_index], (base + "/image").c_str());
        debug_utils.nameImageView(swapchain_image_views[image_index].get(),
                                  (base + "/view").c_str());
    }
    debug_utils.nameImage(depth_image.image.get(), "swapchain/depth/image");
    debug_utils.nameImageView(depth_image_view.get(), "swapchain/depth/view");
}

void SwapchainFrameTarget::recreateSurfaceDependants() {
    device.waitIdle();
    submission_leases.completeAll();
    surfaceDependantsSetup();
    surface_stale = false;
    current_image_index = 0;
    has_rendered_frame = false;
}

SwapchainFrameTarget::SwapchainFrameTarget()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      render_cmd_bufs{}, in_flight_frame_index{0} {
    frame_cleanup =
        std::make_shared<FrameTargetFrameCleanup>(
            this, &SwapchainFrameTarget::
                      cleanupAbandonedFrame);

    const auto &vkcore = GET_MODULE(VulkanManageCore);
    {
        auto tmp_cmd_bufs = vkcore.allocCmdBufs(in_flight_frames_num);
        assert(tmp_cmd_bufs.size() == in_flight_frames_num);
        for (int i = 0; i < in_flight_frames_num; i++) {
            render_cmd_bufs[i] = std::move(tmp_cmd_bufs[i]);
        }
    }

    surfaceDependantsSetup();

    LOG_INFO(logger, "rendertarget initialized");
}

SwapchainFrameTarget::~SwapchainFrameTarget() {
    frame_cleanup->detach(this);
}

FrameBeginResult SwapchainFrameTarget::beginFrame(
    std::shared_ptr<const RendererRuntimeGeneration>
        runtime_generation,
    GpuSubmissionLease submission_lease,
    FrameBeginMode mode) {
    if (active_frame) {
        throw std::logic_error(
            "swapchain frame target begin called with an unfinished frame");
    }
    const bool nonblocking =
        mode == FrameBeginMode::nonblocking;
    if (surface_stale) {
        const auto framebuffer = GET_MODULE(Window).framebufferExtent();
        if (framebuffer.width == 0 || framebuffer.height == 0) {
            if (nonblocking) {
                return FrameBeginResult{
                    .disposition =
                        FrameBeginDisposition::unavailable,
                    .reason =
                        FrameUnavailableReason::zero_extent,
                };
            }
        }
        if (nonblocking) {
            return FrameBeginResult{
                .disposition =
                    FrameBeginDisposition::unavailable,
                .reason =
                    FrameUnavailableReason::surface_stale,
            };
        }
        recreateSurfaceDependants();
    }
    do {
        if (nonblocking) {
            const auto framebuffer = GET_MODULE(Window).framebufferExtent();
            if (framebuffer.width == 0 || framebuffer.height == 0) {
                return FrameBeginResult{
                    .disposition =
                        FrameBeginDisposition::unavailable,
                    .reason =
                        FrameUnavailableReason::zero_extent,
                };
            }
        }
        const auto image_prepared_semaphore = image_acquire_semaphores[in_flight_frame_index].get();
        const auto &cmd_buf = render_cmd_bufs[in_flight_frame_index];

        const auto fence_result = device.waitForFences(
            {cmd_buf.getFence()}, VK_TRUE, nonblocking ? 0 : UINT64_MAX);
        if (nonblocking && fence_result == vk::Result::eTimeout) {
            return FrameBeginResult{
                .disposition =
                    FrameBeginDisposition::unavailable,
                .reason =
                    FrameUnavailableReason::
                        acquire_not_ready,
            };
        }
        if (fence_result != vk::Result::eSuccess) {
            throw std::runtime_error(
                "failed to wait for swapchain submission fence: " +
                vk::to_string(fence_result));
        }
        submission_leases.complete(
            in_flight_frame_index);

        // vulkan.hpp only tolerates {eSuccess, eTimeout, eNotReady,
        // eSuboptimalKHR} here and throws for eErrorOutOfDateKHR, so the
        // surface-recovery branches below are reachable only if the exception
        // is translated back into the result they already handle. A window
        // resize is the ordinary source of that result, not a fatal error.
        vk::ResultValue<std::uint32_t> image_acquire_result{
            vk::Result::eErrorOutOfDateKHR, 0};
        try {
            image_acquire_result =
                device.acquireNextImageKHR(swapchain.swapchain.get(),
                                           nonblocking ? 0 : UINT64_MAX,
                                           image_prepared_semaphore);
        } catch (const vk::OutOfDateKHRError &) {
            image_acquire_result = vk::ResultValue<std::uint32_t>{
                vk::Result::eErrorOutOfDateKHR, 0};
        }
        if (nonblocking &&
            (image_acquire_result.result == vk::Result::eTimeout ||
             image_acquire_result.result == vk::Result::eNotReady)) {
            return FrameBeginResult{
                .disposition =
                    FrameBeginDisposition::unavailable,
                .reason =
                    FrameUnavailableReason::
                        acquire_not_ready,
            };
        }
        if (image_acquire_result.result == vk::Result::eErrorOutOfDateKHR) {
            if (nonblocking) {
                surface_stale = true;
                return FrameBeginResult{
                    .disposition =
                        FrameBeginDisposition::unavailable,
                    .reason =
                        FrameUnavailableReason::
                            output_out_of_date,
                };
            }
            recreateSurfaceDependants();
            continue;
        }
        if (image_acquire_result.result != vk::Result::eSuccess &&
            image_acquire_result.result != vk::Result::eSuboptimalKHR) {
            throw std::runtime_error("failed on vkAcquireNextImageKHR : " + vk::to_string(image_acquire_result.result));
        }

        if (next_frame_serial ==
            std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error(
                "swapchain frame token serial space exhausted");
        }
        const auto serial = next_frame_serial++;
        current_image_index =
            image_acquire_result.value;
        active_frame = ActiveFrame{
            .serial = serial,
            .slot = in_flight_frame_index,
            .image_index =
                image_acquire_result.value,
            .begin_mode = mode,
        };
        try {
            cmd_buf.recordBegin();
            active_frame->phase =
                ActiveFramePhase::recording;

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

            {
                vk::ImageMemoryBarrier barrier;
                barrier.oldLayout = vk::ImageLayout::eUndefined;
                barrier.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
                barrier.image = swapchain_images[current_image_index];
                barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = 1;
                barrier.srcAccessMask = {};
                barrier.dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead |
                                        vk::AccessFlagBits::eColorAttachmentWrite;
                cmd_buf->pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                         vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {}, {barrier});
            }
        } catch (...) {
            abandonFrameSerial(serial);
            throw;
        }

        auto frame = makeFrame(
            FrameRenderContext{
                .cmd_buf = *cmd_buf,
                .color_image =
                    swapchain_images[
                        image_acquire_result.value],
                .color_attachment =
                    swapchain_image_views[
                        image_acquire_result.value]
                        .get(),
                .depth_attachment =
                    depth_image_view.get(),
                .extent = extent,
                .image_prepared_semaphore =
                    image_prepared_semaphore,
                .required_layout =
                    vk::ImageLayout::ePresentSrcKHR,
                .in_flight_frame_index =
                    in_flight_frame_index,
            },
            std::move(runtime_generation),
            std::move(submission_lease), frame_cleanup,
            serial);
        FrameBeginResult result;
        result.disposition =
            FrameBeginDisposition::ready;
        result.reason = FrameUnavailableReason::none;
        result.frame.emplace(std::move(frame));
        return result;
    } while (true);
}

void SwapchainFrameTarget::recordOutputTransformCopy(vk::CommandBuffer cmd_buf, vk::Image source,
                                                     vk::Format source_format,
                                                     vk::Extent2D source_extent) {
    if (!active_frame ||
        active_frame->phase !=
            ActiveFramePhase::recording) {
        throw std::logic_error(
            "swapchain output transform requires an active frame");
    }
    if (active_frame->output_transform_recorded) {
        throw std::runtime_error("output_transform was recorded more than once");
    }
    if (source_format != swapchain.format || source_extent.width != extent.width ||
        source_extent.height != extent.height) {
        throw std::runtime_error(
            "output_transform resolver v1 requires identical format, channel order, extent, and sample count");
    }

    vk::ImageMemoryBarrier to_transfer;
    to_transfer.srcAccessMask = vk::AccessFlagBits::eColorAttachmentRead |
                                vk::AccessFlagBits::eColorAttachmentWrite;
    to_transfer.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
    to_transfer.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
    to_transfer.newLayout = vk::ImageLayout::eTransferDstOptimal;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image =
        swapchain_images[active_frame->image_index];
    to_transfer.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    cmd_buf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, {to_transfer});

    vk::ImageCopy region;
    region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.extent = vk::Extent3D{extent.width, extent.height, 1};
    cmd_buf.copyImage(source, vk::ImageLayout::eTransferSrcOptimal,
                      swapchain_images[active_frame->image_index],
                      vk::ImageLayout::eTransferDstOptimal,
                      {region});

    vk::ImageMemoryBarrier to_present;
    to_present.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    to_present.dstAccessMask = {};
    to_present.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    to_present.newLayout = vk::ImageLayout::ePresentSrcKHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image =
        swapchain_images[active_frame->image_index];
    to_present.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    cmd_buf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, {to_present});
    active_frame->output_transform_recorded = true;
}

FrameSubmitResult SwapchainFrameTarget::submit(
    FrameTargetFrame frame) {
    validateFrameTarget(
        frame, frame_cleanup,
        "swapchain frame target");
    if (!active_frame) {
        throw std::logic_error(
            "swapchain frame target submit called without an active frame");
    }
    const auto active = *active_frame;
    auto consumed = consumeFrame(
        std::move(frame), frame_cleanup, active.serial,
        "swapchain frame target");
    if (active.phase != ActiveFramePhase::recording) {
        throw std::logic_error(
            "swapchain frame target submit called without a recording frame");
    }
    const auto &cmd_buf = render_cmd_bufs[active.slot];

    try {
        if (!active.output_transform_recorded) {
            vk::ImageMemoryBarrier barrier;
            barrier.srcAccessMask =
                vk::AccessFlagBits::eColorAttachmentRead |
                vk::AccessFlagBits::eColorAttachmentWrite;
            barrier.oldLayout =
                vk::ImageLayout::eColorAttachmentOptimal;
            barrier.newLayout =
                vk::ImageLayout::ePresentSrcKHR;
            barrier.image =
                swapchain_images[active.image_index];
            barrier.subresourceRange.aspectMask =
                vk::ImageAspectFlagBits::eColor;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            cmd_buf->pipelineBarrier(
                vk::PipelineStageFlagBits::
                    eColorAttachmentOutput,
                vk::PipelineStageFlagBits::eBottomOfPipe,
                {}, {}, {}, {barrier});
        }

        const auto rendered_semaphore =
            rendered_semaphores[active.image_index].get();
        cmd_buf.recordEndSubmit(
            {rendered_semaphore},
            {image_acquire_semaphores[active.slot].get()},
            {vk::PipelineStageFlagBits::eTopOfPipe});
        active_frame->phase =
            ActiveFramePhase::submitted;
        // Capture immediately after queue submission. Presentation may fail
        // after the GPU has accepted the work, so the lease must already be
        // retained.
        auto lease =
            consumed.submission_lease != nullptr
                ? std::move(consumed.submission_lease)
                : GpuSubmissionLease{
                      std::move(
                          consumed.runtime_generation)};
        submission_leases.submitted(
            active.slot, std::move(lease));

        vk::PresentInfoKHR present_info;
        present_info.setSwapchains(
            swapchain.swapchain.get());
        present_info.setImageIndices(
            active.image_index);
        present_info.setWaitSemaphores(
            rendered_semaphore);

        // vulkan.hpp throws for eErrorOutOfDateKHR even though it is routine
        // WSI control flow, so normalize it before classifying the result.
        vk::Result present_result =
            vk::Result::eSuccess;
        try {
            present_result =
                presen_queue.presentKHR(present_info);
        } catch (const vk::OutOfDateKHRError &) {
            present_result =
                vk::Result::eErrorOutOfDateKHR;
        }

        auto disposition =
            FrameSubmitDisposition::presented;
        bool recreated = false;
        if (present_result ==
                vk::Result::eSuboptimalKHR ||
            present_result ==
                vk::Result::eErrorOutOfDateKHR) {
            if (active.begin_mode ==
                FrameBeginMode::nonblocking) {
                surface_stale = true;
                disposition =
                    FrameSubmitDisposition::
                        output_stale;
            } else {
                recreateSurfaceDependants();
                recreated = true;
                disposition =
                    FrameSubmitDisposition::
                        output_stale;
            }
        } else if (present_result !=
                   vk::Result::eSuccess) {
            throw std::runtime_error(
                "failed on vkQueuePresentKHR : " +
                vk::to_string(present_result));
        }

        in_flight_frame_index =
            (active.slot + 1) %
            in_flight_frames_num;
        current_image_index =
            recreated ? 0 : active.image_index;
        active_frame.reset();
        has_rendered_frame = !recreated;
        return FrameSubmitResult{
            .disposition = disposition};
    } catch (...) {
        abandonFrameSerial(active.serial);
        throw;
    }
}

void SwapchainFrameTarget::cleanupAbandonedFrame(
    void *owner, std::uint64_t serial) noexcept {
    static_cast<SwapchainFrameTarget *>(owner)
        ->abandonFrameSerial(serial);
}

void SwapchainFrameTarget::abandonFrameSerial(
    std::uint64_t serial) noexcept {
    if (!active_frame ||
        active_frame->serial != serial) {
        return;
    }
    const auto abandoned = *active_frame;
    const auto &cmd_buf =
        render_cmd_bufs[abandoned.slot];
    if (abandoned.phase ==
        ActiveFramePhase::recording) {
        cmd_buf.abortRecording();
    }
    // Token destruction must not enter a Vulkan wait or a swapchain create.
    // The blocking flat path retires this incomplete WP215 swapchain before
    // the next acquire. A nonblocking mirror stays unavailable until WP216's
    // epoch maintenance worker publishes a replacement.
    surface_stale = true;
    active_frame.reset();
    has_rendered_frame = false;
}

void SwapchainFrameTarget::abandon(
    FrameTargetFrame frame) noexcept {
    abandonFrame(std::move(frame));
}

FrameTargetCaps SwapchainFrameTarget::caps() const {
    const auto &vkcore = GET_MODULE(VulkanManageCore);
    return FrameTargetCaps{
        .compile_facts =
            OutputCompileFacts{
                .target_kind = OutputTargetKind::window,
                .extent = extent,
                .color_format = swapchain.format,
                .color_space = swapchain.color_space,
                .encoding_path =
                    (swapchain.format ==
                             vk::Format::eR8G8B8A8Srgb ||
                         swapchain.format ==
                             vk::Format::eB8G8R8A8Srgb)
                        ? OutputEncodingPath::srgb_hardware
                        : OutputEncodingPath::
                              srgb_shader_unorm,
                .selected_usage =
                    swapchain.selected_usage,
                .capture_available =
                    swapchain.capture_available,
                .surface_transform =
                    swapchain.surface_transform,
                .graphics_queue_family =
                    vkcore.getGraphicsQueueFamilyIndex(),
                .presentation_queue_family =
                    vkcore.getPresentationQueueFamilyIndex(),
            },
    };
}

std::vector<uint8_t> SwapchainFrameTarget::readbackLastFrameRGBA8() {
    if (!swapchain.capture_available) {
        throw std::runtime_error("capture unavailable_windowed: surface lacks TRANSFER_SRC support");
    }
    if (!has_rendered_frame) {
        throw std::runtime_error("SwapchainFrameTarget has no rendered frame to read back");
    }

    device.waitIdle();
    auto &vkcore = GET_MODULE(VulkanManageCore);
    const vk::DeviceSize bytes_num = static_cast<vk::DeviceSize>(extent.width) * extent.height * 4;
    auto staging = vkcore.allocBuf(bytes_num, vk::BufferUsageFlagBits::eTransferDst,
                                   vma::MemoryUsage::eAutoPreferHost,
                                   vma::AllocationCreateFlagBits::eHostAccessRandom);

    vk::BufferImageCopy copy_region;
    copy_region.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    copy_region.imageExtent = vk::Extent3D{extent.width, extent.height, 1};
    const auto image = swapchain_images[current_image_index];
    GET_MODULE(VulkanUtils).executeOneTimeCmd(
        [&](vk::CommandBuffer cmd_buf) {
            vk::ImageMemoryBarrier to_transfer;
            to_transfer.oldLayout = vk::ImageLayout::ePresentSrcKHR;
            to_transfer.newLayout = vk::ImageLayout::eTransferSrcOptimal;
            to_transfer.srcAccessMask = {};
            to_transfer.dstAccessMask = vk::AccessFlagBits::eTransferRead;
            to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_transfer.image = image;
            to_transfer.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
            cmd_buf.pipelineBarrier(vk::PipelineStageFlagBits::eBottomOfPipe,
                                    vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, {to_transfer});
            cmd_buf.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal,
                                      staging.buffer.get(), {copy_region});
            vk::ImageMemoryBarrier to_present = to_transfer;
            to_present.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
            to_present.newLayout = vk::ImageLayout::ePresentSrcKHR;
            to_present.srcAccessMask = vk::AccessFlagBits::eTransferRead;
            to_present.dstAccessMask = {};
            cmd_buf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                    vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, {to_present});
        },
        true);
    return vkcore.readBuf(staging, bytes_num);
}

} // namespace Pelican
