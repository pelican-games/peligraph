#include "coretemplate.hpp"

#include "../../../container.hpp"
#include "../../../ecs/componentinfo.hpp"
#include "../../../job_system.hpp"
#include "../../../launchconfig.hpp"
#include "../../../log.hpp"
#include "../../../profiler.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace internal {
size_t getIndexFromComponentId_Ref(ComponentId id) {
    return GET_MODULE(ComponentInfoManager).getIndexFromComponentId(id);
}

namespace {

using DependencyMap = std::unordered_map<SystemId, std::set<SystemId>>;

std::vector<std::vector<SystemId>> makeExecutionLevels(
    const std::vector<ECSSystemGraphNode> &nodes, const DependencyMap &dependencies) {
    std::unordered_map<SystemId, size_t> in_degree;
    std::unordered_map<SystemId, std::vector<SystemId>> depended_by;
    std::set<SystemId> zero_degree;
    for (const auto &node : nodes) {
        const auto &node_dependencies = dependencies.at(node.id);
        in_degree.emplace(node.id, node_dependencies.size());
        if (node_dependencies.empty()) {
            zero_degree.insert(node.id);
        }
        for (const auto dependency : node_dependencies) {
            depended_by[dependency].push_back(node.id);
        }
    }
    for (auto &[dependency, dependents] : depended_by) {
        (void)dependency;
        std::sort(dependents.begin(), dependents.end());
    }

    std::vector<std::vector<SystemId>> levels;
    size_t executed_count = 0;
    while (!zero_degree.empty()) {
        std::vector<SystemId> level{zero_degree.begin(), zero_degree.end()};
        zero_degree.clear();
        executed_count += level.size();
        levels.push_back(level);
        for (const auto executed : level) {
            const auto found = depended_by.find(executed);
            if (found == depended_by.end()) {
                continue;
            }
            for (const auto dependent : found->second) {
                auto &degree = in_degree.at(dependent);
                if (--degree == 0) {
                    zero_degree.insert(dependent);
                }
            }
        }
    }

    if (executed_count != nodes.size()) {
        std::ostringstream message;
        message << "ECS dependency cycle detected; unexecuted systems:";
        for (const auto &node : nodes) {
            if (in_degree.at(node.id) != 0) {
                message << " '" << node.name << "'";
            }
        }
        throw std::runtime_error(message.str());
    }
    return levels;
}

bool isOrderedBefore(SystemId before, SystemId after, const DependencyMap &dependencies) {
    std::vector<SystemId> pending{after};
    std::unordered_set<SystemId> visited;
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second) {
            continue;
        }
        for (const auto dependency : dependencies.at(current)) {
            if (dependency == before) {
                return true;
            }
            pending.push_back(dependency);
        }
    }
    return false;
}

std::vector<std::string> conflictingComponents(const ECSSystemGraphNode &left,
                                               const ECSSystemGraphNode &right) {
    std::map<size_t, std::string> conflicts;
    for (const auto &left_access : left.component_accesses) {
        for (const auto &right_access : right.component_accesses) {
            if (left_access.component_index == right_access.component_index &&
                (left_access.writes || right_access.writes)) {
                conflicts.try_emplace(left_access.component_index, left_access.component_name);
            }
        }
    }
    std::vector<std::string> result;
    result.reserve(conflicts.size());
    for (const auto &[index, name] : conflicts) {
        (void)index;
        result.push_back(name);
    }
    return result;
}

std::string hazardMessage(const ECSSystemGraphNode &left, const ECSSystemGraphNode &right,
                          const std::vector<std::string> &components) {
    std::ostringstream message;
    message << "ECS unordered component hazard between systems '" << left.name << "' and '"
            << right.name << "' on component" << (components.size() == 1 ? " " : "s ");
    for (size_t i = 0; i < components.size(); ++i) {
        if (i != 0) {
            message << ", ";
        }
        message << "'" << components[i] << "'";
    }
    return message.str();
}

} // namespace

