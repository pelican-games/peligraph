#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"
#include "pelican_features.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D albedoSampler;
layout(set = PELICAN_SET_PASS_INPUT, binding = 1) uniform sampler2D normalSampler;
layout(set = PELICAN_SET_PASS_INPUT, binding = 2) uniform sampler2D materialSampler; // R: roughness, G: metallic, B: AO
layout(set = PELICAN_SET_PASS_INPUT, binding = 3) uniform sampler2D worldPosSampler;
layout(set = PELICAN_SET_PASS_INPUT, binding = 4) uniform sampler2D emissiveSampler;
layout(set = PELICAN_SET_PASS_INPUT, binding = 5) uniform sampler2D ssaoSampler;
#ifdef PELICAN_FEATURE_SHADOW
layout(set = PELICAN_SET_PASS_INPUT, binding = 6) uniform sampler2D shadowMapSampler;
#endif

const float PI = 3.14159265359;

// GGX法線分布関数
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return a2 / max(denom, 0.0001);
}

// Smith幾何減衰関数（高さ相関バージョン - より正確）
float GeometrySchlickGGX(float NdotV, float roughness) {
    float a = roughness;
    float k = (a * a) / 2.0;  // direct lighting用
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

float OpenPbrDistributionGGX(vec3 N, vec3 H, float roughness) {
    float alpha = max(roughness * roughness, 0.0025);
    float alpha2 = alpha * alpha;
    float NdotH = max(dot(N, H), 0.0);
    float denominator = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(PI * denominator * denominator, 0.000001);
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

float brdfDenominator(vec3 N, vec3 V, vec3 L, bool openPbrBase) {
    float value = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0);
    return openPbrBase ? max(value, 0.000001) : value + 0.0001;
}

float distanceAttenuation(float distance, bool openPbrBase) {
    float distanceSquared = distance * distance;
    return openPbrBase ? 1.0 / max(distanceSquared, 0.0001)
                       : 1.0 / (distanceSquared + 0.01);
}

// Fresnel反射（Schlick近似）
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

#ifdef PELICAN_FEATURE_SHADOW
float directionalShadowVisibility(vec3 worldPos, vec3 normal, vec3 lightDir) {
    vec4 shadowClip = pelicanLights.shadowViewProjection * vec4(worldPos, 1.0);
    if (shadowClip.w <= 0.0) {
        return 1.0;
    }

    vec3 shadowNdc = shadowClip.xyz / shadowClip.w;
    vec2 shadowUv = shadowNdc.xy * 0.5 + 0.5;
    if (shadowUv.x < 0.0 || shadowUv.x > 1.0 || shadowUv.y < 0.0 || shadowUv.y > 1.0 ||
        shadowNdc.z < 0.0 || shadowNdc.z > 1.0) {
        return 1.0;
    }

    float storedDepth = texture(shadowMapSampler, shadowUv).r;
    float bias = max(0.0025 * (1.0 - dot(normal, lightDir)), 0.0008);
    return shadowNdc.z - bias <= storedDepth ? 1.0 : 0.35;
}
#endif

void main() {
    // G-bufferからデータを読み取る
    vec3 albedo = texture(albedoSampler, inUV).rgb;
    vec3 normal = normalize(texture(normalSampler, inUV).rgb * 2.0 - 1.0);
    vec4 materialSample = texture(materialSampler, inUV);
    vec3 material = materialSample.rgb;
    uint shadingModel = uint(round(materialSample.a * 255.0));
    bool openPbrBase = shadingModel == 1u;
    vec3 worldPos = texture(worldPosSampler, inUV).rgb;
    vec3 emissive = texture(emissiveSampler, inUV).rgb;
    
    float roughness = clamp(material.r, 0.04, 1.0);  // material.r = roughness
    float metallic = clamp(material.g, 0.0, 1.0);   // material.g = metallic
    float materialAO = material.b;

    // Sample SSAO
    float ssao = texture(ssaoSampler, inUV).r;
    
    // Combine material AO with SSAO
    // You can adjust the power to control the strength of the SSAO effect.
    // A higher power will make the shadows darker.
    float ao = min(materialAO, pow(ssao, 3.0));
    
    vec3 cameraPos = pelicanFrame.camera_position.xyz;
    
    vec3 V = normalize(cameraPos - worldPos);
    
    // F0（表面の基底反射率）- 金属はalbedo色、非金属は0.04
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);
    
    // 反射率方程式
    vec3 Lo = vec3(0.0);

    for(int i = 0; i < pelicanLights.directionalLightCount; ++i) {
        vec3 L = normalize(-pelicanLights.directionalLights[i].direction);
        vec3 H = normalize(V + L);
        vec3 radiance = pelicanLights.directionalLights[i].color * pelicanLights.directionalLights[i].intensity;
#ifdef PELICAN_FEATURE_SHADOW
        if (i == 0) {
            radiance *= directionalShadowVisibility(worldPos, normal, L);
        }
#endif
        
        // Cook-Torrance BRDF
        float NDF = openPbrBase ? OpenPbrDistributionGGX(normal, H, roughness)
                                : DistributionGGX(normal, H, roughness);
        float G = openPbrBase ? OpenPbrGeometrySmith(normal, V, L, roughness)
                              : GeometrySmith(normal, V, L, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
        
        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallic;
        
        vec3 numerator = NDF * G * F;
        float denominator = brdfDenominator(normal, V, L, openPbrBase);
        vec3 specular = numerator / denominator;
        
        float NdotL = max(dot(normal, L), 0.0);
        float directDiffuseOcclusion = openPbrBase ? 1.0 : ao;
        Lo += (kD * albedo / PI * directDiffuseOcclusion + specular) * radiance * NdotL;
    }

    for(int i = 0; i < pelicanLights.pointLightCount; ++i) {
        vec3 L = normalize(pelicanLights.pointLights[i].position - worldPos);
        vec3 H = normalize(V + L);
        float distance = length(pelicanLights.pointLights[i].position - worldPos);
        float attenuation = distanceAttenuation(distance, openPbrBase);
        vec3 radiance = pelicanLights.pointLights[i].color * pelicanLights.pointLights[i].intensity * attenuation;
        
        // Cook-Torrance BRDF
        float NDF = openPbrBase ? OpenPbrDistributionGGX(normal, H, roughness)
                                : DistributionGGX(normal, H, roughness);
        float G = openPbrBase ? OpenPbrGeometrySmith(normal, V, L, roughness)
                              : GeometrySmith(normal, V, L, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
        
        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallic;
        
        vec3 numerator = NDF * G * F;
        float denominator = brdfDenominator(normal, V, L, openPbrBase);
        vec3 specular = numerator / denominator;
        
        float NdotL = max(dot(normal, L), 0.0);
        float directDiffuseOcclusion = openPbrBase ? 1.0 : ao;
        Lo += (kD * albedo / PI * directDiffuseOcclusion + specular) * radiance * NdotL;
    }

    for(int i = 0; i < pelicanLights.spotLightCount; ++i) {
        vec3 L = normalize(pelicanLights.spotLights[i].position - worldPos);
        vec3 H = normalize(V + L);

        // Spotlight intensity calculation
        float spotDirDotL = dot(normalize(-L), normalize(pelicanLights.spotLights[i].direction));
        float spotFactor = smoothstep(pelicanLights.spotLights[i].outerConeAngle, pelicanLights.spotLights[i].innerConeAngle, spotDirDotL);
        
        // Attenuation and radiance
        float distance = length(pelicanLights.spotLights[i].position - worldPos);
        float attenuation = distanceAttenuation(distance, openPbrBase);
        vec3 radiance = pelicanLights.spotLights[i].color * pelicanLights.spotLights[i].intensity * attenuation * spotFactor;

        if (spotFactor > 0.0)
        {
            // Cook-Torrance BRDF
            float NDF = openPbrBase ? OpenPbrDistributionGGX(normal, H, roughness)
                                    : DistributionGGX(normal, H, roughness);
            float G = openPbrBase ? OpenPbrGeometrySmith(normal, V, L, roughness)
                                  : GeometrySmith(normal, V, L, roughness);
            vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
            
            vec3 kS = F;
            vec3 kD = vec3(1.0) - kS;
            kD *= 1.0 - metallic;
            
            vec3 numerator = NDF * G * F;
            float denominator = brdfDenominator(normal, V, L, openPbrBase);
            vec3 specular = numerator / denominator;
            
            float NdotL = max(dot(normal, L), 0.0);
            float directDiffuseOcclusion = openPbrBase ? 1.0 : ao;
            Lo += (kD * albedo / PI * directDiffuseOcclusion + specular) * radiance * NdotL;
        }
    }
    
    // 環境光をより充実させる
    float sky = clamp(normal.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 openPbrAmbient = albedo * (1.0 - metallic) * ao *
                          mix(vec3(0.015), vec3(0.06, 0.07, 0.09), sky);
    vec3 standardAmbient = mix(vec3(0.03) * albedo * ao,
                               albedo * 0.12 * ao, metallic);
    vec3 ambient = openPbrBase ? openPbrAmbient : standardAmbient;
    vec3 color = ambient + Lo + emissive;
    
#if !defined(PELICAN_FEATURE_HDR) && !defined(PELICAN_HYBRID_SCENE_LINEAR)
    // HDR off owns the single tone curve here. Transfer encoding is terminal-only.
    color = color / (color + vec3(0.155)) * 1.019;
#endif
    
    outColor = vec4(color, 1.0);
}
