#version 460
#extension GL_GOOGLE_include_directive : enable

#include "pelican_sets.glsl"
#include "pelican_frame.glsl"

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D albedoSampler;
layout(set = PELICAN_SET_PASS_INPUT, binding = 1) uniform sampler2D normalSampler;
layout(set = PELICAN_SET_PASS_INPUT, binding = 2) uniform sampler2D materialSampler;
layout(set = PELICAN_SET_PASS_INPUT, binding = 3) uniform sampler2D worldPosSampler;
layout(set = PELICAN_SET_PASS_INPUT, binding = 4) uniform sampler2D emissiveSampler;

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    vec2 uv = clamp(inTexCoord, vec2(0.001), vec2(0.999));
    vec3 albedo = texture(albedoSampler, uv).rgb;
    vec3 normal = normalize(texture(normalSampler, uv).xyz * 2.0 - vec3(1.0));
    vec3 material = texture(materialSampler, uv).rgb;
    vec3 world = texture(worldPosSampler, uv).rgb;
    vec3 emissive = texture(emissiveSampler, uv).rgb;

    float roughness = clamp(material.r, 0.045, 1.0);
    float metallic = clamp(material.g, 0.0, 1.0);
    float occlusion = clamp(material.b, 0.0, 1.0);
    vec3 N = normal;
    vec3 V = normalize(pelicanFrame.camera_position.xyz - world);
    vec3 L = normalize(vec3(-0.45, 0.72, 0.55));
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float dDenom = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    float D = alpha2 / max(3.14159265 * dDenom * dDenom, 0.0001);
    float k = pow(roughness + 1.0, 2.0) / 8.0;
    float Gv = NdotV / max(NdotV * (1.0 - k) + k, 0.0001);
    float Gl = NdotL / max(NdotL * (1.0 - k) + k, 0.0001);
    vec3 F0 = mix(vec3(0.04), albedo, vec3(metallic));
    vec3 F = fresnelSchlick(VdotH, F0);
    vec3 specular = (D * Gv * Gl) * F / max(4.0 * NdotV * NdotL, 0.001);
    vec3 diffuse = (vec3(1.0) - F) * (1.0 - metallic) * albedo / 3.14159265;
    vec3 lightColor = vec3(1.0, 0.96, 0.86) * 2.8;
    vec3 fillDirection = normalize(vec3(0.62, 0.38, 0.74));
    vec3 fill = max(dot(N, fillDirection), 0.0) * albedo * vec3(0.13, 0.22, 0.20) * (1.0 - metallic * 0.55);
    vec3 ambient = albedo * vec3(0.105, 0.145, 0.125) * occlusion;
    vec3 color = (diffuse + specular) * lightColor * NdotL + ambient + fill + emissive;
    vec3 mapped = vec3(1.0) - exp(-color * 1.38);
    outColor = vec4(pow(mapped, vec3(0.92)), 1.0);
}
