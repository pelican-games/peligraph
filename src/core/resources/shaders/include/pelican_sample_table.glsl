#ifndef PELICAN_SAMPLE_TABLE_GLSL
#define PELICAN_SAMPLE_TABLE_GLSL

// AB-1: a baked table (bake_audio_analysis's R32_SFLOAT KTX2) read on the GPU
// the way value.sample_table reads it on the CPU.
//
//   float pelican_sample_table(sampler2D table, uint feature,
//                              uint rows_per_feature, uint sample_count,
//                              float hop_seconds, float t)
//
// Feature `feature` (its row in the manifest's layout.features: rms, peak,
// band:0 ...) at `t` seconds: the two samples either side of t / hop_seconds
// (texelFetch, no filtering), linearly; before the first or after the last,
// the end's value. Sample i of feature f is the texel
// (i % width, f * rows_per_feature + i / width), width the table's own.
//
// The table is a render.image that starts from the asset (initial: {asset},
// lifetime persistent); rows_per_feature, sample_count and hop_seconds come
// from the manifest's layout, passed in a render.scalar vec4 with the time,
// e.g. {rows_per_feature, sample_count, hop_seconds, scene_time}.

float pelican_sample_table(sampler2D table, uint feature, uint rows_per_feature, uint sample_count,
                           float hop_seconds, float t) {
    if (sample_count == 0u || !(hop_seconds > 0.0)) return 0.0;
    uint width = uint(textureSize(table, 0).x);
    float position = clamp(t / hop_seconds, 0.0, float(sample_count - 1u));
    uint first = uint(floor(position));
    uint second = min(first + 1u, sample_count - 1u);
    float fraction = position - float(first);
    uint base = feature * rows_per_feature;
    float a = texelFetch(table, ivec2(int(first % width), int(base + first / width)), 0).r;
    float b = texelFetch(table, ivec2(int(second % width), int(base + second / width)), 0).r;
    return mix(a, b, fraction);
}

#endif
