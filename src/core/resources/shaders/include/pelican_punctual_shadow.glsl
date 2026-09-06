#ifndef PELICAN_PUNCTUAL_SHADOW_GLSL
#define PELICAN_PUNCTUAL_SHADOW_GLSL

// Resource-port functions are supplied by the graph, with no fixed descriptor ABI.

#ifdef PELICAN_FEATURE_POINT_SHADOW
float pelican_point_shadow_visibility(uint index, vec3 world_position) {
    uvec4 header = pelican_load_point_shadow_matrices(0u);
    uint available = pelican_count_point_shadow_matrices();
    if (header.x != 0x50564d31u || available < 1u ||
        header.y > (available - 1u) / 4u || index >= header.y / 6u) return 1.0;
    for (uint face = 0u; face < 6u; ++face) {
        uint layer = index * 6u + face;
        uint first = 1u + layer * 4u;
        mat4 matrix = mat4(
            uintBitsToFloat(pelican_load_point_shadow_matrices(first)),
            uintBitsToFloat(pelican_load_point_shadow_matrices(first + 1u)),
            uintBitsToFloat(pelican_load_point_shadow_matrices(first + 2u)),
            uintBitsToFloat(pelican_load_point_shadow_matrices(first + 3u)));
        vec4 clip = matrix * vec4(world_position, 1.0);
        if (!(clip.w > 0.0)) continue;
        vec3 ndc = clip.xyz / clip.w;
        if (!all(greaterThanEqual(ndc, vec3(-1.0, -1.0, 0.0))) ||
            !all(lessThanEqual(ndc, vec3(1.0)))) continue;
        float depth = pelican_sample_lod_point_shadow_texture(ndc.xy * 0.5 + 0.5, layer, 0.0).r;
        return ndc.z - 0.0015 <= depth ? 1.0 : 0.35;
    }
    return 1.0;
}
#endif

#ifdef PELICAN_FEATURE_SPOT_SHADOW
float pelican_spot_shadow_visibility(uint index, vec3 world_position) {
    uvec4 header = pelican_load_spot_shadow_matrices(0u);
    uint available = pelican_count_spot_shadow_matrices();
    if (header.x != 0x50564d31u || available < 1u ||
        header.y > (available - 1u) / 4u || index >= header.y / 1u) return 1.0;
    for (uint face = 0u; face < 1u; ++face) {
        uint layer = index * 1u + face;
        uint first = 1u + layer * 4u;
        mat4 matrix = mat4(
            uintBitsToFloat(pelican_load_spot_shadow_matrices(first)),
            uintBitsToFloat(pelican_load_spot_shadow_matrices(first + 1u)),
            uintBitsToFloat(pelican_load_spot_shadow_matrices(first + 2u)),
            uintBitsToFloat(pelican_load_spot_shadow_matrices(first + 3u)));
        vec4 clip = matrix * vec4(world_position, 1.0);
        if (!(clip.w > 0.0)) continue;
        vec3 ndc = clip.xyz / clip.w;
        if (!all(greaterThanEqual(ndc, vec3(-1.0, -1.0, 0.0))) ||
            !all(lessThanEqual(ndc, vec3(1.0)))) continue;
        float depth = pelican_sample_lod_spot_shadow_texture(ndc.xy * 0.5 + 0.5, layer, 0.0).r;
        return ndc.z - 0.0015 <= depth ? 1.0 : 0.35;
    }
    return 1.0;
}
#endif

float pelican_local_shadow_visibility(uint local_index, vec3 world_position) {
    uint index = pelican_light_inventory_index(local_index);
    uint directional_count = pelicanLights.directionalLightCount;
    uint point_count = pelicanLights.pointLightCount;
#ifdef PELICAN_FEATURE_CLUSTERED_LIGHTING
    if (pelican_light_selection_v2_valid()) {
        uvec4 counts = pelican_load_light_inventory(1u);
        directional_count = counts.x;
        point_count = counts.y;
    }
#endif
    if (index < directional_count) return 1.0;
    index -= directional_count;
    if (index < point_count) {
#ifdef PELICAN_FEATURE_POINT_SHADOW
        return pelican_point_shadow_visibility(index, world_position);
#else
        return 1.0;
#endif
    }
#ifdef PELICAN_FEATURE_SPOT_SHADOW
    return pelican_spot_shadow_visibility(index - point_count, world_position);
#else
    return 1.0;
#endif
}
#endif

