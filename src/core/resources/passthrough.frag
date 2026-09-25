#version 460
#extension GL_GOOGLE_include_directive : enable

// VI-1: engine://passthrough. Every input is variadic (first = 0): the pass
// may give it any number of 2D render targets, and it writes input
// PELICAN_PASSTHROUGH_INPUT (default 0; a pass sets it through
// shader.defines) unchanged. With no inputs at all it draws the UV gradient
// the blank template's present shader draws, so a pass that runs is visible.
// An input number past the ones the pass supplies reads the engine's default
// (black).

#include "pelican_sets.glsl"
#include "pelican_view.glsl"

#ifndef PELICAN_PASSTHROUGH_INPUT
#define PELICAN_PASSTHROUGH_INPUT 0
#endif

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

PELICAN_DECLARE_VARIADIC_INPUTS(0, 8);

void main() {
    if (PELICAN_VARIADIC_INPUT_COUNT == 0) {
        outColor = vec4(inUV.x, inUV.y, 0.5, 1.0);
        return;
    }
    outColor = PELICAN_VARIADIC_TEXTURE(PELICAN_PASSTHROUGH_INPUT, inUV);
}
