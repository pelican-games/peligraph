#pragma once

#include "renderingpass.hpp"
#include "rendertargetmetadata.hpp"
#include <functional>

namespace Pelican {

class RenderTargetContainer;

class RenderTargetMetadataResolver {
    std::function<RenderTargetMetadata(GlobalRenderTargetId)> get_metadata;

  public:
    explicit RenderTargetMetadataResolver(const RenderTargetContainer &rt_container);
    explicit RenderTargetMetadataResolver(std::function<RenderTargetMetadata(GlobalRenderTargetId)> get_metadata);
    RenderTargetMetadata get(GlobalRenderTargetId id) const;
};

} // namespace Pelican
