#version 460

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D albedoSampler;
layout(set = 0, binding = 1) uniform sampler2D normalSampler;
layout(set = 0, binding = 2) uniform sampler2D materialSampler; // R: roughness, G: metallic, B: AO
layout(set = 0, binding = 3) uniform sampler2D worldPosSampler;

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

void main() {
    // G-bufferからデータを読み取る
    vec3 albedo = texture(albedoSampler, inUV).rgb;
    vec3 normal = normalize(texture(normalSampler, inUV).rgb * 2.0 - 1.0);
    vec3 material = texture(materialSampler, inUV).rgb;
    vec3 worldPos = texture(worldPosSampler, inUV).rgb;
    
    float roughness = clamp(material.r, 0.04, 1.0);  // material.r = roughness
    float metallic = clamp(material.g, 0.0, 1.0);   // material.g = metallic
    float ao = material.b;
    
    vec3 cameraPos = pushConsts.cameraPos.xyz;
    
    vec3 V = normalize(cameraPos - worldPos);
    
    // リアルなスタジオライティング配置（3つのライト）
    vec3 lightPositions[3] = vec3[](
        vec3(0.0, 0.0, -5.0),   // 手前の赤色ライト（カメラ側）
        vec3(-4.0, 3.0, 4.0),   // フィルライト（反対側から柔らかく）
        vec3(0.0, -3.0, 3.0)    // リムライト（下から輪郭を強調）
    );
    
    vec3 lightColors[3] = vec3[](
        vec3(100.0, 0.0, 0.0),    // 赤色
        vec3(40.0, 50.0, 60.0),   // 寒色系フィルライト
        vec3(30.0, 30.0, 35.0)    // ニュートラルリムライト
    );
    
    // F0（表面の基底反射率）- 金属はalbedo色、非金属は0.04
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);
    
    // 反射率方程式
    vec3 Lo = vec3(0.0);
    
    for(int i = 0; i < 3; ++i) {
        vec3 L = normalize(lightPositions[i] - worldPos);
        vec3 H = normalize(V + L);
        float distance = length(lightPositions[i] - worldPos);
        float attenuation = 1.0 / (distance * distance + 0.01);
        vec3 radiance = lightColors[i] * attenuation;
        
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
        Lo += (kD * albedo / PI + specular) * radiance * NdotL;
    }
    
    // 環境光をより充実させる
    vec3 ambient = mix(vec3(0.03) * albedo, albedo * 0.12, metallic) * ao;
    vec3 color = ambient + Lo;
    
    // HDRトーンマッピング（ACES approximation）
    color = color / (color + vec3(0.155)) * 1.019;
    
    // ガンマ補正
    color = pow(color, vec3(1.0/2.2));
    
    outColor = vec4(color, 1.0);
}