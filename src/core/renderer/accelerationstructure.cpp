#include "../vkcore/accelerationstructure.hpp"

#include "../log.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Pelican {

namespace {

std::string geometryName(
    const RayQueryGeometryInstanceSnapshot &instance) {
    return "asset[" + std::to_string(instance.asset_identity) +
           "]/geometry[" +
           std::to_string(instance.geometry_allocation_id) +
           "]/mesh[" + std::to_string(instance.mesh_index) +
           "]/primitive[" +
           std::to_string(instance.primitive_index) +
           "]/node[" + std::to_string(instance.node_index) + "]";
}

void appendUnique(std::set<std::uint64_t> &identities,
                  std::vector<std::string> &names,
                  const RayQueryGeometryInstanceSnapshot &instance) {
    if (identities.emplace(instance.geometry_allocation_id).second) {
        names.push_back(geometryName(instance));
    }
}

std::string joinNames(std::span<const std::string> names) {
    std::ostringstream output;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index != 0) output << ", ";
        output << names[index];
    }
    return output.str();
}

vk::DeviceAddress bufferDeviceAddress(vk::Device device,
                                      const BufferWrapper &buffer) {
    return device.getBufferAddress(
        vk::BufferDeviceAddressInfo{buffer.buffer.get()});
}

struct AccelerationStructureResource {
    BufferWrapper storage;
    UniqueAccelerationStructure handle;
    vk::DeviceAddress address = 0;
    vk::DeviceAddress index_input_address = 0;
    vk::DeviceAddress vertex_input_address = 0;
};

struct RecordedSubmissionResources {
    std::shared_ptr<const void> state;
    std::vector<BufferWrapper> transient_buffers;
};

struct AddressedBuffer {
    BufferWrapper buffer;
    vk::DeviceAddress address = 0;
    vk::DeviceSize offset = 0;
};

AddressedBuffer allocateAddressedBuffer(
    VulkanManageCore &vkcore, vk::DeviceSize size,
    vk::BufferUsageFlags usage, vma::MemoryUsage memory_usage,
    vma::AllocationCreateFlags allocation_flags,
    vk::DeviceSize alignment) {
    if (size == 0) {
        throw std::runtime_error(
            "acceleration-structure buffer size must be non-zero");
    }
    alignment = std::max<vk::DeviceSize>(alignment, 1);
    if (size > std::numeric_limits<vk::DeviceSize>::max() -
                   (alignment - 1)) {
        throw std::runtime_error(
            "acceleration-structure aligned buffer size overflow");
    }
    auto buffer = vkcore.allocBuf(
        size + alignment - 1, usage, memory_usage,
        allocation_flags);
    const auto base =
        bufferDeviceAddress(vkcore.getDevice(), buffer);
    const auto remainder = base % alignment;
    const auto offset =
        remainder == 0 ? 0 : alignment - remainder;
    return AddressedBuffer{
        .buffer = std::move(buffer),
        .address = base + offset,
        .offset = offset,
    };
}

std::shared_ptr<AccelerationStructureResource>
createAccelerationStructure(
    VulkanManageCore &vkcore,
    vk::AccelerationStructureTypeKHR type,
    vk::DeviceSize size) {
    auto storage = vkcore.allocBuf(
        size,
        vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
            vk::BufferUsageFlagBits::eShaderDeviceAddress,
        vma::MemoryUsage::eAutoPreferDevice, {});
    const vk::AccelerationStructureCreateInfoKHR create_info{
        {}, storage.buffer.get(), 0, size, type};
    auto handle = vkcore.getDevice()
                      .createAccelerationStructureKHRUnique(
                          create_info, nullptr,
                          vkcore.getAccelerationStructureDispatch());
    const auto address = vkcore.getDevice()
                             .getAccelerationStructureAddressKHR(
                                 vk::AccelerationStructureDeviceAddressInfoKHR{
                                     handle.get()},
                                 vkcore.getAccelerationStructureDispatch());
    return std::make_shared<AccelerationStructureResource>(
        AccelerationStructureResource{
            .storage = std::move(storage),
            .handle = std::move(handle),
            .address = address,
        });
}

