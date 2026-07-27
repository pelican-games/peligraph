#include "swapchainframetarget.hpp"

#include "../log.hpp"
#include "../os/window.hpp"
#include "../renderer/camera.hpp"
#include "core.hpp"
#include "image.hpp"
#include "rendertarget.hpp"
#include "swapchainrecovery.hpp"
#include "util.hpp"
#include "windowsurface.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Pelican {

namespace {

struct SwapchainWithFormat {
    vk::UniqueSwapchainKHR swapchain;
    vk::Format format = vk::Format::eUndefined;
    vk::ColorSpaceKHR color_space =
        vk::ColorSpaceKHR::eSrgbNonlinear;
    vk::Extent2D extent{};
    vk::ImageUsageFlags selected_usage{};
    vk::SurfaceTransformFlagBitsKHR surface_transform =
        vk::SurfaceTransformFlagBitsKHR::eIdentity;
    WsiPresentConfiguration present_configuration;
    bool capture_available = false;
    std::uint64_t surface_support_fingerprint = 0;
};

std::uint64_t hashWord(
    std::uint64_t hash, std::uint64_t value) noexcept {
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (std::uint32_t shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xffULL;
        hash *= prime;
    }
    return hash;
}

std::uint64_t surfaceSupportFingerprint(
    const vk::SurfaceCapabilitiesKHR &capabilities,
    const std::vector<vk::SurfaceFormatKHR> &formats,
    const std::vector<vk::PresentModeKHR> &present_modes) {
    auto hash = 14695981039346656037ULL;
    hash = hashWord(hash, capabilities.minImageCount);
    hash = hashWord(hash, capabilities.maxImageCount);
    hash = hashWord(hash, capabilities.currentExtent.width);
    hash = hashWord(hash, capabilities.currentExtent.height);
    hash = hashWord(hash, capabilities.minImageExtent.width);
    hash = hashWord(hash, capabilities.minImageExtent.height);
    hash = hashWord(hash, capabilities.maxImageExtent.width);
    hash = hashWord(hash, capabilities.maxImageExtent.height);
    hash = hashWord(
        hash,
        static_cast<VkImageUsageFlags>(
            capabilities.supportedUsageFlags));
    hash = hashWord(
        hash,
        static_cast<VkSurfaceTransformFlagsKHR>(
            capabilities.supportedTransforms));
    hash = hashWord(
        hash,
        static_cast<VkSurfaceTransformFlagBitsKHR>(
            capabilities.currentTransform));
    hash = hashWord(
        hash,
        static_cast<VkCompositeAlphaFlagsKHR>(
            capabilities.supportedCompositeAlpha));
    hash = hashWord(hash, formats.size());
    for (const auto &format : formats) {
        hash = hashWord(
            hash, static_cast<VkFormat>(format.format));
        hash = hashWord(
            hash,
            static_cast<VkColorSpaceKHR>(
                format.colorSpace));
    }
    hash = hashWord(hash, present_modes.size());
    for (const auto mode : present_modes) {
        hash = hashWord(
            hash, static_cast<VkPresentModeKHR>(mode));
    }
    return hash;
}

std::uint32_t chooseSwapchainImageCount(
    const vk::SurfaceCapabilitiesKHR &capabilities) {
    auto image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        image_count = std::min(
            image_count, capabilities.maxImageCount);
    }
    return image_count;
}

vk::Extent2D chooseSwapchainExtent(
    const vk::SurfaceCapabilitiesKHR &capabilities,
    vk::Extent2D framebuffer_extent) {
    if (capabilities.currentExtent.width !=
        std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }
    return vk::Extent2D{
        std::clamp(
            framebuffer_extent.width,
            capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width),
        std::clamp(
            framebuffer_extent.height,
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height),
    };
}

vk::CompositeAlphaFlagBitsKHR chooseCompositeAlpha(
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

SwapchainWithFormat createSwapchain(
    vk::Device device,
    vk::PhysicalDevice physical_device,
    vk::SurfaceKHR surface,
    FramebufferExtentSnapshot framebuffer,
    std::uint32_t graphics_queue_family,
    std::uint32_t presentation_queue_family,
    vk::SwapchainKHR old_swapchain) {
    if (framebuffer.extent.width == 0 ||
        framebuffer.extent.height == 0) {
        throw std::invalid_argument(
            "cannot prepare a swapchain for a zero-sized framebuffer");
    }

    const auto capabilities =
        physical_device.getSurfaceCapabilitiesKHR(surface);
    auto formats =
        physical_device.getSurfaceFormatsKHR(surface);
    auto present_modes =
        physical_device.getSurfacePresentModesKHR(surface);
    if (formats.empty()) {
        throw std::runtime_error(
            "No Vulkan surface formats available");
    }
    if (present_modes.empty()) {
        throw std::runtime_error(
            "No Vulkan present modes available");
    }
    const auto support_fingerprint =
        surfaceSupportFingerprint(
            capabilities, formats, present_modes);

    const auto format_score =
        [](vk::SurfaceFormatKHR value) {
            if ((value.format ==
                     vk::Format::eR8G8B8A8Srgb ||
                 value.format ==
                     vk::Format::eB8G8R8A8Srgb) &&
                value.colorSpace ==
                    vk::ColorSpaceKHR::eSrgbNonlinear) {
                return 20;
            }
            if ((value.format ==
                     vk::Format::eR8G8B8A8Unorm ||
                 value.format ==
                     vk::Format::eB8G8R8A8Unorm) &&
                value.colorSpace ==
                    vk::ColorSpaceKHR::eSrgbNonlinear) {
                return 10;
            }
            return 0;
        };
    std::stable_sort(
        formats.begin(), formats.end(),
        [&](const auto &left, const auto &right) {
            return format_score(left) >
                   format_score(right);
        });
    std::stable_sort(
        present_modes.begin(), present_modes.end(),
        [](const auto left, const auto right) {
            return (left == vk::PresentModeKHR::eMailbox) >
                   (right == vk::PresentModeKHR::eMailbox);
        });

    const auto selected_extent =
        chooseSwapchainExtent(
            capabilities, framebuffer.extent);
    const auto selected_format_features =
        physical_device
            .getFormatProperties(formats.front().format)
            .optimalTilingFeatures;
    const bool capture_available =
        static_cast<bool>(
            capabilities.supportedUsageFlags &
            vk::ImageUsageFlagBits::eTransferSrc) &&
        static_cast<bool>(
            selected_format_features &
            vk::FormatFeatureFlagBits::eTransferSrc);

    vk::SwapchainCreateInfoKHR create_info;
    create_info.surface = surface;
    create_info.minImageCount =
        chooseSwapchainImageCount(capabilities);
    create_info.imageFormat = formats.front().format;
    create_info.imageColorSpace =
        formats.front().colorSpace;
    create_info.imageExtent = selected_extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage =
        vk::ImageUsageFlagBits::eColorAttachment;
    if (capture_available) {
        create_info.imageUsage |=
            vk::ImageUsageFlagBits::eTransferSrc;
    }
    const std::array queue_families{
        graphics_queue_family,
        presentation_queue_family};
    if (graphics_queue_family ==
        presentation_queue_family) {
        create_info.imageSharingMode =
            vk::SharingMode::eExclusive;
    } else {
        create_info.imageSharingMode =
            vk::SharingMode::eConcurrent;
        create_info.setQueueFamilyIndices(queue_families);
    }
    create_info.preTransform =
        capabilities.currentTransform;
    create_info.presentMode = present_modes.front();
    create_info.compositeAlpha =
        chooseCompositeAlpha(
            capabilities.supportedCompositeAlpha);
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = old_swapchain;

    return SwapchainWithFormat{
        .swapchain =
            device.createSwapchainKHRUnique(create_info),
        .format = formats.front().format,
        .color_space = formats.front().colorSpace,
        .extent = selected_extent,
        .selected_usage = create_info.imageUsage,
        .surface_transform = create_info.preTransform,
        .present_configuration =
            WsiPresentConfiguration{
                .present_mode = create_info.presentMode,
                .image_count = create_info.minImageCount,
                .composite_alpha =
                    create_info.compositeAlpha,
                .clipped = true,
            },
        .capture_available = capture_available,
        .surface_support_fingerprint =
            support_fingerprint,
    };
}

std::vector<vk::UniqueImageView> createColorImageViews(
    vk::Device device,
    const std::vector<vk::Image> &images,
    vk::Format format) {
    std::vector<vk::UniqueImageView> result;
    result.reserve(images.size());
    for (const auto image : images) {
        vk::ImageViewCreateInfo create_info;
        create_info.image = image;
        create_info.viewType = vk::ImageViewType::e2D;
        create_info.format = format;
        create_info.components = {
            vk::ComponentSwizzle::eR,
            vk::ComponentSwizzle::eG,
            vk::ComponentSwizzle::eB,
            vk::ComponentSwizzle::eA};
        create_info.subresourceRange = {
            vk::ImageAspectFlagBits::eColor,
            0, 1, 0, 1};
        result.push_back(
            device.createImageViewUnique(create_info));
    }
    return result;
}

vk::UniqueImageView createDepthImageView(
    vk::Device device, const ImageWrapper &image) {
    vk::ImageViewCreateInfo create_info;
    create_info.image = image.image.get();
    create_info.viewType = vk::ImageViewType::e2D;
    create_info.format = image.format;
    create_info.components = {
        vk::ComponentSwizzle::eR,
        vk::ComponentSwizzle::eG,
        vk::ComponentSwizzle::eB,
        vk::ComponentSwizzle::eA};
    create_info.subresourceRange = {
        vk::ImageAspectFlagBits::eDepth,
        0, 1, 0, 1};
    return device.createImageViewUnique(create_info);
}

std::vector<vk::UniqueSemaphore> createSemaphores(
    vk::Device device, std::size_t count) {
    std::vector<vk::UniqueSemaphore> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count;
         ++index) {
        result.push_back(
            device.createSemaphoreUnique({}));
    }
    return result;
}

