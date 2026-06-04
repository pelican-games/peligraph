#pragma once

#include "renderingpass.hpp"
#include <vector>

namespace Pelican {

class FullscreenPassContainer;
class RenderTarget;
class RenderTargetContainer;
class ShaderContainer;

struct RenderingPassRuntimeDependencies {
    RenderTarget *render_target = nullptr;
    RenderTargetContainer *render_target_container = nullptr;
    ShaderContainer *shader_container = nullptr;
    FullscreenPassContainer *fullscreen_pass_container = nullptr;
};

CompiledRenderingPass compileRenderingPassRuntime(const RenderingPassDefinition &definition,
                                                  RenderingPassRuntimeDependencies dependencies = {});
std::vector<CompiledRenderingPass> compileRenderingPassesRuntime(
    const std::vector<RenderingPassDefinition> &definitions,
    RenderingPassRuntimeDependencies dependencies = {});

} // namespace Pelican
