#ifndef PELICAN_OPENPBR_LIGHTING_GLSL
#define PELICAN_OPENPBR_LIGHTING_GLSL

// OpenPBR Surface 1.1.1 real-time subset.
// Exact specification pin: tag v1.1.1, commit
// f8d6d947dfae4c9b599965a86c22826ea7a8dbfb.
//
// This is an unprivileged B-layer lighting library: it uses only the public
// PelicanSurfaceV1/input types, generated param/texture accessors, and the
// pelican_light/shadow/env_ambient functions available to standard and toon.

const float PELICAN_OPENPBR_PI = 3.14159265358979323846;

float pelican_openpbr_saturate(float value) {
    return clamp(value, 0.0, 1.0);
}

float pelican_openpbr_scalar(float factor, vec4 sample_value) {
    return max(factor * sample_value.r, 0.0);
}

vec3 pelican_openpbr_color(vec4 factor, vec4 sample_value) {
    return max(factor.rgb * sample_value.rgb, vec3(0.0));
}

float pelican_openpbr_ior_f0(float ior) {
    float safe_ior = max(ior, 0.0001);
    float ratio = (safe_ior - 1.0) / (safe_ior + 1.0);
    return ratio * ratio;
}

vec3 pelican_openpbr_fresnel_schlick(float cos_theta, vec3 f0) {
    float grazing = pow(1.0 - pelican_openpbr_saturate(cos_theta), 5.0);
    return f0 + (vec3(1.0) - f0) * grazing;
}

float pelican_openpbr_ggx_distribution(float ndoth, float roughness) {
    float alpha = max(roughness * roughness, 0.0025);
    float alpha2 = alpha * alpha;
    float denominator = ndoth * ndoth * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(PELICAN_OPENPBR_PI * denominator * denominator, 0.000001);
}

float pelican_openpbr_smith_g1(float ndotx, float roughness) {
    float alpha = max(roughness * roughness, 0.0025);
    float alpha2 = alpha * alpha;
    return (2.0 * ndotx) /
           max(ndotx + sqrt(alpha2 + (1.0 - alpha2) * ndotx * ndotx), 0.000001);
}

float pelican_openpbr_smith_visibility(float ndotl, float ndotv, float roughness) {
    return pelican_openpbr_smith_g1(ndotl, roughness) *
           pelican_openpbr_smith_g1(ndotv, roughness);
}

mat3 pelican_openpbr_tangent_frame(vec3 normal, vec3 world_position, vec2 uv) {
    vec3 dpdx = dFdx(world_position);
    vec3 dpdy = dFdy(world_position);
    vec2 duvdx = dFdx(uv);
    vec2 duvdy = dFdy(uv);
    float determinant = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (abs(determinant) > 0.000001 && length(dpdx) > 0.000001 && length(dpdy) > 0.000001) {
        vec3 tangent = normalize((dpdx * duvdy.y - dpdy * duvdx.y) / determinant);
        tangent = normalize(tangent - normal * dot(normal, tangent));
        vec3 bitangent = normalize(cross(normal, tangent)) * sign(determinant);
        return mat3(tangent, bitangent, normal);
    }

    vec3 axis = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 tangent = normalize(cross(axis, normal));
    return mat3(tangent, cross(normal, tangent), normal);
}

vec3 pelican_openpbr_sample_normal(vec3 geometric_normal, vec3 world_position, vec2 uv,
                                   vec3 encoded_normal, float scale) {
    vec3 tangent_normal = encoded_normal * 2.0 - 1.0;
    tangent_normal.xy *= max(scale, 0.0);
    if (dot(tangent_normal, tangent_normal) < 0.000001) {
        tangent_normal = vec3(0.0, 0.0, 1.0);
    }
    return normalize(pelican_openpbr_tangent_frame(geometric_normal, world_position, uv) *
                     normalize(tangent_normal));
}

vec3 pelican_openpbr_oriented_normal(vec3 normal) {
#if PELICAN_OPENPBR_DOUBLE_SIDED
    return gl_FrontFacing ? normal : -normal;
#else
    return normal;
#endif
}

