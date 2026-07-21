#include "modelinstanceslots.hpp"

#include <cassert>
#include <limits>
#include <stdexcept>
#include <string>

namespace Pelican::RendererInternal {

bool ModelInstanceSlots::isLive(ModelInstanceId id) const noexcept {
    return id.scene_epoch == scene_epoch_ && id.index < alive_.size() &&
           alive_[id.index] && generations_[id.index] == id.generation;
}

std::uint32_t ModelInstanceSlots::requireLive(ModelInstanceId id,
                                              const char *api_name) const {
    if (!isLive(id)) {
        throw std::runtime_error(std::string{"PolygonInstanceContainer::"} +
                                 api_name + ": stale ModelInstanceId " +
                                 toString(id));
    }
    return id.index;
}

ModelInstanceId
ModelInstanceSlots::nextId(std::uint32_t appended_index) const noexcept {
    if (free_indices_.empty()) {
        assert(appended_index == generations_.size());
        return ModelInstanceId{appended_index, 1u, scene_epoch_};
    }

    const auto index = free_indices_.back();
    assert(index < generations_.size());
    assert(!alive_[index]);
    return ModelInstanceId{index, generations_[index], scene_epoch_};
}

void ModelInstanceSlots::reserve(std::size_t capacity) {
    generations_.reserve(capacity);
    alive_.reserve(capacity);
    free_indices_.reserve(capacity);
}

void ModelInstanceSlots::publish(ModelInstanceId id) noexcept {
    assert(id.scene_epoch == scene_epoch_);
    if (id.index == generations_.size()) {
        assert(generations_.size() < generations_.capacity());
        assert(alive_.size() < alive_.capacity());
        generations_.push_back(id.generation);
        alive_.push_back(true);
    } else {
        assert(!free_indices_.empty());
        assert(free_indices_.back() == id.index);
        assert(id.index < alive_.size() && !alive_[id.index]);
        assert(generations_[id.index] == id.generation);
        free_indices_.pop_back();
        alive_[id.index] = true;
    }
    ++live_count_;
}

void ModelInstanceSlots::retire(ModelInstanceId id) noexcept {
    assert(isLive(id));
    alive_[id.index] = false;
    --live_count_;
    if (generations_[id.index] !=
        std::numeric_limits<std::uint32_t>::max()) {
        ++generations_[id.index];
        free_indices_.push_back(id.index);
    }
}

void ModelInstanceSlots::prepareClear() {
    if (scene_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "PolygonInstanceContainer::clear: ModelInstanceId scene epoch exhausted");
    }
    free_indices_.clear();
    free_indices_.reserve(generations_.size());
}

void ModelInstanceSlots::clearPrepared() noexcept {
    assert(scene_epoch_ != std::numeric_limits<std::uint64_t>::max());
    assert(free_indices_.empty());
    for (std::uint32_t index = 0; index < alive_.size(); ++index) {
        if (alive_[index]) {
            alive_[index] = false;
            if (generations_[index] !=
                std::numeric_limits<std::uint32_t>::max()) {
                ++generations_[index];
            }
        }
        if (generations_[index] !=
            std::numeric_limits<std::uint32_t>::max()) {
            free_indices_.push_back(index);
        }
    }
    live_count_ = 0;
    ++scene_epoch_;
}

bool ModelInstanceSlots::alive(std::uint32_t index) const noexcept {
    return index < alive_.size() && alive_[index];
}

ModelInstanceId ModelInstanceSlots::idAt(std::uint32_t index) const {
    if (!alive(index)) {
        throw std::runtime_error(
            "PolygonInstanceContainer::modelInstanceIdForTesting: slot is not live: " +
            std::to_string(index));
    }
    return ModelInstanceId{index, generations_[index], scene_epoch_};
}

ModelInstanceId ModelInstanceSlots::forceGeneration(ModelInstanceId id,
                                                    std::uint32_t generation) {
    const auto index = requireLive(id, "forceGenerationForTesting");
    if (generation == 0) {
        throw std::invalid_argument(
            "PolygonInstanceContainer::forceGenerationForTesting: generation must be non-zero");
    }
    generations_[index] = generation;
    return ModelInstanceId{index, generation, scene_epoch_};
}

void ModelInstanceSlots::forceSceneEpoch(std::uint64_t epoch) {
    if (live_count_ != 0) {
        throw std::runtime_error(
            "PolygonInstanceContainer::forceSceneEpochForTesting: live instances remain");
    }
    if (epoch == 0) {
        throw std::invalid_argument(
            "PolygonInstanceContainer::forceSceneEpochForTesting: epoch must be non-zero");
    }
    scene_epoch_ = epoch;
}

} // namespace Pelican::RendererInternal
