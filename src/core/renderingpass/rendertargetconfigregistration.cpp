#include "rendertargetconfigregistration.hpp"
#include "rendertargetcontainer.hpp"

namespace Pelican {

void registerRenderTargetDefinitions(const std::vector<RenderTargetDefinition> &definitions,
                                     RenderTargetContainer &rt_container) {
    for (const auto &definition : definitions) {
        rt_container.registerRenderTarget(definition.name, definition.extent, definition.format, definition.usage,
                                          vma::MemoryUsage::eAutoPreferDevice);
    }
}

} // namespace Pelican