float pelican_openpbr_opacity(vec2 uv) {
    return pelican_openpbr_saturate(
        pelican_openpbr_scalar(pelican_param_geometry_opacity(),
                               pelican_sample_geometry_opacity_map(uv)));
}

void pelican_openpbr_surface_v1(in PelicanSurfaceInputV1 input_data,
                                inout PelicanSurfaceV1 surface) {
    vec2 uv = input_data.uv;
    float base_weight = pelican_openpbr_saturate(
        pelican_openpbr_scalar(pelican_param_base_weight(), pelican_sample_base_weight_map(uv)));
    vec3 base_color = pelican_openpbr_color(pelican_param_base_color(),
                                            pelican_sample_base_color_map(uv));
    float metalness = pelican_openpbr_saturate(
        pelican_openpbr_scalar(pelican_param_base_metalness(),
                               pelican_sample_base_metalness_map(uv)));
    float roughness = pelican_openpbr_saturate(
        pelican_openpbr_scalar(pelican_param_specular_roughness(),
                               pelican_sample_specular_roughness_map(uv)));
    float opacity = pelican_openpbr_opacity(uv);

    vec3 geometric_normal = pelican_openpbr_oriented_normal(normalize(input_data.normal));
    surface.normal = pelican_openpbr_sample_normal(
        geometric_normal, input_data.world_position, uv,
        pelican_sample_geometry_normal_map(uv).xyz,
        pelican_param_geometry_normal_scale());
    surface.base_color = vec4(base_color * base_weight,
#if PELICAN_OPENPBR_ALPHA_MODE == 2
                              opacity
#else
                              1.0
#endif
    );
    surface.metallic = metalness;
    surface.roughness = roughness;
    surface.occlusion = 1.0;
    surface.emissive =
        pelican_openpbr_color(pelican_param_emission_color(),
                              pelican_sample_emission_color_map(uv)) *
        pelican_openpbr_scalar(pelican_param_emission_luminance(),
                               pelican_sample_emission_luminance_map(uv));

#if PELICAN_OPENPBR_ALPHA_MODE == 1
    if (opacity < pelican_openpbr_saturate(pelican_param_alpha_cutoff())) {
        discard;
    }
#endif
}

