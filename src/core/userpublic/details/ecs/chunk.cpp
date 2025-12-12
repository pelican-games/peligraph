#include "chunk.hpp"

#include "componentdeclare.hpp"
#include <iterator>
#include "../../../ecs/componentinfo.hpp"

namespace Pelican {

ECSComponentChunk::ECSComponentChunk(std::span<const ComponentId> _component_ids) : count{0} {
    std::copy(_component_ids.begin(), _component_ids.end(), std::back_inserter(component_ids));
    
    ComponentId max_id = 0;
    for (const auto component : component_ids) {
        if (component > max_id) max_id = component;
    }
    component_arrays.resize(max_id + 1);

    for (const auto component : component_ids) {
        component_arrays[component].emplace(GET_MODULE(ComponentInfoManager).getSizeFromComponentId(component));
        component_arrays[component]->reserve(CHUNK_CAPACITY);
    }
}

uint32_t ECSComponentChunk::allocate(std::span<const ComponentId> component_ids, std::span<void *> component_ptrs,
                                     size_t ex_count) {
    for (int i = 0; const auto component_id : component_ids) {
        auto &arr = *component_arrays[component_id];

        auto old_count = arr.size();
        arr.expand(ex_count);

        component_ptrs[i] = arr.at(old_count);
        i++;
    }
    count += ex_count;
    return ex_count;
}

void ECSComponentChunk::free(uint32_t free_count) {
    for (const auto component_id : component_ids) {
        (*component_arrays[component_id]).shrink(free_count);
    }
    count -= free_count;
}

} // namespace Pelican
