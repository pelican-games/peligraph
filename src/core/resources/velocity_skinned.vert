#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_frame.glsl"
#include "pelican_skinning.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 5) in ivec4 inJoints;
layout(location = 6) in vec4 inWeights;
layout(location = 0) out vec4 currentClip;
layout(location = 1) out vec4 previousClip;

void main() {
    vec4 local_position = pelican_skin_matrix(inJoints, inWeights) * vec4(inPos, 1.0);
    mat4 current_model = pelicanObjects.objects[gl_BaseInstance].model;
    mat4 previous_model = pelicanPreviousObjects.objects[gl_BaseInstance].model;
    currentClip = pelicanFrame.projection * pelicanFrame.view * current_model * local_position;
    previousClip = pelicanFrame.previous_projection * pelicanFrame.previous_view *
                   previous_model * local_position;
    gl_Position = currentClip;
}
