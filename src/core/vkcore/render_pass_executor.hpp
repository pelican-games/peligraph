#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "render_target_layout_tracker.hpp"
#include "render_pass_frame_setup.hpp"
#include "rendertarget.hpp"
#include <span>

namespace Pelican {

class RenderTargetContainer;
struct RenderPassDispatchDependencies;
class VulkanUtils;

struct RenderPassExecutorDependencies {
    RenderTargetContainer &render_target_container;
    VulkanUtils &vk_utils;
    const RenderPassDispatchDependencies &dispatch;
};

DECLARE_MODULE(RenderPassExecutor) {
  public:
    void execute(const FrameRenderContext &frame, const CompiledPass &pass,
                 const RenderPassExecutorDependencies &dependencies,
                 RenderTargetLayoutTracker &layout_tracker,
                 RenderPassViewInvocation invocation = {}) const;
    void beginRenderingScope(
        const FrameRenderContext &frame,
        std::span<const CompiledPass *const> passes,
        const RenderPassExecutorDependencies &dependencies,
        RenderTargetLayoutTracker &layout_tracker,
        RenderPassViewInvocation invocation = {}) const;
    void executeRenderingScopePass(
        const FrameRenderContext &frame,
        const CompiledPass &pass,
        const RenderPassExecutorDependencies &dependencies,
        RenderPassViewInvocation invocation = {}) const;
    void renderingScopeDependency(
        vk::CommandBuffer cmd_buf) const;
    void endRenderingScope(
        vk::CommandBuffer cmd_buf) const;
};

} // namespace Pelican
