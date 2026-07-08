#version 450

struct PelicanSurface {
    vec3 albedo;
    vec2 uv;
};

layout(set = 2, binding = 0) uniform sampler2D userTexture;
layout(location = 0) out vec4 dummyOut;

void pelican_surface(inout PelicanSurface surface) {
    vec3 texel = texture(userTexture, surface.uv).rgb;
#ifdef PELICAN_VARIANT_WARM
    surface.albedo = surface.albedo * vec3(1.0, 0.78, 0.50) + texel * 0.25;
#else
    surface.albedo = surface.albedo * vec3(0.45, 0.70, 1.0) + texel * 0.15;
#endif
}

void main() {
    PelicanSurface surface;
    surface.albedo = vec3(0.0);
    surface.uv = vec2(0.0);
    pelican_surface(surface);
    dummyOut = vec4(surface.albedo, 1.0);
}
