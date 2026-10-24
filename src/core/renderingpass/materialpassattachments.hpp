#pragma once

// RC-3c2a: the material pass attachment formats are project's table
// (materialattachmentformats.hpp); these are its Vulkan spellings.

#include "vulkanimagevocabulary.hpp"
#include "../../project/materialattachmentformats.hpp"

#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

inline std::vector<vk::Format> materialPassColorAttachmentFormats(bool hdr) {
    const auto &formats = hdr ? materialPassColorImageFormatsHdr : materialPassColorImageFormatsSdr;
    std::vector<vk::Format> result;
    for (const auto format : formats) result.push_back(toVulkanFormat(format));
    return result;
}

inline vk::Format forwardMaterialPassColorAttachmentFormat() {
    return toVulkanFormat(forwardMaterialPassColorImageFormat);
}

inline vk::Format materialPassDepthAttachmentFormat() {
    return toVulkanFormat(materialPassDepthImageFormat);
}

inline bool materialPassUsesHdrColorFormats(vk::Format first_color) {
    const auto format = imageFormatFromVulkan(first_color);
    return format && materialPassUsesHdrColorImageFormats(*format);
}

inline std::vector<vk::Format> materialPassColorAttachmentFormatsFor(MaterialShaderContract shader_contract,
                                                                     bool hdr) {
    std::vector<vk::Format> result;
    for (const auto format : materialPassColorImageFormatsFor(shader_contract, hdr))
        result.push_back(toVulkanFormat(format));
    return result;
}

} // namespace Pelican
