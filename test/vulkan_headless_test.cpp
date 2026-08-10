#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/vkcore/core.hpp"
#include "vulkan_test_support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <string_view>

namespace Pelican {

TEST_CASE("VulkanManageCore initializes without a Window in headless mode", "[vulkan][headless]") {
    setupLogger();
    FastModuleContainer modules;
    auto &launch_config = GET_MODULE(EngineLaunchConfig);
    launch_config.headless = true;
    launch_config.headless_extent = vk::Extent2D{64, 64};

    auto &vkcore = TestSupport::requireVulkanDevice(
        "Vulkan headless initialization unavailable");
    REQUIRE(static_cast<VkDevice>(vkcore.getDevice()) != VK_NULL_HANDLE);
    REQUIRE(static_cast<VkPhysicalDevice>(vkcore.getPhysDevice()) != VK_NULL_HANDLE);
    REQUIRE_FALSE(
        vkcore.takeInitialWindowSurface().has_value());
    const auto &runtime_capabilities =
        vkcore.getRuntimeCapabilities();
    const auto enabled_extensions =
        vkcore.getEnabledDeviceExtensions();
    const auto has_enabled_extension =
        [&](std::string_view name) {
            return std::ranges::any_of(
                enabled_extensions,
                [&](const std::string &extension) {
                    return extension == name;
                });
        };
    const std::array ray_query_extensions{
        std::string_view{
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME},
        std::string_view{VK_KHR_RAY_QUERY_EXTENSION_NAME},
        std::string_view{
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME},
    };
    CAPTURE(runtime_capabilities.ray_query);
    REQUIRE(runtime_capabilities.acceleration_structure ==
            runtime_capabilities.ray_query);
    REQUIRE(runtime_capabilities.buffer_device_address ==
            runtime_capabilities.ray_query);
    for (const auto extension : ray_query_extensions) {
        REQUIRE(has_enabled_extension(extension) ==
                runtime_capabilities.ray_query);
    }
    if (runtime_capabilities.ray_query) {
        REQUIRE(runtime_capabilities
                    .min_acceleration_structure_scratch_offset_alignment >
                0);
    }
    if (vkcore.getRuntimeCapabilities().dynamic_rendering_local_read) {
        const auto extensions =
            vkcore.getPhysDevice().enumerateDeviceExtensionProperties();
        CHECK(std::ranges::any_of(extensions, [](const auto &extension) {
            return std::string_view{extension.extensionName.data()} ==
                   VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
        }));

        const auto features =
            vkcore.getPhysDevice()
                .getFeatures2<
                    vk::PhysicalDeviceFeatures2,
                    vk::PhysicalDeviceDynamicRenderingLocalReadFeaturesKHR>();
        CHECK(features
                  .get<vk::PhysicalDeviceDynamicRenderingLocalReadFeaturesKHR>()
                  .dynamicRenderingLocalRead == VK_TRUE);
    }
    vkcore.waitIdle();
}

} // namespace Pelican
