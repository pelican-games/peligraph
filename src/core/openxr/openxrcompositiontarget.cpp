#include "openxrcompositiontarget.hpp"

#include "../vkcore/core.hpp"
#include "../vkcore/image.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican::OpenXr {
namespace {

constexpr XrViewConfigurationType primary_view_configuration =
    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
constexpr vk::ImageLayout runtime_release_layout =
    vk::ImageLayout::eColorAttachmentOptimal;

struct XrCompositionApi {
    PFN_xrEnumerateViewConfigurationViews enumerate_view_configuration_views = nullptr;
    PFN_xrEnumerateSwapchainFormats enumerate_swapchain_formats = nullptr;
    PFN_xrCreateSwapchain create_swapchain = nullptr;
    PFN_xrDestroySwapchain destroy_swapchain = nullptr;
    PFN_xrEnumerateSwapchainImages enumerate_swapchain_images = nullptr;
    PFN_xrAcquireSwapchainImage acquire_swapchain_image = nullptr;
    PFN_xrWaitSwapchainImage wait_swapchain_image = nullptr;
    PFN_xrReleaseSwapchainImage release_swapchain_image = nullptr;
};

class XrCompositionError final : public std::runtime_error {
    XrResult xr_result;

  public:
    XrCompositionError(const char *operation, XrResult result)
        : std::runtime_error(std::string{operation} + " failed (XrResult " +
                             std::to_string(result) + ")"),
          xr_result{result} {}

    XrResult result() const noexcept { return xr_result; }
};

template <class Function>
void resolveRequired(PFN_xrGetInstanceProcAddr get_instance_proc_addr,
                     XrInstance instance, const char *name, Function &destination) {
    PFN_xrVoidFunction function = nullptr;
    const auto result = get_instance_proc_addr(instance, name, &function);
    if (XR_FAILED(result) || function == nullptr) {
        throw std::runtime_error(std::string{"xrGetInstanceProcAddr could not resolve "} +
                                 name);
    }
    destination = reinterpret_cast<Function>(function);
}

void requireSuccess(const char *operation, XrResult result) {
    if (XR_FAILED(result) || result == XR_SESSION_LOSS_PENDING) {
        throw XrCompositionError{operation, result};
    }
}

bool isLossResult(XrResult result) {
    return result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST ||
           result == XR_SESSION_LOSS_PENDING;
}

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

std::unique_ptr<IXrCompositionGraphics>
makeProductionGraphics(const XrCompositionDependencies &dependencies) {
    if (dependencies.vulkan == nullptr) {
        throw std::runtime_error(
            "OpenXR composition production target requires VulkanManageCore");
    }
    return std::make_unique<VulkanXrCompositionGraphics>(*dependencies.vulkan);
}

} // namespace

class XrCompositionTarget::Impl {
    struct SwapchainFrameState {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        XrSwapchainState state = XrSwapchainState::idle;
        std::uint32_t image_index = 0;
        bool release_attempted = false;
    };

    struct ViewState {
        bool view_begun = false;
        bool view_ended = false;
        bool submission_complete = false;
    };

    XrCompositionDependencies dependencies;
    SessionRuntime &session_runtime;
    XrCompositionApi api;
    // Declared before graphics so the graphics bridge (which waits the device
    // idle on destruction) is destroyed before a retained failed-wait lease.
    GpuSubmissionLease submission_lease;
    std::unique_ptr<IXrCompositionGraphics> graphics;
    std::array<XrViewConfigurationView, xr_stereo_view_count> view_configs{
        XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
        XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
    };
    XrCompositionCapabilities capabilities;
    SwapchainFrameState color;
    SwapchainFrameState depth;
    std::array<ViewState, xr_stereo_view_count> views;
    std::optional<XrDisplayTiming> display_timing;
    XrLocatedViews located_views;
    float near_z = 0.05F;
    float far_z = 1000.0F;
    std::uint32_t in_flight_frame_index = 0;
    std::uint32_t next_view = 0;
    bool frame_prepared = false;
    bool logical_frame_begun = false;
    bool view_family_begun = false;
    bool view_family_ended = false;
    bool view_family_submission_complete = false;
    bool depth_submission_active = false;
    bool teardown_required = false;

