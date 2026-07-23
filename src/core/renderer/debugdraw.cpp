#include "debugdraw.hpp"
#include "frameresources.hpp"

#include "../shader/pelican_sets.hpp"
#include "../vkcore/core.hpp"
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

vk::UniqueDescriptorPool createDescriptorPool(vk::Device device) {
    vk::DescriptorPoolSize pool_size;
    pool_size.type = vk::DescriptorType::eStorageBuffer;
    pool_size.descriptorCount = 32;

    vk::DescriptorPoolCreateInfo create_info;
    create_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    create_info.maxSets = 32;
    create_info.poolSizeCount = 1;
    create_info.pPoolSizes = &pool_size;
    return device.createDescriptorPoolUnique(create_info);
}

vk::DeviceSize nextCapacity(vk::DeviceSize required) {
    vk::DeviceSize capacity = sizeof(DebugDrawVertex) * 64;
    while (capacity < required) {
        capacity *= 2;
    }
    return capacity;
}

} // namespace

DebugDraw::DebugDraw() = default;
DebugDraw::~DebugDraw() = default;

void DebugDraw::ensureDevice() {
    if (!device) {
        device = GET_MODULE(VulkanManageCore).getDevice();
    }
}

void DebugDraw::ensureDescriptorPool() {
    ensureDevice();
    if (!descriptor_pool) {
        descriptor_pool = createDescriptorPool(device);
    }
}

void DebugDraw::ensureVertexCapacity(size_t vertex_count) {
    const auto required_bytes =
        static_cast<vk::DeviceSize>(vertex_count * sizeof(DebugDrawVertex));
    if (required_bytes <= vertex_buffer_bytes) {
        return;
    }

    auto &vkcore = GET_MODULE(VulkanManageCore);
    vertex_buffer_bytes = nextCapacity(required_bytes);
    vertex_buffer = vkcore.allocBuf(vertex_buffer_bytes, vk::BufferUsageFlagBits::eStorageBuffer,
                                    vma::MemoryUsage::eAutoPreferHost,
                                    vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

void DebugDraw::ensureDescriptorSet(PassId pass_id, PipelineRecord &record) {
    if (record.descriptor_set) {
        return;
    }

    ensureDescriptorPool();
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto layout = pipeline_factory.descriptorSetLayout(record.pipeline, PELICAN_SET_FREE);

    vk::DescriptorSetAllocateInfo alloc_info;
    alloc_info.descriptorPool = descriptor_pool.get();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &layout;

    auto descriptor_sets = device.allocateDescriptorSetsUnique(alloc_info);
    if (descriptor_sets.empty()) {
        throw std::runtime_error("DebugDraw descriptor set allocation failed");
    }
    record.descriptor_set = std::move(descriptor_sets.front());
    (void)pass_id;
}

void DebugDraw::updateDescriptorSet(const PipelineRecord &record, vk::DeviceSize bytes) {
    vk::DescriptorBufferInfo buffer_info;
    buffer_info.buffer = vertex_buffer.buffer.get();
    buffer_info.offset = 0;
    buffer_info.range = bytes;

    vk::WriteDescriptorSet write;
    write.dstSet = record.descriptor_set.get();
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eStorageBuffer;
    write.pBufferInfo = &buffer_info;
    device.updateDescriptorSets(write, {});
}

PassId DebugDraw::registerPass(vk::Format color_format, ShaderBundleId vert_shader,
                               ShaderBundleId frag_shader,
                               std::vector<std::string> shader_defines,
                               vk::SampleCountFlagBits samples) {
    ensureDevice();
    enabled = true;

    GraphicsPipelineDesc desc;
    desc.vert = vert_shader;
    desc.frag = frag_shader;
    desc.color_formats = {color_format};
    desc.shader_defines = std::move(shader_defines);
    desc.blend = true;
    desc.src_color_blend_factor = vk::BlendFactor::eSrcAlpha;
    desc.dst_color_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    desc.src_alpha_blend_factor = vk::BlendFactor::eOne;
    desc.dst_alpha_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    desc.topology = vk::PrimitiveTopology::eLineList;
    desc.rasterization_samples = samples;

    const auto pass_id = PassId{static_cast<int>(pipelines.size())};
    pipelines.emplace(pass_id, PipelineRecord{GET_MODULE(PipelineFactory).create(desc), {}});
    return pass_id;
}

void DebugDraw::line(glm::vec3 from_ndc, glm::vec3 to_ndc, glm::vec4 color) {
    line(from_ndc, to_ndc, color, color);
}

void DebugDraw::line(glm::vec3 from_ndc, glm::vec3 to_ndc, glm::vec4 from_color, glm::vec4 to_color) {
    if (!enabled) {
        return;
    }
    vertices.push_back(DebugDrawVertex{glm::vec4{from_ndc, 1.0f}, from_color});
    vertices.push_back(DebugDrawVertex{glm::vec4{to_ndc, 1.0f}, to_color});
}

void DebugDraw::clear() {
    vertices.clear();
}

void DebugDraw::render(vk::CommandBuffer cmd_buf, PassId pass_id, const FrameResources &frame_resources) {
    if (!enabled || vertices.empty()) {
        return;
    }

    auto found = pipelines.find(pass_id);
    if (found == pipelines.end()) {
        throw std::runtime_error("DebugDraw pass pipeline not found");
    }

    const auto bytes = static_cast<vk::DeviceSize>(vertices.size() * sizeof(DebugDrawVertex));
    ensureVertexCapacity(vertices.size());
    ensureDescriptorSet(pass_id, found->second);
    GET_MODULE(VulkanManageCore).writeBuf(vertex_buffer, vertices.data(), 0, bytes);
    updateDescriptorSet(found->second, bytes);

    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                         pipeline_factory.pipeline(found->second.pipeline));
    frame_resources.bindGraphics(cmd_buf, pipeline_factory.layout(found->second.pipeline));
    cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                               pipeline_factory.layout(found->second.pipeline),
                               PELICAN_SET_FREE, found->second.descriptor_set.get(), {});
    cmd_buf.draw(static_cast<uint32_t>(vertices.size()), 1, 0, 0);
    vertices.clear();
}

} // namespace Pelican
