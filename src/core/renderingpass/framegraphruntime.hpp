#pragma once

#include "frameplanner.hpp"
#include "renderpipelinegpuarena.hpp"
#include "renderingpass.hpp"
#include "../../project/executionplan.hpp"
#include "../../project/targetrenderplanning.hpp"
#include "../container.hpp"
#include "../vkcore/outputcompilefacts.hpp"
#include <details/ecs/entity.hpp>
#include "graphdeformedattribute.hpp"
#include "graphmodelsource.hpp"
#if PELICAN_WITH_ANIMATION
#include "../animation/posegraph.hpp"
#endif
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace Pelican {
namespace ValueGraph { class Graph; }
class MaterialDrawLoopTable;
struct MaterialDrawLoopRules;

class PreparedVulkanNativeScopeGraph;

// Authored JSON references and the bytes used to compile one variant family.
using RenderGraphDocumentSnapshot = std::map<std::string, std::string, std::less<>>;

// IA-1: the asset a persistent render.image starts from (`initial: {asset}`),
// read when the generation was compiled. The renderer copies `texels` (level
// 0, rows packed) into the image each time the image is made and on the
// first frame of a generation.
struct RenderImageInitialAsset {
    // The reference as written, and the file it resolved to.
    std::string asset;
    std::filesystem::path file;
    // The file's sha256 in its asset store's assets.manifest, when listed.
    std::optional<std::string> sha256;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The Vulkan name of the texels' format ("R32G32B32A32_SFLOAT").
    std::string format;
    std::vector<std::byte> texels;
};
using RenderImageInitialAssets =
    std::map<std::string, std::shared_ptr<const RenderImageInitialAsset>, std::less<>>;

struct CompiledFrameGraphBarrier {
    std::string resource;
    size_t from_node_index = 0;
    FramePlanNodeKind from_kind = FramePlanNodeKind::render;
    FramePlanNodeKind to_kind = FramePlanNodeKind::render;
};

struct FrameGraphExecutionNode {
    FramePlanNodeKind kind = FramePlanNodeKind::render;
    std::string name;
    size_t index = 0;
    std::vector<CompiledFrameGraphBarrier> incoming_barriers;
    std::string view_family{
        mainRenderViewFamilyId};
    RenderViewSelect view_select;
    // PS-1: FramePlanNode::once.
    bool once = false;
};

struct CompiledMaterialRouteBinding {
    MaterialRouteClass route = MaterialRouteClass::deferred_geometry;
    PassId pass_id{};
    MaterialPassContract pass_contract =
        MaterialPassContract::deferred_geometry_v1;
    std::optional<MaterialOutputSchema> output_schema;
    std::vector<MaterialOutputAttachmentState> output_states;
};

struct CompiledFrameGraphExecution {
    FramePlan plan;
    FrameExecutionPlan execution_plan;
    std::shared_ptr<const CompiledRenderPipeline> render_pipeline;
    std::shared_ptr<const VulkanTargetPlan> target_plan;
    std::shared_ptr<const PreparedVulkanNativeScopeGraph>
        native_scopes;
    std::shared_ptr<const ResolvedSampleCountPlan> sample_count_plan;
    std::vector<CompiledMaterialRouteBinding> material_routes;
    std::unordered_map<std::string, GlobalRenderTargetId>
        render_target_bindings;
    std::unordered_map<std::string, FrameGraphBufferId>
        buffer_bindings;
    std::vector<FrameGraphExecutionNode> nodes;
    std::shared_ptr<const ValueGraph::Graph> value_graph;
#if PELICAN_WITH_ANIMATION
    // The animation partition of the same document. Evaluated in the animation
    // phase, one step earlier in the frame than value_graph.
    std::shared_ptr<const Animation::PoseGraph> pose_graph;
#endif
    // Owner scopes omitted from this generation's GPU compilation.
    std::set<std::string> inactive_graph_scopes;
    std::map<std::string, GameObjectId> graph_owner_objects;
    std::shared_ptr<const RenderGraphDocumentSnapshot> source_documents;
    std::vector<GraphDeformedAttribute> deformed_attributes;
    std::vector<GraphPoseAttribute> pose_attributes;
    // Resolved dependencies of the attribute snapshot owned by this program.
    // Runtime metadata only; never copied into the authored graph document.
    std::vector<std::filesystem::path> attribute_source_files;
    // IA-1: by render target name, the persistent images' initial assets.
    RenderImageInitialAssets image_initial_assets;
};

struct CompiledRenderProgram {
    RenderingPassId rendering_pass_id = invalidRenderingPassId();
    std::string owner_scope;
    CompiledRenderingPass rendering_pass;
    CompiledFrameGraphExecution frame_graph;
    // Conservative cross-scope dependencies transferred when another
    // owner is replaced while this program remains published.
    std::vector<std::shared_ptr<const void>> resource_leases;
};

struct WindowOutputGeneration {
    std::uint64_t generation = 0;
    OutputCompileFacts compile_facts;
    std::uint64_t compile_fingerprint = 0;
    // The concrete target/pipeline arena selected by compile_facts. The
    // renderer root and every frame token retain this lease together.
    std::shared_ptr<const void> target_resource_lease;
};

// One immutable publication root for every value that must agree while a
// frame is recorded. A frame keeps a shared snapshot, so replacing the active
// root cannot retire the previous pass/plan generation early.
struct RendererRuntimeGeneration {
    std::uint64_t generation = 0;
    std::vector<std::string> enabled_feature_names;
    std::vector<RenderingPassId> rendering_pass_ids;
    std::unordered_map<std::string, RenderingPassId> name_to_id;
    // Declared before programs so provider executors and their private device
    // objects retire before the registry resources they can reference.
    std::shared_ptr<const RenderPipelineGpuArena> gpu_arena;
    std::unordered_map<RenderingPassId, CompiledRenderProgram,
                       RenderingPassId::Hash>
        programs;
    std::shared_ptr<const WindowOutputGeneration> window_output;
    // LP-2: this generation's draw-step table and the override pipelines it
    // owns. Built when the generation is prepared, amended when a material
    // is registered while it is alive, and released with it. Never null in
    // a prepared generation; the table is mutable while the generation is
    // not.
    std::shared_ptr<MaterialDrawLoopTable> material_draw_loops;

    const CompiledRenderProgram *find(
        RenderingPassId rendering_pass_id) const noexcept;
};

struct RendererRuntimePublication {
    std::atomic<
        std::shared_ptr<const RendererRuntimeGeneration>>
        active_generation{nullptr};
};

struct RenderPipelineProgramPreparation {
    std::string owner_scope;
    CompiledRenderingPass rendering_pass;
    FramePlan frame_plan;
    FrameExecutionPlan execution_plan;
    std::shared_ptr<const CompiledRenderPipeline> render_pipeline;
    std::shared_ptr<const VulkanTargetPlan> target_plan;
    std::shared_ptr<const PreparedVulkanNativeScopeGraph>
        native_scopes;
    std::unordered_map<std::string, GlobalRenderTargetId>
        render_target_bindings;
    std::unordered_map<std::string, FrameGraphBufferId>
        buffer_bindings;
    std::optional<RenderingPassId> rendering_pass_id;
    std::shared_ptr<const ValueGraph::Graph> value_graph;
#if PELICAN_WITH_ANIMATION
    std::shared_ptr<const Animation::PoseGraph> pose_graph;
#endif
    std::set<std::string> inactive_graph_scopes;
    std::map<std::string, GameObjectId> graph_owner_objects;
    std::shared_ptr<const RenderGraphDocumentSnapshot> source_documents;
    std::vector<GraphDeformedAttribute> deformed_attributes;
    std::vector<GraphPoseAttribute> pose_attributes;
    std::vector<std::filesystem::path> attribute_source_files;
    RenderImageInitialAssets image_initial_assets;
};

class PreparedRendererRuntimeGeneration {
    friend class FrameGraphRuntimeContainer;

    std::uint64_t base_generation_ = 0;
    std::shared_ptr<const RendererRuntimeGeneration> candidate_;
    std::vector<RenderingPassId> prepared_rendering_pass_ids_;

    PreparedRendererRuntimeGeneration(
        std::uint64_t base_generation,
        std::shared_ptr<const RendererRuntimeGeneration> candidate,
        std::vector<RenderingPassId> prepared_rendering_pass_ids);

  public:
    PreparedRendererRuntimeGeneration() = default;
    PreparedRendererRuntimeGeneration(
        const PreparedRendererRuntimeGeneration &) = delete;
    PreparedRendererRuntimeGeneration &operator=(
        const PreparedRendererRuntimeGeneration &) = delete;
    PreparedRendererRuntimeGeneration(
        PreparedRendererRuntimeGeneration &&) noexcept = default;
    PreparedRendererRuntimeGeneration &operator=(
        PreparedRendererRuntimeGeneration &&) noexcept = default;

    bool valid() const noexcept {
        return candidate_ != nullptr;
    }
    std::uint64_t baseGeneration() const noexcept {
        return base_generation_;
    }
    std::uint64_t generation() const noexcept {
        return candidate_ != nullptr ? candidate_->generation : 0;
    }
    const std::vector<RenderingPassId> &renderingPassIds() const noexcept {
        return prepared_rendering_pass_ids_;
    }
    const RendererRuntimeGeneration &candidate() const {
        if (candidate_ == nullptr) {
            throw std::logic_error(
                "Renderer runtime candidate is empty");
        }
        return *candidate_;
    }
};

// LP-2: draw-loop rules a caller supplies for a generation being prepared,
// on top of the ones its programs' compiled render pipelines carry. Null or
// empty: none.
using MaterialDrawLoopRuleSource =
    std::function<std::shared_ptr<const MaterialDrawLoopRules>(
        const RendererRuntimeGeneration &candidate)>;

DECLARE_MODULE(FrameGraphRuntimeContainer) {
    struct StagedPublication {
        PreparedRendererRuntimeGeneration prepared;
        std::shared_ptr<const void> lifetime;
        std::function<void()> commit_lifetime;
        std::function<void(const RendererRuntimeGeneration &)>
            before_publish;
        std::function<void(const RendererRuntimeGeneration &)>
            after_publish;
        std::function<void()> after_rollback;
        bool ready = false;
    };

    std::shared_ptr<RendererRuntimePublication>
        publication;
    mutable std::mutex publication_mutex;
    MaterialDrawLoopRuleSource material_draw_loop_rule_source;
    std::optional<StagedPublication> staged_publication;

  public:
    FrameGraphRuntimeContainer();
    ~FrameGraphRuntimeContainer();

    // LP-2: every generation prepared from now on also evaluates the rules
    // this returns. Rules of the same pass name replace the pipeline's own.
    void setMaterialDrawLoopRuleSource(MaterialDrawLoopRuleSource source);

    PreparedRendererRuntimeGeneration prepareGeneration(
        std::vector<RenderPipelineProgramPreparation> programs,
        std::optional<std::vector<std::string>> enabled_feature_names =
            std::nullopt,
        std::optional<RenderPipelineGpuScopePreparation>
            gpu_scope = std::nullopt,
        std::optional<OutputCompileFacts>
            window_output_facts = std::nullopt) const;
    void publishPreparedGeneration(
        PreparedRendererRuntimeGeneration &&prepared,
        const std::function<void(
            const RendererRuntimeGeneration &)>
            &before_publish = {},
        const std::function<void(
            const RendererRuntimeGeneration &)>
            &after_publish = {});
    // A ray-layout generation is executable while private, but remains
    // absent from snapshot() until the frame containing its complete SBT,
    // row snapshot, and TLAS has been submitted successfully.
    void stagePreparedGeneration(
        PreparedRendererRuntimeGeneration &&prepared,
        std::shared_ptr<const void> lifetime,
        std::function<void()> commit_lifetime,
        std::function<void(const RendererRuntimeGeneration &)>
            before_publish = {},
        std::function<void(const RendererRuntimeGeneration &)>
            after_publish = {},
        std::function<void()> after_rollback = {});
    std::shared_ptr<const RendererRuntimeGeneration>
    renderingSnapshot() const noexcept;
    std::uint64_t stagedGeneration() const noexcept;
    void prepareStagedGenerationCommit(std::uint64_t generation);
    bool publishStagedGeneration(std::uint64_t generation) noexcept;
    bool rollbackStagedGeneration(std::uint64_t generation) noexcept;
    void rollbackPreparedGeneration(
        PreparedRendererRuntimeGeneration &prepared) const noexcept;

    std::shared_ptr<const RendererRuntimeGeneration>
    snapshot() const noexcept;
    std::shared_ptr<const RendererRuntimePublication>
    publicationState() const noexcept;
    std::uint64_t activeGeneration() const noexcept;
    std::shared_ptr<const CompiledRenderProgram> findProgram(
        RenderingPassId rendering_pass_id) const;

    // Compatibility facade for isolated callers. Production registration
    // prepares every program first and publishes the whole generation once.
    void registerExecutionPlan(RenderingPassId rendering_pass_id,
                               const CompiledRenderingPass &compiled_pass,
                               FramePlan plan,
                               std::shared_ptr<const CompiledRenderPipeline>
                                   render_pipeline,
                               std::shared_ptr<const VulkanTargetPlan>
                                   target_plan = {});
    std::shared_ptr<const CompiledFrameGraphExecution> find(
        RenderingPassId rendering_pass_id) const;
};

} // namespace Pelican
