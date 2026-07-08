#version 450

struct PelicanSurface {
    vec3 albedo;
    vec2 uv;
};

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void pelican_surface(inout PelicanSurface surface) {
}

void main() {
    PelicanSurface surface;
    surface.albedo = vec3(0.20, 0.20, 0.20);
    surface.uv = inUv;
    pelican_surface(surface);
    outColor = vec4(surface.albedo, 1.0);
}
