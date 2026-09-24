#ifndef PT_RNG_GLSL
#define PT_RNG_GLSL

uint ptPcgHash(uint value) {
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
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
