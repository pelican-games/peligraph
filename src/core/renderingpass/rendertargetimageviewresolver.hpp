#pragma once

#include "renderingpass.hpp"
#include "rendertargetread.hpp"
#include <string_view>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetContainer;

class RenderTargetImageViewResolver {
    const RenderTargetContainer &rt_container;

  public:
    explicit RenderTargetImageViewResolver(const RenderTargetContainer &rt_container);
    vk::ImageView getImageView(GlobalRenderTargetId id, RenderTargetRead read = {}) const;
    vk::ImageView getImageSubresourceView(
        GlobalRenderTargetId id,
        ImageSubresourceRange subresource,
        ImageSubresourceViewDimension dimension,
        RenderTargetRead read = {}) const;
    vk::ImageView getImageLayerView(
        GlobalRenderTargetId id, std::uint32_t array_layer,
        RenderTargetRead read = {}) const;
    vk::ImageView getLayeredImageView(
        GlobalRenderTargetId id, RenderTargetRead read = {}) const;
    std::uint32_t arrayLayers(
        GlobalRenderTargetId id) const;
    std::uint32_t mipLevels(
        GlobalRenderTargetId id) const;
    ImageResourceDimension dimension(
        GlobalRenderTargetId id) const;
    // The registration name of the target, so a validation failure can say
    // which resource it is about instead of only where it sat in a list.
    std::string_view name(
        GlobalRenderTargetId id) const;
};

} // namespace Pelican
