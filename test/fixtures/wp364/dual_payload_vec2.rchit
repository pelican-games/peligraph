#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 1) rayPayloadInEXT vec2 payload;
hitAttributeEXT vec2 hit_attribute;

void main() {
    payload = hit_attribute;
}
