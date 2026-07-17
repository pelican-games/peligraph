#include "openxrcompositiontarget.hpp"

#include "../vkcore/core.hpp"
#include "../vkcore/image.hpp"

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
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

vk::UniqueImageView createImageView(vk::Device device, vk::Image image,
                                    vk::Format format, vk::ImageAspectFlags aspect) {
    vk::ImageViewCreateInfo info;
    info.image = image;
    info.viewType = vk::ImageViewType::e2D;
    info.format = format;
    info.components = {
        vk::ComponentSwizzle::eR,
        vk::ComponentSwizzle::eG,
        vk::ComponentSwizzle::eB,
        vk::ComponentSwizzle::eA,
    };
    info.subresourceRange = {aspect, 0, 1, 0, 1};
    return device.createImageViewUnique(info);
}

class VulkanXrCompositionGraphics final : public IXrCompositionGraphics {
    struct ViewResources {
        vk::Extent2D extent;
        std::vector<vk::Image> images;
        std::vector<vk::UniqueImageView> image_views;
        ImageWrapper depth_image;
        vk::UniqueImageView depth_view;
    };

    VulkanManageCore &vulkan;
    vk::Device device;
    std::array<ViewResources, xr_stereo_view_count> views;
    std::vector<CommandBufWrapper> command_buffers;
    vk::Format color_format = vk::Format::eUndefined;
    vk::ImageLayout release_layout = runtime_release_layout;

    CommandBufWrapper &command(std::uint32_t view_index,
                               std::uint32_t in_flight_frame_index) {
        return command_buffers.at(static_cast<std::size_t>(in_flight_frame_index) *
                                      xr_stereo_view_count +
                                  view_index);
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
        release_layout = config.release_layout;
        command_buffers =
            vulkan.allocCmdBufs(in_flight_frames_num * xr_stereo_view_count);

        for (std::uint32_t view = 0; view < xr_stereo_view_count; ++view) {
            std::uint32_t image_count = 0;
            requireSuccess("xrEnumerateSwapchainImages",
                           config.enumerate_swapchain_images(config.swapchains[view], 0,
                                                             &image_count, nullptr));
            if (image_count == 0) {
                throw std::runtime_error("OpenXR swapchain has no Vulkan images");
            }
            std::vector<XrSwapchainImageVulkan2KHR> xr_images(
                image_count, XrSwapchainImageVulkan2KHR{
                                 XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
            std::uint32_t returned_count = 0;
            requireSuccess(
                "xrEnumerateSwapchainImages",
                config.enumerate_swapchain_images(
                    config.swapchains[view], image_count, &returned_count,
                    reinterpret_cast<XrSwapchainImageBaseHeader *>(xr_images.data())));
            if (returned_count != image_count) {
                throw std::runtime_error(
                    "OpenXR swapchain image count changed during enumeration");
            }

            auto &resources = views[view];
            resources.extent = vk::Extent2D{
                config.views[view].recommendedImageRectWidth,
                config.views[view].recommendedImageRectHeight};
            resources.images.reserve(image_count);
            resources.image_views.reserve(image_count);
            for (const auto &xr_image : xr_images) {
                const vk::Image image{xr_image.image};
                resources.images.push_back(image);
                resources.image_views.push_back(createImageView(
                    device, image, color_format, vk::ImageAspectFlagBits::eColor));
            }

            resources.depth_image = vulkan.allocImage(
                vk::Extent3D{resources.extent, 1}, vk::Format::eD32Sfloat,
                vk::ImageUsageFlagBits::eDepthStencilAttachment,
                vma::MemoryUsage::eAutoPreferDevice, {});
            resources.depth_view = createImageView(
                device, resources.depth_image.image.get(), resources.depth_image.format,
                vk::ImageAspectFlagBits::eDepth);
        }
    }

    FrameRenderContext beginView(std::uint32_t view_index,
                                 std::uint32_t image_index,
                                 std::uint32_t in_flight_frame_index) override {
        auto &resources = views.at(view_index);
        if (image_index >= resources.images.size()) {
            throw std::runtime_error(
                "OpenXR acquired swapchain image index is out of range");
        }
        auto &cmd = command(view_index, in_flight_frame_index);
        if (device.waitForFences({cmd.getFence()}, VK_TRUE, UINT64_MAX) !=
            vk::Result::eSuccess) {
            throw std::runtime_error("OpenXR composition command fence wait failed");
        }
        cmd.recordBegin();

        vk::ImageMemoryBarrier barrier;
        barrier.oldLayout = vk::ImageLayout::eUndefined;
        barrier.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = resources.images[image_index];
        barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        barrier.dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead |
                                vk::AccessFlagBits::eColorAttachmentWrite;
        cmd->pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                             vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {},
                             {}, {barrier});

        vk::Viewport viewport{0.0F, 0.0F,
                              static_cast<float>(resources.extent.width),
                              static_cast<float>(resources.extent.height), 0.0F, 1.0F};
        cmd->setViewport(0, {viewport});
        cmd->setScissor(0, {vk::Rect2D{{0, 0}, resources.extent}});

        return FrameRenderContext{
            .cmd_buf = *cmd,
            .color_attachment = resources.image_views[image_index].get(),
            .depth_attachment = resources.depth_view.get(),
            .extent = resources.extent,
            .image_prepared_semaphore = {},
            .required_layout = release_layout,
            .in_flight_frame_index = in_flight_frame_index,
        };
    }

    void submitView(std::uint32_t view_index,
                    std::uint32_t in_flight_frame_index) override {
        command(view_index, in_flight_frame_index).recordEndSubmit();
    }

    void waitForSubmission(std::uint32_t view_index,
                           std::uint32_t in_flight_frame_index) override {
        const auto &fence = command(view_index, in_flight_frame_index).getFence();
        if (device.waitForFences({fence}, VK_TRUE, UINT64_MAX) !=
            vk::Result::eSuccess) {
            throw std::runtime_error("OpenXR composition GPU submission failed to finish");
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
    struct ViewState {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        XrSwapchainState state = XrSwapchainState::idle;
        std::uint32_t image_index = 0;
        bool view_begun = false;
        bool view_ended = false;
        bool submission_complete = false;
        bool release_attempted = false;
    };

    XrCompositionDependencies dependencies;
    SessionRuntime &session_runtime;
    XrCompositionApi api;
    std::unique_ptr<IXrCompositionGraphics> graphics;
    std::array<XrViewConfigurationView, xr_stereo_view_count> view_configs{
        XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
        XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
    };
    std::array<ViewState, xr_stereo_view_count> views;
    std::optional<XrDisplayTiming> display_timing;
    XrLocatedViews located_views;
    std::uint32_t in_flight_frame_index = 0;
    std::uint32_t next_view = 0;
    bool frame_prepared = false;
    bool logical_frame_begun = false;
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
                view.maxSwapchainSampleCount < 1) {
                throw std::runtime_error(
                    "OpenXR view configuration cannot support the XR2a.1 swapchain");
            }
        }
    }

