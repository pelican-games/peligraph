#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"
// P-2a-2: `intensity` is a runtime value, not a define. The generated include
// declares the accessor for the feature's `bloom.intensity` buffer.
// RS-11: the port is optional. The generated include defines
// PELICAN_RESOURCE_PORT_<port> for each port the pass maps; a pass without
// a bloomIntensity port (a graph's render.fullscreen that leaves it out)
// compiles against the engine's empty pelican_resource_ports.glsl and reads
// the feature's parameter default (features/bloom.json intensity).
#include "pelican_resource_ports.glsl"

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D inputTexture0;
layout(set = PELICAN_SET_PASS_INPUT, binding = 1) uniform sampler2D inputTexture1;

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    // A value of 1.0 is a neutral additive blend.
    // Higher values make the bloom effect stronger.
#ifdef PELICAN_RESOURCE_PORT_bloomIntensity
    float bloomIntensity = pelican_load_bloomIntensity(0u);
#else
    float bloomIntensity = 2.0;
#endif

    vec4 color0 = texture(inputTexture0, inTexCoord);
    vec4 color1 = texture(inputTexture1, inTexCoord);

    // Additive blend
    outColor = color0 + color1 * bloomIntensity;
}
