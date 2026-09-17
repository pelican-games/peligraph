#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"

#ifndef PELICAN_FEATURE_BLOOM_INTENSITY
#error PELICAN_FEATURE_BLOOM_INTENSITY must be defined for the bloom composite shader
#endif

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D inputTexture0;
layout(set = PELICAN_SET_PASS_INPUT, binding = 1) uniform sampler2D inputTexture1;

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

// A value of 1.0 is a neutral additive blend.
// Higher values make the bloom effect stronger.
// float() は define が "2" のような整数リテラルで届いた場合の保険。
const float bloomIntensity = float(PELICAN_FEATURE_BLOOM_INTENSITY);

void main() {
    vec4 color0 = texture(inputTexture0, inTexCoord);
    vec4 color1 = texture(inputTexture1, inTexCoord);

    // Additive blend
    outColor = color0 + color1 * bloomIntensity;
}
