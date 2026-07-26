#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_GOOGLE_cpp_style_line_directive : enable

#include "pelican_material.glsl"
#define PELICAN_MATERIAL_INSTANCE_LOCATION 8
#include "pelican_material_instance.glsl"
#include "pelican_lighting_v1.glsl"
#define PELICAN_SURFACE_STAGE_FRAGMENT 1
#include "__pelican_surface_params.glsl"

layout(set = PELICAN_SET_MATERIAL, binding = 0) uniform sampler2D baseColorSampler;
layout(set = PELICAN_SET_MATERIAL, binding = 1) uniform sampler2D metallicRoughnessSampler;
layout(set = PELICAN_SET_MATERIAL, binding = 2) uniform sampler2D normalSampler;
layout(set = PELICAN_SET_MATERIAL, binding = 3) uniform sampler2D emissiveSampler;

#include "__pelican_user_surface.glsl"
#if !defined(PELICAN_HAS_BRDF_V1) && !defined(PELICAN_HAS_LIGHTING_V1)
#include "shaders/material/standard_lighting.glsl"
#endif

layout(location = 0) in vec2 texUV;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec3 inWorldPos;
layout(location = 4) in vec3 inTangent;
layout(location = 5) in vec3 inBitangent;
layout(location = 6) in vec4 inCustom0;
layout(location = 7) in vec4 inCustom1;

#ifdef PELICAN_PASS_FORWARD
layout(location = 0) out vec4 outColor;
#else
layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec4 outWorldPos;
layout(location = 4) out vec4 outEmissive;
#endif

void main() {
#ifdef PELICAN_PASS_DEPTH
    return;
#else
    PelicanMaterialData material = pelicanMaterials.materials[pelicanPush.materialIndex];
    vec2 materialUV = pelican_material_instance_uv(texUV);
    PelicanSurfaceInputV1 input_data;
    input_data.uv = materialUV;
    input_data.vertex_color = inColor;
    input_data.world_position = inWorldPos;
    input_data.normal = normalize(inNormal);
    input_data.view_direction = normalize(pelicanFrame.camera_position.xyz - inWorldPos);
    input_data.custom0 = inCustom0;
    input_data.custom1 = inCustom1;

    PelicanSurfaceV1 surface;
    surface.base_color = texture(baseColorSampler, materialUV) * inColor;
    surface.normal = input_data.normal;
    if (length(inTangent) != 0.0) {
        mat3 tbn = mat3(normalize(inTangent), normalize(inBitangent), input_data.normal);
        vec3 tangent_normal = texture(normalSampler, materialUV).xyz * 2.0 - 1.0;
        tangent_normal.xy *= material.surfaceFactors.z;
        surface.normal = normalize(tbn * normalize(tangent_normal));
    }
    vec3 mr = texture(metallicRoughnessSampler, materialUV).rgb;
    surface.roughness = mr.g * material.surfaceFactors.y;
    surface.metallic = mr.b * material.surfaceFactors.x;
    surface.occlusion = mix(1.0, mr.r, material.surfaceFactors.w);
    surface.emissive = texture(emissiveSampler, materialUV).rgb *
                       pelican_material_instance_emissive_source_factor(
                           material.emissiveFactor).rgb;
#ifdef PELICAN_HAS_SURFACE_V1
    pelican_surface_v1(input_data, surface);
#endif
    surface.base_color = pelican_material_instance_apply_base_color(surface.base_color);
    surface.emissive = pelican_material_instance_apply_emissive(surface.emissive);

#ifdef PELICAN_PASS_DEFERRED_GEOMETRY
    float shading_model = 0.0;
#ifdef PELICAN_GBUFFER_MODEL_OPENPBR_BASE_V1
    shading_model = 1.0 / 255.0;
#endif
    outAlbedo = surface.base_color;
    outNormal = vec4(surface.normal * 0.5 + 0.5, 1.0);
    outMaterial = vec4(surface.roughness, surface.metallic, surface.occlusion,
                       shading_model);
    outWorldPos = vec4(input_data.world_position, 1.0);
    outEmissive = vec4(surface.emissive, 1.0);
#else
    vec3 lit;
#ifdef PELICAN_HAS_LIGHTING_V1
    lit = pelican_lighting_v1(surface, input_data);
#elif defined(PELICAN_HAS_BRDF_V1)
    lit = surface.emissive;
    for (uint i = 0u; i < pelican_light_count(); ++i) {
        PelicanLightV1 light = pelican_light(i, input_data.world_position);
        lit += pelican_brdf_v1(surface, light.direction, input_data.view_direction,
                              light.radiance) * light.attenuation *
               pelican_shadow(i, input_data.world_position);
    }
#ifdef PELICAN_HAS_AMBIENT_V1
    lit += pelican_ambient_v1(surface, input_data.view_direction,
                             pelican_env_ambient(surface.normal));
#else
    lit += surface.base_color.rgb * pelican_env_ambient(surface.normal);
#endif
#else
    lit = pelican_lighting_v1(surface, input_data);
#endif

#ifdef PELICAN_PASS_FORWARD
    outColor = vec4(lit, surface.base_color.a);
#else
    outAlbedo = vec4(lit, surface.base_color.a);
    outNormal = vec4(surface.normal * 0.5 + 0.5, 1.0);
    outMaterial = vec4(surface.roughness, surface.metallic, surface.occlusion, 1.0);
    outWorldPos = vec4(input_data.world_position, 1.0);
    outEmissive = vec4(surface.emissive, 1.0);
#endif
#endif
#endif
}
