#version 450

#ifndef PELICAN_FEATURE_HDR
#error PELICAN_FEATURE_HDR must be defined for the HDR tonemap shader
#endif

layout(set = 1, binding = 0) uniform sampler2D inputTexture;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

vec3 reinhardTonemap(vec3 color) {
    color = max(color, vec3(0.0));
    return color / (color + vec3(1.0));
}

void main() {
    vec3 hdrColor = texture(inputTexture, inUV).rgb;
    outColor = vec4(reinhardTonemap(hdrColor), 1.0);
}
