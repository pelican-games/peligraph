#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
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

struct DirectionalLight {
    vec3 direction;
    float intensity;
    vec3 color;
    float padding; // for alignment
};

struct PointLight {
    vec3 position;
    float intensity;
    vec3 color;
    float padding; // for alignment
};

struct SpotLight {
	vec3 position;
    float innerConeAngle; // cos(angle)
    vec3 direction;
    float outerConeAngle; // cos(angle)
    vec3 color;
    float intensity;
};

layout(set = PELICAN_SET_FRAME, binding = 0) uniform LightUBO {
    uint directionalLightCount;
    uint pointLightCount;
    uint spotLightCount;
    float padding;
    DirectionalLight directionalLights[8];
    PointLight pointLights[16];
    SpotLight spotLights[8];
    mat4 shadowViewProjection;
} lightUBO;

layout(push_constant) uniform PushConstants {
    vec4 cameraPos;
} pushConsts;

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

// Fresnel反射（Schlick近似）
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

#ifdef PELICAN_FEATURE_SHADOW
float directionalShadowVisibility(vec3 worldPos, vec3 normal, vec3 lightDir) {
    vec4 shadowClip = lightUBO.shadowViewProjection * vec4(worldPos, 1.0);
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
    vec3 material = texture(materialSampler, inUV).rgb;
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
    
    vec3 cameraPos = pushConsts.cameraPos.xyz;
    
    vec3 V = normalize(cameraPos - worldPos);
    
    // F0（表面の基底反射率）- 金属はalbedo色、非金属は0.04
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);
    
    // 反射率方程式
    vec3 Lo = vec3(0.0);

    for(int i = 0; i < lightUBO.directionalLightCount; ++i) {
        vec3 L = normalize(-lightUBO.directionalLights[i].direction);
        vec3 H = normalize(V + L);
        vec3 radiance = lightUBO.directionalLights[i].color * lightUBO.directionalLights[i].intensity;
#ifdef PELICAN_FEATURE_SHADOW
        if (i == 0) {
            radiance *= directionalShadowVisibility(worldPos, normal, L);
        }
#endif
        
        // Cook-Torrance BRDF
        float NDF = DistributionGGX(normal, H, roughness);
        float G = GeometrySmith(normal, V, L, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
        
        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallic;
        
        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(normal, V), 0.0) * max(dot(normal, L), 0.0) + 0.0001;
        vec3 specular = numerator / denominator;
        
        float NdotL = max(dot(normal, L), 0.0);
        Lo += (kD * albedo / PI * ao + specular) * radiance * NdotL;
    }

    for(int i = 0; i < lightUBO.pointLightCount; ++i) {
        vec3 L = normalize(lightUBO.pointLights[i].position - worldPos);
        vec3 H = normalize(V + L);
        float distance = length(lightUBO.pointLights[i].position - worldPos);
        float attenuation = 1.0 / (distance * distance + 0.01);
        vec3 radiance = lightUBO.pointLights[i].color * lightUBO.pointLights[i].intensity * attenuation;
        
        // Cook-Torrance BRDF
        float NDF = DistributionGGX(normal, H, roughness);
        float G = GeometrySmith(normal, V, L, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
        
        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallic;
        
        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(normal, V), 0.0) * max(dot(normal, L), 0.0) + 0.0001;
        vec3 specular = numerator / denominator;
        
        float NdotL = max(dot(normal, L), 0.0);
        Lo += (kD * albedo / PI * ao + specular) * radiance * NdotL;
    }

    for(int i = 0; i < lightUBO.spotLightCount; ++i) {
        vec3 L = normalize(lightUBO.spotLights[i].position - worldPos);
        vec3 H = normalize(V + L);

        // Spotlight intensity calculation
        float spotDirDotL = dot(normalize(-L), normalize(lightUBO.spotLights[i].direction));
        float spotFactor = smoothstep(lightUBO.spotLights[i].outerConeAngle, lightUBO.spotLights[i].innerConeAngle, spotDirDotL);
        
        // Attenuation and radiance
        float distance = length(lightUBO.spotLights[i].position - worldPos);
        float attenuation = 1.0 / (distance * distance + 0.01);
        vec3 radiance = lightUBO.spotLights[i].color * lightUBO.spotLights[i].intensity * attenuation * spotFactor;

        if (spotFactor > 0.0)
        {
            // Cook-Torrance BRDF
            float NDF = DistributionGGX(normal, H, roughness);
            float G = GeometrySmith(normal, V, L, roughness);
            vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
            
            vec3 kS = F;
            vec3 kD = vec3(1.0) - kS;
            kD *= 1.0 - metallic;
            
            vec3 numerator = NDF * G * F;
            float denominator = 4.0 * max(dot(normal, V), 0.0) * max(dot(normal, L), 0.0) + 0.0001;
            vec3 specular = numerator / denominator;
            
            float NdotL = max(dot(normal, L), 0.0);
            Lo += (kD * albedo / PI * ao + specular) * radiance * NdotL;
        }
    }
    
    // 環境光をより充実させる
    vec3 ambient = mix(vec3(0.03) * albedo * ao, albedo * 0.12 * ao, metallic);
    vec3 color = ambient + Lo + emissive;
    
    // HDRトーンマッピング（ACES approximation）
    color = color / (color + vec3(0.155)) * 1.019;
    
    // ガンマ補正
    color = pow(color, vec3(1.0/2.2));
    
    outColor = vec4(color, 1.0);
}
