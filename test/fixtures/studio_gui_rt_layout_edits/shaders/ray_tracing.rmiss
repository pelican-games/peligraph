#version 460
#extension GL_EXT_ray_tracing : require

// ray_tracing.rmiss: a ray tracing set Studio wrote (raygen + miss + closest hit).
// Runs where a ray hits nothing.

// The payload the three stages share: xyz is where the ray hit, w is 1
// on a hit and 0 when the miss shader ran.
layout(location = 0) rayPayloadInEXT vec4 hit;

void main() {
    hit.w = 0.0;
}
