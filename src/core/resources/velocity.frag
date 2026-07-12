#version 460

layout(location = 0) in vec4 currentClip;
layout(location = 1) in vec4 previousClip;
layout(location = 0) out vec2 outVelocity;

void main() {
    vec2 current_ndc = currentClip.xy / currentClip.w;
    vec2 previous_ndc = previousClip.xy / previousClip.w;
    outVelocity = (current_ndc - previous_ndc) * 0.5;
}
