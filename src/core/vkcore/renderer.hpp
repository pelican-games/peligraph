#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../renderer/projectionjitter.hpp"
#include "render_target_layout_tracker.hpp"
#include "rendertarget.hpp"
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {

enum class RenderGraphVariant {
    flat,
    xr,
};

struct RenderViewParameters {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 camera_position{0.0f};
    // XR eye views set this true. Flat and mirror/observer views retain the
    // default so VRM head geometry remains visible.
    bool first_person_view = false;
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
    RenderingPassId flat_rendering_pass_id = invalidRenderingPassId();
    std::optional<RenderingPassId> xr_rendering_pass_id;
    RenderGraphVariant active_graph_variant = RenderGraphVariant::flat;
    std::vector<TemporalFrameHistory> flat_temporal_histories;
    std::vector<TemporalFrameHistory> xr_temporal_histories;
    std::vector<RenderFrameSnapshot> last_view_snapshots;
    bool temporal_reset_requested = true;
    std::uint64_t observed_time_set_revision = 0;
    std::uint64_t observed_camera_discontinuity_revision = 0;
    std::optional<ProjectionJitterSettings> projection_jitter;
    std::optional<vk::Extent2D> internal_render_extent;
    std::vector<std::string> xr_excluded_features;
    nlohmann::json graph_variant_transition_trace = nlohmann::json::array();
    std::optional<std::size_t> pending_graph_transition;

    std::vector<TemporalFrameHistory> &activeTemporalHistories();
    const std::vector<TemporalFrameHistory> &activeTemporalHistories() const;

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
    void selectGraphVariant(RenderGraphVariant variant);
    RenderGraphVariant graphVariant() const noexcept { return active_graph_variant; }
    bool hasXrGraphVariant() const noexcept { return xr_rendering_pass_id.has_value(); }
    const std::vector<std::string> &xrExcludedFeatures() const noexcept {
        return xr_excluded_features;
    }
    const nlohmann::json &graphVariantTransitionTraceForTesting() const noexcept {
        return graph_variant_transition_trace;
    }
    // Resolves every renderer-owned runtime dependency while module creation is
    // still legal. Subsequent render calls only read the frozen module graph.
    void prepareRuntimeModules();
    void renderLogicalFrame(ILogicalFrameTarget &target, std::uint32_t view_count,
                            const RenderViewProvider &view_provider);
    void render();
};

} // namespace Pelican
