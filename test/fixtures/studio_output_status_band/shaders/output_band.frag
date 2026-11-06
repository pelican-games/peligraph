#version 460
#extension GL_GOOGLE_include_directive : enable
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_VARIADIC_INPUTS(0, 8);

void main() {
    outColor = vec4(inUV.x, inUV.y, 0.25, 1.0);
}
