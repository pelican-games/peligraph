#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_query : require

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

PELICAN_DECLARE_INPUT_0(worldPosSampler);

layout(set = PELICAN_SET_FRAME,
       binding = PELICAN_RAY_QUERY_TLAS_BINDING)
    uniform accelerationStructureEXT pelicanRayQueryScene;

const float RAY_ORIGIN_BIAS = 0.01;
const float RAY_MAX_DISTANCE = 10000.0;

void main() {
    vec4 world = PELICAN_TEXTURE_2D_0(worldPosSampler, inUV);
    if (world.a < 0.5 || pelicanLights.directionalLightCount == 0u) {
        outColor = vec4(1.0);
        return;
    }

    vec3 toLight = normalize(
        -pelicanLights.directionalLights[0].direction);
    vec3 origin = world.xyz + toLight * RAY_ORIGIN_BIAS;
    rayQueryEXT query;
    rayQueryInitializeEXT(
        query, pelicanRayQueryScene,
        gl_RayFlagsTerminateOnFirstHitEXT |
            gl_RayFlagsOpaqueEXT,
        0xff, origin, 0.0, toLight, RAY_MAX_DISTANCE);
    while (rayQueryProceedEXT(query)) {
    }

    bool occluded = rayQueryGetIntersectionTypeEXT(
                        query, true) !=
                    gl_RayQueryCommittedIntersectionNoneEXT;
    float visibility = occluded ? 0.0 : 1.0;
    outColor = vec4(visibility, visibility, visibility, 1.0);
}
