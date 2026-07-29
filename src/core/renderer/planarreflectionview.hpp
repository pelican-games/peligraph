#pragma once

#include "viewfamily.hpp"

namespace Pelican {

inline constexpr std::string_view
    planarReflectionRenderFeatureName =
        "planar_reflection";

struct PlanarReflectionViewSettings {
    RenderViewClipPlane clip_plane;
    // Reflection changes handedness. Pre-multiplying projection by an X
    // reflection preserves raster winding; projective sampling must use the
    // returned projection unchanged.
    bool preserve_raster_winding = true;
};

// Reflects every main-family view across one world-space plane. Stable source
// view identities produce stable reflection identities, including XR eyes.
RenderViewFamily buildPlanarReflectionViewFamily(
    const RenderViewFamily &main_family,
    const PlanarReflectionViewSettings
        &settings);

} // namespace Pelican
