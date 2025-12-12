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

layout(location = 0) out vec2 outTexUV;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec3 outNormal;
layout(location = 3) out vec3 outWorldPos;

void main() {
    mat4 model_matrix = object_buffer.objects[gl_BaseInstance].model;
    vec4 world_pos = model_matrix * vec4(inPos, 1.0);
    
    gl_Position = drawInfo.vpMatrix * world_pos;
    outTexUV = inTexUV;
    outColor = inColor;
    
    // 法線をワールド座標に変換（非均等スケールに対応させるなら転置逆行列を使用）
    outNormal = normalize(mat3(model_matrix) * inNormal);
    outWorldPos = world_pos.xyz;
}