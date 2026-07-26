#include "renderer_config.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../launchconfig.hpp"
#include "../log.hpp"
#include "../profiler.hpp"
#include "../renderer/debugdraw.hpp"
#include "../renderer/debugtext.hpp"
#include "../renderer/atlasassetresource.hpp"
#include "../renderer/shadowdepthpasscontainer.hpp"
#include "../renderer/spriterenderer.hpp"
#include "../renderer/spritescene.hpp"
#include "../renderer/uicontainer.hpp"
#include "../renderer/uirenderer.hpp"
#include "../renderer/velocitypasscontainer.hpp"
#include "../renderingpass/computetask.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/renderingpassconfigregistration.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../ui/module.hpp"
#include "core.hpp"
#include "rendertarget.hpp"
#include "rendertiming.hpp"
#if PELICAN_WITH_OPENXR
#include "../openxr/openxrmirrorsink.hpp"
#endif
#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

#if PELICAN_RUNTIME_SHADER_COMPILER
constexpr bool runtimeShaderCompilerEnabled = true;
#else
constexpr bool runtimeShaderCompilerEnabled = false;
#endif

bool hasOutputTransform(
    const RenderPipelineRuntimeGeneration &generation,
    RenderingPassId rendering_pass_id) {
    const auto *program = generation.find(rendering_pass_id);
    return program != nullptr &&
           !program->frame_graph.nodes.empty() &&
           program->frame_graph.nodes.back().kind ==
               FramePlanNodeKind::output_transform;
}

