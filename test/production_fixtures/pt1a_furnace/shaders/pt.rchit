#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
#include "pt_payload.glsl"

layout(location = 0) rayPayloadInEXT RadiancePayload radiancePayload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer SphereRecord {
    PT_RECORD_MEMBERS
} sphere;

void main() {
    vec3 objectPosition = gl_ObjectRayOriginEXT +
        gl_HitTEXT * gl_ObjectRayDirectionEXT;
    vec3 objectNormal = normalize(objectPosition - sphere.center);
    radiancePayload.hit = 1u;
    radiancePayload.position = gl_WorldRayOriginEXT +
        gl_HitTEXT * gl_WorldRayDirectionEXT;
    // Outward geometric normal; the raygen decides entering / leaving.
    radiancePayload.normal = normalize(
        transpose(mat3(gl_WorldToObjectEXT)) * objectNormal);
    radiancePayload.base_color = sphere.base_color;
    radiancePayload.emission = sphere.emission;
    radiancePayload.metallic = sphere.metallic;
    radiancePayload.roughness = sphere.roughness;
    radiancePayload.ior = sphere.ior;
    radiancePayload.transmission = sphere.transmission;
}
