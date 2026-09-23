#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
#include "pt_payload.glsl"

layout(location = 0) rayPayloadInEXT RadiancePayload radiancePayload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer MeshRecord {
    PT_RECORD_MEMBERS
} mesh;

void main() {
    // glTF triangle: the engine's set-3 tables give the interpolated vertex
    // normal and UV, and the material row gives base color factor x texture.
    // The ray material table carries no metallic / roughness / transmission
    // yet, so meshes shade as a rough dielectric-coated diffuse surface.
    vec3 objectNormal = pelicanRayFetchNormal(
        mesh.geometry_index, gl_PrimitiveID, hit_attribute);
    vec3 normal = normalize(
        transpose(mat3(gl_WorldToObjectEXT)) * objectNormal);
    vec2 uv = pelicanRayFetchUV(
        mesh.geometry_index, gl_PrimitiveID, hit_attribute);
    vec4 baseColor = pelicanRaySampleBaseColor(mesh.material_index, uv);

    radiancePayload.hit = 1u;
    radiancePayload.position = gl_WorldRayOriginEXT +
        gl_HitTEXT * gl_WorldRayDirectionEXT;
    radiancePayload.normal = normal;
    radiancePayload.base_color = baseColor.rgb;
    radiancePayload.emission = vec3(0.0);
    radiancePayload.metallic = 0.0;
    radiancePayload.roughness = 1.0;
    radiancePayload.ior = 1.5;
    radiancePayload.transmission = 0.0;
}
