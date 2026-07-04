#pragma once

#include "frameplanner.hpp"
#include "renderingpass.hpp"
#include "../container.hpp"
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace Pelican {

struct FrameGraphExecutionNode {
    FramePlanNodeKind kind = FramePlanNodeKind::render;
    std::string name;
    size_t index = 0;
};

struct CompiledFrameGraphExecution {
    FramePlan plan;
    std::vector<FrameGraphExecutionNode> nodes;
    bool has_compute = false;
};

DECLARE_MODULE(FrameGraphRuntimeContainer) {
    std::unordered_map<RenderingPassId, CompiledFrameGraphExecution, RenderingPassId::Hash> graphs;

  public:
    FrameGraphRuntimeContainer();
    ~FrameGraphRuntimeContainer();

    void registerExecutionPlan(RenderingPassId rendering_pass_id,
                               const CompiledRenderingPass &compiled_pass,
                               const FrameGraphDefinition &definition);
    const CompiledFrameGraphExecution *find(RenderingPassId rendering_pass_id) const;
};

} // namespace Pelican
