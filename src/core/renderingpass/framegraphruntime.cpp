#include "framegraphruntime.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
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

CompiledFrameGraphExecution compileExecution(
    const CompiledRenderingPass &compiled_pass, FramePlan plan,
    std::shared_ptr<const CompiledRenderPipeline> render_pipeline,
    std::shared_ptr<const VulkanTargetPlan> target_plan) {
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
    execution.target_plan = std::move(target_plan);
    if (execution.target_plan != nullptr &&
        execution.target_plan->sample_count_plan) {
        execution.sample_count_plan =
            std::shared_ptr<const ResolvedSampleCountPlan>(
                execution.target_plan,
                &*execution.target_plan->sample_count_plan);
    }
    execution.material_routes = std::move(material_routes);
    execution.nodes.reserve(execution.plan.nodes.size());

    for (const auto &node : execution.plan.nodes) {
        if (node.kind == FramePlanNodeKind::render) {
            auto found = pass_indices.find(node.name);
            if (found == pass_indices.end()) {
                throw std::runtime_error(
                    "Frame plan render node is not compiled: " +
                    node.name);
            }
            execution.nodes.push_back(FrameGraphExecutionNode{
                node.kind, node.name, found->second, {}});
        } else if (node.kind == FramePlanNodeKind::compute) {
            auto found = task_indices.find(node.name);
            if (found == task_indices.end()) {
                throw std::runtime_error(
                    "Frame plan compute node is not compiled: " +
                    node.name);
            }
            execution.nodes.push_back(FrameGraphExecutionNode{
                node.kind, node.name, found->second, {}});
        } else if (node.kind == FramePlanNodeKind::anchor ||
                   node.kind == FramePlanNodeKind::snapshot_copy ||
                   node.kind == FramePlanNodeKind::output_transform) {
            execution.nodes.push_back(FrameGraphExecutionNode{
                node.kind, node.name, 0, {}});
        } else {
            throw std::runtime_error(
                "Unsupported frame plan node kind: " + node.name);
        }
    }

    std::unordered_map<std::string, size_t> plan_indices;
    for (size_t i = 0; i < execution.plan.nodes.size(); ++i) {
        plan_indices.emplace(execution.plan.nodes[i].name, i);
    }
    for (const auto &barrier : execution.plan.barriers) {
        if (barrier.kind != "read_after_write") {
            throw std::runtime_error(
                "Unsupported frame plan barrier kind: " +
                barrier.kind);
        }
        const auto from = plan_indices.find(barrier.from);
        const auto to = plan_indices.find(barrier.to);
        if (from == plan_indices.end() ||
            to == plan_indices.end()) {
            throw std::runtime_error(
                "Frame plan barrier references an unknown node: " +
                barrier.from + " -> " + barrier.to);
        }
        if (from->second >= to->second) {
            throw std::runtime_error(
                "Frame plan barrier source does not precede target: " +
                barrier.from + " -> " + barrier.to);
        }
        execution.nodes[to->second].incoming_barriers.push_back(
            CompiledFrameGraphBarrier{
                barrier.resource,
                from->second,
                execution.plan.nodes[from->second].kind,
                execution.plan.nodes[to->second].kind,
            });
    }
    return execution;
}

std::uint64_t generationOf(
    const std::shared_ptr<const RenderPipelineRuntimeGeneration> &generation) {
    return generation != nullptr ? generation->generation : 0;
}

} // namespace

const CompiledRenderProgram *RenderPipelineRuntimeGeneration::find(
    RenderingPassId rendering_pass_id) const noexcept {
    const auto found = programs.find(rendering_pass_id);
    return found != programs.end() ? &found->second : nullptr;
}

PreparedRenderPipelineGeneration::PreparedRenderPipelineGeneration(
    std::uint64_t base_generation,
    std::shared_ptr<const RenderPipelineRuntimeGeneration> candidate,
    std::vector<RenderingPassId> prepared_rendering_pass_ids)
    : base_generation_{base_generation},
      candidate_{std::move(candidate)},
      prepared_rendering_pass_ids_{
          std::move(prepared_rendering_pass_ids)} {}

FrameGraphRuntimeContainer::FrameGraphRuntimeContainer()
    : publication{
          std::make_shared<RenderPipelineRuntimePublication>()} {}

FrameGraphRuntimeContainer::~FrameGraphRuntimeContainer() = default;

