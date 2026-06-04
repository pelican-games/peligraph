#pragma once

#include "renderingpass.hpp"
#include "rendertargetmetadata.hpp"

namespace Pelican {

class RenderTargetContainer;

class RenderTargetMetadataResolver {
    const RenderTargetContainer &rt_container;

  public:
    explicit RenderTargetMetadataResolver(const RenderTargetContainer &rt_container);
    RenderTargetMetadata get(GlobalRenderTargetId id) const;
};

} // namespace Pelican
