#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../renderingpass/oncescheduler.hpp"
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
struct RenderImageInitialAsset;
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

// MP-1: the most materials a material node's drawn_materials lists; past it
// the first ones are kept and drawn_materials_truncated is true.
inline constexpr std::size_t drawnMaterialsListLimit = 256;

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

// BS-1: a rendering configuration refusal met before the renderer exists
// (PelicanCore::run's ray-layout preflight reads the root first). Under
// --render-fallback the run records it here instead of stopping, and
// Renderer() starts on the fallback generation with it. Never set otherwise.
DECLARE_MODULE(RenderPipelineStartupRefusal) {
  public:
    std::optional<nlohmann::json> refusal;
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
    // BS-1: set while the published generation is the built-in fallback
    // (--render-fallback met a refusal at start): the refusal, as
    // renderPipelineRefusalJson shaped it. The first publication of a
    // project configuration clears it.
    std::optional<nlohmann::json> render_pipeline_fallback_refusal;
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
    // LP-Va2: the nodes of the last published logical frame whose
    // view_select matched none of that frame's views, and the program they
    // belong to. get_frame_plan copies them as nodes[].no_selected_view.
    std::vector<std::string> last_no_selected_view_nodes;
    RenderingPassId last_no_selected_view_rendering_pass_id =
        invalidRenderingPassId();
    std::uint64_t last_no_selected_view_runtime_generation = 0;
    // MP-1: what each material pass drew in the last published logical
    // frame, by pass name, and the program, generation and frame
    // (EngineTime::evaluationIndex) it was. get_frame_plan copies it as the
    // material nodes' drawn_materials.
    struct DrawnMaterial {
        int material = -1;
        std::string name;
        std::uint64_t draws = 0;
    };
    struct DrawnMaterialsOfPass {
        std::vector<DrawnMaterial> materials;
        bool truncated = false;
    };
    struct LastDrawnMaterials {
        bool recorded = false;
        RenderingPassId rendering_pass_id = invalidRenderingPassId();
        std::uint64_t runtime_generation = 0;
        std::uint64_t frame = 0;
        std::map<std::string, DrawnMaterialsOfPass, std::less<>> passes;
    };
    LastDrawnMaterials last_drawn_materials;
    // PS-1: what each `schedule: "once"` node last ran with, by program
    // (RenderingPassId value) and node name, and the names rerun_once asked
    // to run on the next frame. A record is written when the frame that ran
    // the node is published.
    std::map<int, std::map<std::string, OnceRunRecord, std::less<>>>
        once_runs;
    std::set<std::string, std::less<>> once_rerun_requests;
    // IA-1: what each persistent image with an initial asset was last filled
    // from, by program (RenderingPassId value) and target name: the VkImage,
    // the runtime generation and the asset read for it. Another of any of
    // the three fills it again before the frame records.
    struct ImageInitialUpload {
        std::uint64_t image = 0;
        std::uint64_t generation = 0;
        std::shared_ptr<const RenderImageInitialAsset> asset;
    };
    std::map<int, std::map<std::string, ImageInitialUpload, std::less<>>>
        image_initial_uploads;
    // FP-1: the part of get_frame_plan the published generation and its
    // program decide (the plan, execution plan, physical target plan and
    // the rest that only read them), built the first time the frame plan is
    // asked for in that generation and copied into every answer until
    // another generation, program or rendering root document is current.
    // What a frame, the scene, a file or a runtime setting changes is added
    // to the copy on each call. currentFramePlanJson runs on the frame
    // thread only (RPC at the frame boundary, the plan viewer), so this
    // takes no lock.
    struct FramePlanGenerationPart {
        std::weak_ptr<const RendererRuntimeGeneration> generation;
        std::uint64_t runtime_generation = 0;
        RenderingPassId rendering_pass_id = invalidRenderingPassId();
        std::string graph_reference;
        nlohmann::json plan;
        // How many times a generation's part was built (tests read it).
        std::uint64_t builds = 0;
    };
    mutable FramePlanGenerationPart frame_plan_generation_part;
    std::uint64_t next_ray_scene_generation = 1;

    TemporalViewFamilyHistory &activeTemporalHistory();
    const TemporalViewFamilyHistory &activeTemporalHistory() const;
    void installRenderPipelineReloadParticipant();
    // PB-1: `validate_live_materials` false for the switch to the fallback
    // generation at start, which draws no material.
    void relowerRenderPipelineForCurrentOutput(
        bool validate_live_materials = true);
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
    // The current generation's ray-query acceleration structures, as
    // get_frame_plan's ray_query_acceleration_structures reports them
    // (RTL-1b: production reads it, so it is not named for testing).
    RayQueryAccelerationStructureDiagnostics
    rayQueryAccelerationStructureDiagnostics() const;
    void recreateRenderTargetsAndRebindForTesting(vk::Extent2D extent);

  public:
    Renderer();
    Renderer(RendererFramePlanCpuSeam,
             RenderingPassId rendering_pass_id);
    ~Renderer();
    nlohmann::json currentFramePlanJson() const;
    // PS-1: rerun_once. Asks the next frame to run the once nodes `node`
    // made (a pass or task's graph node; a graph.loop runs all its copies'),
    // or every once node when `node` is empty. Returns the names asked for,
    // in plan order. std::invalid_argument when the current frame plan has
    // no node of that name.
    std::vector<std::string> requestOnceRerun(
        const std::optional<std::string> &node);
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
    // BS-1: whether the published generation is the built-in fallback.
    bool renderPipelineFallback() const noexcept {
        return render_pipeline_fallback_refusal.has_value();
    }
    // BS-1: get_status.render_pipeline.
    //   {state: "ready", root}
    //   {state: "fallback", root, refusal: {code, ...fields, message}}
    // `root` is the rendering root's reference as project.json names it.
    nlohmann::json renderPipelineStatusJson() const;
    // Resolves every renderer-owned runtime dependency while module creation is
    // still legal. Subsequent render calls only read the frozen module graph.
    void prepareRuntimeModules();
    // PB-1 (engine_backlog 23): the end of a --render-fallback launch's start,
    // once every module is built and before the graph is frozen. When a
    // material registered at start found no material pass for its route
    // (MaterialStartupUnserved), the run goes on on the fallback generation
    // with material_route_unserved@1 as its refusal, as a configuration
    // refused when it was composed does. Otherwise nothing changes.
    void enterStartupFallbackForUnservedMaterials();
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
