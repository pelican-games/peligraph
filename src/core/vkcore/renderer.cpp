#include "renderer.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/fullscreenpassrenderer.hpp"
#include "../renderer/materialrender.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../renderer/uicontainer.hpp"
#include "../renderer/uirenderer.hpp"
#include "../launchconfig.hpp"
#include "../light/lightcontainer.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../material/materialcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/rendertargetimageviewresolver.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../appflow/enginetime.hpp"
#include "deletionqueue.hpp"
#include "render_pass_dispatch.hpp"
#include "render_pass_executor.hpp"
#include "renderer_config.hpp"
#include "rendertarget.hpp"
#include "util.hpp"

namespace Pelican {

namespace {

struct RenderFrameModules {
    RenderTarget &render_target;
    RenderTargetContainer &render_target_container;
    RenderingPassContainer &rendering_pass_container;
    RenderPassExecutor &pass_executor;
    VulkanUtils &vk_utils;
    MaterialRenderer &material_renderer;
    PolygonInstanceContainer &instance_container;
    const VertBufContainer &vert_buf_container;
    const MaterialContainer &material_container;
    FullscreenPassRenderer &fullscreen_pass_renderer;
    FullscreenPassContainer &fullscreen_pass_container;
    UiRenderer &ui_renderer;
    const UIContainer &ui_container;
    const Camera &camera;
    LightContainer &light_container;
};

RenderFrameModules resolveRenderFrameModules() {
    return RenderFrameModules{
        GET_MODULE(RenderTarget),
        GET_MODULE(RenderTargetContainer),
        GET_MODULE(RenderingPassContainer),
        GET_MODULE(RenderPassExecutor),
        GET_MODULE(VulkanUtils),
        GET_MODULE(MaterialRenderer),
        GET_MODULE(PolygonInstanceContainer),
        GET_MODULE(VertBufContainer),
        GET_MODULE(MaterialContainer),
        GET_MODULE(FullscreenPassRenderer),
        GET_MODULE(FullscreenPassContainer),
        GET_MODULE(UiRenderer),
        GET_MODULE(UIContainer),
        GET_MODULE(Camera),
        GET_MODULE(LightContainer),
    };
}

void updateFrameAnimation(LightContainer &light_container, double time) {
    light_container.updateAnimation(static_cast<float>(time));
}

void executeRenderingPasses(const FrameRenderContext &render_ctx, const CompiledRenderingPass &rendering_pass,
                            RenderFrameModules &modules, RenderTargetLayoutTracker &layout_tracker) {
    const MaterialRendererDependencies material_renderer_dependencies{modules.instance_container,
                                                                      modules.vert_buf_container,
                                                                      modules.material_container,
                                                                      modules.light_container,
                                                                      modules.camera,
                                                                      GET_MODULE(EngineTime).now()};
    const FullscreenPassRendererDependencies fullscreen_pass_renderer_dependencies{modules.fullscreen_pass_container,
                                                                                  modules.light_container};
    const UiRendererDependencies ui_renderer_dependencies{modules.ui_container};
    const RenderPassDispatchDependencies pass_dispatch_dependencies{
        modules.material_renderer,
        material_renderer_dependencies,
        modules.fullscreen_pass_renderer,
        fullscreen_pass_renderer_dependencies,
        modules.ui_renderer,
        ui_renderer_dependencies,
        modules.camera,
        modules.render_target.getSwapchainFormat()};
    const RenderPassExecutorDependencies pass_executor_dependencies{modules.render_target_container, modules.vk_utils,
                                                                    pass_dispatch_dependencies};

    for (const auto &pass : rendering_pass.passes) {
        modules.pass_executor.execute(render_ctx, pass, pass_executor_dependencies, layout_tracker);
    }
}

void rebindFullscreenInputs(RenderFrameModules &modules) {
    RenderTargetImageViewResolver rt_views{modules.render_target_container};
    for (const auto pass_id : modules.rendering_pass_container.getRegisteredPassIds()) {
        const auto &compiled_pass = modules.rendering_pass_container.getCompiledRenderingPass(pass_id);
        for (const auto &pass : compiled_pass.passes) {
            if (pass.definition.isFullscreen() && !pass.definition.input_targets.empty()) {
                modules.fullscreen_pass_container.setInputTextures(pass.pass_id, pass.definition.input_targets,
                                                                   rt_views);
            }
        }
    }
}

void handleShaderHotReload(RenderFrameModules &modules) {
    if (!GET_MODULE(EngineLaunchConfig).shader_hot_reload) {
        return;
    }

    if (GET_MODULE(ShaderLibrary).reloadModifiedSources() == 0) {
        return;
    }

    GET_MODULE(PipelineFactory).rebuildDirty();
    rebindFullscreenInputs(modules);
}

void handleFrameTargetResize(RenderFrameModules &modules, RenderTargetLayoutTracker &layout_tracker) {
    if (!modules.render_target.consumeExtentChanged()) {
        return;
    }

    modules.render_target_container.recreateForExtent(modules.render_target.getExtent());
    rebindFullscreenInputs(modules);
    layout_tracker.reset();
}

} // namespace

Renderer::Renderer() {
    current_rendering_pass_id = loadDefaultRenderingPassFromConfig();
}

Renderer::~Renderer() = default;

void Renderer::render() {
    GET_MODULE(DeletionQueue).beginFrame();

    auto modules = resolveRenderFrameModules();
    handleShaderHotReload(modules);
    updateFrameAnimation(modules.light_container, GET_MODULE(EngineTime).now());

    const auto render_ctx = modules.render_target.render_begin();
    handleFrameTargetResize(modules, render_target_layout_tracker);

    const auto &rendering_pass =
        modules.rendering_pass_container.getCompiledRenderingPass(current_rendering_pass_id);
    executeRenderingPasses(render_ctx, rendering_pass, modules, render_target_layout_tracker);

    modules.render_target.render_end();
}

} // namespace Pelican
