#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"
#include "pelican_material.glsl"
#include "pelican_morph.glsl"

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

float playbackFrame() {
    PelicanMaterialData material = pelicanMaterials.materials[pelicanPush.materialIndex];
    float frame_count = max(material.vatBoundsMinFrameCount.w, 1.0);
    float frame = max(pelicanFrame.time_delta.x, 0.0) * material.vatBoundsExtentFps.w;
    if (material.vatFlags.y != 0) {
        return mod(frame, frame_count);
    }
    return clamp(frame, 0.0, frame_count - 1.0);
}

vec3 sampleVatPosition(int vertex_index, int row) {
    PelicanMaterialData material = pelicanMaterials.materials[pelicanPush.materialIndex];
    vec3 normalized_pos = texelFetch(vatPositionSampler, ivec2(vertex_index, row), 0).rgb;
    return material.vatBoundsMinFrameCount.xyz + normalized_pos * material.vatBoundsExtentFps.xyz;
}

vec3 sampleVatNormal(int vertex_index, int row) {
    return texelFetch(vatNormalSampler, ivec2(vertex_index, row), 0).rgb;
}

void main() {
    PelicanMaterialData material = pelicanMaterials.materials[pelicanPush.materialIndex];
    int texture_width = textureSize(vatPositionSampler, 0).x;
    int vertex_index = int(gl_VertexIndex) - material.vatFlags.x;
    vertex_index = clamp(vertex_index, 0, max(texture_width - 1, 0));

    float frame = playbackFrame();
    float frame0 = floor(frame);
    float frame_count = max(material.vatBoundsMinFrameCount.w, 1.0);
    float frame1 = min(frame0 + 1.0, frame_count - 1.0);
    if (material.vatFlags.y != 0) {
        frame1 = mod(frame0 + 1.0, frame_count);
    }
    float frame_blend = fract(frame);

    vec3 local_pos = mix(sampleVatPosition(vertex_index, int(frame0)),
                         sampleVatPosition(vertex_index, int(frame1)),
                         frame_blend);
    vec3 local_normal = inNormal;
    if (material.vatFlags.z != 0) {
        local_normal = normalize(mix(sampleVatNormal(vertex_index, int(frame0)),
                                     sampleVatNormal(vertex_index, int(frame1)),
                                     frame_blend));
    }
    PelicanMorphedVertex morphed = pelican_morph_vertex(
        local_pos, local_normal, inTangent.xyz, gl_VertexIndex, false);
    local_pos = morphed.position;
    local_normal = morphed.normal;

    mat4 model_matrix = pelicanObjects.objects[gl_BaseInstance].model;
    vec4 world_pos = model_matrix * vec4(local_pos, 1.0);

    gl_Position = pelicanPush.engineMvp * world_pos;
    outTexUV = inTexUV;
    outColor = inColor * material.baseColorFactor;

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
