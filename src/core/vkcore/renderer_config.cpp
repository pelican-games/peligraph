#include "renderer_config.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../launchconfig.hpp"
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
#if PELICAN_WITH_OPENXR
#include "../openxr/openxrmirrorsink.hpp"
#endif
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

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

RenderingPassConfigRegistrationDependencies registrationDependencies(
    RenderingPassConfigRegistrationDependencies::Options options = {}) {
    auto &rt_module = GET_MODULE(RenderTarget);
    auto &rt_container = GET_MODULE(RenderTargetContainer);
    auto &shader_library = GET_MODULE(ShaderLibrary);
    auto &fs_container = GET_MODULE(FullscreenPassContainer);
    auto &pass_container = GET_MODULE(RenderingPassContainer);
    auto &frame_graph_resources = GET_MODULE(FrameGraphResourceContainer);
    auto &compute_task_container = GET_MODULE(ComputeTaskContainer);
    auto &frame_graph_runtime = GET_MODULE(FrameGraphRuntimeContainer);
    auto &path_resolver = GET_MODULE(PathResolver);
    return {
        RenderingPassConfigRenderTargetDependencies{rt_container},
        RenderingPassConfigRuntimeDependencies{
            rt_module,
            shader_library,
            fs_container,
            path_resolver,
            {},
            GET_MODULE(ProjectBasicConfig).usesProjectSource(),
            []() -> DebugDraw & { return GET_MODULE(DebugDraw); },
            []() -> DebugText & { return GET_MODULE(DebugText); },
        },
        frame_graph_resources,
        compute_task_container,
        frame_graph_runtime,
        pass_container,
        std::move(options),
    };
}

RenderingPassConfigRegistrationResult registerConfiguredRenderingPasses(
    const ProjectBasicConfig &config,
    RenderingPassConfigRegistrationDependencies::Options options = {}) {
    ScopedLogTimer timer{"register configured rendering passes"};

    try {
        const auto main_config_json = config.renderingConfigJson();
        return registerRenderingPassConfigFromJsonData(
            main_config_json, baseExtentFromConfig(config),
            registrationDependencies(std::move(options)));
    } catch (const std::exception &e) {
        LOG_ERROR(logger, "Failed to load main rendering configuration: {}", e.what());
        throw;
    }
}

RenderingPassId requireDefaultPass(std::string name) {
    const auto rendering_pass_id =
        GET_MODULE(RenderingPassContainer).getRenderingPassIdByName(name);
    if (!isValidRenderingPassId(rendering_pass_id)) {
        throw std::runtime_error("Rendering pass not found: " + name);
    }
    if (!hasOutputTransform(rendering_pass_id)) {
        throw std::runtime_error("Default rendering pass has no terminal output_transform: " +
                                 name);
    }
    return rendering_pass_id;
}

#if PELICAN_WITH_OPENXR
void registerXrMirrorIntermediate(vk::Extent2D extent) {
    auto &targets = GET_MODULE(RenderTargetContainer);
    const auto display = targets.getRenderTargetIdByName("display");
    if (!isConcreteRenderTarget(display)) {
        throw std::runtime_error(
            "OpenXR mirror requires the canonical engine-owned display intermediate");
    }
    const auto metadata = targets.getMetadata(display);
    targets.registerRenderTarget(
        std::string{OpenXr::xr_mirror_intermediate_name}, extent,
        "explicit(" + vk::to_string(metadata.format) + ")", "color", 1.0f,
        metadata.extent, metadata.format,
        vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        vma::MemoryUsage::eAutoPreferDevice);
}
#endif

} // namespace

RenderingPassId loadDefaultRenderingPassFromConfig() {
    return loadRenderGraphVariantsFromConfig().flat;
}

RenderGraphVariantConfig loadRenderGraphVariantsFromConfig() {
    ScopedLogTimer timer{"load default rendering pass from config"};

    const auto &config = GET_MODULE(ProjectBasicConfig);

    registerConfiguredRenderingPasses(config);

    const auto default_pass_name = config.defaultRenderingPass();
    // Compose and validate the third graph before runtime modules are frozen.
    // It remains a data program and intentionally performs no shared render
    // target/pass registration.
    auto &path_resolver = GET_MODULE(PathResolver);
    auto preview = precompilePreviewGraph(
        config.renderingConfigJson(),
        [&path_resolver](std::string_view ref) { return path_resolver.loadText(ref); },
        config.usesProjectSource());
    RenderGraphVariantConfig variants{.flat = requireDefaultPass(default_pass_name),
                                      .preview = std::move(preview)};
#if PELICAN_WITH_OPENXR
    if (GET_MODULE(EngineLaunchConfig).xr_active) {
        registerXrMirrorIntermediate(baseExtentFromConfig(config));
        RenderingPassConfigRegistrationDependencies::Options xr_options;
        xr_options.graph_variant = RenderPipelineGraphVariant::xr;
        xr_options.publish_enabled_features = false;
        const auto xr_registration =
            registerConfiguredRenderingPasses(config, std::move(xr_options));
        variants.xr = requireDefaultPass(default_pass_name + "#xr");
        variants.xr_excluded_features = xr_registration.excluded_feature_names;
    }
#endif
    return variants;
}

} // namespace Pelican
