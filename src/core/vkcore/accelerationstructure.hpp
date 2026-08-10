#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class DeletionQueue;
class VulkanManageCore;
struct BufferWrapper;

// Renderer-owned data frozen before recording begins. vkcore deliberately
// sees only raw stable identities/ranges and has no dependency on model or
// draw-inventory types.
struct RayQueryGeometryInstanceSnapshot {
    std::uint64_t asset_identity = 0;
    std::uint64_t geometry_allocation_id = 0;
    std::uint32_t index_count = 0;
    std::uint32_t index_offset = 0;
    std::int32_t vertex_offset = 0;
    std::uint32_t vertex_count = 0;
    bool skinned = false;
    bool morph_deformed = false;
    bool vat_deformed = false;
    std::uint32_t mesh_index = 0;
    std::uint32_t primitive_index = 0;
    std::uint32_t node_index =
        std::numeric_limits<std::uint32_t>::max();
    glm::mat4 world_transform{1.0F};
};

struct RayQueryGeometryExclusionDiagnostics {
    std::size_t primitive_count = 0;
    std::size_t instance_count = 0;
    std::size_t skinned_primitive_count = 0;
    std::size_t morph_primitive_count = 0;
    std::size_t vat_primitive_count = 0;
    std::size_t blas_ineligible_primitive_count = 0;
    std::vector<std::string> skinned_names;
    std::vector<std::string> morph_names;
    std::vector<std::string> vat_names;
    std::vector<std::string> blas_ineligible_names;

    bool operator==(
        const RayQueryGeometryExclusionDiagnostics &) const = default;
};

struct RayQueryStaticGeometryClassification {
    std::vector<std::size_t> static_instance_indices;
    RayQueryGeometryExclusionDiagnostics excluded;
};

RayQueryStaticGeometryClassification classifyRayQueryStaticGeometry(
    std::span<const RayQueryGeometryInstanceSnapshot> instances);

struct RayQueryBlasInputDiagnostics {
    std::uint64_t geometry_allocation_id = 0;
    std::uint64_t index_device_address = 0;
    std::uint64_t vertex_device_address = 0;

    bool operator==(
        const RayQueryBlasInputDiagnostics &) const = default;
};

struct RayQueryAccelerationStructureDiagnostics {
    bool requested = false;
    std::uint64_t blas_build_count = 0;
    std::uint64_t tlas_build_count = 0;
    std::size_t active_blas_count = 0;
    std::size_t tlas_instance_count = 0;
    std::uint64_t geometry_pool_generation = 0;
    std::uint64_t model_rebuild_generation = 0;
    std::uint64_t geometry_pool_invalidation_count = 0;
    std::uint64_t model_rebuild_invalidation_count = 0;
    RayQueryGeometryExclusionDiagnostics excluded;
    std::vector<RayQueryBlasInputDiagnostics> active_blas_inputs;
};

nlohmann::json rayQueryAccelerationStructureDiagnosticsToJson(
    const RayQueryAccelerationStructureDiagnostics &diagnostics);

// Owner-scope resource for the static-only ray-query scene. FrameBuild is a
// publication token: commands and their resources are leased immediately,
// but the new BLAS/TLAS set becomes active only after the real submission
// succeeds.
class RayQueryAccelerationStructureScope {
    struct State;

  public:
    class FrameBuild {
        friend class RayQueryAccelerationStructureScope;
        RayQueryAccelerationStructureScope *owner_ = nullptr;
        std::shared_ptr<State> candidate_;

        FrameBuild(RayQueryAccelerationStructureScope *owner,
                   std::shared_ptr<State> candidate) noexcept;

      public:
        FrameBuild() noexcept;
        ~FrameBuild();
        FrameBuild(FrameBuild &&) noexcept;
        FrameBuild &operator=(FrameBuild &&) noexcept;
        FrameBuild(const FrameBuild &) = delete;
        FrameBuild &operator=(const FrameBuild &) = delete;

        vk::AccelerationStructureKHR topLevel() const noexcept;
        void commit() noexcept;
    };

  private:
    std::shared_ptr<State> active_state_;
    RayQueryAccelerationStructureDiagnostics diagnostics_{
        .requested = true,
    };

  public:
    RayQueryAccelerationStructureScope() = default;
    ~RayQueryAccelerationStructureScope();
    RayQueryAccelerationStructureScope(
        const RayQueryAccelerationStructureScope &) = delete;
    RayQueryAccelerationStructureScope &operator=(
        const RayQueryAccelerationStructureScope &) = delete;

    FrameBuild recordFrame(
        vk::CommandBuffer command_buffer,
        VulkanManageCore &vkcore,
        DeletionQueue &deletion_queue,
        const BufferWrapper &index_buffer,
        const BufferWrapper &vertex_buffer,
        std::uint64_t geometry_pool_generation,
        std::span<const RayQueryGeometryInstanceSnapshot> instances,
        std::uint64_t model_rebuild_generation);

    const RayQueryAccelerationStructureDiagnostics &
    diagnostics() const noexcept {
        return diagnostics_;
    }
    vk::AccelerationStructureKHR topLevel() const noexcept;
    vk::DeviceAddress topLevelDeviceAddress() const noexcept;
};

} // namespace Pelican
