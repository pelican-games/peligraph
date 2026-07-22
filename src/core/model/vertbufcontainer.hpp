#pragma once

#include "../container.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/deferredcallback.hpp"
#include "polygonvertdata.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct CommonVertStruct {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec4 tangent;
    glm::vec2 texcoord;
    glm::vec4 color;
};

struct CommonSkinningVertStruct {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec4 tangent;
    glm::vec2 texcoord;
    glm::vec4 color;
    glm::i16vec4 joint;
    glm::vec4 weight;
};

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
    // Declared last and explicitly closed at destructor entry. Deferred range
    // callbacks pin this state while using the owner.
    DeferredCallbackLifetime deferred_callbacks;

    static uint32_t allocateRange(std::vector<FreeRange> &free_ranges,
                                  uint32_t &high_water, uint32_t count);
    static void releaseRange(std::vector<FreeRange> &free_ranges,
                             uint32_t offset, uint32_t count);
    void ensureIndexCapacity(uint32_t required);
    void ensureVertexCapacity(uint32_t required, bool skinned);
    std::vector<MorphTargetDeltaRange>
    uploadMorphData(const CommonPolygonVertData &data, uint32_t vertex_offset,
                    bool skinned, uint32_t &delta_offset, uint32_t &delta_count);
    void releaseGeometryNow(std::span<const ModelGeometryAllocation> allocations) noexcept;

  public:
    VertBufContainer();
    ~VertBufContainer();
    ModelTemplate::PrimitiveRefInfo addPrimitiveEntry(CommonPolygonVertData &&data);
    ModelTemplate::PrimitiveRefInfo addSkinnedPrimitiveEntry(CommonPolygonVertData &&data);
    ModelGeometryAllocation addPrimitiveAllocation(CommonPolygonVertData &&data);
    ModelGeometryAllocation addSkinnedPrimitiveAllocation(CommonPolygonVertData &&data);
    void releaseModelGeometry(std::vector<ModelGeometryAllocation> allocations,
                              bool deferred) noexcept;

    void bindVertexBuffer(vk::CommandBuffer cmd_buf, bool skinned = false) const;
    const BufferWrapper &morphMetadataBuffer(bool skinned) const {
        return skinned ? morph_skinned_metadata_buffer : morph_static_metadata_buffer;
    }
    const BufferWrapper &morphDeltaBuffer() const { return morph_delta_buffer; }

    struct CommonVertDataDescription {
        std::vector<vk::VertexInputBindingDescription> binding_descs;
        std::vector<vk::VertexInputAttributeDescription> attr_descs;
    };
    static CommonVertDataDescription getDescription();
    static CommonVertDataDescription getSkinnedDescription();
    size_t allocatedIndexCountForTesting() const;
    size_t allocatedVertexCountForTesting(bool skinned = false) const;
    size_t allocatedMorphDeltaCountForTesting() const;
};

} // namespace Pelican
