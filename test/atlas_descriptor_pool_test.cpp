#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderer/atlasassetresource.hpp"
#include "../src/core/vkcore/core.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

namespace Pelican {

TEST_CASE("AtlasAssetResource grows descriptor pools after the active pool fills",
          "[atlas][descriptor-pool][growth]") {
    setupLogger();
    FastModuleContainer modules;
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    try {
        (void)GET_MODULE(VulkanManageCore);
    } catch (const std::exception &error) {
        SKIP(std::string{"Vulkan headless initialization unavailable: "} + error.what());
    }

    auto &atlas = GET_MODULE(AtlasAssetResource);
    REQUIRE(atlas.descriptorPoolCountForTesting() == 1);
    const auto initial_capacity = AtlasAssetResource::pageCapacityPerPoolForTesting();
    REQUIRE(atlas.pageCountForTesting() == 1);
    for (std::size_t index = 0; index < initial_capacity; ++index) {
        const auto page = atlas.registerPage({.stable_name = "wp173-page-" + std::to_string(index),
                                              .embedded_resource = "debug_text_font.png",
                                              .size = {128, 96}});
        REQUIRE(static_cast<VkDescriptorSet>(atlas.descriptor(page, asset::SamplerKey::nearest)) !=
                VK_NULL_HANDLE);
        REQUIRE(static_cast<VkDescriptorSet>(atlas.descriptor(page, asset::SamplerKey::linear)) !=
                VK_NULL_HANDLE);
    }
    REQUIRE(atlas.pageCountForTesting() == initial_capacity + 1);
    REQUIRE(atlas.descriptorPoolCountForTesting() == 2);
    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace Pelican
