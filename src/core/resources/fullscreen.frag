#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"
#include "pelican_features.glsl"

// engine://fullscreen -- the engine's deferred lighting pass.
//
// Inputs (G-buffer, by position): 0 albedo, 1 normal (encoded to [0, 1]),
// 2 material (R roughness, G metallic, B material occlusion, A shading model),
// 3 world position, 4 emissive, 5 screen-space ambient occlusion and, with the
// directional shadow feature, 6 the cascade shadow map.
//
// Every light adds its direct reflection: a Lambert diffuse lobe for the part
// the specular lobe does not reflect, plus a microfacet specular lobe
//   f_spec = D(h) G(l, v) F(v.h) / (4 (n.l)(n.v))
// (Cook and Torrance 1982, in the form Walter et al. 2007 give it). Two
// shading models choose the D and G below; F is Schlick's for both:
//   0 standard: GGX D with alpha = roughness^2 and a separable Smith G built
//     from Schlick's G1 with k = alpha / 2 (Karis 2013)
//   1 OpenPBR base: GGX D and the exact separable Smith G1 for GGX
//     (Walter et al. 2007), with a lower alpha bound
// Ambient is the sky ambient feature's radiance times albedo and occlusion.
//
// References:
//   R. L. Cook, K. E. Torrance, "A Reflectance Model for Computer Graphics",
//     ACM TOG 1(1), 1982.
//   B. Walter, S. R. Marschner, H. Li, K. E. Torrance, "Microfacet Models for
//     Refraction through Rough Surfaces", EGSR 2007.
//   C. Schlick, "An Inexpensive BRDF Model for Physically-based Rendering",
//     Computer Graphics Forum 13(3), 1994.
//   B. Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013 course notes.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

PELICAN_DECLARE_INPUT_0(albedoSampler);
PELICAN_DECLARE_INPUT_1(normalSampler);
PELICAN_DECLARE_INPUT_2(materialSampler); // R: roughness, G: metallic, B: AO
PELICAN_DECLARE_INPUT_3(worldPosSampler);
PELICAN_DECLARE_INPUT_4(emissiveSampler);
PELICAN_DECLARE_INPUT_5(ssaoSampler);
#ifdef PELICAN_FEATURE_SHADOW
#include "pelican_shadow_cascade.glsl"
PELICAN_DECLARE_INPUT_6(shadowMapSampler);
#endif

#if defined(PELICAN_FEATURE_CLUSTERED_LIGHTING) || defined(PELICAN_FEATURE_POINT_SHADOW) || defined(PELICAN_FEATURE_SPOT_SHADOW)
#include "pelican_resource_ports.glsl"
#endif
#define PELICAN_PASS_DEFERRED_LIGHTING 1
#include "pelican_lighting_v1.glsl"

const float kPi = 3.141592653589793;

// Normal-incidence reflectance of a dielectric: ((1.5 - 1) / (1.5 + 1))^2 for
// an index of refraction of 1.5. A metal reflects its albedo instead.
const float kDielectricF0 = 0.04;

// Direct diffuse light is not darkened by occlusion: glTF's occlusion texture
// (and the SSAO estimate) attenuates indirect light only.
const float kDirectDiffuseOcclusion = 1.0;

// Guards of the standard shading model: the GGX denominator's floor and the
// bias added to the specular denominator 4 (n.l)(n.v).
const float kStandardNdfFloor = 0.0001;
const float kStandardSpecularBias = 0.0001;

// --- Shading model 0 (standard) ---------------------------------------------

// GGX / Trowbridge-Reitz distribution of microfacet normals:
//   D(h) = alpha^2 / (pi ((n.h)^2 (alpha^2 - 1) + 1)^2)
float standardGgxD(float cosNormalHalf, float alpha) {
    float alphaSquared = alpha * alpha;
    float t = cosNormalHalf * cosNormalHalf * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(kPi * t * t, kStandardNdfFloor);
}

// Schlick's masking term for one direction, G1(x) = x / (x (1 - k) + k).
float standardSchlickG1(float cosine, float k) {
    return cosine / (cosine * (1.0 - k) + k);
}

// Separable Smith shadowing-masking, G1(l) G1(v), with k = alpha / 2.
float standardSmithG(float cosNormalLight, float cosNormalView, float roughness) {
    float k = (roughness * roughness) / 2.0;
    return standardSchlickG1(cosNormalLight, k) * standardSchlickG1(cosNormalView, k);
}

// --- Shading model 1 (OpenPBR base) -----------------------------------------

float OpenPbrDistributionGGX(vec3 N, vec3 H, float roughness) {
    float alpha = max(roughness * roughness, 0.0025);
    float alpha2 = alpha * alpha;
    float NdotH = max(dot(N, H), 0.0);
    float denominator = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(kPi * denominator * denominator, 0.000001);
}