    std::int64_t selectFormat() {
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
        const auto expected = static_cast<std::int64_t>(
            static_cast<VkFormat>(dependencies.renderer_color_format));
        for (const auto format : formats) {
            if (format == expected) return format;
        }
        throw std::runtime_error(
            "OpenXR runtime does not support the renderer's compiled flat target format");
    }

    void createSwapchains(std::int64_t format) {
        for (std::uint32_t view = 0; view < xr_stereo_view_count; ++view) {
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            info.format = format;
            info.sampleCount = 1;
            info.width = view_configs[view].recommendedImageRectWidth;
            info.height = view_configs[view].recommendedImageRectHeight;
            info.faceCount = 1;
            info.arraySize = 1;
            info.mipCount = 1;
            const auto result = api.create_swapchain(session_runtime.sessionHandle(), &info,
                                                     &views[view].swapchain);
            if (isLossResult(result)) reportLoss(result);
            if (XR_FAILED(result) || result == XR_SESSION_LOSS_PENDING ||
                views[view].swapchain == XR_NULL_HANDLE) {
                views[view].swapchain = XR_NULL_HANDLE;
                throw XrCompositionError{"xrCreateSwapchain", result};
            }
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
        display_timing.reset();
        located_views = {};
        in_flight_frame_index =
            (in_flight_frame_index + 1) % in_flight_frames_num;
    }

    bool releaseView(std::uint32_t view_index) noexcept {
        auto &view = views[view_index];
        if (view.release_attempted) return false;
        view.release_attempted = true;
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const auto result = api.release_swapchain_image(view.swapchain, &info);
        if (XR_FAILED(result) || result == XR_SESSION_LOSS_PENDING) {
            teardown_required = true;
            if (isLossResult(result)) reportLoss(result);
            return false;
        }
        view.state = XrSwapchainState::released;
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
        for (std::uint32_t view_index = 0; view_index < xr_stereo_view_count;
             ++view_index) {
            auto &view = views[view_index];
            if (view.state == XrSwapchainState::submitted &&
                !view.submission_complete) {
                try {
                    graphics->waitForSubmission(view_index, in_flight_frame_index);
                    view.submission_complete = true;
                } catch (...) {
                    teardown_required = true;
                    continue;
                }
            }
            if (view.state == XrSwapchainState::waited ||
                (view.state == XrSwapchainState::submitted &&
                 view.submission_complete)) {
                (void)releaseView(view_index);
            } else if (view.state == XrSwapchainState::acquired) {
                // xrReleaseSwapchainImage before a successful wait is illegal.
                // Leave this image to generation teardown instead.
                teardown_required = true;
            }
        }
        closeZeroLayerAfterFailure();
        resetFrameFlags();
    }

    void acquireAndWait(std::uint32_t view_index) {
        auto &view = views[view_index];
        XrSwapchainImageAcquireInfo acquire_info{
            XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        const auto acquire_result = api.acquire_swapchain_image(
            view.swapchain, &acquire_info, &view.image_index);
        if (XR_FAILED(acquire_result) || acquire_result == XR_SESSION_LOSS_PENDING) {
            if (isLossResult(acquire_result)) reportLoss(acquire_result);
            throw XrCompositionError{"xrAcquireSwapchainImage", acquire_result};
        }
        view.state = XrSwapchainState::acquired;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait_info.timeout = dependencies.image_wait_timeout;
        while (true) {
            const auto wait_result =
                api.wait_swapchain_image(view.swapchain, &wait_info);
            if (wait_result == XR_TIMEOUT_EXPIRED) continue;
            if (XR_FAILED(wait_result) || wait_result == XR_SESSION_LOSS_PENDING) {
                if (isLossResult(wait_result)) reportLoss(wait_result);
                throw XrCompositionError{"xrWaitSwapchainImage", wait_result};
            }
            view.state = XrSwapchainState::waited;
            return;
        }
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
            const auto format = selectFormat();
            createSwapchains(format);
            this->graphics->initialize(XrCompositionGraphicsConfig{
                .enumerate_swapchain_images = api.enumerate_swapchain_images,
                .swapchains = {views[0].swapchain, views[1].swapchain},
                .views = view_configs,
                .color_format = dependencies.renderer_color_format,
                .release_layout = runtime_release_layout,
            });
        } catch (...) {
            this->graphics.reset();
            for (auto view = xr_stereo_view_count; view-- > 0;) {
                if (views[view].swapchain != XR_NULL_HANDLE) {
                    (void)api.destroy_swapchain(views[view].swapchain);
                    views[view].swapchain = XR_NULL_HANDLE;
                }
            }
            throw;
        }
    }

    ~Impl() {
        graphics.reset();
        for (auto view = xr_stereo_view_count; view-- > 0;) {
            if (views[view].swapchain != XR_NULL_HANDLE &&
                api.destroy_swapchain != nullptr) {
                (void)api.destroy_swapchain(views[view].swapchain);
            }
        }
    }

    void prepareFrame(const XrDisplayTiming &timing,
                      const XrLocatedViews &new_views) {
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
        display_timing.emplace(timing.predictedDisplayTime(),
                               timing.predictedDisplayPeriod(), true);
        located_views = new_views;
        frame_prepared = true;
    }

    void endFrameWithoutLayers(const XrDisplayTiming &timing) {
        if (frame_prepared || logical_frame_begun || timing.shouldRender()) {
            throw std::logic_error(
                "OpenXR zero-layer close requires an unprepared non-render frame");
        }
        session_runtime.endFrame(timing);
    }

    void beginLogicalFrame(std::uint32_t view_count) {
        if (!frame_prepared || logical_frame_begun ||
            view_count != xr_stereo_view_count) {
            throw std::logic_error(
                "OpenXR composition requires one prepared two-view logical frame");
        }
        for (auto &view : views) {
            if (view.state != XrSwapchainState::idle &&
                view.state != XrSwapchainState::released) {
                throw std::logic_error(
                    "OpenXR swapchain generation is not reusable");
            }
            view.state = XrSwapchainState::idle;
            view.view_begun = false;
            view.view_ended = false;
            view.submission_complete = false;
            view.release_attempted = false;
        }
        logical_frame_begun = true;
        next_view = 0;
        try {
            for (std::uint32_t view = 0; view < xr_stereo_view_count; ++view) {
                acquireAndWait(view);
            }
        } catch (...) {
            abortFrame();
            throw;
        }
    }

    FrameRenderContext beginView(std::uint32_t view_index) {
        if (!logical_frame_begun || view_index != next_view ||
            view_index >= xr_stereo_view_count ||
            views[view_index].state != XrSwapchainState::waited ||
            views[view_index].view_begun) {
            throw std::logic_error("OpenXR composition view begin is out of order");
        }
        try {
            auto context = graphics->beginView(view_index, views[view_index].image_index,
                                               in_flight_frame_index);
            views[view_index].view_begun = true;
            return context;
        } catch (...) {
            teardown_required = true;
            abortFrame();
            throw;
        }
    }

    void endView(std::uint32_t view_index) {
        if (!logical_frame_begun || view_index != next_view ||
            view_index >= xr_stereo_view_count ||
            !views[view_index].view_begun || views[view_index].view_ended ||
            views[view_index].state != XrSwapchainState::waited) {
            throw std::logic_error("OpenXR composition view end is out of order");
        }
        try {
            graphics->submitView(view_index, in_flight_frame_index);
            views[view_index].state = XrSwapchainState::submitted;
            views[view_index].view_ended = true;
            ++next_view;
        } catch (...) {
            teardown_required = true;
            abortFrame();
            throw;
        }
    }

    void endLogicalFrame() {
        if (!logical_frame_begun || next_view != xr_stereo_view_count) {
            throw std::logic_error(
                "OpenXR composition logical frame ended before both submissions");
        }
        try {
            for (std::uint32_t view = 0; view < xr_stereo_view_count; ++view) {
                graphics->waitForSubmission(view, in_flight_frame_index);
                views[view].submission_complete = true;
            }
            for (std::uint32_t view = 0; view < xr_stereo_view_count; ++view) {
                auto &state = views[view];
                state.release_attempted = true;
                XrSwapchainImageReleaseInfo info{
                    XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                const auto result = api.release_swapchain_image(state.swapchain, &info);
                if (XR_FAILED(result) || result == XR_SESSION_LOSS_PENDING) {
                    teardown_required = true;
                    if (isLossResult(result)) reportLoss(result);
                    throw XrCompositionError{"xrReleaseSwapchainImage", result};
                }
                state.state = XrSwapchainState::released;
            }

            std::array<XrCompositionLayerProjectionView, xr_stereo_view_count>
                projection_views{
                    XrCompositionLayerProjectionView{
                        XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                    XrCompositionLayerProjectionView{
                        XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                };
            for (std::uint32_t view = 0; view < xr_stereo_view_count; ++view) {
                projection_views[view].pose = located_views.views[view].pose;
                projection_views[view].fov = located_views.views[view].fov;
                projection_views[view].subImage.swapchain = views[view].swapchain;
                projection_views[view].subImage.imageRect.offset = {0, 0};
                projection_views[view].subImage.imageRect.extent = {
                    static_cast<std::int32_t>(
                        view_configs[view].recommendedImageRectWidth),
                    static_cast<std::int32_t>(
                        view_configs[view].recommendedImageRectHeight),
                };
                projection_views[view].subImage.imageArrayIndex = 0;
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
        return views[view_index].state;
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
                                       const XrLocatedViews &located_views) {
    impl->prepareFrame(display_timing, located_views);
}

void XrCompositionTarget::endFrameWithoutLayers(
    const XrDisplayTiming &display_timing) {
    impl->endFrameWithoutLayers(display_timing);
}

void XrCompositionTarget::beginLogicalFrame(std::uint32_t view_count) {
    impl->beginLogicalFrame(view_count);
}

FrameRenderContext XrCompositionTarget::beginView(std::uint32_t view_index) {
    return impl->beginView(view_index);
}

void XrCompositionTarget::endView(std::uint32_t view_index) {
    impl->endView(view_index);
}

void XrCompositionTarget::endLogicalFrame() { impl->endLogicalFrame(); }

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

} // namespace Pelican::OpenXr
