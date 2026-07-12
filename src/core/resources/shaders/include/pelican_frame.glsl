#ifndef PELICAN_FRAME_GLSL
#define PELICAN_FRAME_GLSL

#include "pelican_sets.glsl"

layout(set = PELICAN_SET_FRAME, binding = PELICAN_FRAME_UBO_BINDING, std140) uniform PelicanFrameUBO {
    vec4 time_delta;
    uvec4 frame_index;
    vec4 resolution;
    vec4 camera_position;
    mat4 view;
    mat4 projection;
    mat4 previous_view;
    mat4 previous_projection;
} pelicanFrame;

struct PelicanObjectData {
    mat4 model;
};

layout(set = PELICAN_SET_FRAME, binding = PELICAN_OBJECT_BUFFER_BINDING, std430) readonly buffer PelicanObjectBuffer {
    PelicanObjectData objects[];
} pelicanObjects;

layout(set = PELICAN_SET_FRAME, binding = PELICAN_PREVIOUS_OBJECT_BUFFER_BINDING, std430) readonly buffer PelicanPreviousObjectBuffer {
    PelicanObjectData objects[];
} pelicanPreviousObjects;

struct PelicanDirectionalLight {
    vec3 direction;
    float intensity;
    vec3 color;
    float padding;
};

struct PelicanPointLight {
    vec3 position;
    float intensity;
    vec3 color;
    float padding;
};

struct PelicanSpotLight {
    vec3 position;
    float innerConeAngle;
    vec3 direction;
    float outerConeAngle;
    vec3 color;
    float intensity;
};

layout(set = PELICAN_SET_FRAME, binding = PELICAN_LIGHT_UBO_BINDING, std140) uniform PelicanLightUBO {
    uint directionalLightCount;
    uint pointLightCount;
    uint spotLightCount;
    float padding;
    PelicanDirectionalLight directionalLights[8];
    PelicanPointLight pointLights[16];
    PelicanSpotLight spotLights[8];
    mat4 shadowViewProjection;
} pelicanLights;

#endif
