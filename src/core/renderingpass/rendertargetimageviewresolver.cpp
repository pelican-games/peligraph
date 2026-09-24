#include "rendertargetimageviewresolver.hpp"
#include "rendertargetcontainer.hpp"

namespace Pelican {

RenderTargetImageViewResolver::RenderTargetImageViewResolver(const RenderTargetContainer &rt_container)
    : rt_container{rt_container} {}

vk::ImageView RenderTargetImageViewResolver::getImageView(GlobalRenderTargetId id, RenderTargetRead read) const {
    return rt_container.getImageView(id, read);
}

vk::ImageView
RenderTargetImageViewResolver::
    getImageSubresourceView(
        GlobalRenderTargetId id,
        ImageSubresourceRange subresource,
        ImageSubresourceViewDimension dimension,
        RenderTargetRead read) const {
    return rt_container
        .getImageSubresourceView(
            id, subresource, dimension, read);
}

vk::ImageView
RenderTargetImageViewResolver::getImageLayerView(
    GlobalRenderTargetId id, std::uint32_t array_layer,
    RenderTargetRead read) const {
    return rt_container.getImageLayerView(
        id, array_layer, read);
}

vk::ImageView
RenderTargetImageViewResolver::getLayeredImageView(
    GlobalRenderTargetId id, RenderTargetRead read) const {
    return rt_container.getLayeredImageView(
        id, read);
}

std::uint32_t RenderTargetImageViewResolver::arrayLayers(
    GlobalRenderTargetId id) const {
    return rt_container.getMetadata(id).array_layers;
}

std::uint32_t RenderTargetImageViewResolver::mipLevels(
    GlobalRenderTargetId id) const {
    return rt_container.getMetadata(id).mip_levels;
}

ImageResourceDimension
RenderTargetImageViewResolver::dimension(
    GlobalRenderTargetId id) const {
    return rt_container.getMetadata(id).dimension;
}

std::string_view RenderTargetImageViewResolver::name(
    GlobalRenderTargetId id) const {
    return rt_container.getRenderTargetName(id);
}

} // namespace Pelican
