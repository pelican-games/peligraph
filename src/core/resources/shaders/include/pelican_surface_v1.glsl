#ifndef PELICAN_SURFACE_V1_GLSL
#define PELICAN_SURFACE_V1_GLSL

// Frozen B-layer source ABI. New fields require PelicanSurface*V2 and adapter
// functions; these v1 layouts and meanings must not be changed.
struct PelicanVertexV1 {
    vec3 position;
    vec3 normal;
    vec4 custom0;
    vec4 custom1;
};

struct PelicanSurfaceInputV1 {
    vec2 uv;
    vec4 vertex_color;
    vec3 world_position;
    vec3 normal;
    vec3 view_direction;
    vec4 custom0;
    vec4 custom1;
};

struct PelicanSurfaceV1 {
    vec4 base_color;
    vec3 normal;
    float metallic;
    float roughness;
    float occlusion;
    vec3 emissive;
};

struct PelicanLightV1 {
    vec3 direction;
    vec3 radiance;
    float attenuation;
};

#endif
