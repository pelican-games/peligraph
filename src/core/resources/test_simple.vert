#version 450
#extension GL_ARB_separate_shader_objects : enable

void main() {
    if(gl_VertexIndex == 0) {
        gl_Position = vec4(0.0, -0.5, 0.0, 1.0);
    } else if(gl_VertexIndex == 1) {
        gl_Position = vec4(0.5, 0.5, 0.0, 1.0);
    } else if(gl_VertexIndex == 2) {
        gl_Position = vec4(-0.5, 0.5, 0.0, 1.0);
    }
}
