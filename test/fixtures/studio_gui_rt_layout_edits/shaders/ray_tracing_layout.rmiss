#version 460
#extension GL_EXT_ray_tracing : require

// ray_tracing_layout.rmiss: runs where a ray hits nothing.
layout(location = 0) rayPayloadInEXT vec4 hit;

void main() {
    hit = vec4(0.0);
}
