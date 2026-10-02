#version 460
#extension GL_GOOGLE_include_directive : enable

// PV-1 fixture: adds 1/16 to what `accum` held the frame before (its
// history, input 0), so every frame differs from the last until it reaches
// 1. With no input it writes a flat 0.25.

#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

PELICAN_DECLARE_VARIADIC_INPUTS(0, 1);

void main() {
    if (PELICAN_VARIADIC_INPUT_COUNT == 0) {
        outColor = vec4(0.25, 0.25, 0.25, 1.0);
        return;
    }
    vec4 previous = PELICAN_VARIADIC_TEXTURE(0, inUV);
    outColor = vec4(min(previous.rgb + vec3(1.0 / 16.0), vec3(1.0)), 1.0);
}
