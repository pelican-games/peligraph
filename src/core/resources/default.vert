#version 460
#extension GL_ARB_separate_shader_objects : enable

struct ObjectData{
    mat4 model;
};

layout(set = 0, binding = 0) readonly buffer ObjectBuffer{
    ObjectData objects[];
} object_buffer;

layout(push_constant) uniform SceneData {
    mat4 vpMatrix;
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

void main() {
    mat4 model_matrix = object_buffer.objects[gl_BaseInstance].model;
    vec4 world_pos = model_matrix * vec4(inPos, 1.0);
    
    gl_Position = drawInfo.vpMatrix * world_pos;
    outTexUV = inTexUV;
    outColor = inColor;
    
    // TBN matrix components to world space
    vec3 N = normalize(mat3(model_matrix) * inNormal);
    
    // Only calculate T and B if tangent data is present
    if (inTangent.w != 0.0)
    {
        vec3 T = normalize(mat3(model_matrix) * inTangent.xyz);
        // Re-orthogonalize T with respect to N
        T = normalize(T - dot(T, N) * N);
        vec3 B = cross(N, T) * inTangent.w;
        outTangent = T;
        outBitangent = B;
    }
    else
    {
        // Pass zero vectors if no tangent data
        outTangent = vec3(0.0, 0.0, 0.0);
        outBitangent = vec3(0.0, 0.0, 0.0);
    }

    outNormal = N;
    outWorldPos = world_pos.xyz;
}