#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_tracing : require
// The fixture's starting closest hit: the surface normal as a colour.
#include "__pelican_ray_group_layout.glsl"
layout(location = 0) rayPayloadInEXT vec4 hit;
hitAttributeEXT vec2 barycentric;
layout(shaderRecordEXT, std430) buffer PelicanHitRecord {
    PELICAN_RAY_ENGINE_RECORD_MEMBERS
} record;
void main() {
    vec3 object_normal = pelicanRayFetchNormal(record.geometry_index, gl_PrimitiveID, barycentric);
    vec3 normal = normalize(transpose(mat3(gl_WorldToObjectEXT)) * object_normal);
    hit = vec4(normal * 0.5 + 0.5, gl_HitTEXT);
}
