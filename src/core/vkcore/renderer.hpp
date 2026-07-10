#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "render_target_layout_tracker.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {

DECLARE_MODULE(Renderer) {
    RenderingPassId current_rendering_pass_id;
    RenderTargetLayoutTracker render_target_layout_tracker;
    bool execution_tracing_for_testing = false;
    nlohmann::json last_execution_trace;

  public:
    Renderer();
    ~Renderer();
    nlohmann::json currentFramePlanJson() const;
    std::vector<std::string> currentFramePlanOrderForTesting() const;
    void setExecutionTracingForTesting(bool enabled) { execution_tracing_for_testing = enabled; }
    const nlohmann::json &lastExecutionTraceForTesting() const { return last_execution_trace; }
    void recreateRenderTargetsAndRebindForTesting(vk::Extent2D extent);
    void render();
};

} // namespace Pelican