ECSExecutionPlan buildECSExecutionPlan(std::span<const ECSSystemGraphNode> input_nodes,
                                       ECSHazardPolicy hazard_policy) {
    std::vector<ECSSystemGraphNode> nodes{input_nodes.begin(), input_nodes.end()};
    std::sort(nodes.begin(), nodes.end(),
              [](const ECSSystemGraphNode &left, const ECSSystemGraphNode &right) {
                  return left.id < right.id;
              });

    std::unordered_map<SystemId, const ECSSystemGraphNode *> nodes_by_id;
    DependencyMap dependencies;
    for (const auto &node : nodes) {
        if (!nodes_by_id.emplace(node.id, &node).second) {
            throw std::runtime_error("ECS execution graph contains duplicate system id " +
                                     std::to_string(node.id));
        }
        dependencies.emplace(node.id, std::set<SystemId>{});
    }
    for (const auto &node : nodes) {
        auto &node_dependencies = dependencies.at(node.id);
        for (const auto dependency : node.dependencies) {
            const auto found = nodes_by_id.find(dependency);
            if (found == nodes_by_id.end()) {
                throw std::runtime_error("ECS system '" + node.name +
                                         "' depends on missing system id " +
                                         std::to_string(dependency) +
                                         "; system would be unexecuted");
            }
            if (!node_dependencies.insert(dependency).second) {
                throw std::runtime_error("ECS system '" + node.name +
                                         "' declares dependency on system '" +
                                         found->second->name +
                                         "' more than once; system would be unexecuted");
            }
        }
    }

    // Validate the declared graph before using reachability to inspect hazards.
    (void)makeExecutionLevels(nodes, dependencies);

    ECSExecutionPlan result;
    std::vector<std::string> strict_hazards;
    for (size_t left_index = 0; left_index < nodes.size(); ++left_index) {
        for (size_t right_index = left_index + 1; right_index < nodes.size(); ++right_index) {
            const auto &left = nodes[left_index];
            const auto &right = nodes[right_index];
            const auto components = conflictingComponents(left, right);
            if (components.empty() || isOrderedBefore(left.id, right.id, dependencies) ||
                isOrderedBefore(right.id, left.id, dependencies)) {
                continue;
            }

            const auto message = hazardMessage(left, right, components);
            if (hazard_policy == ECSHazardPolicy::strict) {
                strict_hazards.push_back(message);
                continue;
            }

            // System IDs are monotonic registration IDs, so left-before-right is the
            // required registration-order migration behavior.
            dependencies.at(right.id).insert(left.id);
            result.automatic_serializations.push_back(ECSAutomaticSerialization{
                .before = left.id,
                .after = right.id,
                .before_name = left.name,
                .after_name = right.name,
                .component_names = components,
            });
        }
    }

    if (!strict_hazards.empty()) {
        std::ostringstream message;
        for (size_t i = 0; i < strict_hazards.size(); ++i) {
            if (i != 0) {
                message << "; ";
            }
            message << strict_hazards[i];
        }
        message << "; add dependency edges or use automatic serialization";
        throw std::runtime_error(message.str());
    }

    result.levels = makeExecutionLevels(nodes, dependencies);
    return result;
}
} // namespace internal

ECSCoreTemplatePublic::MutationScope::MutationScope(ECSCoreTemplatePublic &value) : owner{value} {
    if (owner.mutation_active) {
        throw std::logic_error("ECS structural mutation is not reentrant");
    }
    owner.mutation_active = true;
}

ECSCoreTemplatePublic::MutationScope::~MutationScope() {
    owner.mutation_active = false;
}

std::optional<ECSCoreTemplatePublic::EntityRef> ECSCoreTemplatePublic::resolve(EntityId id) const noexcept {
    if (id.index == std::numeric_limits<std::uint32_t>::max() || id.index >= id_table.size()) {
        return std::nullopt;
    }
    const auto &entry = id_table[id.index];
    if (!entry.live || entry.generation != id.generation || !entry.ref.has_value()) {
        return std::nullopt;
    }
    const auto ref = *entry.ref;
    if (ref.chunk_index >= chunks_storage.size() || ref.array_index >= chunks_storage[ref.chunk_index].size()) {
        return std::nullopt;
    }
    return ref;
}

