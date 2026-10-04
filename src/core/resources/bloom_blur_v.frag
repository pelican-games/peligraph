#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D inputTexture;

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

// engine://bloom_blur_v -- the vertical half of a separable 9-tap Gaussian
// (engine://bloom_blur_h is the horizontal half).
//
// The taps are the middle nine binomial coefficients of order 12,
// C(12, 6 +- k) for k = 0..4 (924, 792, 495, 220, 66), divided by their sum
// 4070 and written to 7 significant digits. A binomial row approximates a
// Gaussian; dropping the two outermost coefficients on each side loses
// little weight.
const float kBinomialTap[5] = float[](0.227027, 0.1945946, 0.1216216, 0.05405405, 0.01621621);

// The input's colour k texels down (k > 0) or up (k < 0) in texture coordinates.
vec3 texelsAway(vec2 texelStep, int k) {
    return texture(inputTexture, inTexCoord + vec2(0.0, texelStep.y * float(k))).rgb;
}

void main() {
    vec2 texelStep = 1.0 / textureSize(inputTexture, 0);
    vec3 blurred = texture(inputTexture, inTexCoord).rgb * kBinomialTap[0];
    for (int k = 1; k <= 4; ++k) {
        blurred += texelsAway(texelStep, k) * kBinomialTap[k];
        blurred += texelsAway(texelStep, -k) * kBinomialTap[k];
    }
    outColor = vec4(blurred, 1.0);
}
