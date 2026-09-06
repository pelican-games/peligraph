#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace Pelican {

inline constexpr std::uint32_t punctualShadowDataV1Magic = 0x50505331u;
inline constexpr std::uint32_t punctualShadowDataV1Version = 1u;

enum class PunctualShadowKind : std::uint32_t {
    point = 1,
    spot = 2,
};

struct PunctualShadowDataInput {
    std::uint32_t inventory_index;
    PunctualShadowKind kind;
    std::uint32_t first_layer;
    std::span<const glm::mat4> view_projections;
};

// std430 uvec4 elements, with offsets measured in elements:
//   header = magic, version, record count, 0
//   record = inventory index, kind (point=1 / spot=2), first matrix, first layer
//   matrix = four columns encoded with floatBitsToUint semantics
// Records and their matrices are sorted by inventory index. Point records own
// six consecutive matrices/layers in +X,-X,+Y,-Y,+Z,-Z order; spot records own
// one. The caller supplies the matrices and layer allocation; no camera or
// light-selection policy is applied. Duplicate inventory indices are rejected.
std::vector<glm::uvec4> packPunctualShadowDataV1(
    std::span<const PunctualShadowDataInput> lights);

} // namespace Pelican
