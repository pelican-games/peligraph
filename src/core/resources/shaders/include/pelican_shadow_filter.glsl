#ifndef PELICAN_SHADOW_FILTER_GLSL
#define PELICAN_SHADOW_FILTER_GLSL

// Instantiate against either generated resource-port functions or a sampler
// adapter. Radius 0/1/2 selects 1/9/25 depth comparisons. Policy and defaults
// belong to the feature declaration, so this kernel supplies no fallback values.
#define PELICAN_DEFINE_SHADOW_FILTER(NAME, SAMPLE, SIZE) \
float NAME(vec2 uv, uint layer, float depth, int radius, float bias, float strength) { \
    vec2 texel = 1.0 / vec2(SIZE()); \
    float visible = 0.0; \
    for (int y = -radius; y <= radius; ++y) { \
        for (int x = -radius; x <= radius; ++x) { \
            float stored = SAMPLE(uv + vec2(x, y) * texel, layer, 0.0).r; \
            visible += depth - bias <= stored ? 1.0 : 0.0; \
        } \
    } \
    float width = float(radius * 2 + 1); \
    return 1.0 - strength * (1.0 - visible / (width * width)); \
}

#endif
