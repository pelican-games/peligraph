#pragma once

#include "renderingpass.hpp"
#include <functional>
#include <string>
#include <vector>

namespace Pelican {

class DebugDraw;
class DebugText;
class FullscreenPassContainer;
class FrameGraphResourceContainer;
class PathResolver;
class RenderTarget;
class RenderTargetImageViewResolver;
class RenderTargetMetadataResolver;
class ShadowDepthPassContainer;
class VelocityPassContainer;
class ShaderLibrary;
struct VulkanTargetPlan;

struct RenderingPassRuntimeDependencies {
    RenderTarget *render_target = nullptr;
    const RenderTargetMetadataResolver *render_target_metadata = nullptr;
    const RenderTargetImageViewResolver *render_target_views = nullptr;
    ShaderLibrary *shader_library = nullptr;
    FullscreenPassContainer *fullscreen_pass_container = nullptr;
    ShadowDepthPassContainer *shadow_depth_pass_container = nullptr;
    VelocityPassContainer *velocity_pass_container = nullptr;
    FrameGraphResourceContainer *frame_graph_resources = nullptr;
    const PathResolver *path_resolver = nullptr;
    std::vector<std::string> shader_defines;
    bool warn_backend_specific_shader_refs = false;
    std::function<DebugDraw &()> debug_draw_provider;
    std::function<DebugText &()> debug_text_provider;
    const VulkanTargetPlan *target_plan = nullptr;
};

CompiledRenderingPass compileRenderingPassRuntime(const RenderingPassDefinition &definition,
                                                  RenderingPassRuntimeDependencies dependencies = {});
std::vector<CompiledRenderingPass> compileRenderingPassesRuntime(
    const std::vector<RenderingPassDefinition> &definitions,
    RenderingPassRuntimeDependencies dependencies = {});

} // namespace Pelican
