#version 460

layout(push_constant) uniform UiPush {
    vec3 pos;   // normalized position (0-1, top-left origin), z for depth
    float _pad1;
    vec2 size;  // normalized size relative to framebuffer
    float _pad2;
} pushConsts;

layout(location = 0) out vec2 outUV;

vec2 quadVerts(int idx) {
    // Triangle list covering a unit quad
    if (idx == 0) return vec2(0.0, 0.0);
    if (idx == 1) return vec2(1.0, 0.0);
    if (idx == 2) return vec2(0.0, 1.0);
    if (idx == 3) return vec2(0.0, 1.0);
    if (idx == 4) return vec2(1.0, 0.0);
    return vec2(1.0, 1.0);
}

void main() {
    vec2 uv = quadVerts(int(gl_VertexIndex));
    // Convert to clip space. Y is flipped to treat pos as top-left origin.
    // Orthographic projection: pos=(0,0) is top-left, z is depth (0-1)
    vec2 posNorm = pushConsts.pos.xy + uv * pushConsts.size;
    // Vulkan clip space: X: -1 (left) to 1 (right), Y: -1 (top) to 1 (bottom)
    vec2 clipPos = vec2(posNorm.x * 2.0 - 1.0, posNorm.y * 2.0 - 1.0);
    // Z is mapped to clip space: [0-1] -> [0-1] (preserves depth order)
    float clipZ = pushConsts.pos.z;

    gl_Position = vec4(clipPos, clipZ, 1.0);
    outUV = uv;
}