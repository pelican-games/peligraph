#include "polygoninstancecontainer.hpp"

#include "../loader/engineresources.hpp"
#include "../material/materialcontainer.hpp"
#include "../appflow/enginetime.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../vkcore/core.hpp"
#include <array>
#include <cstring>
#include <stdexcept>

namespace Pelican {

struct RayDeformationPipeline {
    vk::UniqueDescriptorSetLayout empty_layout;
    vk::UniqueDescriptorSetLayout descriptor_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
};

namespace {

struct DeformationJob {
    std::uint32_t vertex_offset;
    std::uint32_t vertex_count;
    std::uint32_t instance_index;
    std::uint32_t skinned;
    std::uint32_t source_stride_words;
    std::uint32_t joint_offset_words;
    std::uint32_t weight_offset_words;
    std::uint32_t vat;
    glm::vec4 vat_bounds_min_frame_count;
    glm::vec4 vat_bounds_extent_fps;
    glm::ivec4 vat_flags;
    float time;
};

static_assert(offsetof(DeformationJob, vat) == 28);
static_assert(offsetof(DeformationJob, vat_bounds_min_frame_count) == 32);
static_assert(offsetof(DeformationJob, time) == 80);
// Pushed as raw bytes (sizeof(job)): no tail padding after `time`.
static_assert(sizeof(DeformationJob) == 84);

// The frame's geometry snapshots retain this object until submission completes.
// Keeping the pool and pipeline here also covers reload and aborted publication.
struct DeformedGeometry {
    std::shared_ptr<RayDeformationPipeline> pipeline;
    std::shared_ptr<const void> source_lease;
    BufferWrapper vertices;
    vk::UniqueDescriptorPool descriptors;
};

std::shared_ptr<RayDeformationPipeline> createPipeline(vk::Device device) {
    auto result = std::make_shared<RayDeformationPipeline>();
    result->empty_layout = device.createDescriptorSetLayoutUnique({});
    std::array<vk::DescriptorSetLayoutBinding, 11> bindings;
    for (std::uint32_t binding = 0; binding < bindings.size(); ++binding) {
        bindings[binding] = vk::DescriptorSetLayoutBinding{
            binding, binding < 9 ? vk::DescriptorType::eStorageBuffer
                                 : vk::DescriptorType::eCombinedImageSampler, 1,
            vk::ShaderStageFlagBits::eCompute};
    }
    vk::DescriptorSetLayoutCreateInfo descriptor_info;
    descriptor_info.setBindings(bindings);
    result->descriptor_layout = device.createDescriptorSetLayoutUnique(descriptor_info);
    const std::array layouts{
        result->empty_layout.get(), result->empty_layout.get(),
        result->empty_layout.get(), result->descriptor_layout.get()};
    const vk::PushConstantRange push{
        vk::ShaderStageFlagBits::eCompute, 0, sizeof(DeformationJob)};
    vk::PipelineLayoutCreateInfo layout_info;
    layout_info.setSetLayouts(layouts).setPushConstantRanges(push);
    result->layout = device.createPipelineLayoutUnique(layout_info);

    const auto embedded = engineResourceOrThrow("ray_deform.comp.spv");
    std::vector<std::uint32_t> words(embedded.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), embedded.data(), embedded.size());
    vk::ShaderModuleCreateInfo module_info;
    module_info.setCode(words);
    auto module = device.createShaderModuleUnique(module_info);
    vk::ComputePipelineCreateInfo pipeline_info;
    pipeline_info.stage = vk::PipelineShaderStageCreateInfo{
        {}, vk::ShaderStageFlagBits::eCompute, module.get(), "main"};
    pipeline_info.layout = result->layout.get();
    auto pipeline = device.createComputePipelineUnique({}, pipeline_info);
    if (pipeline.result != vk::Result::eSuccess)
        throw std::runtime_error("failed to create ray geometry deformation pipeline");
    result->pipeline = std::move(pipeline.value);
    return result;
}

} // namespace

