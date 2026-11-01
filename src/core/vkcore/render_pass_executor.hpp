#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "render_target_layout_tracker.hpp"
#include "render_pass_frame_setup.hpp"
#include "rendertarget.hpp"
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Pelican {

namespace test_support { struct RenderPassExecutorTestAccess; }

class RenderTargetContainer;
struct RenderPassDispatchDependencies;
class VulkanUtils;

struct RenderPassExecutorDependencies {
    RenderTargetContainer &render_target_container;
    VulkanUtils &vk_utils;
    const RenderPassDispatchDependencies &dispatch;
};

DECLARE_MODULE(RenderPassExecutor) {
    friend struct test_support::RenderPassExecutorTestAccess;
    bool trace_bodies_for_testing = false;
    mutable std::vector<std::pair<std::string, std::uint32_t>> body_calls_for_testing;
    void recordBodyForTesting(const CompiledPass &pass, RenderPassViewInvocation invocation) const {
        if (trace_bodies_for_testing)
            body_calls_for_testing.emplace_back(pass.definition.name, invocation.view_index);
    }
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
        vk::CommandBuffer cmd_buf, RenderTargetLayoutTracker *layout_tracker = nullptr) const;
};

} // namespace Pelican
