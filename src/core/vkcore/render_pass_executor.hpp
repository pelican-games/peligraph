#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "render_target_layout_tracker.hpp"
#include "rendertarget.hpp"

namespace Pelican {

DECLARE_MODULE(RenderPassExecutor) {
  public:
    void execute(const FrameRenderContext &frame, const PassDefinition &pass_def, PassId pass_id,
                 RenderTargetLayoutTracker &layout_tracker) const;
};

} // namespace Pelican
