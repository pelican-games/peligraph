#include "renderer_config.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../loader/basicconfig.hpp"
#include "../log.hpp"
#include "../profiler.hpp"
#include "../renderingpass/renderingpassconfigregistration.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../shader/shaderlibrary.hpp"
#include "core.hpp"
#include "rendertarget.hpp"
#include <cstdint>
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

vk::Extent2D baseExtentFromConfig(const ProjectBasicConfig &config) {
    const auto window_size = config.initialWindowSize();
    return vk::Extent2D{
        static_cast<uint32_t>(window_size.width),
        static_cast<uint32_t>(window_size.height),
    };
}

void registerConfiguredRenderingPasses(const ProjectBasicConfig &config) {
    ScopedLogTimer timer{"register configured rendering passes"};

    try {
        const auto main_config_path = config.renderingConfigJson();
        if (std::filesystem::exists(main_config_path)) {
            auto &rt_module = GET_MODULE(RenderTarget);
            auto &rt_container = GET_MODULE(RenderTargetContainer);
            auto &shader_library = GET_MODULE(ShaderLibrary);
            auto &fs_container = GET_MODULE(FullscreenPassContainer);
            auto &pass_container = GET_MODULE(RenderingPassContainer);
            registerRenderingPassConfigFromJson(
                main_config_path, baseExtentFromConfig(config),
                RenderingPassConfigRegistrationDependencies{
                    RenderingPassConfigRenderTargetDependencies{
                        rt_container,
                    },
                    RenderingPassConfigRuntimeDependencies{
                        rt_module,
                        shader_library,
                        fs_container,
                    },
                    pass_container,
                });
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
    ScopedLogTimer timer{"load default rendering pass from config"};

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
