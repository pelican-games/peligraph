#version 460

layout(binding = 0) uniform sampler2D debugTexture;

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    // Output the red channel to all color channels to visualize a single-channel texture
    float value = texture(debugTexture, inTexCoord).r;
    outColor = vec4(value, value, value, 1.0);
}
