#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D inputTexture;

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 color = texture(inputTexture, inTexCoord).rgb;
    float luminance = max(color.r, max(color.g, color.b));
    float mask = smoothstep(0.34, 0.72, luminance);
    outColor = vec4(color * mask, 1.0);
}
