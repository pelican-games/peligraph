#pragma once

#include "../renderingpass/renderingpass.hpp"
#include "../renderingpass/previewgraph.hpp"
#include "../../project/targetplanning.hpp"

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
    std::function<void(
        const RendererRuntimeGeneration &)>
        before_publish;
};

RenderGraphVariantConfig loadRenderGraphVariantsFromConfig();
RenderGraphVariantConfig loadRenderGraphVariantsFromConfigData(
    std::string_view rendering_config_json,
    RenderGraphVariantLoadHooks hooks = {});

// Explicit launch/tooling path. The baseline functions above remain solely
// project-authored and never inspect EngineLaunchConfig overlays.
RenderGraphVariantConfig
loadRenderGraphVariantsFromConfigWithStartupFeatureOverlays();
RenderGraphVariantConfig
loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
    std::string_view rendering_config_json,
    RenderGraphVariantLoadHooks hooks = {});

} // namespace Pelican
