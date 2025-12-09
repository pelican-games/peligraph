#pragma once

#include <map>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <details/ecs/componentdeclare.hpp>
#include <details/ecs/chunk.hpp>

namespace Pelican {

using SystemId = uint64_t;

class ECSCoreTemplatePublic {
    // Component Management
  private:
    using ChunkIndex = uint32_t;
    using WithinChunkIndex = uint32_t;

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
        std::vector<SystemId> depends_list;
        std::unordered_set<SystemId> depended_by;
        std::vector<ChunkIndex> matching_chunk_indices; // Cache of chunks that match this system
        void *system_ref;
        void (*p_func)(ECSCoreTemplatePublic &ecs, void *system, std::span<ChunkIndex> chunks);
        bool (*matches)(ECSComponentChunk &chunk);
    };
    uint64_t systems_counter = 0;
    std::unordered_map<SystemId, InternalSystemWrapper> systems;

  public:
    template <class TSystem, class... TComponents>
    SystemId registerSystem(TSystem &system, std::vector<SystemId> &&depends_list) {
        static_assert(std::is_same<typename TSystem::QueryComponents, std::tuple<TComponents *...>>::value);
        SystemId new_sys_id = systems_counter++;

        for (const auto depends : depends_list) {
            systems.at(depends).depended_by.insert(new_sys_id);
        }

        // Find existing chunks that match this system
        std::vector<ChunkIndex> matching_chunks;
        static const ComponentId components[] = {ComponentIdByType<TComponents>::value...};
        for (size_t i = 0; i < chunks_storage.size(); ++i) {
            if (chunks_storage[i].has_all(std::span(components))) {
                matching_chunks.push_back(i);
            }
        }

        systems.insert({
            new_sys_id,
            InternalSystemWrapper{
                .depends_list = std::move(depends_list),
                .depended_by = {},
                .matching_chunk_indices = std::move(matching_chunks),
                .system_ref = &system,
                .p_func =
                    [](ECSCoreTemplatePublic &ecs, void *p_system, std::span<ChunkIndex> chunks) {
                        static const ComponentId components[] = {ComponentIdByType<TComponents>::value...};
                        TSystem &system = *static_cast<TSystem *>(p_system);
                        for (auto chunk_idx : chunks) {
                            auto &chunk = ecs.chunks_storage[chunk_idx];
                            // No need to check has_all here, it's guaranteed by the cache
                            system.process(std::make_tuple(static_cast<TComponents *>(
                                               chunk.get(ComponentIdByType<TComponents>::value).ptr)...),
                                           chunk.size());
                        }
                    },
                .matches = [](ECSComponentChunk &chunk) -> bool {
                    static const ComponentId components[] = {ComponentIdByType<TComponents>::value...};
                    return chunk.has_all(std::span(components));
                },
            },
        });

        return new_sys_id;
    }
    void unregisterSystem(SystemId system_id);

    void update();
};

} // namespace Pelican