void ECSCoreTemplatePublic::updateSystemChunkCache(ChunkIndex chunk_index) {
    auto &chunk = chunks_storage[chunk_index];
    for (auto &[id, system] : systems) {
        (void)id;
        if ((chunk.getMask() & system.matching_mask) == system.matching_mask) {
            system.matching_chunk_indices.push_back(chunk_index);
        }
    }
}

void ECSCoreTemplatePublic::rebuildChunkCaches() {
    archetype_to_chunks.clear();
    for (auto &[id, system] : systems) {
        (void)id;
        system.matching_chunk_indices.clear();
    }
    for (ChunkIndex chunk_index = 0; chunk_index < chunks_storage.size(); ++chunk_index) {
        auto key = std::vector<ComponentId>{chunks_storage[chunk_index].getComponentList().begin(),
                                            chunks_storage[chunk_index].getComponentList().end()};
        std::sort(key.begin(), key.end());
        archetype_to_chunks[key].push_back(chunk_index);
        updateSystemChunkCache(chunk_index);
    }
}

void ECSCoreTemplatePublic::releaseId(EntityId id) noexcept {
    auto &entry = id_table[id.index];
    entry.ref.reset();
    entry.live = false;
    if (entry.generation == std::numeric_limits<std::uint32_t>::max()) {
        return;
    }
    ++entry.generation;
    free_indices.push_back(id.index);
}

void ECSCoreTemplatePublic::validateFreshIndexCapacityForTesting(size_t id_table_size) {
    if (id_table_size >= std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("ECS EntityId index capacity exhausted");
    }
}

