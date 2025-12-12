#include "chunk.hpp"

#include "componentdeclare.hpp"
#include <iterator>
#include "../../../ecs/componentinfo.hpp"

namespace Pelican {

ECSComponentChunk::ECSComponentChunk(std::span<const uint32_t> component_indices, std::span<const ComponentId> generic_ids) 
    : count{0}, component_ids(generic_ids.begin(), generic_ids.end()), indices(component_indices.begin(), component_indices.end()) {
    
    uint32_t max_index = 0;
    for (const auto idx : indices) {
        if (idx > max_index) max_index = idx;
    }
    // Resize to Max Index
    component_arrays.resize(max_index + 1);
    component_versions.resize(max_index + 1, 0); // Initialize versions to 0

    for (const auto index : indices) {
        auto& arr = component_arrays[index].emplace(GET_MODULE(ComponentInfoManager).getSizeFromIndex(index));
        arr.reserve(CHUNK_CAPACITY);
    }
}

uint32_t ECSComponentChunk::allocate(std::span<const uint32_t> component_indices, std::span<void *> component_ptrs,
                                     size_t ex_count) {
    for (int i = 0; const auto idx : component_indices) {
        auto &arr = *component_arrays[idx];

        auto old_count = arr.size();
        arr.expand(ex_count);

        component_ptrs[i] = arr.at(old_count);
        i++;
    }
    count += ex_count;
    return ex_count;
}

void ECSComponentChunk::free(size_t ex_count) {
    for (const auto idx : indices) {
        auto &arr = *component_arrays[idx];
        arr.shrink(ex_count);
    }
    count -= ex_count;
}

} // namespace Pelican
