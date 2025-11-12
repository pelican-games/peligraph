#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(set = 0, binding = 0) uniform sampler2D texSampler;

layout(location = 0) in vec2 texUV;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex_sampled = texture(texSampler, texUV);
    outColor = vec4(tex_sampled.rgb * tex_sampled.a, 1.0) + vec4(inColor.rgb * inColor.a, 1.0);
}
