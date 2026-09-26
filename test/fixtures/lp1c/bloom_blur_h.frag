#version 460
// LP-1c test fixture: engine://bloom_blur_h with its pass inputs read through
// resource-port accessors, the only way a graph render.fullscreen pass
// binds an image. The arithmetic is the engine shader's, line for line.
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"
#include "pelican_resource_ports.glsl"


layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

const float weight[5] = float[] (0.227027, 0.1945946, 0.1216216, 0.05405405, 0.01621621);

void main() {
    vec2 texelSize = 1.0 / pelican_size_source();
    vec3 result = pelican_sample_source(inTexCoord).rgb * weight[0];

    for (int i = 1; i < 5; ++i) {
        result += pelican_sample_source(inTexCoord + vec2(texelSize.x * float(i), 0.0)).rgb * weight[i];
        result += pelican_sample_source(inTexCoord - vec2(texelSize.x * float(i), 0.0)).rgb * weight[i];
    }

    outColor = vec4(result, 1.0);
}
