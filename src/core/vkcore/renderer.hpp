#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../renderer/projectionjitter.hpp"
#include "render_target_layout_tracker.hpp"
#include "rendertarget.hpp"
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {

struct RenderViewParameters {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 camera_position{0.0f};
};

class ILogicalFrameTarget {
  public:
    virtual ~ILogicalFrameTarget() = default;
    virtual void beginLogicalFrame(std::uint32_t view_count) = 0;
    virtual FrameRenderContext beginView(std::uint32_t view_index) = 0;
    virtual void endView(std::uint32_t view_index) = 0;
    virtual void endLogicalFrame() = 0;
    virtual vk::Format colorFormat(std::uint32_t view_index) const = 0;
    virtual bool consumeExtentChanged() = 0;
};

using RenderViewProvider =
    std::function<RenderViewParameters(std::uint32_t, const FrameRenderContext &)>;

DECLARE_MODULE(Renderer) {
    RenderingPassId current_rendering_pass_id;
    RenderTargetLayoutTracker render_target_layout_tracker;
    bool execution_tracing_for_testing = false;
    nlohmann::json last_execution_trace;
    std::vector<TemporalFrameHistory> temporal_histories;
    std::vector<RenderFrameSnapshot> last_view_snapshots;
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
    const std::vector<RenderFrameSnapshot> &lastViewSnapshotsForTesting() const {
        return last_view_snapshots;
    }
    void recreateRenderTargetsAndRebindForTesting(vk::Extent2D extent);
    void resetTemporalHistory();
    // Resolves every renderer-owned runtime dependency while module creation is
    // still legal. Subsequent render calls only read the frozen module graph.
    void prepareRuntimeModules();
    void renderLogicalFrame(ILogicalFrameTarget &target, std::uint32_t view_count,
                            const RenderViewProvider &view_provider);
    void render();
};

} // namespace Pelican
