#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
#include "pt_payload.glsl"

layout(location = 1) rayPayloadInEXT ShadowPayload shadowPayload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer ShadowRecord {
    PT_RECORD_MEMBERS
} record;

void main() {
    shadowPayload.occluded = 1u;
}