vec3 pelican_openpbr_direct_lighting(in PelicanSurfaceV1 surface,
                                     in PelicanSurfaceInputV1 input_data,
                                     PelicanLightV1 light) {
    vec2 uv = input_data.uv;
    vec3 normal = normalize(surface.normal);
    vec3 view_direction = normalize(input_data.view_direction);
    vec3 light_direction = normalize(light.direction);
    vec3 half_direction = normalize(view_direction + light_direction);
    float ndotl = pelican_openpbr_saturate(dot(normal, light_direction));
    float ndotv = pelican_openpbr_saturate(dot(normal, view_direction));
    float ndoth = pelican_openpbr_saturate(dot(normal, half_direction));
    float vdoth = pelican_openpbr_saturate(dot(view_direction, half_direction));
    if (ndotl <= 0.0 || ndotv <= 0.0) {
        return vec3(0.0);
    }

    // The surface template applies surface-independent instance overrides before
    // lighting. Consume that canonical value here instead of resampling the
    // authored base color and bypassing the override.
    vec3 weighted_base_color = surface.base_color.rgb;
    float diffuse_roughness = pelican_openpbr_saturate(
        pelican_openpbr_scalar(pelican_param_base_diffuse_roughness(),
                               pelican_sample_base_diffuse_roughness_map(uv)));
    float metalness = pelican_openpbr_saturate(
        pelican_openpbr_scalar(pelican_param_base_metalness(),
                               pelican_sample_base_metalness_map(uv)));
    float specular_weight = pelican_openpbr_scalar(
        pelican_param_specular_weight(), pelican_sample_specular_weight_map(uv));
    vec3 specular_color = pelican_openpbr_color(pelican_param_specular_color(),
                                                pelican_sample_specular_color_map(uv));
    float specular_roughness = pelican_openpbr_saturate(
        pelican_openpbr_scalar(pelican_param_specular_roughness(),
                               pelican_sample_specular_roughness_map(uv)));
    float specular_ior = pelican_openpbr_scalar(
        pelican_param_specular_ior(), pelican_sample_specular_ior_map(uv));

    vec3 dielectric_f0 = vec3(pelican_openpbr_ior_f0(specular_ior)) *
                         specular_color * specular_weight;
    vec3 f0 = mix(dielectric_f0, weighted_base_color, metalness);
    vec3 fresnel = pelican_openpbr_fresnel_schlick(vdoth, f0);
    float distribution = pelican_openpbr_ggx_distribution(ndoth, specular_roughness);
    float visibility = pelican_openpbr_smith_visibility(ndotl, ndotv, specular_roughness);
    vec3 specular = fresnel * distribution * visibility /
                    max(4.0 * ndotl * ndotv, 0.000001);

    float oren_nayar_preview = 1.0 - 0.5 * diffuse_roughness;
    vec3 diffuse = weighted_base_color * (1.0 - metalness) *
                   (vec3(1.0) - fresnel) *
                   (oren_nayar_preview / PELICAN_OPENPBR_PI);

    float coat_weight = pelican_openpbr_saturate(
        pelican_openpbr_scalar(pelican_param_coat_weight(), pelican_sample_coat_weight_map(uv)));
    vec3 coat_color = pelican_openpbr_color(pelican_param_coat_color(),
                                            pelican_sample_coat_color_map(uv));
    float coat_roughness = pelican_openpbr_saturate(
        pelican_openpbr_scalar(pelican_param_coat_roughness(),
                               pelican_sample_coat_roughness_map(uv)));
    float coat_ior = pelican_openpbr_scalar(pelican_param_coat_ior(),
                                            pelican_sample_coat_ior_map(uv));
    float coat_darkening = pelican_openpbr_saturate(
        pelican_openpbr_scalar(pelican_param_coat_darkening(),
                               pelican_sample_coat_darkening_map(uv)));
    vec3 geometric_normal = pelican_openpbr_oriented_normal(normalize(input_data.normal));
    vec3 coat_normal = pelican_openpbr_sample_normal(
        geometric_normal, input_data.world_position, uv,
        pelican_sample_geometry_coat_normal_map(uv).xyz,
        pelican_param_geometry_coat_normal_scale());
    float coat_ndotl = pelican_openpbr_saturate(dot(coat_normal, light_direction));
    float coat_ndotv = pelican_openpbr_saturate(dot(coat_normal, view_direction));
    float coat_ndoth = pelican_openpbr_saturate(dot(coat_normal, half_direction));
    vec3 coat_f0 = vec3(pelican_openpbr_ior_f0(coat_ior)) * coat_color;
    vec3 coat_fresnel = pelican_openpbr_fresnel_schlick(vdoth, coat_f0) * coat_weight;
    float coat_distribution = pelican_openpbr_ggx_distribution(coat_ndoth, coat_roughness);
    float coat_visibility = pelican_openpbr_smith_visibility(coat_ndotl, coat_ndotv,
                                                             coat_roughness);
    vec3 coat = coat_fresnel * coat_distribution * coat_visibility /
                max(4.0 * coat_ndotl * coat_ndotv, 0.000001);

    vec3 base_layer = (diffuse + specular) * (vec3(1.0) - coat_fresnel) *
                      mix(1.0, coat_darkening, coat_weight);
    return (base_layer * ndotl + coat * coat_ndotl) * light.radiance * light.attenuation;
}

vec3 pelican_openpbr_lighting_v1(in PelicanSurfaceV1 surface,
                                 in PelicanSurfaceInputV1 input_data) {
    vec3 color = surface.emissive;
    vec3 ambient = pelican_env_ambient(surface.normal);
    // This feature is a solid visibility fallback, not an IBL approximation.
    // Keep metal visible until a directional environment BRDF is selected.
    color += surface.base_color.rgb * ambient;
    for (uint index = 0u; index < pelican_light_count(); ++index) {
        PelicanLightV1 light = pelican_light(index, input_data.world_position);
        color += pelican_openpbr_direct_lighting(surface, input_data, light) *
                 pelican_shadow(index, input_data.world_position);
    }
    return max(color, vec3(0.0));
}

#endif
