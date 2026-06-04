#pragma once

#include "rendertargetdefinition.hpp"
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetContainer;

void registerRenderTargetDefinitions(const std::vector<RenderTargetDefinition> &definitions,
                                     vk::Extent2D base_extent,
                                     RenderTargetContainer &rt_container);

} // namespace Pelican
