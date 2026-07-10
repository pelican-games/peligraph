#include "coretemplate.hpp"

#include "../../../container.hpp"
#include "../../../ecs/componentinfo.hpp"
#include "../../../job_system.hpp"
#include "../../../profiler.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>

namespace Pelican {

namespace internal {
size_t getIndexFromComponentId_Ref(ComponentId id) {
    return GET_MODULE(ComponentInfoManager).getIndexFromComponentId(id);
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
}

void ECSCoreTemplatePublic::update() {
    ++global_tick;
    JobSystem::Get().init();

    TimeProfilerStart("ECS_Update_Sort");
    std::unordered_map<SystemId, size_t> in_degree;
    std::queue<SystemId> zero_degree_queue;
    for (const auto &[id, system] : systems) {
        in_degree[id] = system.depends_list.size();
        if (system.depends_list.empty()) {
            zero_degree_queue.push(id);
        }
    }

    std::vector<std::vector<SystemId>> execution_levels;
    while (!zero_degree_queue.empty()) {
        std::vector<SystemId> current_level;
        const auto level_size = zero_degree_queue.size();
        for (size_t i = 0; i < level_size; ++i) {
            const auto id = zero_degree_queue.front();
            zero_degree_queue.pop();
            current_level.push_back(id);
        }
        execution_levels.push_back(std::move(current_level));
        for (const auto executed_id : execution_levels.back()) {
            for (const auto depended : systems.at(executed_id).depended_by) {
                if (--in_degree[depended] == 0) {
                    zero_degree_queue.push(depended);
                }
            }
        }
    }
    TimeProfilerEnd("ECS_Update_Sort");

    TimeProfilerStart("ECS_Update_Execution");
    for (const auto &level : execution_levels) {
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
