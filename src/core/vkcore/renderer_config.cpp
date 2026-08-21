#include "renderer_config.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../launchconfig.hpp"
#include "../log.hpp"
#include "../material/materialcontainer.hpp"
#include "../profiler.hpp"
#include "../renderer/debugdraw.hpp"
#include "../renderer/debugtext.hpp"
#include "../renderer/gizmo.hpp"
#include "../renderer/atlasassetresource.hpp"
#include "../renderer/shadowdepthpasscontainer.hpp"
#include "../renderer/spriterenderer.hpp"
#include "../renderer/spritescene.hpp"
#include "../renderer/uicontainer.hpp"
#include "../renderer/uirenderer.hpp"
#include "../renderer/velocitypasscontainer.hpp"
#include "../renderingpass/computetask.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/renderingsamplecount.hpp"
#include "../renderingpass/renderingpassconfigregistration.hpp"
#include "../renderingpass/renderingpassconfigloader.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../ui/module.hpp"
#include "core.hpp"
#include "rendertarget.hpp"
#include "rendertiming.hpp"
#include "../../project/renderfeatureoverlay.hpp"
#include "../../project/featurecompose.hpp"
#include "../../project/vulkanviewplanning.hpp"
#if PELICAN_WITH_OPENXR
#include "../openxr/openxrmirrorsink.hpp"
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Pelican {

namespace {

template <typename Module>
bool runtimeModuleInitialized() noexcept {
    return FastModuleContainer::isInitialized<Module>();
}

static constexpr std::array<RenderRuntimeModuleRequirement, 1>
    debug_draw_modules{{
        {"DebugDraw", &runtimeModuleInitialized<DebugDraw>},
    }};
static constexpr std::array<RenderRuntimeModuleRequirement, 1>
    debug_text_modules{{
        {"DebugText", &runtimeModuleInitialized<DebugText>},
    }};
static constexpr std::array<RenderRuntimeModuleRequirement, 1>
    gizmo_modules{{
        {"Gizmo", &runtimeModuleInitialized<Gizmo>},
    }};
static constexpr std::array<RenderRuntimeModuleRequirement, 1>
    gpu_timing_modules{{
        {"RenderTiming", &runtimeModuleInitialized<RenderTiming>},
    }};
static constexpr std::array<RenderRuntimeModuleRequirement, 3>
    ui_modules{{
        {"UiRenderer", &runtimeModuleInitialized<UiRenderer>},
        {"UIContainer", &runtimeModuleInitialized<UIContainer>},
        {"ui::UiModule", &runtimeModuleInitialized<ui::UiModule>},
    }};
static constexpr std::array<RenderRuntimeModuleRequirement, 3>
    sprite_modules{{
        {"SpriteScene", &runtimeModuleInitialized<SpriteScene>},
        {"SpriteRenderer", &runtimeModuleInitialized<SpriteRenderer>},
        {"AtlasAssetResource", &runtimeModuleInitialized<AtlasAssetResource>},
    }};

static constexpr std::array<RenderFeatureRuntimeModuleRequirement, 6>
    runtime_feature_modules{{
        {"debug_draw", debug_draw_modules},
        {"debug_text", debug_text_modules},
        {"gizmo", gizmo_modules},
        {"gpu_timing", gpu_timing_modules},
        {"ui", ui_modules},
        {"sprite", sprite_modules},
    }};

const RenderFeatureRuntimeModuleRequirement *
findRuntimeFeatureModules(std::string_view feature) noexcept {
    const auto found = std::ranges::find(
        runtime_feature_modules, feature,
        &RenderFeatureRuntimeModuleRequirement::feature);
    return found == runtime_feature_modules.end()
               ? nullptr
               : &*found;
}

void requireRuntimeFeatureModules(
    const RenderFeatureRuntimeModuleRequirement &requirement,
    const std::function<bool(std::string_view)> &module_initialized) {
    for (const auto &module : requirement.modules) {
        const bool initialized = module_initialized
                                     ? module_initialized(module.module)
                                     : module.initialized();
        if (initialized) continue;
        throw std::runtime_error(
            "Render pipeline hot reload cannot enable feature '" +
            std::string{requirement.feature} +
            "' after module creation is frozen; missing runtime module " +
            std::string{module.module});
    }
}

RenderFeatureRuntimeAvailabilityEnvironment
currentRenderFeatureRuntimeAvailabilityEnvironment() {
#if PELICAN_RUNTIME_SHADER_COMPILER
    constexpr bool runtime_shader_compiler_enabled = true;
#else
    constexpr bool runtime_shader_compiler_enabled = false;
#endif
    const auto &capabilities =
        GET_MODULE(VulkanManageCore).getRuntimeCapabilities();
    RenderingTargetPlanDeviceFacts device_facts{
        .multiview = capabilities.multiview,
        .max_multiview_view_count =
            capabilities.multiview ? 1u : 0u,
        .ray_query = capabilities.ray_query,
        .ray_tracing_pipeline =
            capabilities.ray_tracing_pipeline,
        .transient_attachments = true,
        .dynamic_rendering_local_read =
            capabilities.dynamic_rendering_local_read,
    };
    return {
        .runtime_shader_compiler_enabled =
            runtime_shader_compiler_enabled,
        .target_endpoint = renderingTargetRuntimeEndpoint(
            device_facts,
            capabilities.dynamic_rendering_local_read),
        .runtime_module_creation_frozen =
            FastModuleContainer::isCreationFrozen(),
    };
}

} // namespace

std::span<const std::string_view>
renderFeaturesRequiringRuntimeModules() noexcept {
    static const auto names = [] {
        std::array<std::string_view,
                   runtime_feature_modules.size()>
            result{};
        std::ranges::transform(
            runtime_feature_modules, result.begin(),
            &RenderFeatureRuntimeModuleRequirement::feature);
        return result;
    }();
    return names;
}

std::span<const RenderFeatureRuntimeModuleRequirement>
renderFeatureRuntimeModuleRequirements() noexcept {
    return runtime_feature_modules;
}

bool renderFeatureRequiresRuntimeModule(std::string_view name) noexcept {
    return findRuntimeFeatureModules(name) != nullptr;
}

void requireRenderFeatureRuntimeAvailability(
    std::string_view feature_name,
    const nlohmann::json &feature_document,
    const RenderFeatureRuntimeAvailabilityEnvironment &environment) {
    if (environment.runtime_module_creation_frozen) {
        if (const auto *modules =
                findRuntimeFeatureModules(feature_name)) {
            requireRuntimeFeatureModules(
                *modules,
                environment.runtime_module_initialized);
        }
    }
    if (!environment.runtime_shader_compiler_enabled &&
        renderFeatureRequiresRuntimeShaderCompiler(
            feature_document, feature_name)) {
        throw std::runtime_error(std::string{
            renderFeatureRuntimeCompilerRequiredMessage});
    }
    const auto required_capabilities =
        renderFeatureRequiredCapabilities(
            feature_document, feature_name);
    requireVulkanEndpointCapabilities(
        environment.target_endpoint,
        required_capabilities);
}

RenderFeatureRuntimeAvailability evaluateRenderFeatureRuntimeAvailability(
    std::string_view feature_name,
    const nlohmann::json &feature_document,
    const RenderFeatureRuntimeAvailabilityEnvironment &environment) noexcept {
    try {
        requireRenderFeatureRuntimeAvailability(
            feature_name, feature_document, environment);
        return {.available = true};
    } catch (const std::exception &error) {
        return {
            .available = false,
            .unavailable_reason = error.what(),
        };
    } catch (...) {
        return {
            .available = false,
            .unavailable_reason =
                "unknown render feature availability failure",
        };
    }
}

RenderFeatureRuntimeAvailability currentRenderFeatureRuntimeAvailability(
    std::string_view feature_name,
    const nlohmann::json &feature_document) noexcept {
    try {
        return evaluateRenderFeatureRuntimeAvailability(
            feature_name, feature_document,
            currentRenderFeatureRuntimeAvailabilityEnvironment());
    } catch (const std::exception &error) {
        return {
            .available = false,
            .unavailable_reason = error.what(),
        };
    } catch (...) {
        return {
            .available = false,
            .unavailable_reason =
                "unknown render feature environment failure",
        };
    }
}

void requireCurrentRenderFeatureRuntimeAvailability(
    std::string_view feature_name,
    const nlohmann::json &feature_document) {
    requireRenderFeatureRuntimeAvailability(
        feature_name, feature_document,
        currentRenderFeatureRuntimeAvailabilityEnvironment());
}

namespace {

bool hasOutputTransform(
    const RendererRuntimeGeneration &generation,
    RenderingPassId rendering_pass_id) {
    const auto *program = generation.find(rendering_pass_id);
    return program != nullptr &&
           !program->frame_graph.nodes.empty() &&
           program->frame_graph.nodes.back().kind ==
               FramePlanNodeKind::output_transform;
}

RenderingPassConfigRegistrationDependencies registrationDependencies(
    RenderingPassConfigRegistrationDependencies::Options options = {}) {
    options.validate_render_feature =
        [](std::string_view feature_name,
           const nlohmann::json &feature_document) {
            requireCurrentRenderFeatureRuntimeAvailability(
                feature_name, feature_document);
        };
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
            []() -> Gizmo & { return GET_MODULE(Gizmo); },
            []() -> DebugText & { return GET_MODULE(DebugText); },
        },
        frame_graph_resources,
        compute_task_container,
        frame_graph_runtime,
        pass_container,
        std::move(options),
    };
}

