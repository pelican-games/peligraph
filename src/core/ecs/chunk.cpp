#include "chunk.hpp"

#include "component_meta.hpp"
#include <iterator>

namespace Pelican {

ECSComponentChunk::ECSComponentChunk(std::span<const ComponentId> _component_ids) : count{0} {
    std::copy(_component_ids.begin(), _component_ids.end(), std::back_inserter(component_ids));
    for (const auto component : component_ids) {
        component_arrays.insert({
            component,                                      // type
            VariedArray(getSizeFromComponentId(component)), // empty array
        });
    }
}

uint32_t ECSComponentChunk::allocate(std::span<const ComponentId> component_ids, std::span<void *> component_ptrs,
                                     size_t ex_count) {
    for (int i = 0; const auto component_id : component_ids) {
        auto &arr = component_arrays.at(component_id);

        auto old_count = arr.size();
        arr.expand(ex_count);

        component_ptrs[i] = arr.at(old_count);
        i++;
    }
    count += ex_count;
    return ex_count;
}

void ECSComponentChunk::free(uint32_t free_count) {
    for (auto &[type, arr] : component_arrays)
        arr.shrink(free_count);
    count -= free_count;
}

} // namespace Pelican
