#include "outputslotqueue.hpp"

#include <algorithm>

namespace Pelican {

const OutputSlotEntry *OutputSlotQueue::entry(std::size_t slot) const {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const auto &candidate) {
                                        return candidate.slot == slot;
                                    });
    return found == entries_.end() ? nullptr : &*found;
}

std::size_t OutputSlotQueue::queueDepth() const {
    return static_cast<std::size_t>(std::count_if(
        slots_.begin(), slots_.end(), [](const auto state) {
            return state != OutputSlotState::free;
        }));
}

OutputSlotCaptureResult OutputSlotQueue::capture(std::uint64_t frame,
                                             std::uint64_t generation,
                                             bool drop) {
    const auto free = std::find(slots_.begin(), slots_.end(),
                                OutputSlotState::free);
    std::size_t slot = 0;
    if (free != slots_.end()) {
        slot = static_cast<std::size_t>(free - slots_.begin());
    } else if (!drop) {
        return {};
    } else {
        const auto oldest = std::find_if(
            entries_.begin(), entries_.end(), [&](const auto &candidate) {
                return slots_[candidate.slot] == OutputSlotState::cpu_ready;
            });
        if (oldest == entries_.end()) {
            ++frames_dropped_;
            return {.choice = OutputCaptureChoice::dropped_new};
        }
        slot = oldest->slot;
        const auto replaced = oldest->frame;
        entries_.erase(oldest);
        ++frames_dropped_;
        slots_[slot] = OutputSlotState::gpu_pending;
        entries_.push_back({slot, frame, generation});
        return {.choice = OutputCaptureChoice::replaced_oldest,
                .slot = slot,
                .replaced_frame = replaced};
    }
    slots_[slot] = OutputSlotState::gpu_pending;
    entries_.push_back({slot, frame, generation});
    return {.choice = OutputCaptureChoice::captured, .slot = slot};
}

bool OutputSlotQueue::markCpuReady(std::size_t slot) {
    if (slots_.at(slot) != OutputSlotState::gpu_pending) return false;
    slots_[slot] = OutputSlotState::cpu_ready;
    return true;
}

bool OutputSlotQueue::markSending(std::size_t slot) {
    if (slots_.at(slot) != OutputSlotState::gpu_pending &&
        slots_.at(slot) != OutputSlotState::cpu_ready)
        return false;
    slots_[slot] = OutputSlotState::sending;
    return true;
}

bool OutputSlotQueue::cancel(std::size_t slot) {
    if (slots_.at(slot) != OutputSlotState::gpu_pending) return false;
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const auto &candidate) {
                                        return candidate.slot == slot;
                                    });
    if (found == entries_.end()) return false;
    entries_.erase(found);
    slots_[slot] = OutputSlotState::free;
    return true;
}

bool OutputSlotQueue::discardReady(std::size_t slot) {
    if (slots_.at(slot) != OutputSlotState::cpu_ready) return false;
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const auto &candidate) {
                                        return candidate.slot == slot;
                                    });
    if (found == entries_.end()) return false;
    entries_.erase(found);
    slots_[slot] = OutputSlotState::free;
    ++frames_discarded_;
    return true;
}

bool OutputSlotQueue::finish(std::size_t slot) {
    if (slots_.at(slot) != OutputSlotState::sending) return false;
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const auto &candidate) {
                                        return candidate.slot == slot;
                                    });
    if (found == entries_.end()) return false;
    const auto discarded = found->discard;
    entries_.erase(found);
    slots_[slot] = OutputSlotState::free;
    if (discarded) ++frames_discarded_;
    return true;
}

} // namespace Pelican
