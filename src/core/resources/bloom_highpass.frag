#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"

// RS-8: the bloom feature defines these from its parameters. Outside the
// feature (a graph's render.fullscreen, the embedded SPIR-V) the shader reads
// the feature's parameter defaults (features/bloom.json threshold / knee),
// so it compiles on its own. The names stay the engine's: a pass cannot
// define them (shader.defines refuses PELICAN_FEATURE_*).
#ifndef PELICAN_FEATURE_BLOOM_THRESHOLD
#define PELICAN_FEATURE_BLOOM_THRESHOLD 0.4
#endif
#ifndef PELICAN_FEATURE_BLOOM_KNEE
#define PELICAN_FEATURE_BLOOM_KNEE 0.7
#endif

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D inputTexture;

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

// 輝度閾値と膝領域の強度 (より滑らかなロールオフ用)。
// float() は define が "2" のような整数リテラルで届いた場合の保険。
const float threshold = float(PELICAN_FEATURE_BLOOM_THRESHOLD);
const float knee = float(PELICAN_FEATURE_BLOOM_KNEE);

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
