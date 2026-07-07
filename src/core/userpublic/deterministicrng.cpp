#include "deterministicrng.hpp"

#include "../loader/basicconfig.hpp"

#include <limits>
#include <stdexcept>

namespace Pelican {

namespace {

constexpr std::uint64_t pcg_multiplier = 6364136223846793005ull;
constexpr std::uint64_t pcg_stream = 0xda3e39cb94b95bdbull;
constexpr double one_over_two_to_53 = 1.0 / 9007199254740992.0;

} // namespace

DeterministicRng::DeterministicRng() {
    setSeed(GET_MODULE(ProjectBasicConfig).seed());
}

std::uint32_t DeterministicRng::nextUInt() {
    const auto old_state = state;
    state = old_state * pcg_multiplier + increment;
    const auto xorshifted = static_cast<std::uint32_t>(((old_state >> 18u) ^ old_state) >> 27u);
    const auto rot = static_cast<std::uint32_t>(old_state >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
}

double DeterministicRng::random() {
    const auto hi = static_cast<std::uint64_t>(nextUInt() >> 5u);
    const auto lo = static_cast<std::uint64_t>(nextUInt() >> 6u);
    return static_cast<double>((hi << 26u) | lo) * one_over_two_to_53;
}

int DeterministicRng::randomInt(int min, int max) {
    if (min > max) {
        throw std::runtime_error("randomInt requires min <= max");
    }

    const auto min64 = static_cast<std::int64_t>(min);
    const auto span = static_cast<std::uint64_t>(static_cast<std::int64_t>(max) - min64) + 1ull;
    if (span == 1ull) {
        return min;
    }

    std::uint32_t offset = 0;
    if (span == (static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1ull)) {
        offset = nextUInt();
    } else {
        const auto bound = static_cast<std::uint32_t>(span);
        const auto threshold = static_cast<std::uint32_t>(0u - bound) % bound;
        for (;;) {
            const auto candidate = nextUInt();
            if (candidate >= threshold) {
                offset = candidate % bound;
                break;
            }
        }
    }

    return static_cast<int>(min64 + static_cast<std::int64_t>(offset));
}

float DeterministicRng::randomFloat(float min, float max) {
    if (min > max) {
        throw std::runtime_error("randomFloat requires min <= max");
    }
    if (min == max) {
        return min;
    }

    return static_cast<float>(static_cast<double>(min) +
                              (static_cast<double>(max) - static_cast<double>(min)) * random());
}

void DeterministicRng::setSeed(std::uint64_t seed) {
    current_seed = seed;
    state = 0;
    increment = (pcg_stream << 1u) | 1u;
    (void)nextUInt();
    state += seed;
    (void)nextUInt();
}

} // namespace Pelican
