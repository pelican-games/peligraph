#pragma once

#include "renderingpass.hpp"
#include <vector>

namespace Pelican {

class FullscreenPassContainer;
class PathResolver;
class RenderTarget;
class RenderTargetImageViewResolver;
class RenderTargetMetadataResolver;
class ShaderLibrary;

struct RenderingPassRuntimeDependencies {
    RenderTarget *render_target = nullptr;
    const RenderTargetMetadataResolver *render_target_metadata = nullptr;
    const RenderTargetImageViewResolver *render_target_views = nullptr;
    ShaderLibrary *shader_library = nullptr;
    FullscreenPassContainer *fullscreen_pass_container = nullptr;
    const PathResolver *path_resolver = nullptr;
    bool warn_backend_specific_shader_refs = false;
};

CompiledRenderingPass compileRenderingPassRuntime(const RenderingPassDefinition &definition,
                                                  RenderingPassRuntimeDependencies dependencies = {});
std::vector<CompiledRenderingPass> compileRenderingPassesRuntime(
    const std::vector<RenderingPassDefinition> &definitions,
    RenderingPassRuntimeDependencies dependencies = {});

} // namespace Pelican
