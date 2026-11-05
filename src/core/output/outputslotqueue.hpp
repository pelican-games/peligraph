#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace Pelican {

inline constexpr std::size_t output_slot_count = 4;

enum class OutputSlotState : std::uint8_t {
    free,
    gpu_pending,
    cpu_ready,
    sending,
};

struct OutputSlotEntry {
    std::size_t slot = 0;
    std::uint64_t frame = 0;
    std::uint64_t generation = 0;
    bool discard = false;
};

enum class OutputCaptureChoice : std::uint8_t {
    captured,
    replaced_oldest,
    dropped_new,
    full,
};

struct OutputSlotCaptureResult {
    OutputCaptureChoice choice = OutputCaptureChoice::full;
    std::optional<std::size_t> slot;
    std::optional<std::uint64_t> replaced_frame;
};

// The Vulkan-free half of one sink's four-slot pool. Its caller holds the
// module mutex whenever it changes slots or the FIFO, so those changes are one
// atomic operation to the render and worker threads.
class OutputSlotQueue {
  public:
    OutputSlotCaptureResult capture(std::uint64_t frame, std::uint64_t generation,
                                bool drop);
    bool markCpuReady(std::size_t slot);
    bool markSending(std::size_t slot);
    bool finish(std::size_t slot);
    bool cancel(std::size_t slot);
    bool discardReady(std::size_t slot);

    OutputSlotState state(std::size_t slot) const { return slots_.at(slot); }
    const OutputSlotEntry *entry(std::size_t slot) const;
    std::size_t size() const { return entries_.size(); }
    std::size_t queueDepth() const;
    std::uint64_t framesDropped() const { return frames_dropped_; }
    std::uint64_t framesDiscarded() const { return frames_discarded_; }
    void countDiscarded(std::uint64_t count = 1) { frames_discarded_ += count; }
    const std::deque<OutputSlotEntry> &entries() const { return entries_; }

  private:
    std::array<OutputSlotState, output_slot_count> slots_{};
    std::deque<OutputSlotEntry> entries_;
    std::uint64_t frames_dropped_ = 0;
    std::uint64_t frames_discarded_ = 0;
};

} // namespace Pelican
