#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D inputTexture;

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(inputTexture, 0));
    vec2 direction = vec2(0.0, texelSize.y);
    vec2 uv = inTexCoord;
    vec3 color = texture(inputTexture, uv).rgb * 0.28;
    color += texture(inputTexture, uv + direction * 1.5).rgb * 0.22;
    color += texture(inputTexture, uv - direction * 1.5).rgb * 0.22;
    color += texture(inputTexture, uv + direction * 3.5).rgb * 0.14;
    color += texture(inputTexture, uv - direction * 3.5).rgb * 0.14;
    outColor = vec4(color, 1.0);
}
