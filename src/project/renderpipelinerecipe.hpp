#pragma once

#include "renderpipeline.hpp"

namespace Pelican {
// Expands only the recipe boundary. Ownership stays outside the compiler JSON.
RenderPipelinePresetResolution expandRenderPipelineRecipe(
    const nlohmann::json &config, const nlohmann::json &overrides,
    const RenderPipelinePresetLoader &load_json);
}
