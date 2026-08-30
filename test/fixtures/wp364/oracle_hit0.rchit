#version 460
#extension GL_EXT_ray_tracing : require

layout(set = 0, binding = 6)
    uniform accelerationStructureEXT pelicanRayTracingScene;

struct Payload {
    uint record_id;
    float first_t;
    uint depth;
    uint status;
};

layout(location = 0) rayPayloadInEXT Payload payload;
layout(location = 1) rayPayloadEXT Payload nested_payload;
hitAttributeEXT vec2 hit_attribute;

void main() {
    payload.record_id = 0x364u;
    payload.status = 1u;
    payload.depth += 1u;
    if (payload.depth == 1u) {
        payload.first_t = gl_HitTEXT;
    }
    if (payload.depth < 2u) {
        nested_payload = payload;
        traceRayEXT(
            pelicanRayTracingScene, gl_RayFlagsOpaqueEXT, 0xffu,
            1u, 0u, 1u,
            gl_WorldRayOriginEXT, gl_HitTEXT + 0.001,
            gl_WorldRayDirectionEXT, 4096.0, 1);
        payload = nested_payload;
    }
}
