#include "chunk.hpp"

#include "../../../ecs/componentinfo.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <new>
#include <stdexcept>

namespace Pelican {

void *ECSComponentChunk::VariedArray::atUnchecked(size_t index) const noexcept {
    return static_cast<std::byte *>(storage) + stride * index;
}

ECSComponentChunk::VariedArray::VariedArray(size_t value_stride, size_t value_alignment,
                                            void (*construct)(void *), void (*destroy)(void *) noexcept,
                                            void (*relocate)(void *, void *) noexcept,
                                            void (*deinit)(void *) noexcept)
    : stride{value_stride}, alignment{value_alignment}, construct_one{construct}, destroy_one{destroy},
      relocate_one{relocate}, deinit_one{deinit} {
    if (stride == 0 || alignment == 0 || construct_one == nullptr || destroy_one == nullptr ||
        relocate_one == nullptr || stride > std::numeric_limits<size_t>::max() / CHUNK_CAPACITY) {
        throw std::invalid_argument("Invalid ECS component storage metadata");
    }
    storage = ::operator new(stride * CHUNK_CAPACITY, std::align_val_t{alignment});
}

ECSComponentChunk::VariedArray::~VariedArray() {
    rollbackTail(count);
    ::operator delete(storage, std::align_val_t{alignment});
}

void *ECSComponentChunk::VariedArray::at(size_t index) const {
    if (index >= count) {
        throw std::out_of_range("ECS component array index out of range");
    }
    return atUnchecked(index);
}

void ECSComponentChunk::VariedArray::expand(size_t expand_count) {
    if (expand_count > CHUNK_CAPACITY - count) {
        throw std::length_error("ECS chunk capacity exceeded");
    }
    const auto first = count;
    size_t constructed = 0;
    try {
        for (; constructed < expand_count; ++constructed) {
            construct_one(atUnchecked(first + constructed));
        }
        count += expand_count;
    } catch (...) {
        while (constructed != 0) {
            --constructed;
            destroy_one(atUnchecked(first + constructed));
        }
        throw;
    }
}

void ECSComponentChunk::VariedArray::rollbackTail(size_t rollback_count) noexcept {
    assert(rollback_count <= count);
    while (rollback_count != 0) {
        --count;
        --rollback_count;
        destroy_one(atUnchecked(count));
    }
}

void ECSComponentChunk::VariedArray::removeAt(size_t index) noexcept {
    assert(index < count);
    const auto last = count - 1;
    if (deinit_one != nullptr) {
        deinit_one(atUnchecked(index));
    }
    destroy_one(atUnchecked(index));
    if (index != last) {
        relocate_one(atUnchecked(index), atUnchecked(last));
    }
    --count;
}

void ECSComponentChunk::VariedArray::clear() noexcept {
    while (count != 0) {
        const auto index = count - 1;
        if (deinit_one != nullptr) {
            deinit_one(atUnchecked(index));
        }
        destroy_one(atUnchecked(index));
        --count;
    }
}

ECSComponentChunk::ECSComponentChunk(std::span<const size_t> component_indices,
                                     std::span<const ComponentId> generic_ids)
    : indices(component_indices.begin(), component_indices.end()),
      component_ids(generic_ids.begin(), generic_ids.end()) {
    if (indices.empty() || indices.size() != component_ids.size()) {
        throw std::invalid_argument("ECS chunk requires matching component metadata");
    }
    const auto max_index = *std::max_element(indices.begin(), indices.end());
    component_arrays.resize(max_index + 1);
    component_versions.resize(max_index + 1, 0);

    auto &manager = GET_MODULE(ComponentInfoManager);
    for (const auto index : indices) {
        if (index >= 64) {
            throw std::length_error("ECS supports at most 64 dense component indices");
        }
        const auto &info = manager.getFromIndex(index);
        component_arrays[index] = std::make_unique<VariedArray>(
            info.size, info.alignment, info.cb_construct, info.cb_destroy, info.cb_relocate, info.cb_deinit);
        mask |= (1ULL << index);
    }
}

bool ECSComponentChunk::has(ComponentId component_index) const noexcept {
    return component_index < component_arrays.size() && component_arrays[component_index] != nullptr;
}

void ECSComponentChunk::updateVersion(size_t index, uint64_t tick) noexcept {
    if (index < component_versions.size()) {
        component_versions[index] = tick;
    }
}

uint64_t ECSComponentChunk::getVersion(size_t index) const noexcept {
    return index < component_versions.size() ? component_versions[index] : 0;
}

bool ECSComponentChunk::has_all(std::span<const size_t> required_indices) const noexcept {
    return std::all_of(required_indices.begin(), required_indices.end(), [this](size_t index) {
        return has(index);
    });
}

ComponentRef ECSComponentChunk::getRef(size_t component_index) const {
    if (!has(component_index)) {
        throw std::out_of_range("ECS component is absent from chunk");
    }
    const auto &array = *component_arrays[component_index];
    return ComponentRef{.ptr = array.data(), .stride = array.size_one()};
}

void *ECSComponentChunk::at(size_t component_index, size_t array_index) const {
    if (!has(component_index)) {
        throw std::out_of_range("ECS component is absent from chunk");
    }
    return component_arrays[component_index]->at(array_index);
}

size_t ECSComponentChunk::allocate(std::span<const size_t> component_indices,
                                   std::span<void *> component_ptrs, size_t expand_count) {
    if (component_indices.size() != component_ptrs.size()) {
        throw std::invalid_argument("ECS component pointer span size mismatch");
    }
    if (expand_count > remainingCapacity()) {
        throw std::length_error("ECS chunk capacity exceeded");
    }

    const auto first = count;
    size_t expanded_arrays = 0;
    try {
        for (; expanded_arrays < component_indices.size(); ++expanded_arrays) {
            const auto index = component_indices[expanded_arrays];
            auto &array = *component_arrays.at(index);
            array.expand(expand_count);
            component_ptrs[expanded_arrays] = static_cast<std::byte *>(array.data()) + first * array.size_one();
        }
        count += expand_count;
        return expand_count;
    } catch (...) {
        while (expanded_arrays != 0) {
            --expanded_arrays;
            component_arrays[component_indices[expanded_arrays]]->rollbackTail(expand_count);
        }
        throw;
    }
}

void ECSComponentChunk::rollbackTail(size_t rollback_count) noexcept {
    assert(rollback_count <= count);
    for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
        component_arrays[*it]->rollbackTail(rollback_count);
    }
    count -= rollback_count;
}

void ECSComponentChunk::removeAt(size_t array_index) noexcept {
    assert(array_index < count);
    for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
        component_arrays[*it]->removeAt(array_index);
    }
    --count;
}

void ECSComponentChunk::clear() noexcept {
    for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
        component_arrays[*it]->clear();
    }
    count = 0;
}

} // namespace Pelican
