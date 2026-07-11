#include "renderer_config.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"
#include "../profiler.hpp"
#include "../renderer/debugdraw.hpp"
#include "../renderer/debugtext.hpp"
#include "../renderingpass/computetask.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/renderingpassconfigregistration.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../shader/shaderlibrary.hpp"
#include "core.hpp"
#include "rendertarget.hpp"
#include <cstdint>
#include <stdexcept>

namespace Pelican {

namespace {

bool hasOutputTransform(RenderingPassId rendering_pass_id) {
    const auto *frame_graph = GET_MODULE(FrameGraphRuntimeContainer).find(rendering_pass_id);
    return frame_graph != nullptr && !frame_graph->nodes.empty() &&
           frame_graph->nodes.back().kind == FramePlanNodeKind::output_transform;
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
        const auto main_config_json = config.renderingConfigJson();
        auto &rt_module = GET_MODULE(RenderTarget);
        auto &rt_container = GET_MODULE(RenderTargetContainer);
        auto &shader_library = GET_MODULE(ShaderLibrary);
        auto &fs_container = GET_MODULE(FullscreenPassContainer);
        auto &pass_container = GET_MODULE(RenderingPassContainer);
        auto &frame_graph_resources = GET_MODULE(FrameGraphResourceContainer);
        auto &compute_task_container = GET_MODULE(ComputeTaskContainer);
        auto &frame_graph_runtime = GET_MODULE(FrameGraphRuntimeContainer);
        auto &path_resolver = GET_MODULE(PathResolver);
        registerRenderingPassConfigFromJsonData(
            main_config_json, baseExtentFromConfig(config),
            RenderingPassConfigRegistrationDependencies{
                RenderingPassConfigRenderTargetDependencies{
                    rt_container,
                },
                RenderingPassConfigRuntimeDependencies{
                    rt_module,
                    shader_library,
                    fs_container,
                    path_resolver,
                    {},
                    config.usesProjectSource(),
                    []() -> DebugDraw & { return GET_MODULE(DebugDraw); },
                    []() -> DebugText & { return GET_MODULE(DebugText); },
                },
                frame_graph_resources,
                compute_task_container,
                frame_graph_runtime,
                pass_container,
            });
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

    if (!hasOutputTransform(rendering_pass_id)) {
        throw std::runtime_error("Default rendering pass has no terminal output_transform: " +
                                 default_pass_name);
    }

    return rendering_pass_id;
}

} // namespace Pelican
