#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"
#include "pelican_morph.glsl"

layout(push_constant) uniform SceneData {
    layout(offset = 0) mat4 vpMatrix;
} drawInfo;

layout(location = 0) in vec3 inPos;

void main() {
    PelicanMorphedVertex morphed =
        pelican_morph_vertex(inPos, vec3(0.0), vec3(0.0), gl_VertexIndex, false);
    mat4 model_matrix = pelicanObjects.objects[gl_BaseInstance].model;
    gl_Position = drawInfo.vpMatrix * model_matrix * vec4(morphed.position, 1.0);
}
