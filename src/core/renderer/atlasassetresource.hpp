#pragma once

#include "../asset/atlasasset.hpp"
#include "../container.hpp"
#include "../vkcore/image.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct AtlasPageSource {
    std::string stable_name;
    std::filesystem::path image_path;
    std::string embedded_resource;
    asset::AtlasExtent size{};
};

struct AtlasGpuPage {
    std::uint16_t id = 0;
    std::string stable_name;
    ImageWrapper image;
    vk::UniqueImageView view;
    vk::UniqueDescriptorSet nearest_set;
    vk::UniqueDescriptorSet linear_set;
};

// Shared, lazily-created GPU page owner. It knows no UI or sprite command ABI.
// Stable page names deduplicate allocations when both consumers are active.
DECLARE_MODULE(AtlasAssetResource) {
    vk::Device device;
    vk::UniqueDescriptorSetLayout descriptor_set_layout;
    std::vector<vk::UniqueDescriptorPool> descriptor_pools;
    std::uint32_t active_pool_descriptor_count = 0;
    vk::UniqueSampler nearest_sampler;
    vk::UniqueSampler linear_sampler;
    std::vector<AtlasGpuPage> pages;

    static constexpr std::uint32_t descriptor_sets_per_pool = 32;
    std::vector<vk::UniqueDescriptorSet> allocatePageDescriptorSets();
    AtlasGpuPage makePage(std::uint16_t id, std::string stable_name, ImageWrapper image);

  public:
    AtlasAssetResource();
    ~AtlasAssetResource();

    std::uint16_t registerPage(const AtlasPageSource &source);
    vk::DescriptorSet descriptor(std::uint16_t page, asset::SamplerKey sampler) const;
    vk::DescriptorSetLayout descriptorSetLayout() const { return descriptor_set_layout.get(); }
    std::size_t pageCountForTesting() const noexcept { return pages.size(); }
    std::size_t descriptorPoolCountForTesting() const noexcept { return descriptor_pools.size(); }
    static constexpr std::size_t pageCapacityPerPoolForTesting() noexcept {
        return descriptor_sets_per_pool / 2;
    }
};

} // namespace Pelican
