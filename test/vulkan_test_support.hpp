#pragma once

#include "../src/core/container.hpp"
#include "../src/core/vkcore/core.hpp"

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <string>
#include <string_view>

namespace Pelican::TestSupport {

inline bool isVulkanDeviceUnavailable(const std::exception &error) {
    return std::string_view{error.what()}.find(
               "No suitable Vulkan physical device found") !=
        std::string_view::npos;
}

inline void skipIfVulkanDeviceUnavailable(
    const std::exception &error, std::string_view purpose) {
    if (isVulkanDeviceUnavailable(error)) {
        SKIP(std::string{purpose} + ": " + error.what());
    }
}

inline VulkanManageCore &requireVulkanDevice(std::string_view purpose) {
    try {
        return GET_MODULE(VulkanManageCore);
    } catch (const std::exception &error) {
        skipIfVulkanDeviceUnavailable(error, purpose);
        throw;
    }
}

} // namespace Pelican::TestSupport