    void resolveApi() {
        const auto instance = session_runtime.instanceHandle();
        auto get_proc = dependencies.get_instance_proc_addr;
        if (get_proc == nullptr) {
            throw std::runtime_error(
                "OpenXR composition target has no xrGetInstanceProcAddr");
        }
        resolveRequired(get_proc, instance, "xrEnumerateViewConfigurationViews",
                        api.enumerate_view_configuration_views);
        resolveRequired(get_proc, instance, "xrEnumerateSwapchainFormats",
                        api.enumerate_swapchain_formats);
        resolveRequired(get_proc, instance, "xrCreateSwapchain", api.create_swapchain);
        resolveRequired(get_proc, instance, "xrDestroySwapchain", api.destroy_swapchain);
        resolveRequired(get_proc, instance, "xrEnumerateSwapchainImages",
                        api.enumerate_swapchain_images);
        resolveRequired(get_proc, instance, "xrAcquireSwapchainImage",
                        api.acquire_swapchain_image);
        resolveRequired(get_proc, instance, "xrWaitSwapchainImage",
                        api.wait_swapchain_image);
        resolveRequired(get_proc, instance, "xrReleaseSwapchainImage",
                        api.release_swapchain_image);
    }

    void queryViews() {
        std::uint32_t count = 0;
        requireSuccess("xrEnumerateViewConfigurationViews",
                       api.enumerate_view_configuration_views(
                           session_runtime.instanceHandle(), session_runtime.systemId(),
                           primary_view_configuration, 0, &count, nullptr));
        if (count != xr_stereo_view_count) {
            throw std::runtime_error(
                "OpenXR PRIMARY_STEREO must expose exactly two views");
        }
        std::uint32_t returned_count = 0;
        requireSuccess("xrEnumerateViewConfigurationViews",
                       api.enumerate_view_configuration_views(
                           session_runtime.instanceHandle(), session_runtime.systemId(),
                           primary_view_configuration, xr_stereo_view_count,
                           &returned_count, view_configs.data()));
        if (returned_count != xr_stereo_view_count) {
            throw std::runtime_error(
                "OpenXR PRIMARY_STEREO view count changed during enumeration");
        }
        for (const auto &view : view_configs) {
            if (view.recommendedImageRectWidth == 0 ||
                view.recommendedImageRectHeight == 0 ||
                view.maxImageRectWidth == 0 ||
                view.maxImageRectHeight == 0 ||
                view.maxSwapchainSampleCount < 1) {
                throw std::runtime_error(
                    "OpenXR view configuration cannot support the XR2a.1 swapchain");
            }
        }
        const auto desired_width = std::max(
            view_configs[0].recommendedImageRectWidth,
            view_configs[1].recommendedImageRectWidth);
        const auto desired_height = std::max(
            view_configs[0].recommendedImageRectHeight,
            view_configs[1].recommendedImageRectHeight);
        const auto shared_max_width = std::min(
            view_configs[0].maxImageRectWidth,
            view_configs[1].maxImageRectWidth);
        const auto shared_max_height = std::min(
            view_configs[0].maxImageRectHeight,
            view_configs[1].maxImageRectHeight);
        capabilities.extent = vk::Extent2D{
            std::min(desired_width, shared_max_width),
            std::min(desired_height, shared_max_height),
        };
        if (!capabilities.extent.width ||
            !capabilities.extent.height) {
            throw std::runtime_error(
                "OpenXR PRIMARY_STEREO has no common array-swapchain extent");
        }
    }

