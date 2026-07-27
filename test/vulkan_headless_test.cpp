#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/vkcore/core.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <string_view>

namespace Pelican {

TEST_CASE("VulkanManageCore initializes without a Window in headless mode", "[vulkan][headless]") {
    setupLogger();
    FastModuleContainer modules;
    auto &launch_config = GET_MODULE(EngineLaunchConfig);
    launch_config.headless = true;
    launch_config.headless_extent = vk::Extent2D{64, 64};

    try {
        auto &vkcore = GET_MODULE(VulkanManageCore);
        REQUIRE(static_cast<VkDevice>(vkcore.getDevice()) != VK_NULL_HANDLE);
        REQUIRE(static_cast<VkPhysicalDevice>(vkcore.getPhysDevice()) != VK_NULL_HANDLE);
        REQUIRE_FALSE(
            vkcore.takeInitialWindowSurface().has_value());
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
    } catch (const std::exception &ex) {
        SKIP(std::string{"Vulkan headless initialization unavailable: "} + ex.what());
    }
}

} // namespace Pelican
