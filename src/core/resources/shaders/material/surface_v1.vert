#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_GOOGLE_cpp_style_line_directive : enable

#include "pelican_frame.glsl"
#include "pelican_material.glsl"
#include "pelican_surface_v1.glsl"
#include "pelican_morph.glsl"
#include "__pelican_surface_params.glsl"
#ifdef PELICAN_SKINNED
#include "pelican_skinning.glsl"
#endif

#ifdef PELICAN_HAS_VERTEX_DISPLACE_V1
#include "__pelican_user_surface.glsl"
#endif

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;
#ifdef PELICAN_SKINNED
layout(location = 5) in ivec4 inJoints;
layout(location = 6) in vec4 inWeights;
#endif

layout(location = 0) out vec2 outTexUV;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec3 outNormal;
layout(location = 3) out vec3 outWorldPos;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outCustom0;
layout(location = 7) out vec4 outCustom1;
layout(location = 8) flat out uint outMaterialInstanceIndex;

void main() {
    PelicanVertexV1 vertex;
    PelicanMorphedVertex morphed =
        pelican_morph_vertex(inPos, inNormal, inTangent.xyz, gl_VertexIndex, false);
#ifdef PELICAN_SKINNED
    mat4 skin_matrix = pelican_skin_matrix(inJoints, inWeights);
    vertex.position = (skin_matrix * vec4(morphed.position, 1.0)).xyz;
    vertex.normal = normalize(mat3(skin_matrix) * morphed.normal);
#else
    vertex.position = morphed.position;
    vertex.normal = morphed.normal;
#endif
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
    outMaterialInstanceIndex = gl_BaseInstance;
    if (inTangent.w != 0.0) {
        vec3 tangent_local = morphed.tangent;
#ifdef PELICAN_SKINNED
        tangent_local = mat3(skin_matrix) * tangent_local;
#endif
        vec3 tangent = normalize(mat3(model_matrix) * tangent_local);
        tangent = normalize(tangent - dot(tangent, normal) * normal);
        outTangent = tangent;
        outBitangent = cross(normal, tangent) * inTangent.w;
    } else {
        outTangent = vec3(0.0);
        outBitangent = vec3(0.0);
    }
}
