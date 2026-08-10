#include "frameresources.hpp"

#include "../shader/pelican_sets.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/rendertarget.hpp"
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace Pelican {

namespace {

vk::UniqueDescriptorPool createDescriptorPool(
    vk::Device device, const FrameDescriptorPoolPlan &plan) {
    vk::DescriptorPoolCreateInfo create_info;
    create_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    create_info.maxSets = plan.max_sets;
    create_info.setPoolSizes(plan.pool_sizes);
    return device.createDescriptorPoolUnique(create_info);
}

} // namespace

FrameDescriptorPoolPlan makeFrameDescriptorPoolPlan(
    std::uint32_t slot_count, bool ray_query) {
    const auto multiplier = ray_query ? 6u : 3u;
    if (slot_count >
        std::numeric_limits<std::uint32_t>::max() /
            multiplier) {
        throw std::overflow_error(
            "frame descriptor pool size overflow");
    }
    if (ray_query &&
        slot_count >
            std::numeric_limits<std::uint32_t>::max() / 2u) {
        throw std::overflow_error(
            "frame descriptor set count overflow");
    }
    FrameDescriptorPoolPlan result{
        .max_sets = slot_count * (ray_query ? 2u : 1u),
        .pool_sizes = {
            {vk::DescriptorType::eUniformBuffer,
             multiplier * slot_count},
            {vk::DescriptorType::eStorageBuffer,
             multiplier * slot_count},
        },
    };
    if (ray_query) {
        result.pool_sizes.emplace_back(
            vk::DescriptorType::eAccelerationStructureKHR,
            slot_count);
    }
    return result;
}

std::vector<std::byte> packFrameUniformViews(
    std::span<const FrameUniformData> views) {
    if (views.empty()) {
        throw std::runtime_error(
            "multiview frame data requires at least one view");
    }
    if (views.size() > 32) {
        throw std::runtime_error(
            "multiview frame data supports at most 32 views");
    }
    if (views.size() >
        std::numeric_limits<std::size_t>::max() /
            sizeof(FrameUniformData)) {
        throw std::overflow_error(
            "multiview frame data byte size overflow");
    }
    std::vector<std::byte> bytes(
        views.size() * sizeof(FrameUniformData));
    std::memcpy(bytes.data(), views.data(), bytes.size());
    return bytes;
}

std::vector<std::byte> packFrameResolutionViews(
    std::span<const FrameResolutionUniformData> views) {
    if (views.empty()) {
        throw std::runtime_error(
            "multiview frame resolution data requires at least one view");
    }
    if (views.size() > 32) {
        throw std::runtime_error(
            "multiview frame resolution data supports at most 32 views");
    }
    if (views.size() >
        std::numeric_limits<std::size_t>::max() /
            sizeof(FrameResolutionUniformData)) {
        throw std::overflow_error(
            "multiview frame resolution data byte size overflow");
    }
    std::vector<std::byte> bytes(
        views.size() * sizeof(FrameResolutionUniformData));
    std::memcpy(bytes.data(), views.data(), bytes.size());
    return bytes;
}

FrameResources::FrameResources() : device{GET_MODULE(VulkanManageCore).getDevice()} {
    configureViewCount(1, 1, false);
}

FrameResources::~FrameResources() = default;

void FrameResources::configureViewCount(
    std::uint32_t count,
    std::uint32_t sequential_count,
    bool ray_query) {
    if (count == 0) {
        throw std::runtime_error("FrameResources requires at least one view");
    }
    if (sequential_count < count) {
        throw std::runtime_error(
            "FrameResources sequential view capacity cannot be smaller "
            "than the main view count");
    }
    if (view_count == count &&
        sequential_view_count ==
            sequential_count &&
        ray_query_enabled == ray_query) {
        return;
    }

    device.waitIdle();
    frame_slots.clear();
    multiview_frame_slots.clear();
    descriptor_pool.reset();

    const auto sequential_slot_count =
        static_cast<std::uint32_t>(
            in_flight_frames_num) *
        sequential_count;
    const auto multiview_slot_count =
        count > 1
            ? static_cast<std::uint32_t>(
                  in_flight_frames_num)
            : 0u;
    const auto descriptor_count =
        sequential_slot_count +
        multiview_slot_count;
    descriptor_pool =
        createDescriptorPool(
            device,
            makeFrameDescriptorPoolPlan(
                descriptor_count, ray_query));
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto layout =
        pipeline_factory.frameDescriptorSetLayout(false);
    std::vector<vk::DescriptorSetLayout> layouts(
        descriptor_count, layout);
    if (ray_query) {
        const auto ray_query_layout =
            pipeline_factory.frameDescriptorSetLayout(true);
        layouts.insert(
            layouts.end(), descriptor_count,
            ray_query_layout);
    }
    vk::DescriptorSetAllocateInfo allocate_info;
    allocate_info.descriptorPool = descriptor_pool.get();
    allocate_info.setSetLayouts(layouts);
    auto descriptor_sets = device.allocateDescriptorSetsUnique(allocate_info);

    frame_slots.reserve(sequential_slot_count);
    for (std::uint32_t slot = 0;
         slot < sequential_slot_count; ++slot) {
        FrameSlot frame_slot;
        frame_slot.frame_buffer = GET_MODULE(VulkanManageCore).allocBuf(
            sizeof(FrameUniformData), vk::BufferUsageFlagBits::eUniformBuffer,
            vma::MemoryUsage::eAuto,
            vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
        frame_slot.resolution_buffer =
            GET_MODULE(VulkanManageCore).allocBuf(
                sizeof(FrameResolutionUniformData),
                vk::BufferUsageFlagBits::eUniformBuffer,
                vma::MemoryUsage::eAuto,
                vma::AllocationCreateFlagBits::
                    eHostAccessSequentialWrite);
        frame_slot.descriptor_set = std::move(descriptor_sets[slot]);
        if (ray_query) {
            frame_slot.ray_query_descriptor_set =
                std::move(descriptor_sets[
                    descriptor_count + slot]);
        }

        const auto frame_index =
            slot / sequential_count;
        const auto view_index =
            slot % sequential_count;
        const auto base = "frame/in_flight/" + std::to_string(frame_index) +
                          "/view/" + std::to_string(view_index);
        const auto &debug_utils = GET_MODULE(VulkanManageCore).getDebugUtils();
        debug_utils.nameBuffer(frame_slot.frame_buffer.buffer.get(),
                               (base + "/ubo").c_str());
        debug_utils.nameBuffer(
            frame_slot.resolution_buffer.buffer.get(),
            (base + "/resolution_ubo").c_str());
        debug_utils.nameDescriptorSet(frame_slot.descriptor_set.get(),
                                      (base + "/descriptor_set").c_str());
        if (ray_query) {
            debug_utils.nameDescriptorSet(
                frame_slot.ray_query_descriptor_set.get(),
                (base + "/ray_query_descriptor_set").c_str());
        }

        const std::array buffer_infos{
            vk::DescriptorBufferInfo{
                frame_slot.frame_buffer.buffer.get(), 0,
                sizeof(FrameUniformData)},
            vk::DescriptorBufferInfo{
                frame_slot.resolution_buffer.buffer.get(), 0,
                sizeof(FrameResolutionUniformData)},
        };
        const std::array descriptor_sets_to_update{
            frame_slot.descriptor_set.get(),
            frame_slot.ray_query_descriptor_set.get()};
        for (const auto descriptor_set :
             descriptor_sets_to_update) {
            if (!descriptor_set) continue;
            std::array<vk::WriteDescriptorSet, 2> writes{};
            writes[0].dstSet = descriptor_set;
            writes[0].dstBinding = PELICAN_FRAME_UBO_BINDING;
            writes[0].descriptorType =
                vk::DescriptorType::eUniformBuffer;
            writes[0].setBufferInfo(buffer_infos[0]);
            writes[1].dstSet = descriptor_set;
            writes[1].dstBinding =
                PELICAN_FRAME_RESOLUTION_UBO_BINDING;
            writes[1].descriptorType =
                vk::DescriptorType::eUniformBuffer;
            writes[1].setBufferInfo(buffer_infos[1]);
            device.updateDescriptorSets(writes, {});
        }
        frame_slots.push_back(std::move(frame_slot));
    }

    multiview_frame_slots.reserve(
        multiview_slot_count);
    for (std::uint32_t frame = 0;
         frame < multiview_slot_count; ++frame) {
        MultiviewFrameSlot frame_slot;
        const auto byte_size =
            static_cast<vk::DeviceSize>(
                count) *
            sizeof(FrameUniformData);
        frame_slot.frame_buffer =
            GET_MODULE(VulkanManageCore).allocBuf(
                byte_size,
                vk::BufferUsageFlagBits::eUniformBuffer,
                vma::MemoryUsage::eAuto,
                vma::AllocationCreateFlagBits::
                    eHostAccessSequentialWrite);
        const auto resolution_byte_size =
            static_cast<vk::DeviceSize>(count) *
            sizeof(FrameResolutionUniformData);
        frame_slot.resolution_buffer =
            GET_MODULE(VulkanManageCore).allocBuf(
                resolution_byte_size,
                vk::BufferUsageFlagBits::eUniformBuffer,
                vma::MemoryUsage::eAuto,
                vma::AllocationCreateFlagBits::
                    eHostAccessSequentialWrite);
        frame_slot.descriptor_set =
            std::move(
                descriptor_sets
                    [sequential_slot_count + frame]);
        if (ray_query) {
            frame_slot.ray_query_descriptor_set =
                std::move(descriptor_sets[
                    descriptor_count +
                    sequential_slot_count + frame]);
        }
        frame_slot.last_data.resize(count);
        frame_slot.last_resolution.resize(count);

        const auto base =
            "frame/in_flight/" +
            std::to_string(frame) +
            "/multiview";
        const auto &debug_utils =
            GET_MODULE(VulkanManageCore)
                .getDebugUtils();
        debug_utils.nameBuffer(
            frame_slot.frame_buffer.buffer.get(),
            (base + "/ubo").c_str());
        debug_utils.nameBuffer(
            frame_slot.resolution_buffer.buffer.get(),
            (base + "/resolution_ubo").c_str());
        debug_utils.nameDescriptorSet(
            frame_slot.descriptor_set.get(),
            (base + "/descriptor_set").c_str());
        if (ray_query) {
            debug_utils.nameDescriptorSet(
                frame_slot.ray_query_descriptor_set.get(),
                (base + "/ray_query_descriptor_set").c_str());
        }

        const std::array buffer_infos{
            vk::DescriptorBufferInfo{
                frame_slot.frame_buffer.buffer.get(), 0,
                byte_size},
            vk::DescriptorBufferInfo{
                frame_slot.resolution_buffer.buffer.get(), 0,
                resolution_byte_size},
        };
        const std::array descriptor_sets_to_update{
            frame_slot.descriptor_set.get(),
            frame_slot.ray_query_descriptor_set.get()};
        for (const auto descriptor_set :
             descriptor_sets_to_update) {
            if (!descriptor_set) continue;
            std::array<vk::WriteDescriptorSet, 2> writes{};
            writes[0].dstSet = descriptor_set;
            writes[0].dstBinding = PELICAN_FRAME_UBO_BINDING;
            writes[0].descriptorType =
                vk::DescriptorType::eUniformBuffer;
            writes[0].setBufferInfo(buffer_infos[0]);
            writes[1].dstSet = descriptor_set;
            writes[1].dstBinding =
                PELICAN_FRAME_RESOLUTION_UBO_BINDING;
            writes[1].descriptorType =
                vk::DescriptorType::eUniformBuffer;
            writes[1].setBufferInfo(buffer_infos[1]);
            device.updateDescriptorSets(writes, {});
        }
        multiview_frame_slots.push_back(
            std::move(frame_slot));
    }

    view_count = count;
    sequential_view_count =
        sequential_count;
    ray_query_enabled = ray_query;
    active_slot = 0;
    active_multiview_slot = false;
    updateSceneDescriptors();
}

void FrameResources::updateSceneDescriptors() {
    if (!object_buffer || !previous_object_buffer || !light_buffer ||
        !directional_shadow_buffer) {
        return;
    }

    const std::array buffer_infos{
        vk::DescriptorBufferInfo{object_buffer, 0, vk::WholeSize},
        vk::DescriptorBufferInfo{light_buffer, 0, vk::WholeSize},
        vk::DescriptorBufferInfo{previous_object_buffer, 0, vk::WholeSize},
        vk::DescriptorBufferInfo{directional_shadow_buffer, 0, vk::WholeSize},
    };
    const auto update = [&](vk::DescriptorSet descriptor_set) {
        std::array<vk::WriteDescriptorSet, 4> writes{};
        writes[0].dstSet = descriptor_set;
        writes[0].dstBinding = PELICAN_OBJECT_BUFFER_BINDING;
        writes[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[0].setBufferInfo(buffer_infos[0]);
        writes[1].dstSet = descriptor_set;
        writes[1].dstBinding = PELICAN_LIGHT_UBO_BINDING;
        writes[1].descriptorType = vk::DescriptorType::eUniformBuffer;
        writes[1].setBufferInfo(buffer_infos[1]);
        writes[2].dstSet = descriptor_set;
        writes[2].dstBinding = PELICAN_PREVIOUS_OBJECT_BUFFER_BINDING;
        writes[2].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[2].setBufferInfo(buffer_infos[2]);
        writes[3].dstSet = descriptor_set;
        writes[3].dstBinding = PELICAN_DIRECTIONAL_SHADOW_DATA_BINDING;
        writes[3].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[3].setBufferInfo(buffer_infos[3]);
        device.updateDescriptorSets(writes, {});
    };
    for (const auto &slot : frame_slots) {
        update(slot.descriptor_set.get());
        if (slot.ray_query_descriptor_set) {
            update(slot.ray_query_descriptor_set.get());
        }
    }
    for (const auto &slot : multiview_frame_slots) {
        update(slot.descriptor_set.get());
        if (slot.ray_query_descriptor_set) {
            update(slot.ray_query_descriptor_set.get());
        }
    }
}

void FrameResources::setSceneBuffers(const BufferWrapper &objects,
                                     const BufferWrapper &previous_objects,
                                     const BufferWrapper &lights,
                                     const BufferWrapper &directional_shadows) {
    if (object_buffer == objects.buffer.get() &&
        previous_object_buffer == previous_objects.buffer.get() &&
        light_buffer == lights.buffer.get() &&
        directional_shadow_buffer == directional_shadows.buffer.get()) {
        return;
    }
    object_buffer = objects.buffer.get();
    previous_object_buffer = previous_objects.buffer.get();
    light_buffer = lights.buffer.get();
    directional_shadow_buffer = directional_shadows.buffer.get();
    updateSceneDescriptors();
}

void FrameResources::beginLogicalFrame(std::uint32_t count) {
    beginLogicalFrame(count, count, false);
}

void FrameResources::beginLogicalFrame(
    std::uint32_t main_view_count,
    std::uint32_t sequential_count) {
    beginLogicalFrame(
        main_view_count, sequential_count, false);
}

void FrameResources::beginLogicalFrame(
    std::uint32_t main_view_count,
    std::uint32_t sequential_count,
    bool ray_query) {
    configureViewCount(
        main_view_count,
        sequential_count,
        ray_query);
}

void FrameResources::setRayQueryAccelerationStructure(
    std::uint32_t in_flight_frame_index,
    vk::AccelerationStructureKHR top_level) {
    if (!ray_query_enabled) {
        throw std::logic_error(
            "ray-query frame descriptor set is not enabled");
    }
    if (!top_level) {
        throw std::runtime_error(
            "ray-query frame descriptor requires a TLAS");
    }
    if (in_flight_frame_index >= in_flight_frames_num) {
        throw std::out_of_range(
            "ray-query in-flight frame index is out of range");
    }
    const std::array acceleration_structures{top_level};
    vk::WriteDescriptorSetAccelerationStructureKHR acceleration_info;
    acceleration_info.setAccelerationStructures(
        acceleration_structures);
    const auto update = [&](vk::DescriptorSet descriptor_set) {
        vk::WriteDescriptorSet write;
        write.pNext = &acceleration_info;
        write.dstSet = descriptor_set;
        write.dstBinding = PELICAN_RAY_QUERY_TLAS_BINDING;
        write.descriptorCount = 1;
        write.descriptorType =
            vk::DescriptorType::eAccelerationStructureKHR;
        device.updateDescriptorSets(write, {});
    };
    const auto first =
        static_cast<std::size_t>(in_flight_frame_index) *
        sequential_view_count;
    for (std::uint32_t view = 0;
         view < sequential_view_count; ++view) {
        update(frame_slots.at(first + view)
                   .ray_query_descriptor_set.get());
    }
    if (!multiview_frame_slots.empty()) {
        update(multiview_frame_slots
                   .at(in_flight_frame_index)
                   .ray_query_descriptor_set.get());
    }
}

void FrameResources::selectView(std::uint32_t in_flight_frame_index,
                                std::uint32_t view_index) {
    if (view_index >= view_count) {
        throw std::runtime_error("FrameResources view index is out of range");
    }
    selectSequentialView(
        in_flight_frame_index,
        view_index);
}

void FrameResources::selectSequentialView(
    std::uint32_t in_flight_frame_index,
    std::uint32_t sequential_view_index) {
    if (in_flight_frame_index >=
        in_flight_frames_num) {
        throw std::runtime_error(
            "FrameResources in-flight frame index is out of range");
    }
    if (sequential_view_index >=
        sequential_view_count) {
        throw std::runtime_error(
            "FrameResources sequential view index is out of range");
    }
    active_slot =
        static_cast<std::size_t>(
            in_flight_frame_index) *
            sequential_view_count +
        sequential_view_index;
    active_multiview_slot = false;
}

void FrameResources::selectMultiview(
    std::uint32_t in_flight_frame_index) {
    if (view_count < 2) {
        throw std::runtime_error(
            "FrameResources multiview selection requires "
            "at least two logical views");
    }
    if (in_flight_frame_index >=
        in_flight_frames_num) {
        throw std::runtime_error(
            "FrameResources multiview in-flight frame "
            "index is out of range");
    }
    active_slot = in_flight_frame_index;
    active_multiview_slot = true;
}

void FrameResources::update(const FrameUniformData &data) {
    if (active_multiview_slot) {
        throw std::logic_error(
            "scalar frame data cannot update the active "
            "multiview slot");
    }
    auto &slot = frame_slots.at(active_slot);
    GET_MODULE(VulkanManageCore).writeBuf(slot.frame_buffer, &data, 0, sizeof(data));
    slot.last_data = data;
}

void FrameResources::updateResolution(
    const FrameResolutionUniformData &data) {
    if (active_multiview_slot) {
        throw std::logic_error(
            "scalar frame resolution data cannot update the active multiview slot");
    }
    auto &slot = frame_slots.at(active_slot);
    GET_MODULE(VulkanManageCore).writeBuf(
        slot.resolution_buffer, &data, 0, sizeof(data));
    slot.last_resolution = data;
}

void FrameResources::updateMultiview(
    std::span<const FrameUniformData> data) {
    if (!active_multiview_slot) {
        throw std::logic_error(
            "multiview frame data requires an active "
            "multiview slot");
    }
    if (data.size() != view_count) {
        throw std::runtime_error(
            "multiview frame data count does not match "
            "the logical frame");
    }
    const auto bytes = packFrameUniformViews(data);
    auto &slot =
        multiview_frame_slots.at(active_slot);
    GET_MODULE(VulkanManageCore).writeBuf(
        slot.frame_buffer, bytes.data(), 0,
        bytes.size());
    slot.last_data.assign(
        data.begin(), data.end());
}

void FrameResources::updateMultiviewResolutions(
    std::span<const FrameResolutionUniformData> data) {
    if (!active_multiview_slot) {
        throw std::logic_error(
            "multiview frame resolution data requires an active multiview slot");
    }
    if (data.size() != view_count) {
        throw std::runtime_error(
            "multiview frame resolution data count does not match the logical frame");
    }
    const auto bytes =
        packFrameResolutionViews(data);
    auto &slot =
        multiview_frame_slots.at(active_slot);
    GET_MODULE(VulkanManageCore).writeBuf(
        slot.resolution_buffer, bytes.data(), 0,
        bytes.size());
    slot.last_resolution.assign(
        data.begin(), data.end());
}

void FrameResources::bindFrameDescriptorSet(
    vk::CommandBuffer cmd_buf,
    vk::PipelineLayout pipeline_layout,
    vk::PipelineBindPoint bind_point) const {
    const auto ray_query =
        GET_MODULE(PipelineFactory)
            .pipelineLayoutUsesRayQueryFrameSet(
                pipeline_layout);
    vk::DescriptorSet descriptor_set;
    if (active_multiview_slot) {
        const auto &slot =
            multiview_frame_slots.at(active_slot);
        descriptor_set =
            ray_query ? slot.ray_query_descriptor_set.get()
                      : slot.descriptor_set.get();
    } else {
        const auto &slot = frame_slots.at(active_slot);
        descriptor_set =
            ray_query ? slot.ray_query_descriptor_set.get()
                      : slot.descriptor_set.get();
    }
    if (!descriptor_set) {
        throw std::runtime_error(
            "pipeline requires a ray-query frame descriptor set");
    }
    cmd_buf.bindDescriptorSets(bind_point, pipeline_layout,
                               PELICAN_SET_FRAME,
                               descriptor_set, {});
}

void FrameResources::bindGraphics(
    vk::CommandBuffer cmd_buf,
    vk::PipelineLayout pipeline_layout) const {
    bindFrameDescriptorSet(
        cmd_buf, pipeline_layout,
        vk::PipelineBindPoint::eGraphics);
}

void FrameResources::bindCompute(
    vk::CommandBuffer cmd_buf,
    vk::PipelineLayout pipeline_layout) const {
    bindFrameDescriptorSet(
        cmd_buf, pipeline_layout,
        vk::PipelineBindPoint::eCompute);
}

void FrameResources::bindRayTracing(
    vk::CommandBuffer cmd_buf,
    vk::PipelineLayout pipeline_layout) const {
    bindFrameDescriptorSet(
        cmd_buf, pipeline_layout,
        vk::PipelineBindPoint::eRayTracingKHR);
}

vk::Buffer FrameResources::slotBufferForTesting(std::uint32_t in_flight_frame_index,
                                                std::uint32_t view_index) const {
    if (in_flight_frame_index >= in_flight_frames_num ||
        view_index >= sequential_view_count) {
        throw std::out_of_range("FrameResources testing slot is out of range");
    }
    const auto slot =
        static_cast<std::size_t>(
            in_flight_frame_index) *
            sequential_view_count +
        view_index;
    return frame_slots.at(slot).frame_buffer.buffer.get();
}

const FrameUniformData &FrameResources::slotDataForTesting(
    std::uint32_t in_flight_frame_index, std::uint32_t view_index) const {
    if (in_flight_frame_index >= in_flight_frames_num ||
        view_index >= sequential_view_count) {
        throw std::out_of_range("FrameResources testing slot is out of range");
    }
    const auto slot =
        static_cast<std::size_t>(
            in_flight_frame_index) *
            sequential_view_count +
        view_index;
    return frame_slots.at(slot).last_data;
}

const FrameResolutionUniformData &
FrameResources::slotResolutionForTesting(
    std::uint32_t in_flight_frame_index,
    std::uint32_t view_index) const {
    if (in_flight_frame_index >= in_flight_frames_num ||
        view_index >= sequential_view_count) {
        throw std::out_of_range(
            "FrameResources testing resolution slot is out of range");
    }
    const auto slot =
        static_cast<std::size_t>(
            in_flight_frame_index) *
            sequential_view_count +
        view_index;
    return frame_slots.at(slot).last_resolution;
}

const BufferWrapper &
FrameResources::multiviewSlotBufferForTesting(
    std::uint32_t in_flight_frame_index) const {
    if (in_flight_frame_index >=
            in_flight_frames_num ||
        multiview_frame_slots.empty()) {
        throw std::out_of_range(
            "FrameResources testing multiview slot is "
            "out of range");
    }
    return multiview_frame_slots
        .at(in_flight_frame_index)
        .frame_buffer;
}

const std::vector<FrameUniformData> &
FrameResources::multiviewSlotDataForTesting(
    std::uint32_t in_flight_frame_index) const {
    if (in_flight_frame_index >=
            in_flight_frames_num ||
        multiview_frame_slots.empty()) {
        throw std::out_of_range(
            "FrameResources testing multiview slot is "
            "out of range");
    }
    return multiview_frame_slots
        .at(in_flight_frame_index)
        .last_data;
}

const std::vector<FrameResolutionUniformData> &
FrameResources::multiviewSlotResolutionForTesting(
    std::uint32_t in_flight_frame_index) const {
    if (in_flight_frame_index >=
            in_flight_frames_num ||
        multiview_frame_slots.empty()) {
        throw std::out_of_range(
            "FrameResources testing multiview resolution slot is out of range");
    }
    return multiview_frame_slots
        .at(in_flight_frame_index)
        .last_resolution;
}

} // namespace Pelican
