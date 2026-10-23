#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace Pelican {

// AT-1 decision 1. One mix chunk as the audio thread saw it: when it had
// mixed the chunk (the steady clock) and how many engine frames it had mixed
// by then, that chunk included.
struct AudioClockStamp {
    std::int64_t steady_ns = 0;
    std::uint64_t frames = 0;
};

// The stamps go from the audio thread to the game thread through a fixed
// ring: the audio thread writes without a lock or an allocation, the game
// thread drains what is new once a frame. Each slot carries the index it
// holds (odd while it is being written), so a slot the writer is overwriting
// -- the reader fell a whole ring behind -- is skipped rather than read torn.
class AudioClockRing {
  public:
    static constexpr std::size_t capacity = 1024;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);

    // The audio thread's side; never blocks, never allocates.
    void push(const AudioClockStamp &stamp) noexcept;

    // The game thread's side: every stamp written since the last call, oldest
    // first. Stamps more than a ring behind are lost.
    template <class Visit> void drain(Visit &&visit) noexcept {
        const auto end = written_.load(std::memory_order_acquire);
        auto index = read_;
        if (end - index > capacity) index = end - capacity;
        for (; index < end; ++index) {
            const auto &slot = slots_[index % capacity];
            const auto before = slot.sequence.load(std::memory_order_acquire);
            const AudioClockStamp stamp{slot.steady_ns.load(std::memory_order_relaxed),
                                        slot.frames.load(std::memory_order_relaxed)};
            std::atomic_thread_fence(std::memory_order_acquire);
            const auto after = slot.sequence.load(std::memory_order_relaxed);
            if (before != 2 * index + 2 || after != before) continue;
            visit(stamp);
        }
        read_ = end;
    }

  private:
    struct Slot {
        std::atomic<std::uint64_t> sequence{0};
        std::atomic<std::int64_t> steady_ns{0};
        std::atomic<std::uint64_t> frames{0};
    };
    std::array<Slot, capacity> slots_{};
    std::atomic<std::uint64_t> written_{0};
    std::uint64_t read_ = 0; // the game thread's alone
};

// AT-1 decision 1: a device's audio clock, on the game thread. The engine's
// frame count moves in chunks (about 10 ms); a least-squares line through
// the stamps of the last `window_seconds` gives frames(t) at any t between
// and just after them, and
//     audio_time(t) = (frames(t) - latency_frames) / sample_rate,
// the second of the sound being heard at t (the latency is an estimate:
// what the device buffers). The value never goes down.
class AudioDeviceClock {
  public:
    struct Reading {
        // Seconds; never less than an earlier reading, never below 0.
        double audio_time = 0.0;
        // The fitted engine frames mixed by the reading's time.
        double frames = 0.0;
        // Since the clock started: how far the audio clock ran ahead of the
        // steady clock (the device's crystal against the CPU's). Runs while
        // the device is held do not count.
        double drift_seconds = 0.0;
        // Whether any stamp has come yet.
        bool running = false;
    };

    AudioDeviceClock(double sample_rate, double latency_frames, double window_seconds = 1.5);

    // One stamp the audio thread wrote, at `seconds` on the steady clock. A
    // stamp after a long silence (the device stalled, stopped or restarted)
    // or with fewer frames than the one before starts a new line.
    void add(double seconds, std::uint64_t frames);
    // The device stopped (true): the clock stands where it is and does not
    // run on from its last line. Started again (false): the next stamps
    // start a new line, from where it stood.
    void hold(bool held);
    // The clock at `seconds` on the steady clock (a frame's head).
    Reading at(double seconds);

    double sampleRate() const noexcept { return sample_rate_; }
    double latencyFrames() const noexcept { return latency_frames_; }
    std::size_t pointCountForTesting() const noexcept { return points_.size(); }

  private:
    struct Point {
        double seconds;
        double frames;
    };
    void startLine();

    double sample_rate_;
    double latency_frames_;
    double window_seconds_;
    std::deque<Point> points_;
    bool held_ = false;
    Reading last_{};
    // The drift's reference on the current line, once the line is long
    // enough to trust; the drift of the lines before it.
    std::optional<std::pair<double, double>> drift_reference_;
    double drift_before_ = 0.0;
};

} // namespace Pelican
