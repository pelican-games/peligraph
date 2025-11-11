#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(push_constant) uniform SceneData {
    mat4 mvpMatrix;
} drawInfo;

layout(location = 0) in vec3 inPos;

void main() {
    gl_Position = drawInfo.mvpMatrix * vec4(inPos, 1.0);
}