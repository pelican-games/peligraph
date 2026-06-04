#pragma once

#include "renderingpass.hpp"

namespace Pelican {

CompiledRenderingPass compileRenderingPassRuntime(const RenderingPassDefinition &definition);

} // namespace Pelican
