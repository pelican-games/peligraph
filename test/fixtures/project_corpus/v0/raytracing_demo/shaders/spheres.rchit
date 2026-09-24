#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
layout(location=0) rayPayloadInEXT vec3 color;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer SphereRecord {
    PELICAN_RAY_ENGINE_RECORD_MEMBERS
    float radius;
    vec3 center;
} sphere;
void main() {
    vec3 position = gl_ObjectRayOriginEXT + gl_HitTEXT * gl_ObjectRayDirectionEXT;
    vec3 objectNormal = normalize(position - sphere.center);
    vec3 normal = normalize(transpose(mat3(gl_WorldToObjectEXT)) * objectNormal);
    vec3 light = normalize(vec3(-0.6, 0.8, -0.7));
    float diffuse = max(dot(normal, light), 0.0);
    vec3 base = sphere.radius > 1.0 ? vec3(0.10, 0.55, 0.95) : vec3(1.0, 0.30, 0.10);
    float highlight = pow(max(dot(reflect(-light, normal), -gl_WorldRayDirectionEXT), 0.0), 48.0);
    color = base * (0.16 + 0.84 * diffuse) + vec3(0.45) * highlight;
}
