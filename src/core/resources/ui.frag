#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D uiTexture;

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uiTexture, inUV) * inColor;
}
