#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "pt_payload.glsl"

layout(location = 1) rayPayloadInEXT ShadowPayload shadowPayload;

void main() {
    shadowPayload.occluded = 0u;
}
