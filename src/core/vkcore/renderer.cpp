#include "renderer.hpp"
#include "../light/lightcontainer.hpp"
#include "../loader/basicconfig.hpp"
#include "../log.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/renderingpassjsonloader.hpp"
#include "battery/embed.hpp"
#include "core.hpp"
#include "render_pass_executor.hpp"
#include "rendertarget.hpp"
#include <chrono>
#include <filesystem>

namespace Pelican {

namespace {

bool outputsToSwapchain(const CompiledRenderingPass &rendering_pass) {
    for (const auto &pass : rendering_pass.passes) {
        for (const auto &rt_id : pass.definition.output_color) {
            if (isSwapchainRenderTarget(rt_id)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

Renderer::Renderer() : device{GET_MODULE(VulkanManageCore).getDevice()} {
    auto &pass_container = GET_MODULE(RenderingPassContainer);
    const auto &config = GET_MODULE(ProjectBasicConfig);

    try {
        const auto main_config_path = config.renderingConfigJson();
        if (std::filesystem::exists(main_config_path)) {
            GET_MODULE(RenderingPassJsonLoader).registerRenderingPassesFromJson(main_config_path);
        } else {
            throw std::runtime_error("Main rendering configuration JSON file not found: " + main_config_path);
        }
    } catch (const std::exception &e) {
        LOG_ERROR(logger, "Failed to load main rendering configuration: {}", e.what());
        throw;
    }

    current_rendering_pass_id = pass_container.getRenderingPassIdByName(config.defaultRenderingPass());
    if (!isValidRenderingPassId(current_rendering_pass_id)) {
        throw std::runtime_error("Rendering pass not found: " + config.defaultRenderingPass());
    }
    const auto &default_rendering_pass = pass_container.getCompiledRenderingPass(current_rendering_pass_id);
    if (!outputsToSwapchain(default_rendering_pass)) {
        throw std::runtime_error("Default rendering pass does not output to swapchain: " +
                                 config.defaultRenderingPass());
    }
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
