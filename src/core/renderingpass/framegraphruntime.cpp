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
        if (selected->definition.materialInfo().output_schema !=
            route.output_schema) {
            throw std::runtime_error(
                "Compiled material route material_outputs "
                "schema mismatch for pass: " +
                route.pass_name);
        }
        if (selected->definition.materialInfo().output_states !=
            route.output_states) {
            throw std::runtime_error(
                "Compiled material route material_output_states "
                "mismatch for pass: " +
                route.pass_name);
        }
        bindings.push_back(CompiledMaterialRouteBinding{
            route.route, selected->pass_id,
            route.pass_contract, route.output_schema,
            route.output_states});
    }
    return bindings;
}

CompiledFrameGraphExecution compileExecution(
    const CompiledRenderingPass &compiled_pass, FramePlan plan,
    std::shared_ptr<const CompiledRenderPipeline> render_pipeline,
    std::shared_ptr<const VulkanTargetPlan> target_plan,
    std::unordered_map<std::string, GlobalRenderTargetId>
        render_target_bindings,
    std::unordered_map<std::string, FrameGraphBufferId>
        buffer_bindings) {
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
    execution.render_target_bindings =
        std::move(render_target_bindings);
    execution.buffer_bindings =
        std::move(buffer_bindings);
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
        if (barrier.kind != "read_after_write" &&
            barrier.kind != "write_after_write") {
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
    const std::shared_ptr<const RendererRuntimeGeneration> &generation) {
    return generation != nullptr ? generation->generation : 0;
}

} // namespace

const CompiledRenderProgram *RendererRuntimeGeneration::find(
    RenderingPassId rendering_pass_id) const noexcept {
    const auto found = programs.find(rendering_pass_id);
    return found != programs.end() ? &found->second : nullptr;
}

PreparedRendererRuntimeGeneration::PreparedRendererRuntimeGeneration(
    std::uint64_t base_generation,
    std::shared_ptr<const RendererRuntimeGeneration> candidate,
    std::vector<RenderingPassId> prepared_rendering_pass_ids)
    : base_generation_{base_generation},
      candidate_{std::move(candidate)},
      prepared_rendering_pass_ids_{
          std::move(prepared_rendering_pass_ids)} {}

FrameGraphRuntimeContainer::FrameGraphRuntimeContainer()
    : publication{
          std::make_shared<RendererRuntimePublication>()} {}

FrameGraphRuntimeContainer::~FrameGraphRuntimeContainer() = default;