void recordBuild(
    vk::CommandBuffer command_buffer,
    VulkanManageCore &vkcore,
    vk::AccelerationStructureBuildGeometryInfoKHR build_info,
    const vk::AccelerationStructureBuildRangeInfoKHR &range,
    vk::DeviceSize scratch_size,
    std::vector<BufferWrapper> &transient_buffers) {
    const auto alignment =
        vkcore.getRuntimeCapabilities()
            .min_acceleration_structure_scratch_offset_alignment;
    auto scratch = allocateAddressedBuffer(
        vkcore, scratch_size,
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eShaderDeviceAddress,
        vma::MemoryUsage::eAutoPreferDevice, {}, alignment);
    build_info.scratchData =
        vk::DeviceOrHostAddressKHR{scratch.address};
    const auto *range_pointer = &range;
    command_buffer.buildAccelerationStructuresKHR(
        1, &build_info, &range_pointer,
        vkcore.getAccelerationStructureDispatch());
    transient_buffers.push_back(std::move(scratch.buffer));
}

vk::TransformMatrixKHR toVulkanTransform(
    const glm::mat4 &matrix) {
    std::array<std::array<float, 4>, 3> rows{};
    for (glm::length_t row = 0; row < 3; ++row) {
        for (glm::length_t column = 0; column < 4; ++column) {
            rows[static_cast<std::size_t>(row)]
                [static_cast<std::size_t>(column)] =
                    matrix[column][row];
        }
    }
    return vk::TransformMatrixKHR{rows};
}

void recordMemoryBarrier(
    vk::CommandBuffer command_buffer,
    vk::PipelineStageFlags source_stage,
    vk::PipelineStageFlags destination_stage,
    vk::AccessFlags source_access,
    vk::AccessFlags destination_access) {
    command_buffer.pipelineBarrier(
        source_stage, destination_stage, {},
        vk::MemoryBarrier{source_access, destination_access},
        {}, {});
}

} // namespace

RayQueryStaticGeometryClassification classifyRayQueryStaticGeometry(
    std::span<const RayQueryGeometryInstanceSnapshot> instances) {
    RayQueryStaticGeometryClassification result;
    result.static_instance_indices.reserve(instances.size());
    std::set<std::uint64_t> excluded;
    std::set<std::uint64_t> skinned;
    std::set<std::uint64_t> morph;
    std::set<std::uint64_t> vat;
    for (std::size_t index = 0; index < instances.size(); ++index) {
        const auto &instance = instances[index];
        if (!instance.skinned && !instance.morph_deformed &&
            !instance.vat_deformed) {
            result.static_instance_indices.push_back(index);
            continue;
        }
        ++result.excluded.instance_count;
        excluded.emplace(instance.geometry_allocation_id);
        if (instance.skinned) {
            appendUnique(skinned, result.excluded.skinned_names,
                         instance);
        }
        if (instance.morph_deformed) {
            appendUnique(morph, result.excluded.morph_names,
                         instance);
        }
        if (instance.vat_deformed) {
            appendUnique(vat, result.excluded.vat_names,
                         instance);
        }
    }
    result.excluded.primitive_count = excluded.size();
    result.excluded.skinned_primitive_count = skinned.size();
    result.excluded.morph_primitive_count = morph.size();
    result.excluded.vat_primitive_count = vat.size();
    return result;
}

