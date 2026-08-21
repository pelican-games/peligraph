#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../renderingpass/previewgraph.hpp"
#include "../renderer/viewfamily.hpp"
#include "render_target_layout_tracker.hpp"
#include "rendertarget.hpp"
#include "../../project/renderconfigdocument.hpp"
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican {

struct RenderPipelineReloadState;
struct RenderPipelineAuthoringApplyResult {
    bool committed = false;
    std::uint64_t published_generation = 0;
    std::string error;
    std::string post_commit_error;
};
struct RayQueryAccelerationStructureDiagnostics;
namespace watch {
struct ReloadRequest;
}

enum class RenderGraphVariant {
    flat,
    xr,
};

struct PickingModelInstanceToken {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    std::uint64_t scene_epoch = 0;

    bool operator==(const PickingModelInstanceToken &) const = default;
};

struct PickingReadbackResult {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    vk::Extent2D extent{};
    std::uint64_t frame_index = 0;
    std::optional<PickingModelInstanceToken> model_instance;
};

struct R8RenderTargetReadback {
    vk::Extent2D extent{};
    std::vector<std::uint8_t> pixels;
};

struct LogicalFrameRuntime {
    std::shared_ptr<const RendererRuntimeGeneration>
        renderer_generation;
    GpuSubmissionLease submission_lease;
};

class ILogicalFrameTarget {
  public:
    virtual ~ILogicalFrameTarget() = default;
    // Called once before beginLogicalFrame. Undefined disables the optional
    // export. A true result promises that subsequent contexts expose a
    // compatible external depth image for this logical frame.
    virtual bool configureExternalDepthSubmission(
        vk::Format, vk::Extent2D) {
        return false;
    }
    virtual void beginLogicalFrame(
        std::uint32_t view_count,
        LogicalFrameRuntime runtime = {}) = 0;
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
    virtual void abortLogicalFrame() noexcept = 0;
    virtual vk::Format colorFormat(std::uint32_t view_index) const = 0;
    virtual std::optional<OutputCompileFacts>
    outputCompileFacts() const {
        return std::nullopt;
    }
};

DECLARE_MODULE(Renderer) {
    RenderingPassId current_rendering_pass_id;
    RenderTargetLayoutTracker render_target_layout_tracker;
    bool execution_tracing_for_testing = false;
    nlohmann::json last_execution_trace;
    RenderingPassId flat_rendering_pass_id = invalidRenderingPassId();
    std::optional<RenderingPassId> xr_rendering_pass_id;
    RenderGraphVariant active_graph_variant = RenderGraphVariant::flat;
    TemporalViewFamilyHistory flat_temporal_history;
    TemporalViewFamilyHistory xr_temporal_history;
    std::map<std::string, TemporalViewFamilyHistory,
             std::less<>>
        secondary_temporal_histories;
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
    GlobalRenderTargetId last_picking_target = noRenderTargetId();
    RenderingPassId last_picking_rendering_pass_id =
        invalidRenderingPassId();
    std::uint64_t last_picking_runtime_generation = 0;
    std::optional<vk::Extent2D> last_picking_extent;
    std::uint64_t last_picking_frame_index = 0;
    std::vector<std::optional<PickingModelInstanceToken>>
        last_picking_model_instances;

    TemporalViewFamilyHistory &activeTemporalHistory();
    const TemporalViewFamilyHistory &activeTemporalHistory() const;
    void installRenderPipelineReloadParticipant();
    void relowerRenderPipelineForCurrentOutput();
    bool reloadRenderPipelineFromDisk(
        std::string &error,
        std::span<const watch::ReloadRequest>
            companion_requests = {}) noexcept;

  public:
    Renderer();
    ~Renderer();
    nlohmann::json currentFramePlanJson() const;
    RenderPipelineAuthoringApplyResult
    applyRenderPipelineAuthoringCandidate(
        RenderConfigCandidateDocumentSet candidate_documents,
        const std::function<void()> &source_commit);
    PickingReadbackResult readPickingPixel(std::uint32_t x,
                                           std::uint32_t y);
    R8RenderTargetReadback readR8RenderTargetForTesting(
        std::string_view name);
    std::optional<vk::Format>
    xrCompositionDepthFormat() const;
    std::vector<std::string> currentFramePlanOrderForTesting() const;
    RayQueryAccelerationStructureDiagnostics
    rayQueryAccelerationStructureDiagnosticsForTesting() const;
    void setExecutionTracingForTesting(bool enabled) { execution_tracing_for_testing = enabled; }
    const nlohmann::json &lastExecutionTraceForTesting() const { return last_execution_trace; }
    std::size_t imageMemoryDependencyCountForTesting() const noexcept {
        return render_target_layout_tracker.memoryDependencyCountForTesting();
    }
    std::size_t imageAliasDependencyCountForTesting() const noexcept {
        return render_target_layout_tracker.aliasDependencyCountForTesting();
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
        const RenderViewFamily &view_family);
    void renderLogicalFrame(
        ILogicalFrameTarget &target,
        const RenderViewFamilies &view_families);
    // Windowed/headless flat-output convenience path with caller-authored
    // secondary families. Supplying a family bypasses its registered runtime
    // provider while preserving the normal graph scheduler and backend.
    void render(
        const RenderViewFamilies
            &view_families);
    void render();
};

} // namespace Pelican
