#pragma once

#include "renderingpass.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetContainer;

class RenderTargetImageViewResolver {
    const RenderTargetContainer &rt_container;

  public:
    explicit RenderTargetImageViewResolver(const RenderTargetContainer &rt_container);
    vk::ImageView getImageView(GlobalRenderTargetId id, bool history_read = false) const;
    vk::ImageView getImageViewForFrame(GlobalRenderTargetId id, bool history_read,
                                       uint32_t frame_index) const;
    vk::ImageView getImageSubresourceViewForFrame(
        GlobalRenderTargetId id,
        ImageSubresourceRange subresource,
        bool array_view, bool history_read,
        std::uint32_t frame_index) const;
    vk::ImageView getImageLayerViewForFrame(
        GlobalRenderTargetId id, std::uint32_t array_layer,
        bool history_read, std::uint32_t frame_index) const;
    vk::ImageView getLayeredImageViewForFrame(
        GlobalRenderTargetId id, bool history_read,
        std::uint32_t frame_index) const;
    std::uint32_t arrayLayers(
        GlobalRenderTargetId id) const;
};

} // namespace Pelican
