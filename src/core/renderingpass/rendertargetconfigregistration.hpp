#pragma once

#include "rendertargetdefinition.hpp"
#include <vector>

namespace Pelican {

class RenderTargetContainer;

void registerRenderTargetDefinitions(const std::vector<RenderTargetDefinition> &definitions,
                                     RenderTargetContainer &rt_container);

} // namespace Pelican
