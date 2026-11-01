#include "rendertargetconfigregistration.hpp"
#include "rendertargetcontainer.hpp"
#include "rendertargetimagecontract.hpp"
#include "../vkcore/core.hpp"
#include <map>
#include <set>
#include <stdexcept>

namespace Pelican {
namespace {

void requireDisplayFormatSupport(const RenderTargetDefinition &definition) {
    if (definition.format_class != "display") return;
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

} // namespace

void registerRenderTargetDefinitions(const std::vector<RenderTargetDefinition> &planned_definitions,
                                     vk::Extent2D base_extent,
                                     RenderTargetContainer &rt_container) {
    const auto &definitions=planned_definitions;
    for (const auto &definition : definitions) requireDisplayFormatSupport(definition);
    rt_container.registerMemoryTargets(definitions,base_extent);
}

void validateUnregisteredRenderTargetDefinitions(const std::vector<RenderTargetDefinition> &definitions,
                                                 vk::Extent2D base_extent) {
    for (const auto &definition : definitions) {
        requireDisplayFormatSupport(definition);
        validateRenderTargetDefinition(
            definition, base_extent,
            definition.storage_mode == RenderTargetStorageMode::tile_local_attachment &&
                GET_MODULE(VulkanManageCore).getRuntimeCapabilities().dynamic_rendering_local_read);
        requireRenderTargetFormatFeatures(
            definition.name,
            renderTargetImageContract(definition, base_extent,
                                      GET_MODULE(VulkanManageCore).getGraphicsQueueFamilyIndex())
                .createInfo());
    }
}

} // namespace Pelican
