#pragma once

#include "../container.hpp"
#include <array>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chunk.hpp"
#include "componentinfo.hpp"

namespace Pelican {

using SystemId = uint64_t;

DECLARE_MODULE(ECSCore) {
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

  public:
    ECSCore() {}
    ~ECSCore() {}

    EntityId allocateEntity(std::span<const ComponentId> component_ids, std::span<void *> component_ptrs, size_t count);
    void remove(EntityId id);
    void compaction();

    // System Management
  private:
    struct InternalSystemWrapper {
        std::vector<SystemId> depends_list;
        std::unordered_set<SystemId> depended_by;
        void *system_ref;
        void (*p_func)(ECSCore &ecs, void *system);
    };
    uint64_t systems_counter = 0;
    std::unordered_map<SystemId, InternalSystemWrapper> systems;

  public:
    template <class TSystem, class... TComponents>
    SystemId registerSystem(TSystem & system, std::vector<SystemId> && depends_list) {
        static_assert(std::is_same<typename TSystem::QueryComponents, std::tuple<TComponents *...>>::value);
        SystemId new_sys_id = systems_counter++;

        for (const auto depends : depends_list) {
            systems.at(depends).depended_by.insert(new_sys_id);
        }
        systems.insert({
            new_sys_id,
            InternalSystemWrapper{
                .depends_list = std::move(depends_list),
                .depended_by = {},
                .system_ref = &system,
                .p_func =
                    [](ECSCore &ecs, void *p_system) {
                        static const ComponentId components[] = {ComponentIdByType<TComponents>::value...};
                        TSystem &system = *static_cast<TSystem *>(p_system);
                        for (auto &chunk : ecs.chunks_storage) {
                            if (chunk.has_all(components))
                                system.process(std::make_tuple(static_cast<TComponents *>(
                                                   chunk.get(ComponentIdByType<TComponents>::value).ptr)...),
                                               chunk.size());
                        }
                    },
            },
        });

        return new_sys_id;
    }
    void unregisterSystem(SystemId system_id);

    void update();
};

} // namespace Pelican