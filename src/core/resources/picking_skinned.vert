#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_frame.glsl"
#include "pelican_skinning.glsl"
#include "pelican_morph.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 5) in ivec4 inJoints;
layout(location = 6) in vec4 inWeights;
layout(location = 0) flat out uint pickingId;

void main() {
    PelicanMorphedVertex current_morph =
        pelican_morph_vertex(inPos, vec3(0.0), vec3(0.0), gl_VertexIndex, false);
    vec4 local_position = pelican_skin_matrix(inJoints, inWeights) *
                          vec4(current_morph.position, 1.0);
    mat4 model = pelicanObjects.objects[gl_BaseInstance].model;
    gl_Position = pelicanFrame.projection * pelicanFrame.view * model *
                  local_position;
    pickingId = uint(gl_BaseInstance) + 1u;
}
