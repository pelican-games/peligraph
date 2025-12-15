#pragma once

#include "coretemplate.hpp"
#include "coretemplate.hpp"
#include "../../../profiler.hpp"
#include "../../../job_system.hpp"
#include "../../../container.hpp"
#include "../../../ecs/componentinfo.hpp"

#include <queue>
#include <algorithm>

namespace Pelican {

namespace internal {
    size_t getIndexFromComponentId_Ref(ComponentId id) {
        auto& mgr = GET_MODULE(ComponentInfoManager);
        return mgr.getIndexFromComponentId(id);
    }
}

void ECSCoreTemplatePublic::updateSystemChunkCache(ChunkIndex chunk_index) {
    auto &chunk = chunks_storage[chunk_index];
    for (auto &[id, sys] : systems) {
        if ((chunk.getMask() & sys.matching_mask) == sys.matching_mask) {
            sys.matching_chunk_indices.push_back(chunk_index);
        }
    }
}

    EntityId ECSCoreTemplatePublic::allocateEntity(std::span<const ComponentId> component_ids, std::span<void *> component_ptrs,
                                 size_t count) {
    TimeProfilerStart("ECS_AllocateEntity");
    // Entity id is recorded as implicit component
    ComponentId component_ids_ex[65]; // MAX_COMPONENTS + 1
    size_t ex_size = component_ids.size() + 1;
    if(ex_size > 65) { return 1; }
    component_ids_ex[0] = ComponentIdByType<EntityId>::value;
    for(size_t i=0; i<component_ids.size();++i) component_ids_ex[i+1] = component_ids[i];

    // Convert to Dense Indices
    auto& mgr = GET_MODULE(ComponentInfoManager);
    std::vector<size_t> component_indices_ex(ex_size);
    for(size_t i=0; i<ex_size; ++i) {
        component_indices_ex[i] = mgr.getIndexFromComponentId(component_ids_ex[i]);
    }

    // Sort component IDs to form the archetype key
    std::vector<ComponentId> archetype_key(component_ids_ex, component_ids_ex + ex_size);
    std::sort(archetype_key.begin(), archetype_key.end());
    
    // find suitable chunk using archetype index
    ChunkIndex chunk_index = UINT32_MAX;
    
    auto it = archetype_to_chunks.find(archetype_key);
    if (it != archetype_to_chunks.end()) {
        for (auto idx : it->second) {
            if (chunks_storage[idx].size() + count <= ECSComponentChunk::CHUNK_CAPACITY) {
                chunk_index = idx;
                break; 
            }
        }
    }

    const auto entity_id_comp_idx = component_indices_ex[0];

    // insert entity
    EntityId entity_id_first = id_to_ref.size();

    // add new chunk if suitable chunk is not found
    if (chunk_index == UINT32_MAX) {
        chunk_index = chunks_storage.size();
        // Construct with INDICES and GENERIC IDs
        chunks_storage.emplace_back(std::span(component_indices_ex), std::span(component_ids_ex, ex_size));
        
        // Register in archetype map
        archetype_to_chunks[archetype_key].push_back(chunk_index);
        
        // Update system cache
        updateSystemChunkCache(chunk_index);
    }

    const auto first_index = chunks_storage[chunk_index].size();

    std::vector<void *> component_ptrs_ex(component_ids.size() + 1);
    
    const auto allocated_count = chunks_storage[chunk_index].allocate(component_indices_ex, component_ptrs_ex, count);
    
    // Set version of all components in this chunk to global_tick
    for(auto idx : component_indices_ex) {
         chunks_storage[chunk_index].updateVersion(idx, global_tick);
    }

    std::copy(component_ptrs_ex.begin() + 1, component_ptrs_ex.end(), component_ptrs.begin());

    EntityId *entity_ids = static_cast<EntityId *>(chunks_storage[chunk_index].getRef(entity_id_comp_idx).ptr);

    for (size_t i = 0; i < count; i++) {
        EntityRef entity_ref{
            .chunk_index = chunk_index,
            .array_index = static_cast<WithinChunkIndex>(first_index + i),
        };
        entity_ids[i] = id_to_ref.size();
        id_to_ref.emplace_back(entity_ref);
    }

    TimeProfilerEnd("ECS_AllocateEntity");
    return entity_id_first;
}

void ECSCoreTemplatePublic::remove(EntityId id) {
    const auto ref = id_to_ref[id];
    auto &chunk = chunks_storage[ref.chunk_index];

    // Get EntityId component array using Dense Index
    auto& mgr = GET_MODULE(ComponentInfoManager);
    size_t entity_id_idx = mgr.getIndexFromComponentId(ComponentIdByType<EntityId>::value);

    EntityId moved_id = static_cast<EntityId *>(chunk.getRef(entity_id_idx).ptr)[chunk.size() - 1];

    // Swap and erase
    for (const auto index : chunk.getIndices()) {
        auto component_arr = chunk.getRef(index);

        auto ptr = static_cast<uint8_t *>(component_arr.ptr);
        auto stride = component_arr.stride;
        std::memcpy(ptr + stride * ref.array_index, ptr + stride * (chunk.size() - 1), stride);
    }

    chunks_storage[ref.chunk_index].free(1);
    id_to_ref[moved_id].array_index = ref.array_index;
}

void ECSCoreTemplatePublic::compaction() { /* TODO */ }

void ECSCoreTemplatePublic::unregisterSystem(SystemId system_id) {
    for (const auto depends : systems.at(system_id).depends_list) {
        systems.at(depends).depended_by.erase(system_id);
    }
    systems.erase(system_id);
}

void ECSCoreTemplatePublic::update() {
    global_tick++; 
    JobSystem::Get().init(); 

    TimeProfilerStart("ECS_Update_Sort");
    
    // Level-based Topological Sort
    std::unordered_map<SystemId, size_t> in_degree;
    std::queue<SystemId> zero_degree_queue; 
    
    for (const auto &[id, sys] : systems) {
        size_t d = sys.depends_list.size();
        in_degree[id] = d;
        if (d == 0) zero_degree_queue.push(id);
    }
    
    std::vector<std::vector<SystemId>> execution_levels;
    
    while (!zero_degree_queue.empty()) {
        std::vector<SystemId> current_level;
        size_t level_size = zero_degree_queue.size();
        for(size_t i = 0; i < level_size; ++i) {
            SystemId id = zero_degree_queue.front();
            zero_degree_queue.pop();
            current_level.push_back(id);
        }
        
        execution_levels.push_back(std::move(current_level));
        
        for (const auto& executed_id : execution_levels.back()) {
            for (const auto depended : systems.at(executed_id).depended_by) {
                in_degree[depended]--;
                if (in_degree[depended] == 0) {
                    zero_degree_queue.push(depended);
                }
            }
        }
    }

    TimeProfilerEnd("ECS_Update_Sort");

    TimeProfilerStart("ECS_Update_Execution");

    // Execute levels
    for (const auto& level : execution_levels) {
        if (level.empty()) continue;
        
        for (const auto& sys_id : level) {
             JobSystem::Get().schedule([this, sys_id]() {
                auto &sys = systems.at(sys_id);
                // Pass component_indices to p_func
                sys.p_func(*this, sys.system_ref, sys.matching_chunk_indices, sys.component_indices);
             });
        }
        
        JobSystem::Get().wait();
    }
    
    TimeProfilerEnd("ECS_Update_Execution");
}

} // namespace Pelican