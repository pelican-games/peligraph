#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_frame.glsl"

layout(location = 0) in vec2 inPositionPx;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outColor;

void main() {
    vec2 clipPos = vec2(inPositionPx.x * pelicanFrame.resolution.z * 2.0 - 1.0,
                        inPositionPx.y * pelicanFrame.resolution.w * 2.0 - 1.0);
    gl_Position = vec4(clipPos, 0.0, 1.0);
    outUV = inUV;
    outColor = inColor;
}
