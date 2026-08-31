#pragma once

#include "runtimecapabilities.hpp"

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
    bool runtime_descriptor_array_feature = false;
    bool shader_sampled_image_array_non_uniform_indexing_feature = false;
    bool descriptor_binding_partially_bound_feature = false;
    std::uint32_t max_bound_descriptor_sets = 0;
    std::uint32_t max_descriptor_set_sampled_images = 0;
    std::string device_name;
    std::uint32_t
        min_acceleration_structure_scratch_offset_alignment = 0;
    std::uint32_t shader_group_handle_size = 0;
    std::uint32_t shader_group_base_alignment = 0;
    std::uint32_t shader_group_handle_alignment = 0;
    RayTracingRuntimeFacts ray_tracing_facts;
};

// Ray query is one atomic optional device contract. None of its extensions or
// features are enabled unless the complete dependency set is available.
struct RayQueryDeviceSelection {
    bool acceleration_structure = false;
    bool ray_query = false;
    bool buffer_device_address = false;
    bool ray_tracing_pipeline = false;
    bool runtime_descriptor_array = false;
    bool shader_sampled_image_array_non_uniform_indexing = false;
    bool descriptor_binding_partially_bound = false;
    bool ray_material_bindless = false;
    std::uint32_t max_bound_descriptor_sets = 0;
    std::uint32_t max_descriptor_set_sampled_images = 0;
    std::string device_name;
    std::uint32_t
        min_acceleration_structure_scratch_offset_alignment = 0;
    std::uint32_t shader_group_handle_size = 0;
    std::uint32_t shader_group_base_alignment = 0;
    std::uint32_t shader_group_handle_alignment = 0;
    RayTracingRuntimeFacts ray_tracing_facts;
    std::vector<std::string> device_extensions;
};

RayQueryDeviceSelection selectRayQueryDeviceFeatures(
    const RayQueryDeviceSupport &support);

void requireRayMaterialBindlessDevice(
    const VulkanRuntimeCapabilities &capabilities);

vma::AllocatorCreateFlags selectVmaAllocatorCreateFlags(
    bool memory_budget_enabled,
    bool buffer_device_address_enabled) noexcept;

} // namespace Pelican
