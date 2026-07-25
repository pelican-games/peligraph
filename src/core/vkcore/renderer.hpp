#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../renderingpass/previewgraph.hpp"
#include "../renderer/projectionjitter.hpp"
#include "render_target_layout_tracker.hpp"
#include "rendertarget.hpp"
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican {

struct RenderPipelineReloadState;

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
    virtual bool supportsViewFamilyExecution() const noexcept {
        return false;
    }
    virtual FrameRenderContext beginViewFamily(
        std::uint32_t) {
        throw std::runtime_error(
            "logical-frame target does not support view-family execution");
    }
    // Targets that submit per view must capture the lease before this call
    // returns after a successful submit.
    virtual void endView(
        std::uint32_t view_index,
        GpuSubmissionLease lease = {}) = 0;
    virtual void endViewFamily(
        GpuSubmissionLease = {}) {
        throw std::runtime_error(
            "logical-frame target does not support view-family execution");
    }
    // Implementations must capture the lease before returning (or throwing)
    // after any successful GPU submit, and release it only after completion.
    virtual void endLogicalFrame(GpuSubmissionLease lease = {}) = 0;
    virtual vk::Format colorFormat(std::uint32_t view_index) const = 0;
    virtual bool consumeExtentChanged() = 0;
};

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
    std::optional<vk::Extent2D> internal_render_extent;
    std::vector<std::string> xr_excluded_features;
    nlohmann::json graph_variant_transition_trace = nlohmann::json::array();
    std::optional<std::size_t> pending_graph_transition;
    PreviewGraphProgram preview_graph_program;
    std::unique_ptr<RenderPipelineReloadState>
        render_pipeline_reload_state;

    std::vector<TemporalFrameHistory> &activeTemporalHistories();
    const std::vector<TemporalFrameHistory> &activeTemporalHistories() const;
    void installRenderPipelineReloadParticipant();
    bool reloadRenderPipelineFromDisk(
        std::string &error) noexcept;

  public:
    Renderer();
    ~Renderer();
    nlohmann::json currentFramePlanJson() const;
    std::vector<std::string> currentFramePlanOrderForTesting() const;
    void setExecutionTracingForTesting(bool enabled) { execution_tracing_for_testing = enabled; }
    const nlohmann::json &lastExecutionTraceForTesting() const { return last_execution_trace; }
    std::size_t imageMemoryDependencyCountForTesting() const noexcept {
        return render_target_layout_tracker.memoryDependencyCountForTesting();
    }
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
    const PreviewGraphProgram &previewGraphProgram() const noexcept {
        return preview_graph_program;
    }
    nlohmann::ordered_json previewIsolationStateJson() const;
    const nlohmann::json &graphVariantTransitionTraceForTesting() const noexcept {
        return graph_variant_transition_trace;
    }
    // Resolves every renderer-owned runtime dependency while module creation is
    // still legal. Subsequent render calls only read the frozen module graph.
    void prepareRuntimeModules();
    void renderLogicalFrame(
        ILogicalFrameTarget &target,
        std::span<const RenderViewParameters> views);
    void render();
};

} // namespace Pelican
