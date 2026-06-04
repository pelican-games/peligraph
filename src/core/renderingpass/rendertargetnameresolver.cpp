#include "rendertargetnameresolver.hpp"
#include "rendertargetcontainer.hpp"

namespace Pelican {

RenderTargetNameResolver::RenderTargetNameResolver(const RenderTargetContainer &rt_container)
    : rt_container{rt_container} {}

GlobalRenderTargetId RenderTargetNameResolver::resolve(const std::string &name) const {
    return rt_container.getRenderTargetIdByName(name);
}

} // namespace Pelican
