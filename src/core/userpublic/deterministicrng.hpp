#pragma once

#include "../container.hpp"

#include <cstdint>

namespace Pelican {

DECLARE_MODULE(DeterministicRng) {
    std::uint64_t current_seed = 0;
    std::uint64_t state = 0;
    std::uint64_t increment = 0;

    std::uint32_t nextUInt();

  public:
    DeterministicRng();

    double random();
    int randomInt(int min, int max);
    float randomFloat(float min, float max);
    void setSeed(std::uint64_t seed);
    std::uint64_t seed() const { return current_seed; }
};

} // namespace Pelican
