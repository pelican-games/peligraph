#include "openxrcompositiongraphics.hpp"

#include "../vkcore/core.hpp"
#include "../vkcore/image.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican::OpenXr {
namespace {

vk::ImageAspectFlags depthLayoutAspect(
    vk::Format format) {
    if (format == vk::Format::eD24UnormS8Uint ||
        format == vk::Format::eD32SfloatS8Uint) {
        return vk::ImageAspectFlagBits::eDepth |
               vk::ImageAspectFlagBits::eStencil;
    }
    return vk::ImageAspectFlagBits::eDepth;
}

vk::UniqueImageView createImageView(
    vk::Device device, vk::Image image, vk::Format format,
    vk::ImageAspectFlags aspect, vk::ImageViewType type,
    std::uint32_t base_array_layer, std::uint32_t layer_count) {
    vk::ImageViewCreateInfo info;
    info.image = image;
    info.viewType = type;
    info.format = format;
    info.components = {
        vk::ComponentSwizzle::eR,
        vk::ComponentSwizzle::eG,
        vk::ComponentSwizzle::eB,
        vk::ComponentSwizzle::eA,
    };
    info.subresourceRange = {
        aspect, 0, 1, base_array_layer, layer_count};
    return device.createImageViewUnique(info);
}

class VulkanXrCompositionGraphics final : public IXrCompositionGraphics {
    struct SwapchainResources {
        std::vector<vk::Image> images;
        std::vector<vk::UniqueImageView> layered_views;
        std::array<
            std::vector<vk::UniqueImageView>,
            xr_stereo_view_count>
            layer_views;
    };

    static constexpr std::uint32_t view_family_command_slot =
        xr_stereo_view_count;
    static constexpr std::uint32_t command_slots_per_frame =
        xr_stereo_view_count + 1;

    VulkanManageCore &vulkan;
    vk::Device device;
    SwapchainResources color;
    SwapchainResources depth;
    std::vector<CommandBufWrapper> command_buffers;
    vk::Extent2D extent{};
    vk::Format color_format = vk::Format::eUndefined;
    vk::Format depth_format = vk::Format::eUndefined;
    vk::ImageLayout release_layout = runtime_release_layout;

    CommandBufWrapper &command(std::uint32_t slot,
                               std::uint32_t in_flight_frame_index) {
        return command_buffers.at(static_cast<std::size_t>(in_flight_frame_index) *
                                      command_slots_per_frame +
                                  slot);
    }

