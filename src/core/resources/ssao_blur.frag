#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"
#include "pelican_resource_ports.glsl"

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    // This is a simple blur for the SSAO map.
    // A better implementation would be a bilateral blur that respects depth discontinuities.
    
    vec2 texelSize = 1.0 / vec2(pelican_size_ssaoInput());
    float result = 0.0;
    
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            result += pelican_sample_ssaoInput(
                inTexCoord + offset, pelican_view_index()).r;
        }
    }
    
    float final_ao = result / 25.0;
    
    outColor = vec4(final_ao, final_ao, final_ao, 1.0);
}
