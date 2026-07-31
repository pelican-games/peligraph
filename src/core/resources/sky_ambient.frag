#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_frame.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

PELICAN_DECLARE_INPUT_0(sceneDepthSampler);

void main() {
    float scene_depth =
        PELICAN_TEXTURE_2D_0(
            sceneDepthSampler, inUV).r;
    if (scene_depth < 1.0) {
        discard;
    }
    outColor = vec4(
        pelicanLights.environmentSkyRadiance.rgb,
        1.0);
}
