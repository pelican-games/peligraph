#pragma once

#include "../light/light.hpp"

#include <string_view>

namespace Pelican {

struct CompiledRenderPipeline;

inline constexpr std::string_view
    skyAmbientRenderFeatureName =
        "sky_ambient";

SkyAmbientLighting resolveSkyAmbientLighting(
    const CompiledRenderPipeline
        &pipeline);

} // namespace Pelican
