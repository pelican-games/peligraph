#pragma once

#include <cstdint>
#include <vulkan/vulkan.hpp>

namespace Pelican {

// Vulkan-Hpp dispatch table backed by the five extension entry points loaded
// by VulkanManageCore. Keeping this object in VulkanManageCore gives every
// vk::UniqueHandle deleter a stable dispatch lifetime.
struct AccelerationStructureDispatch {
    PFN_vkCreateAccelerationStructureKHR
        vkCreateAccelerationStructureKHR = nullptr;
    PFN_vkDestroyAccelerationStructureKHR
        vkDestroyAccelerationStructureKHR = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR
        vkGetAccelerationStructureBuildSizesKHR = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR
        vkGetAccelerationStructureDeviceAddressKHR = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR
        vkCmdBuildAccelerationStructuresKHR = nullptr;

    std::uint32_t getVkHeaderVersion() const noexcept {
        return VK_HEADER_VERSION;
    }
};

using UniqueAccelerationStructure =
    vk::UniqueHandle<vk::AccelerationStructureKHR,
                     AccelerationStructureDispatch>;

} // namespace Pelican