void PolygonInstanceContainer::recordGeometryDeformation(
    vk::CommandBuffer cmd_buf,
    std::vector<RayQueryGeometryInstanceSnapshot> &instances,
    bool for_acceleration_structures) {
    auto &core = GET_MODULE(VulkanManageCore);
    auto &geometry = GET_MODULE(VertBufContainer);
    const auto device = core.getDevice();
    bool dispatched = false;
    for (auto &instance : instances) {
        if (instance.deformed_vertex_buffer ||
            instance.geometry_kind != RayGeometryKind::triangles ||
            (!instance.vat_deformed && !instance.skinned && !instance.morph_deformed) ||
            instance.vertex_count == 0 || instance.vertex_offset < 0)
            continue;
        if (!ray_deformation_pipeline)
            ray_deformation_pipeline = createPipeline(device);

        auto output = std::make_shared<DeformedGeometry>();
        output->pipeline = ray_deformation_pipeline;
        output->source_lease = instance.geometry_lease;
        vk::BufferUsageFlags output_usage =
            vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc;
        if (for_acceleration_structures)
            output_usage |= vk::BufferUsageFlagBits::eShaderDeviceAddress |
                vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
        output->vertices = core.allocBuf(
            vk::DeviceSize{instance.vertex_count} * sizeof(CommonVertStruct),
            output_usage,
            vma::MemoryUsage::eAutoPreferDevice, {});
        const std::array pool_size{
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 9},
            vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, 2}};
        vk::DescriptorPoolCreateInfo pool_info;
        pool_info.maxSets = 1;
        pool_info.setPoolSizes(pool_size);
        output->descriptors = device.createDescriptorPoolUnique(pool_info);
        const auto set_layout = ray_deformation_pipeline->descriptor_layout.get();
        vk::DescriptorSetAllocateInfo allocate_info;
        allocate_info.descriptorPool = output->descriptors.get();
        allocate_info.setSetLayouts(set_layout);
        const auto set = device.allocateDescriptorSets(allocate_info).front();
        const std::array buffers{
            geometry.vertexBuffer(instance.skinned).buffer.get(),
            output->vertices.buffer.get(),
            skin_palette_buffer.buffer.get(),
            previous_skin_palette_buffer.buffer.get(),
            morph_instance_buffer.buffer.get(),
            morph_weight_buffer.buffer.get(),
            previous_morph_weight_buffer.buffer.get(),
            geometry.morphMetadataBuffer(instance.skinned).buffer.get(),
            geometry.morphDeltaBuffer().buffer.get()};
        std::array<vk::DescriptorBufferInfo, 9> buffer_infos;
        std::array<vk::WriteDescriptorSet, 11> writes;
        for (std::uint32_t binding = 0; binding < buffers.size(); ++binding) {
            buffer_infos[binding] = vk::DescriptorBufferInfo{buffers[binding], 0, VK_WHOLE_SIZE};
            writes[binding].dstSet = set;
            writes[binding].dstBinding = binding;
            writes[binding].descriptorType = vk::DescriptorType::eStorageBuffer;
            writes[binding].setBufferInfo(buffer_infos[binding]);
        }
        const auto material = GET_MODULE(MaterialContainer).rayDeformationMaterial(
            GlobalMaterialId{instance.resolved_material_handle});
        if (instance.vat_deformed && !material.vat)
            throw std::runtime_error("VAT ray geometry requires VAT playback material");
        for (std::uint32_t i = 0; i < 2; ++i) {
            writes[9 + i].dstSet = set;
            writes[9 + i].dstBinding = 9 + i;
            writes[9 + i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            writes[9 + i].setImageInfo(material.images[i]);
        }
        device.updateDescriptorSets(writes, {});
        DeformationJob job{
            static_cast<std::uint32_t>(instance.vertex_offset),
            instance.vertex_count, instance.instance_index,
            instance.skinned ? 1u : 0u,
            static_cast<std::uint32_t>((instance.skinned
                ? sizeof(CommonSkinningVertStruct) : sizeof(CommonVertStruct)) / 4),
            static_cast<std::uint32_t>(offsetof(CommonSkinningVertStruct, joint) / 4),
            static_cast<std::uint32_t>(offsetof(CommonSkinningVertStruct, weight) / 4)};
        job.vat = instance.vat_deformed ? 1u : 0u;
        job.time = static_cast<float>(GET_MODULE(EngineTime).now());
        if (job.vat) {
            const auto &vat = *material.vat;
            job.vat_bounds_min_frame_count = glm::vec4{vat.bounds_min, float(vat.frame_count)};
            job.vat_bounds_extent_fps = glm::vec4{vat.bounds_max - vat.bounds_min, vat.fps};
            job.vat_flags = glm::ivec4{vat.base_vertex, vat.loop, vat.has_normal, 0};
        }
        cmd_buf.bindPipeline(vk::PipelineBindPoint::eCompute,
                             ray_deformation_pipeline->pipeline.get());
        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
            ray_deformation_pipeline->layout.get(), 3, set, {});
        cmd_buf.pushConstants(ray_deformation_pipeline->layout.get(),
            vk::ShaderStageFlagBits::eCompute, 0, sizeof(job), &job);
        cmd_buf.dispatch((instance.vertex_count + 63u) / 64u, 1, 1);
        instance.deformed_vertex_buffer = output->vertices.buffer.get();
        if (for_acceleration_structures)
            instance.deformed_vertex_address = device.getBufferAddress(
                vk::BufferDeviceAddressInfo{instance.deformed_vertex_buffer});
        instance.deformation_source_geometry_id = instance.geometry_allocation_id;
        instance.geometry_allocation_id = geometry.reserveGeometryAllocationId();
        instance.geometry_lease = std::move(output);
        dispatched = true;
    }
    if (dispatched) {
        vk::AccessFlags consumer_access = vk::AccessFlagBits::eTransferRead |
            vk::AccessFlagBits::eShaderRead;
        vk::PipelineStageFlags consumer_stages = vk::PipelineStageFlagBits::eAllCommands;
        if (for_acceleration_structures) {
            consumer_access |= vk::AccessFlagBits::eAccelerationStructureReadKHR;
            consumer_stages |= vk::PipelineStageFlagBits::eAccelerationStructureBuildKHR;
        }
        const vk::MemoryBarrier barrier{
            vk::AccessFlagBits::eShaderWrite,
            consumer_access};
        cmd_buf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
            consumer_stages,
            {}, barrier, {}, {});
    }
}

} // namespace Pelican
