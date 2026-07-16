#pragma once

#include "../container.hpp"
#include "../vkcore/buf.hpp"
#include "modeltemplate.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct CommonPolygonVertData {
    std::vector<uint32_t> indices;
    std::vector<glm::vec3> pos;
    std::vector<glm::vec3> normal;
    std::vector<glm::vec4> tangent;
    std::vector<glm::vec2> texcoord;
    std::vector<glm::vec4> color;
    std::vector<glm::i16vec4> joint;
    std::vector<glm::vec4> weight;
};

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
    std::vector<FreeRange> free_indices;
    std::vector<FreeRange> free_vertices;
    std::vector<FreeRange> free_skin_vertices;
    // Deferred range-return objects hold a weak guard rather than assuming a
    // particular module teardown order.
    std::shared_ptr<int> lifetime_token = std::make_shared<int>(0);

    static uint32_t allocateRange(std::vector<FreeRange> &free_ranges,
                                  uint32_t &high_water, uint32_t count);
    static void releaseRange(std::vector<FreeRange> &free_ranges,
                             uint32_t offset, uint32_t count);
    void ensureIndexCapacity(uint32_t required);
    void ensureVertexCapacity(uint32_t required, bool skinned);
    void releaseGeometryNow(std::span<const ModelGeometryAllocation> allocations) noexcept;

  public:
    VertBufContainer();
    ModelTemplate::PrimitiveRefInfo addPrimitiveEntry(CommonPolygonVertData &&data);
    ModelTemplate::PrimitiveRefInfo addSkinnedPrimitiveEntry(CommonPolygonVertData &&data);
    ModelGeometryAllocation addPrimitiveAllocation(CommonPolygonVertData &&data);
    ModelGeometryAllocation addSkinnedPrimitiveAllocation(CommonPolygonVertData &&data);
    void releaseModelGeometry(std::vector<ModelGeometryAllocation> allocations,
                              bool deferred) noexcept;

    void bindVertexBuffer(vk::CommandBuffer cmd_buf, bool skinned = false) const;

    struct CommonVertDataDescription {
        std::vector<vk::VertexInputBindingDescription> binding_descs;
        std::vector<vk::VertexInputAttributeDescription> attr_descs;
    };
    static CommonVertDataDescription getDescription();
    static CommonVertDataDescription getSkinnedDescription();
    size_t allocatedIndexCountForTesting() const;
    size_t allocatedVertexCountForTesting(bool skinned = false) const;
};

} // namespace Pelican