RenderingPassId validateDefaultPass(
    const RendererRuntimeGeneration &generation,
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
    return found->second;
}

bool generationEnablesFeature(
    const RendererRuntimeGeneration &generation,
    std::string_view name) {
    return std::find(
               generation.enabled_feature_names.begin(),
               generation.enabled_feature_names.end(),
               name) !=
           generation.enabled_feature_names.end();
}

void validateFrozenRuntimeFeatureModules(
    const RendererRuntimeGeneration &generation) {
    if (!FastModuleContainer::isCreationFrozen()) {
        return;
    }
    for (const auto &requirement :
         renderFeatureRuntimeModuleRequirements()) {
        if (generationEnablesFeature(
                generation, requirement.feature)) {
            requireRuntimeFeatureModules(requirement, {});
        }
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

RenderGraphVariantConfig
loadRenderGraphVariantsFromConfigWithStartupFeatureOverlays() {
    const auto &overlays =
        GET_MODULE(EngineLaunchConfig).render_feature_overlays;
    if (overlays.empty()) {
        return loadRenderGraphVariantsFromConfig();
    }
    return loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
        GET_MODULE(ProjectBasicConfig).renderingConfigJson());
}

RenderGraphVariantConfig
loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
    std::string_view rendering_config_json,
    RenderGraphVariantLoadHooks hooks) {
    const auto &overlays =
        GET_MODULE(EngineLaunchConfig).render_feature_overlays;
    if (overlays.empty()) {
        return loadRenderGraphVariantsFromConfigData(
            rendering_config_json, std::move(hooks));
    }

    const auto authored = loadRenderingPassConfigJsonFromString(
        rendering_config_json, "ProjectBasicConfig");
    const auto effective = applyRenderFeatureOverlays(
        authored, overlays,
        [](std::string_view reference) {
            return GET_MODULE(PathResolver).loadText(reference);
        });
    return loadRenderGraphVariantsFromConfigData(
        effective.dump(), std::move(hooks));
}

