#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "pelican_frame.glsl"
#include "pelican_material.glsl"
#include "pelican_skinning.glsl"
#include "pelican_morph.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;
layout(location = 5) in ivec4 inJoints;
layout(location = 6) in vec4 inWeights;

layout(location = 0) out vec2 outTexUV;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec3 outNormal;
layout(location = 3) out vec3 outWorldPos;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) flat out uint outMaterialInstanceIndex;

void main() {
    PelicanMorphedVertex morphed =
        pelican_morph_vertex(inPos, inNormal, inTangent.xyz, gl_VertexIndex, false);
    mat4 skin = pelican_skin_matrix(inJoints, inWeights);
    mat4 model_matrix = pelicanObjects.objects[gl_BaseInstance].model;
    vec4 world_pos = model_matrix * skin * vec4(morphed.position, 1.0);
    gl_Position = pelicanPush.engineMvp * world_pos;
    outTexUV = inTexUV;
    outColor = inColor * pelicanMaterials.materials[pelicanPush.materialIndex].baseColorFactor;
    mat3 normal_matrix = mat3(model_matrix) * mat3(skin);
    vec3 normal = normalize(normal_matrix * morphed.normal);
    outNormal = normal;
    outWorldPos = world_pos.xyz;
    outMaterialInstanceIndex = gl_BaseInstance;
    if (inTangent.w != 0.0) {
        vec3 tangent = normalize(normal_matrix * morphed.tangent);
        tangent = normalize(tangent - dot(tangent, normal) * normal);
        outTangent = tangent;
        outBitangent = cross(normal, tangent) * inTangent.w;
    } else {
        outTangent = vec3(0.0);
        outBitangent = vec3(0.0);
    }
}
