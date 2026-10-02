#version 450
#extension GL_GOOGLE_include_directive : require

#include "intro_link_color.glsl"

layout(push_constant) uniform Push {
    float time;
} push;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(introLinkColor(push.time), 1.0);
}
