#ifndef PELICAN_LIGHTING_V1_GLSL
#define PELICAN_LIGHTING_V1_GLSL

#include "pelican_frame.glsl"
#include "pelican_surface_v1.glsl"

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

// v1 exposes the stable call even before a shadow atlas descriptor becomes a
// public resource. The source backend returns fully lit visibility; the
// function ABI remains the upgrade point for the shadow implementation.
float pelican_shadow(uint light_index, vec3 world_position) {
    return 1.0;
}

vec3 pelican_env_ambient(vec3 normal) {
    float sky = clamp(normal.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(0.015), vec3(0.06, 0.07, 0.09), sky);
}

#endif
