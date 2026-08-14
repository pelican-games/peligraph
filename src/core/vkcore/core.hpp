#pragma once

#include "../container.hpp"
#include "buf.hpp"
#include "accelerationstructuredispatch.hpp"
#include "cmdbuf.hpp"
#include "debugutils.hpp"
#include "image.hpp"
#include "memorydiagnostics.hpp"
#include "raytracingpipelinedispatch.hpp"
#include "runtimecapabilities.hpp"
#include "vulkanvalidation.hpp"
#include "windowsurface.hpp"
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct QueueSet {
    uint32_t graphic_queue;
    uint32_t presentation_queue;
    uint32_t compute_queue;
};

enum class VulkanProcessType {
    graphics,
    compute,
};

DECLARE_MODULE(VulkanManageCore) {
    vk::UniqueInstance instance;
    std::optional<PreparedWindowSurface>
        initial_window_surface;
    vk::PhysicalDevice phys_device;
    QueueSet queue_set;
    vk::UniqueDevice device;

    vk::Queue graphic_queue, presen_queue, compute_queue;

    vk::UniqueCommandPool graphic_cmd_pool, compute_cmd_pool;
    vma::UniqueAllocator allocator;
    DebugUtilsDispatch debug_utils;
    VulkanValidationStatus vulkan_validation;
    bool memory_budget_enabled = false;
    VulkanRuntimeCapabilities runtime_capabilities;
    std::vector<std::string>
        enabled_device_extensions;
    PFN_vkCmdSetRenderingAttachmentLocationsKHR
        set_rendering_attachment_locations = nullptr;
    PFN_vkCmdSetRenderingInputAttachmentIndicesKHR
        set_rendering_input_attachment_indices = nullptr;
    PFN_vkReleaseSwapchainImagesEXT
        release_swapchain_images = nullptr;
    PFN_vkCreateAccelerationStructureKHR
        create_acceleration_structure = nullptr;
    PFN_vkDestroyAccelerationStructureKHR
        destroy_acceleration_structure = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR
        get_acceleration_structure_build_sizes = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR
        get_acceleration_structure_device_address = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR
        cmd_build_acceleration_structures = nullptr;
    AccelerationStructureDispatch
        acceleration_structure_dispatch;
    PFN_vkCreateRayTracingPipelinesKHR
        create_ray_tracing_pipelines = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR
        get_ray_tracing_shader_group_handles = nullptr;
    PFN_vkCmdTraceRaysKHR cmd_trace_rays = nullptr;
    PFN_vkDestroyPipeline destroy_pipeline = nullptr;
    RayTracingPipelineDispatch
        ray_tracing_pipeline_dispatch;
    mutable std::mutex presentation_quarantine_mutex;
    std::vector<std::shared_ptr<const void>>
        presentation_quarantine;

  public:
    VulkanManageCore();
    ~VulkanManageCore();

    vk::Device getDevice() const { return device.get(); };
    vk::Instance getInstance() const { return instance.get(); }
    vk::PhysicalDevice getPhysDevice() const { return phys_device; };
    vk::Queue getGraphicsQueue() const { return graphic_queue; }
    uint32_t getGraphicsQueueFamilyIndex() const { return queue_set.graphic_queue; }
    uint32_t
    getBootstrapPresentationQueueFamilyIndex() const {
        return queue_set.presentation_queue;
    }
    std::optional<PreparedWindowSurface>
    takeInitialWindowSurface() noexcept;
    std::vector<std::uint32_t>
    createdQueueFamilyIndices() const;
    std::optional<vk::Queue> createdQueue(
        std::uint32_t family) const noexcept;
    const DebugUtilsDispatch &getDebugUtils() const noexcept { return debug_utils; }
    const VulkanValidationStatus &getVulkanValidationStatus() const noexcept {
        return vulkan_validation;
    }
    const VulkanRuntimeCapabilities &getRuntimeCapabilities() const noexcept {
        return runtime_capabilities;
    }
    const AccelerationStructureDispatch &
    getAccelerationStructureDispatch() const noexcept {
        return acceleration_structure_dispatch;
    }
    const RayTracingPipelineDispatch &
    getRayTracingPipelineDispatch() const noexcept {
        return ray_tracing_pipeline_dispatch;
    }
    std::span<const std::string>
    getEnabledDeviceExtensions() const noexcept {
        return enabled_device_extensions;
    }
    void setRenderingAttachmentLocations(
        vk::CommandBuffer command_buffer,
        const vk::RenderingAttachmentLocationInfoKHR
            &locations) const;
    void setRenderingInputAttachmentIndices(
        vk::CommandBuffer command_buffer,
        const vk::RenderingInputAttachmentIndexInfoKHR
            &indices) const;
    vk::Result releaseSwapchainImages(
        const vk::ReleaseSwapchainImagesInfoEXT
            &release_info) const noexcept;
    DriverMemoryStatus driverMemoryStatus() const;
    void setCurrentFrameIndex(std::uint64_t logical_frame) const noexcept;
    void quarantinePresentationResources(
        std::shared_ptr<const void> resources);
    std::size_t
    quarantinedPresentationResourceCount() const noexcept;

    void waitIdle() const;

    std::vector<CommandBufWrapper> allocCmdBufs(size_t num) const;
    std::vector<vk::UniqueSemaphore> createSemaphores(size_t num) const;

    BufferWrapper allocBuf(vk::DeviceSize bytes_num, vk::BufferUsageFlags usage, vma::MemoryUsage mem_usage,
                           vma::AllocationCreateFlags alloc_flags,
                           VulkanProcessType type = VulkanProcessType::graphics) const;
    void writeBuf(const BufferWrapper &dst, const void *src, vk::DeviceSize offset, vk::DeviceSize bytes_num) const;
    std::vector<uint8_t> readBuf(const BufferWrapper &src, vk::DeviceSize bytes_num) const;

    ImageWrapper allocImage(vk::Extent3D extent, vk::Format format, vk::ImageUsageFlags usage,
                            vma::MemoryUsage mem_usage, vma::AllocationCreateFlags alloc_flags,
                            VulkanProcessType type = VulkanProcessType::graphics,
                            std::span<const vk::Format> compatible_view_formats = {},
                            uint32_t mip_levels = 1,
                            vk::SampleCountFlagBits samples =
                                vk::SampleCountFlagBits::e1,
                            uint32_t array_layers = 1,
                            vk::MemoryPropertyFlags
                                preferred_memory_flags = {},
                            vk::ImageCreateFlags image_flags = {},
                            vk::ImageType image_type =
                                vk::ImageType::e2D) const;
    ImageWrapper allocAliasingImage(
        const ImageWrapper &allocation_owner) const;
    void writeImage(const ImageWrapper &dst, const void *src, vk::DeviceSize bytes_num) const;
};

} // namespace Pelican
