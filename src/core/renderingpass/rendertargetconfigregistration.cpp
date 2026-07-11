#include "rendertargetconfigregistration.hpp"
#include "rendertargetcontainer.hpp"
#include "../vkcore/core.hpp"
#include <stdexcept>

namespace Pelican {

void registerRenderTargetDefinitions(const std::vector<RenderTargetDefinition> &definitions,
                                     vk::Extent2D base_extent,
                                     RenderTargetContainer &rt_container) {
    for (const auto &definition : definitions) {
        if (definition.format_class == "display") {
            const auto features = GET_MODULE(VulkanManageCore)
                                      .getPhysDevice()
                                      .getFormatProperties(definition.format)
                                      .optimalTilingFeatures;
            const auto required = vk::FormatFeatureFlagBits::eColorAttachment |
                                  vk::FormatFeatureFlagBits::eTransferSrc;
            if ((features & required) != required) {
                throw std::runtime_error(
                    "resolver v1 display format lacks COLOR_ATTACHMENT or TRANSFER_SRC support");
            }
        }
        rt_container.registerRenderTarget(definition.name, base_extent, definition.format_class,
                                          definition.extent_scale,
                                          definition.fixed_extent, definition.format, definition.usage,
                                          vma::MemoryUsage::eAutoPreferDevice);
    }
}

} // namespace Pelican
