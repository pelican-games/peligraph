#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <details/ecs/component.hpp>

namespace Pelican {

class ECSCoreTemplatePublic;
class ECSArchetypeMigration;
class ECSArchetypeMigrationToken;
class ECSEntityMutationToken;
class ECSEntityMutation;

class ECSComponentChunk {
    friend class ECSCoreTemplatePublic;
    friend class ECSArchetypeMigration;
    friend class ECSArchetypeMigrationToken;
    friend class ECSEntityMutationToken;
    friend class ECSEntityMutation;
    class VariedArray {
        friend class ECSComponentChunk;
        friend class ECSArchetypeMigration;
        friend class ECSArchetypeMigrationToken;
        friend class ECSEntityMutationToken;
        friend class ECSEntityMutation;

        size_t count = 0;
        size_t stride = 0;
        size_t alignment = 0;
        void *storage = nullptr;
        void (*construct_one)(void *) = nullptr;
        void (*destroy_one)(void *) noexcept = nullptr;
        void (*relocate_one)(void *, void *) noexcept = nullptr;
        void (*deinit_one)(void *) noexcept = nullptr;

        void *atUnchecked(size_t index) const noexcept;

      public:
        VariedArray(size_t stride, size_t alignment, void (*construct)(void *),
                    void (*destroy)(void *) noexcept, void (*relocate)(void *, void *) noexcept,
                    void (*deinit)(void *) noexcept);
        VariedArray(const VariedArray &) = delete;
        VariedArray &operator=(const VariedArray &) = delete;
        ~VariedArray();

        size_t size() const noexcept { return count; }
        size_t size_one() const noexcept { return stride; }
        void *data() const noexcept { return storage; }
        void *at(size_t index) const;
        void expand(size_t expand_count);
        void rollbackTail(size_t rollback_count) noexcept;
        void removeAt(size_t index) noexcept;
        void clear() noexcept;
    };

    // Indexed by dense component index.
    std::vector<std::unique_ptr<VariedArray>> component_arrays;
    std::vector<size_t> indices;
    std::vector<ComponentId> component_ids;
    std::vector<uint64_t> component_versions;
    size_t count = 0;
    uint64_t mask = 0;

  public:
    static constexpr size_t CHUNK_CAPACITY = 4096;

    ECSComponentChunk(std::span<const size_t> component_indices, std::span<const ComponentId> generic_ids);
    ECSComponentChunk(const ECSComponentChunk &) = delete;
    ECSComponentChunk &operator=(const ECSComponentChunk &) = delete;
    ECSComponentChunk(ECSComponentChunk &&) noexcept = default;
    ECSComponentChunk &operator=(ECSComponentChunk &&) noexcept = default;
    ~ECSComponentChunk() = default;

    size_t size() const noexcept { return count; }
    size_t remainingCapacity() const noexcept { return CHUNK_CAPACITY - count; }
    uint64_t getMask() const noexcept { return mask; }

    bool has(ComponentId component_index) const noexcept;
    void updateVersion(size_t index, uint64_t tick) noexcept;
    uint64_t getVersion(size_t index) const noexcept;
    bool has_all(std::span<const size_t> required_indices) const noexcept;

    ComponentRef getRef(size_t component_index) const;
    void *at(size_t component_index, size_t array_index) const;
    void *tryAt(size_t component_index, size_t array_index) const noexcept;
    std::span<const ComponentId> getComponentList() const noexcept { return component_ids; }
    std::span<const size_t> getIndices() const noexcept { return indices; }

  private:
    // Low-level chunk batch. The caller must keep count <= remainingCapacity().
    size_t allocate(std::span<const size_t> component_indices, std::span<void *> component_ptrs,
                    size_t expand_count);
    void rollbackTail(size_t rollback_count) noexcept;
    void removeAt(size_t array_index) noexcept;
    void clear() noexcept;
};

} // namespace Pelican
