#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_view.glsl"
layout(location=0) in vec2 inUV;
layout(location=0) out vec4 outColor;
PELICAN_DECLARE_INPUT_0(rayColor);
void main() {
    outColor = PELICAN_TEXTURE_2D_0(rayColor, inUV);
}
