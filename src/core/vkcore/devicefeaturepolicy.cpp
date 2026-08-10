#include "devicefeaturepolicy.hpp"

namespace Pelican {

RayQueryDeviceSelection selectRayQueryDeviceFeatures(
    const RayQueryDeviceSupport &support) {
    const auto enabled =
        support.acceleration_structure_feature &&
        support.ray_query_feature &&
        support.buffer_device_address_feature &&
        support.acceleration_structure_extension &&
        support.ray_query_extension &&
        support.deferred_host_operations_extension;
    if (!enabled) return {};

    return {
        .acceleration_structure = true,
        .ray_query = true,
        .buffer_device_address = true,
        .min_acceleration_structure_scratch_offset_alignment =
            support
                .min_acceleration_structure_scratch_offset_alignment,
        .device_extensions = {
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_RAY_QUERY_EXTENSION_NAME,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        },
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
