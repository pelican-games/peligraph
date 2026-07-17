#pragma once

#include "modeltemplate.hpp"

#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace Pelican {

struct VrmAutoTriangleSplitInput {
    std::span<const std::uint32_t> indices;
    std::uint32_t vertex_count = 0;
    std::span<const glm::i16vec4> joints;
    std::span<const glm::vec4> weights;
    std::span<const int> skin_joint_nodes;
    std::span<const std::uint8_t> head_related_nodes;
};

struct VrmAutoTriangleSplit {
    std::vector<std::uint32_t> both_indices;
    std::vector<std::uint32_t> third_person_only_indices;
};

// VRM 1.0 MeshAnnotation.Auto. Original triangle order is retained within
// each output group. A triangle belongs to the head group when any of its
// vertices has a positive weight for Head or one of Head's descendants.
VrmAutoTriangleSplit
splitVrmAutoTriangles(const VrmAutoTriangleSplitInput &input);

} // namespace Pelican
