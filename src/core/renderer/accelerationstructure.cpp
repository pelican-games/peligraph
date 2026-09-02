#include "../vkcore/accelerationstructure.hpp"

#include "../log.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <iomanip>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <picosha2.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Pelican {

namespace {

thread_local RayProfileAccelerationStructureFlagsOverrideForTesting
    ray_profile_as_flags_override_for_testing;

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

bool isBlasIneligibleStaticGeometry(
    const RayQueryGeometryInstanceSnapshot &instance) noexcept {
    return instance.vertex_count == 0 || instance.index_count == 0 ||
           instance.index_count % 3 != 0 || instance.vertex_offset < 0;
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
    std::optional<AddressedBuffer> scratch;
    if (scratch_size != 0) {
        const auto alignment =
            vkcore.getRuntimeCapabilities()
                .min_acceleration_structure_scratch_offset_alignment;
        scratch.emplace(allocateAddressedBuffer(
            vkcore, scratch_size,
            vk::BufferUsageFlagBits::eStorageBuffer |
                vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vma::MemoryUsage::eAutoPreferDevice, {}, alignment));
        build_info.scratchData =
            vk::DeviceOrHostAddressKHR{scratch->address};
    }
    const auto *range_pointer = &range;
    command_buffer.buildAccelerationStructuresKHR(
        1, &build_info, &range_pointer,
        vkcore.getAccelerationStructureDispatch());
    if (scratch) {
        transient_buffers.push_back(
            std::move(scratch->buffer));
    }
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
    std::set<std::uint64_t> blas_ineligible;
    for (std::size_t index = 0; index < instances.size(); ++index) {
        const auto &instance = instances[index];
        const bool deformed = instance.skinned ||
                              instance.morph_deformed ||
                              instance.vat_deformed;
        const bool invalid_static_geometry =
            !deformed && isBlasIneligibleStaticGeometry(instance);
        if (!deformed && !invalid_static_geometry) {
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
        if (invalid_static_geometry) {
            appendUnique(
                blas_ineligible,
                result.excluded.blas_ineligible_names,
                instance);
        }
    }
    result.excluded.primitive_count = excluded.size();
    result.excluded.skinned_primitive_count = skinned.size();
    result.excluded.morph_primitive_count = morph.size();
    result.excluded.vat_primitive_count = vat.size();
    result.excluded.blas_ineligible_primitive_count =
        blas_ineligible.size();
    return result;
}

RayTracingGeometrySnapshot makeRayTracingGeometrySnapshot(
    std::span<const RayQueryGeometryInstanceSnapshot> instances,
    std::uint64_t index_pool_address,
    std::uint64_t vertex_pool_address) {
    static_assert(modelPrimitiveAttributeNormal ==
                  rayGeometryAttributeNormal);
    static_assert(modelPrimitiveAttributeUv0 ==
                  rayGeometryAttributeUv0);
    static_assert(modelPrimitiveAttributeColor0 ==
                  rayGeometryAttributeColor0);
    const auto classification =
        classifyRayQueryStaticGeometry(instances);
    std::map<std::uint64_t, RayTracingGeometrySnapshotRow> unique;
    for (const auto source_index :
         classification.static_instance_indices) {
        const auto &instance = instances[source_index];
        if (instance.geometry_allocation_id == 0 ||
            instance.vertex_offset < 0 || instance.vertex_count == 0 ||
            instance.index_count == 0 || instance.index_count % 3u != 0) {
            throw std::runtime_error(
                "pelican.ray_geometry_snapshot_invalid@1: static geometry has invalid allocation/range metadata");
        }
        RayTracingGeometrySnapshotRow row{
            .geometry_allocation_id =
                instance.geometry_allocation_id,
            .index_count = instance.index_count,
            .index_offset = instance.index_offset,
            .vertex_offset = instance.vertex_offset,
            .vertex_count = instance.vertex_count,
            .gpu =
                RayGeometryTableRow{
                    .vertex_address =
                        vertex_pool_address +
                        sizeof(CommonVertStruct) *
                            static_cast<std::uint32_t>(
                                instance.vertex_offset),
                    .index_address =
                        index_pool_address +
                        sizeof(std::uint32_t) *
                            instance.index_offset,
                    .vertex_stride = static_cast<std::uint32_t>(
                        sizeof(CommonVertStruct)),
                    .index_type = static_cast<std::uint32_t>(
                        vk::IndexType::eUint32),
                    .attribute_presence =
                        instance.attribute_presence,
                },
        };
        const auto [found, inserted] = unique.emplace(
            row.geometry_allocation_id, row);
        if (!inserted && found->second != row) {
            throw std::runtime_error(
                "pelican.ray_geometry_snapshot_invalid@1: geometry allocation identity maps to inconsistent ranges or attributes");
        }
    }
    RayTracingGeometrySnapshot result;
    result.rows.reserve(unique.size());
    std::string encoded;
    for (const auto &[identity, row] : unique) {
        result.rows.push_back(row);
        encoded.append(reinterpret_cast<const char *>(&identity),
                       sizeof(identity));
        encoded.append(reinterpret_cast<const char *>(&row.index_count),
                       sizeof(row.index_count));
        encoded.append(reinterpret_cast<const char *>(&row.index_offset),
                       sizeof(row.index_offset));
        encoded.append(reinterpret_cast<const char *>(&row.vertex_offset),
                       sizeof(row.vertex_offset));
        encoded.append(reinterpret_cast<const char *>(&row.vertex_count),
                       sizeof(row.vertex_count));
        encoded.append(reinterpret_cast<const char *>(&row.gpu),
                       sizeof(row.gpu));
    }
    result.digest_sha256 = picosha2::hash256_hex_string(
        encoded.begin(), encoded.end());
    return result;
}

void validateRayProfileAccelerationStructureFlags(
    bool profile_active,
    vk::GeometryFlagsKHR geometry_flags,
    vk::GeometryInstanceFlagsKHR instance_flags,
    std::string_view diagnostic) {
    if (!profile_active) return;
    if ((geometry_flags & vk::GeometryFlagBitsKHR::eOpaque) ||
        (instance_flags &
         vk::GeometryInstanceFlagBitsKHR::eForceOpaque)) {
        throw std::runtime_error(
            "pelican.ray_profile_opaque_as@1: " +
            std::string{diagnostic} +
            " requires non-opaque geometry and instance flags");
    }
}

ScopedRayProfileAccelerationStructureFlagsOverrideForTesting::
    ScopedRayProfileAccelerationStructureFlagsOverrideForTesting(
        RayProfileAccelerationStructureFlagsOverrideForTesting replacement)
    : previous_{ray_profile_as_flags_override_for_testing} {
    ray_profile_as_flags_override_for_testing = replacement;
}

ScopedRayProfileAccelerationStructureFlagsOverrideForTesting::
    ~ScopedRayProfileAccelerationStructureFlagsOverrideForTesting() {
    ray_profile_as_flags_override_for_testing = previous_;
}

std::string canonicalTransformIdentity(const glm::mat4 &transform) {
    // Spell the IEEE-754 payload as fixed-width hexadecimal so the sort key
    // is independent of locale, stream float formatting, and host byte order.
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (glm::length_t column = 0; column < 4; ++column) {
        for (glm::length_t row = 0; row < 4; ++row) {
            output << std::setw(8)
                   << std::bit_cast<std::uint32_t>(
                          transform[column][row]);
        }
    }
    return output.str();
}

RaySceneRowSnapshot resolveRayTracingSceneRows(
    std::span<const RayQueryGeometryInstanceSnapshot> instances,
    const RayTracingProjectConfig &project) {
    // Validate authored instance declarations before static classification.
    // Otherwise a component attached only to an excluded primitive would be
    // erased by the classification filter and silently ignored.
    std::map<std::uint64_t, bool> declared_targets;
    for (const auto &instance : instances) {
        if (!instance.ray_instance) continue;
        auto [target, inserted] = declared_targets.emplace(
            instance.authoring_object_id, false);
        (void)inserted;
        const bool eligible =
            !instance.skinned && !instance.morph_deformed &&
            !instance.vat_deformed &&
            !isBlasIneligibleStaticGeometry(instance);
        target->second = target->second || eligible;
    }
    for (const auto &[authoring_object_id, has_static_geometry] :
         declared_targets) {
        if (authoring_object_id == 0 || !has_static_geometry) {
            throw std::runtime_error(
                "pelican.ray_instance_not_ray_traced@1: ray_instance authoring object " +
                std::to_string(authoring_object_id) +
                " has no BLAS-eligible static geometry");
        }
    }
    const auto classification =
        classifyRayQueryStaticGeometry(instances);
    std::vector<RaySceneRowInput> inputs;
    inputs.reserve(classification.static_instance_indices.size());
    for (const auto source_index :
         classification.static_instance_indices) {
        const auto &instance = instances[source_index];
        const auto asset =
            instance.material_asset_reference.empty()
                ? "asset-id:" +
                      std::to_string(instance.asset_identity)
                : instance.material_asset_reference;
        inputs.push_back(RaySceneRowInput{
            // Authored/content identity only: instance slot, generation, and
            // draw publication ordinal would make q depend on publish order.
            .scene_identity =
                asset + "@" +
                canonicalTransformIdentity(instance.world_transform),
            .geometry_identity =
                asset + "#" +
                std::to_string(instance.geometry_allocation_id) + ":" +
                std::to_string(instance.index_offset) + ":" +
                std::to_string(instance.vertex_offset) + ":" +
                std::to_string(instance.mesh_index) + ":" +
                std::to_string(instance.primitive_index) + ":" +
                std::to_string(instance.node_index),
            .geometry_match_identity =
                std::to_string(instance.geometry_allocation_id) + ":" +
                std::to_string(instance.index_offset) + ":" +
                std::to_string(instance.index_count) + ":" +
                std::to_string(instance.vertex_offset) + ":" +
                std::to_string(instance.vertex_count) + "@" +
                canonicalTransformIdentity(instance.world_transform),
            .material_document =
                instance.material_document_reference,
            .material_asset =
                instance.material_asset_reference,
            .material_name = instance.material_name,
            .source_material_index =
                instance.source_material_index,
            .resolved_material_handle =
                instance.resolved_material_handle,
            .resolved_material_generation =
                instance.resolved_material_generation,
            .resolved_material_lease =
                instance.resolved_material_lease,
            .geometry_allocation_id =
                instance.geometry_allocation_id,
            .authoring_object_id =
                instance.authoring_object_id,
            .ray_instance = instance.ray_instance,
            .source_index = source_index,
            .alpha_override_mask =
                instance.alpha_override_mask,
            .alpha_override_base_color_factor =
                instance.alpha_override_base_color_factor,
            .alpha_override_uv_offset_scale =
                instance.alpha_override_uv_offset_scale,
            .alpha_override_uv_rotation =
                instance.alpha_override_uv_rotation,
        });
    }
    return resolveRaySceneRows(
        inputs, project.layout,
        project.bindings ? &*project.bindings : nullptr);
}

std::vector<RayTracingTlasInstancePlanEntry>
lowerRayTracingTlasInstancePlan(
    std::span<const RayQueryGeometryInstanceSnapshot> instances,
    std::span<const std::size_t> static_instance_indices,
    const RaySceneRowSnapshot *ray_scene_rows,
    std::uint32_t ray_type_count) {
    std::vector<RayTracingTlasInstancePlanEntry> result;
    result.reserve(static_instance_indices.size());
    if (ray_scene_rows == nullptr) {
        for (const auto source_index : static_instance_indices) {
            if (source_index >= instances.size() ||
                result.size() > 0x00ffffffu) {
                throw std::runtime_error(
                    "ray-query TLAS instance identity exceeds 24 bits");
            }
            result.push_back({
                .source_index = source_index,
                .q = 0,
                .instance_custom_index =
                    static_cast<std::uint32_t>(result.size()),
                .instance_sbt_record_offset = 0,
                .instance_mask =
                    rayAccelerationStructureCanonicalDefaults.mask,
                .instance_flags =
                    rayAccelerationStructureCanonicalDefaults.instance_flags,
                .geometry_flags =
                    rayAccelerationStructureCanonicalDefaults.geometry_flags,
            });
        }
        return result;
    }
    if (ray_type_count == 0 ||
        ray_scene_rows->rows.size() != static_instance_indices.size()) {
        throw std::runtime_error(
            "pelican.sbt.row_snapshot_invalid@1: TLAS row count or ray type count does not match the static scene");
    }
    std::vector<bool> is_static(instances.size(), false);
    for (const auto index : static_instance_indices) {
        if (index >= instances.size() || is_static[index]) {
            throw std::runtime_error(
                "pelican.sbt.row_snapshot_invalid@1: static scene indices are invalid or duplicated");
        }
        is_static[index] = true;
    }
    std::vector<bool> consumed(instances.size(), false);
    for (std::size_t canonical_index = 0;
         canonical_index < ray_scene_rows->rows.size();
         ++canonical_index) {
        const auto &row = ray_scene_rows->rows[canonical_index];
        if (row.q != canonical_index ||
            row.source_index >= instances.size() ||
            !is_static[row.source_index] ||
            consumed[row.source_index]) {
            throw std::runtime_error(
                "pelican.sbt.row_snapshot_invalid@1: TLAS rows must be a canonical permutation of the static scene");
        }
        consumed[row.source_index] = true;
        result.push_back({
            .source_index = row.source_index,
            .q = row.q,
            .instance_custom_index = row.instance_custom_index,
            .instance_sbt_record_offset =
                overrideRayTracingTlasRecordOffsetForTesting(
                    row.q,
                    rayTracingInstanceRecordOffset(
                        row.q, ray_type_count)),
            .instance_mask = row.instance_mask,
            .instance_flags = row.instance_flags,
            .geometry_flags = row.geometry_flags,
        });
    }
    return result;
}

nlohmann::json rayQueryAccelerationStructureDiagnosticsToJson(
    const RayQueryAccelerationStructureDiagnostics &diagnostics) {
    nlohmann::json inputs = nlohmann::json::array();
    for (const auto &input : diagnostics.active_blas_inputs) {
        inputs.push_back({
            {"geometry_allocation_id",
             input.geometry_allocation_id},
            {"geometry_flags", input.geometry_flags},
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
        {"blas_variant_count", diagnostics.blas_variant_count},
        {"resident_blas_keys", inputs},
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
          {"blas_ineligible_primitive_count",
           diagnostics.excluded
               .blas_ineligible_primitive_count},
          {"skinned_names",
           diagnostics.excluded.skinned_names},
          {"morph_names", diagnostics.excluded.morph_names},
          {"vat_names", diagnostics.excluded.vat_names},
          {"blas_ineligible_names",
           diagnostics.excluded.blas_ineligible_names}}},
        {"active_blas_inputs", std::move(inputs)},
    };
}

struct RayQueryAccelerationStructureScope::State {
    struct BlasVariantKey {
        std::uint64_t geometry_allocation_id = 0;
        std::uint32_t geometry_flags = 0;

        auto operator<=>(const BlasVariantKey &) const = default;
    };

    std::map<
        BlasVariantKey,
        std::shared_ptr<AccelerationStructureResource>>
        blas;
    std::shared_ptr<AccelerationStructureResource> tlas;
    std::shared_ptr<const RaySceneRowSnapshot> ray_scene_rows;
    std::shared_ptr<const RayTracingGeometrySnapshot>
        geometry_snapshot;
    std::uint32_t ray_type_count = 0;
    // The dynamic hit records contain device addresses consumed by the same
    // submission as this TLAS.  Retaining them in the unpublished candidate
    // makes the FrameBuild commit the row map, SBT, and TLAS as one unit and
    // keeps the buffers alive through the submission lease.
    std::shared_ptr<const RayTracingSceneShaderBindingTables>
        ray_tracing_scene_tables;
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

vk::AccelerationStructureKHR
RayQueryAccelerationStructureScope::FrameBuild::topLevel() const noexcept {
    return candidate_ != nullptr && candidate_->tlas != nullptr
               ? candidate_->tlas->handle.get()
               : vk::AccelerationStructureKHR{};
}

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
                "VAT={} [{}], BLAS-ineligible={} [{}]. A compute deform "
                "pass for deformed geometry is future work; invalid static "
                "triangle ranges remain raster-only.",
                owner_->diagnostics_.active_blas_count,
                owner_->diagnostics_.tlas_instance_count,
                excluded.primitive_count,
                excluded.instance_count,
                excluded.skinned_primitive_count,
                joinNames(excluded.skinned_names),
                excluded.morph_primitive_count,
                joinNames(excluded.morph_names),
                excluded.vat_primitive_count,
                joinNames(excluded.vat_names),
                excluded.blas_ineligible_primitive_count,
                joinNames(excluded.blas_ineligible_names));
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
    std::shared_ptr<const RayTracingGeometrySnapshot>
        geometry_snapshot,
    std::uint64_t model_rebuild_generation,
    std::shared_ptr<const RaySceneRowSnapshot> ray_scene_rows,
    std::uint32_t ray_type_count,
    std::shared_ptr<const RayTracingSceneShaderBindingTables>
        ray_tracing_scene_tables,
    bool material_alpha_profile_active) {
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
    if (!geometry_snapshot) {
        const auto device = vkcore.getDevice();
        geometry_snapshot =
            std::make_shared<RayTracingGeometrySnapshot>(
                makeRayTracingGeometrySnapshot(
                    instances, bufferDeviceAddress(device, index_buffer),
                    bufferDeviceAddress(device, vertex_buffer)));
    }
    candidate->geometry_snapshot = std::move(geometry_snapshot);
    candidate->ray_scene_rows = std::move(ray_scene_rows);
    candidate->ray_type_count = ray_type_count;
    candidate->ray_tracing_scene_tables =
        std::move(ray_tracing_scene_tables);
    if (candidate->ray_scene_rows && candidate->ray_type_count == 0) {
        throw std::runtime_error(
            "pelican.sbt.row_snapshot_invalid@1: layout scene requires a non-zero ray type count");
    }
    if (static_cast<bool>(candidate->ray_scene_rows) !=
        static_cast<bool>(candidate->ray_tracing_scene_tables)) {
        throw std::runtime_error(
            "pelican.ray_tracing.scene_generation_mismatch@1: row snapshot and complete SBT must be prepared together");
    }
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
    if (!classification.static_instance_indices.empty()) {
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
        !model_invalidated &&
        !ray_profile_as_flags_override_for_testing
             .blas_geometry_flags;
    std::map<std::uint64_t,
             const RayTracingGeometrySnapshotRow *>
        unique_geometry;
    for (const auto &geometry :
         candidate->geometry_snapshot->rows) {
        unique_geometry.emplace(
            geometry.geometry_allocation_id, &geometry);
    }
    for (const auto index :
         classification.static_instance_indices) {
        const auto &instance = instances[index];
        if (instance.geometry_allocation_id == 0) {
            throw std::runtime_error(
                "static ray-query primitive has no geometry allocation identity");
        }
        const auto found = unique_geometry.find(
            instance.geometry_allocation_id);
        if (found == unique_geometry.end() ||
            found->second->index_count != instance.index_count ||
            found->second->index_offset != instance.index_offset ||
            found->second->vertex_offset != instance.vertex_offset ||
            found->second->vertex_count != instance.vertex_count ||
            found->second->gpu.attribute_presence !=
                instance.attribute_presence) {
            throw std::runtime_error(
                "pelican.ray_geometry_snapshot_invalid@1: AS input does not match the immutable geometry snapshot");
        }
    }
    std::set<std::uint64_t> instance_allocations;
    for (const auto index :
         classification.static_instance_indices) {
        instance_allocations.emplace(
            instances[index].geometry_allocation_id);
    }
    if (unique_geometry.size() != instance_allocations.size()) {
        throw std::runtime_error(
            "pelican.ray_geometry_snapshot_invalid@1: immutable geometry snapshot has missing or extra allocation rows");
    }

    const auto planned_instances = lowerRayTracingTlasInstancePlan(
        instances, classification.static_instance_indices,
        candidate->ray_scene_rows.get(), candidate->ray_type_count);
    std::set<State::BlasVariantKey> required_variants;
    for (const auto &planned : planned_instances) {
        auto flags = vk::GeometryFlagsKHR{planned.geometry_flags};
        flags |= ray_profile_as_flags_override_for_testing
                     .blas_geometry_flags;
        required_variants.emplace(State::BlasVariantKey{
            .geometry_allocation_id =
                instances[planned.source_index]
                    .geometry_allocation_id,
            .geometry_flags = static_cast<std::uint32_t>(
                static_cast<VkGeometryFlagsKHR>(flags)),
        });
    }

    for (const auto &variant : required_variants) {
        const auto identity = variant.geometry_allocation_id;
        const auto geometry_found = unique_geometry.find(identity);
        if (geometry_found == unique_geometry.end()) {
            throw std::runtime_error(
                "pelican.ray_geometry_snapshot_invalid@1: BLAS variant has no geometry row");
        }
        const auto *instance = geometry_found->second;
        if (can_reuse) {
            const auto existing =
                active_state_->blas.find(variant);
            if (existing != active_state_->blas.end()) {
                candidate->blas.emplace(variant,
                                        existing->second);
                continue;
            }
        }
        const auto index_address = instance->gpu.index_address;
        const auto vertex_address = instance->gpu.vertex_address;
        const vk::AccelerationStructureGeometryTrianglesDataKHR
            triangles{
                vk::Format::eR32G32B32Sfloat,
                vk::DeviceOrHostAddressConstKHR{vertex_address},
                sizeof(CommonVertStruct),
                instance->vertex_count - 1,
                vk::IndexType::eUint32,
                vk::DeviceOrHostAddressConstKHR{index_address},
            };
        const auto geometry_flags =
            vk::GeometryFlagsKHR{variant.geometry_flags};
        validateRayProfileAccelerationStructureFlags(
            material_alpha_profile_active, geometry_flags, {},
            "BLAS geometry '" + std::to_string(identity) + "'");
        const vk::AccelerationStructureGeometryKHR geometry_info{
            vk::GeometryTypeKHR::eTriangles,
            vk::AccelerationStructureGeometryDataKHR{triangles},
            geometry_flags};
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
        candidate->blas.emplace(variant, std::move(blas));
        ++candidate->diagnostics.blas_build_count;
    }

    candidate->diagnostics.active_blas_count =
        candidate->blas.size();
    candidate->diagnostics.blas_variant_count =
        candidate->blas.size();
    candidate->diagnostics.active_blas_inputs.clear();
    candidate->diagnostics.active_blas_inputs.reserve(
        candidate->blas.size());
    for (const auto &[variant, blas] : candidate->blas) {
        candidate->diagnostics.active_blas_inputs.push_back(
            RayQueryBlasInputDiagnostics{
                .geometry_allocation_id =
                    variant.geometry_allocation_id,
                .geometry_flags = variant.geometry_flags,
                .index_device_address =
                    blas->index_input_address,
                .vertex_device_address =
                    blas->vertex_input_address,
            });
    }
    std::ranges::sort(
        candidate->diagnostics.active_blas_inputs,
        [](const auto &left, const auto &right) {
            return std::tie(left.geometry_allocation_id,
                            left.geometry_flags) <
                   std::tie(right.geometry_allocation_id,
                            right.geometry_flags);
        });

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
    }

    std::vector<vk::AccelerationStructureInstanceKHR>
        tlas_instances;
    tlas_instances.reserve(classification.static_instance_indices.size());
    for (const auto &planned : planned_instances) {
        const auto &instance = instances[planned.source_index];
        auto resolved_geometry_flags =
            vk::GeometryFlagsKHR{planned.geometry_flags};
        resolved_geometry_flags |=
            ray_profile_as_flags_override_for_testing
                .blas_geometry_flags;
        const State::BlasVariantKey blas_key{
            .geometry_allocation_id =
                instance.geometry_allocation_id,
            .geometry_flags = static_cast<std::uint32_t>(
                static_cast<VkGeometryFlagsKHR>(
                    resolved_geometry_flags)),
        };
        const auto blas = candidate->blas.find(
            blas_key);
        if (blas == candidate->blas.end()) {
            throw std::runtime_error(
                "static ray-query TLAS instance has no BLAS");
        }
        auto instance_flags =
            vk::GeometryInstanceFlagsKHR{planned.instance_flags};
        instance_flags |= ray_profile_as_flags_override_for_testing
                              .tlas_instance_flags;
        validateRayProfileAccelerationStructureFlags(
            material_alpha_profile_active, {}, instance_flags,
            "TLAS instance row " + std::to_string(planned.q));
        tlas_instances.emplace_back(
            toVulkanTransform(instance.world_transform),
            planned.instance_custom_index,
            planned.instance_mask,
            planned.instance_sbt_record_offset,
            instance_flags,
            blas->second->address);
    }

    // Vulkan requires a valid, 16-byte-aligned instances address even when
    // primitiveCount is zero. Keep one inactive record as backing storage;
    // the zero build count makes the resulting TLAS a valid miss-only scene.
    const auto instance_capacity = std::max<std::size_t>(
        tlas_instances.size(), 1);
    auto instance_buffer = allocateAddressedBuffer(
        vkcore,
        sizeof(vk::AccelerationStructureInstanceKHR) *
            instance_capacity,
        vk::BufferUsageFlagBits::
                eAccelerationStructureBuildInputReadOnlyKHR |
            vk::BufferUsageFlagBits::eShaderDeviceAddress,
        vma::MemoryUsage::eAutoPreferDevice,
        vma::AllocationCreateFlagBits::
            eHostAccessSequentialWrite,
        16);
    if (tlas_instances.empty()) {
        const vk::AccelerationStructureInstanceKHR inactive{};
        vkcore.writeBuf(
            instance_buffer.buffer, &inactive,
            instance_buffer.offset, sizeof(inactive));
    } else {
        vkcore.writeBuf(
            instance_buffer.buffer, tlas_instances.data(),
            instance_buffer.offset,
            sizeof(vk::AccelerationStructureInstanceKHR) *
                tlas_instances.size());
    }
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
    const auto tlas_geometry_flags =
        ray_profile_as_flags_override_for_testing
            .tlas_geometry_flags;
    validateRayProfileAccelerationStructureFlags(
        material_alpha_profile_active, tlas_geometry_flags, {},
        "TLAS instances geometry");
    const vk::AccelerationStructureGeometryKHR geometry_info{
        vk::GeometryTypeKHR::eInstances,
        vk::AccelerationStructureGeometryDataKHR{
            instance_data},
        tlas_geometry_flags};
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
    vk::PipelineStageFlags ray_consumer_stages =
        vk::PipelineStageFlagBits::eFragmentShader |
        vk::PipelineStageFlagBits::eComputeShader;
    if (vkcore.getRuntimeCapabilities()
            .ray_tracing_pipeline) {
        ray_consumer_stages |=
            vk::PipelineStageFlagBits::eRayTracingShaderKHR;
    }
    recordMemoryBarrier(
        command_buffer,
        vk::PipelineStageFlagBits::
            eAccelerationStructureBuildKHR,
        ray_consumer_stages,
        vk::AccessFlagBits::
            eAccelerationStructureWriteKHR,
        vk::AccessFlagBits::
            eAccelerationStructureReadKHR);

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

const RaySceneRowSnapshot *
RayQueryAccelerationStructureScope::
    activeRaySceneRowsForTesting() const noexcept {
    return active_state_ && active_state_->ray_scene_rows
               ? active_state_->ray_scene_rows.get()
               : nullptr;
}

const RayTracingGeometrySnapshot *
RayQueryAccelerationStructureScope::
    activeGeometrySnapshotForTesting() const noexcept {
    return active_state_ && active_state_->geometry_snapshot
               ? active_state_->geometry_snapshot.get()
               : nullptr;
}

vk::DeviceAddress
RayQueryAccelerationStructureScope::topLevelDeviceAddress() const noexcept {
    return active_state_ != nullptr && active_state_->tlas != nullptr
               ? active_state_->tlas->address
               : 0;
}

} // namespace Pelican
