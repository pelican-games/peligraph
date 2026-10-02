#version 450

// Only gl_Position: the fragment shader reads gl_FragCoord.
out gl_PerVertex { vec4 gl_Position; };

// One triangle that covers the screen, (-1,-1) (3,-1) (-1,3); no vertex buffer.
void main() {
    gl_Position = vec4(float((gl_VertexIndex << 2) & 4) - 1.0, float((gl_VertexIndex << 1) & 4) - 1.0, 0.0, 1.0);
}