std::vector<EntityId> ECSCoreTemplatePublic::createEntities(std::span<const ComponentId> component_ids,
                                                            size_t entity_count,
                                                            const PopulateBatch &populate) {
    MutationScope mutation{*this};
    if (entity_count == 0) {
        return {};
    }
    if (component_ids.size() >= MAX_COMPONENTS) {
        throw std::length_error("ECS entity has too many component types");
    }

    std::vector<ComponentId> component_ids_ex;
    component_ids_ex.reserve(component_ids.size() + 1);
    component_ids_ex.push_back(ComponentIdByType<EntityId>::value);
    component_ids_ex.insert(component_ids_ex.end(), component_ids.begin(), component_ids.end());
    auto duplicate_check = component_ids_ex;
    std::sort(duplicate_check.begin(), duplicate_check.end());
    if (std::adjacent_find(duplicate_check.begin(), duplicate_check.end()) != duplicate_check.end()) {
        throw std::invalid_argument("ECS entity component list contains a duplicate");
    }

    auto &manager = GET_MODULE(ComponentInfoManager);
    std::vector<size_t> component_indices_ex;
    component_indices_ex.reserve(component_ids_ex.size());
    for (const auto id : component_ids_ex) {
        const auto index = manager.getIndexFromComponentId(id);
        (void)manager.getFromIndex(index);
        component_indices_ex.push_back(index);
    }

    const auto reusable_count = std::min(entity_count, free_indices.size());
    const auto fresh_count = entity_count - reusable_count;
    if (fresh_count > std::numeric_limits<std::uint32_t>::max() - id_table.size()) {
        throw std::length_error("ECS EntityId index capacity exhausted");
    }
    const auto final_id_table_size = id_table.size() + fresh_count;
    id_table.reserve(final_id_table_size);
    free_indices.reserve(final_id_table_size);

    struct Allocation {
        ChunkIndex chunk_index;
        size_t first;
        size_t count;
        std::vector<EntityId> ids;
        std::vector<void *> user_component_ptrs;
    };
    struct Initialized {
        void (*deinit)(void *) noexcept;
        void *ptr;
    };

    const auto original_chunk_count = chunks_storage.size();
    const auto original_id_table = id_table;
    const auto original_free_indices = free_indices;
    std::vector<Allocation> allocations;
    std::vector<Initialized> initialized;
    std::vector<EntityId> result;
    result.reserve(entity_count);

    try {
        size_t remaining = entity_count;
        while (remaining != 0) {
            ChunkIndex chunk_index = std::numeric_limits<ChunkIndex>::max();
            const auto found = archetype_to_chunks.find(duplicate_check);
            if (found != archetype_to_chunks.end()) {
                for (const auto candidate : found->second) {
                    if (chunks_storage[candidate].remainingCapacity() != 0) {
                        chunk_index = candidate;
                        break;
                    }
                }
            }
            if (chunk_index == std::numeric_limits<ChunkIndex>::max()) {
                chunk_index = chunks_storage.size();
                chunks_storage.emplace_back(component_indices_ex, component_ids_ex);
                archetype_to_chunks[duplicate_check].push_back(chunk_index);
                updateSystemChunkCache(chunk_index);
            }

            auto &chunk = chunks_storage[chunk_index];
            const auto batch_count = std::min(remaining, chunk.remainingCapacity());
            const auto first = chunk.size();
            std::vector<void *> component_ptrs_ex(component_indices_ex.size());
            allocations.push_back(Allocation{
                .chunk_index = chunk_index,
                .first = first,
                .count = 0,
                .ids = {},
                .user_component_ptrs = std::vector<void *>(component_ids.size()),
            });
            auto &allocation = allocations.back();
            allocation.ids.reserve(batch_count);
            chunk.allocate(component_indices_ex, component_ptrs_ex, batch_count);
            allocation.count = batch_count;
            std::copy(component_ptrs_ex.begin() + 1, component_ptrs_ex.end(),
                      allocation.user_component_ptrs.begin());
            auto *entity_components = static_cast<EntityId *>(component_ptrs_ex[0]);

            for (size_t i = 0; i < batch_count; ++i) {
                std::uint32_t index = 0;
                if (!free_indices.empty()) {
                    index = free_indices.back();
                    free_indices.pop_back();
                } else {
                    validateFreshIndexCapacityForTesting(id_table.size());
                    index = static_cast<std::uint32_t>(id_table.size());
                    id_table.emplace_back();
                }
                auto &entry = id_table[index];
                entry.ref = EntityRef{chunk_index, first + i};
                entry.live = false;
                const EntityId id{index, entry.generation};
                entity_components[i] = id;
                allocation.ids.push_back(id);
                result.push_back(id);
            }
            remaining -= batch_count;
        }

        if (populate) {
            for (auto &allocation : allocations) {
                populate(allocation.ids, allocation.user_component_ptrs, allocation.count);
            }
        }

        if (component_indices_ex.size() > std::numeric_limits<size_t>::max() / entity_count) {
            throw std::length_error("ECS initialization tracking capacity exceeded");
        }
        initialized.reserve(component_indices_ex.size() * entity_count);
        for (const auto &allocation : allocations) {
            auto &chunk = chunks_storage[allocation.chunk_index];
            for (size_t offset = 0; offset < allocation.count; ++offset) {
                for (const auto component_index : chunk.getIndices()) {
                    const auto &info = manager.getFromIndex(component_index);
                    if (info.cb_init == nullptr) {
                        continue;
                    }
                    auto *ptr = chunk.at(component_index, allocation.first + offset);
                    info.cb_init(ptr);
                    initialized.push_back(Initialized{info.cb_deinit, ptr});
                }
            }
        }

        for (const auto id : result) {
            id_table[id.index].live = true;
        }
        for (const auto &allocation : allocations) {
            auto &chunk = chunks_storage[allocation.chunk_index];
            for (const auto component_index : chunk.getIndices()) {
                chunk.updateVersion(component_index, global_tick);
            }
        }
        return result;
    } catch (...) {
        for (auto it = initialized.rbegin(); it != initialized.rend(); ++it) {
            if (it->deinit != nullptr) {
                it->deinit(it->ptr);
            }
        }
        for (auto it = allocations.rbegin(); it != allocations.rend(); ++it) {
            chunks_storage[it->chunk_index].rollbackTail(it->count);
        }
        if (chunks_storage.size() > original_chunk_count) {
            chunks_storage.erase(chunks_storage.begin() + static_cast<std::ptrdiff_t>(original_chunk_count),
                                 chunks_storage.end());
        }
        id_table = original_id_table;
        free_indices = original_free_indices;
        rebuildChunkCaches();
        throw;
    }
}