OutputCompileFacts outputFacts(
    const SwapchainWithFormat &swapchain,
    std::uint32_t graphics_queue_family,
    std::uint32_t presentation_queue_family) {
    return OutputCompileFacts{
        .target_kind = OutputTargetKind::window,
        .extent = swapchain.extent,
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
        .selected_usage = swapchain.selected_usage,
        .capture_available =
            swapchain.capture_available,
        .surface_transform =
            swapchain.surface_transform,
        .graphics_queue_family =
            graphics_queue_family,
        .presentation_queue_family =
            presentation_queue_family,
    };
}

vk::Result resultFromSystemError(
    const vk::SystemError &error) noexcept {
    return static_cast<vk::Result>(
        error.code().value());
}

} // namespace

struct SwapchainFrameTarget::Impl {
    struct SurfaceEpoch {
        SurfaceEpochId id = 0;
        vk::UniqueSurfaceKHR surface;
        vk::Queue presentation_queue;
        std::uint32_t presentation_queue_family =
            VK_QUEUE_FAMILY_IGNORED;
        BasePresentRetirementTracker
            base_retirement;
        bool lost = false;
        mutable std::mutex host_access;
    };

    struct SwapchainEpoch {
        // This lease precedes the swapchain so reverse member destruction
        // destroys every swapchain child before releasing its VkSurfaceKHR.
        std::shared_ptr<SurfaceEpoch> surface_epoch;
        vk::Device device;
        SwapchainEpochId id = 0;
        FramebufferExtentSnapshot framebuffer;
        OutputCompileFacts output_facts;
        SwapchainRecoveryKey recovery_key;

        // Pools precede their children so reverse member destruction releases
        // command buffers before the command pool.
        vk::UniqueCommandPool command_pool;
        std::array<CommandBufWrapper,
                   in_flight_frames_num>
            command_buffers;
        GpuSubmissionLeaseSlots<
            in_flight_frames_num>
            submission_leases;
        std::array<bool, in_flight_frames_num>
            submission_pending{};

        // The swapchain precedes every dependent view/synchronization object,
        // causing those objects to be destroyed first.
        SwapchainWithFormat swapchain;
        std::vector<vk::Image> images;
        std::vector<vk::UniqueImageView> image_views;
        ImageWrapper depth_image;
        vk::UniqueImageView depth_image_view;
        std::vector<vk::UniqueSemaphore>
            acquire_semaphores;
        std::vector<vk::UniqueSemaphore>
            present_semaphores;
        std::vector<vk::UniqueFence> present_fences;
        std::vector<bool> present_fence_pending;
        std::vector<vk::UniqueFence>
            superseded_present_fences;
        std::vector<bool> present_wait_pending;
        bool ever_presented = false;
        bool has_rendered_frame = false;
        std::uint32_t last_rendered_image = 0;
        std::uint32_t next_slot = 0;
        mutable std::mutex host_access;

        bool pollGpuCompletion() {
            bool all_complete = true;
            for (std::size_t slot = 0;
                 slot < submission_pending.size();
                 ++slot) {
                if (!submission_pending[slot]) continue;
                const auto status = device.getFenceStatus(
                    command_buffers[slot].getFence());
                if (status == vk::Result::eSuccess) {
                    submission_pending[slot] = false;
                    submission_leases.complete(slot);
                } else if (
                    status == vk::Result::eNotReady) {
                    all_complete = false;
                } else {
                    throw std::runtime_error(
                        "failed to poll swapchain submission fence: " +
                        vk::to_string(status));
                }
            }
            return all_complete;
        }

        bool pollPresentCompletion(
            bool maintenance1) {
            if (!maintenance1) {
                return !ever_presented;
            }
            bool all_complete = true;
            for (std::size_t image = 0;
                 image < present_fence_pending.size();
                 ++image) {
                if (!present_fence_pending[image]) {
                    continue;
                }
                const auto status = device.getFenceStatus(
                    present_fences[image].get());
                if (status == vk::Result::eSuccess) {
                    present_fence_pending[image] = false;
                } else if (
                    status == vk::Result::eNotReady) {
                    all_complete = false;
                } else {
                    throw std::runtime_error(
                        "failed to poll swapchain present fence: " +
                        vk::to_string(status));
                }
            }
            auto first_pending =
                std::remove_if(
                    superseded_present_fences.begin(),
                    superseded_present_fences.end(),
                    [&](const auto &fence) {
                        const auto status =
                            device.getFenceStatus(
                                fence.get());
                        if (status ==
                            vk::Result::eSuccess) {
                            return true;
                        }
                        if (status ==
                            vk::Result::eNotReady) {
                            all_complete = false;
                            return false;
                        }
                        throw std::runtime_error(
                            "failed to poll superseded present fence: " +
                            vk::to_string(status));
                    });
            superseded_present_fences.erase(
                first_pending,
                superseded_present_fences.end());
            return all_complete;
        }

        void noteAcquired(
            std::uint32_t image_index,
            bool maintenance1) {
            if (image_index >=
                present_wait_pending.size()) {
                throw std::runtime_error(
                    "vkAcquireNextImageKHR returned an image outside the epoch");
            }
            if (!present_wait_pending[image_index]) {
                return;
            }
            present_wait_pending[image_index] = false;
            if (!maintenance1) {
                surface_epoch->base_retirement
                    .noteSuccessorImageReacquired(
                        id, true);
            }
        }

        vk::Fence preparePresentFence(
            std::uint32_t image_index) {
            if (image_index >= present_fences.size()) {
                throw std::logic_error(
                    "present fence image index is outside the epoch");
            }
            if (present_fence_pending[image_index]) {
                const auto status = device.getFenceStatus(
                    present_fences[image_index].get());
                if (status == vk::Result::eSuccess) {
                    device.resetFences(
                        {present_fences[image_index].get()});
                    present_fence_pending[image_index] =
                        false;
                } else if (
                    status == vk::Result::eNotReady) {
                    superseded_present_fences.push_back(
                        std::move(
                            present_fences[image_index]));
                    present_fences[image_index] =
                        device.createFenceUnique({});
                    present_fence_pending[image_index] =
                        false;
                } else {
                    throw std::runtime_error(
                        "failed to recycle a swapchain present fence: " +
                        vk::to_string(status));
                }
            }
            return present_fences[image_index].get();
        }
    };

    enum class ActiveFramePhase {
        acquired,
        recording,
        submitted,
    };

    struct ActiveFrame {
        std::uint64_t serial = 0;
        std::shared_ptr<SwapchainEpoch> epoch;
        std::uint32_t slot = 0;
        std::uint32_t image_index = 0;
        FrameBeginMode begin_mode =
            FrameBeginMode::blocking;
        ActiveFramePhase phase =
            ActiveFramePhase::acquired;
        bool output_transform_recorded = false;
        bool refresh_after_present = false;
    };

    struct PendingAbandonment {
        std::shared_ptr<SwapchainEpoch> epoch;
        std::uint32_t image_index = 0;
    };

    struct BuildRequest {
        SwapchainEpochId epoch_id = 0;
        SurfaceEpochId surface_epoch_id = 0;
        SwapchainPreparationRequest policy;
        FramebufferExtentSnapshot framebuffer;
        std::shared_ptr<SurfaceEpoch>
            surface_epoch;
        std::shared_ptr<SwapchainEpoch> old_epoch;
        std::optional<std::uint32_t>
            release_image_index;
    };

    struct BuildCompletion {
        BuildRequest request;
        std::shared_ptr<SwapchainEpoch> candidate;
        // Set only when vkCreateSwapchainKHR succeeded but a dependent
        // resource failed. This becomes oldSwapchain for the next attempt;
        // the request's old epoch was retired by the successful create.
        std::shared_ptr<SwapchainEpoch>
            replacement_anchor;
        SurfaceDeviceRebuildReason
            device_rebuild_reason =
                SurfaceDeviceRebuildReason::none;
        vk::Result failure_result =
            vk::Result::eSuccess;
        std::string error;
    };

    struct PumpResult {
        bool output_facts_changed = false;
    };

    VulkanManageCore &core;
    Window &window;
    WindowSurfaceFactory surface_factory;
    Camera &camera;
    vk::Device device;
    vk::PhysicalDevice physical_device;
    vk::Queue graphics_queue;
    std::uint32_t graphics_queue_family = 0;
    std::uint32_t
        current_presentation_queue_family = 0;
    std::vector<std::uint32_t>
        created_queue_families;
    bool maintenance1 = false;

