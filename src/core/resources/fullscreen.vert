#version 450
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"

layout(location = 0) out vec2 outUV;

void main() {
    // フルスクリーンクワッド（頂点バッファ不要）
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0 - 1.0, 0.0, 1.0);
}
