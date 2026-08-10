#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_query : require

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

PELICAN_DECLARE_INPUT_0(worldPosSampler);
PELICAN_DECLARE_INPUT_1(normalSampler);

layout(set = PELICAN_SET_FRAME,
       binding = PELICAN_RAY_QUERY_TLAS_BINDING)
    uniform accelerationStructureEXT pelicanRayQueryScene;

const float RAY_ORIGIN_ABSOLUTE_BIAS = 0.01;
// R16F world positions have roughly one ULP per 1024 units of magnitude.
// Two ULPs cover interpolation/round-trip error before the normal offset.
const float RAY_ORIGIN_RELATIVE_BIAS = 1.0 / 512.0;
const float RAY_MAX_DISTANCE = 10000.0;

void main() {
    vec4 world = PELICAN_TEXTURE_2D_0(worldPosSampler, inUV);
    if (world.a < 0.5 || pelicanLights.directionalLightCount == 0u) {
        outColor = vec4(1.0);
        return;
    }

    vec3 toLight = normalize(
        -pelicanLights.directionalLights[0].direction);
    vec3 normal = normalize(
        PELICAN_TEXTURE_2D_1(normalSampler, inUV).xyz * 2.0 - 1.0);
    float coordinateScale = max(
        1.0, max(abs(world.x), max(abs(world.y), abs(world.z))));
    float originBias = max(
        RAY_ORIGIN_ABSOLUTE_BIAS,
        coordinateScale * RAY_ORIGIN_RELATIVE_BIAS);
    float normalSide = dot(normal, toLight) >= 0.0 ? 1.0 : -1.0;
    vec3 origin = world.xyz + normal * normalSide * originBias;
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
