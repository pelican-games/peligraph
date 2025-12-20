#version 460

layout(binding = 0) uniform sampler2D inputTexture;

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

const float weight[5] = float[] (0.227027, 0.1945946, 0.1216216, 0.05405405, 0.01621621);

void main() {
    vec2 texelSize = 1.0 / textureSize(inputTexture, 0);
    vec3 result = texture(inputTexture, inTexCoord).rgb * weight[0];

    for (int i = 1; i < 5; ++i) {
        result += texture(inputTexture, inTexCoord + vec2(texelSize.x * float(i), 0.0)).rgb * weight[i];
        result += texture(inputTexture, inTexCoord - vec2(texelSize.x * float(i), 0.0)).rgb * weight[i];
    }

    outColor = vec4(result, 1.0);
}