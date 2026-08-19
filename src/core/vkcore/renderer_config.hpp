#pragma once

#include "../renderingpass/renderingpass.hpp"
#include "../renderingpass/previewgraph.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct RendererRuntimeGeneration;

// The complete v1 list whose enablement depends on modules created before the
// runtime module graph is frozen.  WP331 exposes these entries in the engine
// catalog but rejects hot-add by name instead of hiding them in Studio.
std::span<const std::string_view>
renderFeaturesRequiringRuntimeModules() noexcept;
bool renderFeatureRequiresRuntimeModule(std::string_view name) noexcept;

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
