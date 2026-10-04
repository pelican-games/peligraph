#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_tracing : require
// The shadow ray's miss: nothing between the point and the light.
#include "__pelican_ray_group_layout.glsl"
layout(location = 1) rayPayloadInEXT float occluded;
void main() { occluded = 0.0; }
