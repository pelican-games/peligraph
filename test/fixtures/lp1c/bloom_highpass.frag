#version 460
// LP-1c test fixture: engine://bloom_highpass with its pass inputs read through
// resource-port accessors, the only way a graph render.fullscreen pass
// binds an image. The arithmetic is the engine shader's, line for line.
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"
#include "pelican_resource_ports.glsl"

#ifndef PELICAN_FEATURE_BLOOM_THRESHOLD
#error PELICAN_FEATURE_BLOOM_THRESHOLD must be defined for the bloom highpass shader
#endif
#ifndef PELICAN_FEATURE_BLOOM_KNEE
#error PELICAN_FEATURE_BLOOM_KNEE must be defined for the bloom highpass shader
#endif


layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

const float threshold = float(PELICAN_FEATURE_BLOOM_THRESHOLD);
const float knee = float(PELICAN_FEATURE_BLOOM_KNEE);

void main() {
    vec4 color = pelican_sample_source(inTexCoord);
    
    // RGB to Luminance (ITU-R BT.709)
    float luminance = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));

    float softThreshold = luminance - threshold;
    float bright = softThreshold * (2.0 * knee);
    bright = clamp(bright, 0.0, 1.0);
    bright = softThreshold - bright * bright / (4.0 * knee);
    bright = max(bright, 0.0); // Ensure no negative values

    outColor = color * bright;
}
