#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(push_constant) uniform SceneData {
    mat4 mvpMatrix;
} drawInfo;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inNormal;
layout(location = 2) in vec2 inTexUV;
layout(location = 3) in vec2 inColor;
layout(location = 0) out vec2 outTexUV;

void main() {
    gl_Position = drawInfo.mvpMatrix * vec4(inPos, 1.0);
    outTexUV = inTexUV;
}