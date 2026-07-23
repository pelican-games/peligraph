#pragma once

#include "../renderingpass/renderingpass.hpp"
#include "util.hpp"
#include <cstddef>
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetContainer;

enum class RenderTargetImageKind : std::uint8_t {
    resolved,
    attachment,
};

class RenderTargetLayoutTracker {
  public:
    void transition(vk::CommandBuffer cmd_buf, RenderTargetContainer &rt_container, VulkanUtils &vk_utils,
                    GlobalRenderTargetId rt_id, vk::ImageLayout new_layout,
                    bool history_read = false,
                    RenderTargetImageKind image_kind =
                        RenderTargetImageKind::resolved);
    void memoryDependency(vk::CommandBuffer cmd_buf, RenderTargetContainer &rt_container,
                          VulkanUtils &vk_utils, GlobalRenderTargetId rt_id,
                          bool history_read = false);
    vk::ImageLayout currentLayout(GlobalRenderTargetId rt_id, bool history_read = false,
                                  const RenderTargetContainer *rt_container = nullptr,
                                  RenderTargetImageKind image_kind =
                                      RenderTargetImageKind::resolved) const;
    void reset();
    std::size_t memoryDependencyCountForTesting() const noexcept {
        return memory_dependency_count;
    }

  private:
    std::unordered_map<std::uint64_t, vk::ImageLayout> layouts;
    std::size_t memory_dependency_count = 0;
};

} // namespace Pelican
