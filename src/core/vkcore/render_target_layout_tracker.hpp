#pragma once

#include "../renderingpass/renderingpass.hpp"
#include "util.hpp"
#include <cstddef>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetContainer;

enum class RenderTargetImageKind : std::uint8_t {
    resolved,
    attachment,
};

class RenderTargetLayoutTracker {
  public:
    struct AliasTransition {
        std::uint64_t token;
        GlobalRenderTargetId resource;
        bool dependency;
        vk::PipelineStageFlags src_stage, dst_stage;
        vk::AccessFlags src_access, dst_access;
        vk::ImageLayout old_layout, new_layout;
        bool inside_rendering = false;
    };
    // Observes the arguments of recorded alias dependencies and transitions.
    std::function<void(const AliasTransition &)> alias_transition_observer;
    void transition(vk::CommandBuffer cmd_buf, RenderTargetContainer &rt_container, VulkanUtils &vk_utils,
                    GlobalRenderTargetId rt_id, vk::ImageLayout new_layout,
                    bool history_read = false,
                    RenderTargetImageKind image_kind =
                        RenderTargetImageKind::resolved,
                    vk::PipelineStageFlags shader_stages =
                        vk::PipelineStageFlagBits::eVertexShader |
                        vk::PipelineStageFlagBits::eFragmentShader |
                        vk::PipelineStageFlagBits::eComputeShader);
    void memoryDependency(vk::CommandBuffer cmd_buf, RenderTargetContainer &rt_container,
                          VulkanUtils &vk_utils, GlobalRenderTargetId rt_id,
                          bool history_read = false,
                          vk::PipelineStageFlags shader_stages =
                              vk::PipelineStageFlagBits::eVertexShader |
                              vk::PipelineStageFlagBits::eFragmentShader |
                              vk::PipelineStageFlagBits::eComputeShader);
    vk::ImageLayout currentLayout(GlobalRenderTargetId rt_id, bool history_read = false,
                                  const RenderTargetContainer *rt_container = nullptr,
                                  RenderTargetImageKind image_kind =
                                      RenderTargetImageKind::resolved) const;
    // Records a transition performed by a manual NativeScope without
    // emitting another barrier. The NativeScope owns synchronization; the
    // tracker only resumes from its declared outer layout.
    void assumeLayout(
        RenderTargetContainer &rt_container,
        GlobalRenderTargetId rt_id,
        vk::ImageLayout layout,
        bool history_read = false,
        RenderTargetImageKind image_kind =
            RenderTargetImageKind::resolved);
    void setInsideRendering(bool value) noexcept {inside_rendering=value;}
    void reset();
    void beginFrame();
    std::size_t frameAliasDependencyCount() const noexcept {return frame_alias_dependency_count;}
    std::size_t memoryDependencyCountForTesting() const noexcept {
        return memory_dependency_count;
    }
    std::size_t aliasDependencyCountForTesting() const noexcept {
        return alias_dependency_count;
    }

  private:
    bool inside_rendering = false;
    // Every access, including a dependency that keeps the layout, acquires
    // its physical range before recording the image barrier.
    void recordAccess(
        vk::CommandBuffer cmd_buf, RenderTargetContainer &rt_container,
        VulkanUtils &vk_utils, GlobalRenderTargetId rt_id,
        vk::ImageLayout new_layout, bool history_read,
        RenderTargetImageKind image_kind,
        vk::PipelineStageFlags shader_stages, bool memory_dependency);
    // PS-1: a persistent target's entry names the image it was tracked on.
    // reset() keeps such entries; a lookup that finds a different image (the
    // target was made anew) starts over from the initial layout.
    void forgetRecreatedPersistentImage(
        const RenderTargetContainer &rt_container,
        GlobalRenderTargetId rt_id, std::uint64_t key,
        RenderTargetImageKind image_kind, bool history_read);
    void notePersistentImage(
        const RenderTargetContainer &rt_container,
        GlobalRenderTargetId rt_id, std::uint64_t key,
        RenderTargetImageKind image_kind, bool history_read);

    std::unordered_map<std::uint64_t, vk::ImageLayout> layouts;
    std::unordered_map<std::uint64_t, VkImage> persistent_images;
    std::unordered_map<std::uint64_t, vk::PipelineStageFlags>
        shader_access_stages;
    struct RangeAccess {
        std::uint64_t region=0, offset=0, size=0, access=0;
        VkImage image=VK_NULL_HANDLE;
    };
    std::unordered_map<std::uint64_t,RangeAccess> alias_ranges;
    std::unordered_set<std::uint64_t> frame_accessed;
    std::uint64_t access_serial=0;
    std::size_t memory_dependency_count = 0;
    std::size_t alias_dependency_count = 0;
    std::size_t frame_alias_dependency_count = 0;
};

} // namespace Pelican
