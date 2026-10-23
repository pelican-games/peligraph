#pragma once

#include "../renderingpass/renderingpass.hpp"
#include "../renderingpass/previewgraph.hpp"
#include "../../project/targetplanning.hpp"

#include <exception>
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct RendererRuntimeGeneration;

struct RenderRuntimeModuleRequirement {
    std::string_view module;
    bool (*initialized)() noexcept = nullptr;
};

struct RenderFeatureRuntimeModuleRequirement {
    std::string_view feature;
    std::span<const RenderRuntimeModuleRequirement> modules;
};

struct RenderFeatureRuntimeAvailabilityEnvironment {
    bool runtime_shader_compiler_enabled = false;
    TargetEndpoint target_endpoint;
    bool runtime_module_creation_frozen = false;
    std::function<bool(std::string_view)> runtime_module_initialized;
};

struct RenderFeatureRuntimeAvailability {
    bool available = false;
    std::string unavailable_reason;

    bool operator==(
        const RenderFeatureRuntimeAvailability &) const = default;
};

// The complete v1 list whose enablement depends on modules created before the
// runtime module graph is frozen.  WP331 exposes these entries in the engine
// catalog but rejects hot-add by name instead of hiding them in Studio.
std::span<const std::string_view>
renderFeaturesRequiringRuntimeModules() noexcept;
std::span<const RenderFeatureRuntimeModuleRequirement>
renderFeatureRuntimeModuleRequirements() noexcept;
bool renderFeatureRequiresRuntimeModule(std::string_view name) noexcept;

// PB-1 (engine_backlog 22): a rendering configuration that enables a feature
// whose runtime module this engine was started without, after the module
// graph was frozen. Only a restart creates the module:
//   render_feature_requires_restart@1 {feature?, module}
// `module` is the module's name ("UiRenderer"); `feature` the render feature
// that needs it ("ui"), left out when the module is not one a feature of
// renderFeatureRuntimeModuleRequirements names. It travels as "<code>:
// <fields JSON>" (splitCodedRefusalMessage), and a startup refusal, a hot
// reload, reload_render_documents and an edit ticket all carry the code.
inline constexpr std::string_view renderFeatureRequiresRestartCode =
    "render_feature_requires_restart@1";
std::string renderFeatureRequiresRestartMessage(
    std::optional<std::string_view> feature, std::string_view module);
// "render_feature_requires_restart@1: feature 'ui' needs runtime module
// UiRenderer, which this engine was started without; restart the engine to
// enable it".
std::string renderFeatureRequiresRestartText(const nlohmann::json &fields);
// The fields of a module container's "Module '<type>' was first requested
// after the runtime module graph was frozen" refusal; nullopt for any other
// message.
std::optional<nlohmann::json> frozenModuleRefusalFields(
    std::string_view message);

void requireRenderFeatureRuntimeAvailability(
    std::string_view feature_name,
    const nlohmann::json &feature_document,
    const RenderFeatureRuntimeAvailabilityEnvironment &environment);
RenderFeatureRuntimeAvailability evaluateRenderFeatureRuntimeAvailability(
    std::string_view feature_name,
    const nlohmann::json &feature_document,
    const RenderFeatureRuntimeAvailabilityEnvironment &environment) noexcept;
RenderFeatureRuntimeAvailability currentRenderFeatureRuntimeAvailability(
    std::string_view feature_name,
    const nlohmann::json &feature_document) noexcept;
void requireCurrentRenderFeatureRuntimeAvailability(
    std::string_view feature_name,
    const nlohmann::json &feature_document);

RenderingPassId loadDefaultRenderingPassFromConfig();

struct RenderGraphVariantConfig {
    RenderingPassId flat;
    std::optional<RenderingPassId> xr;
    std::vector<std::string> xr_excluded_features;
    PreviewGraphProgram preview;
};

struct RenderGraphVariantLoadHooks {
    bool validate_live_materials = true;
    std::function<std::string(std::string_view)> load_document;
    std::function<void(
        const RendererRuntimeGeneration &)>
        before_publish;
    std::function<void(
        const RendererRuntimeGeneration &)>
        after_publish;
    std::function<void()> after_rollback;
};

RenderGraphVariantConfig loadRenderGraphVariantsFromConfig();
RenderGraphVariantConfig loadRenderGraphVariantsFromConfigData(
    std::string_view rendering_config_json,
    RenderGraphVariantLoadHooks hooks = {});

// The owner identity used by the production variant-family registration.
// When OpenXR is active, flat and XR pipelines (including every pipeline's
// SBT) must retire and publish as one scope.
std::string renderPipelineVariantGpuOwnerScope(
    bool xr_active,
    RenderPipelineGraphVariant variant);

// Explicit launch/tooling path. The baseline functions above remain solely
// project-authored and never inspect EngineLaunchConfig overlays.
// BS-1: the engine's built-in fallback generation, the one a launch with
// --render-fallback starts on when the project's rendering configuration
// cannot be composed. One rendering pass named `default_pass_name` (the
// project's default_rendering_pass, so the renderer selects it as it would the
// project's) with one fullscreen pass that draws engine://fallback_clear's
// flat colour to `display`; the engine adds the output transform as it does to
// every graph. It reads no project document and draws no material.
inline constexpr std::string_view renderPipelineFallbackPassName =
    "render_pipeline_fallback";
std::string renderPipelineFallbackConfigJson(
    std::string_view default_pass_name);
// RTL-1b: whether a rendering configuration is the fallback generation's
// (renderPipelineFallbackConfigJson's one pass, with the launch's feature
// overlays or without). Its ray layout mode is not judged: it stands in for a
// project started with or without a ray group layout.
bool isRenderPipelineFallbackConfig(std::string_view rendering_config_json);

// BS-1: the code of a startup refusal that carries no name of its own (the
// one render_pipeline_preflight_failed a reload's ticket gets for it).
inline constexpr std::string_view renderPipelineUnnamedRefusalCode =
    "render_pipeline_preflight_failed";
// BS-1: a rendering configuration refusal as get_status.render_pipeline
// reports it, the shape a render edit's rejected `error` has:
//   {code, ...the refusal's fields, message}
// A GraphRegistryError keeps its code and detail; a "<code>@<n>: <JSON>"
// message (splitCodedRefusalMessage) its code and fields; anything else is
// render_pipeline_preflight_failed. A graph.* refusal names, as CT-2 does,
// the root document `root_json` was read from (`root_reference`).
// BS-1b: a `root_json` that is not JSON is render_document_syntax@1
// (renderDocumentSyntax) whatever the error was.
nlohmann::json renderPipelineRefusalJson(
    const std::exception &error, std::string_view root_json,
    std::string_view root_reference);

// Synchronous edit failures: validation refusals stay in the RPC answer;
// resource allocation/device failures also enter Problems. Startup/reload
// and staged rollback record directly, regardless of the refusal's type.
void recordRenderPipelineAuthoringFailure(
    const std::exception &error, std::string_view source_reference) noexcept;

RenderGraphVariantConfig
loadRenderGraphVariantsFromConfigWithStartupFeatureOverlays();
RenderGraphVariantConfig
loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
    std::string_view rendering_config_json,
    RenderGraphVariantLoadHooks hooks = {});

} // namespace Pelican
