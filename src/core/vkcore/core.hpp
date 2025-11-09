#pragma once

#include "../container.hpp"
#include "cmdbuf.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct QueueSet {
    uint32_t graphic_queue;
    uint32_t presentation_queue;
    uint32_t compute_queue;
};

class VulkanManageCore {
    vk::UniqueInstance instance;
    vk::UniqueSurfaceKHR surface;
    vk::PhysicalDevice phys_device;
    QueueSet queue_set;
    vk::UniqueDevice device;

    vk::Queue graphic_queue, presen_queue, compute_queue;

    vk::UniqueCommandPool graphic_cmd_pool, compute_cmd_pool;

  public:
    VulkanManageCore(DependencyContainer &container);
    ~VulkanManageCore();

    vk::Device getDevice() const { return device.get(); };
    vk::PhysicalDevice getPhysDevice() const { return phys_device; };
    vk::SurfaceKHR getSurface() const { return surface.get(); };
    vk::Queue getPresentationQueue() const { return presen_queue; }

    std::vector<CommandBufWrapper> allocCmdBufs(size_t num) const;
    std::vector<vk::UniqueSemaphore> createSemaphores(size_t num) const;
};

} // namespace Pelican
