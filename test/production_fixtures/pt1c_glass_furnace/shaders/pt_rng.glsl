#ifndef PT_RNG_GLSL
#define PT_RNG_GLSL

// 32-bit hash: one step of a 32-bit LCG followed by PCG's RXS-M-XS output
// permutation. PCG: M. E. O'Neill, https://www.pcg-random.org (Apache-2.0);
// as a GPU hash ("pcg"): M. Jarzynski, M. Olano, "Hash Functions for GPU
// Rendering", JCGT 9(3), 2020.
uint ptPcgHash(uint value) {
    uint lcgState = value * 747796405u + 2891336453u;
    uint permuted = ((lcgState >> ((lcgState >> 28u) + 4u)) ^ lcgState) * 277803737u;
    return (permuted >> 22u) ^ permuted;
}

uint ptSeed(uvec2 pixel, uint frameIndex) {
    uint seed = ptPcgHash(pixel.x ^ 0x9e3779b9u);
    seed = ptPcgHash(seed ^ pixel.y ^ 0x85ebca6bu);
    return ptPcgHash(seed ^ frameIndex ^ 0xc2b2ae35u);
}

float ptRandom(uint seed, uint bounce, uint dimension) {
    uint value = seed ^ ptPcgHash(bounce + 0x68bc21ebu);
    value ^= ptPcgHash(dimension + 0x02e5be93u);
    return float(ptPcgHash(value) >> 8u) * (1.0 / 16777216.0);
}

#endif
