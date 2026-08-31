#include "devicefeaturepolicy.hpp"

#include <stdexcept>
#include <utility>

namespace Pelican {

RayQueryDeviceSelection selectRayQueryDeviceFeatures(
    const RayQueryDeviceSupport &support) {
    RayQueryDeviceSelection result;
    const auto descriptor_indexing_enabled =
        support.runtime_descriptor_array_feature &&
        support.shader_sampled_image_array_non_uniform_indexing_feature &&
        support.descriptor_binding_partially_bound_feature;
    result.runtime_descriptor_array = descriptor_indexing_enabled;
    result.shader_sampled_image_array_non_uniform_indexing =
        descriptor_indexing_enabled;
    result.descriptor_binding_partially_bound =
        descriptor_indexing_enabled;
    result.max_bound_descriptor_sets = support.max_bound_descriptor_sets;
    result.max_descriptor_set_sampled_images =
        support.max_descriptor_set_sampled_images;
    result.device_name = support.device_name;
    result.ray_material_bindless =
        descriptor_indexing_enabled &&
        support.max_bound_descriptor_sets >= 4 &&
        support.max_descriptor_set_sampled_images >= 1024;

    const auto ray_query_enabled =
        support.acceleration_structure_feature &&
        support.ray_query_feature &&
        support.buffer_device_address_feature &&
        support.acceleration_structure_extension &&
        support.ray_query_extension &&
        support.deferred_host_operations_extension;
    if (!ray_query_enabled) return result;

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

    result.acceleration_structure = true;
    result.ray_query = true;
    result.buffer_device_address = true;
    result.ray_tracing_pipeline = ray_tracing_pipeline_enabled;
    result.min_acceleration_structure_scratch_offset_alignment =
        support.min_acceleration_structure_scratch_offset_alignment;
    result.shader_group_handle_size =
        ray_tracing_pipeline_enabled ? support.shader_group_handle_size : 0;
    result.shader_group_base_alignment =
        ray_tracing_pipeline_enabled ? support.shader_group_base_alignment : 0;
    result.shader_group_handle_alignment =
        ray_tracing_pipeline_enabled
            ? support.shader_group_handle_alignment
            : 0;
    result.ray_tracing_facts =
        ray_tracing_pipeline_enabled
            ? support.ray_tracing_facts
            : RayTracingRuntimeFacts{};
    result.device_extensions = std::move(extensions);
    return result;
}

void requireRayMaterialBindlessDevice(
    const VulkanRuntimeCapabilities &capabilities) {
    const auto reject_bool = [&](std::string_view property, bool actual) {
        if (actual) return;
        throw std::runtime_error(
            "pelican.vulkan.ray_material_bindless_required_unavailable@1: property=" +
            std::string{property} +
            " requested=VK_TRUE actual=VK_FALSE device='" +
            capabilities.device_name + "'");
    };
    reject_bool("runtimeDescriptorArray",
                capabilities.runtime_descriptor_array);
    reject_bool("shaderSampledImageArrayNonUniformIndexing",
                capabilities
                    .shader_sampled_image_array_non_uniform_indexing);
    reject_bool("descriptorBindingPartiallyBound",
                capabilities.descriptor_binding_partially_bound);
    const auto reject_limit = [&](std::string_view property,
                                  std::uint32_t requested,
                                  std::uint32_t actual) {
        if (actual >= requested) return;
        throw std::runtime_error(
            "pelican.vulkan.ray_material_bindless_limit_unavailable@1: property=" +
            std::string{property} + " requested>=" +
            std::to_string(requested) + " actual=" +
            std::to_string(actual) + " device='" +
            capabilities.device_name + "'");
    };
    reject_limit("maxBoundDescriptorSets", 4,
                 capabilities.max_bound_descriptor_sets);
    reject_limit("maxDescriptorSetSampledImages", 1024,
                 capabilities.max_descriptor_set_sampled_images);
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
