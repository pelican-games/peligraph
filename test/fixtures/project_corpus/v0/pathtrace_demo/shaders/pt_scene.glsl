#ifndef PT_SCENE_GLSL
#define PT_SCENE_GLSL

#define PT_SKY_RADIANCE(direction) \
    mix(vec3(0.025, 0.04, 0.065), vec3(0.14, 0.23, 0.34), \
        clamp((direction).y * 2.0 + 0.5, 0.0, 1.0))

const vec3 PT_EMITTER_CENTER = vec3(0.0, 2.4, 7.0);
const float PT_EMITTER_RADIUS = 0.5;
const vec3 PT_EMITTER_RADIANCE = vec3(12.0);
const uint PT_MAX_BOUNCES = 8u;

#endif