    std::shared_ptr<SwapchainEpoch> active_epoch;
    // A complete stale candidate, a partially built replacement, or the
    // previous active epoch after create-before-retire failed. Exactly one
    // non-retired swapchain is retained here between preparation attempts.
    std::shared_ptr<SwapchainEpoch>
        recovery_anchor;
    std::vector<std::shared_ptr<SwapchainEpoch>>
        retired_epochs;
    SwapchainEpochId next_epoch_id = 1;
    SurfaceEpochId next_surface_epoch_id = 1;
    SurfaceEpochId published_surface_epoch_id = 0;
    std::uint64_t surface_recovery_count = 0;
    SurfaceDeviceRebuildReason
        device_rebuild_reason =
            SurfaceDeviceRebuildReason::none;
    FrameTargetCaps published_caps;
    WsiPresentConfiguration
        last_present_configuration;
    std::uint64_t last_support_fingerprint = 0;

    std::unique_ptr<
        WindowOutputRecoveryStateMachine>
        recovery;
    std::uint64_t maintenance_tick = 0;
    vk::Result last_wsi_result =
        vk::Result::eSuccess;
    std::optional<ActiveFrame> active_frame;
    std::optional<PendingAbandonment>
        pending_abandonment;
    std::uint64_t next_frame_serial = 1;
    std::shared_ptr<FrameTargetFrameCleanup>
        frame_cleanup;

    std::mutex worker_mutex;
    std::condition_variable_any worker_wakeup;
    std::optional<BuildRequest> worker_request;
    std::optional<BuildCompletion>
        worker_completion;
    bool worker_busy = false;
    std::jthread worker;

    static void cleanupAbandonedFrame(
        void *owner, std::uint64_t serial) noexcept {
        static_cast<Impl *>(owner)
            ->abandonFrameSerial(serial);
    }

    Impl()
        : core{GET_MODULE(VulkanManageCore)},
          window{GET_MODULE(Window)},
          surface_factory{
              core.getInstance(), window},
          camera{GET_MODULE(Camera)},
          device{core.getDevice()},
          physical_device{core.getPhysDevice()},
          graphics_queue{core.getGraphicsQueue()},
          graphics_queue_family{
              core.getGraphicsQueueFamilyIndex()},
          current_presentation_queue_family{
              core
                  .getBootstrapPresentationQueueFamilyIndex()},
          created_queue_families{
              core.createdQueueFamilyIndices()},
          maintenance1{
              core.getRuntimeCapabilities()
                  .swapchain_maintenance1} {
        frame_cleanup =
            std::make_shared<FrameTargetFrameCleanup>(
                this, &Impl::cleanupAbandonedFrame);

        const auto framebuffer =
            window.framebufferSnapshot();
        if (framebuffer.extent.width == 0 ||
            framebuffer.extent.height == 0) {
            throw std::runtime_error(
                "initial window framebuffer has zero extent; "
                "swapchain bootstrap cannot block the engine loop");
        }
        auto initial_surface =
            core.takeInitialWindowSurface();
        if (!initial_surface) {
            throw std::runtime_error(
                "window frame target requires the one-shot bootstrap surface");
        }
        const auto initial_queue =
            core.createdQueue(
                current_presentation_queue_family);
        if (!initial_queue) {
            throw std::logic_error(
                "bootstrap presentation queue was not created");
        }
        auto initial_surface_epoch =
            std::make_shared<SurfaceEpoch>();
        initial_surface_epoch->id =
            next_surface_epoch_id++;
        initial_surface_epoch->surface =
            std::move(initial_surface->surface);
        initial_surface_epoch->presentation_queue =
            *initial_queue;
        initial_surface_epoch
            ->presentation_queue_family =
            current_presentation_queue_family;
        BuildRequest initial_request{
            .epoch_id = next_epoch_id++,
            .surface_epoch_id =
                initial_surface_epoch->id,
            .framebuffer = framebuffer,
            .surface_epoch =
                std::move(initial_surface_epoch),
        };
        active_epoch = buildEpoch(initial_request);
        published_surface_epoch_id =
            active_epoch->surface_epoch->id;
        published_caps.compile_facts =
            active_epoch->output_facts;
        last_present_configuration =
            active_epoch->swapchain
                .present_configuration;
        last_support_fingerprint =
            active_epoch->swapchain
                .surface_support_fingerprint;
        recovery = std::make_unique<
            WindowOutputRecoveryStateMachine>(
            active_epoch->id,
            active_epoch->recovery_key);
        camera.setScreenSize(
            active_epoch->output_facts.extent.width,
            active_epoch->output_facts.extent.height);

        worker = std::jthread(
            [this](std::stop_token stop) {
                workerMain(stop);
            });
        LOG_INFO(
            logger,
            "swapchain epoch initialized: epoch={} extent={}x{} "
            "surface_epoch={} maintenance1={}",
            active_epoch->id,
            active_epoch->output_facts.extent.width,
            active_epoch->output_facts.extent.height,
            active_epoch->surface_epoch->id,
            maintenance1);
    }

    ~Impl() {
        frame_cleanup->detach(this);
        worker.request_stop();
        worker_wakeup.notify_all();
        if (worker.joinable()) worker.join();

        try {
            pollRetiredEpochs();
        } catch (...) {
        }

        std::vector<std::shared_ptr<SwapchainEpoch>>
            quarantine;
        std::unordered_set<const SwapchainEpoch *> seen;
        const auto retain =
            [&](std::shared_ptr<SwapchainEpoch> epoch) {
                if (epoch == nullptr ||
                    !seen.insert(epoch.get()).second) {
                    return;
                }
                bool gpu_complete = false;
                bool present_complete = false;
                try {
                    gpu_complete =
                        epoch->pollGpuCompletion();
                    present_complete =
                        epoch->pollPresentCompletion(
                            maintenance1);
                } catch (...) {
                }
                if (!gpu_complete ||
                    !present_complete) {
                    quarantine.push_back(
                        std::move(epoch));
                }
            };

        retain(std::move(active_epoch));
        retain(std::move(recovery_anchor));
        for (auto &epoch : retired_epochs) {
            retain(std::move(epoch));
        }
        retired_epochs.clear();
        if (worker_request) {
            retain(std::move(
                worker_request->old_epoch));
        }
        if (worker_completion) {
            retain(std::move(
                worker_completion->request.old_epoch));
            // An unpublished candidate has never been submitted or
            // presented and can be destroyed immediately.
            worker_completion->candidate.reset();
            worker_completion
                ->replacement_anchor.reset();
        }

        for (auto &epoch : quarantine) {
            try {
                core.quarantinePresentationResources(
                    std::move(epoch));
            } catch (...) {
                // Vulkan device teardown is the final fallback for the base
                // swapchain contract. Preserve destructor noexcept behavior.
            }
        }
    }

    std::shared_ptr<SwapchainEpoch> buildEpoch(
        const BuildRequest &request,
        std::shared_ptr<SwapchainEpoch>
            *replacement_anchor = nullptr) {
        if (request.surface_epoch == nullptr ||
            !request.surface_epoch->surface) {
            throw std::logic_error(
                "swapchain preparation requires a live surface epoch");
        }
        if (request.old_epoch != nullptr &&
            request.old_epoch->surface_epoch !=
                request.surface_epoch) {
            throw std::logic_error(
                "oldSwapchain cannot cross surface epochs");
        }
        auto epoch =
            std::make_shared<SwapchainEpoch>();
        epoch->surface_epoch =
            request.surface_epoch;
        epoch->device = device;
        epoch->id = request.epoch_id;
        epoch->framebuffer = request.framebuffer;

        if (request.old_epoch != nullptr) {
            std::scoped_lock lock{
                request.surface_epoch->host_access,
                request.old_epoch->host_access};
            releaseAbandonedImage(request);
            epoch->swapchain = createSwapchain(
                device, physical_device,
                request.surface_epoch->surface.get(),
                request.framebuffer,
                graphics_queue_family,
                request.surface_epoch
                    ->presentation_queue_family,
                request.old_epoch->swapchain
                    .swapchain.get());
        } else {
            std::scoped_lock lock{
                request.surface_epoch->host_access};
            epoch->swapchain = createSwapchain(
                device, physical_device,
                request.surface_epoch->surface.get(),
                request.framebuffer,
                graphics_queue_family,
                request.surface_epoch
                    ->presentation_queue_family,
                {});
        }
        // vkCreateSwapchainKHR success retires oldSwapchain immediately.
        // Preserve the new handle before creating any dependent object so a
        // later failure still has a valid non-retired anchor for the retry.
        if (replacement_anchor != nullptr) {
            *replacement_anchor = epoch;
        }

        epoch->images =
            device.getSwapchainImagesKHR(
                epoch->swapchain.swapchain.get());
        epoch->image_views = createColorImageViews(
            device, epoch->images,
            epoch->swapchain.format);
        epoch->depth_image = core.allocImage(
            vk::Extent3D{
                epoch->swapchain.extent, 1},
            vk::Format::eD32Sfloat,
            vk::ImageUsageFlagBits::
                eDepthStencilAttachment,
            vma::MemoryUsage::eAutoPreferDevice,
            {});
        epoch->depth_image_view =
            createDepthImageView(
                device, epoch->depth_image);
        epoch->acquire_semaphores =
            createSemaphores(
                device, in_flight_frames_num);
        epoch->present_semaphores =
            createSemaphores(
                device, epoch->images.size());
        epoch->present_wait_pending.assign(
            epoch->images.size(), false);

        if (maintenance1) {
            epoch->present_fences.reserve(
                epoch->images.size());
            for (std::size_t image = 0;
                 image < epoch->images.size();
                 ++image) {
                epoch->present_fences.push_back(
                    device.createFenceUnique({}));
            }
            epoch->present_fence_pending.assign(
                epoch->images.size(), false);
        }

        vk::CommandPoolCreateInfo pool_info;
        pool_info.flags =
            vk::CommandPoolCreateFlagBits::
                eResetCommandBuffer;
        pool_info.queueFamilyIndex =
            graphics_queue_family;
        epoch->command_pool =
            device.createCommandPoolUnique(pool_info);
        vk::CommandBufferAllocateInfo allocation_info;
        allocation_info.commandPool =
            epoch->command_pool.get();
        allocation_info.level =
            vk::CommandBufferLevel::ePrimary;
        allocation_info.commandBufferCount =
            in_flight_frames_num;
        auto command_buffers =
            device.allocateCommandBuffersUnique(
                allocation_info);
        vk::FenceCreateInfo submission_fence_info;
        submission_fence_info.flags =
            vk::FenceCreateFlagBits::eSignaled;
        for (std::size_t slot = 0;
             slot < in_flight_frames_num; ++slot) {
            epoch->command_buffers[slot] =
                CommandBufWrapper{
                    device, graphics_queue,
                    std::move(command_buffers[slot]),
                    device.createFenceUnique(
                        submission_fence_info)};
        }

        epoch->output_facts = outputFacts(
            epoch->swapchain,
            graphics_queue_family,
            request.surface_epoch
                ->presentation_queue_family);
        epoch->recovery_key =
            SwapchainRecoveryKey{
                .framebuffer_revision =
                    request.framebuffer.revision,
                .framebuffer_extent =
                    request.framebuffer.extent,
                .surface_support_fingerprint =
                    epoch->swapchain
                        .surface_support_fingerprint,
                .output_facts_fingerprint =
                    outputCompileFactsFingerprint(
                        epoch->output_facts),
                .present_configuration_fingerprint =
                    wsiPresentConfigurationFingerprint(
                        epoch->swapchain
                            .present_configuration),
            };

        const auto &debug_utils =
            core.getDebugUtils();
        const auto epoch_name =
            "swapchain/" +
            std::to_string(epoch->id);
        debug_utils.nameSwapchain(
            epoch->swapchain.swapchain.get(),
            epoch_name.c_str());
        for (std::size_t image = 0;
             image < epoch->images.size(); ++image) {
            const auto base =
                epoch_name + "/image/" +
                std::to_string(image);
            debug_utils.nameImage(
                epoch->images[image],
                (base + "/image").c_str());
            debug_utils.nameImageView(
                epoch->image_views[image].get(),
                (base + "/view").c_str());
        }
        debug_utils.nameImage(
            epoch->depth_image.image.get(),
            (epoch_name + "/depth/image").c_str());
        debug_utils.nameImageView(
            epoch->depth_image_view.get(),
            (epoch_name + "/depth/view").c_str());
        return epoch;
    }

