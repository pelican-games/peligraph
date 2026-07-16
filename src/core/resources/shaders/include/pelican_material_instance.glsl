#ifndef PELICAN_MATERIAL_INSTANCE_GLSL
#define PELICAN_MATERIAL_INSTANCE_GLSL

#include "pelican_sets.glsl"

#ifndef PELICAN_MATERIAL_INSTANCE_VERTEX_STAGE
#ifndef PELICAN_MATERIAL_INSTANCE_LOCATION
#error PELICAN_MATERIAL_INSTANCE_LOCATION must name the flat instance varying location
#endif
layout(location = PELICAN_MATERIAL_INSTANCE_LOCATION) flat in uint pelicanMaterialInstanceIndex;
#endif

#define PELICAN_MATERIAL_OVERRIDE_BASE_COLOR 1u
#define PELICAN_MATERIAL_OVERRIDE_EMISSIVE 2u
#define PELICAN_MATERIAL_OVERRIDE_UV_TRANSFORM 4u

struct PelicanMaterialInstanceOverrideBlock {
    vec4 baseColorFactor;
    vec4 emissiveFactor;
    vec4 uvOffsetScale;
    vec4 uvRotationReserved;
    uvec4 metadata;
};

struct PelicanMaterialInstanceAbsoluteOverrideHeader {
    uint recordOffset;
    uint recordCount;
    uint instanceGeneration;
    uint reserved;
};

struct PelicanMaterialInstanceAbsoluteOverrideBlock {
    vec4 baseColorFactor;
    vec4 emissiveFactor;
    vec4 uvOffsetScale;
    vec4 uvRotationReserved;
    // mask, source glTF material index, revision low, revision high
    uvec4 metadata;
};

#ifndef PELICAN_MATERIAL_INSTANCE_VERTEX_STAGE
layout(set = PELICAN_SET_FREE,
       binding = PELICAN_MATERIAL_INSTANCE_OVERRIDE_BINDING,
       std430) readonly buffer PelicanMaterialInstanceOverrideBuffer {
    PelicanMaterialInstanceOverrideBlock overrides[];
} pelicanMaterialInstanceOverrides;
#endif

layout(set = PELICAN_SET_FREE,
       binding = PELICAN_MATERIAL_INSTANCE_ABSOLUTE_HEADER_BINDING,
       std430) readonly buffer PelicanMaterialInstanceAbsoluteOverrideHeaderBuffer {
    PelicanMaterialInstanceAbsoluteOverrideHeader headers[];
} pelicanMaterialInstanceAbsoluteOverrideHeaders;

layout(set = PELICAN_SET_FREE,
       binding = PELICAN_MATERIAL_INSTANCE_ABSOLUTE_RECORD_BINDING,
       std430) readonly buffer PelicanMaterialInstanceAbsoluteOverrideRecordBuffer {
    PelicanMaterialInstanceAbsoluteOverrideBlock records[];
} pelicanMaterialInstanceAbsoluteOverrideRecords;

uint pelican_material_instance_index() {
#ifdef PELICAN_MATERIAL_INSTANCE_VERTEX_STAGE
    return gl_BaseInstance;
#else
    return pelicanMaterialInstanceIndex;
#endif
}

bool pelican_material_instance_absolute_override(
    out PelicanMaterialInstanceAbsoluteOverrideBlock result) {
    PelicanMaterialInstanceAbsoluteOverrideHeader header =
        pelicanMaterialInstanceAbsoluteOverrideHeaders.headers[
            pelican_material_instance_index()];
    for (uint index = 0u; index < header.recordCount; ++index) {
        PelicanMaterialInstanceAbsoluteOverrideBlock candidate =
            pelicanMaterialInstanceAbsoluteOverrideRecords.records[
                header.recordOffset + index];
        if (candidate.metadata.y == pelicanPush.sourceMaterialIndex) {
            result = candidate;
            return true;
        }
    }
    return false;
}

vec4 pelican_material_instance_base_color_source_factor(vec4 baseFactor) {
    PelicanMaterialInstanceAbsoluteOverrideBlock absoluteOverride;
    if (pelican_material_instance_absolute_override(absoluteOverride) &&
        (absoluteOverride.metadata.x & PELICAN_MATERIAL_OVERRIDE_BASE_COLOR) != 0u) {
        return absoluteOverride.baseColorFactor;
    }
    return baseFactor;
}

