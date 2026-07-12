#pragma once

#include "../container.hpp"
#include "../ui/drawcommands.hpp"

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

// UI-specific adapter: page uploads and lifetime live in AtlasAssetResource.
DECLARE_MODULE(UIContainer) {
    std::vector<std::uint16_t> shared_pages;

  public:
    UIContainer();
    ~UIContainer();

    vk::DescriptorSet descriptor(std::uint16_t page, ui::Sampler sampler) const;
    vk::DescriptorSetLayout descriptorSetLayout() const;
    std::size_t pageCountForTesting() const noexcept { return shared_pages.size(); }
};

} // namespace Pelican
