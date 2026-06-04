#include "renderer_config.hpp"
#include "../loader/basicconfig.hpp"
#include "../log.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/renderingpassjsonloader.hpp"
#include "core.hpp"
#include <filesystem>
#include <stdexcept>

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

void registerConfiguredRenderingPasses(const ProjectBasicConfig &config) {
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
}

} // namespace

RenderingPassId loadDefaultRenderingPassFromConfig() {
    auto &pass_container = GET_MODULE(RenderingPassContainer);
    const auto &config = GET_MODULE(ProjectBasicConfig);

    registerConfiguredRenderingPasses(config);

    const auto default_pass_name = config.defaultRenderingPass();
    const auto rendering_pass_id = pass_container.getRenderingPassIdByName(default_pass_name);
    if (!isValidRenderingPassId(rendering_pass_id)) {
        throw std::runtime_error("Rendering pass not found: " + default_pass_name);
    }

    const auto &default_rendering_pass = pass_container.getCompiledRenderingPass(rendering_pass_id);
    if (!outputsToSwapchain(default_rendering_pass)) {
        throw std::runtime_error("Default rendering pass does not output to swapchain: " + default_pass_name);
    }

    return rendering_pass_id;
}

} // namespace Pelican
