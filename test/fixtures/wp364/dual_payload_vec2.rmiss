#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 1) rayPayloadInEXT vec2 payload;

void main() {
    payload = vec2(1.0);
}
