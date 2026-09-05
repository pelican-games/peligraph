#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"
#include "pelican_material.glsl"
#define PELICAN_MATERIAL_INSTANCE_VERTEX_STAGE
#include "pelican_material_instance.glsl"
#include "pelican_morph.glsl"
#include "pelican_vat.glsl"

layout(set = PELICAN_SET_MATERIAL, binding = 4) uniform sampler2D vatPositionSampler;
layout(set = PELICAN_SET_MATERIAL, binding = 5) uniform sampler2D vatNormalSampler;

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
layout(location = 6) flat out uint outMaterialInstanceIndex;

void main() {
    PelicanMaterialData material = pelicanMaterials.materials[pelicanPush.materialIndex];
    PelicanVatVertex vatVertex = pelican_vat_vertex(
        vatPositionSampler, vatNormalSampler,
        material.vatBoundsMinFrameCount, material.vatBoundsExtentFps, material.vatFlags,
        pelicanFrame.time_delta.x, gl_VertexIndex, inNormal);
    vec3 local_pos = vatVertex.position;
    vec3 local_normal = vatVertex.normal;
    PelicanMorphedVertex morphed = pelican_morph_vertex(
        local_pos, local_normal, inTangent.xyz, gl_VertexIndex, false);
    local_pos = morphed.position;
    local_normal = morphed.normal;

    mat4 model_matrix = pelicanObjects.objects[gl_BaseInstance].model;
    vec4 world_pos = model_matrix * vec4(local_pos, 1.0);

    gl_Position = pelicanPush.engineMvp * world_pos;
    outTexUV = inTexUV;
    outColor = inColor *
               pelican_material_instance_base_color_source_factor(
                   material.baseColorFactor);

    vec3 N = normalize(mat3(model_matrix) * local_normal);

    if (inTangent.w != 0.0)
    {
        vec3 T = normalize(mat3(model_matrix) * morphed.tangent);
        T = normalize(T - dot(T, N) * N);
        vec3 B = cross(N, T) * inTangent.w;
        outTangent = T;
        outBitangent = B;
    }
    else
    {
        outTangent = vec3(0.0, 0.0, 0.0);
        outBitangent = vec3(0.0, 0.0, 0.0);
    }

    outNormal = N;
    outWorldPos = world_pos.xyz;
    outMaterialInstanceIndex = gl_BaseInstance;
}
