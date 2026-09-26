#version 460
// LP-1c test fixture: engine://bloom_composite with its pass inputs read through
// resource-port accessors, the only way a graph render.fullscreen pass
// binds an image. The arithmetic is the engine shader's, line for line.
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"
// P-2a-2: `intensity` is a runtime value, not a define. The generated include
// declares the accessor for the feature's `bloom.intensity` buffer; a pass that
// does not map that port fails to compile by the accessor's name.
#include "pelican_resource_ports.glsl"


layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    // A value of 1.0 is a neutral additive blend.
    // Higher values make the bloom effect stronger.
    float bloomIntensity = pelican_load_bloomIntensity(0u);

    vec4 color0 = pelican_sample_scene(inTexCoord);
    vec4 color1 = pelican_sample_bloom(inTexCoord);

    // Additive blend
    outColor = color0 + color1 * bloomIntensity;
}
