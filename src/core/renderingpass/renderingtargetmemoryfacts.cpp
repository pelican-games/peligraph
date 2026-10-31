#include "renderingsamplecount.hpp"

namespace Pelican {
RenderingTargetPlanDeviceFacts renderingTargetMemoryDeviceFacts(
    vk::Device device, vk::PhysicalDevice physical_device) {
    RenderingTargetPlanDeviceFacts facts;
    const auto memory = physical_device.getMemoryProperties();
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        facts.memory_types.push_back({memory.memoryTypes[i].propertyFlags,
                                      memory.memoryTypes[i].heapIndex});
    if (device) facts.image_memory_requirements = [device](const vk::ImageCreateInfo &create_info)
        -> std::optional<ImageMemoryRequirements> {
        vk::DeviceImageMemoryRequirements query;
        query.pCreateInfo = &create_info;
        vk::MemoryDedicatedRequirements dedicated;
        vk::MemoryRequirements2 requirements;
        requirements.pNext = &dedicated;
        device.getImageMemoryRequirements(&query, &requirements);
        const auto &r = requirements.memoryRequirements;
        return ImageMemoryRequirements{r.size, r.alignment, r.memoryTypeBits,
            dedicated.requiresDedicatedAllocation == VK_TRUE,
            dedicated.prefersDedicatedAllocation == VK_TRUE};
    };
    return facts;
}
} // namespace Pelican
