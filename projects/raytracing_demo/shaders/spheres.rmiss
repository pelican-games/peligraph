#version 460
#extension GL_EXT_ray_tracing : require
layout(location=0) rayPayloadInEXT vec3 color;
void main() {
    float elevation = clamp(gl_WorldRayDirectionEXT.y * 2.0 + 0.5, 0.0, 1.0);
    color = mix(vec3(0.025, 0.04, 0.065), vec3(0.14, 0.23, 0.34), elevation);
}
