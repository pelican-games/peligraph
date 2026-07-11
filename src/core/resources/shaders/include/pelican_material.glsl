#ifndef PELICAN_MATERIAL_GLSL
#define PELICAN_MATERIAL_GLSL

#include "pelican_sets.glsl"

struct PelicanMaterialData {
    vec4 baseColorFactor;
    vec4 emissiveFactor;
    vec4 surfaceFactors;
    vec4 vatBoundsMinFrameCount;
    vec4 vatBoundsExtentFps;
    ivec4 vatFlags;
};

layout(set = PELICAN_SET_MATERIAL, binding = PELICAN_MATERIAL_BUFFER_BINDING, std430) readonly buffer PelicanMaterialBuffer {
    PelicanMaterialData materials[];
} pelicanMaterials;

layout(push_constant) uniform PelicanMaterialPushConstants {
    layout(offset = 0) mat4 engineMvp;
    layout(offset = PELICAN_PUSH_ENGINE_BYTES) uint materialIndex;
} pelicanPush;

#endif
