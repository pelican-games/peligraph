#include "rendertargetconfigregistration.hpp"
#include "rendertargetcontainer.hpp"
#include "rendertargetimagecontract.hpp"
#include "../vkcore/core.hpp"
#include <map>
#include <set>
#include <stdexcept>

namespace Pelican {

void registerRenderTargetDefinitions(const std::vector<RenderTargetDefinition> &planned_definitions,
                                     vk::Extent2D base_extent,
                                     RenderTargetContainer &rt_container) {
    const auto &definitions=planned_definitions;
    for (const auto &definition : definitions) {
        if (definition.format_class == "display") {
            const auto features = GET_MODULE(VulkanManageCore)
                                      .getPhysDevice()
                                      .getFormatProperties(toVulkanFormat(definition.format))
                                      .optimalTilingFeatures;
            const auto required = vk::FormatFeatureFlagBits::eColorAttachment |
                                  vk::FormatFeatureFlagBits::eTransferSrc;
            if ((features & required) != required) {
                throw std::runtime_error(
                    "resolver v1 display format lacks COLOR_ATTACHMENT or TRANSFER_SRC support");
            }
        }

    }
    rt_container.registerMemoryTargets(definitions,base_extent);
}

} // namespace Pelican