float OpenPbrSmithG1(float NdotX, float roughness) {
    float alpha = max(roughness * roughness, 0.0025);
    float alpha2 = alpha * alpha;
    return (2.0 * NdotX) /
           max(NdotX + sqrt(alpha2 + (1.0 - alpha2) * NdotX * NdotX), 0.000001);
}

float OpenPbrGeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return OpenPbrSmithG1(NdotL, roughness) * OpenPbrSmithG1(NdotV, roughness);
}

// --- Shared by both models --------------------------------------------------

// Schlick's approximation of the Fresnel reflectance at the angle whose
// cosine is given: F0 + (1 - F0)(1 - cos)^5.
vec3 schlickFresnel(float cosine, vec3 normalIncidence) {
    return normalIncidence +
           (1.0 - normalIncidence) * pow(clamp(1.0 - cosine, 0.0, 1.0), 5.0);
}

// 4 (n.l)(n.v), the microfacet specular denominator, kept away from zero.
float specularDenominator(float cosNormalLight, float cosNormalView, bool openPbrBase) {
    float value = 4.0 * cosNormalView * cosNormalLight;
    return openPbrBase ? max(value, 0.000001) : value + kStandardSpecularBias;
}

// Inverse-square falloff of a point or spot light, softened near the light.
float distanceAttenuation(float distance, bool openPbrBase) {
    float distanceSquared = distance * distance;
    return openPbrBase ? 1.0 / max(distanceSquared, 0.0001)
                       : 1.0 / (distanceSquared + 0.01);
}

// What the lights see of the pixel's surface.
struct LitSurface {
    vec3 normal;
    vec3 toCamera;
    vec3 albedo;
    float roughness;
    float metallic;
    vec3 normalIncidence;  // Fresnel F0
    bool openPbrBase;
};

// Radiance reflected toward the camera from light that arrives along the unit
// direction toLight with the given radiance (shadows already applied).
vec3 reflectedDirectLight(LitSurface surface, vec3 toLight, vec3 radiance) {
    vec3 halfway = normalize(surface.toCamera + toLight);
    float cosNormalView = max(dot(surface.normal, surface.toCamera), 0.0);
    float cosNormalLight = max(dot(surface.normal, toLight), 0.0);

    float distribution = surface.openPbrBase
        ? OpenPbrDistributionGGX(surface.normal, halfway, surface.roughness)
        : standardGgxD(max(dot(surface.normal, halfway), 0.0),
                       surface.roughness * surface.roughness);
    float visibility = surface.openPbrBase
        ? OpenPbrGeometrySmith(surface.normal, surface.toCamera, toLight, surface.roughness)
        : standardSmithG(cosNormalLight, cosNormalView, surface.roughness);
    vec3 fresnel = schlickFresnel(max(dot(halfway, surface.toCamera), 0.0),
                                  surface.normalIncidence);

    vec3 specular = distribution * visibility * fresnel /
                    specularDenominator(cosNormalLight, cosNormalView, surface.openPbrBase);
    // Energy the specular lobe does not reflect is diffused; metals diffuse none.
    vec3 diffuseShare = (vec3(1.0) - fresnel) * (1.0 - surface.metallic);
    vec3 diffuse = diffuseShare * surface.albedo / kPi * kDirectDiffuseOcclusion;
    return (diffuse + specular) * radiance * cosNormalLight;
}

#ifdef PELICAN_FEATURE_SHADOW
#include "pelican_shadow_filter.glsl"
vec4 pelican_sample_deferred_shadow(vec2 uv, uint layer, float lod) {
    return textureLod(shadowMapSampler, vec3(uv, float(layer)), lod);
}
ivec2 pelican_size_deferred_shadow() { return textureSize(shadowMapSampler, 0).xy; }
PELICAN_DEFINE_SHADOW_FILTER(pelican_deferred_shadow_filter,
    pelican_sample_deferred_shadow, pelican_size_deferred_shadow)
float directionalShadowVisibility(
    uint lightIndex,
    vec3 worldPos,
    vec3 normal,
    vec3 lightDir) {
    vec3 shadowNdc;
    vec2 shadowUv;
    uint shadowLayer;
    if (!pelican_directional_shadow_projection(
            lightIndex, worldPos,
            shadowNdc, shadowUv,
            shadowLayer)) {
        return 1.0;
    }

    return pelican_deferred_shadow_filter(shadowUv, shadowLayer, shadowNdc.z,
        PELICAN_FEATURE_SHADOW_DIRECTIONAL_PCF_RADIUS,
        PELICAN_FEATURE_SHADOW_DIRECTIONAL_BIAS,
        PELICAN_FEATURE_SHADOW_DIRECTIONAL_STRENGTH);
}
#endif

