#include "core.hpp"
#include "bootstrap.hpp"
#include "../startup.hpp"
#include "../config.hpp"
#include "../launchconfig.hpp"
#include "../log.hpp"
#include "../os/window.hpp"
#if PELICAN_WITH_OPENXR
#include "../openxr/openxrdiscovery.hpp"
#endif
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Pelican {

constexpr auto vulkan_api_version = VK_MAKE_API_VERSION(0, 1, 3, 283);

static vk::UniqueInstance vulkanCreateInstance(bool headless) {
    LOG_INFO(logger, "initializing vulkan instance...");

    vk::ApplicationInfo app_info;
    app_info.pApplicationName = "Pelican App";
    app_info.applicationVersion = 0;
    app_info.pEngineName = engineName;
    app_info.engineVersion = engineVersion;
    app_info.apiVersion = vulkan_api_version;

    std::vector<const char *> layers, exts;

#ifdef _DEBUG
    layers.push_back("VK_LAYER_KHRONOS_validation");
#endif

    if (!headless) {
        exts = GET_MODULE(Window).getRequiredVulkanInstanceExts();
    }
    vk::InstanceCreateInfo create_info;
#ifdef _DEBUG
    const vk::ValidationFeatureEnableEXT synchronization_validation =
        vk::ValidationFeatureEnableEXT::eSynchronizationValidation;
    vk::ValidationFeaturesEXT validation_features;
    validation_features.setEnabledValidationFeatures(synchronization_validation);
    create_info.pNext = &validation_features;
#endif
#ifdef __APPLE__
    create_info.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
    exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

    create_info.pApplicationInfo = &app_info;
    create_info.setPEnabledExtensionNames(exts);
    create_info.setPEnabledLayerNames(layers);

    return vk::createInstanceUnique(create_info);
}

#if PELICAN_WITH_OPENXR
static std::vector<std::string> supportedInstanceExtensions() {
    std::vector<std::string> result;
    for (const auto &extension : vk::enumerateInstanceExtensionProperties()) {
        result.emplace_back(extension.extensionName.data());
    }
    return result;
}

static std::vector<std::string> requiredXrInstanceExtensions(bool headless) {
    std::vector<std::string> result;
    if (!headless) {
        const auto window_extensions = GET_MODULE(Window).getRequiredVulkanInstanceExts();
        appendUniqueVulkanExtensions(result, window_extensions);
    }
#ifdef __APPLE__
    constexpr std::array portability_extensions{VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    appendUniqueVulkanExtensions(result, portability_extensions);
#endif
    return result;
}

static vk::UniqueInstance xrCreateVulkanInstance(bool headless) {
    LOG_INFO(logger, "initializing OpenXR-selected vulkan instance...");

    vk::ApplicationInfo app_info;
    app_info.pApplicationName = "Pelican App";
    app_info.applicationVersion = 0;
    app_info.pEngineName = engineName;
    app_info.engineVersion = engineVersion;
    app_info.apiVersion = vulkan_api_version;

    std::vector<const char *> layers;
#ifdef _DEBUG
    layers.push_back("VK_LAYER_KHRONOS_validation");
#endif

    const auto required_extensions = requiredXrInstanceExtensions(headless);
    const auto supported_extensions = supportedInstanceExtensions();
    if (const auto missing =
            firstMissingVulkanExtension(required_extensions, supported_extensions)) {
        throw OpenXr::VulkanBootstrapError("required Vulkan instance extension missing: " +
                                           *missing);
    }
    const auto extension_names = vulkanExtensionNamePointers(required_extensions);

    vk::InstanceCreateInfo create_info;
#ifdef _DEBUG
    const vk::ValidationFeatureEnableEXT synchronization_validation =
        vk::ValidationFeatureEnableEXT::eSynchronizationValidation;
    vk::ValidationFeaturesEXT validation_features;
    validation_features.setEnabledValidationFeatures(synchronization_validation);
    create_info.pNext = &validation_features;
#endif
#ifdef __APPLE__
    create_info.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
#endif
    create_info.pApplicationInfo = &app_info;
    create_info.setPEnabledExtensionNames(extension_names);
    create_info.setPEnabledLayerNames(layers);

    const auto raw_instance = OpenXr::createVulkanInstance(
        vulkan_api_version, &vkGetInstanceProcAddr,
        *reinterpret_cast<const VkInstanceCreateInfo *>(&create_info));
    return vk::UniqueInstance{vk::Instance{raw_instance}};
}
#endif

static std::optional<QueueSet> pickQueues(const vk::PhysicalDevice &phys_device,
                                          std::vector<vk::QueueFamilyProperties> queue_families,
                                          vk::SurfaceKHR surface, bool headless) {
    std::optional<uint32_t> graphics_queue;
    std::optional<uint32_t> presentation_queue;
    std::optional<uint32_t> compute_queue;

    if (!headless) {
        for (int i = 0; i < queue_families.size(); i++) {
            if ((queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
                phys_device.getSurfaceSupportKHR(i, surface) &&
                (queue_families[i].queueFlags & vk::QueueFlagBits::eCompute)) {
                graphics_queue = i;
                presentation_queue = i;
                compute_queue = i;
                break;
            }
        }
        if (!graphics_queue || !presentation_queue)
            for (int i = 0; i < queue_families.size(); i++) {
                if ((queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
                    phys_device.getSurfaceSupportKHR(i, surface)) {
                    graphics_queue = i;
                    presentation_queue = i;
                    break;
                }
            }
        if (!presentation_queue)
            for (int i = 0; i < queue_families.size(); i++) {
                if (phys_device.getSurfaceSupportKHR(i, surface)) {
                    presentation_queue = i;
                    break;
                }
            }
    } else {
        for (int i = 0; i < queue_families.size(); i++) {
            if ((queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
                (queue_families[i].queueFlags & vk::QueueFlagBits::eCompute)) {
                graphics_queue = i;
                compute_queue = i;
                break;
            }
        }
    }
    if (!graphics_queue)
        for (int i = 0; i < queue_families.size(); i++) {
            if (queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics) {
                graphics_queue = i;
                break;
            }
        }
    if (!compute_queue)
        for (int i = 0; i < queue_families.size(); i++) {
            if (queue_families[i].queueFlags & vk::QueueFlagBits::eCompute) {
                compute_queue = i;
                break;
            }
        }
    if (headless && graphics_queue) {
        presentation_queue = graphics_queue;
    }

    if (graphics_queue && presentation_queue && compute_queue) {
        return QueueSet{
            .graphic_queue = *graphics_queue,
            .presentation_queue = *presentation_queue,
            .compute_queue = *compute_queue,
        };
    }

    return std::nullopt;
}

static vk::PhysicalDevice pickPhysicalDevice(vk::Instance instance, vk::SurfaceKHR surface, bool headless) {
    LOG_INFO(logger, "initializing vulkan physical device...");

    const auto phys_devices = instance.enumeratePhysicalDevices();

    std::vector<std::pair<int, int>> score_index_pair;
    // decide device priority
    for (int i = 0; i < phys_devices.size(); i++) {
        int score = 0;
        const auto &phys_device = phys_devices[i];

        {
            // evaluate queue
            const auto queue_families = phys_device.getQueueFamilyProperties();
            const auto queue_set = pickQueues(phys_device, queue_families, surface, headless);

            if (!queue_set)
                continue;
            if (headless || queue_set->graphic_queue == queue_set->presentation_queue)
                score += 100;
        }
        if (!headless) {
            // evaluate extension
            const auto supported_exts = phys_device.enumerateDeviceExtensionProperties();
            std::vector<std::string> supported_exts_names;
            std::vector<const char *> required_exts_names = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

            for (const auto ext : supported_exts) {
                supported_exts_names.push_back(ext.extensionName.data());
            }
        }
        {
            const auto feature = phys_device.getFeatures();
            if (!feature.multiDrawIndirect)
                continue;
        }

        score_index_pair.push_back({score, i});
    }

    if (score_index_pair.empty()) {
        throw std::runtime_error("No suitable Vulkan physical device found");
    }
    std::stable_sort(score_index_pair.rbegin(), score_index_pair.rend());
    const auto choice_index = score_index_pair[0].second;

    return phys_devices[choice_index];
}

static std::vector<std::string> supportedDeviceExtensions(vk::PhysicalDevice physical_device) {
    std::vector<std::string> result;
    for (const auto &extension : physical_device.enumerateDeviceExtensionProperties()) {
        result.emplace_back(extension.extensionName.data());
    }
    return result;
}

static std::vector<std::string> requiredDeviceExtensions(bool headless) {
    std::vector<std::string> result;
    if (!headless) result.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    return result;
}

static vk::UniqueDevice createLogicalDevice(vk::PhysicalDevice phys_device, const QueueSet &queues_info,
                                            bool headless, bool use_openxr = false) {
    LOG_INFO(logger, "initializing vulkan device...");

    const auto required_extensions = requiredDeviceExtensions(headless);
    const auto exts = vulkanExtensionNamePointers(required_extensions);

    vk::DeviceQueueCreateInfo graphics_queue_info, presentation_queue_info, compute_queue_info;

    std::vector<vk::DeviceQueueCreateInfo> queues;
    const float queue_priority = 1.0f;
    {
        std::set<uint32_t> queue_indices = {queues_info.graphic_queue, queues_info.presentation_queue,
                                            queues_info.compute_queue};
        for (const auto index : queue_indices) {
            vk::DeviceQueueCreateInfo queue_create_info;
            queue_create_info.queueFamilyIndex = index;
            queue_create_info.setQueuePriorities(queue_priority);
            queues.push_back(queue_create_info);
        }
    }

    vk::DeviceCreateInfo create_info;

    if (!exts.empty()) {
        create_info.setPEnabledExtensionNames(exts);
    }
    create_info.setQueueCreateInfos(queues);

    vk::PhysicalDeviceFeatures2 features;
    features.features.multiDrawIndirect = true; // necessary for multi draw indirect
    vk::PhysicalDeviceVulkan11Features vk11features;
    vk11features.shaderDrawParameters = true; // necessary for using gl_BaseIndex in shader
    const auto supported_feature_chain =
        phys_device.getFeatures2<vk::PhysicalDeviceFeatures2,
                                 vk::PhysicalDeviceVulkan12Features>();
    const bool timeline_semaphore_supported =
        supported_feature_chain.get<vk::PhysicalDeviceVulkan12Features>()
            .timelineSemaphore == VK_TRUE;
#if PELICAN_WITH_OPENXR
    if (use_openxr && !timeline_semaphore_supported) {
        throw OpenXr::VulkanBootstrapError(
            "runtime-selected physical device lacks Vulkan feature timelineSemaphore required by OpenXR");
    }
#endif
    vk::PhysicalDeviceVulkan12Features vk12features;
    vk12features.timelineSemaphore = timeline_semaphore_supported ? VK_TRUE : VK_FALSE;

    vk::StructureChain create_info_chain{
        create_info,
        features,
        vk11features,
        vk12features,
        vk::PhysicalDeviceDynamicRenderingFeatures{VK_TRUE}, // necessary for dynamic rendering
    };

#if PELICAN_WITH_OPENXR
    if (use_openxr) {
        const auto supported_extensions = supportedDeviceExtensions(phys_device);
        if (const auto missing =
                firstMissingVulkanExtension(required_extensions, supported_extensions)) {
            throw OpenXr::VulkanBootstrapError("required Vulkan device extension missing: " +
                                               *missing);
        }
        const auto &chained_create_info = create_info_chain.get<vk::DeviceCreateInfo>();
        const auto raw_device = OpenXr::createVulkanDevice(
            &vkGetInstanceProcAddr, static_cast<VkPhysicalDevice>(phys_device),
            *reinterpret_cast<const VkDeviceCreateInfo *>(&chained_create_info));
        return vk::UniqueDevice{vk::Device{raw_device}};
    }
#else
    (void)use_openxr;
#endif
    return phys_device.createDeviceUnique(create_info_chain.get<vk::DeviceCreateInfo>());
}

static vk::UniqueCommandPool createCommandPool(vk::Device device, uint32_t queue_family_index) {
    LOG_INFO(logger, "creating vulkan command pool...");

    vk::CommandPoolCreateInfo create_info;
    create_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    create_info.queueFamilyIndex = queue_family_index;

    return device.createCommandPoolUnique(create_info);
}

static vma::UniqueAllocator createAllocator(vk::PhysicalDevice phys_device, vk::Device device, vk::Instance instance) {
    vma::AllocatorCreateInfo create_info;
    create_info.vulkanApiVersion = vulkan_api_version;
    create_info.physicalDevice = phys_device;
    create_info.device = device;
    create_info.instance = instance;
    return vma::createAllocatorUnique(create_info);
}

struct VulkanBootstrapState {
    vk::UniqueInstance instance;
    vk::UniqueSurfaceKHR surface;
    vk::PhysicalDevice physical_device;
    QueueSet queues{};
    vk::UniqueDevice device;
};

static VulkanBootstrapState bootstrapFlatVulkan(bool headless) {
    VulkanBootstrapState result;
    result.instance = vulkanCreateInstance(headless);
    if (!headless) result.surface = GET_MODULE(Window).getVulkanSurface(result.instance.get());
    result.physical_device = pickPhysicalDevice(result.instance.get(), result.surface.get(), headless);
    const auto queues = pickQueues(result.physical_device,
                                   result.physical_device.getQueueFamilyProperties(),
                                   result.surface.get(), headless);
    if (!queues) throw std::runtime_error("No suitable Vulkan queue families found");
    result.queues = *queues;
    result.device = createLogicalDevice(result.physical_device, result.queues, headless);
    return result;
}

#if PELICAN_WITH_OPENXR
static VulkanBootstrapState bootstrapXrVulkan(bool headless) {
    VulkanBootstrapState result;
    result.instance = xrCreateVulkanInstance(headless);
    if (!headless) result.surface = GET_MODULE(Window).getVulkanSurface(result.instance.get());

    const auto raw_physical_device =
        OpenXr::getVulkanGraphicsDevice(static_cast<VkInstance>(result.instance.get()));
    result.physical_device = vk::PhysicalDevice{raw_physical_device};
    const auto queues = pickQueues(result.physical_device,
                                   result.physical_device.getQueueFamilyProperties(),
                                   result.surface.get(), headless);
    if (!queues) {
        throw OpenXr::VulkanBootstrapError(
            "runtime-selected physical device has no engine-compatible queue families");
    }
    if (!result.physical_device.getFeatures().multiDrawIndirect) {
        throw OpenXr::VulkanBootstrapError(
            "runtime-selected physical device lacks multiDrawIndirect");
    }
    result.queues = *queues;

    const auto properties = result.physical_device.getProperties();
    LOG_INFO(logger, "OpenXR runtime selected Vulkan physical device: {} (vendor {}, device {})",
             properties.deviceName.data(), properties.vendorID, properties.deviceID);
    result.device = createLogicalDevice(result.physical_device, result.queues, headless, true);
    return result;
}
#endif

VulkanManageCore::VulkanManageCore() {
    StartupPhaseTimer startup_timer{&StartupMetrics::addVulkan};
    auto &launch_config = GET_MODULE(EngineLaunchConfig);
    const bool headless = launch_config.headless;
    VulkanBootstrapState bootstrap;
#if PELICAN_WITH_OPENXR
    if (launch_config.xr_active) {
        try {
            bootstrap = bootstrapXrVulkan(headless);
        } catch (const OpenXr::VulkanBootstrapError &error) {
            OpenXr::abandonDiscovery();
            const auto info = resolveXrBootstrapFailure(launch_config, error.what());
            LOG_INFO(logger, "{}", info);
            bootstrap = bootstrapFlatVulkan(headless);
        }
    } else {
        bootstrap = bootstrapFlatVulkan(headless);
    }
#else
    bootstrap = bootstrapFlatVulkan(headless);
#endif
    instance = std::move(bootstrap.instance);
    surface = std::move(bootstrap.surface);
    phys_device = bootstrap.physical_device;
    queue_set = bootstrap.queues;
    device = std::move(bootstrap.device);
    graphic_queue = device->getQueue(queue_set.graphic_queue, 0);
    presen_queue = device->getQueue(queue_set.presentation_queue, 0);
    compute_queue = device->getQueue(queue_set.compute_queue, 0);
    graphic_cmd_pool = createCommandPool(device.get(), queue_set.graphic_queue);
    compute_cmd_pool = createCommandPool(device.get(), queue_set.compute_queue);
    allocator = createAllocator(phys_device, device.get(), instance.get());
    LOG_INFO(logger, "vulkan core initialized");
}
VulkanManageCore::~VulkanManageCore() {}

vk::SurfaceKHR VulkanManageCore::getSurface() const {
    if (!surface) {
        throw std::runtime_error("Vulkan surface is unavailable in headless mode");
    }
    return surface.get();
}

void VulkanManageCore::waitIdle() const { device->waitIdle(); }

std::vector<CommandBufWrapper> VulkanManageCore::allocCmdBufs(size_t num) const {
    vk::CommandBufferAllocateInfo alloc_info;
    alloc_info.commandPool = graphic_cmd_pool.get();
    alloc_info.level = vk::CommandBufferLevel::ePrimary;
    alloc_info.commandBufferCount = static_cast<uint32_t>(num);
    auto raw_cmd_bufs = device->allocateCommandBuffersUnique(alloc_info);

    vk::FenceCreateInfo fence_create_info;
    fence_create_info.flags = vk::FenceCreateFlagBits::eSignaled;

    std::vector<CommandBufWrapper> cmd_bufs;
    cmd_bufs.reserve(num);
    for (int i = 0; i < num; i++) {
        cmd_bufs.emplace_back(device.get(), graphic_queue, std::move(raw_cmd_bufs[i]),
                              device->createFenceUnique(fence_create_info));
    }
    assert(cmd_bufs.size() == num);

    return cmd_bufs;
}

std::vector<vk::UniqueSemaphore> VulkanManageCore::createSemaphores(size_t num) const {
    std::vector<vk::UniqueSemaphore> semaphores;
    semaphores.reserve(num);

    vk::SemaphoreCreateInfo create_info;
    for (int i = 0; i < num; i++) {
        semaphores.emplace_back(device->createSemaphoreUnique(create_info));
    }

    return semaphores;
}

BufferWrapper VulkanManageCore::allocBuf(vk::DeviceSize bytes_num, vk::BufferUsageFlags usage,
                                         vma::MemoryUsage mem_usage, vma::AllocationCreateFlags alloc_flags,
                                         VulkanProcessType type) const {
    vk::BufferCreateInfo create_info;
    create_info.size = bytes_num;
    create_info.usage = usage;
    create_info.sharingMode = vk::SharingMode::eExclusive;
    std::array<uint32_t, 1> queue_families;
    if (type == VulkanProcessType::graphics) {
        queue_families[0] = queue_set.graphic_queue;
        create_info.setQueueFamilyIndices(queue_families);
    } else {
        queue_families[0] = queue_set.compute_queue;
        create_info.setQueueFamilyIndices(queue_families);
    }

    vma::AllocationCreateInfo alloc_info;
    alloc_info.flags = alloc_flags;
    alloc_info.usage = mem_usage;

    // vma-hpp v3.3.0 以降、戻り値は pair<UniqueAllocation, UniqueBuffer> (allocation が先)
    auto [allocation, buffer] = allocator->createBufferUnique(create_info, alloc_info);

    return BufferWrapper{
        .buffer = std::move(buffer),
        .allocation = std::move(allocation),
    };
}

void VulkanManageCore::writeBuf(const BufferWrapper &dst, const void *src, vk::DeviceSize offset,
                                vk::DeviceSize bytes_num) const {
    allocator->copyMemoryToAllocation(src, dst.allocation.get(), offset, bytes_num);
}

std::vector<uint8_t> VulkanManageCore::readBuf(const BufferWrapper &src, vk::DeviceSize bytes_num) const {
    allocator->invalidateAllocation(src.allocation.get(), 0, bytes_num);
    const auto mapped = allocator->mapMemory(src.allocation.get());
    std::vector<uint8_t> bytes(static_cast<size_t>(bytes_num));
    std::memcpy(bytes.data(), mapped, bytes.size());
    allocator->unmapMemory(src.allocation.get());
    return bytes;
}

ImageWrapper VulkanManageCore::allocImage(vk::Extent3D extent, vk::Format format, vk::ImageUsageFlags usage,
                                          vma::MemoryUsage mem_usage, vma::AllocationCreateFlags alloc_flags,
                                          VulkanProcessType type,
                                          std::span<const vk::Format> compatible_view_formats,
                                          uint32_t mip_levels) const {
    vk::ImageCreateInfo create_info;
    create_info.imageType = vk::ImageType::e2D;
    create_info.format = format;
    create_info.extent = extent;
    create_info.mipLevels = mip_levels;
    create_info.arrayLayers = 1;
    create_info.samples = vk::SampleCountFlagBits::e1;
    create_info.tiling = vk::ImageTiling::eOptimal;
    create_info.usage = usage;
    create_info.sharingMode = vk::SharingMode::eExclusive;
    create_info.initialLayout = vk::ImageLayout::eUndefined;
    vk::ImageFormatListCreateInfo format_list;
    if (!compatible_view_formats.empty()) {
        create_info.flags |= vk::ImageCreateFlagBits::eMutableFormat;
        format_list.setViewFormats(compatible_view_formats);
        create_info.pNext = &format_list;
    }

    std::array<uint32_t, 1> queue_families;
    if (type == VulkanProcessType::graphics) {
        queue_families[0] = queue_set.graphic_queue;
        create_info.setQueueFamilyIndices(queue_families);
    } else {
        queue_families[0] = queue_set.compute_queue;
        create_info.setQueueFamilyIndices(queue_families);
    }

    vma::AllocationCreateInfo alloc_info;
    alloc_info.flags = alloc_flags;
    alloc_info.usage = mem_usage;

    // vma-hpp v3.3.0 以降、戻り値は pair<UniqueAllocation, UniqueImage> (allocation が先)
    auto [allocation, image] = allocator->createImageUnique(create_info, alloc_info);

    return ImageWrapper{
        .extent = extent,
        .format = format,
        .mip_levels = mip_levels,
        .image = std::move(image),
        .allocation = std::move(allocation),
    };
}
void VulkanManageCore::writeImage(const ImageWrapper &dst, const void *src, vk::DeviceSize bytes_num) const {
    allocator->copyMemoryToAllocation(src, dst.allocation.get(), 0, bytes_num);
}

} // namespace Pelican
