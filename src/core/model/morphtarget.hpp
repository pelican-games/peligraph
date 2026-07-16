#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <vector>

namespace Pelican {

// The v1 renderer uses fixed-stride per-instance weights and bounded shared
// morph storage. Targetless geometry emits zero metadata and no delta records.
// Morph-bearing assets fail before GPU publication when a limit is exceeded.
inline constexpr std::uint32_t maxMorphTargetsPerPrimitive = 64;
inline constexpr std::uint32_t maxMorphWeightsPerInstance = 256;
inline constexpr std::uint32_t maxMorphVerticesPerPool = 262144;
inline constexpr std::uint32_t maxMorphDeltaRecords = 524288;

inline constexpr std::uint32_t morphPositionPresent = 1u << 0u;
inline constexpr std::uint32_t morphNormalPresent = 1u << 1u;
inline constexpr std::uint32_t morphTangentPresent = 1u << 2u;
inline constexpr std::uint32_t noMorphNode = std::numeric_limits<std::uint32_t>::max();

struct MorphTargetDeltaRange {
    std::uint32_t target_index = 0;
    std::uint32_t delta_offset = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t presence_mask = 0;
};

struct MorphPrimitiveLayout {
    std::uint32_t node_index = noMorphNode;
    std::uint32_t mesh_index = 0;
    std::uint32_t primitive_index = 0;
    std::uint32_t weight_offset = 0;
    std::uint32_t vertex_offset = 0;
    bool skinned = false;
    std::vector<MorphTargetDeltaRange> delta_ranges;
};

// Immutable model-generation identity. Source coordinates deliberately remain
// separate from global GPU offsets so S1b can resolve glTF/VRM binds without
// depending on renderer allocation order.
struct MorphTargetLayout {
    std::uint64_t generation = 0;
    std::vector<float> default_weights;
    std::vector<MorphPrimitiveLayout> primitives;
};

struct MorphTargetVertexData {
    std::vector<glm::vec3> position;
    std::vector<glm::vec3> normal;
    std::vector<glm::vec3> tangent;
    std::uint32_t presence_mask = 0;
};

struct alignas(16) MorphVertexGpuMetadata {
    std::uint32_t delta_offset = 0;
    std::uint32_t vertex_stride = 0;
    std::uint32_t weight_offset = 0;
    std::uint32_t target_count = 0;
};

struct alignas(16) MorphDeltaGpuData {
    glm::vec4 position{0.0f};
    glm::vec4 normal{0.0f};
    glm::vec4 tangent{0.0f};
};

static_assert(sizeof(MorphVertexGpuMetadata) == 16);
static_assert(sizeof(MorphDeltaGpuData) == 48);

} // namespace Pelican
