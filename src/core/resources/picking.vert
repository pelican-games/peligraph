#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_frame.glsl"
#include "pelican_morph.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 0) flat out uint pickingId;

void main() {
    PelicanMorphedVertex current_morph =
        pelican_morph_vertex(inPos, vec3(0.0), vec3(0.0), gl_VertexIndex, false);
    mat4 model = pelicanObjects.objects[gl_BaseInstance].model;
    gl_Position = pelicanFrame.projection * pelicanFrame.view * model *
                  vec4(current_morph.position, 1.0);
    // Zero is the clear/background sentinel. firstInstance is the live model
    // slot by draw-queue contract, so slot N is encoded as N + 1.
    pickingId = uint(gl_BaseInstance) + 1u;
}
