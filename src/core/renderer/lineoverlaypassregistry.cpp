#include "lineoverlaypassregistry.hpp"
#include "frameresources.hpp"

#include "../container.hpp"
#include "../shader/pelican_sets.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

vk::UniqueDescriptorPool createDescriptorPool(vk::Device device,
                                              bool sampled_image) {
    std::array<vk::DescriptorPoolSize, 2> pool_sizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 32},
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, 32},
    };

    vk::DescriptorPoolCreateInfo create_info;
    create_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    create_info.maxSets = 32;
    create_info.poolSizeCount = sampled_image ? 2u : 1u;
    create_info.pPoolSizes = pool_sizes.data();
    return device.createDescriptorPoolUnique(create_info);
}

} // namespace

void LineOverlayPassRegistry::ensureDevice() {
    if (!device_) {
        device_ = GET_MODULE(VulkanManageCore).getDevice();
    }
}

vk::Device LineOverlayPassRegistry::device() {
    ensureDevice();
    return device_;
}

void LineOverlayPassRegistry::ensureDescriptorPool() {
    ensureDevice();
    if (!descriptor_pool_) {
        descriptor_pool_ =
            createDescriptorPool(device_, traits_.sampled_image);
    }
}

void LineOverlayPassRegistry::ensureVertexCapacity(std::size_t vertex_count) {
    const auto required_bytes = static_cast<vk::DeviceSize>(
        static_cast<vk::DeviceSize>(vertex_count) * traits_.vertex_stride);
    if (required_bytes <= vertex_buffer_bytes_) {
        return;
    }

    auto capacity = traits_.vertex_stride * traits_.initial_vertex_count;
    while (capacity < required_bytes) {
        capacity *= 2;
    }

    auto &vkcore = GET_MODULE(VulkanManageCore);
    vertex_buffer_bytes_ = capacity;
    vertex_buffer_ = vkcore.allocBuf(
        vertex_buffer_bytes_, vk::BufferUsageFlagBits::eStorageBuffer,
        vma::MemoryUsage::eAutoPreferHost,
        vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

void LineOverlayPassRegistry::ensureDescriptorSet(
    LineOverlayPipelineRecord &record) {
    if (record.descriptor_set) {
        return;
    }

    ensureDescriptorPool();
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto layout =
        pipeline_factory.descriptorSetLayout(record.pipeline, PELICAN_SET_FREE);

    vk::DescriptorSetAllocateInfo alloc_info;
    alloc_info.descriptorPool = descriptor_pool_.get();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &layout;

    auto descriptor_sets = device_.allocateDescriptorSetsUnique(alloc_info);
    if (descriptor_sets.empty()) {
        throw std::runtime_error(std::string{traits_.label} +
                                 " descriptor set allocation failed");
    }
    record.descriptor_set = std::move(descriptor_sets.front());
}

void LineOverlayPassRegistry::updateDescriptorSet(
    const LineOverlayPipelineRecord &record, vk::DeviceSize bytes) {
    vk::DescriptorBufferInfo buffer_info;
    buffer_info.buffer = vertex_buffer_.buffer.get();
    buffer_info.offset = 0;
    buffer_info.range = bytes;

    std::array<vk::WriteDescriptorSet, 2> writes{};
    writes[0].dstSet = record.descriptor_set.get();
    writes[0].dstBinding = 0;
    writes[0].dstArrayElement = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[0].pBufferInfo = &buffer_info;

    if (!traits_.sampled_image) {
        device_.updateDescriptorSets(writes[0], {});
        return;
    }

    if (!sampled_image_) {
        throw std::runtime_error(std::string{traits_.label} +
                                 " atlas descriptor was never published");
    }
    writes[1].dstSet = record.descriptor_set.get();
    writes[1].dstBinding = 1;
    writes[1].dstArrayElement = 0;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[1].pImageInfo = &*sampled_image_;

    device_.updateDescriptorSets(writes, {});
}

PassId LineOverlayPassRegistry::registerPass(
    vk::Format color_format, ShaderBundleId vert_shader,
    ShaderBundleId frag_shader, std::vector<std::string> shader_defines,
    vk::SampleCountFlagBits samples, bool ray_query,
    std::string acceleration_structure) {
    ensureDevice();
    if (next_pass_id_ == std::numeric_limits<int>::max()) {
        throw std::runtime_error(std::string{traits_.label} +
                                 " pass handle table is exhausted");
    }
    registration_order_.reserve(registration_order_.size() + 1);
    if (traits_.tracks_enabled) {
        enabled_ = true;
    }

    GraphicsPipelineDesc desc;
    desc.auxiliary_descriptor_contract = traits_.contract;
    desc.allow_ray_query = ray_query;
    desc.vert = vert_shader;
    desc.frag = frag_shader;
    desc.color_formats = {color_format};
    desc.shader_defines = std::move(shader_defines);
    desc.blend = true;
    desc.src_color_blend_factor = vk::BlendFactor::eSrcAlpha;
    desc.dst_color_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    desc.src_alpha_blend_factor = vk::BlendFactor::eOne;
    desc.dst_alpha_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    desc.topology = traits_.topology;
    desc.rasterization_samples = samples;

    const auto pass_id = PassId{next_pass_id_++};
    if (!pipelines_
             .emplace(pass_id,
                      LineOverlayPipelineRecord{
                          GET_MODULE(PipelineFactory).create(desc),
                          {},
                          std::move(acceleration_structure)})
             .second) {
        throw std::runtime_error(std::string{traits_.label} +
                                 " pipeline table changed during registration");
    }
    registration_order_.push_back(pass_id);
    return pass_id;
}

LineOverlayPipelineRecord *
LineOverlayPassRegistry::findPass(PassId pass_id) noexcept {
    const auto found = pipelines_.find(pass_id);
    return found == pipelines_.end() ? nullptr : &found->second;
}

void LineOverlayPassRegistry::drawVertices(
    vk::CommandBuffer cmd_buf, LineOverlayPipelineRecord &record,
    const void *vertices, std::size_t vertex_count,
    const FrameResources &frame_resources) {
    const auto bytes = static_cast<vk::DeviceSize>(
        static_cast<vk::DeviceSize>(vertex_count) * traits_.vertex_stride);
    ensureVertexCapacity(vertex_count);
    ensureDescriptorSet(record);
    GET_MODULE(VulkanManageCore).writeBuf(vertex_buffer_, vertices, 0, bytes);
    updateDescriptorSet(record, bytes);

    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                         pipeline_factory.pipeline(record.pipeline));
    frame_resources.bindGraphics(cmd_buf,
                                 pipeline_factory.layout(record.pipeline),
                                 record.acceleration_structure);
    cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                               pipeline_factory.layout(record.pipeline),
                               PELICAN_SET_FREE, record.descriptor_set.get(),
                               {});
    cmd_buf.draw(static_cast<std::uint32_t>(vertex_count), 1, 0, 0);
}

