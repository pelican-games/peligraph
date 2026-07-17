#include "frameresources.hpp"

#include "../shader/pelican_sets.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/rendertarget.hpp"
#include <array>
#include <stdexcept>
#include <string>

namespace Pelican {

namespace {

vk::UniqueDescriptorPool createDescriptorPool(vk::Device device, std::uint32_t slot_count) {
    const std::array pool_sizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 2 * slot_count},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 2 * slot_count},
    };
    vk::DescriptorPoolCreateInfo create_info;
    create_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    create_info.maxSets = slot_count;
    create_info.setPoolSizes(pool_sizes);
    return device.createDescriptorPoolUnique(create_info);
}

} // namespace

FrameResources::FrameResources() : device{GET_MODULE(VulkanManageCore).getDevice()} {
    configureViewCount(1);
}

FrameResources::~FrameResources() = default;

void FrameResources::configureViewCount(std::uint32_t count) {
    if (count == 0) {
        throw std::runtime_error("FrameResources requires at least one view");
    }
    if (view_count == count) {
        return;
    }

    device.waitIdle();
    frame_slots.clear();
    descriptor_pool.reset();

    const auto slot_count = static_cast<std::uint32_t>(in_flight_frames_num) * count;
    descriptor_pool = createDescriptorPool(device, slot_count);
    const auto layout = GET_MODULE(PipelineFactory).frameDescriptorSetLayout();
    std::vector<vk::DescriptorSetLayout> layouts(slot_count, layout);
    vk::DescriptorSetAllocateInfo allocate_info;
    allocate_info.descriptorPool = descriptor_pool.get();
    allocate_info.setSetLayouts(layouts);
    auto descriptor_sets = device.allocateDescriptorSetsUnique(allocate_info);

    frame_slots.reserve(slot_count);
    for (std::uint32_t slot = 0; slot < slot_count; ++slot) {
        FrameSlot frame_slot;
        frame_slot.frame_buffer = GET_MODULE(VulkanManageCore).allocBuf(
            sizeof(FrameUniformData), vk::BufferUsageFlagBits::eUniformBuffer,
            vma::MemoryUsage::eAuto,
            vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
        frame_slot.descriptor_set = std::move(descriptor_sets[slot]);

        const auto frame_index = slot / count;
        const auto view_index = slot % count;
        const auto base = "frame/in_flight/" + std::to_string(frame_index) +
                          "/view/" + std::to_string(view_index);
        const auto &debug_utils = GET_MODULE(VulkanManageCore).getDebugUtils();
        debug_utils.nameBuffer(frame_slot.frame_buffer.buffer.get(),
                               (base + "/ubo").c_str());
        debug_utils.nameDescriptorSet(frame_slot.descriptor_set.get(),
                                      (base + "/descriptor_set").c_str());

        vk::DescriptorBufferInfo frame_info{frame_slot.frame_buffer.buffer.get(), 0,
                                            sizeof(FrameUniformData)};
        vk::WriteDescriptorSet write;
        write.dstSet = frame_slot.descriptor_set.get();
        write.dstBinding = PELICAN_FRAME_UBO_BINDING;
        write.descriptorType = vk::DescriptorType::eUniformBuffer;
        write.setBufferInfo(frame_info);
        device.updateDescriptorSets(write, {});
        frame_slots.push_back(std::move(frame_slot));
    }

    view_count = count;
    active_slot = 0;
    updateSceneDescriptors();
}

void FrameResources::updateSceneDescriptors() {
    if (!object_buffer || !previous_object_buffer || !light_buffer) {
        return;
    }

    const std::array buffer_infos{
        vk::DescriptorBufferInfo{object_buffer, 0, vk::WholeSize},
        vk::DescriptorBufferInfo{light_buffer, 0, vk::WholeSize},
        vk::DescriptorBufferInfo{previous_object_buffer, 0, vk::WholeSize},
    };
    for (const auto &slot : frame_slots) {
        std::array<vk::WriteDescriptorSet, 3> writes{};
        writes[0].dstSet = slot.descriptor_set.get();
        writes[0].dstBinding = PELICAN_OBJECT_BUFFER_BINDING;
        writes[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[0].setBufferInfo(buffer_infos[0]);
        writes[1].dstSet = slot.descriptor_set.get();
        writes[1].dstBinding = PELICAN_LIGHT_UBO_BINDING;
        writes[1].descriptorType = vk::DescriptorType::eUniformBuffer;
        writes[1].setBufferInfo(buffer_infos[1]);
        writes[2].dstSet = slot.descriptor_set.get();
        writes[2].dstBinding = PELICAN_PREVIOUS_OBJECT_BUFFER_BINDING;
        writes[2].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[2].setBufferInfo(buffer_infos[2]);
        device.updateDescriptorSets(writes, {});
    }
}

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
    updateSceneDescriptors();
}

void FrameResources::beginLogicalFrame(std::uint32_t count) {
    configureViewCount(count);
}

void FrameResources::selectView(std::uint32_t in_flight_frame_index,
                                std::uint32_t view_index) {
    if (in_flight_frame_index >= in_flight_frames_num) {
        throw std::runtime_error("FrameResources in-flight frame index is out of range");
    }
    if (view_index >= view_count) {
        throw std::runtime_error("FrameResources view index is out of range");
    }
    active_slot = static_cast<std::size_t>(in_flight_frame_index) * view_count + view_index;
}

void FrameResources::update(const FrameUniformData &data) {
    auto &slot = frame_slots.at(active_slot);
    GET_MODULE(VulkanManageCore).writeBuf(slot.frame_buffer, &data, 0, sizeof(data));
    slot.last_data = data;
}

void FrameResources::bindGraphics(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout) const {
    cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, PELICAN_SET_FRAME,
                               frame_slots.at(active_slot).descriptor_set.get(), {});
}

vk::Buffer FrameResources::slotBufferForTesting(std::uint32_t in_flight_frame_index,
                                                std::uint32_t view_index) const {
    if (in_flight_frame_index >= in_flight_frames_num || view_index >= view_count) {
        throw std::out_of_range("FrameResources testing slot is out of range");
    }
    const auto slot = static_cast<std::size_t>(in_flight_frame_index) * view_count + view_index;
    return frame_slots.at(slot).frame_buffer.buffer.get();
}

const FrameUniformData &FrameResources::slotDataForTesting(
    std::uint32_t in_flight_frame_index, std::uint32_t view_index) const {
    if (in_flight_frame_index >= in_flight_frames_num || view_index >= view_count) {
        throw std::out_of_range("FrameResources testing slot is out of range");
    }
    const auto slot = static_cast<std::size_t>(in_flight_frame_index) * view_count + view_index;
    return frame_slots.at(slot).last_data;
}

} // namespace Pelican
