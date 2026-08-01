#ifndef PELICAN_LIGHTING_V1_GLSL
#define PELICAN_LIGHTING_V1_GLSL

#include "pelican_frame.glsl"
#include "pelican_surface_v1.glsl"

#if defined(PELICAN_FEATURE_SHADOW)
#include "pelican_shadow_cascade.glsl"
#endif

#if defined(PELICAN_FEATURE_SHADOW) && defined(PELICAN_PASS_FORWARD)
#ifndef PELICAN_DIRECTIONAL_SHADOW_BINDING
#error "directional shadow requires PELICAN_DIRECTIONAL_SHADOW_BINDING"
#endif
layout(set = PELICAN_SET_PASS_INPUT,
       binding = PELICAN_DIRECTIONAL_SHADOW_BINDING)
    uniform sampler2DArray pelican_directional_shadow_texture;
#endif

#if defined(PELICAN_FEATURE_CLUSTERED_LIGHTING)
#include "pelican_lighting_v2.glsl"
#else
uint pelican_directional_light_count() {
    return pelicanLights.directionalLightCount;
}

uint pelican_light_count() {
    return pelicanLights.directionalLightCount + pelicanLights.pointLightCount +
           pelicanLights.spotLightCount;
}

PelicanLightV1 pelican_light(uint index, vec3 world_position) {
    PelicanLightV1 light;
    light.direction = vec3(0.0, 1.0, 0.0);
    light.radiance = vec3(0.0);
    light.attenuation = 1.0;
    if (index < pelicanLights.directionalLightCount) {
        PelicanDirectionalLight source = pelicanLights.directionalLights[index];
        light.direction = normalize(-source.direction);
        light.radiance = source.color * source.intensity;
        return light;
    }
    index -= pelicanLights.directionalLightCount;
    if (index < pelicanLights.pointLightCount) {
        PelicanPointLight source = pelicanLights.pointLights[index];
        vec3 delta = source.position - world_position;
        float distance_squared = max(dot(delta, delta), 0.0001);
        light.direction = normalize(delta);
        light.attenuation = 1.0 / distance_squared;
        light.radiance = source.color * source.intensity;
        return light;
    }
    index -= pelicanLights.pointLightCount;
    if (index < pelicanLights.spotLightCount) {
        PelicanSpotLight source = pelicanLights.spotLights[index];
        vec3 delta = source.position - world_position;
        float distance_squared = max(dot(delta, delta), 0.0001);
        light.direction = normalize(delta);
        float cone = dot(normalize(source.direction), -light.direction);
        float cone_weight = smoothstep(source.outerConeAngle, source.innerConeAngle, cone);
        light.attenuation = cone_weight / distance_squared;
        light.radiance = source.color * source.intensity;
    }
    return light;
}

bool pelican_light_selection_overflowed() {
    return false;
}

uint pelican_light_inventory_index(uint local_index) {
    return local_index;
}
#endif

#if defined(PELICAN_FEATURE_SHADOW)
layout(set = PELICAN_SET_FRAME,
       binding = PELICAN_DIRECTIONAL_SHADOW_DATA_BINDING,
       std430) readonly buffer PelicanDirectionalShadowDataV1 {
    uvec4 elements[];
} pelicanDirectionalShadowData;

const uint PELICAN_DIRECTIONAL_SHADOW_DATA_V1_MAGIC = 0x50534831u;
const uint PELICAN_DIRECTIONAL_SHADOW_DATA_V1_VERSION = 1u;
const uint PELICAN_DIRECTIONAL_SHADOW_MATRIX_ELEMENTS = 4u;

