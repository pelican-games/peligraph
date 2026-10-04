#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference2 : require

// ray_tracing_layout.rchit: runs at the nearest surface a ray hits, for the objects
// assigned to this hit group. It answers with the surface's normal as a
// colour, tinted with the group's colour.
#include "__pelican_ray_group_layout.glsl"

layout(location = 0) rayPayloadInEXT vec4 hit;

// This hit group's colour.
const vec3 GROUP_TINT = vec3(1.00, 1.00, 1.00);

hitAttributeEXT vec2 hit_attribute;
// The engine's record: which geometry and material the ray hit.
layout(shaderRecordEXT, std430) buffer HitRecord {
    PELICAN_RAY_ENGINE_RECORD_MEMBERS
} hit_record;

void main() {
    vec3 object_normal = pelicanRayFetchNormal(hit_record.geometry_index,
                                               gl_PrimitiveID, hit_attribute);
    vec3 normal = normalize(transpose(mat3(gl_WorldToObjectEXT)) * object_normal);
    hit = vec4((normal * 0.5 + 0.5) * GROUP_TINT, 1.0);
}
