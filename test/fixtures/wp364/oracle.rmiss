#version 460
#extension GL_EXT_ray_tracing : require

struct Payload {
    uint record_id;
    float first_t;
    uint depth;
    uint status;
};

layout(location = 0) rayPayloadInEXT Payload payload;

void main() {
    payload.record_id = 0x777u;
    payload.status = 0u;
}
