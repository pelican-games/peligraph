#pragma once

#include <cstdint>
#include <optional>
#include "../details/schema/structfieldschema.hpp"

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

enum class RayInstanceFlag : std::uint32_t {
    CullDisable = rayInstanceTriangleFacingCullDisable,
    FlipFacing = rayInstanceTriangleFlipFacing,
    ForceOpaque = rayInstanceForceOpaque,
    ForceNoOpaque = rayInstanceForceNoOpaque,
};

struct RayOccurrenceOverride {
    std::string occurrence_id;
    std::optional<std::uint8_t> mask;
    std::optional<Flags<RayInstanceFlag>> instance_flags;
    std::optional<std::uint32_t> custom_index;

    static constexpr auto schema = structFields(
        componentPolicy("ray_occurrence_override"),
        required(field<&RayOccurrenceOverride::occurrence_id>("occurrence_id")),
        optionalOverride(field<&RayOccurrenceOverride::mask>("mask", urange(0, 255))),
        optionalOverride(field<&RayOccurrenceOverride::instance_flags>("instance_flags"),
            enumValues(
                enumValue("triangle_facing_cull_disable", RayInstanceFlag::CullDisable),
                enumValue("triangle_flip_facing", RayInstanceFlag::FlipFacing),
                enumValue("force_opaque", RayInstanceFlag::ForceOpaque),
                enumValue("force_no_opaque", RayInstanceFlag::ForceNoOpaque))),
        optionalOverride(field<&RayOccurrenceOverride::custom_index>(
            "custom_index", urange(0, rayCustomIndexMaximum))));

    bool operator==(const RayOccurrenceOverride &) const = default;
};

// Presence is field-wise: an authored empty instance_flags array is a real
// override, while an entirely empty component is rejected by the codec.
struct RayInstanceComponent {
    std::optional<std::uint8_t> mask;
    std::optional<std::uint32_t> instance_flags;
    std::optional<std::uint32_t> custom_index;
    std::optional<List<RayOccurrenceOverride>> overrides;

    bool operator==(const RayInstanceComponent &) const = default;
};

} // namespace Pelican
