#version 460
#extension GL_ARB_separate_shader_objects : enable

layout(set = 1, binding = 0) uniform sampler2D baseColorSampler;
layout(set = 1, binding = 1) uniform sampler2D metallicRoughnessSampler;
layout(set = 1, binding = 2) uniform sampler2D normalSampler;

layout(location = 0) in vec2 texUV;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec3 inWorldPos;

// G-Buffer出力
layout(location = 0) out vec4 outAlbedo;      // RGB: Albedo, A: Alpha
layout(location = 1) out vec4 outNormal;      // RGB: 法線(0-1に正規化), A: reserved
layout(location = 2) out vec4 outMaterial;    // R: Roughness, G: Metallic, B: AO, A: reserved
layout(location = 3) out vec4 outWorldPos;    // RGB: WorldPos, A: reserved

void main() {
    // Base Color（テクスチャのみ、頂点カラーは無視）
    vec4 baseColor = texture(baseColorSampler, texUV);
    outAlbedo = baseColor;

    // Normal
    vec3 worldNormal = normalize(inNormal);
    outNormal = vec4(worldNormal * 0.5 + 0.5, 1.0);

    // Metallic-Roughness（glTF形式: G=roughness, B=metallic）
    vec3 mr = texture(metallicRoughnessSampler, texUV).rgb;
    // G-bufferへの出力: R=roughness, G=metallic, B=AO（glTF標準に合わせる）
    // glTF ORM texture: R=Occlusion, G=Roughness, B=Metallic
    outMaterial = vec4(mr.g, mr.b, mr.r, 1.0);  // R=roughness(green), G=metallic(blue), B=AO(red)

    // World Position
    outWorldPos = vec4(inWorldPos, 1.0);
}