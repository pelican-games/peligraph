#pragma once

#include "../../core/log.hpp"
#include <vulkan/vulkan.hpp>
#include <atomic>
#include <stdexcept>

namespace Pelican::Android {
inline std::atomic<unsigned> validation_warnings{0}, validation_errors{0};

inline VKAPI_ATTR VkBool32 VKAPI_CALL validationLogCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validation_errors;
        LOG_ERROR(logger, "Android Vulkan validation: {}", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ++validation_warnings;
        LOG_WARNING(logger, "Android Vulkan validation: {}", data->pMessage);
    }
    return VK_FALSE;
}

inline VkDebugUtilsMessengerCreateInfoEXT validationLogCreateInfo() {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = validationLogCallback;
    return info;
}

// Declared after each UniqueInstance owner, so device/surface destruction is
// observed and this messenger is destroyed before its instance, also on throw.
class ValidationLogMessenger {
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy;
public:
    explicit ValidationLogMessenger(VkInstance value) : instance{value} {
        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (!create || !destroy) throw std::runtime_error("Android validation log messenger unavailable");
        const auto info = validationLogCreateInfo();
        if (create(instance, &info, nullptr, &messenger) != VK_SUCCESS)
            throw std::runtime_error("Android validation log messenger creation failed");
        LOG_INFO(logger, "Android Vulkan validation file callback enabled=true (warning/error)");
    }
    ~ValidationLogMessenger() { destroy(instance, messenger, nullptr); }
    ValidationLogMessenger(const ValidationLogMessenger &) = delete;
    ValidationLogMessenger &operator=(const ValidationLogMessenger &) = delete;
};
}
