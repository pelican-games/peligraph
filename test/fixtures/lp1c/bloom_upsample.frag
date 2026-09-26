#version 460
// LP-1c test fixture: engine://bloom_upsample with its pass inputs read through
// resource-port accessors, the only way a graph render.fullscreen pass
// binds an image. The arithmetic is the engine shader's, line for line.
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"
#include "pelican_resource_ports.glsl"


layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = pelican_sample_source(inTexCoord);
}
