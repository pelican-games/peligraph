#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_frame.glsl"
#include "pelican_morph.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 0) out vec4 currentClip;
layout(location = 1) out vec4 previousClip;

void main() {
    PelicanMorphedVertex current_morph =
        pelican_morph_vertex(inPos, vec3(0.0), vec3(0.0), gl_VertexIndex, false);
    PelicanMorphedVertex previous_morph =
        pelican_morph_vertex(inPos, vec3(0.0), vec3(0.0), gl_VertexIndex, true);
    mat4 current_model = pelicanObjects.objects[gl_BaseInstance].model;
    mat4 previous_model = pelicanPreviousObjects.objects[gl_BaseInstance].model;
    currentClip = pelicanFrame.projection * pelicanFrame.view * current_model *
                  vec4(current_morph.position, 1.0);
    previousClip = pelicanFrame.previous_projection * pelicanFrame.previous_view *
                   previous_model * vec4(previous_morph.position, 1.0);
    gl_Position = currentClip;
}
