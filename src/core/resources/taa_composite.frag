#version 450
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"

#ifndef PELICAN_FEATURE_TAA
#error PELICAN_FEATURE_TAA must be defined for the TAA composite shader
#endif

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D taaAccumSampler;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(taaAccumSampler, inUV);
}
