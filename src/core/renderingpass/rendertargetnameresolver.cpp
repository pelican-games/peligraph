#include "rendertargetnameresolver.hpp"
#include "rendertargetcontainer.hpp"
#include <stdexcept>
#include <utility>

namespace Pelican {

RenderTargetNameResolver::RenderTargetNameResolver(const RenderTargetContainer &rt_container)
    : resolve_target{[&rt_container](const std::string &name) { return rt_container.getRenderTargetIdByName(name); }} {}

RenderTargetNameResolver::RenderTargetNameResolver(
    std::function<GlobalRenderTargetId(const std::string &)> resolve_target)
    : resolve_target{std::move(resolve_target)} {}

GlobalRenderTargetId RenderTargetNameResolver::resolve(const std::string &name) const {
    if (!resolve_target) {
        throw std::runtime_error("Render target name resolver is not configured");
    }
    return resolve_target(name);
}

} // namespace Pelican
