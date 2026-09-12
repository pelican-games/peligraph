#pragma once
#include "../../project/graphinstanceowner.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {
struct CompiledFrameGraphExecution;
struct RayQueryGeometryInstanceSnapshot;
struct GraphDeformedAttribute {
    std::string resource;
    GraphInstanceOwner owner;
    std::string asset;
    std::uint32_t node;
    std::uint32_t mesh;
    std::uint32_t primitive;
    std::uint32_t byte_offset;
    std::uint64_t expected_count;
};

// Validate live owner/mesh references without publishing or writing GPU state.
void validateGraphDeformedAttributes(const std::vector<GraphDeformedAttribute> &attributes);
void validateGraphDeformedAttributes(const std::vector<GraphDeformedAttribute> &attributes,
    const std::vector<RayQueryGeometryInstanceSnapshot> &geometry);

// Called after model/skin/morph upload and before the first graph consumer.
// The caller retains these snapshots until the submitted frame completes.
void recordGraphDeformedAttributes(vk::CommandBuffer command,
    const CompiledFrameGraphExecution &graph,
    std::vector<RayQueryGeometryInstanceSnapshot> &geometry, bool for_acceleration_structures);
}