nlohmann::json rayQueryAccelerationStructureDiagnosticsToJson(
    const RayQueryAccelerationStructureDiagnostics &diagnostics) {
    nlohmann::json inputs = nlohmann::json::array();
    for (const auto &input : diagnostics.active_blas_inputs) {
        inputs.push_back({
            {"geometry_allocation_id",
             input.geometry_allocation_id},
            {"index_device_address",
             input.index_device_address},
            {"vertex_device_address",
             input.vertex_device_address},
        });
    }
    return {
        {"requested", diagnostics.requested},
        {"static_only", true},
        {"blas_build_count", diagnostics.blas_build_count},
        {"tlas_build_count", diagnostics.tlas_build_count},
        {"active_blas_count", diagnostics.active_blas_count},
        {"tlas_instance_count", diagnostics.tlas_instance_count},
        {"geometry_pool_generation",
         diagnostics.geometry_pool_generation},
        {"model_rebuild_generation",
         diagnostics.model_rebuild_generation},
        {"geometry_pool_invalidation_count",
         diagnostics.geometry_pool_invalidation_count},
        {"model_rebuild_invalidation_count",
         diagnostics.model_rebuild_invalidation_count},
        {"excluded",
         {{"primitive_count",
           diagnostics.excluded.primitive_count},
          {"instance_count",
           diagnostics.excluded.instance_count},
          {"skinned_primitive_count",
           diagnostics.excluded.skinned_primitive_count},
          {"morph_primitive_count",
           diagnostics.excluded.morph_primitive_count},
          {"vat_primitive_count",
           diagnostics.excluded.vat_primitive_count},
          {"skinned_names",
           diagnostics.excluded.skinned_names},
          {"morph_names", diagnostics.excluded.morph_names},
          {"vat_names", diagnostics.excluded.vat_names}}},
        {"active_blas_inputs", std::move(inputs)},
    };
}

struct RayQueryAccelerationStructureScope::State {
    std::unordered_map<
        std::uint64_t,
        std::shared_ptr<AccelerationStructureResource>>
        blas;
    std::shared_ptr<AccelerationStructureResource> tlas;
    RayQueryAccelerationStructureDiagnostics diagnostics{
        .requested = true,
    };
    bool log_static_only_classification = false;
};

RayQueryAccelerationStructureScope::FrameBuild::FrameBuild() noexcept =
    default;

RayQueryAccelerationStructureScope::FrameBuild::FrameBuild(
    RayQueryAccelerationStructureScope *owner,
    std::shared_ptr<State> candidate) noexcept
    : owner_{owner}, candidate_{std::move(candidate)} {}

RayQueryAccelerationStructureScope::FrameBuild::~FrameBuild() = default;
RayQueryAccelerationStructureScope::FrameBuild::FrameBuild(
    FrameBuild &&) noexcept = default;
RayQueryAccelerationStructureScope::FrameBuild &
RayQueryAccelerationStructureScope::FrameBuild::operator=(
    FrameBuild &&) noexcept = default;

void RayQueryAccelerationStructureScope::FrameBuild::commit() noexcept {
    if (owner_ == nullptr || candidate_ == nullptr) return;
    owner_->active_state_ = candidate_;
    owner_->diagnostics_ = std::move(candidate_->diagnostics);
    if (candidate_->log_static_only_classification && logger != nullptr) {
        try {
            const auto &excluded =
                owner_->diagnostics_.excluded;
            LOG_INFO(
                logger,
                "Ray-query acceleration structures are static-only: "
                "active_blas={}, tlas_instances={}, excluded_primitives={}, "
                "excluded_instances={}, skinned={} [{}], morph={} [{}], "
                "VAT={} [{}]. A compute deform pass for deformed geometry "
                "is future work.",
                owner_->diagnostics_.active_blas_count,
                owner_->diagnostics_.tlas_instance_count,
                excluded.primitive_count,
                excluded.instance_count,
                excluded.skinned_primitive_count,
                joinNames(excluded.skinned_names),
                excluded.morph_primitive_count,
                joinNames(excluded.morph_names),
                excluded.vat_primitive_count,
                joinNames(excluded.vat_names));
        } catch (...) {
            // Submission publication is intentionally no-fail. Diagnostics
            // remain queryable even if formatting the informational log runs
            // out of host memory.
        }
    }
    owner_ = nullptr;
    candidate_.reset();
}

RayQueryAccelerationStructureScope::~RayQueryAccelerationStructureScope() =
    default;

