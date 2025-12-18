#version 460

layout(set = 0, binding = 0) uniform sampler2D uiTexture;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uiTexture, inUV);
}