void main() {
    vec3 albedo = PELICAN_TEXTURE_2D_0(albedoSampler, inUV).rgb;
    vec3 normal = normalize(PELICAN_TEXTURE_2D_1(normalSampler, inUV).rgb * 2.0 - 1.0);
    vec4 materialTexel = PELICAN_TEXTURE_2D_2(materialSampler, inUV);
    vec3 worldPos = PELICAN_TEXTURE_2D_3(worldPosSampler, inUV).rgb;
    vec3 emissive = PELICAN_TEXTURE_2D_4(emissiveSampler, inUV).rgb;

    LitSurface surface;
    surface.normal = normal;
    surface.toCamera = normalize(pelicanFrame.camera_position.xyz - worldPos);
    surface.albedo = albedo;
    surface.roughness = clamp(materialTexel.r, 0.04, 1.0);
    surface.metallic = clamp(materialTexel.g, 0.0, 1.0);
    surface.normalIncidence = mix(vec3(kDielectricF0), albedo, surface.metallic);
    surface.openPbrBase = uint(round(materialTexel.a * 255.0)) == 1u;

    // Occlusion of the ambient term: the material's own, or the screen-space
    // estimate raised to the third power to deepen it, whichever is darker.
    float screenSpaceOpen = PELICAN_TEXTURE_2D_5(ssaoSampler, inUV).r;
    float ambientOcclusion = min(materialTexel.b, pow(screenSpaceOpen, 3.0));

    vec3 direct = vec3(0.0);

#ifdef PELICAN_FEATURE_CLUSTERED_LIGHTING
    uint selectedLightCount = pelican_light_count();
    for (uint i = 0u; i < selectedLightCount; ++i) {
        PelicanLightV1 light = pelican_light(i, worldPos);
        vec3 radiance = light.radiance * light.attenuation;
#if defined(PELICAN_FEATURE_POINT_SHADOW) || defined(PELICAN_FEATURE_SPOT_SHADOW)
        radiance *= pelican_local_shadow_visibility(i, worldPos);
#endif
#ifdef PELICAN_FEATURE_SHADOW
        radiance *= directionalShadowVisibility(i, worldPos, normal, light.direction);
#endif
        direct += reflectedDirectLight(surface, light.direction, radiance);
    }
#else
    for (int i = 0; i < pelicanLights.directionalLightCount; ++i) {
        vec3 toLight = normalize(-pelicanLights.directionalLights[i].direction);
        vec3 radiance = pelicanLights.directionalLights[i].color *
                        pelicanLights.directionalLights[i].intensity;
#ifdef PELICAN_FEATURE_SHADOW
        radiance *= directionalShadowVisibility(uint(i), worldPos, normal, toLight);
#endif
        direct += reflectedDirectLight(surface, toLight, radiance);
    }

    for (int i = 0; i < pelicanLights.pointLightCount; ++i) {
        vec3 toLight = normalize(pelicanLights.pointLights[i].position - worldPos);
        float lightDistance = length(pelicanLights.pointLights[i].position - worldPos);
        vec3 radiance = pelicanLights.pointLights[i].color *
                        pelicanLights.pointLights[i].intensity *
                        distanceAttenuation(lightDistance, surface.openPbrBase);
#ifdef PELICAN_FEATURE_POINT_SHADOW
        radiance *= pelican_point_shadow_visibility(uint(i), worldPos);
#endif
        direct += reflectedDirectLight(surface, toLight, radiance);
    }

    for (int i = 0; i < pelicanLights.spotLightCount; ++i) {
        vec3 toLight = normalize(pelicanLights.spotLights[i].position - worldPos);
        // The cone: full inside innerConeAngle, none outside outerConeAngle
        // (both stored as cosines), smooth between.
        float cosFromAxis = dot(normalize(-toLight),
                                normalize(pelicanLights.spotLights[i].direction));
        float cone = smoothstep(pelicanLights.spotLights[i].outerConeAngle,
                                pelicanLights.spotLights[i].innerConeAngle,
                                cosFromAxis);
        float lightDistance = length(pelicanLights.spotLights[i].position - worldPos);
        vec3 radiance = pelicanLights.spotLights[i].color *
                        pelicanLights.spotLights[i].intensity *
                        distanceAttenuation(lightDistance, surface.openPbrBase) *
                        cone;
#ifdef PELICAN_FEATURE_SPOT_SHADOW
        radiance *= pelican_spot_shadow_visibility(uint(i), worldPos);
#endif
        if (cone > 0.0) {
            direct += reflectedDirectLight(surface, toLight, radiance);
        }
    }
#endif

    vec3 ambientRadiance = vec3(0.0);
#ifdef PELICAN_FEATURE_SKY_AMBIENT
    ambientRadiance = pelicanLights.environmentAmbientRadiance.rgb;
#endif
    // The solid fallback intentionally lights metals as well. Directional
    // environment response belongs to the later IBL feature.
    vec3 color = albedo * ambientOcclusion * ambientRadiance + direct + emissive;

#if !defined(PELICAN_FEATURE_HDR) && !defined(PELICAN_HYBRID_SCENE_LINEAR)
    // HDR off owns the single tone curve here. Transfer encoding is terminal-only.
    color = color / (color + vec3(0.155)) * 1.019;
#endif

    outColor = vec4(color, 1.0);
}
