#include "rendertargetmetadataresolver.hpp"
#include "rendertargetcontainer.hpp"

namespace Pelican {

RenderTargetMetadataResolver::RenderTargetMetadataResolver(const RenderTargetContainer &rt_container)
    : rt_container{rt_container} {}

RenderTargetMetadata RenderTargetMetadataResolver::get(GlobalRenderTargetId id) const {
    return rt_container.getMetadata(id);
}

} // namespace Pelican
