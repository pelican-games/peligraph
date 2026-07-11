#version 450
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D displaySampler;
layout(location = 0) in vec2 texUV;
layout(location = 0) out vec4 outColor;

vec3 linearToSrgb(vec3 value) {
    bvec3 low = lessThanEqual(value, vec3(0.0031308));
    vec3 linearSegment = value * 12.92;
    vec3 powerSegment = 1.055 * pow(max(value, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(powerSegment, linearSegment, low);
}

void main() {
    vec4 linear = texture(displaySampler, texUV);
#ifdef PELICAN_OUTPUT_UNORM_FALLBACK
    outColor = vec4(linearToSrgb(linear.rgb), linear.a);
#else
    outColor = linear;
#endif
}