bool pelican_directional_shadow_record(
    uint light_index,
    out uvec4 record) {
    record = uvec4(0u);
    uint element_count =
        uint(pelicanDirectionalShadowData.elements.length());
    if (element_count == 0u) {
        return false;
    }
    uvec4 header =
        pelicanDirectionalShadowData.elements[0];
    if (header.x != PELICAN_DIRECTIONAL_SHADOW_DATA_V1_MAGIC ||
        header.y != PELICAN_DIRECTIONAL_SHADOW_DATA_V1_VERSION ||
        header.w == 0u ||
        header.z > element_count - 1u) {
        return false;
    }

    uint inventory_index =
        pelican_light_inventory_index(light_index);
    uint lower = 0u;
    uint upper = header.z;
    while (lower < upper) {
        uint middle = lower + (upper - lower) / 2u;
        uvec4 candidate =
            pelicanDirectionalShadowData.elements[1u + middle];
        if (candidate.x < inventory_index) {
            lower = middle + 1u;
        } else {
            upper = middle;
        }
    }
    if (lower >= header.z) {
        return false;
    }
    record =
        pelicanDirectionalShadowData.elements[1u + lower];
    if (record.x != inventory_index ||
        record.w != header.w ||
        record.y > element_count) {
        return false;
    }
    uint remaining = element_count - record.y;
    return record.w <=
        remaining / PELICAN_DIRECTIONAL_SHADOW_MATRIX_ELEMENTS;
}

mat4 pelican_directional_shadow_matrix(
    uvec4 record,
    uint cascade) {
    uint first =
        record.y +
        cascade * PELICAN_DIRECTIONAL_SHADOW_MATRIX_ELEMENTS;
    return mat4(
        uintBitsToFloat(
            pelicanDirectionalShadowData.elements[first + 0u]),
        uintBitsToFloat(
            pelicanDirectionalShadowData.elements[first + 1u]),
        uintBitsToFloat(
            pelicanDirectionalShadowData.elements[first + 2u]),
        uintBitsToFloat(
            pelicanDirectionalShadowData.elements[first + 3u]));
}

bool pelican_directional_shadow_projection(
    uint light_index,
    vec3 world_position,
    out vec3 shadow_ndc,
    out vec2 shadow_uv,
    out uint shadow_layer) {
    uvec4 record;
    if (!pelican_directional_shadow_record(
            light_index, record)) {
        return false;
    }
    uint cascade =
        pelican_directional_shadow_cascade(
            world_position);
    if (cascade >= record.w ||
        record.z > 0xffffffffu - cascade) {
        return false;
    }
    vec4 shadow_clip =
        pelican_directional_shadow_matrix(
            record, cascade) *
        vec4(world_position, 1.0);
    if (shadow_clip.w <= 0.0) {
        return false;
    }
    shadow_ndc = shadow_clip.xyz / shadow_clip.w;
    shadow_uv = shadow_ndc.xy * 0.5 + 0.5;
    if (shadow_uv.x < 0.0 || shadow_uv.x > 1.0 ||
        shadow_uv.y < 0.0 || shadow_uv.y > 1.0 ||
        shadow_ndc.z < 0.0 || shadow_ndc.z > 1.0) {
        return false;
    }
    shadow_layer = record.z + cascade;
    return true;
}
#endif

float pelican_shadow(uint light_index, vec3 world_position) {
#if defined(PELICAN_FEATURE_SHADOW) && defined(PELICAN_PASS_FORWARD)
    vec3 shadow_ndc;
    vec2 shadow_uv;
    uint shadow_layer;
    if (!pelican_directional_shadow_projection(
            light_index, world_position,
            shadow_ndc, shadow_uv,
            shadow_layer)) {
        return 1.0;
    }

    float stored_depth =
        texture(
            pelican_directional_shadow_texture,
            vec3(
                shadow_uv,
                float(shadow_layer)))
            .r;
    const float bias = 0.0015;
    return shadow_ndc.z - bias <= stored_depth
               ? 1.0
               : 0.35;
#else
    return 1.0;
#endif
}

vec3 pelican_env_ambient(vec3 normal) {
#ifdef PELICAN_FEATURE_SKY_AMBIENT
    return pelicanLights.environmentAmbientRadiance.rgb;
#else
    return vec3(0.0);
#endif
}

#endif
