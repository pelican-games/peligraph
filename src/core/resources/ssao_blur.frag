#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_view.glsl"
#include "pelican_frame.glsl"

PELICAN_DECLARE_INPUT_0(ssaoInput);

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    // This is a simple blur for the SSAO map.
    // A better implementation would be a bilateral blur that respects depth discontinuities.
    
    vec2 texelSize = 1.0 / textureSize(ssaoInput, 0);
    float result = 0.0;
    
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            result += PELICAN_TEXTURE_2D_0(ssaoInput, inTexCoord + offset).r;
        }
    }
    
    float final_ao = result / 25.0;
    
    outColor = vec4(final_ao, final_ao, final_ao, 1.0);
}
