#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_INPUT_0(radianceAccum);

void main() {
    vec4 accumulated = PELICAN_TEXTURE_2D_0(radianceAccum, inUV);
    outColor = vec4(accumulated.rgb / max(accumulated.a, 1.0), 1.0);
}
