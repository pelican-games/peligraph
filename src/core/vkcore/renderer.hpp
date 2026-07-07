#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "render_target_layout_tracker.hpp"
#include <nlohmann/json.hpp>

namespace Pelican {

DECLARE_MODULE(Renderer) {
    RenderingPassId current_rendering_pass_id;
    RenderTargetLayoutTracker render_target_layout_tracker;

  public:
    Renderer();
    ~Renderer();
    nlohmann::json currentFramePlanJson() const;
    void render();
};

} // namespace Pelican
