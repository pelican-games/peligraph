#ifndef PELICAN_SHADOW_CASCADE_GLSL
#define PELICAN_SHADOW_CASCADE_GLSL

#include "pelican_frame.glsl"

const uint PELICAN_MAX_DIRECTIONAL_SHADOW_CASCADES = 8u;

float pelican_directional_shadow_cascade_far(
    uint cascade_index) {
    return pelicanLights
        .directionalShadowCascadeSplits[
            cascade_index / 4u][
            cascade_index % 4u];
}

float pelican_directional_shadow_view_depth(
    vec3 world_position) {
    return max(
        -(pelicanFrame.view *
          vec4(world_position, 1.0)).z,
        0.0);
}

// Returns directionalShadowCascadeCount when the point lies beyond the
// configured shadow distance. Callers treat that sentinel as fully lit.
uint pelican_directional_shadow_cascade(
    vec3 world_position) {
    uint cascade_count =
        clamp(
            pelicanLights
                .directionalShadowCascadeCount,
            1u,
            PELICAN_MAX_DIRECTIONAL_SHADOW_CASCADES);
    float view_depth =
        pelican_directional_shadow_view_depth(
            world_position);
    for (uint cascade = 0u;
         cascade < cascade_count;
         ++cascade) {
        if (view_depth <=
            pelican_directional_shadow_cascade_far(
                cascade)) {
            return cascade;
        }
    }
    return cascade_count;
}

#endif