EntityId ECSCoreTemplatePublic::createEntity(
    std::span<const ComponentId> component_ids,
    const std::function<void(std::span<void *>)> &populate) {
    auto ids = createEntities(component_ids, 1, [&](std::span<const EntityId>, std::span<void *> ptrs, size_t) {
        if (populate) {
            populate(ptrs);
        }
    });
    return ids.front();
}

bool ECSCoreTemplatePublic::remove(EntityId id) {
    MutationScope mutation{*this};
    const auto resolved = resolve(id);
    if (!resolved.has_value()) {
        return false;
    }
    const auto ref = *resolved;
    auto &chunk = chunks_storage[ref.chunk_index];
    const auto entity_index = GET_MODULE(ComponentInfoManager).getIndexFromComponentId(
        ComponentIdByType<EntityId>::value);
    const auto moved_id = *static_cast<EntityId *>(chunk.at(entity_index, chunk.size() - 1));
    chunk.removeAt(ref.array_index);
    if (moved_id != id) {
        id_table[moved_id.index].ref = EntityRef{ref.chunk_index, ref.array_index};
    }
    releaseId(id);
    return true;
}

void ECSCoreTemplatePublic::removeOrThrow(EntityId id) {
    if (!remove(id)) {
        throw std::runtime_error("ECS entity is not live: " + toString(id));
    }
}

void ECSCoreTemplatePublic::clearEntities() {
    MutationScope mutation{*this};
    for (auto &chunk : chunks_storage) {
        chunk.clear();
    }
    chunks_storage.clear();
    archetype_to_chunks.clear();
    for (auto &[id, system] : systems) {
        (void)id;
        system.matching_chunk_indices.clear();
        system.last_run_tick = 0;
    }
    for (std::uint32_t index = 0; index < id_table.size(); ++index) {
        auto &entry = id_table[index];
        if (entry.live) {
            releaseId(EntityId{index, entry.generation});
        }
    }
}

size_t ECSCoreTemplatePublic::liveCount() const noexcept {
    return static_cast<size_t>(std::count_if(id_table.begin(), id_table.end(), [](const IdEntry &entry) {
        return entry.live;
    }));
}

EntityId ECSCoreTemplatePublic::forceGenerationForTesting(EntityId id, std::uint32_t generation) {
    const auto ref = resolve(id);
    if (!ref.has_value()) {
        throw std::runtime_error("Cannot change generation of a stale entity: " + toString(id));
    }
    auto &entry = id_table[id.index];
    entry.generation = generation;
    const EntityId replacement{id.index, generation};
    const auto entity_index = GET_MODULE(ComponentInfoManager).getIndexFromComponentId(
        ComponentIdByType<EntityId>::value);
    *static_cast<EntityId *>(chunks_storage[ref->chunk_index].at(entity_index, ref->array_index)) = replacement;
    return replacement;
}

void *ECSCoreTemplatePublic::tryComponentRaw(EntityId id, ComponentId component_id) {
    const auto ref = resolve(id);
    if (!ref.has_value()) {
        return nullptr;
    }
    const auto component_index = GET_MODULE(ComponentInfoManager).getIndexFromComponentId(component_id);
    auto &chunk = chunks_storage[ref->chunk_index];
    return chunk.has(component_index) ? chunk.at(component_index, ref->array_index) : nullptr;
}

void *ECSCoreTemplatePublic::componentRaw(EntityId id, ComponentId component_id) {
    if (auto *component = tryComponentRaw(id, component_id)) {
        return component;
    }
    std::string component_name = std::to_string(component_id);
    try {
        component_name = GET_MODULE(ComponentInfoManager).get(component_id).name;
    } catch (...) {
    }
    throw std::runtime_error("ECS component '" + component_name + "' not found on entity " + toString(id));
}