    std::vector<std::int64_t> enumerateFormats() {
        std::uint32_t count = 0;
        requireSuccess("xrEnumerateSwapchainFormats",
                       api.enumerate_swapchain_formats(session_runtime.sessionHandle(), 0,
                                                       &count, nullptr));
        if (count == 0) {
            throw std::runtime_error("OpenXR runtime exposed no swapchain formats");
        }
        std::vector<std::int64_t> formats(count);
        std::uint32_t returned_count = 0;
        requireSuccess("xrEnumerateSwapchainFormats",
                       api.enumerate_swapchain_formats(session_runtime.sessionHandle(), count,
                                                       &returned_count, formats.data()));
        if (returned_count != count) {
            throw std::runtime_error(
                "OpenXR swapchain format count changed during enumeration");
        }
        return formats;
    }

    std::int64_t selectColorFormat(
        std::span<const std::int64_t> formats) {
        const auto expected = static_cast<std::int64_t>(
            static_cast<VkFormat>(dependencies.renderer_color_format));
        for (const auto format : formats) {
            if (format == expected) return format;
        }
        throw std::runtime_error(
            "OpenXR runtime does not support the renderer's compiled flat target format");
    }

    std::optional<vk::Format> selectDepthFormat(
        std::span<const std::int64_t> formats) {
        if (!dependencies.composition_layer_depth_enabled) {
            capabilities.depth_reason =
                "XR_KHR_composition_layer_depth_not_enabled";
            return std::nullopt;
        }
        if (dependencies.renderer_depth_format ==
            vk::Format::eUndefined) {
            capabilities.depth_reason =
                "compiled_graph_has_no_external_depth_export";
            return std::nullopt;
        }
        const auto candidate =
            dependencies.renderer_depth_format;
        const auto raw = static_cast<std::int64_t>(
            static_cast<VkFormat>(candidate));
        if (std::find(formats.begin(), formats.end(), raw) ==
            formats.end()) {
            capabilities.depth_reason =
                "compiled_depth_format_not_supported_by_openxr_runtime";
            return std::nullopt;
        }
        if (dependencies.vulkan != nullptr) {
            const auto properties =
                dependencies.vulkan->getPhysDevice()
                    .getFormatProperties(candidate);
            const auto required =
                vk::FormatFeatureFlagBits::
                    eDepthStencilAttachment |
                vk::FormatFeatureFlagBits::eTransferDst;
            if ((properties.optimalTilingFeatures &
                 required) != required) {
                capabilities.depth_reason =
                    "compiled_depth_format_lacks_vulkan_transfer_destination";
                return std::nullopt;
            }
        }
        return candidate;
    }

