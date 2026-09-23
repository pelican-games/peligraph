#ifndef PT_PAYLOAD_GLSL
#define PT_PAYLOAD_GLSL

// Closest-hit shaders describe the surface; the raygen owns all shading.
// The material fields follow the Principled BSDF / glTF PBR vocabulary.
struct RadiancePayload {
    uint hit;
    vec3 position;
    vec3 normal;
    vec3 base_color;
    vec3 emission;
    float metallic;
    float roughness;
    float ior;
    float transmission;
};

struct ShadowPayload {
    uint occluded;
};

// Shader record shared by every hit group. Procedural spheres author these
// fields per asset in assets.json; triangle rows leave them zero.
#define PT_RECORD_MEMBERS \
    PELICAN_RAY_ENGINE_RECORD_MEMBERS \
    float radius; \
    vec3 center; \
    vec3 base_color; \
    vec3 emission; \
    float metallic; \
    float roughness; \
    float ior; \
    float transmission;

#endif
