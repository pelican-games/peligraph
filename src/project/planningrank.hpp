#pragma once

#include <cstdint>
#include <string_view>

namespace Pelican {

// Shared by the planner's ready-node selection and physical candidate policy.
inline std::uint64_t stablePlanningRank(std::uint64_t seed, std::uint64_t salt,
                                        std::string_view value) {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto add_byte = [&](std::uint8_t byte) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    };
    for (std::uint32_t shift = 0; shift < 64; shift += 8) {
        add_byte(static_cast<std::uint8_t>(seed >> shift));
        add_byte(static_cast<std::uint8_t>(salt >> shift));
    }
    for (const auto character : value) add_byte(static_cast<std::uint8_t>(character));
    // SplitMix64 finalizer (S. Vigna, public domain).
    hash ^= hash >> 30U;
    hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27U;
    hash *= 0x94d049bb133111ebULL;
    return hash ^ (hash >> 31U);
}

} // namespace Pelican
