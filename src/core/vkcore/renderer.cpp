#include "renderer.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/fullscreenpassrenderer.hpp"
#include "../renderer/materialrender.hpp"
#include "../renderer/uirenderer.hpp"
#include "../light/lightcontainer.hpp"
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
    auto &fullscreen_pass_renderer = GET_MODULE(FullscreenPassRenderer);
    auto &ui_renderer = GET_MODULE(UiRenderer);
    const auto &camera = GET_MODULE(Camera);

    {
        auto current_time = std::chrono::high_resolution_clock::now();
        const float time =
            std::chrono::duration<float, std::chrono::seconds::period>(current_time - start_time).count();
        GET_MODULE(LightContainer).updateAnimation(time);
    }

    const auto render_ctx = rt.render_begin();
    const auto &rendering_pass = pass_container.getCompiledRenderingPass(current_rendering_pass_id);
    const RenderPassDispatchDependencies pass_dispatch_dependencies{
        material_renderer, fullscreen_pass_renderer, ui_renderer, camera, rt.getSwapchainFormat()};
    const RenderPassExecutorDependencies pass_executor_dependencies{
        rt_container, vk_utils, pass_dispatch_dependencies};

    for (const auto &pass : rendering_pass.passes) {
        pass_executor.execute(render_ctx, pass, pass_executor_dependencies, render_target_layout_tracker);
    }

    rt.render_end();
}

} // namespace Pelican
