#pragma once

#include "../renderingpass/renderingpass.hpp"
#include "../renderingpass/previewgraph.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

RenderingPassId loadDefaultRenderingPassFromConfig();

struct RenderGraphVariantConfig {
    RenderingPassId flat;
    std::optional<RenderingPassId> xr;
    std::vector<std::string> xr_excluded_features;
    PreviewGraphProgram preview;
};

RenderGraphVariantConfig loadRenderGraphVariantsFromConfig();
RenderGraphVariantConfig loadRenderGraphVariantsFromConfigData(
    std::string_view rendering_config_json);

} // namespace Pelican
