#pragma once

#include "frameplanner.hpp"
#include "renderingpass.hpp"
#include "../container.hpp"
#include <cstddef>
#include <memory>
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
    std::vector<CompiledMaterialRouteBinding> material_routes;
    std::vector<FrameGraphExecutionNode> nodes;
};

DECLARE_MODULE(FrameGraphRuntimeContainer) {
    std::unordered_map<RenderingPassId, CompiledFrameGraphExecution, RenderingPassId::Hash> graphs;

  public:
    FrameGraphRuntimeContainer();
    ~FrameGraphRuntimeContainer();

    void registerExecutionPlan(RenderingPassId rendering_pass_id,
                               const CompiledRenderingPass &compiled_pass,
                               FramePlan plan,
                               std::shared_ptr<const CompiledRenderPipeline>
                                   render_pipeline);
    const CompiledFrameGraphExecution *find(RenderingPassId rendering_pass_id) const;
};

} // namespace Pelican
