#pragma once

#include "rendertargetdefinition.hpp"
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetContainer;

// physicalRenderTargetHistoryClearColor() now lives with the definition it
// resolves, in rendertargetdefinition.hpp, which this header includes.

void registerRenderTargetDefinitions(const std::vector<RenderTargetDefinition> &definitions,
                                     vk::Extent2D base_extent,
                                     RenderTargetContainer &rt_container);

} // namespace Pelican
