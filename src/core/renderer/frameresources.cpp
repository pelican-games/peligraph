#include "frameresources.hpp"

#include "../shader/pelican_sets.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/core.hpp"
#include <array>

namespace Pelican {

namespace {

vk::UniqueDescriptorPool createDescriptorPool(vk::Device device) {
    const std::array pool_sizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 2},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 2},
    };
    vk::DescriptorPoolCreateInfo create_info;
    create_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    create_info.maxSets = 1;
    create_info.setPoolSizes(pool_sizes);
    return device.createDescriptorPoolUnique(create_info);
}

} // namespace

FrameResources::FrameResources()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      frame_buffer{GET_MODULE(VulkanManageCore).allocBuf(
          sizeof(FrameUniformData), vk::BufferUsageFlagBits::eUniformBuffer, vma::MemoryUsage::eAuto,
          vma::AllocationCreateFlagBits::eHostAccessSequentialWrite)},
      descriptor_pool{createDescriptorPool(device)} {
    const auto layout = GET_MODULE(PipelineFactory).frameDescriptorSetLayout();
    vk::DescriptorSetAllocateInfo allocate_info;
    allocate_info.descriptorPool = descriptor_pool.get();
    allocate_info.setSetLayouts(layout);
    descriptor_set = std::move(device.allocateDescriptorSetsUnique(allocate_info).front());

    vk::DescriptorBufferInfo frame_info{frame_buffer.buffer.get(), 0, sizeof(FrameUniformData)};
    vk::WriteDescriptorSet write;
    write.dstSet = descriptor_set.get();
    write.dstBinding = PELICAN_FRAME_UBO_BINDING;
    write.descriptorType = vk::DescriptorType::eUniformBuffer;
    write.setBufferInfo(frame_info);
    device.updateDescriptorSets(write, {});
}

FrameResources::~FrameResources() = default;

void FrameResources::setSceneBuffers(const BufferWrapper &objects,
                                     const BufferWrapper &previous_objects,
                                     const BufferWrapper &lights) {
    if (object_buffer == objects.buffer.get() &&
        previous_object_buffer == previous_objects.buffer.get() &&
        light_buffer == lights.buffer.get()) {
        return;
    }
    object_buffer = objects.buffer.get();
    previous_object_buffer = previous_objects.buffer.get();
    light_buffer = lights.buffer.get();

    const std::array buffer_infos{
        vk::DescriptorBufferInfo{object_buffer, 0, vk::WholeSize},
        vk::DescriptorBufferInfo{light_buffer, 0, vk::WholeSize},
        vk::DescriptorBufferInfo{previous_object_buffer, 0, vk::WholeSize},
    };
    std::array<vk::WriteDescriptorSet, 3> writes{};
    writes[0].dstSet = descriptor_set.get();
    writes[0].dstBinding = PELICAN_OBJECT_BUFFER_BINDING;
    writes[0].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[0].setBufferInfo(buffer_infos[0]);
    writes[1].dstSet = descriptor_set.get();
    writes[1].dstBinding = PELICAN_LIGHT_UBO_BINDING;
    writes[1].descriptorType = vk::DescriptorType::eUniformBuffer;
    writes[1].setBufferInfo(buffer_infos[1]);
    writes[2].dstSet = descriptor_set.get();
    writes[2].dstBinding = PELICAN_PREVIOUS_OBJECT_BUFFER_BINDING;
    writes[2].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[2].setBufferInfo(buffer_infos[2]);
    device.updateDescriptorSets(writes, {});
}

void FrameResources::update(const FrameUniformData &data) const {
    GET_MODULE(VulkanManageCore).writeBuf(frame_buffer, &data, 0, sizeof(data));
}

void FrameResources::bindGraphics(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout) const {
    cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, PELICAN_SET_FRAME,
                               descriptor_set.get(), {});
}

} // namespace Pelican
