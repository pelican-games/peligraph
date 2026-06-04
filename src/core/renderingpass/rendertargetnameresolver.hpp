#pragma once

#include "renderingpass.hpp"
#include <string>

namespace Pelican {

class RenderTargetContainer;

class RenderTargetNameResolver {
    const RenderTargetContainer &rt_container;

  public:
    explicit RenderTargetNameResolver(const RenderTargetContainer &rt_container);
    GlobalRenderTargetId resolve(const std::string &name) const;
};

} // namespace Pelican
