#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "render_target_layout_tracker.hpp"
#include "rendertarget.hpp"

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
                 RenderTargetLayoutTracker &layout_tracker) const;
};

} // namespace Pelican
