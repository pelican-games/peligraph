#pragma once

#include "../container.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/deferredcallback.hpp"
#include "polygonvertdata.hpp"
#include "modelresourcerelease.hpp"
#include <cstddef>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class VertBufContainer;
namespace test_support {
void setMorphMetadataReleaseFailureForTesting(
    VertBufContainer &geometry, bool enabled) noexcept;
} // namespace test_support

struct CommonVertStruct {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec4 tangent;
    glm::vec2 texcoord;
    glm::vec4 color;
};

static_assert(offsetof(CommonVertStruct, pos) == 0);
static_assert(offsetof(CommonVertStruct, normal) == 12);
static_assert(offsetof(CommonVertStruct, tangent) == 24);
static_assert(offsetof(CommonVertStruct, texcoord) == 40);
static_assert(offsetof(CommonVertStruct, color) == 48);
static_assert(sizeof(CommonVertStruct) == 64);

struct CommonSkinningVertStruct {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec4 tangent;
    glm::vec2 texcoord;
    glm::vec4 color;
    glm::i16vec4 joint;
    glm::vec4 weight;
};

// Uploaded to the GPU as raw bytes: the size is the sum of the members,
// so the struct has no padding for stale bytes to hide in.
static_assert(sizeof(CommonSkinningVertStruct) == 88);

DECLARE_MODULE(VertBufContainer) {
    struct FreeRange {
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    uint32_t indices_offset;
    uint32_t vertices_offset;
    uint32_t indices_cap;
    uint32_t vertices_cap;
    uint32_t skin_vertices_offset;
    uint32_t skin_vertices_cap;
    std::uint64_t geometry_address_generation = 1;
    std::uint64_t next_geometry_allocation_id = 1;
    BufferWrapper indices_mem_pool;
    BufferWrapper vertices_mem_pool;
    BufferWrapper skin_vertices_mem_pool;
    BufferWrapper morph_static_metadata_buffer;
    BufferWrapper morph_skinned_metadata_buffer;
    BufferWrapper morph_delta_buffer;
    uint32_t morph_delta_offset = 0;
    std::vector<FreeRange> free_indices;
    std::vector<FreeRange> free_vertices;
    std::vector<FreeRange> free_skin_vertices;
    std::vector<FreeRange> free_morph_deltas;
    bool fail_morph_metadata_release_for_testing = false;
    // Declared last and explicitly closed at destructor entry. Deferred range
    // callbacks pin this state while using the owner.
    DeferredCallbackLifetime deferred_callbacks;

    friend void test_support::setMorphMetadataReleaseFailureForTesting(
        VertBufContainer &geometry, bool enabled) noexcept;

    static uint32_t allocateRange(std::vector<FreeRange> &free_ranges,
                                  uint32_t &high_water, uint32_t count);
    static void releaseRange(std::vector<FreeRange> &free_ranges,
                             uint32_t offset, uint32_t count);
    void ensureIndexCapacity(uint32_t required);
    void ensureVertexCapacity(uint32_t required, bool skinned);
    std::vector<MorphTargetDeltaRange>
    uploadMorphData(const CommonPolygonVertData &data, uint32_t vertex_offset,
                    bool skinned, uint32_t &delta_offset, uint32_t &delta_count);

  public:
    std::uint64_t reserveGeometryAllocationId();
    VertBufContainer();
    ~VertBufContainer();
    ModelTemplate::PrimitiveRefInfo addPrimitiveEntry(CommonPolygonVertData &&data);
    ModelTemplate::PrimitiveRefInfo addSkinnedPrimitiveEntry(CommonPolygonVertData &&data);
    ModelGeometryAllocation addPrimitiveAllocation(CommonPolygonVertData &&data);
    ModelGeometryAllocation addSkinnedPrimitiveAllocation(CommonPolygonVertData &&data);
    [[nodiscard]] ReleaseReport releaseGeometryNow(
        std::span<const ModelGeometryAllocation> allocations) noexcept;
    [[nodiscard]] ReleaseReport releaseModelGeometry(
        std::vector<ModelGeometryAllocation> allocations,
        bool deferred) noexcept;

    void bindVertexBuffer(vk::CommandBuffer cmd_buf, bool skinned = false) const;
    const BufferWrapper &morphMetadataBuffer(bool skinned) const {
        return skinned ? morph_skinned_metadata_buffer : morph_static_metadata_buffer;
    }
    const BufferWrapper &morphDeltaBuffer() const { return morph_delta_buffer; }
    const BufferWrapper &indexBuffer() const { return indices_mem_pool; }
    const BufferWrapper &vertexBuffer(bool skinned = false) const {
        return skinned ? skin_vertices_mem_pool : vertices_mem_pool;
    }
    std::uint64_t geometryAddressGeneration() const noexcept {
        return geometry_address_generation;
    }

    struct CommonVertDataDescription {
        std::vector<vk::VertexInputBindingDescription> binding_descs;
        std::vector<vk::VertexInputAttributeDescription> attr_descs;
    };
    static CommonVertDataDescription getDescription();
    static CommonVertDataDescription getSkinnedDescription();
    size_t allocatedIndexCountForTesting() const;
    size_t allocatedVertexCountForTesting(bool skinned = false) const;
    size_t allocatedMorphDeltaCountForTesting() const;
    std::uint32_t indexCapacityForTesting() const noexcept {
        return indices_cap;
    }
    std::uint32_t vertexCapacityForTesting(bool skinned = false) const noexcept {
        return skinned ? skin_vertices_cap : vertices_cap;
    }
};

} // namespace Pelican
