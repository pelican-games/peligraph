#pragma once

#include "../renderingpass/renderingpass.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Pelican {

RenderingPassId loadDefaultRenderingPassFromConfig();

struct RenderGraphVariantConfig {
    RenderingPassId flat;
    std::optional<RenderingPassId> xr;
    std::vector<std::string> xr_excluded_features;
};

RenderGraphVariantConfig loadRenderGraphVariantsFromConfig();

} // namespace Pelican
