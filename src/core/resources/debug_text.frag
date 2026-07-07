#version 450
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"

layout(set = PELICAN_SET_FREE, binding = 1) uniform sampler2D debugTextAtlas;

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
    float alpha = texture(debugTextAtlas, inUV).a;
    outColor = vec4(inColor.rgb, inColor.a * alpha);
}
