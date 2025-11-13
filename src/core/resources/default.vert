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
layout(location = 1) in vec2 inNormal;
layout(location = 2) in vec2 inTexUV;
layout(location = 3) in vec4 inColor;
layout(location = 0) out vec2 outTexUV;
layout(location = 1) out vec4 outColor;

void main() {
    gl_Position = drawInfo.vpMatrix * object_buffer.objects[gl_BaseInstance].model * vec4(inPos, 1.0);
    outTexUV = inTexUV;
    outColor = inColor;
}