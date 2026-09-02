#pragma once

#include "../container.hpp"

#include <cstdint>

namespace Pelican {

class RngSequenceScope;

DECLARE_MODULE(DeterministicRng) {
    std::uint64_t current_seed = 0;
    std::uint64_t state = 0;
    std::uint64_t increment = 0;

    std::uint32_t nextUInt();

  public:
    struct PreparedSequence {
        std::uint64_t current_seed = 0;
        std::uint64_t state = 0;
        std::uint64_t increment = 0;
    };

    DeterministicRng();

    double random();
    int randomInt(int min, int max);
    float randomFloat(float min, float max);
    void setSeed(std::uint64_t seed);
    std::uint64_t seed() const;
    PreparedSequence prepareSequence() const noexcept;
    void publishPreparedSequence(PreparedSequence prepared) noexcept;
};

class RngSequenceScope {
    DeterministicRng::PreparedSequence *previous_ = nullptr;

  public:
    explicit RngSequenceScope(
        DeterministicRng::PreparedSequence &candidate) noexcept;
    ~RngSequenceScope();
    RngSequenceScope(const RngSequenceScope &) = delete;
    RngSequenceScope &operator=(const RngSequenceScope &) = delete;
};

} // namespace Pelican
