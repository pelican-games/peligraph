#version 460
#extension GL_GOOGLE_include_directive : require
#include "pelican_sets.glsl"
#include "pelican_frame.glsl"
// The scene lights come from the frame's light block (pelicanLights in
// pelican_frame.glsl). A project with clustered lighting still fills that
// block; the clustered light list needs resource ports this pass does not have.
#undef PELICAN_FEATURE_CLUSTERED_LIGHTING
#include "pelican_lighting_v1.glsl"

// Fragment shader of the fullscreen pass "deferred_lighting": deferred lighting.
//
// It reads a G-buffer by input position -- 0 albedo, 1 normal, 2 material
// (r roughness, g metallic, b occlusion), 3 world position, 4 emissive and,
// when the pass lists it, 5 ambient occlusion -- and lights every pixel with
// every scene light: Lambert diffuse and a Blinn-Phong highlight. It is a
// starting point to change, not the engine's own lighting (engine://fullscreen).
//
// The pass declares "uses_light_data": true and "push_constants":
// "camera_position" (Uses scene lights and Camera position in the Pass
// Editor), as the engine's lighting pass does.
//
// The directional light casts a shadow when the project has the
// shadow_directional feature and the pass lists shadow_map among its inputs
// (New... adds it then): the feature defines PELICAN_FEATURE_SHADOW for a pass
// that reads shadow_map, and the input is declared below as
// PELICAN_TEMPLATE_SHADOW_MAP. Without either, no light is shadowed.
//
// The input after the pass's inputs is an optional shadow mask
// (PELICAN_TEMPLATE_SHADOW_MASK_READ): an R8 image, 1 lit and 0 shadowed, such
// as engine://rt_shadow_mask_pipeline writes. Connect one and the key light
// (the first directional light) is shadowed by the smaller of the shadow map
// and the mask. Left unconnected, the binding reads zeros, alpha included,
// which is taken as no mask.
//
// The colour it writes is scene-linear and may be brighter than 1. Tone map it
// once before the display: a pass made with New... > Tone map does that.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;
PELICAN_DECLARE_INPUT_0(in_render_material_pass_albedo);  // render_material_pass__albedo: albedo
PELICAN_DECLARE_INPUT_1(in_render_material_pass_normal);  // render_material_pass__normal: normal
PELICAN_DECLARE_INPUT_2(in_render_material_pass_material);  // render_material_pass__material: material
PELICAN_DECLARE_INPUT_3(in_render_material_pass_worldpos);  // render_material_pass__worldpos: world position
PELICAN_DECLARE_INPUT_4(in_render_material_pass_emissive);  // render_material_pass__emissive: emissive
PELICAN_DECLARE_INPUT_5(in_shadow_map);  // shadow_map: the directional shadow map
#define PELICAN_TEMPLATE_SHADOW_MAP in_shadow_map
PELICAN_DECLARE_INPUT_6(in_shadow_mask);  // shadow_mask (optional): connect an R8 mask here, 1 lit, 0 shadowed
#define PELICAN_TEMPLATE_SHADOW_MASK_READ(uv) PELICAN_TEXTURE_2D_6(in_shadow_mask, uv)

