#include "rendertargetconfigregistration.hpp"
#include "rendertargetcontainer.hpp"
#include <cstdint>
#include <stdexcept>

namespace Pelican {

namespace {

vk::Extent2D resolveRenderTargetExtent(const RenderTargetDefinition &definition, vk::Extent2D base_extent) {
    const vk::Extent2D extent{
        static_cast<uint32_t>(base_extent.width * definition.extent_scale),
        static_cast<uint32_t>(base_extent.height * definition.extent_scale),
    };

    if (extent.width == 0 || extent.height == 0) {
        throw std::runtime_error("Render target extent became zero-sized: " + definition.name);
    }
    return extent;
}

} // namespace

void registerRenderTargetDefinitions(const std::vector<RenderTargetDefinition> &definitions,
                                     vk::Extent2D base_extent,
                                     RenderTargetContainer &rt_container) {
    for (const auto &definition : definitions) {
        rt_container.registerRenderTarget(definition.name, resolveRenderTargetExtent(definition, base_extent),
                                          definition.format, definition.usage, vma::MemoryUsage::eAutoPreferDevice);
    }
}

} // namespace Pelican
