#version 450
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"

struct GizmoVertex {
    vec4 position;
    vec4 color;
};

layout(set = PELICAN_SET_FREE, binding = 0, std430) readonly buffer GizmoVertices {
    GizmoVertex vertices[];
};

layout(location = 0) out vec4 outColor;

void main() {
    GizmoVertex vertex = vertices[gl_VertexIndex];
    gl_Position = vertex.position;
    outColor = vertex.color;
}
