#pragma once

// The one poke a test makes on RenderTarget: make the next frames report an
// output with no image, the state a window reaches before its swapchain
// exists. Production reaches it only through a real window, so without this a
// headless test could not exercise the renderer's "skip this frame" path.

#include "../src/core/vkcore/rendertarget.hpp"
#include "../src/core/vkcore/offscreenframetarget.hpp"
#include "../src/core/vkcore/core.hpp"

#include <cstdint>

namespace Pelican::test_support {

struct RenderTargetTestAccess {
    static OffscreenFrameTarget &offscreen(RenderTarget &target) { return dynamic_cast<OffscreenFrameTarget &>(*target.impl); }
    static void deferSubmissionWait(RenderTarget &target, bool enabled, vk::Semaphore gate={}) {
        auto &o=offscreen(target); o.defer_wait_for_testing=enabled; o.submission_gate_for_testing=gate;
    }
    static void beforeNextBegin(RenderTarget &target,std::function<void()> callback) {offscreen(target).before_begin_for_testing=std::move(callback);}
    static std::size_t outstandingSubmissions(RenderTarget &target) {return offscreen(target).submission_leases.outstandingForTesting();}
    static std::size_t unsignaledSubmissions(RenderTarget &target) {
        auto &o=offscreen(target);std::size_t count=0;
        for(const auto &cmd:o.render_cmd_bufs) if(o.device.getFenceStatus(cmd.getFence())==vk::Result::eNotReady) ++count;
        return count;
    }
    static void completeSubmissions(RenderTarget &target) {offscreen(target).completePendingSubmissions();}

    static std::shared_ptr<const void> resizeOffscreen(RenderTarget &target,vk::Extent2D extent,bool window_path) {
        auto &o=offscreen(target); auto &core=GET_MODULE(VulkanManageCore);
        struct Retired { ImageWrapper color,depth; vk::UniqueImageView color_view,depth_view; };
        auto retired=std::make_shared<Retired>();
        retired->color=std::move(o.color_image); retired->depth=std::move(o.depth_image);
        retired->color_view=std::move(o.color_image_view); retired->depth_view=std::move(o.depth_image_view);
        o.color_image=core.allocImage({extent.width,extent.height,1},o.color_format,vk::ImageUsageFlagBits::eColorAttachment|vk::ImageUsageFlagBits::eTransferSrc,vma::MemoryUsage::eAutoPreferDevice,{});
        o.depth_image=core.allocImage({extent.width,extent.height,1},vk::Format::eD32Sfloat,vk::ImageUsageFlagBits::eDepthStencilAttachment,vma::MemoryUsage::eAutoPreferDevice,{});
        const auto view=[&](const ImageWrapper &image,vk::ImageAspectFlags aspect) {
            vk::ImageViewCreateInfo ci;ci.image=image.image.get();ci.viewType=vk::ImageViewType::e2D;ci.format=image.format;ci.subresourceRange={aspect,0,1,0,1};return core.getDevice().createImageViewUnique(ci);
        };
        o.color_image_view=view(o.color_image,vk::ImageAspectFlagBits::eColor);
        o.depth_image_view=view(o.depth_image,vk::ImageAspectFlagBits::eDepth);
        o.extent=extent; o.color_layout=vk::ImageLayout::eUndefined; o.has_rendered_frame=false;
        o.window_output_for_testing=window_path;
        return retired;
    }
    static void reportUnavailableFrames(RenderTarget &target,
                                        std::uint32_t frames) {
        target.unavailable_frames_for_testing = frames;
    }
};

} // namespace Pelican::test_support
