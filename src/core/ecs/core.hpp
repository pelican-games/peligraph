#pragma once

#include "../container.hpp"
#include <array>
#include <memory>
#include <queue>
#include <span>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Pelican {

using EntityId = uint64_t;
using ComponentId = uint64_t;
using SystemId = uint64_t;

template <class T> constexpr ComponentId getComponentIdFromType(std::type_info &type) {
    return 0; // TODO
}

constexpr const std::type_info &getTypeFromComponentId(ComponentId id) {
    return typeid(int); // TODO
}

constexpr uint32_t getSizeFromComponentId(ComponentId id) {
    return sizeof(int); // TODO
}

struct ComponentDataRef {
    ComponentId component_id;
    void *p_data;
};

DECLARE_MODULE(ECSCore) {
    using ChunkIndex = uint32_t;
    using WithinChunkIndex = uint32_t;

    struct VariedArray {
        WithinChunkIndex count;
        uint32_t stride;
        std::vector<uint8_t> arr;

        static VariedArray make(uint32_t stride) {
            return VariedArray{
                .count = 0,
                .stride = stride,
                .arr = {},
            };
        }
        template <class T> T *data() { return reinterpret_cast<T *>(arr.data()); }
        template <class T> T &at(size_t index) { return *(data<T>() + index); }
        template <class T> T &back() { return at<T>(count - 1); }
        template <class T> void push(T val) {
            arr.resize((++count) * stride);
            back<T>() = val;
        }

        uint8_t *data_dynamic() { return arr.data(); }
        uint8_t *at_dynamic(size_t index) { return data_dynamic() + stride * index; }
        void *back_dynamic() { return at_dynamic(count - 1); }
        void push_dynamic(uint8_t *val) {
            arr.resize((++count) * stride);
            std::memcpy(back_dynamic(), val, stride);
        }

        void erase(WithinChunkIndex index) {
            std::memcpy(arr.data() + index * stride, arr.data() + (--count) * stride, stride);
            arr.resize(count * stride);
        }
    };

    struct ECSComponentChunk {
        uint32_t count;
        std::unordered_map<std::type_index, VariedArray> component_arrays;

        template <class TComponent> bool has() const {
            return component_arrays.find(std::type_index{typeid(TComponent)}) != component_arrays.end();
        }
        template <class... TComponent> bool has_all() const { return (has<TComponent>() && ...); }
        template <class TComponent> TComponent *get() {
            const auto ptr = component_arrays.at(std::type_index{typeid(TComponent)}).data<TComponent>();
            return reinterpret_cast<TComponent *>(ptr);
        }
        template <class... TComponent> bool match_type() const {
            return component_arrays.size() == sizeof...(TComponent) // components count is match
                   && (has_all<TComponent...>());                   // component type is match
        }

        bool has_dynamic(ComponentId component_id) const {
            std::type_index tid{getTypeFromComponentId(component_id)};
            return component_arrays.find(tid) != component_arrays.end();
        }
        bool has_all_dynamic(std::span<ComponentId> components_id) const {
            for (const auto component_id : components_id)
                if (!has_dynamic(component_id))
                    return false;
            return true;
        }
        bool match_type_dynamic(std::span<ComponentId> components_id) const {
            return component_arrays.size() == components_id.size() && has_all_dynamic(components_id);
        }

        // construct with recording component types
        template <typename... TComponent> static ECSComponentChunk make() {
            return ECSComponentChunk{
                .count = 0, // empty -> count = 0
                .component_arrays{
                    // each component arrays
                    {
                        std::type_index{typeid(TComponent)},   // type
                        VariedArray::make(sizeof(TComponent)), // empty array
                    }...,
                },
            };
        }
        static ECSComponentChunk make_dynamic(std::span<ComponentId> components_id) {
            std::unordered_map<std::type_index, VariedArray> arr;
            for (const auto component : components_id) {
                arr.insert({
                    std::type_index{getTypeFromComponentId(component)},   // type
                    VariedArray::make(getSizeFromComponentId(component)), // empty array
                });
            }
            return ECSComponentChunk{
                .count = 0, // empty -> count = 0
                .component_arrays = std::move(arr),
            };
        }

        // returns inserted entity index
        template <typename... TComponent> WithinChunkIndex insert(TComponent... component) {
            ((component_arrays.at(std::type_index{typeid(TComponent)}).push(component)), ...);
            return count++;
        }
        // swap and pop method, returns moved last entity information
        template <typename... TComponent> auto remove(WithinChunkIndex index) {
            auto moved_components =
                std::make_tuple(component_arrays.at(std::type_index{typeid(TComponent)}).back<TComponent>()...);
            for (auto &[type, arr] : component_arrays)
                arr.erase(index);
            count--;
            return moved_components;
        }

        WithinChunkIndex insert_dynamic(std::span<ComponentDataRef> components) {
            for (const auto component : components) {
                std::type_index tid{getTypeFromComponentId(component.component_id)};
                component_arrays.at(tid).push_dynamic(static_cast<uint8_t *>(component.p_data));
            }
            return count++;
        }
    };
    std::vector<ECSComponentChunk> chunks_storage;

    struct EntityRef {
        ChunkIndex chunk_index;
        WithinChunkIndex array_index;
    };
    std::vector<EntityRef> id_to_ref;

  public:
    ECSCore() {}
    ~ECSCore() {}

    template <class... TComponent> EntityId insert(TComponent && ...component) {
        // find suitable chunk
        ChunkIndex chunk_index = UINT32_MAX;
        for (uint32_t i = 0; i < chunks_storage.size(); i++) {
            if (chunks_storage[i].match_type<EntityId, TComponent...>()) {
                chunk_index = i;
                break;
            }
        }

        // add new chunk if suitable chunk is not found
        if (chunk_index == UINT32_MAX) {
            chunk_index = chunks_storage.size();
            chunks_storage.emplace_back(ECSComponentChunk::make<EntityId, TComponent...>());
        }

        // insert entity
        EntityId entity_id = id_to_ref.size();

        EntityRef entity_ref{
            .chunk_index = chunk_index,
            .array_index = chunks_storage[chunk_index].insert(entity_id, std::move(component)...),
        };

        id_to_ref.emplace_back(entity_ref);
        return entity_id;
    }

    EntityId insert_dynamic(std::span<ComponentDataRef> components) {
        std::vector<ComponentId> components_id;
        for (const auto component : components) {
            components_id.push_back(component.component_id);
        }

        // find suitable chunk
        ChunkIndex chunk_index = UINT32_MAX;
        for (uint32_t i = 0; i < chunks_storage.size(); i++) {
            if (chunks_storage[i].match_type_dynamic(components_id)) {
                chunk_index = i;
                break;
            }
        }

        // add new chunk if suitable chunk is not found
        if (chunk_index == UINT32_MAX) {
            chunk_index = chunks_storage.size();
            chunks_storage.emplace_back(ECSComponentChunk::make());
        }

        // insert entity
        EntityId entity_id = id_to_ref.size();

        EntityRef entity_ref{
            .chunk_index = chunk_index,
            .array_index = chunks_storage[chunk_index].insert_dynamic(components),
        };

        id_to_ref.emplace_back(entity_ref);
        return entity_id;
    }
    void remove(EntityId id) {
        const auto ref = id_to_ref[id];
        auto [moved_id] = chunks_storage[ref.chunk_index].remove<EntityId>(ref.array_index);
        id_to_ref[moved_id].array_index = ref.array_index;
    }

    void compaction() { /* TODO */ }

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
                        TSystem &system = *static_cast<TSystem *>(p_system);
                        for (auto &chunk : ecs.chunks_storage) {
                            if (chunk.has_all<TComponents...>())
                                system.process(std::make_tuple(chunk.get<TComponents>()...), chunk.count);
                        }
                    },
            },
        });

        return new_sys_id;
    }
    void unregisterSystem(SystemId system_id) {
        for (const auto depends : systems.at(system_id).depends_list) {
            systems.at(depends).depended_by.erase(system_id);
        }
        systems.erase(system_id);
    }

    void update();
};

} // namespace Pelican