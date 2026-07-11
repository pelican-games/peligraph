#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "pelican_frame.glsl"
#include "pelican_skinning.glsl"

layout(push_constant) uniform SceneData {
    layout(offset = 0) mat4 vpMatrix;
} drawInfo;

layout(location = 0) in vec3 inPos;
layout(location = 5) in ivec4 inJoints;
layout(location = 6) in vec4 inWeights;

void main() {
    mat4 model_matrix = pelicanObjects.objects[gl_BaseInstance].model;
    gl_Position = drawInfo.vpMatrix * model_matrix * pelican_skin_matrix(inJoints, inWeights) * vec4(inPos, 1.0);
}
