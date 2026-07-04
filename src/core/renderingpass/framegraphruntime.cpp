#include "framegraphruntime.hpp"
#include <stdexcept>

namespace Pelican {

namespace {

std::unordered_map<std::string, size_t> renderPassIndices(const CompiledRenderingPass &compiled_pass) {
    std::unordered_map<std::string, size_t> indices;
    for (size_t i = 0; i < compiled_pass.passes.size(); ++i) {
        indices.emplace(compiled_pass.passes[i].definition.name, i);
    }
    return indices;
}

std::unordered_map<std::string, size_t> computeTaskIndices(const CompiledRenderingPass &compiled_pass) {
    std::unordered_map<std::string, size_t> indices;
    for (size_t i = 0; i < compiled_pass.compute_tasks.size(); ++i) {
        indices.emplace(compiled_pass.compute_tasks[i].definition.name, i);
    }
    return indices;
}

} // namespace

FrameGraphRuntimeContainer::FrameGraphRuntimeContainer() = default;

FrameGraphRuntimeContainer::~FrameGraphRuntimeContainer() = default;

void FrameGraphRuntimeContainer::registerExecutionPlan(RenderingPassId rendering_pass_id,
                                                       const CompiledRenderingPass &compiled_pass,
                                                       const FrameGraphDefinition &definition) {
    auto plan = planFrameGraph(definition);
    auto pass_indices = renderPassIndices(compiled_pass);
    auto task_indices = computeTaskIndices(compiled_pass);

    CompiledFrameGraphExecution execution;
    execution.has_compute = !compiled_pass.compute_tasks.empty();
    execution.plan = std::move(plan);
    execution.nodes.reserve(execution.plan.nodes.size());

    for (const auto &node : execution.plan.nodes) {
        if (node.kind == FramePlanNodeKind::render) {
            auto found = pass_indices.find(node.name);
            if (found == pass_indices.end()) {
                throw std::runtime_error("Frame plan render node is not compiled: " + node.name);
            }
            execution.nodes.push_back(FrameGraphExecutionNode{node.kind, node.name, found->second});
        } else if (node.kind == FramePlanNodeKind::compute) {
            auto found = task_indices.find(node.name);
            if (found == task_indices.end()) {
                throw std::runtime_error("Frame plan compute node is not compiled: " + node.name);
            }
            execution.nodes.push_back(FrameGraphExecutionNode{node.kind, node.name, found->second});
        } else {
            throw std::runtime_error("Unsupported frame plan node kind: " + node.name);
        }
    }

    graphs.insert_or_assign(rendering_pass_id, std::move(execution));
}

const CompiledFrameGraphExecution *FrameGraphRuntimeContainer::find(RenderingPassId rendering_pass_id) const {
    if (auto found = graphs.find(rendering_pass_id); found != graphs.end()) {
        return &found->second;
    }
    return nullptr;
}

} // namespace Pelican
