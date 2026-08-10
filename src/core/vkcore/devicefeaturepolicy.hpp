#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vk_mem_alloc.hpp>

namespace Pelican {

struct RayQueryDeviceSupport {
    bool acceleration_structure_feature = false;
    bool ray_query_feature = false;
    bool buffer_device_address_feature = false;
    bool acceleration_structure_extension = false;
    bool ray_query_extension = false;
    bool deferred_host_operations_extension = false;
    bool ray_tracing_pipeline_feature = false;
    bool ray_tracing_pipeline_extension = false;
    std::uint32_t
        min_acceleration_structure_scratch_offset_alignment = 0;
    std::uint32_t shader_group_handle_size = 0;
    std::uint32_t shader_group_base_alignment = 0;
    std::uint32_t shader_group_handle_alignment = 0;
};

// Ray query is one atomic optional device contract. None of its extensions or
// features are enabled unless the complete dependency set is available.
struct RayQueryDeviceSelection {
    bool acceleration_structure = false;
    bool ray_query = false;
    bool buffer_device_address = false;
    bool ray_tracing_pipeline = false;
    std::uint32_t
        min_acceleration_structure_scratch_offset_alignment = 0;
    std::uint32_t shader_group_handle_size = 0;
    std::uint32_t shader_group_base_alignment = 0;
    std::uint32_t shader_group_handle_alignment = 0;
    std::vector<std::string> device_extensions;
};

RayQueryDeviceSelection selectRayQueryDeviceFeatures(
    const RayQueryDeviceSupport &support);

vma::AllocatorCreateFlags selectVmaAllocatorCreateFlags(
    bool memory_budget_enabled,
    bool buffer_device_address_enabled) noexcept;

} // namespace Pelican
