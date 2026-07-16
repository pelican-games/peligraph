#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../renderer/projectionjitter.hpp"
#include "render_target_layout_tracker.hpp"
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {

DECLARE_MODULE(Renderer) {
    RenderingPassId current_rendering_pass_id;
    RenderTargetLayoutTracker render_target_layout_tracker;
    bool execution_tracing_for_testing = false;
    nlohmann::json last_execution_trace;
    TemporalFrameHistory temporal_history;
    bool temporal_reset_requested = true;
    std::uint64_t observed_time_set_revision = 0;
    std::uint64_t observed_camera_discontinuity_revision = 0;
    std::optional<ProjectionJitterSettings> projection_jitter;

  public:
    Renderer();
    ~Renderer();
    nlohmann::json currentFramePlanJson() const;
    std::vector<std::string> currentFramePlanOrderForTesting() const;
    void setExecutionTracingForTesting(bool enabled) { execution_tracing_for_testing = enabled; }
    const nlohmann::json &lastExecutionTraceForTesting() const { return last_execution_trace; }
    void recreateRenderTargetsAndRebindForTesting(vk::Extent2D extent);
    void resetTemporalHistory();
    // Resolves every renderer-owned runtime dependency while module creation is
    // still legal. Subsequent render calls only read the frozen module graph.
    void prepareRuntimeModules();
    void render();
};

} // namespace Pelican
