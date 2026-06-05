#include "rendertargetmetadataresolver.hpp"
#include "rendertargetcontainer.hpp"
#include <stdexcept>
#include <utility>

namespace Pelican {

RenderTargetMetadataResolver::RenderTargetMetadataResolver(const RenderTargetContainer &rt_container)
    : get_metadata{[&rt_container](GlobalRenderTargetId id) { return rt_container.getMetadata(id); }} {}

RenderTargetMetadataResolver::RenderTargetMetadataResolver(
    std::function<RenderTargetMetadata(GlobalRenderTargetId)> get_metadata)
    : get_metadata{std::move(get_metadata)} {}

RenderTargetMetadata RenderTargetMetadataResolver::get(GlobalRenderTargetId id) const {
    if (!get_metadata) {
        throw std::runtime_error("Render target metadata resolver is not configured");
    }
    return get_metadata(id);
}

} // namespace Pelican
