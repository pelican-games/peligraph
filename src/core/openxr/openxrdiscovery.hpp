#pragma once

#include "../container.hpp"
#include "../xractivation.hpp"

#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#endif
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <stdexcept>
#include <string>

namespace Pelican::OpenXr {

// Only loader-global entry points are seeded here.  Every instance command,
// including all XR_KHR_vulkan_enable2 commands, is resolved through the
// injected xrGetInstanceProcAddr function before it can be invoked.
struct XrApi {
    PFN_xrGetInstanceProcAddr get_instance_proc_addr = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties enumerate_instance_extension_properties = nullptr;
    PFN_xrCreateInstance create_instance = nullptr;
};

XrApi loaderXrApi();

class VulkanBootstrapError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

DECLARE_MODULE(DiscoveryRuntime) {
    XrApi api;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    XrGraphicsRequirementsVulkanKHR graphics_requirements{
        XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};

    PFN_xrDestroyInstance destroy_instance = nullptr;
    PFN_xrGetSystem get_system = nullptr;
    PFN_xrGetVulkanGraphicsRequirements2KHR get_graphics_requirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR create_vulkan_instance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR get_vulkan_graphics_device = nullptr;
    PFN_xrCreateVulkanDeviceKHR create_vulkan_device = nullptr;
    bool discovery_succeeded = false;
    bool win32_time_conversion_enabled = false;
    bool composition_layer_depth_enabled = false;

    XrDiscoveryResult fail(XrDiscoveryAvailability availability, std::string detail);
    bool resolve(const char *name, PFN_xrVoidFunction &function) const;
    void requireDiscovered() const;

  public:
    DiscoveryRuntime();
    explicit DiscoveryRuntime(XrApi injected_api);
    ~DiscoveryRuntime();

    DiscoveryRuntime(const DiscoveryRuntime &) = delete;
    DiscoveryRuntime &operator=(const DiscoveryRuntime &) = delete;

    XrDiscoveryResult discover();
    void abandon() noexcept;

    VkInstance createVulkanInstance(uint32_t vulkan_api_version,
                                    PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                    const VkInstanceCreateInfo &create_info) const;
    VkPhysicalDevice getVulkanGraphicsDevice(VkInstance vulkan_instance) const;
    VkDevice createVulkanDevice(PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                VkPhysicalDevice physical_device,
                                const VkDeviceCreateInfo &create_info) const;

    XrInstance getInstance() const noexcept { return instance; }
    XrSystemId getSystemId() const noexcept { return system_id; }
    PFN_xrGetInstanceProcAddr getInstanceProcAddr() const noexcept {
        return api.get_instance_proc_addr;
    }
    bool win32TimeConversionEnabled() const noexcept {
        return win32_time_conversion_enabled;
    }
    bool compositionLayerDepthEnabled() const noexcept {
        return composition_layer_depth_enabled;
    }
};

XrDiscoveryResult queryDiscovery(void *context);
VkInstance createVulkanInstance(uint32_t vulkan_api_version,
                                PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                const VkInstanceCreateInfo &create_info);
VkPhysicalDevice getVulkanGraphicsDevice(VkInstance vulkan_instance);
VkDevice createVulkanDevice(PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                            VkPhysicalDevice physical_device,
                            const VkDeviceCreateInfo &create_info);
void abandonDiscovery() noexcept;

} // namespace Pelican::OpenXr
