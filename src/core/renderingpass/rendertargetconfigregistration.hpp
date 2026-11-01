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

// RC-7e: the checks registration makes of a definition (the display format's
// device support, then validateRenderTargetDefinition, then the format
// features its image would need, requireRenderTargetFormatFeatures), for the
// definitions it does not register: their errors are the registered ones'.
void validateUnregisteredRenderTargetDefinitions(const std::vector<RenderTargetDefinition> &definitions,
                                                 vk::Extent2D base_extent);

} // namespace Pelican
