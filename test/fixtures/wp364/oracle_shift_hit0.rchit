#version 460
#extension GL_EXT_ray_tracing : require

struct Payload {
    uint record_id;
    float first_t;
    uint depth;
    uint status;
};

layout(location = 0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec2 hit_attribute;

void main() {
    payload.record_id = 0x364u;
    payload.first_t = gl_HitTEXT;
    payload.depth = 1u;
    payload.status = 1u;
}
