#include "rendertargetimageviewresolver.hpp"
#include "rendertargetcontainer.hpp"

namespace Pelican {

RenderTargetImageViewResolver::RenderTargetImageViewResolver(const RenderTargetContainer &rt_container)
    : rt_container{rt_container} {}

vk::ImageView RenderTargetImageViewResolver::getImageView(GlobalRenderTargetId id) const {
    return rt_container.getImageView(id);
}

} // namespace Pelican
