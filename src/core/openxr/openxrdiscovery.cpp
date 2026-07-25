#include "openxrdiscovery.hpp"

#include "../config.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican::OpenXr {
namespace {

std::string xrFailure(const char *operation, XrResult result) {
    return std::string{operation} + " failed (XrResult " + std::to_string(result) + ")";
}

std::string vkFailure(const char *operation, VkResult result) {
    return std::string{operation} + " failed (VkResult " + std::to_string(result) + ")";
}

std::string versionString(XrVersion version) {
    return std::to_string(XR_VERSION_MAJOR(version)) + "." +
           std::to_string(XR_VERSION_MINOR(version)) + "." +
           std::to_string(XR_VERSION_PATCH(version));
}

} // namespace

XrApi loaderXrApi() {
    return {
        .get_instance_proc_addr = &xrGetInstanceProcAddr,
        .enumerate_instance_extension_properties = &xrEnumerateInstanceExtensionProperties,
        .create_instance = &xrCreateInstance,
    };
}

DiscoveryRuntime::DiscoveryRuntime() : DiscoveryRuntime(loaderXrApi()) {}

DiscoveryRuntime::DiscoveryRuntime(XrApi injected_api) : api{injected_api} {
    static_assert(XR_CURRENT_API_VERSION >= XR_MAKE_VERSION(1, 1, 0));
}

DiscoveryRuntime::~DiscoveryRuntime() { abandon(); }

void DiscoveryRuntime::abandon() noexcept {
    if (instance != XR_NULL_HANDLE && destroy_instance != nullptr) {
        (void)destroy_instance(instance);
    }
    instance = XR_NULL_HANDLE;
    system_id = XR_NULL_SYSTEM_ID;
    graphics_requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    destroy_instance = nullptr;
    get_system = nullptr;
    get_graphics_requirements = nullptr;
    create_vulkan_instance = nullptr;
    get_vulkan_graphics_device = nullptr;
    create_vulkan_device = nullptr;
    discovery_succeeded = false;
    win32_time_conversion_enabled = false;
    composition_layer_depth_enabled = false;
}

XrDiscoveryResult DiscoveryRuntime::fail(XrDiscoveryAvailability availability, std::string detail) {
    abandon();
    return {availability, std::move(detail)};
}

bool DiscoveryRuntime::resolve(const char *name, PFN_xrVoidFunction &function) const {
    function = nullptr;
    if (api.get_instance_proc_addr == nullptr) return false;
    const auto result = api.get_instance_proc_addr(instance, name, &function);
    return XR_SUCCEEDED(result) && function != nullptr;
}