RayQueryAccelerationStructureScope::FrameBuild
RayQueryAccelerationStructureScope::recordFrame(
    vk::CommandBuffer command_buffer,
    VulkanManageCore &vkcore,
    DeletionQueue &deletion_queue,
    const BufferWrapper &index_buffer,
    const BufferWrapper &vertex_buffer,
    std::uint64_t geometry_pool_generation,
    std::span<const RayQueryGeometryInstanceSnapshot> instances,
    std::uint64_t model_rebuild_generation) {
    const auto &capabilities = vkcore.getRuntimeCapabilities();
    if (!capabilities.ray_query ||
        !capabilities.acceleration_structure ||
        !capabilities.buffer_device_address ||
        capabilities
                .min_acceleration_structure_scratch_offset_alignment ==
            0) {
        throw std::runtime_error(
            "ray-query acceleration-structure scope requires enabled "
            "pelican.vulkan.ray_query@1 capabilities");
    }

    const auto classification =
        classifyRayQueryStaticGeometry(instances);
    auto candidate = std::make_shared<State>();
    candidate->diagnostics = diagnostics_;
    candidate->diagnostics.requested = true;
    candidate->diagnostics.excluded = classification.excluded;
    candidate->diagnostics.tlas_instance_count =
        classification.static_instance_indices.size();

    const bool first_frame = active_state_ == nullptr;
    const bool pool_invalidated =
        !first_frame &&
        diagnostics_.geometry_pool_generation !=
            geometry_pool_generation;
    const bool model_invalidated =
        !first_frame &&
        diagnostics_.model_rebuild_generation !=
            model_rebuild_generation;
    if (pool_invalidated) {
        ++candidate->diagnostics
              .geometry_pool_invalidation_count;
    }
    if (model_invalidated) {
        ++candidate->diagnostics
              .model_rebuild_invalidation_count;
    }
    candidate->diagnostics.geometry_pool_generation =
        geometry_pool_generation;
    candidate->diagnostics.model_rebuild_generation =
        model_rebuild_generation;
    candidate->log_static_only_classification =
        first_frame ||
        diagnostics_.excluded != classification.excluded;

    auto recorded =
        std::make_shared<RecordedSubmissionResources>();
    const auto device = vkcore.getDevice();
    vk::DeviceAddress index_pool_address = 0;
    vk::DeviceAddress vertex_pool_address = 0;
    if (!classification.static_instance_indices.empty()) {
        index_pool_address = bufferDeviceAddress(
            device, index_buffer);
        vertex_pool_address = bufferDeviceAddress(
            device, vertex_buffer);
        recordMemoryBarrier(
            command_buffer,
            vk::PipelineStageFlagBits::eHost,
            vk::PipelineStageFlagBits::
                eAccelerationStructureBuildKHR,
            vk::AccessFlagBits::eHostWrite,
            vk::AccessFlagBits::
                eAccelerationStructureReadKHR);
    }

    const bool can_reuse =
        active_state_ != nullptr && !pool_invalidated &&
        !model_invalidated;
    std::map<std::uint64_t,
             const RayQueryGeometryInstanceSnapshot *>
        unique_geometry;
    for (const auto index :
         classification.static_instance_indices) {
        const auto &instance = instances[index];
        if (instance.geometry_allocation_id == 0 ||
            instance.vertex_count == 0 ||
            instance.index_count == 0 ||
            instance.index_count % 3 != 0 ||
            instance.vertex_offset < 0) {
            throw std::runtime_error(
                "static ray-query primitive has invalid indexed geometry");
        }
        const auto [found, inserted] = unique_geometry.emplace(
            instance.geometry_allocation_id, &instance);
        if (!inserted) {
            const auto &previous = *found->second;
            if (previous.index_count != instance.index_count ||
                previous.index_offset != instance.index_offset ||
                previous.vertex_offset != instance.vertex_offset ||
                previous.vertex_count != instance.vertex_count) {
                throw std::runtime_error(
                    "geometry allocation identity maps to inconsistent "
                    "ray-query input ranges");
            }
        }
    }

    for (const auto &[identity, instance] : unique_geometry) {
        if (can_reuse) {
            const auto existing =
                active_state_->blas.find(identity);
            if (existing != active_state_->blas.end()) {
                candidate->blas.emplace(identity,
                                        existing->second);
                continue;
            }
        }
        const auto index_address =
            index_pool_address +
            sizeof(std::uint32_t) * instance->index_offset;
        const auto vertex_address =
            vertex_pool_address +
            sizeof(CommonVertStruct) *
                static_cast<std::uint32_t>(
                    instance->vertex_offset);
        const vk::AccelerationStructureGeometryTrianglesDataKHR
            triangles{
                vk::Format::eR32G32B32Sfloat,
                vk::DeviceOrHostAddressConstKHR{vertex_address},
                sizeof(CommonVertStruct),
                instance->vertex_count - 1,
                vk::IndexType::eUint32,
                vk::DeviceOrHostAddressConstKHR{index_address},
            };
        const vk::AccelerationStructureGeometryKHR geometry_info{
            vk::GeometryTypeKHR::eTriangles,
            vk::AccelerationStructureGeometryDataKHR{triangles}};
        vk::AccelerationStructureBuildGeometryInfoKHR build_info{
            vk::AccelerationStructureTypeKHR::eBottomLevel,
            vk::BuildAccelerationStructureFlagBitsKHR::
                ePreferFastTrace,
            vk::BuildAccelerationStructureModeKHR::eBuild,
            {}, {}, 1, &geometry_info};
        const auto primitive_count = instance->index_count / 3;
        vk::AccelerationStructureBuildSizesInfoKHR sizes;
        device.getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice,
            &build_info, &primitive_count, &sizes,
            vkcore.getAccelerationStructureDispatch());
        auto blas = createAccelerationStructure(
            vkcore,
            vk::AccelerationStructureTypeKHR::eBottomLevel,
            sizes.accelerationStructureSize);
        blas->index_input_address = index_address;
        blas->vertex_input_address = vertex_address;
        build_info.dstAccelerationStructure = blas->handle.get();
        recordBuild(
            command_buffer, vkcore, build_info,
            vk::AccelerationStructureBuildRangeInfoKHR{
                primitive_count, 0, 0, 0},
            sizes.buildScratchSize,
            recorded->transient_buffers);
        candidate->blas.emplace(identity, std::move(blas));
        ++candidate->diagnostics.blas_build_count;
    }

    candidate->diagnostics.active_blas_count =
        candidate->blas.size();
    candidate->diagnostics.active_blas_inputs.clear();
    candidate->diagnostics.active_blas_inputs.reserve(
        candidate->blas.size());
    for (const auto &[identity, blas] : candidate->blas) {
        candidate->diagnostics.active_blas_inputs.push_back(
            RayQueryBlasInputDiagnostics{
                .geometry_allocation_id = identity,
                .index_device_address =
                    blas->index_input_address,
                .vertex_device_address =
                    blas->vertex_input_address,
            });
    }
    std::ranges::sort(
        candidate->diagnostics.active_blas_inputs,
        {}, &RayQueryBlasInputDiagnostics::geometry_allocation_id);

    if (!classification.static_instance_indices.empty()) {
        recordMemoryBarrier(
            command_buffer,
            vk::PipelineStageFlagBits::
                eAccelerationStructureBuildKHR,
            vk::PipelineStageFlagBits::
                eAccelerationStructureBuildKHR,
            vk::AccessFlagBits::
                eAccelerationStructureWriteKHR,
            vk::AccessFlagBits::
                eAccelerationStructureReadKHR);

        std::vector<vk::AccelerationStructureInstanceKHR>
            tlas_instances;
        tlas_instances.reserve(
            classification.static_instance_indices.size());
        for (const auto index :
             classification.static_instance_indices) {
            const auto &instance = instances[index];
            const auto blas = candidate->blas.find(
                instance.geometry_allocation_id);
            if (blas == candidate->blas.end()) {
                throw std::runtime_error(
                    "static ray-query TLAS instance has no BLAS");
            }
            if (tlas_instances.size() > 0x00ffffffu) {
                throw std::runtime_error(
                    "ray-query TLAS instance identity exceeds 24 bits");
            }
            tlas_instances.emplace_back(
                toVulkanTransform(instance.world_transform),
                static_cast<std::uint32_t>(
                    tlas_instances.size()),
                0xffu, 0u, vk::GeometryInstanceFlagsKHR{},
                blas->second->address);
        }

        auto instance_buffer = allocateAddressedBuffer(
            vkcore,
            sizeof(vk::AccelerationStructureInstanceKHR) *
                tlas_instances.size(),
            vk::BufferUsageFlagBits::
                    eAccelerationStructureBuildInputReadOnlyKHR |
                vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vma::MemoryUsage::eAutoPreferDevice,
            vma::AllocationCreateFlagBits::
                eHostAccessSequentialWrite,
            16);
        vkcore.writeBuf(
            instance_buffer.buffer, tlas_instances.data(),
            instance_buffer.offset,
            sizeof(vk::AccelerationStructureInstanceKHR) *
                tlas_instances.size());
        recordMemoryBarrier(
            command_buffer,
            vk::PipelineStageFlagBits::eHost,
            vk::PipelineStageFlagBits::
                eAccelerationStructureBuildKHR,
            vk::AccessFlagBits::eHostWrite,
            vk::AccessFlagBits::
                eAccelerationStructureReadKHR);

        const vk::AccelerationStructureGeometryInstancesDataKHR
            instance_data{
                false,
                vk::DeviceOrHostAddressConstKHR{
                    instance_buffer.address}};
        const vk::AccelerationStructureGeometryKHR geometry_info{
            vk::GeometryTypeKHR::eInstances,
            vk::AccelerationStructureGeometryDataKHR{
                instance_data}};
        vk::AccelerationStructureBuildGeometryInfoKHR build_info{
            vk::AccelerationStructureTypeKHR::eTopLevel,
            vk::BuildAccelerationStructureFlagBitsKHR::
                ePreferFastTrace,
            vk::BuildAccelerationStructureModeKHR::eBuild,
            {}, {}, 1, &geometry_info};
        const auto primitive_count = static_cast<std::uint32_t>(
            tlas_instances.size());
        vk::AccelerationStructureBuildSizesInfoKHR sizes;
        device.getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice,
            &build_info, &primitive_count, &sizes,
            vkcore.getAccelerationStructureDispatch());
        candidate->tlas = createAccelerationStructure(
            vkcore,
            vk::AccelerationStructureTypeKHR::eTopLevel,
            sizes.accelerationStructureSize);
        build_info.dstAccelerationStructure =
            candidate->tlas->handle.get();
        recordBuild(
            command_buffer, vkcore, build_info,
            vk::AccelerationStructureBuildRangeInfoKHR{
                primitive_count, 0, 0, 0},
            sizes.buildScratchSize,
            recorded->transient_buffers);
        recorded->transient_buffers.push_back(
            std::move(instance_buffer.buffer));
        ++candidate->diagnostics.tlas_build_count;
        recordMemoryBarrier(
            command_buffer,
            vk::PipelineStageFlagBits::
                eAccelerationStructureBuildKHR,
            vk::PipelineStageFlagBits::eFragmentShader |
                vk::PipelineStageFlagBits::eComputeShader,
            vk::AccessFlagBits::
                eAccelerationStructureWriteKHR,
            vk::AccessFlagBits::
                eAccelerationStructureReadKHR);
    } else {
        candidate->tlas.reset();
    }

    recorded->state = candidate;
    // The renderer leased this exact current batch before target acquisition.
    // Scratch, instance data, and candidate AS handles therefore retire at the
    // submission fence; no frame-count lifetime is introduced here.
    deletion_queue.defer(std::move(recorded));
    return FrameBuild{this, std::move(candidate)};
}

vk::AccelerationStructureKHR
RayQueryAccelerationStructureScope::topLevel() const noexcept {
    return active_state_ != nullptr && active_state_->tlas != nullptr
               ? active_state_->tlas->handle.get()
               : vk::AccelerationStructureKHR{};
}

vk::DeviceAddress
RayQueryAccelerationStructureScope::topLevelDeviceAddress() const noexcept {
    return active_state_ != nullptr && active_state_->tlas != nullptr
               ? active_state_->tlas->address
               : 0;
}

} // namespace Pelican
