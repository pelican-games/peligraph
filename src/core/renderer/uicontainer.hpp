#pragma once

#include "../container.hpp"
#include "../ui/drawcommands.hpp"
#include "../vkcore/image.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct UiGpuPage {
    std::uint16_t id = 0;
    std::string stable_name;
    ImageWrapper image;
    vk::UniqueImageView view;
    vk::UniqueDescriptorSet nearest_set;
    vk::UniqueDescriptorSet linear_set;
};

DECLARE_MODULE(UIContainer) {
    vk::Device device;
    vk::UniqueDescriptorSetLayout descriptor_set_layout;
    vk::UniqueDescriptorPool descriptor_pool;
    vk::UniqueSampler nearest_sampler;
    vk::UniqueSampler linear_sampler;
    std::vector<UiGpuPage> pages;

    UiGpuPage makePage(std::uint16_t id, std::string stable_name, ImageWrapper image);

  public:
    UIContainer();
    ~UIContainer();

    vk::DescriptorSet descriptor(std::uint16_t page, ui::Sampler sampler) const;
    vk::DescriptorSetLayout descriptorSetLayout() const { return descriptor_set_layout.get(); }
    std::size_t pageCountForTesting() const noexcept { return pages.size(); }
};

} // namespace Pelican
