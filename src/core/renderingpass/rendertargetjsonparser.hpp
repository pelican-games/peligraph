#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetContainer;

struct RenderTargetDefinition {
    std::string name;
    vk::Extent2D extent;
    vk::Format format;
    vk::ImageUsageFlags usage;
};

std::vector<RenderTargetDefinition> parseRenderTargetDefinitionsFromJson(const nlohmann::json &data,
                                                                         vk::Extent2D base_extent);

void registerRenderTargetDefinitions(const std::vector<RenderTargetDefinition> &definitions,
                                     RenderTargetContainer &rt_container);

} // namespace Pelican
