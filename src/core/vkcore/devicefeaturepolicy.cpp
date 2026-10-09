#include "devicefeaturepolicy.hpp"

#include <algorithm>
#include <optional>
#include <array>
#include <stdexcept>
#include <utility>

namespace Pelican {

std::size_t selectVulkanPhysicalDevice(std::span<const VulkanDeviceCandidate> devices,
                                      const std::string &override_device) {
    std::optional<std::size_t> selected;
    int best_score = -1;
    std::string diagnostic = "No suitable Vulkan 1.3 GPU";
    if (!override_device.empty()) diagnostic += " for --gpu '" + override_device + "'";
    diagnostic += ". Devices:";
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const auto &device = devices[i];
        diagnostic += " [" + std::to_string(i) + "] " + device.name + " (Vulkan " +
            std::to_string(VK_API_VERSION_MAJOR(device.api_version)) + "." +
            std::to_string(VK_API_VERSION_MINOR(device.api_version)) + ")";
        if (device.api_version < VK_API_VERSION_1_3 || !device.suitable) continue;
        if (!override_device.empty() && override_device != std::to_string(i) &&
            override_device != device.name) continue;
        const int score = (device.type == vk::PhysicalDeviceType::eDiscreteGpu ? 1000 : 0) +
                          (device.unified_queue ? 100 : 0);
        if (score > best_score) { selected = i; best_score = score; }
    }
    if (!selected) throw std::runtime_error(diagnostic);
    return *selected;
}


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
    result.max_descriptor_set_samplers =
        support.max_descriptor_set_samplers;
    result.max_descriptor_set_sampled_images =
        support.max_descriptor_set_sampled_images;
    result.max_per_stage_descriptor_samplers =
        support.max_per_stage_descriptor_samplers;
    result.max_per_stage_descriptor_sampled_images =
        support.max_per_stage_descriptor_sampled_images;
    result.max_per_stage_resources = support.max_per_stage_resources;
    result.device_name = support.device_name;
    result.ray_material_bindless =
        descriptor_indexing_enabled &&
        support.max_bound_descriptor_sets >= 4 &&
        support.max_descriptor_set_samplers >= 1024 &&
        support.max_descriptor_set_sampled_images >= 1024 &&
        support.max_per_stage_descriptor_samplers >= 1024 &&
        support.max_per_stage_descriptor_sampled_images >= 1024 &&
        support.max_per_stage_resources >= 1026;

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
    result.ray_tracing_indirect =
        ray_tracing_pipeline_enabled && support.ray_tracing_indirect_feature;
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

namespace {

void requireRayMaterialBindlessFeatures(
    const VulkanRuntimeCapabilities &capabilities) {
    const auto reject_bool = [&](std::string_view property, bool actual) {
        if (actual) return;
        throw std::runtime_error(
            "pelican.vulkan.ray_material_bindless_required_unavailable@1: property=" +
            std::string{property} +
            " required=VK_TRUE actual=VK_FALSE device='" +
            capabilities.device_name + "'");
    };
    reject_bool("runtimeDescriptorArray",
                capabilities.runtime_descriptor_array);
    reject_bool("shaderSampledImageArrayNonUniformIndexing",
                capabilities
                    .shader_sampled_image_array_non_uniform_indexing);
    reject_bool("descriptorBindingPartiallyBound",
                capabilities.descriptor_binding_partially_bound);
}

void requireRayMaterialBindlessLimit(
    const VulkanRuntimeCapabilities &capabilities,
    std::string_view property, std::uint64_t required,
    std::uint64_t actual) {
    if (actual >= required) return;
    throw std::runtime_error(
        "pelican.vulkan.ray_material_bindless_limit_unavailable@1: property=" +
        std::string{property} + " required>=" +
        std::to_string(required) + " actual=" +
        std::to_string(actual) + " device='" +
        capabilities.device_name + "'");
}

bool samplerDescriptor(vk::DescriptorType type) {
    return type == vk::DescriptorType::eSampler ||
           type == vk::DescriptorType::eCombinedImageSampler;
}

bool sampledImageDescriptor(vk::DescriptorType type) {
    return type == vk::DescriptorType::eSampledImage ||
           type == vk::DescriptorType::eCombinedImageSampler ||
           type == vk::DescriptorType::eUniformTexelBuffer;
}

} // namespace

