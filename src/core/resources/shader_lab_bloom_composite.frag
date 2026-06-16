#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D baseTexture;
layout(set = PELICAN_SET_PASS_INPUT, binding = 1) uniform sampler2D bloomTexture;

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 base = texture(baseTexture, inTexCoord).rgb;
    vec3 bloom = texture(bloomTexture, inTexCoord).rgb;
    vec3 glow = bloom * 1.15;
    vec3 color = vec3(1.0) - exp(-(base + glow * 1.55));
    outColor = vec4(pow(color, vec3(0.92)), 1.0);
}
