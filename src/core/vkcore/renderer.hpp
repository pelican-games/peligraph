#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../renderingpass/previewgraph.hpp"
#include "../renderer/viewfamily.hpp"
#include "render_target_layout_tracker.hpp"
#include "rendertarget.hpp"
#include "../../project/imagesubresource.hpp"
#include "../../project/renderconfigdocument.hpp"
#include <details/ecs/entity.hpp>
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Pelican {

struct RenderPipelineReloadState;
struct RenderPipelinePublicationTransaction;
struct RenderPipelineAuthoringApplyResult {
    bool committed = false;
    bool staged = false;
    std::uint64_t staged_generation = 0;
    std::uint64_t published_generation = 0;
    std::string error;
    std::string post_commit_error;
    // Q-14e: what `error` was read from, so a typed refusal keeps its type
    // on the way to the editor. Empty when the error was not an exception.
    std::exception_ptr exception;
};
using RenderPipelineAuthoringCompletion =
    std::function<void(RenderPipelineAuthoringApplyResult)>;
struct RayQueryAccelerationStructureDiagnostics;
namespace watch {
struct ReloadRequest;
}
namespace test_support {
// Defined in test/renderer_testaccess.hpp. No production translation unit
// includes that header.
struct RendererTestAccess;
}

enum class RenderGraphVariant {
    flat,
    xr,
};

// CPU-only construction seam for exercising the production
// currentFramePlanJson() path against an already-published runtime program.
// It deliberately bypasses startup compilation and creates no GPU objects.
struct RendererFramePlanCpuSeam final {};

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

struct RenderTargetReadback {
    vk::Extent2D extent{};
    vk::Format format{};
    std::uint32_t layer_count = 1;
    // Layers the image actually has, so a caller narrowing to one
    // can tell whether it saw the whole thing.
    std::uint32_t image_layer_count = 1;
    std::vector<std::uint8_t> bytes;
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
    // Some targets (OpenXR sequential/multiview) submit in endView/endViewFamily.
    // This lets the renderer seal the retirement batch at the first successful
    // real submit rather than at logical-frame publication.
    virtual bool submitsOnViewEnd() const noexcept { return false; }
    // Three-phase logical-frame boundary.  Legacy fixtures can continue to
    // implement endLogicalFrame; production targets override these two calls.
    virtual void submitLogicalFrame(GpuSubmissionLease lease = {}) {
        endLogicalFrame(std::move(lease));
    }
    virtual void publishLogicalFrame() {}
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
    // One publish()/rollback() pair serves both render pipeline reload paths.
    friend struct RenderPipelinePublicationTransaction;
    // The reads and pokes only a test performs, including the execution
    // trace below. They are declared here rather than in the public section
    // so the public header states what production calls and nothing else;
    // the struct that reaches them lives in test/.
    friend struct test_support::RendererTestAccess;
    // One logical frame's recording state and steps. Defined in renderer.cpp
    // because its fields name types that only the renderer translation unit
    // has; being a member of Renderer is what lets it reach the renderer state
    // a frame mutates.
    class LogicalFrameRecorder;
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
    std::uint64_t published_logical_frames = 0;
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
    std::uint64_t next_ray_scene_generation = 1;

    TemporalViewFamilyHistory &activeTemporalHistory();
    const TemporalViewFamilyHistory &activeTemporalHistory() const;
    void installRenderPipelineReloadParticipant();
    void relowerRenderPipelineForCurrentOutput();
    void synchronizeGraphGpuOwnership();
    std::uint64_t graph_ownership_attempt_generation = 0;
    std::map<std::string, GameObjectId> graph_ownership_attempt_objects;
    RenderPipelineAuthoringApplyResult
    reloadRenderPipelineFromDisk(
        std::span<const watch::ReloadRequest>
            companion_requests = {}) noexcept;

    RenderTargetReadback readRenderTargetForTesting(
        std::string_view name,
        const ImageSubresourceRange &range);
    R8RenderTargetReadback readR8RenderTargetForTesting(
        std::string_view name);
    std::vector<std::string> currentFramePlanOrderForTesting() const;
    RayQueryAccelerationStructureDiagnostics
    rayQueryAccelerationStructureDiagnosticsForTesting() const;
    void recreateRenderTargetsAndRebindForTesting(vk::Extent2D extent);

  public:
    Renderer();
    Renderer(RendererFramePlanCpuSeam,
             RenderingPassId rendering_pass_id);
    ~Renderer();
    nlohmann::json currentFramePlanJson() const;
    bool hasPendingRenderPipelinePublication() const noexcept;
    // Logical frames this renderer has published since it was created. The
    // engine answers RPC before its first frame; a client that needs a drawn
    // frame (capture, a published frame plan) waits for this to become
    // non-zero instead of retrying named "not yet" errors.
    std::uint64_t publishedLogicalFrameCount() const noexcept {
        return published_logical_frames;
    }
    RenderPipelineAuthoringApplyResult
    applyRenderPipelineAuthoringCandidate(
        RenderConfigCandidateDocumentSet candidate_documents,
        std::function<void()> source_commit,
        RenderPipelineAuthoringCompletion completion = {});
    PickingReadbackResult readPickingPixel(std::uint32_t x,
                                           std::uint32_t y);
    std::optional<vk::Format>
    xrCompositionDepthFormat() const;
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
