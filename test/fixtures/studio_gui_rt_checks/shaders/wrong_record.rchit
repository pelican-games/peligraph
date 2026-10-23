#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_tracing : require
// A closest hit of the shadow ray whose shader record is not the ray
// layout's: one uint, where the layout has the engine's two fields and
// value. The engine refuses it in a traced cell
// (pelican.ray_tracing.shader_record_abi_mismatch@1).
#include "__pelican_ray_group_layout.glsl"
layout(location = 1) rayPayloadInEXT float occluded;
layout(shaderRecordEXT, std430) buffer WrongRecord {
    uint material_index;
} wrong;
void main() { occluded = 1.0 + 0.0 * float(wrong.material_index); }
