#include "renderer.hpp"
#include "../light/lightcontainer.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "core.hpp"
#include "render_pass_executor.hpp"
#include "renderer_config.hpp"
#include "rendertarget.hpp"
#include <chrono>

namespace Pelican {

Renderer::Renderer() : device{GET_MODULE(VulkanManageCore).getDevice()} {
    current_rendering_pass_id = loadDefaultRenderingPassFromConfig();
}

Renderer::~Renderer() {}

void Renderer::render() {
    static auto start_time = std::chrono::high_resolution_clock::now();

    auto &rt = GET_MODULE(RenderTarget);
    auto &pass_container = GET_MODULE(RenderingPassContainer);
    auto &pass_executor = GET_MODULE(RenderPassExecutor);

    {
        auto current_time = std::chrono::high_resolution_clock::now();
        const float time =
            std::chrono::duration<float, std::chrono::seconds::period>(current_time - start_time).count();
        GET_MODULE(LightContainer).updateAnimation(time);
    }

    const auto render_ctx = rt.render_begin();
    const auto &rendering_pass = pass_container.getCompiledRenderingPass(current_rendering_pass_id);

    for (const auto &pass : rendering_pass.passes) {
        pass_executor.execute(render_ctx, pass, render_target_layout_tracker);
    }

    rt.render_end();
}

} // namespace Pelican
