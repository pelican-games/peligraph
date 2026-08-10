#include "devicefeaturepolicy.hpp"

#include <utility>

namespace Pelican {

RayQueryDeviceSelection selectRayQueryDeviceFeatures(
    const RayQueryDeviceSupport &support) {
    const auto ray_query_enabled =
        support.acceleration_structure_feature &&
        support.ray_query_feature &&
        support.buffer_device_address_feature &&
        support.acceleration_structure_extension &&
        support.ray_query_extension &&
        support.deferred_host_operations_extension;
    if (!ray_query_enabled) return {};

    const auto ray_tracing_pipeline_enabled =
        support.ray_tracing_pipeline_feature &&
        support.ray_tracing_pipeline_extension &&
        support.shader_group_handle_size != 0 &&
        support.shader_group_base_alignment != 0 &&
        support.shader_group_handle_alignment != 0;

    std::vector<std::string> extensions{
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    };
    if (ray_tracing_pipeline_enabled) {
        extensions.emplace_back(
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    }

    return {
        .acceleration_structure = true,
        .ray_query = true,
        .buffer_device_address = true,
        .ray_tracing_pipeline =
            ray_tracing_pipeline_enabled,
        .min_acceleration_structure_scratch_offset_alignment =
            support
                .min_acceleration_structure_scratch_offset_alignment,
        .shader_group_handle_size =
            ray_tracing_pipeline_enabled
                ? support.shader_group_handle_size
                : 0,
        .shader_group_base_alignment =
            ray_tracing_pipeline_enabled
                ? support.shader_group_base_alignment
                : 0,
        .shader_group_handle_alignment =
            ray_tracing_pipeline_enabled
                ? support.shader_group_handle_alignment
                : 0,
        .device_extensions = std::move(extensions),
    };
}

vma::AllocatorCreateFlags selectVmaAllocatorCreateFlags(
    bool memory_budget_enabled,
    bool buffer_device_address_enabled) noexcept {
    vma::AllocatorCreateFlags result;
    if (memory_budget_enabled) {
        result |= vma::AllocatorCreateFlagBits::eExtMemoryBudget;
    }
    if (buffer_device_address_enabled) {
        result |=
            vma::AllocatorCreateFlagBits::eBufferDeviceAddress;
    }
    return result;
}

} // namespace Pelican
