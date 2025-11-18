#pragma once

#include <array>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace Pelican {

using EntityId = uint64_t;

DECLARE_MODULE(ECSCore) {
    using ChunkIndex = uint32_t;
    using WithinChunkIndex = uint32_t;

    struct VariedArray {
        WithinChunkIndex count;
        uint32_t stride;
        std::vector<uint8_t> arr;

        template <class T> static VariedArray make() {
            return VariedArray{
                .count = 0,
                .stride = sizeof(T),
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

        // construct with recording component types
        template <typename... TComponent> static ECSComponentChunk make() {
            return ECSComponentChunk{
                .count = 0, // empty -> count = 0
                .component_arrays{
                    // each component arrays
                    {
                        std::type_index{typeid(TComponent)}, // type
                        VariedArray::make<TComponent>(),     // empty array
                    }...,
                },
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

    template <class... TComponent> EntityId insert(TComponent &&...component) {
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
    void remove(EntityId id) {
        const auto ref = id_to_ref[id];
        auto [moved_id] = chunks_storage[ref.chunk_index].remove<EntityId>(ref.array_index);
        id_to_ref[moved_id].array_index = ref.array_index;
    }

    void compaction() { /* TODO */ }

    template <class... TComponent> struct QueryResult {
        std::vector<ECSComponentChunk *> chunks;

        struct Iterator {
            ECSComponentChunk **pp_chunks;
            size_t count_remains, chunk_remains;
            std::tuple<TComponent *...> ptrs;
            std::tuple<TComponent &...> operator*() { return std::tie(*std::get<TComponent *>(ptrs)...); }
            Iterator &operator++() {
                count_remains--;
                if (count_remains > 0) {
                    ((std::get<TComponent *>(ptrs)++), ...);
                    return *this;
                }

                // reached chunk bound
                chunk_remains--;
                if (chunk_remains > 0) {
                    pp_chunks++;
                    const auto p_chunk = *pp_chunks;
                    auto &chunk = *p_chunk;
                    count_remains = chunk.count;
                    ptrs = std::make_tuple(chunk.get<TComponent>()...);
                    return *this;
                }
                return *this;
            }
            bool operator!=(const Iterator &v) { return chunk_remains != v.chunk_remains; }
        };
        Iterator begin() {
            if (chunks.size() == 0)
                return end();
            auto &first_chunk = *chunks[0];
            return Iterator{
                .pp_chunks = chunks.data(),
                .count_remains = first_chunk.count,
                .chunk_remains = chunks.size(),
                .ptrs = std::make_tuple(first_chunk.get<TComponent>()...),
            };
        }
        Iterator end() {
            return Iterator{
                .chunk_remains = 0,
            };
        }
    };
    template <class... TComponent> QueryResult<TComponent...> query() {
        QueryResult<TComponent...> result;
        for (auto &chunk : chunks_storage) {
            if (chunk.has_all<TComponent...>() && chunk.count > 0)
                result.chunks.push_back(&chunk);
        }
        return result;
    }
};

} // namespace Pelican