    XrSwapchain createSwapchain(
        std::int64_t format, XrSwapchainUsageFlags usage) {
        XrSwapchainCreateInfo info{
            XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = usage;
        info.format = format;
        info.sampleCount = 1;
        info.width = capabilities.extent.width;
        info.height = capabilities.extent.height;
        info.faceCount = 1;
        info.arraySize = xr_stereo_view_count;
        info.mipCount = 1;
        XrSwapchain swapchain = XR_NULL_HANDLE;
        const auto result = api.create_swapchain(
            session_runtime.sessionHandle(), &info,
            &swapchain);
        if (isLossResult(result)) reportLoss(result);
        if (XR_FAILED(result) ||
            result == XR_SESSION_LOSS_PENDING ||
            swapchain == XR_NULL_HANDLE) {
            if (swapchain != XR_NULL_HANDLE) {
                (void)api.destroy_swapchain(swapchain);
            }
            throw XrCompositionError{
                "xrCreateSwapchain", result};
        }
        return swapchain;
    }

    void createSwapchains(
        std::int64_t color_format,
        std::optional<vk::Format> depth_format) {
        color.swapchain = createSwapchain(
            color_format,
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
        capabilities.array_color_swapchain = true;
        capabilities.view_family_execution = true;

        if (!depth_format) return;
        try {
            depth.swapchain = createSwapchain(
                static_cast<std::int64_t>(
                    static_cast<VkFormat>(*depth_format)),
                XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT);
            capabilities.depth_submission = true;
            capabilities.depth_format = *depth_format;
            capabilities.depth_reason.clear();
        } catch (const XrCompositionError &error) {
            if (isLossResult(error.result())) throw;
            depth.swapchain = XR_NULL_HANDLE;
            capabilities.depth_reason =
                "depth_swapchain_creation_failed_xr_result_" +
                std::to_string(error.result());
        }
    }

    void reportLoss(XrResult result) noexcept {
        teardown_required = true;
        session_runtime.reportCompositionLoss(result);
    }

    void resetFrameFlags() noexcept {
        frame_prepared = false;
        logical_frame_begun = false;
        next_view = 0;
        view_family_begun = false;
        view_family_ended = false;
        view_family_submission_complete = false;
        depth_submission_active = false;
        display_timing.reset();
        located_views = {};
        in_flight_frame_index =
            (in_flight_frame_index + 1) % in_flight_frames_num;
    }

    bool releaseSwapchain(
        SwapchainFrameState &swapchain) noexcept {
        if (swapchain.swapchain == XR_NULL_HANDLE ||
            swapchain.release_attempted) {
            return false;
        }
        swapchain.release_attempted = true;
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const auto result = api.release_swapchain_image(
            swapchain.swapchain, &info);
        if (XR_FAILED(result) || result == XR_SESSION_LOSS_PENDING) {
            teardown_required = true;
            if (isLossResult(result)) reportLoss(result);
            return false;
        }
        swapchain.state = XrSwapchainState::released;
        return true;
    }

    void closeZeroLayerAfterFailure() noexcept {
        if (!display_timing || session_runtime.hasTerminalPath()) return;
        try {
            session_runtime.endFrame(*display_timing);
        } catch (...) {
            teardown_required = true;
        }
    }

    void abortFrame() noexcept {
        bool all_submissions_complete = true;
        if (view_family_ended &&
            !view_family_submission_complete) {
            try {
                graphics->waitForViewFamilySubmission(
                    in_flight_frame_index);
                view_family_submission_complete = true;
            } catch (...) {
                teardown_required = true;
                all_submissions_complete = false;
            }
        } else {
            for (std::uint32_t view_index = 0;
                 view_index < xr_stereo_view_count;
                 ++view_index) {
                auto &view = views[view_index];
                if (!view.view_ended ||
                    view.submission_complete) {
                    continue;
                }
                try {
                    graphics->waitForSubmission(
                        view_index,
                        in_flight_frame_index);
                    view.submission_complete = true;
                } catch (...) {
                    teardown_required = true;
                    all_submissions_complete = false;
                }
            }
        }

        const auto release_if_legal =
            [&](SwapchainFrameState &swapchain) {
                if (swapchain.swapchain ==
                    XR_NULL_HANDLE) {
                    return;
                }
                if ((swapchain.state ==
                         XrSwapchainState::waited ||
                     swapchain.state ==
                         XrSwapchainState::submitted) &&
                    all_submissions_complete) {
                    (void)releaseSwapchain(swapchain);
                } else if (
                    swapchain.state ==
                    XrSwapchainState::acquired) {
                // xrReleaseSwapchainImage before a successful wait is illegal.
                // Leave this image to generation teardown instead.
                    teardown_required = true;
                }
            };
        // Release the auxiliary depth image before the projection color image.
        release_if_legal(depth);
        release_if_legal(color);
        closeZeroLayerAfterFailure();
        if (all_submissions_complete) {
            submission_lease.reset();
        }
        resetFrameFlags();
    }

    void acquireAndWait(
        SwapchainFrameState &swapchain) {
        if (swapchain.swapchain == XR_NULL_HANDLE) {
            return;
        }
        XrSwapchainImageAcquireInfo acquire_info{
            XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        const auto acquire_result = api.acquire_swapchain_image(
            swapchain.swapchain, &acquire_info,
            &swapchain.image_index);
        if (XR_FAILED(acquire_result) || acquire_result == XR_SESSION_LOSS_PENDING) {
            if (isLossResult(acquire_result)) reportLoss(acquire_result);
            throw XrCompositionError{"xrAcquireSwapchainImage", acquire_result};
        }
        swapchain.state = XrSwapchainState::acquired;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait_info.timeout = dependencies.image_wait_timeout;
        while (true) {
            const auto wait_result =
                api.wait_swapchain_image(
                    swapchain.swapchain, &wait_info);
            if (wait_result == XR_TIMEOUT_EXPIRED) continue;
            if (XR_FAILED(wait_result) || wait_result == XR_SESSION_LOSS_PENDING) {
                if (isLossResult(wait_result)) reportLoss(wait_result);
                throw XrCompositionError{"xrWaitSwapchainImage", wait_result};
            }
            swapchain.state = XrSwapchainState::waited;
            return;
        }
    }

    XrCompositionAcquiredImages acquiredImages() const {
        return {
            .color = color.image_index,
            .depth =
                depth_submission_active
                    ? std::optional{
                          depth.image_index}
                    : std::nullopt,
        };
    }

  public:
    Impl(const XrCompositionDependencies &dependencies,
         std::unique_ptr<IXrCompositionGraphics> graphics)
        : dependencies{dependencies},
          session_runtime{dependencies.session_runtime != nullptr
                              ? *dependencies.session_runtime
                              : throw std::runtime_error(
                                    "OpenXR composition target requires SessionRuntime")},
          graphics{std::move(graphics)} {
        if (!this->graphics) {
            throw std::runtime_error(
                "OpenXR composition target requires a graphics bridge");
        }
        if (dependencies.renderer_color_format == vk::Format::eUndefined) {
            throw std::runtime_error(
                "OpenXR composition target requires the compiled renderer format");
        }
        resolveApi();
        try {
            queryViews();
            const auto formats = enumerateFormats();
            const auto color_format =
                selectColorFormat(formats);
            const auto depth_format =
                selectDepthFormat(formats);
            createSwapchains(
                color_format, depth_format);
            this->graphics->initialize(XrCompositionGraphicsConfig{
                .enumerate_swapchain_images = api.enumerate_swapchain_images,
                .color_swapchain =
                    color.swapchain,
                .depth_swapchain =
                    depth.swapchain,
                .views = view_configs,
                .extent = capabilities.extent,
                .color_format = dependencies.renderer_color_format,
                .depth_format =
                    capabilities.depth_format,
                .release_layout = runtime_release_layout,
            });
        } catch (...) {
            this->graphics.reset();
            if (depth.swapchain != XR_NULL_HANDLE) {
                (void)api.destroy_swapchain(
                    depth.swapchain);
                depth.swapchain = XR_NULL_HANDLE;
            }
            if (color.swapchain != XR_NULL_HANDLE) {
                (void)api.destroy_swapchain(
                    color.swapchain);
                color.swapchain = XR_NULL_HANDLE;
            }
            throw;
        }
    }

    ~Impl() {
        graphics.reset();
        if (api.destroy_swapchain == nullptr) return;
        if (depth.swapchain != XR_NULL_HANDLE) {
            (void)api.destroy_swapchain(depth.swapchain);
        }
        if (color.swapchain != XR_NULL_HANDLE) {
            (void)api.destroy_swapchain(color.swapchain);
        }
    }

    void prepareFrame(const XrDisplayTiming &timing,
                      const XrLocatedViews &new_views,
                      float new_near_z, float new_far_z) {
        if (teardown_required) {
            throw std::logic_error(
                "OpenXR composition generation requires teardown");
        }
        if (frame_prepared || logical_frame_begun) {
            throw std::logic_error(
                "OpenXR composition frame was prepared more than once");
        }
        if (!timing.shouldRender() ||
            new_views.views.size() != xr_stereo_view_count) {
            throw std::logic_error(
                "OpenXR projection frame requires two located renderable views");
        }
        if (!(new_near_z > 0.0F &&
              new_far_z > new_near_z)) {
            throw std::logic_error(
                "OpenXR composition depth range requires 0 < nearZ < farZ");
        }
        display_timing.emplace(timing.predictedDisplayTime(),
                               timing.predictedDisplayPeriod(), true);
        located_views = new_views;
        near_z = new_near_z;
        far_z = new_far_z;
        frame_prepared = true;
    }

    void endFrameWithoutLayers(const XrDisplayTiming &timing) {
        if (frame_prepared || logical_frame_begun) {
            throw std::logic_error(
                "OpenXR zero-layer close requires an unprepared frame");
        }
        session_runtime.endFrame(timing);
    }

    bool configureExternalDepthSubmission(
        vk::Format source_format,
        vk::Extent2D source_extent) {
        if (logical_frame_begun) {
            throw std::logic_error(
                "OpenXR composition depth cannot be reconfigured "
                "during a logical frame");
        }
        depth_submission_active =
            source_format != vk::Format::eUndefined &&
            capabilities.depth_submission &&
            source_format == capabilities.depth_format &&
            source_extent == capabilities.extent;
        return depth_submission_active;
    }

    void beginLogicalFrame(std::uint32_t view_count) {
        if (!frame_prepared || logical_frame_begun ||
            view_count != xr_stereo_view_count) {
            throw std::logic_error(
                "OpenXR composition requires one prepared two-view logical frame");
        }
        const auto reusable =
            [](const SwapchainFrameState &state) {
                return state.swapchain ==
                           XR_NULL_HANDLE ||
                       state.state ==
                           XrSwapchainState::idle ||
                       state.state ==
                           XrSwapchainState::released;
            };
        if (!reusable(color) || !reusable(depth)) {
            throw std::logic_error(
                "OpenXR array swapchain generation is not reusable");
        }
        color.state = XrSwapchainState::idle;
        color.release_attempted = false;
        depth.state = XrSwapchainState::idle;
        depth.release_attempted = false;
        for (auto &view : views) {
            view.view_begun = false;
            view.view_ended = false;
            view.submission_complete = false;
        }
        logical_frame_begun = true;
        next_view = 0;
        try {
            acquireAndWait(color);
            if (depth_submission_active) {
                acquireAndWait(depth);
            }
        } catch (...) {
            abortFrame();
            throw;
        }
    }

    FrameRenderContext beginView(std::uint32_t view_index) {
        if (!logical_frame_begun || view_index != next_view ||
            view_index >= xr_stereo_view_count ||
            color.state != XrSwapchainState::waited ||
            (depth_submission_active &&
             depth.state != XrSwapchainState::waited) ||
            view_family_begun ||
            views[view_index].view_begun) {
            throw std::logic_error("OpenXR composition view begin is out of order");
        }
        try {
            auto context = graphics->beginView(
                view_index, acquiredImages(),
                in_flight_frame_index);
            views[view_index].view_begun = true;
            return context;
        } catch (...) {
            teardown_required = true;
            abortFrame();
            throw;
        }
    }

    FrameRenderContext beginViewFamily(
        std::uint32_t view_count) {
        if (!logical_frame_begun ||
            view_count != xr_stereo_view_count ||
            next_view != 0 || view_family_begun ||
            color.state != XrSwapchainState::waited ||
            (depth_submission_active &&
             depth.state != XrSwapchainState::waited)) {
            throw std::logic_error(
                "OpenXR composition view-family begin is out of order");
        }
        try {
            auto context =
                graphics->beginViewFamily(
                    acquiredImages(),
                    in_flight_frame_index);
            view_family_begun = true;
            return context;
        } catch (...) {
            teardown_required = true;
            abortFrame();
            throw;
        }
    }

    void retainSubmissionLease(
        GpuSubmissionLease lease) {
        if (submission_lease != nullptr &&
            lease != nullptr &&
            submission_lease.get() != lease.get()) {
            throw std::logic_error(
                "OpenXR composition frame cannot span GPU submission leases");
        }
        if (submission_lease == nullptr) {
            submission_lease = std::move(lease);
        }
    }

    void endView(
        std::uint32_t view_index,
        GpuSubmissionLease lease) {
        if (!logical_frame_begun || view_index != next_view ||
            view_index >= xr_stereo_view_count ||
            !views[view_index].view_begun || views[view_index].view_ended ||
            view_family_begun ||
            color.state != XrSwapchainState::waited) {
            throw std::logic_error("OpenXR composition view end is out of order");
        }
        try {
            graphics->submitView(
                view_index, in_flight_frame_index);
            retainSubmissionLease(std::move(lease));
            views[view_index].view_ended = true;
            color.state = XrSwapchainState::submitted;
            if (depth_submission_active) {
                depth.state = XrSwapchainState::submitted;
            }
            ++next_view;
            if (next_view < xr_stereo_view_count) {
                // The second layer remains available under the same acquired
                // array image even though the first command was submitted.
                color.state = XrSwapchainState::waited;
                if (depth_submission_active) {
                    depth.state = XrSwapchainState::waited;
                }
            }
        } catch (...) {
            teardown_required = true;
            abortFrame();
            throw;
        }
    }

    void endViewFamily(
        GpuSubmissionLease lease) {
        if (!logical_frame_begun ||
            !view_family_begun ||
            view_family_ended || next_view != 0 ||
            color.state != XrSwapchainState::waited) {
            throw std::logic_error(
                "OpenXR composition view-family end is out of order");
        }
        try {
            graphics->submitViewFamily(
                in_flight_frame_index);
            retainSubmissionLease(std::move(lease));
            view_family_ended = true;
            next_view = xr_stereo_view_count;
            color.state = XrSwapchainState::submitted;
            if (depth_submission_active) {
                depth.state = XrSwapchainState::submitted;
            }
        } catch (...) {
            teardown_required = true;
            abortFrame();
            throw;
        }
    }

    void endLogicalFrame(GpuSubmissionLease lease) {
        if (!logical_frame_begun || next_view != xr_stereo_view_count) {
            throw std::logic_error(
                "OpenXR composition logical frame ended before both submissions");
        }
        retainSubmissionLease(std::move(lease));
        try {
            if (view_family_ended) {
                graphics->waitForViewFamilySubmission(
                    in_flight_frame_index);
                view_family_submission_complete = true;
            } else {
                for (std::uint32_t view = 0;
                     view < xr_stereo_view_count;
                     ++view) {
                    graphics->waitForSubmission(
                        view, in_flight_frame_index);
                    views[view].submission_complete = true;
                }
            }
            if (depth_submission_active &&
                !releaseSwapchain(depth)) {
                throw std::runtime_error(
                    "xrReleaseSwapchainImage failed for composition depth");
            }
            if (!releaseSwapchain(color)) {
                throw std::runtime_error(
                    "xrReleaseSwapchainImage failed for composition color");
            }

            std::array<XrCompositionLayerProjectionView, xr_stereo_view_count>
                projection_views{
                    XrCompositionLayerProjectionView{
                        XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                    XrCompositionLayerProjectionView{
                        XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                };
            std::array<XrCompositionLayerDepthInfoKHR,
                       xr_stereo_view_count>
                depth_views{
                    XrCompositionLayerDepthInfoKHR{
                        XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
                    XrCompositionLayerDepthInfoKHR{
                        XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
                };
            for (std::uint32_t view = 0; view < xr_stereo_view_count; ++view) {
                projection_views[view].pose = located_views.views[view].pose;
                projection_views[view].fov = located_views.views[view].fov;
                projection_views[view].subImage.swapchain =
                    color.swapchain;
                projection_views[view].subImage.imageRect.offset = {0, 0};
                projection_views[view].subImage.imageRect.extent = {
                    static_cast<std::int32_t>(
                        capabilities.extent.width),
                    static_cast<std::int32_t>(
                        capabilities.extent.height),
                };
                projection_views[view].subImage.imageArrayIndex = view;
                if (depth_submission_active) {
                    auto &depth_view = depth_views[view];
                    depth_view.subImage.swapchain =
                        depth.swapchain;
                    depth_view.subImage.imageRect =
                        projection_views[view]
                            .subImage.imageRect;
                    depth_view.subImage.imageArrayIndex =
                        view;
                    depth_view.minDepth = 0.0F;
                    depth_view.maxDepth = 1.0F;
                    depth_view.nearZ = near_z;
                    depth_view.farZ = far_z;
                    projection_views[view].next =
                        &depth_view;
                }
            }
            XrCompositionLayerProjection projection_layer{
                XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection_layer.space = session_runtime.trackingSpace();
            projection_layer.viewCount = xr_stereo_view_count;
            projection_layer.views = projection_views.data();
            session_runtime.endFrame(
                *display_timing,
                reinterpret_cast<const XrCompositionLayerBaseHeader &>(
                    projection_layer));
        } catch (...) {
            abortFrame();
            throw;
        }
        submission_lease.reset();
        resetFrameFlags();
    }

    vk::Format colorFormat(std::uint32_t view_index) const {
        if (view_index >= xr_stereo_view_count) {
            throw std::out_of_range("OpenXR composition color format view is out of range");
        }
        return dependencies.renderer_color_format;
    }

    bool teardownRequired() const noexcept { return teardown_required; }

    XrSwapchainState state(std::uint32_t view_index) const {
        if (view_index >= xr_stereo_view_count) {
            throw std::out_of_range("OpenXR swapchain state view is out of range");
        }
        return color.state;
    }

    XrSwapchainState depthState() const {
        return depth.state;
    }

    bool supportsViewFamilyExecution() const noexcept {
        return capabilities.view_family_execution;
    }

    XrCompositionCapabilities compositionCapabilities() const {
        return capabilities;
    }
};

XrCompositionTarget::XrCompositionTarget(
    const XrCompositionDependencies &dependencies)
    : XrCompositionTarget(dependencies, makeProductionGraphics(dependencies)) {}

XrCompositionTarget::XrCompositionTarget(
    const XrCompositionDependencies &dependencies,
    std::unique_ptr<IXrCompositionGraphics> graphics)
    : impl{std::make_unique<Impl>(dependencies, std::move(graphics))} {}

XrCompositionTarget::~XrCompositionTarget() = default;

void XrCompositionTarget::prepareFrame(const XrDisplayTiming &display_timing,
                                       const XrLocatedViews &located_views,
                                       float near_z, float far_z) {
    impl->prepareFrame(
        display_timing, located_views, near_z, far_z);
}

void XrCompositionTarget::endFrameWithoutLayers(
    const XrDisplayTiming &display_timing) {
    impl->endFrameWithoutLayers(display_timing);
}

bool XrCompositionTarget::configureExternalDepthSubmission(
    vk::Format source_format,
    vk::Extent2D source_extent) {
    return impl->configureExternalDepthSubmission(
        source_format, source_extent);
}

void XrCompositionTarget::beginLogicalFrame(std::uint32_t view_count) {
    impl->beginLogicalFrame(view_count);
}

FrameRenderContext XrCompositionTarget::beginView(std::uint32_t view_index) {
    return impl->beginView(view_index);
}

bool XrCompositionTarget::supportsViewFamilyExecution() const noexcept {
    return impl->supportsViewFamilyExecution();
}

FrameRenderContext XrCompositionTarget::beginViewFamily(
    std::uint32_t view_count) {
    return impl->beginViewFamily(view_count);
}

void XrCompositionTarget::endView(
    std::uint32_t view_index,
    GpuSubmissionLease lease) {
    impl->endView(view_index, std::move(lease));
}

void XrCompositionTarget::endViewFamily(
    GpuSubmissionLease lease) {
    impl->endViewFamily(std::move(lease));
}

void XrCompositionTarget::endLogicalFrame(GpuSubmissionLease lease) {
    impl->endLogicalFrame(std::move(lease));
}

vk::Format XrCompositionTarget::colorFormat(std::uint32_t view_index) const {
    return impl->colorFormat(view_index);
}

bool XrCompositionTarget::consumeExtentChanged() { return false; }

bool XrCompositionTarget::generationTeardownRequired() const noexcept {
    return impl->teardownRequired();
}

XrSwapchainState
XrCompositionTarget::swapchainState(std::uint32_t view_index) const {
    return impl->state(view_index);
}

XrSwapchainState
XrCompositionTarget::depthSwapchainState() const {
    return impl->depthState();
}

XrCompositionCapabilities
XrCompositionTarget::compositionCapabilities() const {
    return impl->compositionCapabilities();
}

} // namespace Pelican::OpenXr
