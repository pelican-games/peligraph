#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_frame.glsl"

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = inTexCoord;
    float wave = 0.5 + 0.5 * sin((uv.x + uv.y) * 8.0);
    outColor = vec4(uv.x, uv.y * 0.82 + wave * 0.18, 0.58 + 0.22 * sin(uv.x * 4.0), 1.0);
}
