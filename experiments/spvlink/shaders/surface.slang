struct PelicanSurface
{
    float3 albedo;
    float2 uv;
};

[[vk::binding(0, 2)]]
Texture2D<float4> userTexture;

[[vk::binding(1, 2)]]
SamplerState userSampler;

[noinline]
void pelican_surface(inout PelicanSurface surface)
{
    float3 texel = userTexture.Sample(userSampler, surface.uv).rgb;
#ifdef PELICAN_VARIANT_WARM
    surface.albedo = surface.albedo * float3(1.0, 0.78, 0.50) + texel * 0.25;
#else
    surface.albedo = surface.albedo * float3(0.45, 0.70, 1.0) + texel * 0.15;
#endif
}

[shader("fragment")]
float4 main() : SV_Target
{
    PelicanSurface surface;
    surface.albedo = float3(0.0);
    surface.uv = float2(0.0);
    pelican_surface(surface);
    return float4(surface.albedo, 1.0);
}
