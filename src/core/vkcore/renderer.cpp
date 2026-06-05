#include "renderer.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/fullscreenpassrenderer.hpp"
#include "../renderer/materialrender.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../renderer/uicontainer.hpp"
#include "../renderer/uirenderer.hpp"
#include "../light/lightcontainer.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../material/materialcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "core.hpp"
#include "render_pass_dispatch.hpp"
#include "render_pass_executor.hpp"
#include "renderer_config.hpp"
#include "rendertarget.hpp"
#include "util.hpp"
#include <chrono>

namespace Pelican {

Renderer::Renderer() : device{GET_MODULE(VulkanManageCore).getDevice()} {
    current_rendering_pass_id = loadDefaultRenderingPassFromConfig();
}

Renderer::~Renderer() {}

void Renderer::render() {
    static auto start_time = std::chrono::high_resolution_clock::now();

    auto &rt = GET_MODULE(RenderTarget);
    auto &rt_container = GET_MODULE(RenderTargetContainer);
    auto &pass_container = GET_MODULE(RenderingPassContainer);
    auto &pass_executor = GET_MODULE(RenderPassExecutor);
    auto &vk_utils = GET_MODULE(VulkanUtils);
    auto &material_renderer = GET_MODULE(MaterialRenderer);
    auto &instance_container = GET_MODULE(PolygonInstanceContainer);
    const auto &vert_buf_container = GET_MODULE(VertBufContainer);
    const auto &material_container = GET_MODULE(MaterialContainer);
    auto &fullscreen_pass_renderer = GET_MODULE(FullscreenPassRenderer);
    auto &fullscreen_pass_container = GET_MODULE(FullscreenPassContainer);
    auto &ui_renderer = GET_MODULE(UiRenderer);
    const auto &ui_container = GET_MODULE(UIContainer);
    const auto &camera = GET_MODULE(Camera);
    auto &light_container = GET_MODULE(LightContainer);

    {
        auto current_time = std::chrono::high_resolution_clock::now();
        const float time =
            std::chrono::duration<float, std::chrono::seconds::period>(current_time - start_time).count();
        light_container.updateAnimation(time);
    }

    const auto render_ctx = rt.render_begin();
    const auto &rendering_pass = pass_container.getCompiledRenderingPass(current_rendering_pass_id);
    const MaterialRendererDependencies material_renderer_dependencies{
        instance_container, vert_buf_container, material_container, light_container, camera};
    const FullscreenPassRendererDependencies fullscreen_pass_renderer_dependencies{
        fullscreen_pass_container, light_container};
    const UiRendererDependencies ui_renderer_dependencies{ui_container};
    const RenderPassDispatchDependencies pass_dispatch_dependencies{
        material_renderer, material_renderer_dependencies, fullscreen_pass_renderer,
        fullscreen_pass_renderer_dependencies, ui_renderer, ui_renderer_dependencies, camera, rt.getSwapchainFormat()};
    const RenderPassExecutorDependencies pass_executor_dependencies{
        rt_container, vk_utils, pass_dispatch_dependencies};

    for (const auto &pass : rendering_pass.passes) {
        pass_executor.execute(render_ctx, pass, pass_executor_dependencies, render_target_layout_tracker);
    }

    rt.render_end();
}

} // namespace Pelican
