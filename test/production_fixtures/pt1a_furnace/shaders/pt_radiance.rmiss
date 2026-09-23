#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "pt_payload.glsl"
#include "pt_scene.glsl"

layout(location = 0) rayPayloadInEXT RadiancePayload radiancePayload;

void main() {
    radiancePayload.hit = 0u;
    radiancePayload.emission = PT_SKY_RADIANCE(gl_WorldRayDirectionEXT);
}
