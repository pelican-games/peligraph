#version 460

layout(binding = 0) uniform sampler2D inputTexture;

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

const float threshold = 0.4; // 輝度閾値
const float knee = 0.7;      // 膝領域の強度 (より滑らかなロールオフ用)

void main() {
    vec4 color = texture(inputTexture, inTexCoord);
    
    // RGB to Luminance (ITU-R BT.709)
    float luminance = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));

    // 高輝度部分を抽出 (閾値処理)
    float softThreshold = luminance - threshold;
    float bright = softThreshold * (2.0 * knee);
    bright = clamp(bright, 0.0, 1.0);
    bright = softThreshold - bright * bright / (4.0 * knee);
    bright = max(bright, 0.0); // Ensure no negative values

    outColor = color * bright;
}
