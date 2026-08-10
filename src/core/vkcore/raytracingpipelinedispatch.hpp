#pragma once

#include <cstdint>
#include <vulkan/vulkan.hpp>

namespace Pelican {

// Vulkan-Hpp dispatch table for VK_KHR_ray_tracing_pipeline. Pelican does not
// install a process-wide dynamic dispatcher, so extension entry points stay
// owned by VulkanManageCore for the full device lifetime.
struct RayTracingPipelineDispatch {
    PFN_vkCreateRayTracingPipelinesKHR
        vkCreateRayTracingPipelinesKHR = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR
        vkGetRayTracingShaderGroupHandlesKHR = nullptr;
    PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR = nullptr;
    PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;

    std::uint32_t getVkHeaderVersion() const noexcept {
        return VK_HEADER_VERSION;
    }
};

using UniqueRayTracingPipeline =
    vk::UniqueHandle<vk::Pipeline, RayTracingPipelineDispatch>;

} // namespace Pelican
