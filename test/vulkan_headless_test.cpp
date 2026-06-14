#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/vkcore/core.hpp"
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

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
        REQUIRE_THROWS_AS(vkcore.getSurface(), std::runtime_error);
        vkcore.waitIdle();
    } catch (const std::exception &ex) {
        SKIP(std::string{"Vulkan headless initialization unavailable: "} + ex.what());
    }
}

} // namespace Pelican
