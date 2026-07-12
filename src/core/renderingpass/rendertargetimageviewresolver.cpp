#include "rendertargetimageviewresolver.hpp"
#include "rendertargetcontainer.hpp"

namespace Pelican {

RenderTargetImageViewResolver::RenderTargetImageViewResolver(const RenderTargetContainer &rt_container)
    : rt_container{rt_container} {}

vk::ImageView RenderTargetImageViewResolver::getImageView(GlobalRenderTargetId id, bool history_read) const {
    return rt_container.getImageView(id, history_read);
}

vk::ImageView RenderTargetImageViewResolver::getImageViewForFrame(
    GlobalRenderTargetId id, bool history_read, uint32_t frame_index) const {
    return rt_container.getImageViewForFrame(id, history_read, frame_index);
}

} // namespace Pelican
