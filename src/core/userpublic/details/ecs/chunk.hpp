#pragma once

#include <span>
#include <unordered_map>
#include <vector>
#include <optional>
#include <array>

#include <details/ecs/component.hpp>

namespace Pelican {

class ECSComponentChunk {
    class VariedArray {
        uint32_t count;
        uint32_t stride;
        std::vector<uint8_t> arr;

      public:
        VariedArray(uint32_t _stride) : count{0}, stride{_stride}, arr{} {}

        uint32_t size() const { return count; }
        uint32_t size_one() const { return stride; }
        void *data() { return arr.data(); }
        void *at(size_t index) { return static_cast<uint8_t *>(data()) + stride * index; }
        void expand(size_t ex_count) {
            arr.resize((count + ex_count) * stride);
            count += ex_count;
        }
        void shrink(size_t shrink_count) {
            arr.resize((count - shrink_count) * stride);
            count -= shrink_count;
        }
        void reserve(size_t capacity) {
            arr.reserve(capacity * stride);
        }
    };

    uint32_t count;
    // Indexed by Dense Index
    std::vector<std::optional<VariedArray>> component_arrays;
    std::vector<uint32_t> indices;
    std::vector<ComponentId> component_ids;
    size_t count = 0;

  public:
    static constexpr size_t CHUNK_CAPACITY = 4096;

    uint32_t size() const { return count; }

    bool has(ComponentId component_id) {
        if (component_id >= component_arrays.size())
            return false;
        return component_arrays[component_id].has_value();
    }

    VariedArray &get(ComponentId component_id) { return *component_arrays[component_id]; }

    // Check if chunk has all components specified by INDICES
    bool has_all(std::span<const uint32_t> req_indices) {
        for(auto req : req_indices) {
             bool found = false;
             for(auto exists : indices) {
                 if(exists == req) { found = true; break; }
             }
             if(!found) return false;
        }
        return true;
    }

    ComponentRef getRef(uint32_t index) {
        return ComponentRef{
            .ptr = component_arrays[index]->data(),
            .stride = component_arrays[index]->size_one()
        };
    }

    std::span<const ComponentId> getComponentList() const { return component_ids; }
    std::span<const uint32_t> getIndices() const { return indices; }
    
    // Chunk Constructor
    ECSComponentChunk(std::span<const uint32_t> component_indices, std::span<const ComponentId> generic_ids);

    // returns allocated count
    uint32_t allocate(std::span<const ComponentId> component_ids, std::span<void *> component_ptrs, size_t ex_count);

    void free(uint32_t free_count);
};

} // namespace Pelican
