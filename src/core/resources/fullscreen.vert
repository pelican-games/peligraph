#version 450
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"

layout(location = 0) out vec2 outUV;

void main() {
    // フルスクリーンクワッド（頂点バッファ不要）
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0 - 1.0, 0.0, 1.0);
#if defined(PELICAN_MULTIVIEW)
    // Keep gl_ViewIndex in the compiled vertex interface. The physical
    // pipeline reflection uses it as the final multiview capability gate.
    if (PELICAN_VIEW_INDEX >= uint(PELICAN_VIEW_COUNT)) {
        gl_Position = vec4(0.0);
    }
#endif
}
