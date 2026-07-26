#ifndef PELICAN_LIGHTING_V1_GLSL
#define PELICAN_LIGHTING_V1_GLSL

#include "pelican_frame.glsl"
#include "pelican_surface_v1.glsl"

#if defined(PELICAN_FEATURE_SHADOW) && defined(PELICAN_PASS_FORWARD)
#ifndef PELICAN_DIRECTIONAL_SHADOW_BINDING
#error "directional shadow requires PELICAN_DIRECTIONAL_SHADOW_BINDING"
#endif
layout(set = PELICAN_SET_PASS_INPUT,
       binding = PELICAN_DIRECTIONAL_SHADOW_BINDING)
    uniform sampler2D pelican_directional_shadow_texture;
#endif

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

float pelican_shadow(uint light_index, vec3 world_position) {
#if defined(PELICAN_FEATURE_SHADOW) && defined(PELICAN_PASS_FORWARD)
    if (light_index != 0u ||
        pelicanLights.directionalLightCount == 0u) {
        return 1.0;
    }

    vec4 shadow_clip =
        pelicanLights.shadowViewProjection *
        vec4(world_position, 1.0);
    if (shadow_clip.w <= 0.0) {
        return 1.0;
    }
    vec3 shadow_ndc =
        shadow_clip.xyz / shadow_clip.w;
    vec2 shadow_uv =
        shadow_ndc.xy * 0.5 + 0.5;
    if (shadow_uv.x < 0.0 || shadow_uv.x > 1.0 ||
        shadow_uv.y < 0.0 || shadow_uv.y > 1.0 ||
        shadow_ndc.z < 0.0 || shadow_ndc.z > 1.0) {
        return 1.0;
    }

    float stored_depth =
        texture(
            pelican_directional_shadow_texture,
            shadow_uv)
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
    float sky = clamp(normal.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(0.015), vec3(0.06, 0.07, 0.09), sky);
}

#endif
