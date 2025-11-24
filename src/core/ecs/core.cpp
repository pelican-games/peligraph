#pragma once

#include "core.hpp"

#include <queue>

namespace Pelican {

EntityId ECSCore::allocateEntity(std::span<const ComponentId> component_ids, std::span<void *> component_ptrs,
                                 size_t count) {
    // entity id is recorded as implicit component
    std::vector<ComponentId> component_ids_ex(component_ids.size() + 1);
    component_ids_ex[0] = ComponentIdByType<EntityId>::value;
    std::copy(component_ids.begin(), component_ids.end(), component_ids_ex.begin() + 1);

    // find suitable chunk
    ChunkIndex chunk_index = UINT32_MAX;
    for (uint32_t i = 0; i < chunks_storage.size(); i++) {
        if (chunks_storage[i].match_type(component_ids_ex)) {
            chunk_index = i;
            break;
        }
    }

    // insert entity
    EntityId entity_id_first = id_to_ref.size();

    // add new chunk if suitable chunk is not found
    if (chunk_index == UINT32_MAX) {
        chunk_index = chunks_storage.size();
        chunks_storage.emplace_back(component_ids_ex);
    }

    const auto first_index = chunks_storage[chunk_index].size();

    std::vector<void *> component_ptrs_ex(component_ids.size() + 1);
    const auto allocated_count = chunks_storage[chunk_index].allocate(component_ids_ex, component_ptrs_ex, count);
    std::copy(component_ptrs_ex.begin() + 1, component_ptrs_ex.end(), component_ptrs.begin());

    EntityId *entity_ids = static_cast<EntityId *>(chunks_storage[chunk_index].get(ComponentIdByType<EntityId>::value).ptr);

    for (int i = 0; i < count; i++) {
        EntityRef entity_ref{
            .chunk_index = chunk_index,
            .array_index = first_index + i,
        };
        entity_ids[i] = id_to_ref.size();
        id_to_ref.emplace_back(entity_ref);
    }

    return entity_id_first;
}
void ECSCore::remove(EntityId id) {
    const auto ref = id_to_ref[id];
    auto &chunk = chunks_storage[ref.chunk_index];

    EntityId moved_id = static_cast<EntityId *>(chunk.get(ComponentIdByType<EntityId>::value).ptr)[chunk.size() - 1];

    // swap and erase
    for (const auto component_id : chunk.getComponentList()) {
        auto component_arr = chunk.get(component_id);

        auto ptr = static_cast<uint8_t *>(component_arr.ptr);
        auto stride = component_arr.stride;
        std::memcpy(ptr + stride * ref.array_index, ptr + stride * (chunk.size() - 1), stride);
    }

    chunks_storage[ref.chunk_index].free(1);
    id_to_ref[moved_id].array_index = ref.array_index;
}

void ECSCore::compaction() { /* TODO */ }

void ECSCore::unregisterSystem(SystemId system_id) {
    for (const auto depends : systems.at(system_id).depends_list) {
        systems.at(depends).depended_by.erase(system_id);
    }
    systems.erase(system_id);
}

void ECSCore::update() {
    std::vector<SystemId> sorted_systems;

    // topological sort
    std::queue<SystemId> que;
    std::unordered_map<SystemId, size_t> dependency_graph;

    for (const auto &[id, sys] : systems) {
        dependency_graph.insert({id, sys.depends_list.size()});
        if (sys.depends_list.empty())
            que.push(id);
    }
    while (!que.empty()) {
        auto next = que.front();
        que.pop();
        sorted_systems.push_back(next);

        for (const auto depended : systems.at(next).depended_by) {
            auto &dep_count = dependency_graph.at(depended);
            dep_count--;
            if (dep_count == 0)
                que.push(depended);
        }
    }

    // invoke
    for (auto sys_id : sorted_systems) {
        auto &sys = systems.at(sys_id);
        sys.p_func(*this, sys.system_ref);
    }
}

} // namespace Pelican