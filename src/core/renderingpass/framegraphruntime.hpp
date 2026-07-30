#pragma once

#include "frameplanner.hpp"
#include "renderpipelinegpuarena.hpp"
#include "renderingpass.hpp"
#include "../../project/executionplan.hpp"
#include "../../project/targetrenderplanning.hpp"
#include "../container.hpp"
#include "../vkcore/outputcompilefacts.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace Pelican {

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
    std::shared_ptr<const ResolvedSampleCountPlan> sample_count_plan;
    std::vector<CompiledMaterialRouteBinding> material_routes;
    std::unordered_map<std::string, GlobalRenderTargetId>
        render_target_bindings;
    std::unordered_map<std::string, FrameGraphBufferId>
        buffer_bindings;
    std::vector<FrameGraphExecutionNode> nodes;
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
    std::unordered_map<RenderingPassId, CompiledRenderProgram,
                       RenderingPassId::Hash>
        programs;
    std::shared_ptr<const RenderPipelineGpuArena> gpu_arena;
    std::shared_ptr<const WindowOutputGeneration> window_output;

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
    std::unordered_map<std::string, GlobalRenderTargetId>
        render_target_bindings;
    std::unordered_map<std::string, FrameGraphBufferId>
        buffer_bindings;
    std::optional<RenderingPassId> rendering_pass_id;
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

DECLARE_MODULE(FrameGraphRuntimeContainer) {
    std::shared_ptr<RendererRuntimePublication>
        publication;
    mutable std::mutex publication_mutex;

  public:
    FrameGraphRuntimeContainer();
    ~FrameGraphRuntimeContainer();

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
            &before_publish = {});
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
