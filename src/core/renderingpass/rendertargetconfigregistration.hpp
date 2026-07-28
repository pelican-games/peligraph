#pragma once

#include "rendertargetdefinition.hpp"
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetContainer;

vk::ClearColorValue physicalRenderTargetHistoryClearColor(
    const RenderTargetDefinition &definition);

void registerRenderTargetDefinitions(const std::vector<RenderTargetDefinition> &definitions,
                                     vk::Extent2D base_extent,
                                     RenderTargetContainer &rt_container);

} // namespace Pelican
