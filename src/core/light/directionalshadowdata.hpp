#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace Pelican {

inline constexpr std::uint32_t
    directionalShadowDataV1Magic = 0x50534831u;
inline constexpr std::uint32_t
    directionalShadowDataV1Version = 1u;
inline constexpr std::uint32_t
    directionalShadowDataV1HeaderElements = 1u;
inline constexpr std::uint32_t
    directionalShadowDataV1MatrixElements = 4u;

struct PackedDirectionalShadowDataV1 {
    std::vector<glm::uvec4> elements;
    std::uint32_t shadow_light_count = 0;
    std::uint32_t cascade_count = 0;
};

// Compact std430 uvec4 table consumed by forward and deferred lighting:
//   header = magic, version, shadow-light count, cascade count
//   record = inventory index, first matrix element, first texture layer,
//            cascade count
//   matrix = four column vectors encoded with floatBitsToUint semantics
// Records are sorted by inventory index so shaders can use binary lookup.
PackedDirectionalShadowDataV1
packDirectionalShadowDataV1(
    std::span<const std::uint32_t>
        light_inventory_indices,
    std::uint32_t cascade_count,
    std::span<const glm::mat4>
        view_projections);

} // namespace Pelican
