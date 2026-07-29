#pragma once

#include "../../renderer/viewfamily.hpp"

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
    // Moves the retained half-space boundary onto the Vulkan zero-to-one
    // near plane when that plane is in front of the reflected camera.
    bool oblique_near_plane = true;
};

// Builds a forward-Z, zero-to-one projection whose near clip boundary is the
// supplied world-space half-space. nullopt means that the retained plane is
// not in front of the camera, recedes through the view volume, or does not
// intersect the projection's far face; the caller can retain fragment/CPU
// clipping with the original projection. Invalid or singular matrices are
// rejected.
std::optional<glm::mat4> tryBuildObliqueNearPlaneProjectionZO(
    const glm::mat4 &projection,
    const glm::mat4 &view,
    const RenderViewClipPlane &clip_plane);

// Reflects every main-family view across one world-space plane. Stable source
// view identities produce stable reflection identities, including XR eyes.
RenderViewFamily buildPlanarReflectionViewFamily(
    const RenderViewFamily &main_family,
    const PlanarReflectionViewSettings
        &settings);

} // namespace Pelican
