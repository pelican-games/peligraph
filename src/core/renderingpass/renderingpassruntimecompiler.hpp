#pragma once

#include "renderingpass.hpp"
#include <vector>

namespace Pelican {

class FullscreenPassContainer;
class RenderTarget;
class RenderTargetImageViewResolver;
class RenderTargetMetadataResolver;
class ShaderContainer;

struct RenderingPassRuntimeDependencies {
    RenderTarget *render_target = nullptr;
    const RenderTargetMetadataResolver *render_target_metadata = nullptr;
    const RenderTargetImageViewResolver *render_target_views = nullptr;
    ShaderContainer *shader_container = nullptr;
    FullscreenPassContainer *fullscreen_pass_container = nullptr;
};

CompiledRenderingPass compileRenderingPassRuntime(const RenderingPassDefinition &definition,
                                                  RenderingPassRuntimeDependencies dependencies = {});
std::vector<CompiledRenderingPass> compileRenderingPassesRuntime(
    const std::vector<RenderingPassDefinition> &definitions,
    RenderingPassRuntimeDependencies dependencies = {});

} // namespace Pelican
