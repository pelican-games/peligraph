#version 460
#extension GL_EXT_ray_tracing : require

struct Payload {
    uint record_id;
    float first_t;
    uint depth;
    uint status;
};

layout(location = 1) rayPayloadInEXT Payload payload;
hitAttributeEXT vec2 hit_attribute;

void main() {
    payload.status = 1u;
    payload.depth += 1u;
}
