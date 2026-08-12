#pragma once

#include "../renderingpass/renderingpass.hpp"
#include "../renderingpass/previewgraph.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct RendererRuntimeGeneration;

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
