#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "render_target_layout_tracker.hpp"

namespace Pelican {

DECLARE_MODULE(Renderer) {
    RenderingPassId current_rendering_pass_id;
    RenderTargetLayoutTracker render_target_layout_tracker;

  public:
    Renderer();
    ~Renderer();
    void render();
};

} // namespace Pelican
