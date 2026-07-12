#pragma once
#include <cstdint>

namespace Pelican::AnimationV0 {

inline constexpr std::uint32_t descriptorVersionV1 = 1;
struct CursorHandle { std::uint64_t identity; std::uint32_t generation; std::uint32_t reserved; };
struct Vec4fV1 { float x, y, z, w; };
struct QuatfV1 { float x, y, z, w; };
struct RootDeltaV1 { Vec4fV1 translation; QuatfV1 rotation; };
struct CrossingV1;

struct AdvanceDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    CursorHandle cursor;
    double delta_seconds;
    double absolute_seconds;
    std::uint32_t flags;
    std::uint32_t reserved2;
};

// First published snapshot: interval values were not visible to the client yet.
struct IntervalResultV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    std::uint32_t result_flags;
    float normalized_phase;
    RootDeltaV1 root_delta;
    CrossingV1 *crossings;
    std::uint32_t crossing_capacity;
    std::uint32_t crossing_count;
};

struct ApiV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    std::uint32_t engine_abi_version;
    std::uint32_t minimum_client_abi_version;
    std::uint64_t capability_bits;
    void *context;
};

} // namespace Pelican::AnimationV0
