#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "render_target_layout_tracker.hpp"
#include <nlohmann/json.hpp>

namespace Pelican {

enum class RendererExecutionPathForTesting {
    current,
    planned,
};

DECLARE_MODULE(Renderer) {
    RenderingPassId current_rendering_pass_id;
    RenderTargetLayoutTracker render_target_layout_tracker;
    RendererExecutionPathForTesting execution_path_for_testing = RendererExecutionPathForTesting::current;
    nlohmann::json last_execution_trace;

  public:
    Renderer();
    ~Renderer();
    nlohmann::json currentFramePlanJson() const;
    void setExecutionPathForTesting(RendererExecutionPathForTesting path) { execution_path_for_testing = path; }
    const nlohmann::json &lastExecutionTraceForTesting() const { return last_execution_trace; }
    void render();
};

} // namespace Pelican
