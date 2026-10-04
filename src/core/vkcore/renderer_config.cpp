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
#include "../../project/graphcomposition.hpp"
#include "../../project/renderconfigdocument.hpp"
#include "../../project/renderconfigsection.hpp"
#include "../../project/renderpipeline.hpp"
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
#include <typeinfo>
#include <utility>

namespace Pelican {

std::string renderPipelineVariantGpuOwnerScope(
    bool xr_active,
    RenderPipelineGraphVariant variant) {
    if (xr_active) {
#if PELICAN_WITH_OPENXR
        if (variant == RenderPipelineGraphVariant::flat ||
            variant == RenderPipelineGraphVariant::xr) {
            return "render_pipeline/variants";
        }
#else
        (void)variant;
        throw std::runtime_error(
            "XR graph variant is unavailable in this build");
#endif
    }
    return "render_pipeline/" +
           std::string{renderPipelineGraphVariantName(variant)};
}

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
        RenderTargetDefinition{
            .name = std::string{OpenXr::xr_mirror_intermediate_name},
            .format_class =
                "explicit(" + vk::to_string(metadata.format) + ")",
            .role = "color",
            .extent_scale = 1.0f,
            .fixed_extent = metadata.extent,
            .format = metadata.format,
            .usage = vk::ImageUsageFlagBits::eTransferDst |
                     vk::ImageUsageFlagBits::eSampled,
            .history = false,
            .history_clear_color = {0.0, 0.0, 0.0, 0.0},
            .samples = 1,
            .mip_levels = ImageMipLevelCount{},
            .array_layers = 1,
            .dimension = ImageResourceDimension::two_d,
            .storage_mode = RenderTargetStorageMode::materialized,
            .alias_group = std::nullopt,
        },
        extent, vma::MemoryUsage::eAutoPreferDevice);
}
#endif

} // namespace

std::string renderPipelineFallbackConfigJson(
    std::string_view default_pass_name) {
    const nlohmann::json pass{
        {"name", std::string{renderPipelineFallbackPassName}},
        {"type", "fullscreen"},
        {"input", nlohmann::json::array()},
        {"output",
         {{"color", nlohmann::json::array({"display"})},
          {"depth", nullptr}}},
        {"shader",
         {{"vertex", "engine://fullscreen"},
          {"fragment", "engine://fallback_clear"}}},
    };
    const nlohmann::json config{
        {"schema", "pelican.graph"},
        {"version", 1},
        {"definitions", nlohmann::json::object()},
        {"nodes", nlohmann::json::array()},
        {"outputs", nlohmann::json::object()},
        {"rendering",
         {{"rendering_passes",
           nlohmann::json::array(
               {{{"name", std::string{default_pass_name}},
                 {"passes", nlohmann::json::array({pass})}}})}}},
    };
    return config.dump(2);
}

nlohmann::json renderPipelineRefusalJson(
    const std::exception &error, std::string_view root_json,
    std::string_view root_reference) {
    // BS-1b: a root that is not JSON is refused by that name, whatever
    // reader met it first.
    if (!root_json.empty())
        if (auto fields = renderDocumentSyntax(root_json, root_reference)) {
            (*fields)["code"] = std::string{renderDocumentSyntaxCode};
            (*fields)["message"] = renderDocumentSyntaxMessage(*fields);
            return std::move(*fields);
        }
    const auto root =
        nlohmann::json::parse(root_json, nullptr, false);
    nlohmann::json refusal = nlohmann::json::object();
    std::string code;
    std::string message = error.what();
    if (const auto *graph =
            dynamic_cast<const GraphRegistryError *>(&error)) {
        // CT-2: named after the root unless the refusal already is.
        const auto named = graphRefusalInDocument(
            *graph, root, root_reference);
        if (named.detail().is_object()) refusal = named.detail();
        code = named.code();
        message = named.what();
    } else if (auto coded = splitCodedRefusalMessage(error.what())) {
        if (coded->fields.is_object()) refusal = coded->fields;
        code = coded->code;
        addGraphRefusalSource(refusal, code, root, root_reference);
        // BS-1b: a feature or preset that is not JSON, said in words.
        if (code == renderDocumentSyntaxCode)
            message = renderDocumentSyntaxMessage(refusal);
    } else {
        code = std::string{renderPipelineUnnamedRefusalCode};
    }
    refusal["code"] = std::move(code);
    refusal["message"] = std::move(message);
    return refusal;
}

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

    // G-4: live materials are judged by the routes this config's
    // material_route_default gives them, for the whole compile, validation
    // and planning of the candidate. The engine itself keeps its start-time
    // snapshot until it restarts.
    std::optional<MaterialContainer::CandidateRouteDefaultScope>
        candidate_route_default;
    if (auto *materials =
            FastModuleContainer::tryGet<MaterialContainer>()) {
        const auto parsed = nlohmann::json::parse(
            rendering_config_json, nullptr, false);
        if (!parsed.is_discarded()) {
            candidate_route_default.emplace(
                *materials,
                readMaterialRouteDefault(
                    renderingSectionOrEmpty(parsed)));
        }
    }

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
    flat_options.after_publish_prepared_generation =
        std::move(hooks.after_publish);
    flat_options.after_rollback_prepared_generation =
        std::move(hooks.after_rollback);
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
            renderPipelineVariantGpuOwnerScope(
                xr_active,
                RenderPipelineGraphVariant::flat);
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
            renderPipelineVariantGpuOwnerScope(
                xr_active,
                RenderPipelineGraphVariant::xr);
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
        // PV-1: normal play goes on without the one-shot preview; one line
        // says why render_preview will refuse.
        if (!family.preview.available) {
            const auto &reason = family.preview.unavailable_reason;
            LOG_WARNING(
                logger,
                "render preview unavailable ({} feature={}{}); normal play "
                "continues, render_preview refuses: {}",
                reason.value("code", std::string{}),
                reason.value("feature", std::string{}),
                reason.contains("target")
                    ? " target=" + reason.value("target", std::string{})
                    : std::string{},
                reason.value("message", std::string{}));
        }
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
    } catch (const GraphRegistryError &error) {
        // CT-2: a graph refusal names the document its node was written in
        // (the root this config was read from), so the startup fatal line,
        // a ticket's rejected error and a reload's message all carry it.
        const auto named = graphRefusalInDocument(
            error,
            nlohmann::json::parse(rendering_config_json, nullptr, false),
            config.renderingConfigReference());
        LOG_ERROR(
            logger,
            "Failed to load main rendering configuration: {}",
            named.what());
        throw named;
    } catch (const std::exception &error) {
        // CT-2: the graph refusals that travel as "<code>: <JSON>" (the
        // view_select and draw-loop codes) take the same fields. Only a plain
        // runtime_error is rebuilt, so no derived type is lost.
        if (typeid(error) == typeid(std::runtime_error)) {
            if (auto refusal = splitCodedRefusalMessage(error.what());
                refusal && refusal->code.starts_with("graph.")) {
                const auto before = refusal->fields;
                addGraphRefusalSource(
                    refusal->fields, refusal->code,
                    nlohmann::json::parse(rendering_config_json, nullptr,
                                          false),
                    config.renderingConfigReference());
                if (refusal->fields != before) {
                    const std::runtime_error named{
                        refusal->code + ": " + refusal->fields.dump()};
                    LOG_ERROR(
                        logger,
                        "Failed to load main rendering configuration: {}",
                        named.what());
                    throw named;
                }
            }
        }
        LOG_ERROR(
            logger,
            "Failed to load main rendering configuration: {}",
            error.what());
        throw;
    }
}

} // namespace Pelican