RenderingPassConfigRegistrationDependencies registrationDependencies(
    RenderingPassConfigRegistrationDependencies::Options options = {}) {
    auto &rt_module = GET_MODULE(RenderTarget);
    auto &rt_container = GET_MODULE(RenderTargetContainer);
    auto &shader_library = GET_MODULE(ShaderLibrary);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    auto &fs_container = GET_MODULE(FullscreenPassContainer);
    auto &shadow_depth_passes =
        GET_MODULE(ShadowDepthPassContainer);
    auto &velocity_passes =
        GET_MODULE(VelocityPassContainer);
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
            pipeline_factory,
            shadow_depth_passes,
            velocity_passes,
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

RenderingPassId requireDefaultPass(std::string name) {
    const auto generation =
        GET_MODULE(FrameGraphRuntimeContainer).snapshot();
    if (generation == nullptr) {
        throw std::runtime_error(
            "Rendering pipeline generation is unavailable");
    }
    const auto found = generation->name_to_id.find(name);
    const auto rendering_pass_id =
        found != generation->name_to_id.end()
            ? found->second
            : invalidRenderingPassId();
    if (!isValidRenderingPassId(rendering_pass_id)) {
        throw std::runtime_error("Rendering pass not found: " + name);
    }
    if (!hasOutputTransform(*generation, rendering_pass_id)) {
        throw std::runtime_error("Default rendering pass has no terminal output_transform: " +
                                 name);
    }
    return rendering_pass_id;
}

void validateDefaultPass(
    const RenderPipelineRuntimeGeneration &generation,
    std::string_view name) {
    const auto found =
        generation.name_to_id.find(std::string{name});
    if (found == generation.name_to_id.end()) {
        throw std::runtime_error(
            "Rendering pass not found: " +
            std::string{name});
    }
    if (!hasOutputTransform(generation, found->second)) {
        throw std::runtime_error(
            "Default rendering pass has no terminal output_transform: " +
            std::string{name});
    }
}

bool generationEnablesFeature(
    const RenderPipelineRuntimeGeneration &generation,
    std::string_view name) {
    return std::find(
               generation.enabled_feature_names.begin(),
               generation.enabled_feature_names.end(),
               name) !=
           generation.enabled_feature_names.end();
}

template <typename Module>
void requireInitializedRuntimeModule(
    std::string_view feature,
    std::string_view module) {
    if (FastModuleContainer::tryGet<Module>() != nullptr) {
        return;
    }
    throw std::runtime_error(
        "Render pipeline hot reload cannot enable feature '" +
        std::string{feature} +
        "' after module creation is frozen; missing runtime module " +
        std::string{module});
}

void validateFrozenRuntimeFeatureModules(
    const RenderPipelineRuntimeGeneration &generation) {
    if (!FastModuleContainer::isCreationFrozen()) {
        return;
    }
    if (generationEnablesFeature(
            generation, "debug_draw")) {
        requireInitializedRuntimeModule<DebugDraw>(
            "debug_draw", "DebugDraw");
    }
    if (generationEnablesFeature(
            generation, "debug_text")) {
        requireInitializedRuntimeModule<DebugText>(
            "debug_text", "DebugText");
    }
    if (generationEnablesFeature(
            generation, "gpu_timing")) {
        requireInitializedRuntimeModule<RenderTiming>(
            "gpu_timing", "RenderTiming");
    }
    if (generationEnablesFeature(generation, "ui")) {
        requireInitializedRuntimeModule<UiRenderer>(
            "ui", "UiRenderer");
        requireInitializedRuntimeModule<UIContainer>(
            "ui", "UIContainer");
        requireInitializedRuntimeModule<ui::UiModule>(
            "ui", "ui::UiModule");
    }
    if (generationEnablesFeature(
            generation, "sprite")) {
        requireInitializedRuntimeModule<SpriteScene>(
            "sprite", "SpriteScene");
        requireInitializedRuntimeModule<SpriteRenderer>(
            "sprite", "SpriteRenderer");
        requireInitializedRuntimeModule<AtlasAssetResource>(
            "sprite", "AtlasAssetResource");
    }
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
    return loadRenderGraphVariantsFromConfigData(
        GET_MODULE(ProjectBasicConfig).renderingConfigJson());
}

RenderGraphVariantConfig loadRenderGraphVariantsFromConfigData(
    std::string_view rendering_config_json) {
    ScopedLogTimer timer{"load default rendering pass from config"};

    const auto &config = GET_MODULE(ProjectBasicConfig);
    // The launch target owns the effective output extent. In particular,
    // headless runs may intentionally override project.json's initial window
    // size, so using the project value here would register output-relative
    // intermediates against a different base than the compiled/runtime
    // resolution contract.
    const auto base_extent =
        GET_MODULE(RenderTarget).getExtent();
    const auto default_pass_name = config.defaultRenderingPass();
    // Compose and validate the third graph before runtime modules are frozen.
    // It remains a data program and intentionally performs no shared render
    // target/pass registration.
    auto &path_resolver = GET_MODULE(PathResolver);
    auto preview = precompilePreviewGraph(
        rendering_config_json,
        [&path_resolver](std::string_view ref) { return path_resolver.loadText(ref); },
        runtimeShaderCompilerEnabled);
    std::vector<RenderingPassConfigRegistrationDependencies>
        variant_dependencies;
    RenderingPassConfigRegistrationDependencies::Options
        flat_options;
    flat_options.validate_prepared_generation =
        [default_pass_name](
            const RenderPipelineRuntimeGeneration &generation) {
            validateDefaultPass(generation,
                                default_pass_name);
            validateFrozenRuntimeFeatureModules(
                generation);
        };
#if PELICAN_WITH_OPENXR
    const bool xr_active =
        GET_MODULE(EngineLaunchConfig).xr_active;
    if (xr_active) {
        // Flat and XR are one publication/retirement unit. Both variants
        // share targets, shaders, and pipelines, so a combined owner is the
        // honest lifetime boundary.
        flat_options.gpu_owner_scope =
            "render_pipeline/variants";
    }
#endif
    variant_dependencies.push_back(
        registrationDependencies(
            std::move(flat_options)));
#if PELICAN_WITH_OPENXR
    if (xr_active) {
        RenderingPassConfigRegistrationDependencies::Options xr_options;
        xr_options.graph_variant = RenderPipelineGraphVariant::xr;
        // The OpenXR composition target owns one two-layer color swapchain
        // and exposes a command context spanning the full view family.
        // Device and per-pass capability checks still decide which scopes
        // may compile to multiview; unsupported scopes remain sequential.
        xr_options.enable_multiview_runtime = true;
        xr_options.enable_external_depth_export = true;
        xr_options.publish_enabled_features = false;
        xr_options.gpu_owner_scope =
            "render_pipeline/variants";
        xr_options.prepare_additional_gpu_resources =
            [base_extent] {
                registerXrMirrorIntermediate(base_extent);
            };
        const auto xr_default_pass_name =
            default_pass_name + "#xr";
        xr_options.validate_prepared_generation =
            [xr_default_pass_name](
                const RenderPipelineRuntimeGeneration &generation) {
                validateDefaultPass(
                    generation, xr_default_pass_name);
            };
        variant_dependencies.push_back(
            registrationDependencies(
                std::move(xr_options)));
    }
#endif
    try {
        auto registrations =
            registerRenderingPassConfigVariantsFromJsonData(
                rendering_config_json,
                base_extent,
                std::move(variant_dependencies));
        RenderGraphVariantConfig variants{
            .flat = requireDefaultPass(
                default_pass_name),
            .preview = std::move(preview),
        };
#if PELICAN_WITH_OPENXR
        if (xr_active) {
            variants.xr = requireDefaultPass(
                default_pass_name + "#xr");
            variants.xr_excluded_features =
                registrations.at(1)
                    .excluded_feature_names;
        }
#endif
        return variants;
    } catch (const std::exception &error) {
        LOG_ERROR(
            logger,
            "Failed to load main rendering configuration: {}",
            error.what());
        throw;
    }
}

} // namespace Pelican
