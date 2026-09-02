#pragma once

#include <cstdint>
#include <optional>

namespace Pelican {

// These values are deliberately the Vulkan operand bits.  The authoring and
// scene layers can therefore transport the resolved declaration without
// depending on Vulkan headers, while the AS builder can consume it verbatim.
inline constexpr std::uint32_t rayInstanceTriangleFacingCullDisable = 0x1u;
inline constexpr std::uint32_t rayInstanceTriangleFlipFacing = 0x2u;
inline constexpr std::uint32_t rayInstanceForceOpaque = 0x4u;
inline constexpr std::uint32_t rayInstanceForceNoOpaque = 0x8u;
inline constexpr std::uint32_t rayGeometryOpaque = 0x1u;
inline constexpr std::uint32_t rayGeometryNoDuplicateAnyHitInvocation = 0x2u;
inline constexpr std::uint32_t rayCustomIndexMaximum = 0x00ffffffu;

// The only canonical omission defaults for the RT3-1 declaration surface.
// Layout parsing, component resolution, TLAS lowering, and tests all consume
// this value instead of restating literals.
struct RayAccelerationStructureCanonicalDefaults {
    std::uint8_t mask = 0xffu;
    std::uint32_t instance_flags = 0;
    std::uint32_t custom_index = 0;
    std::uint32_t geometry_flags = 0;

    bool operator==(const RayAccelerationStructureCanonicalDefaults &) const =
        default;
};

inline constexpr RayAccelerationStructureCanonicalDefaults
    rayAccelerationStructureCanonicalDefaults{};

// Presence is field-wise: an authored empty instance_flags array is a real
// override, while an entirely empty component is rejected by the codec.
struct RayInstanceComponent {
    std::optional<std::uint8_t> mask;
    std::optional<std::uint32_t> instance_flags;
    std::optional<std::uint32_t> custom_index;

    bool operator==(const RayInstanceComponent &) const = default;
};

} // namespace Pelican