LineOverlayPassRegistry::Checkpoint
LineOverlayPassRegistry::checkpointRegistrations() const noexcept {
    return Checkpoint{registration_order_.size(), enabled_, next_pass_id_};
}

void LineOverlayPassRegistry::rollbackRegistrations(Checkpoint checkpoint) {
    if (checkpoint.registration_count > registration_order_.size()) {
        throw std::runtime_error(std::string{traits_.label} +
                                 " registration checkpoint is invalid");
    }
    while (registration_order_.size() > checkpoint.registration_count) {
        pipelines_.erase(registration_order_.back());
        registration_order_.pop_back();
    }
    enabled_ = checkpoint.enabled;
    next_pass_id_ = checkpoint.next_pass_id;
}

std::vector<PassId>
LineOverlayPassRegistry::registrationsSince(Checkpoint checkpoint) const {
    if (checkpoint.registration_count > registration_order_.size()) {
        throw std::runtime_error(std::string{traits_.label} +
                                 " registration checkpoint is invalid");
    }
    return {registration_order_.begin() +
                static_cast<std::ptrdiff_t>(checkpoint.registration_count),
            registration_order_.end()};
}

void LineOverlayPassRegistry::retireRegistrations(
    const std::vector<PassId> &ids) noexcept {
    for (const auto id : ids) {
        const auto found = pipelines_.find(id);
        if (found != pipelines_.end()) {
            auto retired = std::move(found->second);
            pipelines_.erase(found);
            try {
                auto *queue = FastModuleContainer::tryGet<DeletionQueue>();
                if (queue != nullptr && queue->acceptingResources()) {
                    queue->defer(std::move(retired));
                }
            } catch (...) {
            }
        }
        std::erase(registration_order_, id);
    }
    if (traits_.tracks_enabled) {
        enabled_ = !registration_order_.empty();
    }
}

} // namespace Pelican
