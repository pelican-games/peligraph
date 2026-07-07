#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"

struct ObjectData {
    mat4 model;
};

layout(set = PELICAN_SET_FRAME, binding = 0) readonly buffer ObjectBuffer {
    ObjectData objects[];
} object_buffer;

layout(push_constant) uniform SceneData {
    mat4 vpMatrix;
} drawInfo;

layout(location = 0) in vec3 inPos;

void main() {
    mat4 model_matrix = object_buffer.objects[gl_BaseInstance].model;
    gl_Position = drawInfo.vpMatrix * model_matrix * vec4(inPos, 1.0);
}
