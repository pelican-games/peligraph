#pragma once

#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "rendertarget.hpp"
#include <glm/glm.hpp>
#include <optional>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class Camera;
class DebugDraw;
class DebugText;
class FrameResources;
class Gizmo;
struct FullscreenPassRendererDependencies;
class FullscreenPassRenderer;
struct MaterialRendererDependencies;
class MaterialRenderer;
class ShadowDepthPassContainer;
class VelocityPassContainer;
struct UiRendererDependencies;
class UiRenderer;
#if PELICAN_WITH_IMGUI
class ImGuiSystem;
#endif

struct RenderPassDispatchDependencies {
    MaterialRenderer &material_renderer;
    const MaterialRendererDependencies &material_renderer_dependencies;
    FullscreenPassRenderer &fullscreen_pass_renderer;
    const FullscreenPassRendererDependencies &fullscreen_pass_renderer_dependencies;
    ShadowDepthPassContainer &shadow_depth_pass_container;
    VelocityPassContainer &velocity_pass_container;
    UiRenderer *ui_renderer = nullptr;
    const UiRendererDependencies *ui_renderer_dependencies = nullptr;
    DebugDraw *debug_draw = nullptr;
    Gizmo *gizmo = nullptr;
    DebugText *debug_text = nullptr;
#if PELICAN_WITH_IMGUI
    ImGuiSystem *imgui_system = nullptr;
#endif
    const FrameResources &frame_resources;
    const Camera &camera;
    glm::mat4 view_projection{1.0f};
    glm::mat4 view_projection_non_jittered{1.0f};
    std::optional<glm::vec3> gizmo_world_position;
    float output_content_scale = 1.0f;
    vk::Format swapchain_color_format;
};

void renderUiPass(vk::CommandBuffer cmd_buf, const FrameRenderContext &frame, const PassDefinition &pass_def,
                  vk::Extent2D target_extent, RenderTargetContainer &rt_container,
                  const RenderPassDispatchDependencies &dependencies);
#if PELICAN_WITH_IMGUI
void renderImGuiPass(vk::CommandBuffer cmd_buf, const FrameRenderContext &frame,
                     const PassDefinition &pass_def, vk::Extent2D target_extent,
                     RenderTargetContainer &rt_container,
                     const RenderPassDispatchDependencies &dependencies);
#endif
void renderDynamicPassDrawCalls(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def,
                                vk::Extent2D target_extent,
                                const RenderPassDispatchDependencies &dependencies,
                                RenderPassViewInvocation invocation = {});

} // namespace Pelican