PreparedRendererRuntimeGeneration
FrameGraphRuntimeContainer::prepareGeneration(
    std::vector<RenderPipelineProgramPreparation> programs,
    std::optional<std::vector<std::string>>
        enabled_feature_names,
    std::optional<RenderPipelineGpuScopePreparation>
        gpu_scope,
    std::optional<OutputCompileFacts>
        window_output_facts) const {
    if (window_output_facts &&
        window_output_facts->target_kind !=
            OutputTargetKind::window) {
        throw std::runtime_error(
            "renderer window output child requires window compile facts");
    }
    const auto current = snapshot();
    const auto base_generation = generationOf(current);
    if (base_generation ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error(
            "Render pipeline runtime generation is exhausted");
    }

    std::optional<std::string> replaced_owner;
    std::vector<std::shared_ptr<const void>>
        replaced_scope_leases;
    if (gpu_scope && current != nullptr &&
        current->gpu_arena != nullptr) {
        if (const auto *scope =
                current->gpu_arena->findScope(
                    gpu_scope->owner_scope)) {
            replaced_owner = scope->owner_scope;
            replaced_scope_leases =
                scope->resource_leases;
        }
    }

    auto candidate =
        current != nullptr
            ? std::make_shared<RendererRuntimeGeneration>(
                  *current)
            : std::make_shared<RendererRuntimeGeneration>();
    candidate->generation = base_generation + 1;

    std::int64_t next_id = 0;
    for (const auto id : candidate->rendering_pass_ids) {
        next_id = std::max(
            next_id,
            static_cast<std::int64_t>(id.value) + 1);
    }

    std::unordered_map<std::string, RenderingPassId>
        replaced_program_ids;
    std::vector<RenderingPassId> published_order;
    if (replaced_owner) {
        published_order =
            candidate->rendering_pass_ids;
        for (auto found = candidate->programs.begin();
             found != candidate->programs.end();) {
            if (found->second.owner_scope !=
                *replaced_owner) {
                ++found;
                continue;
            }
            replaced_program_ids.emplace(
                found->second.rendering_pass.name,
                found->first);
            candidate->name_to_id.erase(
                found->second.rendering_pass.name);
            std::erase(candidate->rendering_pass_ids,
                       found->first);
            found = candidate->programs.erase(found);
        }

        // A still-published program from another owner may reference
        // resources supplied by the replaced scope. Conservatively retain
        // that old lease until the dependent program is itself replaced.
        for (auto &[id, program] : candidate->programs) {
            (void)id;
            for (const auto &lease :
                 replaced_scope_leases) {
                if (lease == nullptr) continue;
                const auto duplicate = std::find_if(
                    program.resource_leases.begin(),
                    program.resource_leases.end(),
                    [&lease](const auto &existing) {
                        return existing.get() == lease.get();
                    });
                if (duplicate ==
                    program.resource_leases.end()) {
                    program.resource_leases.push_back(
                        lease);
                }
            }
        }
    }

    const auto prepared_owner =
        gpu_scope ? gpu_scope->owner_scope
                  : std::string{};
    candidate->gpu_arena = compileRenderPipelineGpuArena(
        current != nullptr ? current->gpu_arena : nullptr,
        candidate->generation, std::move(gpu_scope));
    if (window_output_facts) {
        const auto output_fingerprint =
            outputCompileFactsFingerprint(
                *window_output_facts);
        candidate->window_output =
            std::make_shared<WindowOutputGeneration>(
                WindowOutputGeneration{
                    .generation = candidate->generation,
                    .compile_facts =
                        std::move(*window_output_facts),
                    .compile_fingerprint =
                        output_fingerprint,
                    .target_resource_lease =
                        candidate->gpu_arena,
                });
    }
    if (enabled_feature_names) {
        candidate->enabled_feature_names =
            std::move(*enabled_feature_names);
    }

    std::unordered_set<std::string> prepared_names;
    std::vector<RenderingPassId> prepared_ids;
    prepared_ids.reserve(programs.size());
    for (auto &program : programs) {
        if (program.owner_scope.empty()) {
            program.owner_scope = prepared_owner;
        }
        if (!prepared_owner.empty() &&
            program.owner_scope != prepared_owner) {
            throw std::runtime_error(
                "Prepared render program owner does not match GPU scope");
        }
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
            const auto *published =
                candidate->find(rendering_pass_id);
            if (published == nullptr) {
                throw std::runtime_error(
                    "Published render pipeline name table is inconsistent: " +
                    name);
            }
            if (!published->owner_scope.empty() &&
                !program.owner_scope.empty() &&
                published->owner_scope !=
                    program.owner_scope) {
                throw std::runtime_error(
                    "Prepared render pipeline cannot overwrite another owner: " +
                    name);
            }
            if (program.rendering_pass_id &&
                *program.rendering_pass_id !=
                    rendering_pass_id) {
                throw std::runtime_error(
                    "Prepared rendering pass id changed for existing pass: " +
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
            } else if (const auto replaced =
                           replaced_program_ids.find(name);
                       replaced !=
                       replaced_program_ids.end()) {
                rendering_pass_id = replaced->second;
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
            std::move(program.target_plan),
            std::move(program.render_target_bindings),
            std::move(program.buffer_bindings));
        candidate->programs.insert_or_assign(
            rendering_pass_id,
            CompiledRenderProgram{
                rendering_pass_id,
                std::move(program.owner_scope),
                std::move(program.rendering_pass),
                std::move(execution),
                {},
            });
        prepared_ids.push_back(rendering_pass_id);
    }

    if (replaced_owner) {
        std::vector<RenderingPassId> stable_order;
        stable_order.reserve(
            candidate->rendering_pass_ids.size());
        const auto append_if_live =
            [&candidate, &stable_order](
                RenderingPassId id) {
                if (candidate->programs.contains(id) &&
                    std::find(stable_order.begin(),
                              stable_order.end(),
                              id) ==
                        stable_order.end()) {
                    stable_order.push_back(id);
                }
            };
        for (const auto id : published_order) {
            append_if_live(id);
        }
        for (const auto id :
             candidate->rendering_pass_ids) {
            append_if_live(id);
        }
        candidate->rendering_pass_ids =
            std::move(stable_order);
    }

    return PreparedRendererRuntimeGeneration{
        base_generation, std::move(candidate),
        std::move(prepared_ids)};
}

void FrameGraphRuntimeContainer::publishPreparedGeneration(
    PreparedRendererRuntimeGeneration &&prepared) {
    if (!prepared.valid()) {
        throw std::runtime_error(
            "Renderer runtime candidate is empty");
    }
    auto expected =
        publication->active_generation.load(
            std::memory_order_acquire);
    if (generationOf(expected) != prepared.base_generation_) {
        throw std::runtime_error(
            "Renderer runtime candidate is stale");
    }
    if (!publication->active_generation
             .compare_exchange_strong(
                 expected, prepared.candidate_,
                 std::memory_order_acq_rel,
                 std::memory_order_acquire)) {
        throw std::runtime_error(
            "Renderer runtime candidate lost publication race");
    }
    prepared.candidate_.reset();
    prepared.prepared_rendering_pass_ids_.clear();
}

void FrameGraphRuntimeContainer::rollbackPreparedGeneration(
    PreparedRendererRuntimeGeneration &prepared) const noexcept {
    prepared.candidate_.reset();
    prepared.prepared_rendering_pass_ids_.clear();
}

std::shared_ptr<const RendererRuntimeGeneration>
FrameGraphRuntimeContainer::snapshot() const noexcept {
    return publication->active_generation.load(
        std::memory_order_acquire);
}

std::shared_ptr<const RendererRuntimePublication>
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
            .rendering_pass = compiled_pass,
            .frame_plan = std::move(plan),
            .render_pipeline = std::move(render_pipeline),
            .target_plan = std::move(target_plan),
            .rendering_pass_id = rendering_pass_id,
        }});
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