    void releaseAbandonedImage(
        const BuildRequest &request) {
        if (!maintenance1 ||
            request.old_epoch == nullptr ||
            !request.release_image_index) {
            return;
        }
        try {
            const auto image_index =
                *request.release_image_index;
            vk::ReleaseSwapchainImagesInfoEXT info;
            info.swapchain =
                request.old_epoch->swapchain
                    .swapchain.get();
            info.imageIndexCount = 1;
            info.pImageIndices = &image_index;
            const auto result =
                core.releaseSwapchainImages(info);
            if (result != vk::Result::eSuccess) {
                throw std::runtime_error(
                    "vkReleaseSwapchainImagesEXT failed: " +
                    vk::to_string(result));
            }
        } catch (const std::exception &error) {
            LOG_WARNING(
                logger,
                "swapchain epoch {} could not release abandoned image "
                "before cutover: {}",
                request.old_epoch->id, error.what());
        }
    }

    void workerMain(std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::optional<BuildRequest> request;
            {
                std::unique_lock lock{worker_mutex};
                worker_wakeup.wait(
                    lock, stop, [&] {
                        return worker_request.has_value();
                    });
                if (stop.stop_requested()) return;
                request.emplace(
                    std::move(*worker_request));
                worker_request.reset();
            }

            BuildCompletion completion{
                .request = std::move(*request)};
            std::shared_ptr<SwapchainEpoch>
                replacement_anchor;
            try {
                if (completion.request.policy
                        .preparation_kind ==
                    WindowOutputPreparationKind::
                        surface) {
                    auto prepared_surface =
                        surface_factory.create();
                    const auto binding =
                        querySurfacePresentationQueue(
                            physical_device,
                            prepared_surface.surface.get(),
                            current_presentation_queue_family,
                            created_queue_families);
                    if (binding.kind ==
                        SurfaceQueueBindingKind::
                            device_rebuild_required) {
                        completion.device_rebuild_reason =
                            binding.rebuild_reason;
                    } else {
                        const auto queue =
                            core.createdQueue(
                                binding
                                    .presentation_queue_family);
                        if (!queue) {
                            throw std::logic_error(
                                "surface policy selected an uncreated queue");
                        }
                        auto surface_epoch =
                            std::make_shared<
                                SurfaceEpoch>();
                        surface_epoch->id =
                            completion.request
                                .surface_epoch_id;
                        surface_epoch->surface =
                            std::move(
                                prepared_surface.surface);
                        surface_epoch
                            ->presentation_queue =
                            *queue;
                        surface_epoch
                            ->presentation_queue_family =
                            binding
                                .presentation_queue_family;
                        completion.request
                            .surface_epoch =
                            std::move(surface_epoch);
                    }
                }
                if (completion.device_rebuild_reason !=
                    SurfaceDeviceRebuildReason::none) {
                    completion.failure_result =
                        vk::Result::
                            eErrorInitializationFailed;
                    completion.error =
                        "fresh surface requires Vulkan device rebuild: " +
                        std::string{
                            surfaceDeviceRebuildReasonName(
                                completion
                                    .device_rebuild_reason)};
                } else {
                    completion.candidate =
                        buildEpoch(
                            completion.request,
                            &replacement_anchor);
                }
            } catch (const vk::SystemError &error) {
                completion.failure_result =
                    resultFromSystemError(error);
                completion.error = error.what();
            } catch (const std::bad_alloc &error) {
                completion.failure_result =
                    vk::Result::eErrorOutOfHostMemory;
                completion.error = error.what();
            } catch (const std::exception &error) {
                completion.failure_result =
                    vk::Result::eErrorUnknown;
                completion.error = error.what();
            } catch (...) {
                completion.failure_result =
                    vk::Result::eErrorUnknown;
                completion.error =
                    "unknown swapchain preparation failure";
            }
            if (completion.candidate == nullptr) {
                completion.replacement_anchor =
                    std::move(replacement_anchor);
            }
            {
                std::scoped_lock lock{worker_mutex};
                worker_completion.emplace(
                    std::move(completion));
            }
        }
    }

    SwapchainRecoveryKey requestedRecoveryKey(
        FramebufferExtentSnapshot framebuffer) const {
        return SwapchainRecoveryKey{
            .framebuffer_revision =
                framebuffer.revision,
            .framebuffer_extent =
                framebuffer.extent,
            .surface_support_fingerprint =
                active_epoch != nullptr
                    ? active_epoch->swapchain
                          .surface_support_fingerprint
                    : last_support_fingerprint,
            .output_facts_fingerprint =
                outputCompileFactsFingerprint(
                    published_caps.compile_facts),
            .present_configuration_fingerprint =
                wsiPresentConfigurationFingerprint(
                    active_epoch != nullptr
                        ? active_epoch->swapchain
                              .present_configuration
                        : last_present_configuration),
        };
    }

    void retireEpoch(
        std::shared_ptr<SwapchainEpoch> epoch) {
        if (epoch == nullptr) return;
        const auto duplicate = std::find_if(
            retired_epochs.begin(),
            retired_epochs.end(),
            [&](const auto &existing) {
                return existing.get() == epoch.get();
            });
        if (duplicate == retired_epochs.end()) {
            retired_epochs.push_back(
                std::move(epoch));
        }
    }

    void markSurfaceLost(
        const std::shared_ptr<SurfaceEpoch>
            &lost_surface,
        SwapchainEpochId observed_epoch) {
        if (lost_surface != nullptr) {
            lost_surface->lost = true;
        }
        const auto belongs_to_lost_surface =
            [&](const auto &epoch) {
                return lost_surface != nullptr &&
                       epoch != nullptr &&
                       epoch->surface_epoch ==
                           lost_surface;
            };
        if (belongs_to_lost_surface(active_epoch)) {
            retireEpoch(std::move(active_epoch));
        }
        if (belongs_to_lost_surface(
                recovery_anchor)) {
            retireEpoch(
                std::move(recovery_anchor));
        }
        if (pending_abandonment &&
            belongs_to_lost_surface(
                pending_abandonment->epoch)) {
            pending_abandonment.reset();
        }
        recovery->markSurfaceLost(observed_epoch);
    }

    void launchPendingPreparation(
        FramebufferExtentSnapshot framebuffer) {
        if (worker_busy) return;
        auto policy =
            recovery->kind() ==
                    WindowOutputStateKind::
                        surface_lost
                ? recovery
                      ->takeSurfacePreparationRequest(
                          requestedRecoveryKey(
                              framebuffer))
                : recovery->takePreparationRequest();
        if (!policy) return;

        BuildRequest request{
            .epoch_id = next_epoch_id++,
            .surface_epoch_id =
                policy->preparation_kind ==
                        WindowOutputPreparationKind::
                            surface
                    ? next_surface_epoch_id++
                    : 0,
            .policy = *policy,
            .framebuffer =
                FramebufferExtentSnapshot{
                    .extent =
                        policy->key.framebuffer_extent,
                    .revision =
                        policy->key
                            .framebuffer_revision},
        };
        if (policy->preparation_kind ==
            WindowOutputPreparationKind::
                swapchain) {
            if (active_epoch != nullptr) {
                request.old_epoch =
                    std::move(active_epoch);
            } else if (
                recovery_anchor != nullptr) {
                request.old_epoch =
                    std::move(recovery_anchor);
            }
            if (request.old_epoch == nullptr) {
                recovery->markFatal();
                throw std::logic_error(
                    "swapchain preparation has no surface anchor");
            }
            request.surface_epoch =
                request.old_epoch->surface_epoch;
        } else {
            if (active_epoch != nullptr ||
                recovery_anchor != nullptr) {
                throw std::logic_error(
                    "surface preparation retained a lost active swapchain");
            }
        }
        if (pending_abandonment) {
            if (request.old_epoch ==
                pending_abandonment->epoch) {
                request.release_image_index =
                    pending_abandonment->image_index;
            }
            pending_abandonment.reset();
        }

        {
            std::scoped_lock lock{worker_mutex};
            if (worker_request ||
                worker_completion) {
                throw std::logic_error(
                    "swapchain maintenance worker already owns a request");
            }
            worker_request.emplace(
                std::move(request));
            worker_busy = true;
        }
        worker_wakeup.notify_one();
    }

    void pollRetiredEpochs() {
        auto first_live =
            std::remove_if(
                retired_epochs.begin(),
                retired_epochs.end(),
                [&](const auto &epoch) {
                    const bool gpu_complete =
                        epoch->pollGpuCompletion();
                    bool present_complete = false;
                    if (maintenance1) {
                        present_complete =
                            epoch->pollPresentCompletion(
                                true);
                    } else {
                        present_complete =
                            epoch->surface_epoch
                                ->base_retirement
                                .mayRetire(
                                epoch->id,
                                epoch->ever_presented);
                    }
                    if (gpu_complete &&
                        !present_complete &&
                        epoch->surface_epoch->lost) {
                        // A successor reacquire is meaningful only within the
                        // same surface. Once that surface is lost, preserve
                        // unproven presentation objects until device teardown.
                        core.quarantinePresentationResources(
                            epoch);
                        return true;
                    }
                    return gpu_complete &&
                           present_complete;
                });
        retired_epochs.erase(
            first_live, retired_epochs.end());
    }

    PumpResult pumpMaintenance(
        FramebufferExtentSnapshot framebuffer) {
        ++maintenance_tick;
        pollRetiredEpochs();
        PumpResult result;

        std::optional<BuildCompletion> completion;
        {
            std::scoped_lock lock{worker_mutex};
            if (worker_completion) {
                completion.emplace(
                    std::move(*worker_completion));
                worker_completion.reset();
                worker_busy = false;
            }
        }
        if (completion) {
            const bool framebuffer_still_matches =
                framebuffer.extent.width != 0 &&
                framebuffer.extent.height != 0 &&
                completion->request.framebuffer ==
                    framebuffer;
            if (completion->candidate != nullptr &&
                framebuffer_still_matches) {
                if (completion->request.old_epoch !=
                    nullptr) {
                    retireEpoch(
                        completion->request.old_epoch);
                }
                const auto previous_fingerprint =
                    outputCompileFactsFingerprint(
                        published_caps.compile_facts);
                active_epoch =
                    std::move(completion->candidate);
                if (published_surface_epoch_id !=
                    active_epoch->surface_epoch->id) {
                    published_surface_epoch_id =
                        active_epoch->surface_epoch->id;
                    ++surface_recovery_count;
                }
                published_caps.compile_facts =
                    active_epoch->output_facts;
                last_present_configuration =
                    active_epoch->swapchain
                        .present_configuration;
                last_support_fingerprint =
                    active_epoch->swapchain
                        .surface_support_fingerprint;
                current_presentation_queue_family =
                    active_epoch->surface_epoch
                        ->presentation_queue_family;
                device_rebuild_reason =
                    SurfaceDeviceRebuildReason::none;
                recovery->preparationSucceeded(
                    active_epoch->id,
                    active_epoch->recovery_key);
                camera.setScreenSize(
                    active_epoch->output_facts
                        .extent.width,
                    active_epoch->output_facts
                        .extent.height);
                result.output_facts_changed =
                    previous_fingerprint !=
                    outputCompileFactsFingerprint(
                        active_epoch->output_facts);
                LOG_INFO(
                    logger,
                    "swapchain epoch published: epoch={} surface_epoch={} "
                    "extent={}x{} reason={} facts_changed={}",
                    active_epoch->id,
                    active_epoch->surface_epoch->id,
                    active_epoch->output_facts
                        .extent.width,
                    active_epoch->output_facts
                        .extent.height,
                    windowOutputRecoveryReasonName(
                        completion->request.policy.reason),
                    result.output_facts_changed);
            } else if (
                completion->candidate != nullptr) {
                if (completion->request.old_epoch !=
                    nullptr) {
                    retireEpoch(
                        completion->request.old_epoch);
                }
                LOG_INFO(
                    logger,
                    "discarding stale swapchain epoch candidate: "
                    "candidate_revision={} current_revision={}",
                    completion->request.framebuffer
                        .revision,
                    framebuffer.revision);
                // The candidate is complete but has stale framebuffer facts.
                // Keep its swapchain as oldSwapchain for the replacement
                // instead of creating a second non-retired swapchain.
                recovery_anchor =
                    std::move(completion->candidate);
                recovery->deferRetry(
                    requestedRecoveryKey(
                        framebuffer),
                    WindowOutputRecoveryReason::
                        framebuffer_changed,
                    maintenance_tick, 1,
                    completion->request.policy
                        .attempt,
                    WindowOutputPreparationKind::
                        swapchain);
                if (framebuffer.extent.width == 0 ||
                    framebuffer.extent.height == 0) {
                    recovery->observeZeroExtent(
                        0, framebuffer);
                } else {
                    (void)recovery->requestRefresh(
                        0,
                        requestedRecoveryKey(
                            framebuffer),
                        WindowOutputRecoveryReason::
                            framebuffer_changed,
                        maintenance_tick);
                }
            } else {
                if (completion
                        ->replacement_anchor != nullptr) {
                    if (completion->request.old_epoch !=
                        nullptr) {
                        retireEpoch(
                            completion->request
                                .old_epoch);
                    }
                    recovery_anchor =
                        std::move(
                            completion
                                ->replacement_anchor);
                } else {
                    // vkCreateSwapchainKHR did not succeed, so oldSwapchain
                    // was not retired and remains the only legal anchor.
                    recovery_anchor =
                        std::move(
                            completion->request
                                .old_epoch);
                }
                last_wsi_result =
                    completion->failure_result;
                const auto classification =
                    classifyWsiResult(
                        completion->failure_result);
                LOG_WARNING(
                    logger,
                    "swapchain epoch preparation failed: result={} "
                    "classification={} error={}",
                    vk::to_string(
                        completion->failure_result),
                    static_cast<int>(classification),
                    completion->error);
                if (completion->device_rebuild_reason !=
                    SurfaceDeviceRebuildReason::none) {
                    device_rebuild_reason =
                        completion
                            ->device_rebuild_reason;
                    recovery->markDeviceLost();
                } else if (
                    classification ==
                    WsiResultClass::
                        surface_unavailable) {
                    auto lost_surface =
                        completion->request
                            .surface_epoch;
                    if (lost_surface == nullptr &&
                        recovery_anchor != nullptr) {
                        lost_surface =
                            recovery_anchor
                                ->surface_epoch;
                    }
                    markSurfaceLost(
                        lost_surface, 0);
                    if (framebuffer.extent.width ==
                            0 ||
                        framebuffer.extent.height ==
                            0) {
                        recovery->observeZeroExtent(
                            0, framebuffer);
                    }
                } else if (
                    framebuffer.extent.width == 0 ||
                    framebuffer.extent.height == 0) {
                    recovery->observeZeroExtent(
                        0, framebuffer);
                } else if (
                    classification ==
                    WsiResultClass::device_lost) {
                    recovery->markDeviceLost();
                } else if (
                    classification ==
                        WsiResultClass::
                            retryable_failure ||
                    classification ==
                        WsiResultClass::
                            swapchain_unavailable ||
                    completion->request.policy
                            .preparation_kind ==
                        WindowOutputPreparationKind::
                            surface) {
                    if (completion
                            ->replacement_anchor !=
                        nullptr ||
                        recovery_anchor != nullptr) {
                        recovery->deferRetry(
                            requestedRecoveryKey(
                                framebuffer),
                            WindowOutputRecoveryReason::
                                prepare_failed,
                            maintenance_tick, 30,
                            completion->request.policy
                                .attempt,
                            WindowOutputPreparationKind::
                                swapchain);
                    } else {
                        recovery->preparationFailed(
                            maintenance_tick, 30);
                    }
                } else {
                    recovery->markFatal();
                }
            }
        }

        if (worker_busy) return result;

        if (framebuffer.extent.width == 0 ||
            framebuffer.extent.height == 0) {
            if (recovery->kind() !=
                WindowOutputStateKind::
                    suspended_zero_extent) {
                recovery->observeZeroExtent(
                    active_epoch != nullptr
                        ? active_epoch->id
                        : 0,
                    framebuffer);
            }
            return result;
        }

        if (recovery->kind() ==
            WindowOutputStateKind::
                unavailable_retry) {
            (void)recovery->retryIfDue(
                maintenance_tick);
            if (recovery->kind() ==
                WindowOutputStateKind::
                    unavailable_retry) {
                (void)recovery->requestRefresh(
                    0,
                    requestedRecoveryKey(
                        framebuffer),
                    WindowOutputRecoveryReason::
                        framebuffer_changed,
                    maintenance_tick);
            }
        }
        if (recovery->kind() ==
            WindowOutputStateKind::
                suspended_zero_extent) {
            (void)recovery->resumeFromPositiveExtent(
                active_epoch != nullptr
                    ? active_epoch->id
                    : 0,
                requestedRecoveryKey(framebuffer),
                maintenance_tick);
        } else if (
            recovery->kind() ==
                WindowOutputStateKind::ready &&
            active_epoch != nullptr &&
            framebuffer.revision !=
                active_epoch->framebuffer.revision) {
            (void)recovery->requestRefresh(
                active_epoch->id,
                requestedRecoveryKey(framebuffer),
                WindowOutputRecoveryReason::
                    framebuffer_changed,
                maintenance_tick);
        }
        launchPendingPreparation(framebuffer);
        return result;
    }

    FrameBeginResult unavailableResult() const {
        switch (recovery->kind()) {
        case WindowOutputStateKind::
            suspended_zero_extent:
            return {
                .disposition =
                    FrameBeginDisposition::unavailable,
                .reason =
                    FrameUnavailableReason::zero_extent,
            };
        case WindowOutputStateKind::preparing:
        case WindowOutputStateKind::
            preparing_surface:
        case WindowOutputStateKind::refresh_pending:
            return {
                .disposition =
                    FrameBeginDisposition::unavailable,
                .reason =
                    FrameUnavailableReason::
                        output_preparing,
            };
        case WindowOutputStateKind::
            unavailable_retry:
            return {
                .disposition =
                    FrameBeginDisposition::unavailable,
                .reason =
                    FrameUnavailableReason::
                        retry_pending,
            };
        case WindowOutputStateKind::surface_lost:
            return {
                .disposition =
                    FrameBeginDisposition::unavailable,
                .reason =
                    FrameUnavailableReason::surface_lost,
            };
        case WindowOutputStateKind::
            device_rebuild_required:
            return {
                .disposition =
                    FrameBeginDisposition::
                        device_rebuild_required,
                .reason =
                    device_rebuild_reason ==
                            SurfaceDeviceRebuildReason::
                                none
                        ? FrameUnavailableReason::
                              device_lost
                        : FrameUnavailableReason::
                              device_rebuild_required,
            };
        case WindowOutputStateKind::fatal:
            return {
                .disposition =
                    FrameBeginDisposition::fatal,
                .reason =
                    FrameUnavailableReason::fatal,
            };
        case WindowOutputStateKind::ready:
            return {
                .disposition =
                    FrameBeginDisposition::unavailable,
                .reason =
                    FrameUnavailableReason::
                        surface_stale,
            };
        }
        return {};
    }

    FrameBeginResult beginFrame(
        std::shared_ptr<const RendererRuntimeGeneration>
            runtime_generation,
        GpuSubmissionLease submission_lease,
        FrameBeginMode mode) {
        if (active_frame) {
            throw std::logic_error(
                "swapchain frame target begin called with an unfinished frame");
        }

        const auto framebuffer =
            window.framebufferSnapshot();
        if (pending_abandonment &&
            !worker_busy &&
            recovery->kind() ==
                WindowOutputStateKind::ready) {
            (void)recovery->requestRefresh(
                pending_abandonment->epoch->id,
                requestedRecoveryKey(framebuffer),
                WindowOutputRecoveryReason::
                    abandoned_frame,
                maintenance_tick);
        }
        const auto pump =
            pumpMaintenance(framebuffer);
        if (framebuffer.extent.width == 0 ||
            framebuffer.extent.height == 0) {
            return {
                .disposition =
                    FrameBeginDisposition::unavailable,
                .reason =
                    FrameUnavailableReason::zero_extent,
            };
        }
        if (pump.output_facts_changed) {
            return {
                .disposition =
                    FrameBeginDisposition::unavailable,
                .reason =
                    FrameUnavailableReason::
                        output_reconfigured,
            };
        }
        if (active_epoch == nullptr ||
            recovery->kind() !=
                WindowOutputStateKind::ready) {
            return unavailableResult();
        }

        const bool nonblocking =
            mode == FrameBeginMode::nonblocking;
        auto epoch = active_epoch;
        const auto slot = epoch->next_slot;
        const auto &command_buffer =
            epoch->command_buffers[slot];
        const auto fence_result =
            device.waitForFences(
                {command_buffer.getFence()}, VK_TRUE,
                nonblocking ? 0 : UINT64_MAX);
        if (fence_result == vk::Result::eTimeout &&
            nonblocking) {
            return {
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
        if (epoch->submission_pending[slot]) {
            epoch->submission_pending[slot] = false;
            epoch->submission_leases.complete(slot);
        }

        const auto acquire_semaphore =
            epoch->acquire_semaphores[slot].get();
        vk::ResultValue<std::uint32_t> acquired{
            vk::Result::eErrorUnknown, 0};
        {
            std::scoped_lock lock{
                epoch->host_access};
            try {
                acquired = device.acquireNextImageKHR(
                    epoch->swapchain.swapchain.get(),
                    nonblocking ? 0 : UINT64_MAX,
                    acquire_semaphore);
            } catch (const vk::SystemError &error) {
                acquired.result =
                    resultFromSystemError(error);
            }
        }
        const auto classification =
            classifyWsiResult(acquired.result);
        last_wsi_result = acquired.result;
        if (classification ==
            WsiResultClass::not_ready) {
            return {
                .disposition =
                    FrameBeginDisposition::unavailable,
                .reason =
                    FrameUnavailableReason::
                        acquire_not_ready,
            };
        }
        if (classification ==
            WsiResultClass::
                swapchain_unavailable) {
            (void)recovery->requestRefresh(
                epoch->id,
                requestedRecoveryKey(framebuffer),
                WindowOutputRecoveryReason::out_of_date,
                maintenance_tick);
            launchPendingPreparation(framebuffer);
            return {
                .disposition =
                    FrameBeginDisposition::unavailable,
                .reason =
                    FrameUnavailableReason::
                        output_out_of_date,
            };
        }
        if (classification ==
            WsiResultClass::surface_unavailable) {
            markSurfaceLost(
                epoch->surface_epoch,
                epoch->id);
            return unavailableResult();
        }
        if (classification ==
            WsiResultClass::device_lost) {
            recovery->markDeviceLost();
            return unavailableResult();
        }
        if (classification ==
            WsiResultClass::retryable_failure) {
            recovery->deferRetry(
                requestedRecoveryKey(framebuffer),
                WindowOutputRecoveryReason::
                    resource_pressure,
                maintenance_tick, 30);
            return unavailableResult();
        }
        if (classification != WsiResultClass::ready &&
            classification !=
                WsiResultClass::refresh_advisory) {
            recovery->markFatal();
            return unavailableResult();
        }

        epoch->noteAcquired(
            acquired.value, maintenance1);
        pollRetiredEpochs();
        if (next_frame_serial ==
            std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error(
                "swapchain frame token serial space exhausted");
        }
        const auto serial = next_frame_serial++;
        active_frame = ActiveFrame{
            .serial = serial,
            .epoch = epoch,
            .slot = slot,
            .image_index = acquired.value,
            .begin_mode = mode,
            .refresh_after_present =
                classification ==
                WsiResultClass::refresh_advisory,
        };

        try {
            command_buffer.recordBegin();
            active_frame->phase =
                ActiveFramePhase::recording;

            vk::Viewport viewport;
            viewport.width = static_cast<float>(
                epoch->output_facts.extent.width);
            viewport.height = static_cast<float>(
                epoch->output_facts.extent.height);
            viewport.maxDepth = 1.0f;
            command_buffer->setViewport(
                0, {viewport});
            command_buffer->setScissor(
                0,
                {vk::Rect2D{
                    {0, 0},
                    epoch->output_facts.extent}});

            vk::ImageMemoryBarrier barrier;
            barrier.oldLayout =
                vk::ImageLayout::eUndefined;
            barrier.newLayout =
                vk::ImageLayout::
                    eColorAttachmentOptimal;
            barrier.image =
                epoch->images[acquired.value];
            barrier.subresourceRange = {
                vk::ImageAspectFlagBits::eColor,
                0, 1, 0, 1};
            barrier.dstAccessMask =
                vk::AccessFlagBits::
                    eColorAttachmentRead |
                vk::AccessFlagBits::
                    eColorAttachmentWrite;
            command_buffer->pipelineBarrier(
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::
                    eColorAttachmentOutput,
                {}, {}, {}, {barrier});
        } catch (...) {
            abandonFrameSerial(serial);
            throw;
        }

        auto frame = SwapchainFrameTarget::makeFrame(
            FrameRenderContext{
                .cmd_buf = *command_buffer,
                .color_image =
                    epoch->images[acquired.value],
                .color_attachment =
                    epoch->image_views[acquired.value]
                        .get(),
                .depth_attachment =
                    epoch->depth_image_view.get(),
                .extent =
                    epoch->output_facts.extent,
                .image_prepared_semaphore =
                    acquire_semaphore,
                .required_layout =
                    vk::ImageLayout::ePresentSrcKHR,
                .in_flight_frame_index = slot,
            },
            std::move(runtime_generation),
            std::move(submission_lease),
            frame_cleanup, serial, epoch);
        FrameBeginResult result;
        result.disposition =
            FrameBeginDisposition::ready;
        result.frame.emplace(std::move(frame));
        return result;
    }

    void recordOutputTransformCopy(
        vk::CommandBuffer command_buffer,
        vk::Image source,
        vk::Format source_format,
        vk::Extent2D source_extent) {
        if (!active_frame ||
            active_frame->phase !=
                ActiveFramePhase::recording) {
            throw std::logic_error(
                "swapchain output transform requires an active frame");
        }
        if (active_frame->output_transform_recorded) {
            throw std::runtime_error(
                "output_transform was recorded more than once");
        }
        const auto &epoch = *active_frame->epoch;
        if (source_format !=
                epoch.output_facts.color_format ||
            source_extent !=
                epoch.output_facts.extent) {
            throw std::runtime_error(
                "output_transform resolver v1 requires identical format, "
                "channel order, extent, and sample count");
        }

        vk::ImageMemoryBarrier to_transfer;
        to_transfer.srcAccessMask =
            vk::AccessFlagBits::eColorAttachmentRead |
            vk::AccessFlagBits::eColorAttachmentWrite;
        to_transfer.dstAccessMask =
            vk::AccessFlagBits::eTransferWrite;
        to_transfer.oldLayout =
            vk::ImageLayout::eColorAttachmentOptimal;
        to_transfer.newLayout =
            vk::ImageLayout::eTransferDstOptimal;
        to_transfer.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image =
            epoch.images[
                active_frame->image_index];
        to_transfer.subresourceRange = {
            vk::ImageAspectFlagBits::eColor,
            0, 1, 0, 1};
        command_buffer.pipelineBarrier(
            vk::PipelineStageFlagBits::
                eColorAttachmentOutput,
            vk::PipelineStageFlagBits::eTransfer,
            {}, {}, {}, {to_transfer});

        vk::ImageCopy region;
        region.srcSubresource = {
            vk::ImageAspectFlagBits::eColor,
            0, 0, 1};
        region.dstSubresource =
            region.srcSubresource;
        region.extent = vk::Extent3D{
            epoch.output_facts.extent, 1};
        command_buffer.copyImage(
            source,
            vk::ImageLayout::eTransferSrcOptimal,
            epoch.images[
                active_frame->image_index],
            vk::ImageLayout::eTransferDstOptimal,
            {region});

        vk::ImageMemoryBarrier to_present =
            to_transfer;
        to_present.srcAccessMask =
            vk::AccessFlagBits::eTransferWrite;
        to_present.dstAccessMask = {};
        to_present.oldLayout =
            vk::ImageLayout::eTransferDstOptimal;
        to_present.newLayout =
            vk::ImageLayout::ePresentSrcKHR;
        command_buffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eBottomOfPipe,
            {}, {}, {}, {to_present});
        active_frame->output_transform_recorded =
            true;
    }

    FrameSubmitResult submit(
        FrameTargetFrame frame) {
        SwapchainFrameTarget::validateFrameTarget(
            frame, frame_cleanup,
            "swapchain frame target");
        if (!active_frame) {
            throw std::logic_error(
                "swapchain frame target submit called without an active frame");
        }
        const auto active = *active_frame;
        auto consumed =
            SwapchainFrameTarget::consumeFrame(
                std::move(frame), frame_cleanup,
                active.serial,
                "swapchain frame target");
        if (active.phase !=
            ActiveFramePhase::recording) {
            throw std::logic_error(
                "swapchain frame target submit called without a recording frame");
        }
        if (consumed.target_epoch_lease.get() !=
            active.epoch.get()) {
            throw std::logic_error(
                "swapchain frame target received a token from another epoch");
        }

        auto epoch = active.epoch;
        const auto &command_buffer =
            epoch->command_buffers[active.slot];
        try {
            if (!active.output_transform_recorded) {
                vk::ImageMemoryBarrier barrier;
                barrier.srcAccessMask =
                    vk::AccessFlagBits::
                        eColorAttachmentRead |
                    vk::AccessFlagBits::
                        eColorAttachmentWrite;
                barrier.oldLayout =
                    vk::ImageLayout::
                        eColorAttachmentOptimal;
                barrier.newLayout =
                    vk::ImageLayout::ePresentSrcKHR;
                barrier.image =
                    epoch->images[
                        active.image_index];
                barrier.subresourceRange = {
                    vk::ImageAspectFlagBits::eColor,
                    0, 1, 0, 1};
                command_buffer->pipelineBarrier(
                    vk::PipelineStageFlagBits::
                        eColorAttachmentOutput,
                    vk::PipelineStageFlagBits::
                        eBottomOfPipe,
                    {}, {}, {}, {barrier});
            }

            const auto present_semaphore =
                epoch->present_semaphores[
                    active.image_index]
                    .get();
            command_buffer.recordEndSubmit(
                {present_semaphore},
                {epoch->acquire_semaphores[
                     active.slot]
                     .get()},
                {vk::PipelineStageFlagBits::
                     eTopOfPipe});
            active_frame->phase =
                ActiveFramePhase::submitted;
            auto lease =
                consumed.submission_lease != nullptr
                    ? std::move(
                          consumed.submission_lease)
                    : GpuSubmissionLease{
                          std::move(
                              consumed
                                  .runtime_generation)};
            epoch->submission_leases.submitted(
                active.slot, std::move(lease));
            epoch->submission_pending[active.slot] =
                true;

            vk::PresentInfoKHR present_info;
            const auto raw_swapchain =
                epoch->swapchain.swapchain.get();
            present_info.swapchainCount = 1;
            present_info.pSwapchains =
                &raw_swapchain;
            present_info.pImageIndices =
                &active.image_index;
            present_info.waitSemaphoreCount = 1;
            present_info.pWaitSemaphores =
                &present_semaphore;

            vk::SwapchainPresentFenceInfoEXT
                present_fence_info;
            vk::Fence present_fence;
            if (maintenance1) {
                present_fence =
                    epoch->preparePresentFence(
                        active.image_index);
                present_fence_info.swapchainCount =
                    1;
                present_fence_info.pFences =
                    &present_fence;
                present_info.pNext =
                    &present_fence_info;
                // Conservatively retain it even if vkQueuePresentKHR throws.
                // An unsignaled fence then keeps this epoch quarantined rather
                // than guessing whether the driver accepted the request.
                epoch->present_fence_pending[
                    active.image_index] = true;
            }

            vk::Result present_result =
                vk::Result::eErrorUnknown;
            {
                std::scoped_lock lock{
                    epoch->host_access};
                try {
                    present_result =
                        epoch->surface_epoch
                            ->presentation_queue
                            .presentKHR(
                                present_info);
                } catch (
                    const vk::SystemError &error) {
                    present_result =
                        resultFromSystemError(error);
                }
            }
            const auto classification =
                classifyWsiResult(present_result);
            last_wsi_result = present_result;
            epoch->present_wait_pending[
                active.image_index] = true;
            epoch->ever_presented = true;

            epoch->next_slot =
                (active.slot + 1) %
                in_flight_frames_num;
            epoch->last_rendered_image =
                active.image_index;
            epoch->has_rendered_frame =
                classification ==
                    WsiResultClass::ready ||
                classification ==
                    WsiResultClass::
                        refresh_advisory;
            active_frame.reset();

            if (classification ==
                WsiResultClass::surface_unavailable) {
                markSurfaceLost(
                    epoch->surface_epoch,
                    epoch->id);
                return {
                    .disposition =
                        FrameSubmitDisposition::
                            output_stale};
            }
            if (classification ==
                WsiResultClass::device_lost) {
                recovery->markDeviceLost();
                return {
                    .disposition =
                        FrameSubmitDisposition::
                            output_stale};
            }
            if (classification ==
                WsiResultClass::retryable_failure) {
                recovery->deferRetry(
                    requestedRecoveryKey(
                        window.framebufferSnapshot()),
                    WindowOutputRecoveryReason::
                        resource_pressure,
                    maintenance_tick, 30);
                return {
                    .disposition =
                        FrameSubmitDisposition::
                            output_stale};
            }
            if (classification !=
                    WsiResultClass::ready &&
                classification !=
                    WsiResultClass::
                        refresh_advisory &&
                classification !=
                    WsiResultClass::
                        swapchain_unavailable) {
                recovery->markFatal();
                throw std::runtime_error(
                    "failed on vkQueuePresentKHR: " +
                    vk::to_string(present_result));
            }

            const bool refresh =
                active.refresh_after_present ||
                classification ==
                    WsiResultClass::
                        refresh_advisory ||
                classification ==
                    WsiResultClass::
                        swapchain_unavailable;
            if (refresh) {
                const auto framebuffer =
                    window.framebufferSnapshot();
                const auto reason =
                    classification ==
                            WsiResultClass::
                                swapchain_unavailable
                        ? WindowOutputRecoveryReason::
                              out_of_date
                        : WindowOutputRecoveryReason::
                              suboptimal;
                (void)recovery->requestRefresh(
                    epoch->id,
                    requestedRecoveryKey(
                        framebuffer),
                    reason, maintenance_tick);
                launchPendingPreparation(framebuffer);
                return {
                    .disposition =
                        FrameSubmitDisposition::
                            output_stale};
            }
            return {
                .disposition =
                    FrameSubmitDisposition::presented};
        } catch (...) {
            abandonFrameSerial(active.serial);
            throw;
        }
    }

    void abandonFrameSerial(
        std::uint64_t serial) noexcept {
        if (!active_frame ||
            active_frame->serial != serial) {
            return;
        }
        const auto abandoned = *active_frame;
        if (abandoned.phase ==
            ActiveFramePhase::recording) {
            abandoned.epoch
                ->command_buffers[abandoned.slot]
                .abortRecording();
        }
        // No wait, queue submit, present, or swapchain create is legal from a
        // token destructor. The next owner-thread begin only enqueues a worker
        // cutover; maintenance1 may release this acquired image there.
        pending_abandonment =
            PendingAbandonment{
                abandoned.epoch,
                abandoned.image_index};
        abandoned.epoch->has_rendered_frame = false;
        active_frame.reset();
    }

    FrameTargetCaps caps() const {
        return published_caps;
    }

    FrameTargetStatus status() const {
        FrameTargetStatus result{
            .surface_epoch =
                published_surface_epoch_id,
            .swapchain_epoch =
                active_epoch != nullptr
                    ? active_epoch->id
                    : 0,
            .presentation_queue_family =
                published_surface_epoch_id != 0
                    ? current_presentation_queue_family
                    : VK_QUEUE_FAMILY_IGNORED,
            .surface_recovery_count =
                surface_recovery_count,
            .recovery_attempt =
                recovery->attempt(),
            .extent_revision =
                active_epoch != nullptr
                    ? active_epoch->framebuffer
                          .revision
                    : window.framebufferSnapshot()
                          .revision,
            .output_facts_fingerprint =
                outputCompileFactsFingerprint(
                    published_caps.compile_facts),
            .last_wsi_result =
                last_wsi_result,
            .retired_epoch_count =
                retired_epochs.size(),
            .quarantined_resource_count =
                core.quarantinedPresentationResourceCount(),
            .asynchronous_maintenance = true,
            .exact_present_retirement =
                maintenance1,
            .device_rebuild_reason =
                std::string{
                    surfaceDeviceRebuildReasonName(
                        device_rebuild_reason)},
        };
        switch (recovery->kind()) {
        case WindowOutputStateKind::ready:
            result.state =
                FrameTargetLifecycleState::ready;
            break;
        case WindowOutputStateKind::refresh_pending:
        case WindowOutputStateKind::preparing:
            result.state =
                FrameTargetLifecycleState::preparing;
            result.reason =
                FrameUnavailableReason::
                    output_preparing;
            break;
        case WindowOutputStateKind::
            preparing_surface:
            result.state =
                FrameTargetLifecycleState::
                    preparing_surface;
            result.reason =
                FrameUnavailableReason::
                    output_preparing;
            break;
        case WindowOutputStateKind::
            suspended_zero_extent:
            result.state =
                FrameTargetLifecycleState::
                    suspended_zero_extent;
            result.reason =
                FrameUnavailableReason::zero_extent;
            break;
        case WindowOutputStateKind::
            unavailable_retry:
            result.state =
                FrameTargetLifecycleState::
                    unavailable_retry;
            result.reason =
                FrameUnavailableReason::
                    retry_pending;
            break;
        case WindowOutputStateKind::surface_lost:
            result.state =
                FrameTargetLifecycleState::
                    surface_lost;
            result.reason =
                FrameUnavailableReason::surface_lost;
            break;
        case WindowOutputStateKind::
            device_rebuild_required:
            result.state =
                FrameTargetLifecycleState::
                    device_rebuild_required;
            result.reason =
                device_rebuild_reason ==
                        SurfaceDeviceRebuildReason::
                            none
                    ? FrameUnavailableReason::
                          device_lost
                    : FrameUnavailableReason::
                          device_rebuild_required;
            break;
        case WindowOutputStateKind::fatal:
            result.state =
                FrameTargetLifecycleState::fatal;
            result.reason =
                FrameUnavailableReason::fatal;
            break;
        }
        return result;
    }

    std::vector<std::uint8_t>
    readbackLastFrameRGBA8() {
        if (active_epoch == nullptr) {
            throw std::runtime_error(
                "window output is unavailable for capture");
        }
        auto epoch = active_epoch;
        if (!epoch->output_facts.capture_available) {
            throw std::runtime_error(
                "capture unavailable_windowed: surface lacks TRANSFER_SRC support");
        }
        if (!epoch->has_rendered_frame) {
            throw std::runtime_error(
                "SwapchainFrameTarget has no rendered frame to read back");
        }

        // Capture remains an explicit blocking operation. Recovery and XR
        // mirror maintenance never enter this path.
        device.waitIdle();
        const auto extent =
            epoch->output_facts.extent;
        const auto bytes =
            static_cast<vk::DeviceSize>(
                extent.width) *
            extent.height * 4;
        auto staging = core.allocBuf(
            bytes,
            vk::BufferUsageFlagBits::eTransferDst,
            vma::MemoryUsage::eAutoPreferHost,
            vma::AllocationCreateFlagBits::
                eHostAccessRandom);

        vk::BufferImageCopy copy_region;
        copy_region.imageSubresource = {
            vk::ImageAspectFlagBits::eColor,
            0, 0, 1};
        copy_region.imageExtent =
            vk::Extent3D{extent, 1};
        const auto image =
            epoch->images[
                epoch->last_rendered_image];
        GET_MODULE(VulkanUtils)
            .executeOneTimeCmd(
                [&](vk::CommandBuffer command_buffer) {
                    vk::ImageMemoryBarrier to_transfer;
                    to_transfer.oldLayout =
                        vk::ImageLayout::
                            ePresentSrcKHR;
                    to_transfer.newLayout =
                        vk::ImageLayout::
                            eTransferSrcOptimal;
                    to_transfer.dstAccessMask =
                        vk::AccessFlagBits::
                            eTransferRead;
                    to_transfer
                        .srcQueueFamilyIndex =
                        VK_QUEUE_FAMILY_IGNORED;
                    to_transfer
                        .dstQueueFamilyIndex =
                        VK_QUEUE_FAMILY_IGNORED;
                    to_transfer.image = image;
                    to_transfer.subresourceRange = {
                        vk::ImageAspectFlagBits::
                            eColor,
                        0, 1, 0, 1};
                    command_buffer.pipelineBarrier(
                        vk::PipelineStageFlagBits::
                            eBottomOfPipe,
                        vk::PipelineStageFlagBits::
                            eTransfer,
                        {}, {}, {},
                        {to_transfer});
                    command_buffer.copyImageToBuffer(
                        image,
                        vk::ImageLayout::
                            eTransferSrcOptimal,
                        staging.buffer.get(),
                        {copy_region});
                    auto to_present = to_transfer;
                    to_present.oldLayout =
                        vk::ImageLayout::
                            eTransferSrcOptimal;
                    to_present.newLayout =
                        vk::ImageLayout::
                            ePresentSrcKHR;
                    to_present.srcAccessMask =
                        vk::AccessFlagBits::
                            eTransferRead;
                    to_present.dstAccessMask = {};
                    command_buffer.pipelineBarrier(
                        vk::PipelineStageFlagBits::
                            eTransfer,
                        vk::PipelineStageFlagBits::
                            eBottomOfPipe,
                        {}, {}, {},
                        {to_present});
                },
                true);
        return core.readBuf(staging, bytes);
    }
};

