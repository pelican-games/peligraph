#include "framegraphruntime.hpp"
#include <stdexcept>
#include <utility>

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

std::vector<CompiledMaterialRouteBinding> bindMaterialRoutes(
    const CompiledRenderingPass &compiled_pass,
    const CompiledRenderPipeline &render_pipeline) {
    std::vector<CompiledMaterialRouteBinding> bindings;
    if (!render_pipeline.material_routing) return bindings;

    bindings.reserve(render_pipeline.material_routing->routes.size());
    for (const auto &route : render_pipeline.material_routing->routes) {
        const CompiledPass *selected = nullptr;
        for (const auto &pass : compiled_pass.passes) {
            if (pass.definition.name != route.pass_name) continue;
            if (selected != nullptr) {
                throw std::runtime_error(
                    "Compiled material route pass is ambiguous: " +
                    route.pass_name);
            }
            selected = &pass;
        }
        if (selected == nullptr) {
            throw std::runtime_error("Compiled material route pass is missing: " +
                                     route.pass_name);
        }
        if (!selected->definition.isMaterial()) {
            throw std::runtime_error(
                "Compiled material route selects a non-material pass: " +
                route.pass_name);
        }
        if (selected->definition.materialInfo().contract !=
            route.pass_contract) {
            throw std::runtime_error(
                "Compiled material route contract mismatch for pass: " +
                route.pass_name);
        }
        bindings.push_back(CompiledMaterialRouteBinding{
            route.route, selected->pass_id, route.pass_contract});
    }
    return bindings;
}

} // namespace

FrameGraphRuntimeContainer::FrameGraphRuntimeContainer() = default;

FrameGraphRuntimeContainer::~FrameGraphRuntimeContainer() = default;

void FrameGraphRuntimeContainer::registerExecutionPlan(RenderingPassId rendering_pass_id,
                                                       const CompiledRenderingPass &compiled_pass,
                                                       FramePlan plan,
                                                       std::shared_ptr<const CompiledRenderPipeline>
                                                           render_pipeline) {
    if (!render_pipeline) {
        throw std::runtime_error(
            "Frame graph execution requires a compiled render pipeline");
    }
    auto pass_indices = renderPassIndices(compiled_pass);
    auto task_indices = computeTaskIndices(compiled_pass);
    auto material_routes =
        bindMaterialRoutes(compiled_pass, *render_pipeline);

    CompiledFrameGraphExecution execution;
    execution.plan = std::move(plan);
    execution.render_pipeline = std::move(render_pipeline);
    execution.material_routes = std::move(material_routes);
    execution.nodes.reserve(execution.plan.nodes.size());

    for (const auto &node : execution.plan.nodes) {
        if (node.kind == FramePlanNodeKind::render) {
            auto found = pass_indices.find(node.name);
            if (found == pass_indices.end()) {
                throw std::runtime_error("Frame plan render node is not compiled: " + node.name);
            }
            execution.nodes.push_back(FrameGraphExecutionNode{node.kind, node.name, found->second, {}});
        } else if (node.kind == FramePlanNodeKind::compute) {
            auto found = task_indices.find(node.name);
            if (found == task_indices.end()) {
                throw std::runtime_error("Frame plan compute node is not compiled: " + node.name);
            }
            execution.nodes.push_back(FrameGraphExecutionNode{node.kind, node.name, found->second, {}});
        } else if (node.kind == FramePlanNodeKind::anchor ||
                   node.kind == FramePlanNodeKind::snapshot_copy ||
                   node.kind == FramePlanNodeKind::output_transform) {
            execution.nodes.push_back(FrameGraphExecutionNode{node.kind, node.name, 0, {}});
        } else {
            throw std::runtime_error("Unsupported frame plan node kind: " + node.name);
        }
    }

    std::unordered_map<std::string, size_t> plan_indices;
    for (size_t i = 0; i < execution.plan.nodes.size(); ++i) {
        plan_indices.emplace(execution.plan.nodes[i].name, i);
    }
    for (const auto &barrier : execution.plan.barriers) {
        if (barrier.kind != "read_after_write") {
            throw std::runtime_error("Unsupported frame plan barrier kind: " + barrier.kind);
        }
        const auto from = plan_indices.find(barrier.from);
        const auto to = plan_indices.find(barrier.to);
        if (from == plan_indices.end() || to == plan_indices.end()) {
            throw std::runtime_error("Frame plan barrier references an unknown node: " + barrier.from +
                                     " -> " + barrier.to);
        }
        if (from->second >= to->second) {
            throw std::runtime_error("Frame plan barrier source does not precede target: " + barrier.from +
                                     " -> " + barrier.to);
        }
        execution.nodes[to->second].incoming_barriers.push_back(CompiledFrameGraphBarrier{
            barrier.resource,
            from->second,
            execution.plan.nodes[from->second].kind,
            execution.plan.nodes[to->second].kind,
        });
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