PreparedRenderPipelineGeneration
FrameGraphRuntimeContainer::prepareGeneration(
    std::vector<RenderPipelineProgramPreparation> programs,
    std::optional<std::vector<std::string>>
        enabled_feature_names) const {
    const auto current = snapshot();
    const auto base_generation = generationOf(current);
    if (base_generation ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error(
            "Render pipeline runtime generation is exhausted");
    }

    auto candidate =
        current != nullptr
            ? std::make_shared<RenderPipelineRuntimeGeneration>(
                  *current)
            : std::make_shared<RenderPipelineRuntimeGeneration>();
    candidate->generation = base_generation + 1;
    if (enabled_feature_names) {
        candidate->enabled_feature_names =
            std::move(*enabled_feature_names);
    }

    std::int64_t next_id = 0;
    for (const auto id : candidate->rendering_pass_ids) {
        next_id = std::max(
            next_id,
            static_cast<std::int64_t>(id.value) + 1);
    }

    std::unordered_set<std::string> prepared_names;
    std::vector<RenderingPassId> prepared_ids;
    prepared_ids.reserve(programs.size());
    for (auto &program : programs) {
        const auto &name = program.rendering_pass.name;
        if (name.empty()) {
            throw std::runtime_error(
                "Prepared rendering pass name must not be empty");
        }
        if (!prepared_names.emplace(name).second) {
            throw std::runtime_error(
                "Prepared render pipeline contains duplicate pass: " +
                name);
        }

        RenderingPassId rendering_pass_id;
        const auto existing = candidate->name_to_id.find(name);
        if (existing != candidate->name_to_id.end()) {
            rendering_pass_id = existing->second;
            if (program.rendering_pass_id &&
                *program.rendering_pass_id !=
                    rendering_pass_id) {
                throw std::runtime_error(
                    "Prepared rendering pass id changed for existing pass: " +
                    name);
            }
            if (candidate->find(rendering_pass_id) == nullptr) {
                throw std::runtime_error(
                    "Published render pipeline name table is inconsistent: " +
                    name);
            }
        } else {
            if (program.rendering_pass_id) {
                rendering_pass_id =
                    *program.rendering_pass_id;
                if (!isValidRenderingPassId(
                        rendering_pass_id) ||
                    candidate->programs.contains(
                        rendering_pass_id)) {
                    throw std::runtime_error(
                        "Prepared rendering pass id is invalid or already used: " +
                        name);
                }
                next_id = std::max(
                    next_id,
                    static_cast<std::int64_t>(
                        rendering_pass_id.value) +
                        1);
            } else {
                if (next_id >
                    std::numeric_limits<int>::max()) {
                    throw std::runtime_error(
                        "Rendering pass id table is exhausted");
                }
                rendering_pass_id =
                    RenderingPassId{
                        static_cast<int>(next_id++)};
            }
            candidate->name_to_id.emplace(name,
                                          rendering_pass_id);
            candidate->rendering_pass_ids.push_back(
                rendering_pass_id);
        }

        auto execution = compileExecution(
            program.rendering_pass,
            std::move(program.frame_plan),
            std::move(program.render_pipeline),
            std::move(program.target_plan));
        candidate->programs.insert_or_assign(
            rendering_pass_id,
            CompiledRenderProgram{
                rendering_pass_id,
                std::move(program.rendering_pass),
                std::move(execution),
            });
        prepared_ids.push_back(rendering_pass_id);
    }

    return PreparedRenderPipelineGeneration{
        base_generation, std::move(candidate),
        std::move(prepared_ids)};
}

void FrameGraphRuntimeContainer::publishPreparedGeneration(
    PreparedRenderPipelineGeneration &&prepared) {
    if (!prepared.valid()) {
        throw std::runtime_error(
            "Render pipeline runtime candidate is empty");
    }
    auto expected =
        publication->active_generation.load(
            std::memory_order_acquire);
    if (generationOf(expected) != prepared.base_generation_) {
        throw std::runtime_error(
            "Render pipeline runtime candidate is stale");
    }
    if (!publication->active_generation
             .compare_exchange_strong(
                 expected, prepared.candidate_,
                 std::memory_order_acq_rel,
                 std::memory_order_acquire)) {
        throw std::runtime_error(
            "Render pipeline runtime candidate lost publication race");
    }
    prepared.candidate_.reset();
    prepared.prepared_rendering_pass_ids_.clear();
}

void FrameGraphRuntimeContainer::rollbackPreparedGeneration(
    PreparedRenderPipelineGeneration &prepared) const noexcept {
    prepared.candidate_.reset();
    prepared.prepared_rendering_pass_ids_.clear();
}

std::shared_ptr<const RenderPipelineRuntimeGeneration>
FrameGraphRuntimeContainer::snapshot() const noexcept {
    return publication->active_generation.load(
        std::memory_order_acquire);
}

std::shared_ptr<const RenderPipelineRuntimePublication>
FrameGraphRuntimeContainer::publicationState() const noexcept {
    return publication;
}

std::uint64_t
FrameGraphRuntimeContainer::activeGeneration() const noexcept {
    return generationOf(snapshot());
}

std::shared_ptr<const CompiledRenderProgram>
FrameGraphRuntimeContainer::findProgram(
    RenderingPassId rendering_pass_id) const {
    const auto generation = snapshot();
    const auto *program =
        generation != nullptr
            ? generation->find(rendering_pass_id)
            : nullptr;
    return program != nullptr
               ? std::shared_ptr<const CompiledRenderProgram>{
                     generation, program}
               : nullptr;
}

void FrameGraphRuntimeContainer::registerExecutionPlan(RenderingPassId rendering_pass_id,
                                                       const CompiledRenderingPass &compiled_pass,
                                                       FramePlan plan,
                                                       std::shared_ptr<const CompiledRenderPipeline>
                                                           render_pipeline,
                                                       std::shared_ptr<const VulkanTargetPlan>
                                                           target_plan) {
    auto prepared = prepareGeneration(
        {RenderPipelineProgramPreparation{
            compiled_pass, std::move(plan),
            std::move(render_pipeline), std::move(target_plan),
            rendering_pass_id}});
    if (prepared.renderingPassIds().size() != 1 ||
        prepared.renderingPassIds().front() != rendering_pass_id) {
        throw std::runtime_error(
            "Compatibility execution registration produced a different id");
    }
    publishPreparedGeneration(std::move(prepared));
}

std::shared_ptr<const CompiledFrameGraphExecution>
FrameGraphRuntimeContainer::find(
    RenderingPassId rendering_pass_id) const {
    auto program = findProgram(rendering_pass_id);
    if (program == nullptr) return nullptr;
    const auto *frame_graph = &program->frame_graph;
    return std::shared_ptr<const CompiledFrameGraphExecution>{
        std::move(program), frame_graph};
}

} // namespace Pelican