bool ECSCoreTemplatePublic::markComponentChanged(EntityId id, ComponentId component_id) {
    const auto ref = resolve(id);
    if (!ref.has_value()) {
        return false;
    }
    const auto component_index = GET_MODULE(ComponentInfoManager).getIndexFromComponentId(component_id);
    auto &chunk = chunks_storage[ref->chunk_index];
    if (!chunk.has(component_index)) {
        return false;
    }
    chunk.updateVersion(component_index, global_tick);
    return true;
}

void ECSCoreTemplatePublic::unregisterSystem(SystemId system_id) {
    for (const auto depends : systems.at(system_id).depends_list) {
        systems.at(depends).depended_by.erase(system_id);
    }
    systems.erase(system_id);
    execution_plan_dirty = true;
}

void ECSCoreTemplatePublic::update() {
    ++global_tick;

    TimeProfilerStart("ECS_Update_Sort");
    const auto hazard_policy = [] {
        const auto *launch_config = FastModuleContainer::tryGet<EngineLaunchConfig>();
        // --strict-assets is the existing startup-wide strict/determinism gate.
        // Reusing it keeps WP148 inside the scheduler-only change boundary.
        return launch_config != nullptr && launch_config->strict_assets
                   ? internal::ECSHazardPolicy::strict
                   : internal::ECSHazardPolicy::automatic_serialization;
    }();
    try {
        if (execution_plan_dirty || cached_hazard_policy != hazard_policy) {
            std::vector<internal::ECSSystemGraphNode> graph_nodes;
            graph_nodes.reserve(systems.size());
            const auto &component_infos = GET_MODULE(ComponentInfoManager);
            for (const auto &[id, system] : systems) {
                internal::ECSSystemGraphNode node{
                    .id = id,
                    .name = system.name,
                    .dependencies = system.depends_list,
                    .component_accesses = {},
                };
                node.component_accesses.reserve(system.read_indices.size() +
                                                 system.write_indices.size());
                for (const auto component_index : system.read_indices) {
                    node.component_accesses.push_back({
                        .component_index = component_index,
                        .component_name = component_infos.getFromIndex(component_index).name,
                        .writes = false,
                    });
                }
                for (const auto component_index : system.write_indices) {
                    node.component_accesses.push_back({
                        .component_index = component_index,
                        .component_name = component_infos.getFromIndex(component_index).name,
                        .writes = true,
                    });
                }
                graph_nodes.push_back(std::move(node));
            }

            auto plan = internal::buildECSExecutionPlan(graph_nodes, hazard_policy);
            execution_levels = std::move(plan.levels);
            cached_hazard_policy = hazard_policy;
            execution_plan_dirty = false;
            if (logger != nullptr) {
                for (const auto &serialization : plan.automatic_serializations) {
                    std::ostringstream components;
                    for (size_t i = 0; i < serialization.component_names.size(); ++i) {
                        if (i != 0) {
                            components << ", ";
                        }
                        components << serialization.component_names[i];
                    }
                    LOG_WARNING(logger,
                                "ECS auto serialization: '{}' before '{}' for component(s) {}; "
                                "add an explicit dependency edge (strict mode rejects this hazard)",
                                serialization.before_name, serialization.after_name,
                                components.str());
                }
            }
        }
    } catch (...) {
        TimeProfilerEnd("ECS_Update_Sort");
        throw;
    }
    TimeProfilerEnd("ECS_Update_Sort");

    JobSystem::Get().init();
    TimeProfilerStart("ECS_Update_Execution");
    for (const auto &level : execution_levels) {
        for (const auto system_id : level) {
            auto &system = systems.at(system_id);
            if (system.prepare_func) {
                system.prepare_func(*this, system.system_ref, system.matching_chunk_indices,
                                    system.component_indices);
            }
        }
        for (const auto system_id : level) {
            JobSystem::Get().schedule([this, system_id] {
                auto &system = systems.at(system_id);
                system.p_func(*this, system.system_ref, system.matching_chunk_indices, system.component_indices);
            });
        }
        JobSystem::Get().wait();
    }
    TimeProfilerEnd("ECS_Update_Execution");
}

} // namespace Pelican
