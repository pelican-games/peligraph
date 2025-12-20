#version 460

layout(binding = 0) uniform sampler2D inputTexture0;
layout(binding = 1) uniform sampler2D inputTexture1;

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

// A value of 1.0 is a neutral additive blend.
// Higher values make the bloom effect stronger.
const float bloomIntensity = 2.0; 

void main() {
    vec4 color0 = texture(inputTexture0, inTexCoord);
    vec4 color1 = texture(inputTexture1, inTexCoord);

    // Additive blend
    outColor = color0 + color1 * bloomIntensity;
}