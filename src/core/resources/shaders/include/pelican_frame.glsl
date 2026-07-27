#ifndef PELICAN_FRAME_GLSL
#define PELICAN_FRAME_GLSL

#include "pelican_sets.glsl"
#include "pelican_view.glsl"

#if defined(PELICAN_MULTIVIEW)
struct PelicanFrameData {
    vec4 time_delta;
    uvec4 frame_index;
    vec4 resolution;
    vec4 camera_position;
    mat4 view;
    mat4 projection;
    mat4 previous_view;
    mat4 previous_projection;
    vec2 jitter_ndc;
    vec2 previous_jitter_ndc;
    uint temporal_reset_epoch;
    uint previous_temporal_reset_epoch;
    uint view_index;
    uint view_count;
};

layout(set = PELICAN_SET_FRAME, binding = PELICAN_FRAME_UBO_BINDING, std140) uniform PelicanFrameUBO {
    PelicanFrameData views[PELICAN_VIEW_COUNT];
} pelicanFrames;

#define pelicanFrame pelicanFrames.views[gl_ViewIndex]
#else
layout(set = PELICAN_SET_FRAME, binding = PELICAN_FRAME_UBO_BINDING, std140) uniform PelicanFrameUBO {
    vec4 time_delta;
    uvec4 frame_index;
    vec4 resolution;
    vec4 camera_position;
    mat4 view;
    mat4 projection;
    mat4 previous_view;
    mat4 previous_projection;
    vec2 jitter_ndc;
    vec2 previous_jitter_ndc;
    uint temporal_reset_epoch;
    uint previous_temporal_reset_epoch;
    uint view_index;
    uint view_count;
} pelicanFrame;
#endif

uint pelican_view_index() {
#if defined(PELICAN_MULTIVIEW)
    return uint(gl_ViewIndex);
#else
    return pelicanFrame.view_index;
#endif
}

uint pelican_view_count() {
    return pelicanFrame.view_count;
}

struct PelicanResolutionData {
    vec4 render_resolution;
    vec4 output_resolution;
};

#if defined(PELICAN_MULTIVIEW)
layout(set = PELICAN_SET_FRAME, binding = PELICAN_FRAME_RESOLUTION_UBO_BINDING, std140) uniform PelicanResolutionUBO {
    PelicanResolutionData views[PELICAN_VIEW_COUNT];
} pelicanResolutions;

#define pelicanResolution pelicanResolutions.views[gl_ViewIndex]
#else
layout(set = PELICAN_SET_FRAME, binding = PELICAN_FRAME_RESOLUTION_UBO_BINDING, std140) uniform PelicanResolutionUBO {
    vec4 render_resolution;
    vec4 output_resolution;
} pelicanResolution;
#endif

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
