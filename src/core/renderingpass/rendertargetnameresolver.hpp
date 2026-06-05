#pragma once

#include "renderingpass.hpp"
#include <functional>
#include <string>

namespace Pelican {

class RenderTargetContainer;

class RenderTargetNameResolver {
    std::function<GlobalRenderTargetId(const std::string &)> resolve_target;

  public:
    explicit RenderTargetNameResolver(const RenderTargetContainer &rt_container);
    explicit RenderTargetNameResolver(std::function<GlobalRenderTargetId(const std::string &)> resolve_target);
    GlobalRenderTargetId resolve(const std::string &name) const;
};

} // namespace Pelican
