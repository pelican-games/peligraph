#pragma once

// VO-5a: the real capability source. Only with PELICAN_WITH_SPOUT, so Windows.
// Everything it asks Vulkan is in vulkan_core.h; no Win32 type appears here.
#include "outputspoutgpupath.hpp"

#include <vulkan/vulkan_core.h>

#include <memory>
#include <string>
#include <vector>

namespace Pelican {

// The Vulkan queries the source makes. A test replaces them to read the
// structure chains the source really sends.
struct OutputSpoutVulkanQueries {
    PFN_vkGetPhysicalDeviceImageFormatProperties2 image_format =
        vkGetPhysicalDeviceImageFormatProperties2;
    PFN_vkGetPhysicalDeviceFormatProperties format = vkGetPhysicalDeviceFormatProperties;
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties semaphore =
        vkGetPhysicalDeviceExternalSemaphoreProperties;
    PFN_vkGetPhysicalDeviceProperties2 properties = vkGetPhysicalDeviceProperties2;
};

std::unique_ptr<OutputSpoutGpuCapabilitySource> makeOutputSpoutGpuCapabilitySource(
    VkPhysicalDevice physical_device, std::vector<std::string> enabled, bool timeline,
    OutputSpoutVulkanQueries queries = {});

} // namespace Pelican
