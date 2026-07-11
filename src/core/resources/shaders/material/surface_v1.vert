#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_GOOGLE_cpp_style_line_directive : enable

#include "pelican_frame.glsl"
#include "pelican_material.glsl"
#include "pelican_surface_v1.glsl"
#include "__pelican_surface_params.glsl"

#ifdef PELICAN_HAS_VERTEX_DISPLACE_V1
#include "__pelican_user_surface.glsl"
#endif

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;

layout(location = 0) out vec2 outTexUV;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec3 outNormal;
layout(location = 3) out vec3 outWorldPos;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outCustom0;
layout(location = 7) out vec4 outCustom1;

void main() {
    PelicanVertexV1 vertex;
    vertex.position = inPos;
    vertex.normal = inNormal;
    vertex.custom0 = vec4(0.0);
    vertex.custom1 = vec4(0.0);
#ifdef PELICAN_HAS_VERTEX_DISPLACE_V1
    pelican_vertex_displace_v1(vertex);
#endif

    mat4 model_matrix = pelicanObjects.objects[gl_BaseInstance].model;
    vec4 world_pos = model_matrix * vec4(vertex.position, 1.0);
    gl_Position = pelicanPush.engineMvp * world_pos;
    outTexUV = inTexUV;
    outColor = inColor * pelicanMaterials.materials[pelicanPush.materialIndex].baseColorFactor;
    vec3 normal = normalize(mat3(model_matrix) * vertex.normal);
    outNormal = normal;
    outWorldPos = world_pos.xyz;
    outCustom0 = vertex.custom0;
    outCustom1 = vertex.custom1;
    if (inTangent.w != 0.0) {
        vec3 tangent = normalize(mat3(model_matrix) * inTangent.xyz);
        tangent = normalize(tangent - dot(tangent, normal) * normal);
        outTangent = tangent;
        outBitangent = cross(normal, tangent) * inTangent.w;
    } else {
        outTangent = vec3(0.0);
        outBitangent = vec3(0.0);
    }
}
