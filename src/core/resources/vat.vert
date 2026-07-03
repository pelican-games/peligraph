#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"

struct ObjectData{
    mat4 model;
};

layout(set = PELICAN_SET_FRAME, binding = 0) readonly buffer ObjectBuffer{
    ObjectData objects[];
} object_buffer;

layout(set = PELICAN_SET_MATERIAL, binding = 4) uniform sampler2D vatPositionSampler;
layout(set = PELICAN_SET_MATERIAL, binding = 5) uniform sampler2D vatNormalSampler;

layout(push_constant) uniform SceneData {
    mat4 vpMatrix;
    vec4 vatBoundsMinTime;
    vec4 vatBoundsExtentFrame;
    vec4 vatPlaybackFlags;
} drawInfo;

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

float playbackFrame() {
    float frame_count = max(drawInfo.vatBoundsExtentFrame.w, 1.0);
    float frame = max(drawInfo.vatBoundsMinTime.w, 0.0) * drawInfo.vatPlaybackFlags.x;
    if (drawInfo.vatPlaybackFlags.y > 0.5) {
        return mod(frame, frame_count);
    }
    return clamp(frame, 0.0, frame_count - 1.0);
}

vec3 sampleVatPosition(int vertex_index, int row) {
    vec3 normalized_pos = texelFetch(vatPositionSampler, ivec2(vertex_index, row), 0).rgb;
    return drawInfo.vatBoundsMinTime.xyz + normalized_pos * drawInfo.vatBoundsExtentFrame.xyz;
}

vec3 sampleVatNormal(int vertex_index, int row) {
    return texelFetch(vatNormalSampler, ivec2(vertex_index, row), 0).rgb;
}

void main() {
    int texture_width = textureSize(vatPositionSampler, 0).x;
    int vertex_index = int(gl_VertexIndex) - int(drawInfo.vatPlaybackFlags.z);
    vertex_index = clamp(vertex_index, 0, max(texture_width - 1, 0));

    float frame = playbackFrame();
    float frame0 = floor(frame);
    float frame_count = max(drawInfo.vatBoundsExtentFrame.w, 1.0);
    float frame1 = min(frame0 + 1.0, frame_count - 1.0);
    if (drawInfo.vatPlaybackFlags.y > 0.5) {
        frame1 = mod(frame0 + 1.0, frame_count);
    }
    float frame_blend = fract(frame);

    vec3 local_pos = mix(sampleVatPosition(vertex_index, int(frame0)),
                         sampleVatPosition(vertex_index, int(frame1)),
                         frame_blend);
    vec3 local_normal = inNormal;
    if (drawInfo.vatPlaybackFlags.w > 0.5) {
        local_normal = normalize(mix(sampleVatNormal(vertex_index, int(frame0)),
                                     sampleVatNormal(vertex_index, int(frame1)),
                                     frame_blend));
    }

    mat4 model_matrix = object_buffer.objects[gl_BaseInstance].model;
    vec4 world_pos = model_matrix * vec4(local_pos, 1.0);

    gl_Position = drawInfo.vpMatrix * world_pos;
    outTexUV = inTexUV;
    outColor = inColor;

    vec3 N = normalize(mat3(model_matrix) * local_normal);

    if (inTangent.w != 0.0)
    {
        vec3 T = normalize(mat3(model_matrix) * inTangent.xyz);
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
}
