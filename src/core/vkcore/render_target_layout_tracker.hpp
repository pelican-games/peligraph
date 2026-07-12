#pragma once

#include "../renderingpass/renderingpass.hpp"
#include "util.hpp"
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetContainer;

class RenderTargetLayoutTracker {
  public:
    void transition(vk::CommandBuffer cmd_buf, RenderTargetContainer &rt_container, VulkanUtils &vk_utils,
                    GlobalRenderTargetId rt_id, vk::ImageLayout new_layout,
                    bool history_read = false);
    vk::ImageLayout currentLayout(GlobalRenderTargetId rt_id, bool history_read = false,
                                  const RenderTargetContainer *rt_container = nullptr) const;
    void reset();

  private:
    std::unordered_map<std::uint64_t, vk::ImageLayout> layouts;
};

} // namespace Pelican
