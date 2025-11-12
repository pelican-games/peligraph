#include "vertbufcontainer.hpp"
#include "../vkcore/core.hpp"
#include <numeric>

namespace Pelican {

constexpr uint32_t initial_indices_num = 65536 * 2;
constexpr uint32_t initial_vertices_num = 32768 * 2;

static BufferWrapper createIndexBuf(VulkanManageCore &vkcore, size_t num) {
    return vkcore.allocBuf(sizeof(uint32_t) * num, vk::BufferUsageFlagBits::eIndexBuffer,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

static BufferWrapper createVertBuf(VulkanManageCore &vkcore, size_t num) {
    return vkcore.allocBuf(sizeof(CommonVertStruct) * num, vk::BufferUsageFlagBits::eVertexBuffer,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

VertBufContainer::VertBufContainer(DependencyContainer &_con)
    : con{_con}, indices_offset{0}, vertices_offset{0}, indices_cap{initial_indices_num},
      vertices_cap{initial_vertices_num}, indices_mem_pool{createIndexBuf(con.get<VulkanManageCore>(), indices_cap)},
      vertices_mem_pool{createVertBuf(con.get<VulkanManageCore>(), vertices_cap)} {}

ModelTemplate::PrimitiveRefInfo VertBufContainer::addPrimitiveEntry(CommonPolygonVertData &&data) {
    uint32_t vert_count = static_cast<uint32_t>(data.pos.size());
    if (vert_count == 0) {
        LOG_ERROR(logger, "invalid primitive: empty vertices position data");
        throw std::runtime_error("invalid model data");
    }
    if (data.indices.empty()) {
        data.indices.resize(vert_count);
        std::iota(data.indices.begin(), data.indices.end(), 0);
    }
    ModelTemplate::PrimitiveRefInfo info{
        .index_count = static_cast<uint32_t>(data.indices.size()),
        .index_offset = indices_offset,
        .vert_offset = vertices_offset,
    };
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

    std::vector<CommonVertStruct> tmp_buf(vert_count);

    for (uint32_t i = 0; i < vert_count; i++)
        tmp_buf[i].pos = data.pos[i];

    if (!data.normal.empty()) {
        for (uint32_t i = 0; i < vert_count; i++)
            tmp_buf[i].normal = data.normal[i];
    } else {
        for (uint32_t i = 0; i < vert_count; i++)
            tmp_buf[i].normal = glm::vec3(0.0, 0.0, 0.0);
    }
    if (!data.texcoord.empty()) {
        for (uint32_t i = 0; i < vert_count; i++)
            tmp_buf[i].texcoord = data.texcoord[i];
    } else {
        for (uint32_t i = 0; i < vert_count; i++)
            tmp_buf[i].texcoord = glm::vec2(0.0, 0.0);
    }
    if (!data.color.empty()) {
        for (uint32_t i = 0; i < vert_count; i++)
            tmp_buf[i].color = data.color[i];
    } else {
        for (uint32_t i = 0; i < vert_count; i++)
            tmp_buf[i].color = glm::vec3(0.0, 0.0, 0.0);
    }

    con.get<VulkanManageCore>().writeBuf(indices_mem_pool, data.indices.data(), sizeof(uint32_t) * indices_offset,
                                         sizeof(uint32_t) * data.indices.size());
    con.get<VulkanManageCore>().writeBuf(vertices_mem_pool, tmp_buf.data(), sizeof(CommonVertStruct) * vertices_offset,
                                         sizeof(CommonVertStruct) * vert_count);
    indices_offset += info.index_count;
    vertices_offset += vert_count;
    return info;
}
void VertBufContainer::removePrimitiveEntry(/* TODO */) {}

void VertBufContainer::bindVertexBuffer(vk::CommandBuffer cmd_buf) const {
    cmd_buf.bindIndexBuffer(*indices_mem_pool.buffer, 0, vk::IndexType::eUint32);
    cmd_buf.bindVertexBuffers(0, {*vertices_mem_pool.buffer}, {0});
}

VertBufContainer::CommonVertDataDescription VertBufContainer::getDescription() {
    VertBufContainer::CommonVertDataDescription descs;

    vk::VertexInputBindingDescription pos_binding;
    pos_binding.binding = 0;
    pos_binding.inputRate = vk::VertexInputRate::eVertex;
    pos_binding.stride = sizeof(CommonVertStruct);
    descs.binding_descs.push_back(pos_binding);

    vk::VertexInputAttributeDescription pos_attr;
    pos_attr.binding = 0;
    pos_attr.location = 0;
    pos_attr.offset = offsetof(CommonVertStruct, pos);
    pos_attr.format = vk::Format::eR32G32B32Sfloat;
    descs.attr_descs.push_back(pos_attr);

    vk::VertexInputAttributeDescription normal_attr;
    normal_attr.binding = 0;
    normal_attr.location = 1;
    normal_attr.offset = offsetof(CommonVertStruct, normal);
    normal_attr.format = vk::Format::eR32G32B32Sfloat;
    descs.attr_descs.push_back(normal_attr);

    vk::VertexInputAttributeDescription texcoord_attr;
    texcoord_attr.binding = 0;
    texcoord_attr.location = 2;
    texcoord_attr.offset = offsetof(CommonVertStruct, texcoord);
    texcoord_attr.format = vk::Format::eR32G32Sfloat;
    descs.attr_descs.push_back(texcoord_attr);

    vk::VertexInputAttributeDescription color_attr;
    color_attr.binding = 0;
    color_attr.location = 3;
    color_attr.offset = offsetof(CommonVertStruct, color);
    color_attr.format = vk::Format::eR32G32B32Sfloat;
    descs.attr_descs.push_back(color_attr);

    return descs;
}

} // namespace Pelican
