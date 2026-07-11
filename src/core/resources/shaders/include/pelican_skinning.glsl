#ifndef PELICAN_SKINNING_GLSL
#define PELICAN_SKINNING_GLSL

#include "pelican_sets.glsl"

#define PELICAN_MAX_SKIN_JOINTS 128

layout(set = PELICAN_SET_FREE, binding = PELICAN_SKIN_PALETTE_BINDING, std430) readonly buffer PelicanSkinPalette {
    mat4 matrices[];
} pelicanSkinPalette;

mat4 pelican_skin_matrix(ivec4 joints, vec4 weights) {
    uint base = uint(gl_BaseInstance) * PELICAN_MAX_SKIN_JOINTS;
    return weights.x * pelicanSkinPalette.matrices[base + uint(joints.x)] +
           weights.y * pelicanSkinPalette.matrices[base + uint(joints.y)] +
           weights.z * pelicanSkinPalette.matrices[base + uint(joints.z)] +
           weights.w * pelicanSkinPalette.matrices[base + uint(joints.w)];
}

#endif
