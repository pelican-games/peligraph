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

vk::ImageView
RenderTargetImageViewResolver::
    getImageSubresourceViewForFrame(
        GlobalRenderTargetId id,
        ImageSubresourceRange subresource,
        bool array_view, bool history_read,
        std::uint32_t frame_index) const {
    return rt_container
        .getImageSubresourceViewForFrame(
            id, subresource, array_view,
            history_read, frame_index);
}

vk::ImageView
RenderTargetImageViewResolver::getImageLayerViewForFrame(
    GlobalRenderTargetId id, std::uint32_t array_layer,
    bool history_read, std::uint32_t frame_index) const {
    return rt_container.getImageLayerViewForFrame(
        id, array_layer, history_read, frame_index);
}

vk::ImageView
RenderTargetImageViewResolver::getLayeredImageViewForFrame(
    GlobalRenderTargetId id, bool history_read,
    std::uint32_t frame_index) const {
    return rt_container.getLayeredImageViewForFrame(
        id, history_read, frame_index);
}

std::uint32_t RenderTargetImageViewResolver::arrayLayers(
    GlobalRenderTargetId id) const {
    return rt_container.getMetadata(id).array_layers;
}

std::uint32_t RenderTargetImageViewResolver::mipLevels(
    GlobalRenderTargetId id) const {
    return rt_container.getMetadata(id).mip_levels;
}

} // namespace Pelican
