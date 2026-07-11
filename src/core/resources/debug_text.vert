#version 450
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"

struct DebugTextVertex {
    vec4 position;
    vec4 color;
    vec2 uv;
    vec2 _pad;
};

layout(set = PELICAN_SET_FREE, binding = 0, std430) readonly buffer DebugTextVertices {
    DebugTextVertex vertices[];
};

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outUV;

void main() {
    DebugTextVertex vertex = vertices[gl_VertexIndex];
    gl_Position = vertex.position;
    outColor = vertex.color;
    outUV = vertex.uv;
}
