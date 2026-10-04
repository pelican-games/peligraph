#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_tracing : require
#include "__pelican_ray_group_layout.glsl"
layout(location = 0) rayPayloadInEXT vec4 hit;
void main() { hit = vec4(0.0); }
