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
};

} // namespace Pelican
