#include "vertbufcontainer.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/util.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace Pelican {

constexpr uint32_t initial_indices_num = 65536;
constexpr uint32_t initial_vertices_num = 32768;

static BufferWrapper createIndexBuf(VulkanManageCore &vkcore, size_t num) {
    return vkcore.allocBuf(sizeof(uint32_t) * num,
                           vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                               vk::BufferUsageFlagBits::eTransferDst,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

static BufferWrapper createVertBuf(VulkanManageCore &vkcore, size_t num) {
    return vkcore.allocBuf(sizeof(CommonVertStruct) * num,
                           vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                               vk::BufferUsageFlagBits::eTransferDst,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

static BufferWrapper createSkinVertBuf(VulkanManageCore &vkcore, size_t num) {
    return vkcore.allocBuf(sizeof(CommonSkinningVertStruct) * num,
                           vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                               vk::BufferUsageFlagBits::eTransferDst,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

static BufferWrapper createMorphMetadataBuf(VulkanManageCore &vkcore) {
    return vkcore.allocBuf(sizeof(MorphVertexGpuMetadata) * maxMorphVerticesPerPool,
                           vk::BufferUsageFlagBits::eStorageBuffer |
                               vk::BufferUsageFlagBits::eTransferSrc |
                               vk::BufferUsageFlagBits::eTransferDst,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

static BufferWrapper createMorphDeltaBuf(VulkanManageCore &vkcore) {
    return vkcore.allocBuf(sizeof(MorphDeltaGpuData) * maxMorphDeltaRecords,
                           vk::BufferUsageFlagBits::eStorageBuffer |
                               vk::BufferUsageFlagBits::eTransferSrc |
                               vk::BufferUsageFlagBits::eTransferDst,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

VertBufContainer::VertBufContainer()
    : indices_offset{0}, vertices_offset{0}, indices_cap{initial_indices_num},
      vertices_cap{initial_vertices_num}, skin_vertices_offset{0}, skin_vertices_cap{initial_vertices_num},
      indices_mem_pool{createIndexBuf(GET_MODULE(VulkanManageCore), indices_cap)},
      vertices_mem_pool{createVertBuf(GET_MODULE(VulkanManageCore), vertices_cap)},
      skin_vertices_mem_pool{createSkinVertBuf(GET_MODULE(VulkanManageCore), skin_vertices_cap)},
      morph_static_metadata_buffer{createMorphMetadataBuf(GET_MODULE(VulkanManageCore))},
      morph_skinned_metadata_buffer{createMorphMetadataBuf(GET_MODULE(VulkanManageCore))},
      morph_delta_buffer{createMorphDeltaBuf(GET_MODULE(VulkanManageCore))} {
    const std::vector<MorphVertexGpuMetadata> empty(maxMorphVerticesPerPool);
    auto &vkcore = GET_MODULE(VulkanManageCore);
    vkcore.writeBuf(morph_static_metadata_buffer, empty.data(), 0,
                    empty.size() * sizeof(MorphVertexGpuMetadata));
    vkcore.writeBuf(morph_skinned_metadata_buffer, empty.data(), 0,
                    empty.size() * sizeof(MorphVertexGpuMetadata));
}

VertBufContainer::~VertBufContainer() {
    deferred_callbacks.closeAndWait();
}

uint32_t VertBufContainer::allocateRange(std::vector<FreeRange> &free_ranges,
                                         uint32_t &high_water, uint32_t count) {
    if (count == 0) throw std::runtime_error("cannot allocate an empty model buffer range");
    for (auto it = free_ranges.begin(); it != free_ranges.end(); ++it) {
        if (it->size < count) continue;
        const auto offset = it->offset;
        it->offset += count;
        it->size -= count;
        if (it->size == 0) free_ranges.erase(it);
        return offset;
    }
    if (count > std::numeric_limits<uint32_t>::max() - high_water)
        throw std::runtime_error("model buffer address space exhausted");
    const auto offset = high_water;
    high_water += count;
    return offset;
}

void VertBufContainer::releaseRange(std::vector<FreeRange> &free_ranges,
                                    uint32_t offset, uint32_t count) {
    if (count == 0) return;
    free_ranges.push_back({offset, count});
    std::sort(free_ranges.begin(), free_ranges.end(), [](const auto &left, const auto &right) {
        return left.offset < right.offset;
    });
    std::vector<FreeRange> merged;
    merged.reserve(free_ranges.size());
    for (const auto range : free_ranges) {
        if (!merged.empty() && merged.back().offset + merged.back().size >= range.offset) {
            const auto end = std::max(merged.back().offset + merged.back().size,
                                      range.offset + range.size);
            merged.back().size = end - merged.back().offset;
        } else {
            merged.push_back(range);
        }
    }
    free_ranges = std::move(merged);
}

namespace {
void deferOldBuffer(BufferWrapper buffer) {
    if (auto *queue = FastModuleContainer::tryGet<DeletionQueue>()) {
        queue->defer(std::move(buffer));
    }
}

void validateVertexStreams(const CommonPolygonVertData &data, bool skinned) {
    const auto count = data.pos.size();
    if (count == 0 || count > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("invalid primitive: POSITION stream is empty or too large");
    const auto optionalMatches = [count](std::size_t size) { return size == 0 || size == count; };
    if (!optionalMatches(data.normal.size()) || !optionalMatches(data.tangent.size()) ||
        !optionalMatches(data.texcoord.size()) || !optionalMatches(data.color.size())) {
        throw std::runtime_error("invalid primitive: vertex attribute counts do not match POSITION");
    }
    if (skinned && (data.joint.size() != count || data.weight.size() != count)) {
        throw std::runtime_error(
            "invalid skinned primitive: POSITION, JOINTS_0, and WEIGHTS_0 counts must match");
    }
    if (data.morph_targets.size() > maxMorphTargetsPerPrimitive)
        throw std::runtime_error("glTF morph target count exceeds renderer limit of " +
                                 std::to_string(maxMorphTargetsPerPrimitive));
    if (data.morph_weight_offset > maxMorphWeightsPerInstance ||
        data.morph_targets.size() >
            maxMorphWeightsPerInstance - data.morph_weight_offset)
        throw std::runtime_error("glTF morph layout exceeds per-instance weight capacity of " +
                                 std::to_string(maxMorphWeightsPerInstance));
    for (const auto &target : data.morph_targets) {
        if (!optionalMatches(target.position.size()) ||
            !optionalMatches(target.normal.size()) ||
            !optionalMatches(target.tangent.size()))
            throw std::runtime_error(
                "invalid glTF morph target: delta accessor count does not match POSITION");
    }
}
} // namespace

std::vector<MorphTargetDeltaRange>
VertBufContainer::uploadMorphData(const CommonPolygonVertData &data,
                                  uint32_t vertex_offset, bool skinned,
                                  uint32_t &delta_offset, uint32_t &delta_count) {
    delta_offset = 0;
    delta_count = 0;
    if (data.morph_targets.empty()) return {};
    const auto vertex_count = static_cast<uint32_t>(data.pos.size());
    if (vertex_offset > maxMorphVerticesPerPool ||
        vertex_count > maxMorphVerticesPerPool - vertex_offset)
        throw std::runtime_error("glTF morph vertex metadata exceeds renderer capacity of " +
                                 std::to_string(maxMorphVerticesPerPool) +
                                 (skinned ? " skinned vertices" : " static vertices"));
    const auto record_count64 = static_cast<std::uint64_t>(vertex_count) *
                                data.morph_targets.size();
    if (record_count64 > maxMorphDeltaRecords)
        throw std::runtime_error("glTF morph delta range exceeds renderer capacity of " +
                                 std::to_string(maxMorphDeltaRecords) + " records");
    delta_count = static_cast<uint32_t>(record_count64);
    delta_offset = allocateRange(free_morph_deltas, morph_delta_offset, delta_count);
    if (delta_offset > maxMorphDeltaRecords ||
        delta_count > maxMorphDeltaRecords - delta_offset) {
        releaseRange(free_morph_deltas, delta_offset, delta_count);
        delta_offset = 0;
        delta_count = 0;
        throw std::runtime_error("shared glTF morph delta buffer capacity exceeded (" +
                                 std::to_string(maxMorphDeltaRecords) + " records)");
    }

    try {
        std::vector<MorphDeltaGpuData> deltas(delta_count);
        std::vector<MorphTargetDeltaRange> ranges;
        ranges.reserve(data.morph_targets.size());
        for (uint32_t target_index = 0;
             target_index < static_cast<uint32_t>(data.morph_targets.size());
             ++target_index) {
            const auto &source = data.morph_targets[target_index];
            const auto target_offset = delta_offset + target_index * vertex_count;
            ranges.push_back({target_index, target_offset, vertex_count,
                              source.presence_mask});
            for (uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
                auto &destination = deltas[target_index * vertex_count + vertex];
                if (!source.position.empty())
                    destination.position = glm::vec4{source.position[vertex], 0.0f};
                if (!source.normal.empty())
                    destination.normal = glm::vec4{source.normal[vertex], 0.0f};
                if (!source.tangent.empty())
                    destination.tangent = glm::vec4{source.tangent[vertex], 0.0f};
            }
        }
        std::vector<MorphVertexGpuMetadata> metadata(vertex_count);
        for (uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
            metadata[vertex] = {
                delta_offset + vertex,
                vertex_count,
                data.morph_weight_offset,
                static_cast<uint32_t>(data.morph_targets.size()),
            };
        }
        auto &vkcore = GET_MODULE(VulkanManageCore);
        vkcore.writeBuf(morph_delta_buffer, deltas.data(),
                        sizeof(MorphDeltaGpuData) * delta_offset,
                        sizeof(MorphDeltaGpuData) * deltas.size());
        auto &metadata_buffer = skinned ? morph_skinned_metadata_buffer
                                        : morph_static_metadata_buffer;
        vkcore.writeBuf(metadata_buffer, metadata.data(),
                        sizeof(MorphVertexGpuMetadata) * vertex_offset,
                        sizeof(MorphVertexGpuMetadata) * metadata.size());
        return ranges;
    } catch (...) {
        releaseRange(free_morph_deltas, delta_offset, delta_count);
        delta_offset = 0;
        delta_count = 0;
        throw;
    }
}

void VertBufContainer::ensureIndexCapacity(uint32_t required) {
    if (required <= indices_cap) return;
    const auto old_cap = indices_cap;
    auto replacement_cap = indices_cap;
    while (required > replacement_cap) {
        if (replacement_cap > std::numeric_limits<uint32_t>::max() / 2)
            throw std::runtime_error("model index buffer capacity exhausted");
        replacement_cap <<= 1;
    }
    auto replacement = createIndexBuf(GET_MODULE(VulkanManageCore), replacement_cap);
    GET_MODULE(VulkanUtils).bufferCopy(indices_mem_pool, replacement, 0, 0,
                                       sizeof(uint32_t) * old_cap);
    auto old = std::move(indices_mem_pool);
    indices_mem_pool = std::move(replacement);
    indices_cap = replacement_cap;
    deferOldBuffer(std::move(old));
    LOG_INFO(logger, "VertBufContainer: reallocated index buffer");
}

void VertBufContainer::ensureVertexCapacity(uint32_t required, bool skinned) {
    auto &capacity = skinned ? skin_vertices_cap : vertices_cap;
    if (required <= capacity) return;
    const auto old_cap = capacity;
    auto replacement_cap = capacity;
    while (required > replacement_cap) {
        if (replacement_cap > std::numeric_limits<uint32_t>::max() / 2)
            throw std::runtime_error("model vertex buffer capacity exhausted");
        replacement_cap <<= 1;
    }
    if (skinned) {
        auto replacement = createSkinVertBuf(GET_MODULE(VulkanManageCore), replacement_cap);
        GET_MODULE(VulkanUtils).bufferCopy(skin_vertices_mem_pool, replacement, 0, 0,
                                           sizeof(CommonSkinningVertStruct) * old_cap);
        auto old = std::move(skin_vertices_mem_pool);
        skin_vertices_mem_pool = std::move(replacement);
        capacity = replacement_cap;
        deferOldBuffer(std::move(old));
    } else {
        auto replacement = createVertBuf(GET_MODULE(VulkanManageCore), replacement_cap);
        GET_MODULE(VulkanUtils).bufferCopy(vertices_mem_pool, replacement, 0, 0,
                                           sizeof(CommonVertStruct) * old_cap);
        auto old = std::move(vertices_mem_pool);
        vertices_mem_pool = std::move(replacement);
        capacity = replacement_cap;
        deferOldBuffer(std::move(old));
    }
    LOG_INFO(logger, "VertBufContainer: reallocated {} vertex buffer",
             skinned ? "skinned" : "static");
}

ModelGeometryAllocation VertBufContainer::addPrimitiveAllocation(CommonPolygonVertData &&data) {
    validateVertexStreams(data, false);
    const auto vertex_count = static_cast<uint32_t>(data.pos.size());
    if (data.indices.empty()) {
        data.indices.resize(vertex_count);
        std::iota(data.indices.begin(), data.indices.end(), 0);
    }
    if (data.indices.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("model primitive index stream is too large");
    const auto index_count = static_cast<uint32_t>(data.indices.size());
    const auto index_offset = allocateRange(free_indices, indices_offset, index_count);
    uint32_t vertex_offset = 0;
    uint32_t morph_offset = 0;
    uint32_t morph_count = 0;
    bool vertex_allocated = false;
    try {
        vertex_offset = allocateRange(free_vertices, vertices_offset, vertex_count);
        vertex_allocated = true;
        if (vertex_offset > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            throw std::runtime_error("model vertex offset exceeds Vulkan draw range");
        ensureIndexCapacity(index_offset + index_count);
        ensureVertexCapacity(vertex_offset + vertex_count, false);

        std::vector<CommonVertStruct> vertices(vertex_count);
        for (uint32_t i = 0; i < vertex_count; ++i) {
            vertices[i].pos = data.pos[i];
            vertices[i].normal = data.normal.empty() ? glm::vec3{0.0f} : data.normal[i];
            vertices[i].tangent = data.tangent.empty() ? glm::vec4{0.0f} : data.tangent[i];
            vertices[i].texcoord = data.texcoord.empty() ? glm::vec2{0.0f} : data.texcoord[i];
            vertices[i].color = data.color.empty() ? glm::vec4{1.0f} : data.color[i];
        }
        GET_MODULE(VulkanManageCore).writeBuf(indices_mem_pool, data.indices.data(),
                                              sizeof(uint32_t) * index_offset,
                                              sizeof(uint32_t) * index_count);
        GET_MODULE(VulkanManageCore).writeBuf(vertices_mem_pool, vertices.data(),
                                              sizeof(CommonVertStruct) * vertex_offset,
                                              sizeof(CommonVertStruct) * vertex_count);
        (void)uploadMorphData(data, vertex_offset, false, morph_offset, morph_count);
    } catch (...) {
        releaseRange(free_indices, index_offset, index_count);
        if (vertex_allocated) releaseRange(free_vertices, vertex_offset, vertex_count);
        throw;
    }
    return {{index_count, index_offset, static_cast<int32_t>(vertex_offset), false},
            vertex_count, morph_offset, morph_count};
}

ModelGeometryAllocation VertBufContainer::addSkinnedPrimitiveAllocation(CommonPolygonVertData &&data) {
    validateVertexStreams(data, true);
    const auto vertex_count = static_cast<uint32_t>(data.pos.size());
    if (data.indices.empty()) {
        data.indices.resize(vertex_count);
        std::iota(data.indices.begin(), data.indices.end(), 0);
    }
    if (data.indices.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("model primitive index stream is too large");
    const auto index_count = static_cast<uint32_t>(data.indices.size());
    const auto index_offset = allocateRange(free_indices, indices_offset, index_count);
    uint32_t vertex_offset = 0;
    uint32_t morph_offset = 0;
    uint32_t morph_count = 0;
    bool vertex_allocated = false;
    try {
        vertex_offset = allocateRange(free_skin_vertices, skin_vertices_offset, vertex_count);
        vertex_allocated = true;
        if (vertex_offset > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            throw std::runtime_error("skinned model vertex offset exceeds Vulkan draw range");
        ensureIndexCapacity(index_offset + index_count);
        ensureVertexCapacity(vertex_offset + vertex_count, true);

        std::vector<CommonSkinningVertStruct> vertices(vertex_count);
        for (uint32_t i = 0; i < vertex_count; ++i) {
            auto &vertex = vertices[i];
            vertex.pos = data.pos[i];
            vertex.normal = data.normal.empty() ? glm::vec3{0.0f} : data.normal[i];
            vertex.tangent = data.tangent.empty() ? glm::vec4{0.0f} : data.tangent[i];
            vertex.texcoord = data.texcoord.empty() ? glm::vec2{0.0f} : data.texcoord[i];
            vertex.color = data.color.empty() ? glm::vec4{1.0f} : data.color[i];
            vertex.joint = data.joint[i];
            vertex.weight = data.weight[i];
            const float sum = vertex.weight.x + vertex.weight.y + vertex.weight.z + vertex.weight.w;
            if (sum > 0.0f) vertex.weight /= sum;
        }
        GET_MODULE(VulkanManageCore).writeBuf(indices_mem_pool, data.indices.data(),
                                              sizeof(uint32_t) * index_offset,
                                              sizeof(uint32_t) * index_count);
        GET_MODULE(VulkanManageCore).writeBuf(skin_vertices_mem_pool, vertices.data(),
                                              sizeof(CommonSkinningVertStruct) * vertex_offset,
                                              sizeof(CommonSkinningVertStruct) * vertex_count);
        (void)uploadMorphData(data, vertex_offset, true, morph_offset, morph_count);
    } catch (...) {
        releaseRange(free_indices, index_offset, index_count);
        if (vertex_allocated)
            releaseRange(free_skin_vertices, vertex_offset, vertex_count);
        throw;
    }
    return {{index_count, index_offset, static_cast<int32_t>(vertex_offset), true},
            vertex_count, morph_offset, morph_count};
}

ModelTemplate::PrimitiveRefInfo VertBufContainer::addPrimitiveEntry(CommonPolygonVertData &&data) {
    return addPrimitiveAllocation(std::move(data)).primitive;
}

ModelTemplate::PrimitiveRefInfo VertBufContainer::addSkinnedPrimitiveEntry(CommonPolygonVertData &&data) {
    return addSkinnedPrimitiveAllocation(std::move(data)).primitive;
}

void VertBufContainer::releaseGeometryNow(
    std::span<const ModelGeometryAllocation> allocations) noexcept {
    try {
        for (const auto &allocation : allocations) {
            releaseRange(free_indices, allocation.primitive.index_offset,
                         allocation.primitive.index_count);
            releaseRange(allocation.primitive.skinned ? free_skin_vertices : free_vertices,
                         static_cast<uint32_t>(allocation.primitive.vert_offset),
                         allocation.vertex_count);
            if (allocation.morph_delta_count != 0) {
                const std::vector<MorphVertexGpuMetadata> empty(allocation.vertex_count);
                auto &metadata_buffer = allocation.primitive.skinned
                                            ? morph_skinned_metadata_buffer
                                            : morph_static_metadata_buffer;
                GET_MODULE(VulkanManageCore).writeBuf(
                    metadata_buffer, empty.data(),
                    sizeof(MorphVertexGpuMetadata) * allocation.primitive.vert_offset,
                    sizeof(MorphVertexGpuMetadata) * empty.size());
                releaseRange(free_morph_deltas, allocation.morph_delta_offset,
                             allocation.morph_delta_count);
            }
        }
    } catch (...) {
        if (logger) LOG_ERROR(logger, "failed to return retired model geometry ranges");
    }
}

void VertBufContainer::releaseModelGeometry(
    std::vector<ModelGeometryAllocation> allocations, bool deferred) noexcept {
    if (allocations.empty()) return;
    if (deferred) {
        struct DeferredRelease {
            VertBufContainer *owner{};
            DeferredCallbackLifetime::Callback callback;
            std::vector<ModelGeometryAllocation> allocations;
            DeferredRelease(VertBufContainer *value,
                            DeferredCallbackLifetime::Callback callback_handle,
                            std::vector<ModelGeometryAllocation> ranges)
                : owner{value}, callback{std::move(callback_handle)},
                  allocations{std::move(ranges)} {}
            DeferredRelease(DeferredRelease &&other) noexcept
                : owner{std::exchange(other.owner, nullptr)},
                  callback{std::move(other.callback)},
                  allocations{std::move(other.allocations)} {}
            DeferredRelease &operator=(DeferredRelease &&) = delete;
            DeferredRelease(const DeferredRelease &) = delete;
            ~DeferredRelease() {
                if (!owner) return;
                const auto lease = callback.acquire();
                if (lease) owner->releaseModelGeometry(std::move(allocations), false);
            }
        };
        if (auto *queue = FastModuleContainer::tryGet<DeletionQueue>()) {
            queue->defer(DeferredRelease{this, deferred_callbacks.callback(),
                                         std::move(allocations)});
            return;
        }
    }
    releaseGeometryNow(allocations);
}

size_t VertBufContainer::allocatedIndexCountForTesting() const {
    size_t free = 0;
    for (const auto range : free_indices) free += range.size;
    return indices_offset - free;
}

size_t VertBufContainer::allocatedVertexCountForTesting(bool skinned) const {
    const auto &ranges = skinned ? free_skin_vertices : free_vertices;
    size_t free = 0;
    for (const auto range : ranges) free += range.size;
    return (skinned ? skin_vertices_offset : vertices_offset) - free;
}

size_t VertBufContainer::allocatedMorphDeltaCountForTesting() const {
    size_t free = 0;
    for (const auto range : free_morph_deltas) free += range.size;
    return morph_delta_offset - free;
}

void VertBufContainer::bindVertexBuffer(vk::CommandBuffer cmd_buf, bool skinned) const {
    cmd_buf.bindIndexBuffer(*indices_mem_pool.buffer, 0, vk::IndexType::eUint32);
    cmd_buf.bindVertexBuffers(0, {skinned ? *skin_vertices_mem_pool.buffer : *vertices_mem_pool.buffer}, {0});
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
    color_attr.format = vk::Format::eR32G32B32A32Sfloat;
    descs.attr_descs.push_back(color_attr);

    vk::VertexInputAttributeDescription tangent_attr;
    tangent_attr.binding = 0;
    tangent_attr.location = 4;
    tangent_attr.offset = offsetof(CommonVertStruct, tangent);
    tangent_attr.format = vk::Format::eR32G32B32A32Sfloat;
    descs.attr_descs.push_back(tangent_attr);

    return descs;
}

VertBufContainer::CommonVertDataDescription VertBufContainer::getSkinnedDescription() {
    CommonVertDataDescription descs;
    descs.binding_descs.push_back(vk::VertexInputBindingDescription{
        0, sizeof(CommonSkinningVertStruct), vk::VertexInputRate::eVertex});
    const auto add = [&](uint32_t location, vk::Format format, uint32_t offset) {
        descs.attr_descs.push_back(vk::VertexInputAttributeDescription{location, 0, format, offset});
    };
    add(0, vk::Format::eR32G32B32Sfloat, offsetof(CommonSkinningVertStruct, pos));
    add(1, vk::Format::eR32G32B32Sfloat, offsetof(CommonSkinningVertStruct, normal));
    add(2, vk::Format::eR32G32Sfloat, offsetof(CommonSkinningVertStruct, texcoord));
    add(3, vk::Format::eR32G32B32A32Sfloat, offsetof(CommonSkinningVertStruct, color));
    add(4, vk::Format::eR32G32B32A32Sfloat, offsetof(CommonSkinningVertStruct, tangent));
    add(5, vk::Format::eR16G16B16A16Sint, offsetof(CommonSkinningVertStruct, joint));
    add(6, vk::Format::eR32G32B32A32Sfloat, offsetof(CommonSkinningVertStruct, weight));
    return descs;
}

} // namespace Pelican
