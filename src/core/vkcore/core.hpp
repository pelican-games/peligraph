#pragma once

#include "../container.hpp"
#include "buf.hpp"
#include "cmdbuf.hpp"
#include "debugutils.hpp"
#include "image.hpp"
#include "memorydiagnostics.hpp"
#include <cstdint>
#include <span>
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
    vk::UniqueSurfaceKHR surface;
    vk::PhysicalDevice phys_device;
    QueueSet queue_set;
    vk::UniqueDevice device;

    vk::Queue graphic_queue, presen_queue, compute_queue;

    vk::UniqueCommandPool graphic_cmd_pool, compute_cmd_pool;
    vma::UniqueAllocator allocator;
    DebugUtilsDispatch debug_utils;
    bool memory_budget_enabled = false;

  public:
    VulkanManageCore();
    ~VulkanManageCore();

    vk::Device getDevice() const { return device.get(); };
    vk::Instance getInstance() const { return instance.get(); }
    vk::PhysicalDevice getPhysDevice() const { return phys_device; };
    vk::Queue getGraphicsQueue() const { return graphic_queue; }
    vk::SurfaceKHR getSurface() const;
    vk::Queue getPresentationQueue() const { return presen_queue; }
    uint32_t getGraphicsQueueFamilyIndex() const { return queue_set.graphic_queue; }
    uint32_t getPresentationQueueFamilyIndex() const { return queue_set.presentation_queue; }
    const DebugUtilsDispatch &getDebugUtils() const noexcept { return debug_utils; }
    DriverMemoryStatus driverMemoryStatus() const;
    void setCurrentFrameIndex(std::uint64_t logical_frame) const noexcept;

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
                            uint32_t mip_levels = 1) const;
    void writeImage(const ImageWrapper &dst, const void *src, vk::DeviceSize bytes_num) const;
};

} // namespace Pelican
