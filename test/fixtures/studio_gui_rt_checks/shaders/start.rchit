#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_tracing : require
// The fixture's closest hit of ray type primary: the surface normal as a
// colour -- a triangle's from the engine's geometry table, the ball's from
// the sphere -- plus the record's value. Its record is in the block Studio
// writes again when the layout's record changes.
#include "__pelican_ray_group_layout.glsl"
layout(location = 0) rayPayloadInEXT vec4 hit;
hitAttributeEXT vec2 barycentric;

// pelican: shader record: Studio writes this block again when the ray group layout's
// record changes.
layout(shaderRecordEXT, std430) buffer PelicanHitRecord {
    PELICAN_RAY_ENGINE_RECORD_MEMBERS
    float value;
} record;
vec3 recordColor(vec3 color) { return color + vec3(record.value); }
// pelican: end shader record

void main() {
    vec3 normal;
    if (gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT || gl_HitKindEXT == gl_HitKindBackFacingTriangleEXT) {
        vec3 object_normal = pelicanRayFetchNormal(record.geometry_index, gl_PrimitiveID, barycentric);
        normal = normalize(transpose(mat3(gl_WorldToObjectEXT)) * object_normal);
    } else {
        vec3 local = gl_ObjectRayOriginEXT + gl_HitTEXT * gl_ObjectRayDirectionEXT;
        normal = normalize(transpose(mat3(gl_WorldToObjectEXT)) * local);
    }
    hit = vec4(recordColor(normal * 0.5 + 0.5), gl_HitTEXT);
}
