#pragma once

#include <span>
#include <unordered_map>
#include <vector>
#include <optional>
#include <array>

#include "component.hpp"

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
    };

    uint32_t count;
    std::vector<std::optional<VariedArray>> component_arrays;
    std::vector<ComponentId> component_ids;

  public:
    uint32_t size() const { return count; }
    bool has(ComponentId component_id) const {
        return component_id < component_arrays.size() && component_arrays[component_id].has_value();
    }
    bool has_all(std::span<const ComponentId> component_ids) const {
        for (const auto component_id : component_ids)
            if (!has(component_id))
                return false;
        return true;
    }
    bool match_type(std::span<const ComponentId> component_ids) const {
        return this->component_ids.size() == component_ids.size() && has_all(component_ids);
    }
    ComponentRef get(ComponentId component_id) {
        auto &arr = *component_arrays[component_id];
        return ComponentRef{
            .ptr = arr.data(),
            .stride = arr.size_one(),
        };
    }
    std::span<const ComponentId> getComponentList() const { return component_ids; }

    // construct with recording component types
    ECSComponentChunk(std::span<const ComponentId> _component_ids);

    // returns allocated count
    uint32_t allocate(std::span<const ComponentId> component_ids, std::span<void *> component_ptrs, size_t ex_count);

    void free(uint32_t free_count);
};

} // namespace Pelican