XrDiscoveryResult DiscoveryRuntime::discover() {
    abandon();
    if (api.enumerate_instance_extension_properties == nullptr || api.create_instance == nullptr ||
        api.get_instance_proc_addr == nullptr) {
        return fail(XrDiscoveryAvailability::runtime_unavailable,
                    "loader-global dispatch is incomplete");
    }

    uint32_t extension_count = 0;
    auto result = api.enumerate_instance_extension_properties(nullptr, 0, &extension_count, nullptr);
    if (XR_FAILED(result)) {
        return fail(XrDiscoveryAvailability::runtime_unavailable,
                    xrFailure("xrEnumerateInstanceExtensionProperties", result));
    }
    std::vector<XrExtensionProperties> extensions(
        extension_count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    result = api.enumerate_instance_extension_properties(nullptr, extension_count, &extension_count,
                                                         extensions.data());
    if (XR_FAILED(result)) {
        return fail(XrDiscoveryAvailability::runtime_unavailable,
                    xrFailure("xrEnumerateInstanceExtensionProperties", result));
    }
    const auto has_extension = [&](std::string_view name) {
        return std::any_of(extensions.begin(), extensions.end(), [&](const auto &extension) {
            return std::string_view{extension.extensionName} == name;
        });
    };
    const auto has_vulkan_enable2 = has_extension(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    if (!has_vulkan_enable2) {
        return fail(XrDiscoveryAvailability::graphics_binding_unavailable,
                    "XR_KHR_vulkan_enable2 is not advertised");
    }

    std::vector<const char *> enabled_extensions{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    // Pelican requests OpenXR 1.0 for compatibility with installed 1.0
    // runtimes. Enable the promoted local-floor extension when advertised so
    // XR2a.2 can still select LOCAL_FLOOR on that API version.
    if (has_extension(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME)) {
        enabled_extensions.push_back(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);
    }
    composition_layer_depth_enabled =
        has_extension(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    if (composition_layer_depth_enabled) {
        enabled_extensions.push_back(
            XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    }
#ifdef _WIN32
    win32_time_conversion_enabled =
        has_extension(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
    if (win32_time_conversion_enabled) {
        enabled_extensions.push_back(
            XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
    }
#endif
    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(create_info.applicationInfo.applicationName,
                  sizeof(create_info.applicationInfo.applicationName), "%s", "Pelican App");
    create_info.applicationInfo.applicationVersion = 0;
    std::snprintf(create_info.applicationInfo.engineName,
                  sizeof(create_info.applicationInfo.engineName), "%s", engineName);
    create_info.applicationInfo.engineVersion = engineVersion;
    // XR1a only consumes OpenXR 1.0 core commands plus
    // XR_KHR_vulkan_enable2.  Requesting the header's 1.1 version would reject
    // otherwise compatible installed 1.0 runtimes before extension discovery.
    create_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
    create_info.enabledExtensionNames = enabled_extensions.data();
    result = api.create_instance(&create_info, &instance);
    if (XR_FAILED(result) || instance == XR_NULL_HANDLE) {
        instance = XR_NULL_HANDLE;
        return fail(XrDiscoveryAvailability::runtime_unavailable,
                    xrFailure("xrCreateInstance", result));
    }

    PFN_xrVoidFunction function = nullptr;
    if (!resolve("xrDestroyInstance", function)) {
        // A fake may deliberately fail this first resolution.  No resolved
        // destroy entry point exists, so discard the opaque handle rather than
        // calling a loader symbol outside the injected table.
        instance = XR_NULL_HANDLE;
        return fail(XrDiscoveryAvailability::runtime_unavailable,
                    "xrGetInstanceProcAddr could not resolve xrDestroyInstance");
    }
    destroy_instance = reinterpret_cast<PFN_xrDestroyInstance>(function);
    if (!resolve("xrGetSystem", function)) {
        return fail(XrDiscoveryAvailability::system_unavailable,
                    "xrGetInstanceProcAddr could not resolve xrGetSystem");
    }
    get_system = reinterpret_cast<PFN_xrGetSystem>(function);
    if (!resolve("xrGetVulkanGraphicsRequirements2KHR", function)) {
        return fail(XrDiscoveryAvailability::graphics_binding_unavailable,
                    "xrGetInstanceProcAddr could not resolve xrGetVulkanGraphicsRequirements2KHR");
    }
    get_graphics_requirements =
        reinterpret_cast<PFN_xrGetVulkanGraphicsRequirements2KHR>(function);
    if (!resolve("xrCreateVulkanInstanceKHR", function)) {
        return fail(XrDiscoveryAvailability::graphics_binding_unavailable,
                    "xrGetInstanceProcAddr could not resolve xrCreateVulkanInstanceKHR");
    }
    create_vulkan_instance = reinterpret_cast<PFN_xrCreateVulkanInstanceKHR>(function);
    if (!resolve("xrGetVulkanGraphicsDevice2KHR", function)) {
        return fail(XrDiscoveryAvailability::graphics_binding_unavailable,
                    "xrGetInstanceProcAddr could not resolve xrGetVulkanGraphicsDevice2KHR");
    }
    get_vulkan_graphics_device =
        reinterpret_cast<PFN_xrGetVulkanGraphicsDevice2KHR>(function);
    if (!resolve("xrCreateVulkanDeviceKHR", function)) {
        return fail(XrDiscoveryAvailability::graphics_binding_unavailable,
                    "xrGetInstanceProcAddr could not resolve xrCreateVulkanDeviceKHR");
    }
    create_vulkan_device = reinterpret_cast<PFN_xrCreateVulkanDeviceKHR>(function);

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = get_system(instance, &system_info, &system_id);
    if (XR_FAILED(result) || system_id == XR_NULL_SYSTEM_ID) {
        return fail(XrDiscoveryAvailability::system_unavailable,
                    xrFailure("xrGetSystem", result));
    }

    result = get_graphics_requirements(instance, system_id, &graphics_requirements);
    if (XR_FAILED(result)) {
        return fail(XrDiscoveryAvailability::graphics_binding_unavailable,
                    xrFailure("xrGetVulkanGraphicsRequirements2KHR", result));
    }

    discovery_succeeded = true;
    return {XrDiscoveryAvailability::available, {}};
}

void DiscoveryRuntime::requireDiscovered() const {
    if (!discovery_succeeded || instance == XR_NULL_HANDLE || system_id == XR_NULL_SYSTEM_ID) {
        throw VulkanBootstrapError("OpenXR discovery was not completed");
    }
}

VkInstance DiscoveryRuntime::createVulkanInstance(
    uint32_t vulkan_api_version, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    const VkInstanceCreateInfo &create_info) const {
    requireDiscovered();
    const auto requested_version =
        XR_MAKE_VERSION(VK_API_VERSION_MAJOR(vulkan_api_version),
                        VK_API_VERSION_MINOR(vulkan_api_version),
                        VK_API_VERSION_PATCH(vulkan_api_version));
    if (requested_version < graphics_requirements.minApiVersionSupported ||
        requested_version > graphics_requirements.maxApiVersionSupported) {
        throw VulkanBootstrapError(
            "Vulkan API " + versionString(requested_version) + " is outside runtime range " +
            versionString(graphics_requirements.minApiVersionSupported) + ".." +
            versionString(graphics_requirements.maxApiVersionSupported));
    }

    XrVulkanInstanceCreateInfoKHR xr_create_info{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xr_create_info.systemId = system_id;
    xr_create_info.pfnGetInstanceProcAddr = get_instance_proc_addr;
    xr_create_info.vulkanCreateInfo = &create_info;

    VkInstance vulkan_instance = VK_NULL_HANDLE;
    VkResult vulkan_result = VK_SUCCESS;
    const auto result = create_vulkan_instance(instance, &xr_create_info, &vulkan_instance,
                                               &vulkan_result);
    if (XR_FAILED(result)) {
        throw VulkanBootstrapError(xrFailure("xrCreateVulkanInstanceKHR", result));
    }
    if (vulkan_result != VK_SUCCESS) {
        throw VulkanBootstrapError(vkFailure("xrCreateVulkanInstanceKHR", vulkan_result));
    }
    if (vulkan_instance == VK_NULL_HANDLE) {
        throw VulkanBootstrapError("xrCreateVulkanInstanceKHR returned a null VkInstance");
    }
    return vulkan_instance;
}

VkPhysicalDevice DiscoveryRuntime::getVulkanGraphicsDevice(VkInstance vulkan_instance) const {
    requireDiscovered();
    XrVulkanGraphicsDeviceGetInfoKHR get_info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    get_info.systemId = system_id;
    get_info.vulkanInstance = vulkan_instance;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    const auto result = get_vulkan_graphics_device(instance, &get_info, &physical_device);
    if (XR_FAILED(result)) {
        throw VulkanBootstrapError(xrFailure("xrGetVulkanGraphicsDevice2KHR", result));
    }
    if (physical_device == VK_NULL_HANDLE) {
        throw VulkanBootstrapError("xrGetVulkanGraphicsDevice2KHR returned a null VkPhysicalDevice");
    }
    return physical_device;
}

VkDevice DiscoveryRuntime::createVulkanDevice(
    PFN_vkGetInstanceProcAddr get_instance_proc_addr, VkPhysicalDevice physical_device,
    const VkDeviceCreateInfo &create_info) const {
    requireDiscovered();
    XrVulkanDeviceCreateInfoKHR xr_create_info{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xr_create_info.systemId = system_id;
    xr_create_info.pfnGetInstanceProcAddr = get_instance_proc_addr;
    xr_create_info.vulkanPhysicalDevice = physical_device;
    xr_create_info.vulkanCreateInfo = &create_info;

    VkDevice vulkan_device = VK_NULL_HANDLE;
    VkResult vulkan_result = VK_SUCCESS;
    const auto result = create_vulkan_device(instance, &xr_create_info, &vulkan_device,
                                             &vulkan_result);
    if (XR_FAILED(result)) {
        throw VulkanBootstrapError(xrFailure("xrCreateVulkanDeviceKHR", result));
    }
    if (vulkan_result != VK_SUCCESS) {
        throw VulkanBootstrapError(vkFailure("xrCreateVulkanDeviceKHR", vulkan_result));
    }
    if (vulkan_device == VK_NULL_HANDLE) {
        throw VulkanBootstrapError("xrCreateVulkanDeviceKHR returned a null VkDevice");
    }
    return vulkan_device;
}

XrDiscoveryResult queryDiscovery(void *) { return GET_MODULE(DiscoveryRuntime).discover(); }

VkInstance createVulkanInstance(uint32_t vulkan_api_version,
                                PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                const VkInstanceCreateInfo &create_info) {
    return GET_MODULE(DiscoveryRuntime)
        .createVulkanInstance(vulkan_api_version, get_instance_proc_addr, create_info);
}

VkPhysicalDevice getVulkanGraphicsDevice(VkInstance vulkan_instance) {
    return GET_MODULE(DiscoveryRuntime).getVulkanGraphicsDevice(vulkan_instance);
}

VkDevice createVulkanDevice(PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                            VkPhysicalDevice physical_device,
                            const VkDeviceCreateInfo &create_info) {
    return GET_MODULE(DiscoveryRuntime)
        .createVulkanDevice(get_instance_proc_addr, physical_device, create_info);
}

void abandonDiscovery() noexcept {
    if (auto *runtime = FastModuleContainer::tryGet<DiscoveryRuntime>()) runtime->abandon();
}

} // namespace Pelican::OpenXr
