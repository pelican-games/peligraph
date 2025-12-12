#pragma once

#include <map>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>
#include <functional>

#include <details/ecs/componentdeclare.hpp>
#include <details/ecs/chunk.hpp>
#include "../../../ecs/componentinfo.hpp"

namespace Pelican {

using SystemId = uint64_t;

class ECSCoreTemplatePublic {
    // Component Management
  private:
    using ChunkIndex = size_t;
    using WithinChunkIndex = size_t;

    std::vector<ECSComponentChunk> chunks_storage;

    struct EntityRef {
        ChunkIndex chunk_index;
        WithinChunkIndex array_index;
    };
    std::vector<EntityRef> id_to_ref;

    struct VectorHash {
        size_t operator()(const std::vector<ComponentId> &v) const {
            std::size_t seed = 0;
            for (auto &i : v) {
                seed ^= std::hash<ComponentId>{}(i) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            }
            return seed;
        }
    };

    // Map from sorted component IDs (Archetype) to list of chunk indices
    std::unordered_map<std::vector<ComponentId>, std::vector<ChunkIndex>, VectorHash> archetype_to_chunks;

  public:
    EntityId allocateEntity(std::span<const ComponentId> component_ids, std::span<void *> component_ptrs, size_t count);
    void remove(EntityId id);
    void compaction();

    // System Management
  private:
    void updateSystemChunkCache(ChunkIndex chunk_index);

    struct InternalSystemWrapper {
        SystemId id;
        ComponentMask matching_mask = 0;
        // p_func receives dense indices
        std::function<void(ECSCoreTemplatePublic &, void*, const std::vector<ChunkIndex> &, const std::vector<size_t>&)> p_func;
        
        void *system_ref; // Pointer to actual system instance
        std::vector<SystemId> depends_list;
        std::set<SystemId> depended_by;
        std::vector<ChunkIndex> matching_chunk_indices;
        std::vector<size_t> component_indices; // Stored dense indices for this system
        std::vector<size_t> read_indices;      // Indices this system reads (const T*)
        std::vector<size_t> write_indices;     // Indices this system writes (T*)
        uint64_t last_run_tick = 0;
    };

    std::unordered_map<SystemId, InternalSystemWrapper> systems;
    uint64_t system_id_counter = 0;
    uint64_t global_tick = 1; // Starts at 1

  public:
    template <class TSystem, class... TComponents>
    SystemId registerSystem(TSystem &system, std::vector<SystemId> &&depends_list) {
        SystemId id =  ++system_id_counter;
        
        // Resolve Component Indices
        auto& mgr = GET_MODULE(ComponentInfoManager);
        
        std::vector<size_t> comp_indices;
        std::vector<size_t> read_indices;
        std::vector<size_t> write_indices;
        ComponentMask matching_mask = 0;
        
        // Deduction Helper
        auto process_component = [&](auto* ptr) {
            using Type = typename std::remove_pointer<decltype(ptr)>::type;
            ComponentId cid = ComponentIdByType<typename std::remove_const<Type>::type>::value;
            size_t idx = mgr.getIndexFromComponentId(cid);
            comp_indices.push_back(idx);
            matching_mask |= (1ULL << idx);
            
            if (std::is_const<Type>::value) {
                read_indices.push_back(idx);
            } else {
                write_indices.push_back(idx);
            }
        };

        // Fold expression to process all components
        (process_component(static_cast<TComponents*>(nullptr)), ...);
        
        InternalSystemWrapper wrapper;
        wrapper.id = id;
        wrapper.system_ref = &system;
        wrapper.depends_list = std::move(depends_list);
        wrapper.component_indices = comp_indices;
        wrapper.read_indices = read_indices;
        wrapper.write_indices = write_indices;
        wrapper.matching_mask = matching_mask;
        
        // Setup dependency graph
        for (auto dep : wrapper.depends_list) {
            systems.at(dep).depended_by.insert(id);
        }
        
        // Process function
        wrapper.p_func = [id](ECSCoreTemplatePublic &core, void* sys_ptr, const std::vector<ChunkIndex> &chunks, const std::vector<size_t>& indices) {
            TSystem &sys = *static_cast<TSystem *>(sys_ptr);
            auto& sys_wrapper = core.systems.at(id);
            
            for (auto chunk_idx : chunks) {
                auto &chunk = core.chunks_storage[chunk_idx];
                
                // Change Detection
                uint64_t max_version = 0;
                for(auto idx : indices) {
                     uint64_t v = chunk.getVersion(idx);
                     if(v > max_version) max_version = v;
                }
                
                // Skip if no changes since last run
                if (max_version <= sys_wrapper.last_run_tick && sys_wrapper.last_run_tick > 0) {
                     continue; 
                }

                auto tuple = [&]<size_t... Is>(std::index_sequence<Is...>) {
                    return std::make_tuple(
                        static_cast<TComponents *>(chunk.getRef(indices[Is]).ptr)...
                    );
                }(std::make_index_sequence<sizeof...(TComponents)>{});
                
                sys.process(tuple, chunk.size());
                
                // Update versions for Write Indices
                for (auto w_idx : sys_wrapper.write_indices) {
                    chunk.updateVersion(w_idx, core.global_tick);
                }
            }
            sys_wrapper.last_run_tick = core.global_tick;
        };

        systems.emplace(id, std::move(wrapper));
        
        // Check existing chunks
        for (size_t i = 0; i < chunks_storage.size(); ++i) {
            if ((chunks_storage[i].getMask() & matching_mask) == matching_mask) {
                systems.at(id).matching_chunk_indices.push_back(i);
            }
        }

        return id;
    }
    void unregisterSystem(SystemId system_id);

    void update();
};

} // namespace Pelican