#if defined(PELICAN_FEATURE_SHADOW) && defined(PELICAN_TEMPLATE_SHADOW_MAP)
// As the engine's lighting pass (engine://fullscreen) reads its shadow map:
// the cascade the point falls in, filtered with the feature's radius, bias
// and strength.
#include "pelican_shadow_filter.glsl"
vec4 sample_shadow_map(vec2 uv, uint layer, float lod) {
    return textureLod(PELICAN_TEMPLATE_SHADOW_MAP, vec3(uv, float(layer)), lod);
}
ivec2 shadow_map_size() { return textureSize(PELICAN_TEMPLATE_SHADOW_MAP, 0).xy; }
PELICAN_DEFINE_SHADOW_FILTER(filter_shadow_map, sample_shadow_map, shadow_map_size)
float directional_shadow(uint light_index, vec3 world_position) {
    vec3 shadow_ndc;
    vec2 shadow_uv;
    uint shadow_layer;
    if (light_index >= pelican_directional_light_count() ||
        !pelican_directional_shadow_projection(light_index, world_position, shadow_ndc,
                                               shadow_uv, shadow_layer))
        return 1.0;
    return filter_shadow_map(shadow_uv, shadow_layer, shadow_ndc.z,
                             PELICAN_FEATURE_SHADOW_DIRECTIONAL_PCF_RADIUS,
                             PELICAN_FEATURE_SHADOW_DIRECTIONAL_BIAS,
                             PELICAN_FEATURE_SHADOW_DIRECTIONAL_STRENGTH);
}
#else
float directional_shadow(uint light_index, vec3 world_position) { return 1.0; }
#endif

#if defined(PELICAN_TEMPLATE_SHADOW_MASK_READ)
// An image with an alpha of 1 (an R8 one reads 1 there) is a mask; the zeros
// an unconnected binding reads are not.
float shadow_mask(vec2 uv) {
    vec4 mask = PELICAN_TEMPLATE_SHADOW_MASK_READ(uv);
    return mask.a > 0.5 ? clamp(mask.r, 0.0, 1.0) : 1.0;
}
#else
float shadow_mask(vec2 uv) { return 1.0; }
#endif

void main() {
    vec3 albedo = PELICAN_TEXTURE_2D_0(in_render_material_pass_albedo, inUV).rgb;
    vec3 normal_encoded = PELICAN_TEXTURE_2D_1(in_render_material_pass_normal, inUV).rgb;
    vec4 material = PELICAN_TEXTURE_2D_2(in_render_material_pass_material, inUV);
    vec3 world_position = PELICAN_TEXTURE_2D_3(in_render_material_pass_worldpos, inUV).rgb;
    vec3 emissive = PELICAN_TEXTURE_2D_4(in_render_material_pass_emissive, inUV).rgb;
    float ambient_occlusion = 1.0;  // input 5 (ambient occlusion): the shadow map is there
    // The geometry pass leaves the normal at zero where it drew nothing.
    if (all(equal(normal_encoded, vec3(0.0)))) {
        outColor = vec4(emissive, 1.0);
        return;
    }

    vec3 normal = normalize(normal_encoded * 2.0 - 1.0);
    float roughness = clamp(material.r, 0.04, 1.0);
    float metallic = clamp(material.g, 0.0, 1.0);
    float occlusion = min(material.b, ambient_occlusion);
    vec3 to_camera = normalize(pelicanFrame.camera_position.xyz - world_position);

    vec3 diffuse_color = albedo * (1.0 - metallic);
    vec3 specular_color = mix(vec3(0.04), albedo, metallic);
    // A rough surface has a wide, dim highlight; a smooth one a small, bright one.
    float shininess = 2.0 / max(pow(roughness, 4.0), 0.0001) - 2.0;
    float highlight_scale = (shininess + 8.0) / (8.0 * PI);

    vec3 color = emissive + diffuse_color * occlusion * pelican_env_ambient(normal);
    for (uint index = 0u; index < pelican_light_count(); ++index) {
        PelicanLightV1 light = pelican_light(index, world_position);
        float n_dot_l = max(dot(normal, light.direction), 0.0);
        vec3 halfway = normalize(light.direction + to_camera);
        float highlight =
            highlight_scale * pow(max(dot(normal, halfway), 0.0), shininess);
        float shadow = directional_shadow(index, world_position);
        // The key light: the shadow map's shadow or the mask's, the darker.
        if (index == 0u && pelican_directional_light_count() > 0u)
            shadow = min(shadow, shadow_mask(inUV));
        vec3 radiance = light.radiance * light.attenuation * shadow;
        color += (diffuse_color / PI + specular_color * highlight) * radiance * n_dot_l;
    }
    outColor = vec4(color, 1.0);
}