vec4 pelican_material_instance_emissive_source_factor(vec4 baseFactor) {
    PelicanMaterialInstanceAbsoluteOverrideBlock absoluteOverride;
    if (pelican_material_instance_absolute_override(absoluteOverride) &&
        (absoluteOverride.metadata.x & PELICAN_MATERIAL_OVERRIDE_EMISSIVE) != 0u) {
        return absoluteOverride.emissiveFactor;
    }
    return baseFactor;
}

#ifndef PELICAN_MATERIAL_INSTANCE_VERTEX_STAGE
uint pelican_material_instance_override_mask() {
    return pelicanMaterialInstanceOverrides
        .overrides[pelican_material_instance_index()].metadata.x;
}

vec4 pelican_material_instance_base_color_factor() {
    if ((pelican_material_instance_override_mask() &
         PELICAN_MATERIAL_OVERRIDE_BASE_COLOR) == 0u) {
        return vec4(1.0);
    }
    return pelicanMaterialInstanceOverrides
        .overrides[pelican_material_instance_index()].baseColorFactor;
}

vec4 pelican_material_instance_emissive_factor() {
    if ((pelican_material_instance_override_mask() &
         PELICAN_MATERIAL_OVERRIDE_EMISSIVE) == 0u) {
        return vec4(1.0);
    }
    return pelicanMaterialInstanceOverrides
        .overrides[pelican_material_instance_index()].emissiveFactor;
}

vec4 pelican_material_instance_apply_base_color(vec4 value) {
    PelicanMaterialInstanceAbsoluteOverrideBlock absoluteOverride;
    if (pelican_material_instance_absolute_override(absoluteOverride) &&
        (absoluteOverride.metadata.x & PELICAN_MATERIAL_OVERRIDE_BASE_COLOR) != 0u) {
        return value;
    }
    if ((pelican_material_instance_override_mask() &
         PELICAN_MATERIAL_OVERRIDE_BASE_COLOR) == 0u) {
        return value;
    }
    return value * pelican_material_instance_base_color_factor();
}

vec4 pelican_material_instance_apply_emissive(vec4 value) {
    PelicanMaterialInstanceAbsoluteOverrideBlock absoluteOverride;
    if (pelican_material_instance_absolute_override(absoluteOverride) &&
        (absoluteOverride.metadata.x & PELICAN_MATERIAL_OVERRIDE_EMISSIVE) != 0u) {
        return value;
    }
    if ((pelican_material_instance_override_mask() &
         PELICAN_MATERIAL_OVERRIDE_EMISSIVE) == 0u) {
        return value;
    }
    return value * pelican_material_instance_emissive_factor();
}

vec3 pelican_material_instance_apply_emissive(vec3 value) {
    PelicanMaterialInstanceAbsoluteOverrideBlock absoluteOverride;
    if (pelican_material_instance_absolute_override(absoluteOverride) &&
        (absoluteOverride.metadata.x & PELICAN_MATERIAL_OVERRIDE_EMISSIVE) != 0u) {
        return value;
    }
    if ((pelican_material_instance_override_mask() &
         PELICAN_MATERIAL_OVERRIDE_EMISSIVE) == 0u) {
        return value;
    }
    return value * pelican_material_instance_emissive_factor().rgb;
}

vec2 pelican_material_instance_transform_uv(
    vec2 value, vec4 offsetScale, float radians) {
    float cosine = cos(radians);
    float sine = sin(radians);
    mat2 rotation = mat2(cosine, sine, -sine, cosine);
    return offsetScale.xy + rotation * (value * offsetScale.zw);
}

vec2 pelican_material_instance_uv(vec2 value) {
    PelicanMaterialInstanceAbsoluteOverrideBlock absoluteOverride;
    if (pelican_material_instance_absolute_override(absoluteOverride) &&
        (absoluteOverride.metadata.x & PELICAN_MATERIAL_OVERRIDE_UV_TRANSFORM) !=
            0u) {
        return pelican_material_instance_transform_uv(
            value, absoluteOverride.uvOffsetScale,
            absoluteOverride.uvRotationReserved.x);
    }
    if ((pelican_material_instance_override_mask() &
         PELICAN_MATERIAL_OVERRIDE_UV_TRANSFORM) == 0u) {
        return value;
    }
    PelicanMaterialInstanceOverrideBlock block =
        pelicanMaterialInstanceOverrides
            .overrides[pelican_material_instance_index()];
    return pelican_material_instance_transform_uv(
        value, block.uvOffsetScale, block.uvRotationReserved.x);
}
#endif

#endif
