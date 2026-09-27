#pragma once

#include "modelinstance.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Pelican::RendererInternal {

// Owns model-instance identity independently from the render and deformation
// data stored by PolygonInstanceContainer. A slot generation changes only when
// that slot dies; the scene epoch changes only when the whole scene is cleared.
//
// The free list always has room for every slot (capacity >= slotCount()), so
// retire() and releaseWithheld() never allocate. A slot withheld by a scene
// replacement is released later, when its retirement drains, into whatever
// table is live by then -- possibly a copy made in between (an editor
// projection's candidate is a copy of the live table, and publication swaps
// it in). A std::vector copy only allocates for the elements it holds, so the
// copy operations below reserve the free list again.
class ModelInstanceSlots {
    std::vector<std::uint32_t> generations_;
    std::vector<bool> alive_;
    std::vector<std::uint32_t> free_indices_;
    std::size_t live_count_ = 0;
    std::uint64_t scene_epoch_ = 1;

  public:
    ModelInstanceSlots() = default;
    ModelInstanceSlots(const ModelInstanceSlots &other);
    ModelInstanceSlots &operator=(const ModelInstanceSlots &other);
    ModelInstanceSlots(ModelInstanceSlots &&) noexcept = default;
    ModelInstanceSlots &operator=(ModelInstanceSlots &&) noexcept = default;
    ~ModelInstanceSlots() = default;

    bool isLive(ModelInstanceId id) const noexcept;
    std::uint32_t requireLive(ModelInstanceId id, const char *api_name) const;

    bool hasReusableSlot() const noexcept { return !free_indices_.empty(); }
    ModelInstanceId nextId(std::uint32_t appended_index) const noexcept;
    void reserve(std::size_t capacity);
    void publish(ModelInstanceId id) noexcept;
    void retire(ModelInstanceId id) noexcept;

    void prepareClear();
    void clearPrepared() noexcept;
    ModelInstanceSlots prepareSceneReplacement(
        std::vector<std::uint32_t> &withheld_live_indices) const;
    void releaseWithheld(std::uint32_t index) noexcept;

    bool alive(std::uint32_t index) const noexcept;
    std::size_t slotCount() const noexcept { return generations_.size(); }
    std::size_t liveCount() const noexcept { return live_count_; }
    std::uint64_t sceneEpoch() const noexcept { return scene_epoch_; }

    ModelInstanceId idAt(std::uint32_t index) const;
    ModelInstanceId forceGeneration(ModelInstanceId id,
                                    std::uint32_t generation);
    void forceSceneEpoch(std::uint64_t epoch);
};

} // namespace Pelican::RendererInternal
