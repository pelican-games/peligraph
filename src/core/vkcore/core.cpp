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
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

constexpr auto vulkan_api_version = VK_MAKE_API_VERSION(0, 1, 3, 283);

static std::vector<std::string> supportedInstanceExtensions() {
    std::vector<std::string> result;
    for (const auto &extension : vk::enumerateInstanceExtensionProperties()) {
        result.emplace_back(extension.extensionName.data());
    }
    return result;
}

static bool supportsInstanceExtension(
    std::string_view name) {
    const auto supported = supportedInstanceExtensions();
    return std::find(
               supported.begin(), supported.end(), name) !=
           supported.end();
}

static bool supportsSurfaceMaintenance1InstanceContract() {
    return supportsInstanceExtension(
               VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) &&
           supportsInstanceExtension(
               VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
}

static void appendUniqueInstanceExtension(
    std::vector<const char *> &extensions,
    const char *name) {
    if (std::none_of(
            extensions.begin(), extensions.end(),
            [&](const char *existing) {
                return std::strcmp(existing, name) == 0;
            })) {
        extensions.push_back(name);
    }
}

static vk::UniqueInstance vulkanCreateInstance(
    bool headless, const DebugUtilsExtensionSelection &debug_utils_selection) {
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
    if (debug_utils_selection.enabled) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    if (!headless &&
        supportsSurfaceMaintenance1InstanceContract()) {
        appendUniqueInstanceExtension(
            exts,
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        appendUniqueInstanceExtension(
            exts,
            VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
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
static std::vector<std::string> requiredXrInstanceExtensions(
    bool headless, const DebugUtilsExtensionSelection &debug_utils_selection) {
    std::vector<std::string> result;
    if (!headless) {
        const auto window_extensions = GET_MODULE(Window).getRequiredVulkanInstanceExts();
        appendUniqueVulkanExtensions(result, window_extensions);
    }
#ifdef __APPLE__
    constexpr std::array portability_extensions{VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    appendUniqueVulkanExtensions(result, portability_extensions);
#endif
    if (debug_utils_selection.enabled) {
        constexpr std::array debug_utils_extensions{VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
        appendUniqueVulkanExtensions(result, debug_utils_extensions);
    }
    if (!headless &&
        supportsSurfaceMaintenance1InstanceContract()) {
        constexpr std::array
            surface_maintenance_extensions{
                VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
                VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
        appendUniqueVulkanExtensions(
            result, surface_maintenance_extensions);
    }
    return result;
}

static vk::UniqueInstance xrCreateVulkanInstance(
    bool headless, const DebugUtilsExtensionSelection &debug_utils_selection) {
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

    const auto required_extensions =
        requiredXrInstanceExtensions(headless, debug_utils_selection);
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

struct DeviceFeatureSupport {
    RequiredVulkanFeatureSupport required;
    bool timeline_semaphore = false;
    bool multiview = false;
    bool dynamic_rendering_local_read = false;
    bool sampler_anisotropy = false;
    bool swapchain_maintenance1 = false;
};

static std::vector<std::string> supportedDeviceExtensions(
    vk::PhysicalDevice physical_device);

static DeviceFeatureSupport queryDeviceFeatureSupport(vk::PhysicalDevice physical_device) {
    const auto chain =
        physical_device.getFeatures2<vk::PhysicalDeviceFeatures2,
                                    vk::PhysicalDeviceVulkan11Features,
                                    vk::PhysicalDeviceVulkan12Features,
                                    vk::PhysicalDeviceDynamicRenderingFeatures>();
    const auto &core = chain.get<vk::PhysicalDeviceFeatures2>().features;
    const auto &vk11 = chain.get<vk::PhysicalDeviceVulkan11Features>();
    const auto &vk12 = chain.get<vk::PhysicalDeviceVulkan12Features>();
    const auto &dynamic = chain.get<vk::PhysicalDeviceDynamicRenderingFeatures>();
    DeviceFeatureSupport result{
        .required =
            {
                .multi_draw_indirect = core.multiDrawIndirect == VK_TRUE,
                .draw_indirect_first_instance = core.drawIndirectFirstInstance == VK_TRUE,
                .shader_draw_parameters = vk11.shaderDrawParameters == VK_TRUE,
                .dynamic_rendering = dynamic.dynamicRendering == VK_TRUE,
            },
        .timeline_semaphore = vk12.timelineSemaphore == VK_TRUE,
        .multiview = vk11.multiview == VK_TRUE,
        .sampler_anisotropy =
            core.samplerAnisotropy == VK_TRUE,
    };

    const auto extensions = supportedDeviceExtensions(physical_device);
    if (std::find(extensions.begin(), extensions.end(),
                  VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME) !=
        extensions.end()) {
        const auto local_read_chain =
            physical_device.getFeatures2<
                vk::PhysicalDeviceFeatures2,
                vk::PhysicalDeviceDynamicRenderingLocalReadFeaturesKHR>();
        result.dynamic_rendering_local_read =
            local_read_chain
                .get<vk::PhysicalDeviceDynamicRenderingLocalReadFeaturesKHR>()
                .dynamicRenderingLocalRead == VK_TRUE;
    }
    if (std::find(
            extensions.begin(), extensions.end(),
            VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) !=
        extensions.end()) {
        const auto maintenance_chain =
            physical_device.getFeatures2<
                vk::PhysicalDeviceFeatures2,
                vk::PhysicalDeviceSwapchainMaintenance1FeaturesEXT>();
        result.swapchain_maintenance1 =
            maintenance_chain
                .get<vk::PhysicalDeviceSwapchainMaintenance1FeaturesEXT>()
                .swapchainMaintenance1 == VK_TRUE;
    }
    return result;
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
        const auto required_extensions = requiredDeviceExtensions(headless);
        const auto supported_extensions = supportedDeviceExtensions(phys_device);
        if (firstMissingVulkanExtension(required_extensions, supported_extensions)) continue;
        if (firstMissingRequiredVulkanFeature(
                queryDeviceFeatureSupport(phys_device).required)) continue;

        score_index_pair.push_back({score, i});
    }

    if (score_index_pair.empty()) {
        throw std::runtime_error("No suitable Vulkan physical device found");
    }
    std::stable_sort(score_index_pair.rbegin(), score_index_pair.rend());
    const auto choice_index = score_index_pair[0].second;

    return phys_devices[choice_index];
}

static vk::UniqueDevice createLogicalDevice(vk::PhysicalDevice phys_device, const QueueSet &queues_info,
                                            bool headless, bool &memory_budget_enabled,
                                            VulkanRuntimeCapabilities &runtime_capabilities,
                                            bool use_openxr = false) {
    LOG_INFO(logger, "initializing vulkan device...");

    const auto required_extensions = requiredDeviceExtensions(headless);
    const auto supported_extensions = supportedDeviceExtensions(phys_device);
    if (const auto missing =
            firstMissingVulkanExtension(required_extensions, supported_extensions)) {
#if PELICAN_WITH_OPENXR
        if (use_openxr) {
            throw OpenXr::VulkanBootstrapError("required Vulkan device extension missing: " +
                                               *missing);
        }
#endif
        throw std::runtime_error("required Vulkan device extension missing: " + *missing);
    }
    const auto feature_support = queryDeviceFeatureSupport(phys_device);
    if (const auto missing =
            firstMissingRequiredVulkanFeature(feature_support.required)) {
#if PELICAN_WITH_OPENXR
        if (use_openxr) {
            throw OpenXr::VulkanBootstrapError(
                "runtime-selected physical device lacks Vulkan feature " + *missing);
        }
#endif
        throw std::runtime_error("physical device lacks required Vulkan feature " + *missing);
    }
    memory_budget_enabled = std::find(supported_extensions.begin(), supported_extensions.end(),
                                      VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) != supported_extensions.end();
    auto enabled_extensions = required_extensions;
    if (memory_budget_enabled) enabled_extensions.emplace_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (feature_support.dynamic_rendering_local_read) {
        enabled_extensions.emplace_back(
            VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    }
    const bool swapchain_maintenance1 =
        !headless &&
        feature_support.swapchain_maintenance1 &&
        supportsSurfaceMaintenance1InstanceContract();
    if (swapchain_maintenance1) {
        enabled_extensions.emplace_back(
            VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    }
    const auto exts = vulkanExtensionNamePointers(enabled_extensions);

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
    features.features.drawIndirectFirstInstance = true; // firstInstance carries the model slot index
    features.features.samplerAnisotropy =
        feature_support.sampler_anisotropy ? VK_TRUE
                                           : VK_FALSE;
    vk::PhysicalDeviceVulkan11Features vk11features;
    vk11features.shaderDrawParameters = true; // necessary for using gl_BaseInstance in shaders
    // Enable opportunistically when available. Target planning still
    // requires an implementation/shader declaration before selecting it.
    vk11features.multiview =
        feature_support.multiview ? VK_TRUE : VK_FALSE;
#if PELICAN_WITH_OPENXR
    if (use_openxr && !feature_support.timeline_semaphore) {
        throw OpenXr::VulkanBootstrapError(
            "runtime-selected physical device lacks Vulkan feature timelineSemaphore required by OpenXR");
    }
#endif
    vk::PhysicalDeviceVulkan12Features vk12features;
    vk12features.timelineSemaphore = feature_support.timeline_semaphore ? VK_TRUE : VK_FALSE;
    vk::PhysicalDeviceDynamicRenderingLocalReadFeaturesKHR local_read_features;
    local_read_features.dynamicRenderingLocalRead =
        feature_support.dynamic_rendering_local_read ? VK_TRUE : VK_FALSE;
    vk::PhysicalDeviceSwapchainMaintenance1FeaturesEXT
        swapchain_maintenance_features;
    swapchain_maintenance_features.swapchainMaintenance1 =
        swapchain_maintenance1 ? VK_TRUE : VK_FALSE;

    vk::StructureChain create_info_chain{
        create_info,
        features,
        vk11features,
        vk12features,
        vk::PhysicalDeviceDynamicRenderingFeatures{VK_TRUE}, // necessary for dynamic rendering
        local_read_features,
        swapchain_maintenance_features,
    };
    if (!feature_support.dynamic_rendering_local_read) {
        create_info_chain
            .unlink<vk::PhysicalDeviceDynamicRenderingLocalReadFeaturesKHR>();
    }
    if (!swapchain_maintenance1) {
        create_info_chain
            .unlink<vk::PhysicalDeviceSwapchainMaintenance1FeaturesEXT>();
    }
    runtime_capabilities = {
        .timeline_semaphore = feature_support.timeline_semaphore,
        .multiview = feature_support.multiview,
        .dynamic_rendering_local_read =
            feature_support.dynamic_rendering_local_read,
        .sampler_anisotropy =
            feature_support.sampler_anisotropy,
        .swapchain_maintenance1 =
            swapchain_maintenance1,
    };

#if PELICAN_WITH_OPENXR
    if (use_openxr) {
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

static vma::UniqueAllocator createAllocator(vk::PhysicalDevice phys_device, vk::Device device,
                                            vk::Instance instance, bool memory_budget_enabled) {
    vma::AllocatorCreateInfo create_info;
    if (memory_budget_enabled) {
        create_info.flags |= vma::AllocatorCreateFlagBits::eExtMemoryBudget;
    }
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
    bool memory_budget_enabled = false;
    VulkanRuntimeCapabilities runtime_capabilities;
};

static VulkanBootstrapState bootstrapFlatVulkan(
    bool headless, const DebugUtilsExtensionSelection &debug_utils_selection) {
    VulkanBootstrapState result;
    result.instance = vulkanCreateInstance(headless, debug_utils_selection);
    if (!headless) result.surface = GET_MODULE(Window).getVulkanSurface(result.instance.get());
    result.physical_device = pickPhysicalDevice(result.instance.get(), result.surface.get(), headless);
    const auto queues = pickQueues(result.physical_device,
                                   result.physical_device.getQueueFamilyProperties(),
                                   result.surface.get(), headless);
    if (!queues) throw std::runtime_error("No suitable Vulkan queue families found");
    result.queues = *queues;
    result.device = createLogicalDevice(result.physical_device, result.queues, headless,
                                        result.memory_budget_enabled,
                                        result.runtime_capabilities);
    return result;
}

#if PELICAN_WITH_OPENXR
static VulkanBootstrapState bootstrapXrVulkan(
    bool headless, const DebugUtilsExtensionSelection &debug_utils_selection) {
    VulkanBootstrapState result;
    result.instance = xrCreateVulkanInstance(headless, debug_utils_selection);
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
    result.queues = *queues;

    const auto properties = result.physical_device.getProperties();
    LOG_INFO(logger, "OpenXR runtime selected Vulkan physical device: {} (vendor {}, device {})",
             properties.deviceName.data(), properties.vendorID, properties.deviceID);
    result.device = createLogicalDevice(result.physical_device, result.queues, headless,
                                        result.memory_budget_enabled,
                                        result.runtime_capabilities, true);
    return result;
}
#endif

VulkanManageCore::VulkanManageCore() {
    StartupPhaseTimer startup_timer{&StartupMetrics::addVulkan};
    auto &launch_config = GET_MODULE(EngineLaunchConfig);
    const bool headless = launch_config.headless;
    const auto debug_utils_selection = selectDebugUtilsExtension(
        launch_config.gpu_labels, supportedInstanceExtensions());
    VulkanBootstrapState bootstrap;
#if PELICAN_WITH_OPENXR
    if (launch_config.xr_active) {
        try {
            bootstrap = bootstrapXrVulkan(headless, debug_utils_selection);
        } catch (const OpenXr::VulkanBootstrapError &error) {
            OpenXr::abandonDiscovery();
            const auto info = resolveXrBootstrapFailure(launch_config, error.what());
            LOG_INFO(logger, "{}", info);
            bootstrap = bootstrapFlatVulkan(headless, debug_utils_selection);
        }
    } else {
        bootstrap = bootstrapFlatVulkan(headless, debug_utils_selection);
    }
#else
    bootstrap = bootstrapFlatVulkan(headless, debug_utils_selection);
#endif
    instance = std::move(bootstrap.instance);
    surface = std::move(bootstrap.surface);
    phys_device = bootstrap.physical_device;
    queue_set = bootstrap.queues;
    device = std::move(bootstrap.device);
    memory_budget_enabled = bootstrap.memory_budget_enabled;
    runtime_capabilities = bootstrap.runtime_capabilities;
    if (runtime_capabilities
            .dynamic_rendering_local_read) {
        const auto raw_device =
            static_cast<VkDevice>(device.get());
        set_rendering_attachment_locations =
            reinterpret_cast<
                PFN_vkCmdSetRenderingAttachmentLocationsKHR>(
                vkGetDeviceProcAddr(
                    raw_device,
                    "vkCmdSetRenderingAttachmentLocationsKHR"));
        set_rendering_input_attachment_indices =
            reinterpret_cast<
                PFN_vkCmdSetRenderingInputAttachmentIndicesKHR>(
                vkGetDeviceProcAddr(
                    raw_device,
                    "vkCmdSetRenderingInputAttachmentIndicesKHR"));
        if (set_rendering_attachment_locations ==
                nullptr ||
            set_rendering_input_attachment_indices ==
                nullptr) {
            throw std::runtime_error(
                "VK_KHR_dynamic_rendering_local_read was enabled "
                "but its device command entry points are "
                "unavailable");
        }
    }
    if (runtime_capabilities
            .swapchain_maintenance1) {
        release_swapchain_images =
            reinterpret_cast<
                PFN_vkReleaseSwapchainImagesEXT>(
                vkGetDeviceProcAddr(
                    static_cast<VkDevice>(
                        device.get()),
                    "vkReleaseSwapchainImagesEXT"));
        if (release_swapchain_images == nullptr) {
            LOG_WARNING(
                logger,
                "VK_EXT_swapchain_maintenance1 is enabled but "
                "vkReleaseSwapchainImagesEXT is unavailable");
        }
    }
    graphic_queue = device->getQueue(queue_set.graphic_queue, 0);
    presen_queue = device->getQueue(queue_set.presentation_queue, 0);
    compute_queue = device->getQueue(queue_set.compute_queue, 0);
    debug_utils = DebugUtilsDispatch::resolve(instance.get(), device.get(),
                                              debug_utils_selection);
    const auto &debug_status = debug_utils.getStatus();
    LOG_INFO(logger, "Vulkan debug utils: available={}, enabled={}, reason={}",
             debug_status.available, debug_status.enabled, debug_status.reason);
    graphic_cmd_pool = createCommandPool(device.get(), queue_set.graphic_queue);
    compute_cmd_pool = createCommandPool(device.get(), queue_set.compute_queue);
    allocator = createAllocator(phys_device, device.get(), instance.get(), memory_budget_enabled);
    LOG_INFO(logger, "Vulkan memory budget: available={}, reason={}", memory_budget_enabled,
             memory_budget_enabled ? "VK_EXT_memory_budget_enabled"
                                   : "VK_EXT_memory_budget_not_supported");
    LOG_INFO(logger,
             "Vulkan optional features: timeline_semaphore={}, multiview={}, "
             "dynamic_rendering_local_read={}, sampler_anisotropy={}, "
             "swapchain_maintenance1={}",
             runtime_capabilities.timeline_semaphore,
             runtime_capabilities.multiview,
             runtime_capabilities.dynamic_rendering_local_read,
             runtime_capabilities.sampler_anisotropy,
             runtime_capabilities.swapchain_maintenance1);
    LOG_INFO(logger, "vulkan core initialized");
}
VulkanManageCore::~VulkanManageCore() {}

void VulkanManageCore::setRenderingAttachmentLocations(
    vk::CommandBuffer command_buffer,
    const vk::RenderingAttachmentLocationInfoKHR
        &locations) const {
    if (!runtime_capabilities
             .dynamic_rendering_local_read ||
        set_rendering_attachment_locations == nullptr) {
        throw std::runtime_error(
            "dynamic rendering local-read attachment locations "
            "are unavailable");
    }
    set_rendering_attachment_locations(
        static_cast<VkCommandBuffer>(command_buffer),
        reinterpret_cast<
            const VkRenderingAttachmentLocationInfo *>(
            &locations));
}

void VulkanManageCore::setRenderingInputAttachmentIndices(
    vk::CommandBuffer command_buffer,
    const vk::RenderingInputAttachmentIndexInfoKHR
        &indices) const {
    if (!runtime_capabilities
             .dynamic_rendering_local_read ||
        set_rendering_input_attachment_indices == nullptr) {
        throw std::runtime_error(
            "dynamic rendering local-read input indices are "
            "unavailable");
    }
    set_rendering_input_attachment_indices(
        static_cast<VkCommandBuffer>(command_buffer),
        reinterpret_cast<
            const VkRenderingInputAttachmentIndexInfo *>(
            &indices));
}

vk::Result VulkanManageCore::releaseSwapchainImages(
    const vk::ReleaseSwapchainImagesInfoEXT
        &release_info) const noexcept {
    if (release_swapchain_images == nullptr) {
        return vk::Result::eErrorExtensionNotPresent;
    }
    return static_cast<vk::Result>(
        release_swapchain_images(
            static_cast<VkDevice>(device.get()),
            reinterpret_cast<
                const VkReleaseSwapchainImagesInfoEXT *>(
                &release_info)));
}

DriverMemoryStatus VulkanManageCore::driverMemoryStatus() const {
    if (!memory_budget_enabled) {
        return {.available = false,
                .reason = "VK_EXT_memory_budget_not_supported",
                .heaps = {}};
    }

    DriverMemoryStatus result{.available = true,
                              .reason = "VK_EXT_memory_budget_enabled"};
    const auto budgets = allocator->getHeapBudgets();
    const auto properties = phys_device.getMemoryProperties();
    result.heaps.reserve(properties.memoryHeapCount);
    for (std::uint32_t heap_index = 0; heap_index < properties.memoryHeapCount;
         ++heap_index) {
        const auto &heap = properties.memoryHeaps[heap_index];
        const auto &budget = budgets[heap_index];
        result.heaps.push_back({
            .heap_index = heap_index,
            .device_local = bool(heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal),
            .size = static_cast<std::uint64_t>(heap.size),
            .usage = static_cast<std::uint64_t>(budget.usage),
            .budget = static_cast<std::uint64_t>(budget.budget),
            .source = "vk_ext_memory_budget",
        });
    }
    return result;
}

void VulkanManageCore::setCurrentFrameIndex(std::uint64_t logical_frame) const noexcept {
    allocator->setCurrentFrameIndex(static_cast<std::uint32_t>(logical_frame));
}

void VulkanManageCore::quarantinePresentationResources(
    std::shared_ptr<const void> resources) {
    if (resources == nullptr) return;
    std::scoped_lock lock{
        presentation_quarantine_mutex};
    presentation_quarantine.push_back(
        std::move(resources));
}

std::size_t VulkanManageCore::
    quarantinedPresentationResourceCount() const noexcept {
    std::scoped_lock lock{
        presentation_quarantine_mutex};
    return presentation_quarantine.size();
}

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
                                          uint32_t mip_levels,
                                          vk::SampleCountFlagBits samples,
                                          uint32_t array_layers,
                                          vk::MemoryPropertyFlags
                                              preferred_memory_flags,
                                          vk::ImageCreateFlags
                                              image_flags,
                                          vk::ImageType
                                              image_type) const {
    if (array_layers == 0) {
        throw std::runtime_error(
            "Vulkan image array_layers must be greater than zero");
    }
    if (image_type == vk::ImageType::e3D &&
        array_layers != 1) {
        throw std::runtime_error(
            "Vulkan 3D images require exactly one array layer");
    }
    if (image_type != vk::ImageType::e3D &&
        extent.depth != 1) {
        throw std::runtime_error(
            "Vulkan non-3D images require extent.depth=1");
    }
    vk::ImageCreateInfo create_info;
    create_info.flags = image_flags;
    create_info.imageType = image_type;
    create_info.format = format;
    create_info.extent = extent;
    create_info.mipLevels = mip_levels;
    create_info.arrayLayers = array_layers;
    create_info.samples = samples;
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
    alloc_info.preferredFlags =
        preferred_memory_flags;

    // vma-hpp v3.3.0 以降、戻り値は pair<UniqueAllocation, UniqueImage> (allocation が先)
    auto [allocation, image] = allocator->createImageUnique(create_info, alloc_info);

    return ImageWrapper{
        .extent = extent,
        .format = format,
        .image_type = image_type,
        .mip_levels = mip_levels,
        .array_layers = array_layers,
        .allocation =
            std::make_shared<vma::UniqueAllocation>(
                std::move(allocation)),
        .image = std::move(image),
        .samples = samples,
        .usage = usage,
        .create_flags = image_flags,
    };
}

ImageWrapper VulkanManageCore::allocAliasingImage(
    const ImageWrapper &allocation_owner) const {
    if (allocation_owner.allocation == nullptr ||
        !*allocation_owner.allocation ||
        !allocation_owner.image) {
        throw std::runtime_error(
            "aliasing image requires a live allocation owner");
    }
    if (!(allocation_owner.create_flags &
          vk::ImageCreateFlagBits::eAlias)) {
        throw std::runtime_error(
            "aliasing image allocation owner was not created for aliasing");
    }

    vk::ImageCreateInfo create_info;
    create_info.flags = allocation_owner.create_flags;
    create_info.imageType =
        allocation_owner.image_type;
    create_info.format = allocation_owner.format;
    create_info.extent = allocation_owner.extent;
    create_info.mipLevels = allocation_owner.mip_levels;
    create_info.arrayLayers = allocation_owner.array_layers;
    create_info.samples = allocation_owner.samples;
    create_info.tiling = vk::ImageTiling::eOptimal;
    create_info.usage = allocation_owner.usage;
    create_info.sharingMode = vk::SharingMode::eExclusive;
    create_info.initialLayout = vk::ImageLayout::eUndefined;

    auto image = allocator->createAliasingImageUnique(
        allocation_owner.allocation->get(), create_info);
    return ImageWrapper{
        .extent = allocation_owner.extent,
        .format = allocation_owner.format,
        .image_type = allocation_owner.image_type,
        .mip_levels = allocation_owner.mip_levels,
        .array_layers = allocation_owner.array_layers,
        .allocation = allocation_owner.allocation,
        .image = std::move(image),
        .samples = allocation_owner.samples,
        .usage = allocation_owner.usage,
        .create_flags = allocation_owner.create_flags,
    };
}

void VulkanManageCore::writeImage(const ImageWrapper &dst, const void *src, vk::DeviceSize bytes_num) const {
    if (dst.allocation == nullptr || !*dst.allocation) {
        throw std::runtime_error(
            "cannot write an image without a live allocation");
    }
    allocator->copyMemoryToAllocation(
        src, dst.allocation->get(), 0, bytes_num);
}

} // namespace Pelican