RayMaterialBindlessLayoutRequirements
rayMaterialBindlessLayoutRequirements(
    std::uint32_t descriptor_set_count,
    std::span<const vk::DescriptorSetLayoutBinding> bindings) {
    RayMaterialBindlessLayoutRequirements result{
        .descriptor_set_count = descriptor_set_count,
    };
    std::array<std::uint64_t, 32> stage_samplers{};
    std::array<std::uint64_t, 32> stage_sampled_images{};
    std::array<std::uint64_t, 32> stage_resources{};
    for (const auto &binding : bindings) {
        const auto count = static_cast<std::uint64_t>(
            binding.descriptorCount);
        const auto sampler = samplerDescriptor(binding.descriptorType);
        const auto sampled_image =
            sampledImageDescriptor(binding.descriptorType);
        if (sampler) result.descriptor_set_samplers += count;
        if (sampled_image) {
            result.descriptor_set_sampled_images += count;
        }
        const auto flags = static_cast<VkShaderStageFlags>(
            binding.stageFlags);
        for (std::uint32_t bit = 0; bit < 32; ++bit) {
            if ((flags & (VkShaderStageFlags{1} << bit)) == 0) continue;
            if (sampler) stage_samplers[bit] += count;
            if (sampled_image) stage_sampled_images[bit] += count;
            stage_resources[bit] += count;
        }
    }
    result.per_stage_descriptor_samplers =
        *std::max_element(stage_samplers.begin(), stage_samplers.end());
    result.per_stage_descriptor_sampled_images = *std::max_element(
        stage_sampled_images.begin(), stage_sampled_images.end());
    result.per_stage_resources =
        *std::max_element(stage_resources.begin(), stage_resources.end());
    return result;
}

void requireRayMaterialBindlessDevice(
    const VulkanRuntimeCapabilities &capabilities) {
    requireRayMaterialBindlessFeatures(capabilities);
    requireRayMaterialBindlessLimit(
        capabilities, "maxBoundDescriptorSets", 4,
        capabilities.max_bound_descriptor_sets);
    requireRayMaterialBindlessLimit(
        capabilities, "maxDescriptorSetSamplers", 1024,
        capabilities.max_descriptor_set_samplers);
    requireRayMaterialBindlessLimit(
        capabilities, "maxDescriptorSetSampledImages", 1024,
        capabilities.max_descriptor_set_sampled_images);
    requireRayMaterialBindlessLimit(
        capabilities, "maxPerStageDescriptorSamplers", 1024,
        capabilities.max_per_stage_descriptor_samplers);
    requireRayMaterialBindlessLimit(
        capabilities, "maxPerStageDescriptorSampledImages", 1024,
        capabilities.max_per_stage_descriptor_sampled_images);
    requireRayMaterialBindlessLimit(
        capabilities, "maxPerStageResources", 1026,
        capabilities.max_per_stage_resources);
}

void requireRayMaterialBindlessDevice(
    const VulkanRuntimeCapabilities &capabilities,
    std::uint32_t descriptor_set_count,
    std::span<const vk::DescriptorSetLayoutBinding> bindings) {
    requireRayMaterialBindlessFeatures(capabilities);
    const auto required = rayMaterialBindlessLayoutRequirements(
        descriptor_set_count, bindings);
    requireRayMaterialBindlessLimit(
        capabilities, "maxBoundDescriptorSets",
        required.descriptor_set_count,
        capabilities.max_bound_descriptor_sets);
    requireRayMaterialBindlessLimit(
        capabilities, "maxDescriptorSetSamplers",
        required.descriptor_set_samplers,
        capabilities.max_descriptor_set_samplers);
    requireRayMaterialBindlessLimit(
        capabilities, "maxDescriptorSetSampledImages",
        required.descriptor_set_sampled_images,
        capabilities.max_descriptor_set_sampled_images);
    requireRayMaterialBindlessLimit(
        capabilities, "maxPerStageDescriptorSamplers",
        required.per_stage_descriptor_samplers,
        capabilities.max_per_stage_descriptor_samplers);
    requireRayMaterialBindlessLimit(
        capabilities, "maxPerStageDescriptorSampledImages",
        required.per_stage_descriptor_sampled_images,
        capabilities.max_per_stage_descriptor_sampled_images);
    requireRayMaterialBindlessLimit(
        capabilities, "maxPerStageResources",
        required.per_stage_resources,
        capabilities.max_per_stage_resources);
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
