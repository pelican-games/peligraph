#version 450
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D spriteAtlas;
layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(spriteAtlas, fragUv) * fragColor;
}
