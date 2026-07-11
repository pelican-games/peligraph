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
    uint customValues[64];
};

layout(set = PELICAN_SET_MATERIAL, binding = PELICAN_MATERIAL_BUFFER_BINDING, std430) readonly buffer PelicanMaterialBuffer {
    PelicanMaterialData materials[];
} pelicanMaterials;

// C-layer accessors for offsets emitted by dump-lowered-material. The payload
// is byte-addressed in the public contract even though its storage uses words.
float pelican_material_float(uint materialIndex, uint byteOffset) {
    return uintBitsToFloat(pelicanMaterials.materials[materialIndex].customValues[byteOffset / 4u]);
}

int pelican_material_int(uint materialIndex, uint byteOffset) {
    return int(pelicanMaterials.materials[materialIndex].customValues[byteOffset / 4u]);
}

vec2 pelican_material_vec2(uint materialIndex, uint byteOffset) {
    return vec2(pelican_material_float(materialIndex, byteOffset),
                pelican_material_float(materialIndex, byteOffset + 4u));
}

vec3 pelican_material_vec3(uint materialIndex, uint byteOffset) {
    return vec3(pelican_material_float(materialIndex, byteOffset),
                pelican_material_float(materialIndex, byteOffset + 4u),
                pelican_material_float(materialIndex, byteOffset + 8u));
}

vec4 pelican_material_vec4(uint materialIndex, uint byteOffset) {
    return vec4(pelican_material_float(materialIndex, byteOffset),
                pelican_material_float(materialIndex, byteOffset + 4u),
                pelican_material_float(materialIndex, byteOffset + 8u),
                pelican_material_float(materialIndex, byteOffset + 12u));
}

layout(push_constant) uniform PelicanMaterialPushConstants {
    layout(offset = 0) mat4 engineMvp;
    layout(offset = PELICAN_PUSH_ENGINE_BYTES) uint materialIndex;
} pelicanPush;

#endif
