#include "rendertargetconfigregistration.hpp"
#include "rendertargetcontainer.hpp"

namespace Pelican {

void registerRenderTargetDefinitions(const std::vector<RenderTargetDefinition> &definitions,
                                     vk::Extent2D base_extent,
                                     RenderTargetContainer &rt_container) {
    for (const auto &definition : definitions) {
        rt_container.registerRenderTarget(definition.name, base_extent, definition.extent_scale,
                                          definition.fixed_extent, definition.format, definition.usage,
                                          vma::MemoryUsage::eAutoPreferDevice);
    }
}

} // namespace Pelican