SwapchainFrameTarget::SwapchainFrameTarget()
    : impl_{std::make_unique<Impl>()} {}

SwapchainFrameTarget::~SwapchainFrameTarget() =
    default;

FrameBeginResult SwapchainFrameTarget::beginFrame(
    std::shared_ptr<const RendererRuntimeGeneration>
        runtime_generation,
    GpuSubmissionLease submission_lease,
    FrameBeginMode mode) {
    return impl_->beginFrame(
        std::move(runtime_generation),
        std::move(submission_lease), mode);
}

void SwapchainFrameTarget::recordOutputTransformCopy(
    vk::CommandBuffer command_buffer,
    vk::Image source,
    vk::Format source_format,
    vk::Extent2D source_extent) {
    impl_->recordOutputTransformCopy(
        command_buffer, source, source_format,
        source_extent);
}

FrameSubmitResult SwapchainFrameTarget::submit(
    FrameTargetFrame frame) {
    return impl_->submit(std::move(frame));
}

void SwapchainFrameTarget::abandon(
    FrameTargetFrame frame) noexcept {
    abandonFrame(std::move(frame));
}

FrameTargetCaps SwapchainFrameTarget::caps() const {
    return impl_->caps();
}

FrameTargetStatus SwapchainFrameTarget::status() const {
    return impl_->status();
}

std::vector<std::uint8_t>
SwapchainFrameTarget::readbackLastFrameRGBA8() {
    return impl_->readbackLastFrameRGBA8();
}

} // namespace Pelican
