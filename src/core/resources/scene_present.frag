#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform PELICAN_SAMPLER_2D_0 sceneColorSampler;

void main() {
    vec4 scene = PELICAN_TEXTURE_2D_0(sceneColorSampler, inUV);
#ifndef PELICAN_FEATURE_HDR
    // Hybrid rendering keeps deferred and forward contributions scene-linear
    // until this single convergence point.
    scene.rgb = scene.rgb / (scene.rgb + vec3(0.155)) * 1.019;
#endif
    outColor = scene;
}
