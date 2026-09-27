#version 460
#extension GL_GOOGLE_include_directive : enable

// RU-1: the Pass Editor's visible edit. present draws scene_color inverted
// with this fragment, so the box and the background both change colour; Undo
// puts engine://passthrough back and the picture with it.

#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

PELICAN_DECLARE_INPUT_0(sceneColorSampler);

void main() {
    vec4 scene = PELICAN_TEXTURE_2D_0(sceneColorSampler, inUV);
    outColor = vec4(vec3(1.0) - clamp(scene.rgb, 0.0, 1.0), 1.0);
}
