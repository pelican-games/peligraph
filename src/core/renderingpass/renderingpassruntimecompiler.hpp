#pragma once

#include "renderingpass.hpp"
#include <vector>

namespace Pelican {

std::vector<PassId> compileRenderingPassRuntime(const RenderingPassDefinition &definition);

} // namespace Pelican
