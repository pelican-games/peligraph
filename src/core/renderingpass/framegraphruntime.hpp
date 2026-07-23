#pragma once

#include "frameplanner.hpp"
#include "renderingpass.hpp"
#include "../../project/targetrenderplanning.hpp"
#include "../container.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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
};

struct CompiledMaterialRouteBinding {
    MaterialRouteClass route = MaterialRouteClass::deferred_geometry;
    PassId pass_id{};
    MaterialPassContract pass_contract =
        MaterialPassContract::deferred_geometry_v1;
};

struct CompiledFrameGraphExecution {
    FramePlan plan;
    std::shared_ptr<const CompiledRenderPipeline> render_pipeline;
    std::shared_ptr<const VulkanTargetPlan> target_plan;
    std::shared_ptr<const ResolvedSampleCountPlan> sample_count_plan;
    std::vector<CompiledMaterialRouteBinding> material_routes;
    std::vector<FrameGraphExecutionNode> nodes;
};

struct CompiledRenderProgram {
    RenderingPassId rendering_pass_id = invalidRenderingPassId();
    CompiledRenderingPass rendering_pass;
    CompiledFrameGraphExecution frame_graph;
};

// One immutable publication root for every value that must agree while a
// frame is recorded. A frame keeps a shared snapshot, so replacing the active
// root cannot retire the previous pass/plan generation early.
struct RenderPipelineRuntimeGeneration {
    std::uint64_t generation = 0;
    std::vector<std::string> enabled_feature_names;
    std::vector<RenderingPassId> rendering_pass_ids;
    std::unordered_map<std::string, RenderingPassId> name_to_id;
    std::unordered_map<RenderingPassId, CompiledRenderProgram,
                       RenderingPassId::Hash>
        programs;

    const CompiledRenderProgram *find(
        RenderingPassId rendering_pass_id) const noexcept;
};

struct RenderPipelineRuntimePublication {
    std::atomic<
        std::shared_ptr<const RenderPipelineRuntimeGeneration>>
        active_generation{nullptr};
};

struct RenderPipelineProgramPreparation {
    CompiledRenderingPass rendering_pass;
    FramePlan frame_plan;
    std::shared_ptr<const CompiledRenderPipeline> render_pipeline;
    std::shared_ptr<const VulkanTargetPlan> target_plan;
    std::optional<RenderingPassId> rendering_pass_id;
};

class PreparedRenderPipelineGeneration {
    friend class FrameGraphRuntimeContainer;

    std::uint64_t base_generation_ = 0;
    std::shared_ptr<const RenderPipelineRuntimeGeneration> candidate_;
    std::vector<RenderingPassId> prepared_rendering_pass_ids_;

    PreparedRenderPipelineGeneration(
        std::uint64_t base_generation,
        std::shared_ptr<const RenderPipelineRuntimeGeneration> candidate,
        std::vector<RenderingPassId> prepared_rendering_pass_ids);

  public:
    PreparedRenderPipelineGeneration() = default;
    PreparedRenderPipelineGeneration(
        const PreparedRenderPipelineGeneration &) = delete;
    PreparedRenderPipelineGeneration &operator=(
        const PreparedRenderPipelineGeneration &) = delete;
    PreparedRenderPipelineGeneration(
        PreparedRenderPipelineGeneration &&) noexcept = default;
    PreparedRenderPipelineGeneration &operator=(
        PreparedRenderPipelineGeneration &&) noexcept = default;

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
};

DECLARE_MODULE(FrameGraphRuntimeContainer) {
    std::shared_ptr<RenderPipelineRuntimePublication>
        publication;

  public:
    FrameGraphRuntimeContainer();
    ~FrameGraphRuntimeContainer();

    PreparedRenderPipelineGeneration prepareGeneration(
        std::vector<RenderPipelineProgramPreparation> programs,
        std::optional<std::vector<std::string>> enabled_feature_names =
            std::nullopt) const;
    void publishPreparedGeneration(
        PreparedRenderPipelineGeneration &&prepared);
    void rollbackPreparedGeneration(
        PreparedRenderPipelineGeneration &prepared) const noexcept;

    std::shared_ptr<const RenderPipelineRuntimeGeneration>
    snapshot() const noexcept;
    std::shared_ptr<const RenderPipelineRuntimePublication>
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
