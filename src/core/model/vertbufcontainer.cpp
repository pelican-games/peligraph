#include "vertbufcontainer.hpp"
#include "../vkcore/core.hpp"

namespace Pelican {

constexpr uint32_t initial_indices_num = 65536 * 2;
constexpr uint32_t initial_vertices_num = 32768 * 2;

static BufferWrapper createIndexBuf(VulkanManageCore &vkcore, size_t num) {
    return vkcore.allocBuf(sizeof(uint32_t) * num, vk::BufferUsageFlagBits::eIndexBuffer,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

static BufferWrapper createVertBuf(VulkanManageCore &vkcore, size_t num) {
    size_t vert_sz = sizeof(glm::vec3)      // pos
                     + sizeof(glm::vec3)    // normal
                     + sizeof(glm::vec2)    // texcoord
                     + sizeof(glm::vec3)    // color
                     + sizeof(glm::i16vec4) // joint
                     + sizeof(glm::vec4)    // weight
        ;
    return vkcore.allocBuf(vert_sz * num, vk::BufferUsageFlagBits::eVertexBuffer, vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

VertBufContainer::VertBufContainer(DependencyContainer &_con)
    : con{_con}, indices_offset{0}, vertices_offset{0}, indices_cap{initial_indices_num},
      vertices_cap{initial_vertices_num}, indices_mem_pool{createIndexBuf(con.get<VulkanManageCore>(), indices_cap)},
      vertices_mem_pool{createVertBuf(con.get<VulkanManageCore>(), vertices_cap)} {}

ModelTemplate::PrimitiveRefInfo VertBufContainer::addPrimitiveEntry(CommonPolygonVertData &&data) {
    ModelTemplate::PrimitiveRefInfo info{
        .index_count = static_cast<uint32_t>(data.indices.size()),
        .index_offset = indices_offset,
        .vert_offset = vertices_offset,
    };
    uint32_t vert_count = static_cast<uint32_t>(data.pos.size());
    if (info.index_offset + info.index_count >= indices_cap) {
        LOG_ERROR(logger, "VertBufContainer: buffer overflow {} >= {}", info.index_offset + info.index_count,
                  indices_cap);
        throw std::runtime_error("VertBufContainer: buffer overflow");
    }
    if (info.vert_offset + data.pos.size() >= vertices_cap) {
        LOG_ERROR(logger, "VertBufContainer: buffer overflow {} >= {}", info.vert_offset + data.pos.size(),
                  vertices_cap);
        throw std::runtime_error("VertBufContainer: buffer overflow");
    }
    con.get<VulkanManageCore>().writeBuf(indices_mem_pool, data.indices.data(), sizeof(uint32_t) * indices_offset,
                                         sizeof(uint32_t) * data.indices.size());
    con.get<VulkanManageCore>().writeBuf(vertices_mem_pool, data.pos.data(), sizeof(glm::vec3) * vertices_offset,
                                         sizeof(uint32_t) * data.indices.size());
    con.get<VulkanManageCore>().writeBuf(vertices_mem_pool, data.pos.data(),
                                         sizeof(glm::vec3) * vertices_cap + sizeof(glm::vec2) * vertices_offset,
                                         sizeof(uint32_t) * data.indices.size());
    indices_offset += info.index_count;
    vertices_offset += vert_count;
    return info;
}
void VertBufContainer::removePrimitiveEntry(/* TODO */) {}

void VertBufContainer::bindVertexBuffer(vk::CommandBuffer cmd_buf) const {
    cmd_buf.bindIndexBuffer(*indices_mem_pool.buffer, 0, vk::IndexType::eUint32);
    cmd_buf.bindVertexBuffers(0, {*vertices_mem_pool.buffer, *vertices_mem_pool.buffer},
                              {0, sizeof(glm::vec3) * vertices_cap});
}

VertBufContainer::CommonVertDataDescription VertBufContainer::getDescription() {
    VertBufContainer::CommonVertDataDescription descs;

    vk::VertexInputBindingDescription pos_binding;
    pos_binding.binding = 0;
    pos_binding.inputRate = vk::VertexInputRate::eVertex;
    pos_binding.stride = sizeof(glm::vec3);
    descs.binding_descs.push_back(pos_binding);

    vk::VertexInputAttributeDescription pos_attr;
    pos_attr.binding = 0;
    pos_attr.location = 0;
    pos_attr.offset = 0;
    pos_attr.format = vk::Format::eR32G32B32Sfloat;
    descs.attr_descs.push_back(pos_attr);

    vk::VertexInputBindingDescription texcoord_binding;
    texcoord_binding.binding = 1;
    texcoord_binding.inputRate = vk::VertexInputRate::eVertex;
    texcoord_binding.stride = sizeof(glm::vec2);
    descs.binding_descs.push_back(texcoord_binding);

    vk::VertexInputAttributeDescription texcoord_attr;
    texcoord_attr.binding = 1;
    texcoord_attr.location = 1;
    texcoord_attr.offset = 0;
    texcoord_attr.format = vk::Format::eR32G32Sfloat;
    descs.attr_descs.push_back(texcoord_attr);

    return descs;
}

} // namespace Pelican