    void enumerateSwapchain(
        PFN_xrEnumerateSwapchainImages enumerate,
        XrSwapchain swapchain, vk::Format format,
        vk::ImageAspectFlags aspect, std::string_view debug_name,
        SwapchainResources &resources) {
        std::uint32_t image_count = 0;
        requireSuccess(
            "xrEnumerateSwapchainImages",
            enumerate(swapchain, 0, &image_count, nullptr));
        if (image_count == 0) {
            throw std::runtime_error(
                "OpenXR swapchain has no Vulkan images");
        }
        std::vector<XrSwapchainImageVulkan2KHR> xr_images(
            image_count,
            XrSwapchainImageVulkan2KHR{
                XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        std::uint32_t returned_count = 0;
        requireSuccess(
            "xrEnumerateSwapchainImages",
            enumerate(
                swapchain, image_count, &returned_count,
                reinterpret_cast<
                    XrSwapchainImageBaseHeader *>(
                    xr_images.data())));
        if (returned_count != image_count) {
            throw std::runtime_error(
                "OpenXR swapchain image count changed during enumeration");
        }

        resources.images.reserve(image_count);
        resources.layered_views.reserve(image_count);
        for (auto &views : resources.layer_views) {
            views.reserve(image_count);
        }
        for (std::uint32_t image_index = 0;
             image_index < image_count; ++image_index) {
            const vk::Image image{
                xr_images[image_index].image};
            resources.images.push_back(image);
            resources.layered_views.push_back(
                createImageView(
                    device, image, format, aspect,
                    vk::ImageViewType::e2DArray, 0,
                    xr_stereo_view_count));
            for (std::uint32_t layer = 0;
                 layer < xr_stereo_view_count; ++layer) {
                resources.layer_views[layer].push_back(
                    createImageView(
                        device, image, format, aspect,
                        vk::ImageViewType::e2D, layer, 1));
            }

            const auto base =
                "xr/" + std::string{debug_name} +
                "/swapchain/image/" +
                std::to_string(image_index);
            vulkan.getDebugUtils().nameImage(
                image, (base + "/image").c_str());
            vulkan.getDebugUtils().nameImageView(
                resources.layered_views.back().get(),
                (base + "/array-view").c_str());
            for (std::uint32_t layer = 0;
                 layer < xr_stereo_view_count; ++layer) {
                vulkan.getDebugUtils().nameImageView(
                    resources.layer_views[layer]
                        .back()
                        .get(),
                    (base + "/layer/" +
                     std::to_string(layer) + "/view")
                        .c_str());
            }
        }
    }

    FrameRenderContext begin(
        std::uint32_t command_slot,
        std::uint32_t base_array_layer,
        std::uint32_t array_layers,
        XrCompositionAcquiredImages images,
        std::uint32_t in_flight_frame_index) {
        if (images.color >= color.images.size()) {
            throw std::runtime_error(
                "OpenXR acquired color image index is out of range");
        }
        if (images.depth &&
            (*images.depth >= depth.images.size() ||
             depth_format == vk::Format::eUndefined)) {
            throw std::runtime_error(
                "OpenXR acquired depth image index is out of range");
        }
        if (!images.depth &&
            depth_format != vk::Format::eUndefined) {
            throw std::runtime_error(
                "OpenXR composition depth image was not acquired");
        }

        auto &cmd =
            command(command_slot, in_flight_frame_index);
        if (device.waitForFences(
                {cmd.getFence()}, VK_TRUE, UINT64_MAX) !=
            vk::Result::eSuccess) {
            throw std::runtime_error(
                "OpenXR composition command fence wait failed");
        }
        cmd.recordBegin();

        std::vector<vk::ImageMemoryBarrier> barriers;
        barriers.reserve(images.depth ? 2u : 1u);
        vk::ImageMemoryBarrier color_barrier;
        color_barrier.oldLayout =
            vk::ImageLayout::eUndefined;
        color_barrier.newLayout =
            vk::ImageLayout::eColorAttachmentOptimal;
        color_barrier.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        color_barrier.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        color_barrier.image = color.images[images.color];
        color_barrier.subresourceRange = {
            vk::ImageAspectFlagBits::eColor, 0, 1,
            base_array_layer, array_layers};
        color_barrier.dstAccessMask =
            vk::AccessFlagBits::eColorAttachmentRead |
            vk::AccessFlagBits::eColorAttachmentWrite;
        barriers.push_back(color_barrier);

        if (images.depth) {
            vk::ImageMemoryBarrier depth_barrier;
            depth_barrier.oldLayout =
                vk::ImageLayout::eUndefined;
            depth_barrier.newLayout =
                vk::ImageLayout::eTransferDstOptimal;
            depth_barrier.srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            depth_barrier.dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            depth_barrier.image =
                depth.images[*images.depth];
            depth_barrier.subresourceRange = {
                depthLayoutAspect(depth_format), 0, 1,
                base_array_layer, array_layers};
            depth_barrier.dstAccessMask =
                vk::AccessFlagBits::eTransferWrite;
            barriers.push_back(depth_barrier);
        }
        cmd->pipelineBarrier(
            vk::PipelineStageFlagBits::eTopOfPipe,
            vk::PipelineStageFlagBits::eColorAttachmentOutput |
                vk::PipelineStageFlagBits::eTransfer |
                vk::PipelineStageFlagBits::eEarlyFragmentTests |
                vk::PipelineStageFlagBits::eLateFragmentTests,
            {}, {}, {}, barriers);

        const vk::Viewport viewport{
            0.0F, 0.0F,
            static_cast<float>(extent.width),
            static_cast<float>(extent.height),
            0.0F, 1.0F};
        cmd->setViewport(0, {viewport});
        cmd->setScissor(
            0, {vk::Rect2D{{0, 0}, extent}});

        FrameRenderContext context{
            .cmd_buf = *cmd,
            .color_image = color.images[images.color],
            .color_attachment =
                array_layers == xr_stereo_view_count
                    ? color.layered_views[images.color].get()
                    : color.layer_views[base_array_layer]
                          [images.color]
                              .get(),
            .color_base_array_layer =
                base_array_layer,
            .color_array_layers = array_layers,
            .extent = extent,
            .image_prepared_semaphore = {},
            .required_layout = release_layout,
            .in_flight_frame_index =
                in_flight_frame_index,
        };
        if (array_layers == xr_stereo_view_count) {
            context.color_layer_attachments.reserve(
                xr_stereo_view_count);
            for (std::uint32_t layer = 0;
                 layer < xr_stereo_view_count; ++layer) {
                context.color_layer_attachments.push_back(
                    color.layer_views[layer]
                        [images.color]
                            .get());
            }
        }
        if (images.depth) {
            context.depth_image =
                depth.images[*images.depth];
            context.depth_attachment =
                array_layers == xr_stereo_view_count
                    ? depth.layered_views[*images.depth].get()
                    : depth.layer_views[base_array_layer]
                          [*images.depth]
                              .get();
            context.depth_base_array_layer =
                base_array_layer;
            context.depth_array_layers = array_layers;
            context.depth_format = depth_format;
            context.depth_copy_layout =
                vk::ImageLayout::eTransferDstOptimal;
            context.depth_required_layout =
                vk::ImageLayout::
                    eDepthStencilAttachmentOptimal;
            if (array_layers ==
                xr_stereo_view_count) {
                context.depth_layer_attachments.reserve(
                    xr_stereo_view_count);
                for (std::uint32_t layer = 0;
                     layer < xr_stereo_view_count;
                     ++layer) {
                    context.depth_layer_attachments.push_back(
                        depth.layer_views[layer]
                            [*images.depth]
                                .get());
                }
            }
        }
        return context;
    }

  public:
    explicit VulkanXrCompositionGraphics(VulkanManageCore &vulkan)
        : vulkan{vulkan}, device{vulkan.getDevice()} {}

    ~VulkanXrCompositionGraphics() override {
        if (device) device.waitIdle();
    }

    void initialize(const XrCompositionGraphicsConfig &config) override {
        if (config.enumerate_swapchain_images == nullptr) {
            throw std::runtime_error(
                "OpenXR composition graphics has no image enumeration function");
        }
        color_format = config.color_format;
        depth_format = config.depth_format;
        extent = config.extent;
        release_layout = config.release_layout;
        if (!config.extent.width || !config.extent.height ||
            config.color_swapchain == XR_NULL_HANDLE ||
            color_format == vk::Format::eUndefined) {
            throw std::runtime_error(
                "OpenXR composition graphics has an invalid array swapchain");
        }
        if ((config.depth_swapchain == XR_NULL_HANDLE) !=
            (depth_format == vk::Format::eUndefined)) {
            throw std::runtime_error(
                "OpenXR composition depth swapchain contract is inconsistent");
        }
        command_buffers = vulkan.allocCmdBufs(
            in_flight_frames_num *
            command_slots_per_frame);
        enumerateSwapchain(
            config.enumerate_swapchain_images,
            config.color_swapchain, color_format,
            vk::ImageAspectFlagBits::eColor,
            "color", color);
        if (config.depth_swapchain != XR_NULL_HANDLE) {
            enumerateSwapchain(
                config.enumerate_swapchain_images,
                config.depth_swapchain, depth_format,
                vk::ImageAspectFlagBits::eDepth,
                "depth", depth);
        }
    }

    FrameRenderContext beginView(std::uint32_t view_index,
                                 XrCompositionAcquiredImages images,
                                 std::uint32_t in_flight_frame_index) override {
        if (view_index >= xr_stereo_view_count) {
            throw std::out_of_range(
                "OpenXR composition view index is out of range");
        }
        return begin(
            view_index, view_index, 1,
            std::move(images), in_flight_frame_index);
    }

    FrameRenderContext beginViewFamily(
        XrCompositionAcquiredImages images,
        std::uint32_t in_flight_frame_index) override {
        return begin(
            view_family_command_slot, 0,
            xr_stereo_view_count, std::move(images),
            in_flight_frame_index);
    }

    void submitView(std::uint32_t view_index,
                    std::uint32_t in_flight_frame_index) override {
        command(view_index, in_flight_frame_index).recordEndSubmit();
    }

    void submitViewFamily(
        std::uint32_t in_flight_frame_index) override {
        command(view_family_command_slot,
                in_flight_frame_index)
            .recordEndSubmit();
    }

    void waitForSubmission(std::uint32_t view_index,
                           std::uint32_t in_flight_frame_index) override {
        const auto &fence = command(view_index, in_flight_frame_index).getFence();
        if (device.waitForFences({fence}, VK_TRUE, UINT64_MAX) !=
            vk::Result::eSuccess) {
            throw std::runtime_error("OpenXR composition GPU submission failed to finish");
        }
    }

    void waitForViewFamilySubmission(
        std::uint32_t in_flight_frame_index) override {
        const auto &fence =
            command(view_family_command_slot,
                    in_flight_frame_index)
                .getFence();
        if (device.waitForFences(
                {fence}, VK_TRUE, UINT64_MAX) !=
            vk::Result::eSuccess) {
            throw std::runtime_error(
                "OpenXR composition view-family GPU submission failed to finish");
        }
    }
};

} // namespace

std::unique_ptr<IXrCompositionGraphics>
makeProductionGraphics(const XrCompositionDependencies &dependencies) {
    if (dependencies.vulkan == nullptr) {
        throw std::runtime_error(
            "OpenXR composition production target requires VulkanManageCore");
    }
    return std::make_unique<VulkanXrCompositionGraphics>(*dependencies.vulkan);
}

} // namespace Pelican::OpenXr
