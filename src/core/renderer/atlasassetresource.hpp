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
    vk::UniqueDescriptorPool descriptor_pool;
    vk::UniqueSampler nearest_sampler;
    vk::UniqueSampler linear_sampler;
    std::vector<AtlasGpuPage> pages;

    AtlasGpuPage makePage(std::uint16_t id, std::string stable_name, ImageWrapper image);

  public:
    AtlasAssetResource();
    ~AtlasAssetResource();

    std::uint16_t registerPage(const AtlasPageSource &source);
    vk::DescriptorSet descriptor(std::uint16_t page, asset::SamplerKey sampler) const;
    vk::DescriptorSetLayout descriptorSetLayout() const { return descriptor_set_layout.get(); }
    std::size_t pageCountForTesting() const noexcept { return pages.size(); }
};

} // namespace Pelican
