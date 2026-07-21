#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D sceneColorSampler;

void main() {
    vec4 scene = texture(sceneColorSampler, inUV);
#ifndef PELICAN_FEATURE_HDR
    // Hybrid rendering keeps deferred and forward contributions scene-linear
    // until this single convergence point.
    scene.rgb = scene.rgb / (scene.rgb + vec3(0.155)) * 1.019;
#endif
    outColor = scene;
}
