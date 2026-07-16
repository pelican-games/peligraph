#ifndef PELICAN_MATERIAL_INSTANCE_GLSL
#define PELICAN_MATERIAL_INSTANCE_GLSL

#include "pelican_sets.glsl"

#ifndef PELICAN_MATERIAL_INSTANCE_LOCATION
#error PELICAN_MATERIAL_INSTANCE_LOCATION must name the flat instance varying location
#endif

#define PELICAN_MATERIAL_OVERRIDE_BASE_COLOR 1u
#define PELICAN_MATERIAL_OVERRIDE_EMISSIVE 2u
#define PELICAN_MATERIAL_OVERRIDE_UV_TRANSFORM 4u

layout(location = PELICAN_MATERIAL_INSTANCE_LOCATION) flat in uint pelicanMaterialInstanceIndex;

struct PelicanMaterialInstanceOverrideBlock {
    vec4 baseColorFactor;
    vec4 emissiveFactor;
    vec4 uvOffsetScale;
    vec4 uvRotationReserved;
    uvec4 metadata;
};

layout(set = PELICAN_SET_FREE,
       binding = PELICAN_MATERIAL_INSTANCE_OVERRIDE_BINDING,
       std430) readonly buffer PelicanMaterialInstanceOverrideBuffer {
    PelicanMaterialInstanceOverrideBlock overrides[];
} pelicanMaterialInstanceOverrides;

uint pelican_material_instance_override_mask() {
    return pelicanMaterialInstanceOverrides.overrides[pelicanMaterialInstanceIndex].metadata.x;
}

vec4 pelican_material_instance_base_color_factor() {
    if ((pelican_material_instance_override_mask() & PELICAN_MATERIAL_OVERRIDE_BASE_COLOR) == 0u) {
        return vec4(1.0);
    }
    return pelicanMaterialInstanceOverrides.overrides[pelicanMaterialInstanceIndex].baseColorFactor;
}

vec4 pelican_material_instance_emissive_factor() {
    if ((pelican_material_instance_override_mask() & PELICAN_MATERIAL_OVERRIDE_EMISSIVE) == 0u) {
        return vec4(1.0);
    }
    return pelicanMaterialInstanceOverrides.overrides[pelicanMaterialInstanceIndex].emissiveFactor;
}

vec4 pelican_material_instance_apply_base_color(vec4 value) {
    if ((pelican_material_instance_override_mask() & PELICAN_MATERIAL_OVERRIDE_BASE_COLOR) == 0u) {
        return value;
    }
    return value * pelican_material_instance_base_color_factor();
}

vec4 pelican_material_instance_apply_emissive(vec4 value) {
    if ((pelican_material_instance_override_mask() & PELICAN_MATERIAL_OVERRIDE_EMISSIVE) == 0u) {
        return value;
    }
    return value * pelican_material_instance_emissive_factor();
}

vec3 pelican_material_instance_apply_emissive(vec3 value) {
    if ((pelican_material_instance_override_mask() & PELICAN_MATERIAL_OVERRIDE_EMISSIVE) == 0u) {
        return value;
    }
    return value * pelican_material_instance_emissive_factor().rgb;
}

vec2 pelican_material_instance_uv(vec2 value) {
    if ((pelican_material_instance_override_mask() & PELICAN_MATERIAL_OVERRIDE_UV_TRANSFORM) == 0u) {
        return value;
    }
    PelicanMaterialInstanceOverrideBlock block =
        pelicanMaterialInstanceOverrides.overrides[pelicanMaterialInstanceIndex];
    float cosine = cos(block.uvRotationReserved.x);
    float sine = sin(block.uvRotationReserved.x);
    mat2 rotation = mat2(cosine, sine, -sine, cosine);
    return block.uvOffsetScale.xy + rotation * (value * block.uvOffsetScale.zw);
}

#endif
