#pragma once

#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "rendertarget.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

class Camera;
struct FullscreenPassRendererDependencies;
class FullscreenPassRenderer;
struct MaterialRendererDependencies;
class MaterialRenderer;
class UiRenderer;

struct RenderPassDispatchDependencies {
    MaterialRenderer &material_renderer;
    const MaterialRendererDependencies &material_renderer_dependencies;
    FullscreenPassRenderer &fullscreen_pass_renderer;
    const FullscreenPassRendererDependencies &fullscreen_pass_renderer_dependencies;
    UiRenderer &ui_renderer;
    const Camera &camera;
    vk::Format swapchain_color_format;
};

void renderUiPass(vk::CommandBuffer cmd_buf, const FrameRenderContext &frame, const PassDefinition &pass_def,
                  vk::Extent2D target_extent, RenderTargetContainer &rt_container,
                  const RenderPassDispatchDependencies &dependencies);
void renderDynamicPassDrawCalls(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def,
                                const RenderPassDispatchDependencies &dependencies);

} // namespace Pelican