RenderGraphVariantConfig loadRenderGraphVariantsFromConfigData(
    std::string_view rendering_config_json,
    RenderGraphVariantLoadHooks hooks) {
    static_assert(std::is_nothrow_move_constructible_v<
                  RenderGraphVariantConfig>);
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
    RenderingPassId prepared_flat_rendering_pass_id =
        invalidRenderingPassId();
    std::vector<RenderingPassConfigRegistrationDependencies>
        variant_dependencies;
    RenderingPassConfigRegistrationDependencies::Options
        flat_options;
    flat_options.load_document = hooks.load_document;
    flat_options.validate_prepared_generation =
        [default_pass_name, &prepared_flat_rendering_pass_id,
         validate_live_materials =
             hooks.validate_live_materials](
            const RendererRuntimeGeneration &generation) {
            prepared_flat_rendering_pass_id =
                validateDefaultPass(generation,
                                    default_pass_name);
            validateFrozenRuntimeFeatureModules(
                generation);
            if (validate_live_materials) {
                if (auto *materials =
                    FastModuleContainer::tryGet<
                        MaterialContainer>()) {
                    materials
                        ->validateRuntimeGenerationCompatibility(
                            generation);
                }
            }
        };
    flat_options.before_publish_prepared_generation =
        std::move(hooks.before_publish);
#if PELICAN_WITH_OPENXR
    const bool xr_active =
        GET_MODULE(EngineLaunchConfig).xr_active;
    RenderingPassId prepared_xr_rendering_pass_id =
        invalidRenderingPassId();
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
        xr_options.load_document = hooks.load_document;
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
            [xr_default_pass_name,
             &prepared_xr_rendering_pass_id](
                const RendererRuntimeGeneration &generation) {
                prepared_xr_rendering_pass_id = validateDefaultPass(
                    generation, xr_default_pass_name);
            };
        variant_dependencies.push_back(
            registrationDependencies(
                std::move(xr_options)));
    }
#endif
    try {
        auto family =
            registerRenderGraphVariantFamilyFromJsonData(
                rendering_config_json,
                base_extent,
                std::move(variant_dependencies));
        RenderGraphVariantConfig variants{
            .flat = prepared_flat_rendering_pass_id,
            .preview =
                std::move(family.preview),
        };
#if PELICAN_WITH_OPENXR
        if (xr_active) {
            variants.xr = prepared_xr_rendering_pass_id;
            variants.xr_excluded_features =
                std::move(family.runtime_variants[1]
                              .excluded_feature_names);
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
