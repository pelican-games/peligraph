#version 460
#extension GL_EXT_ray_tracing : require

struct Payload {
    uint record_id;
    float first_t;
    uint depth;
    uint status;
};

layout(location = 1) rayPayloadInEXT Payload payload;

void main() {
    payload.depth += 1u;
}
