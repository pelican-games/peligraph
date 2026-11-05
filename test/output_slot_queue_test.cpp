#include "../src/core/output/outputslotqueue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using Pelican::OutputCaptureChoice;
using Pelican::OutputSlotQueue;
using Pelican::OutputSlotState;

std::vector<std::uint64_t> frames(const OutputSlotQueue &queue) {
    std::vector<std::uint64_t> result;
    for (const auto &entry : queue.entries()) result.push_back(entry.frame);
    return result;
}

} // namespace

TEST_CASE("VO-2 slots move through the queue and return exactly once",
          "[vo2][output-slot-queue]") {
    OutputSlotQueue queue;
    const auto capture = queue.capture(7, 3, false);
    REQUIRE(capture.choice == OutputCaptureChoice::captured);
    REQUIRE(capture.slot);
    CHECK(queue.state(*capture.slot) == OutputSlotState::gpu_pending);
    CHECK(queue.queueDepth() == 1);
    CHECK_FALSE(queue.markSending(*capture.slot + 1));
    CHECK(queue.markCpuReady(*capture.slot));
    CHECK(queue.state(*capture.slot) == OutputSlotState::cpu_ready);
    CHECK(queue.markSending(*capture.slot));
    CHECK(queue.state(*capture.slot) == OutputSlotState::sending);
    CHECK_FALSE(queue.discardReady(*capture.slot));
    CHECK(queue.finish(*capture.slot));
    CHECK(queue.state(*capture.slot) == OutputSlotState::free);
    CHECK(queue.queueDepth() == 0);
    CHECK_FALSE(queue.finish(*capture.slot));
}

TEST_CASE("VO-2 drop replaces only the oldest cpu-ready reservation",
          "[vo2][output-slot-queue]") {
    OutputSlotQueue queue;
    for (std::uint64_t frame = 0; frame < Pelican::output_slot_count; ++frame)
        REQUIRE(queue.capture(frame, 1, true).choice ==
                OutputCaptureChoice::captured);
    REQUIRE(queue.markSending(0));
    REQUIRE(queue.markCpuReady(1));
    REQUIRE(queue.markCpuReady(3));
    const auto result = queue.capture(4, 1, true);
    CHECK(result.choice == OutputCaptureChoice::replaced_oldest);
    CHECK(result.slot == 1);
    CHECK(result.replaced_frame == 1);
    CHECK(frames(queue) == std::vector<std::uint64_t>{0, 2, 3, 4});
    CHECK(queue.state(0) == OutputSlotState::sending);
    CHECK(queue.state(2) == OutputSlotState::gpu_pending);
    CHECK(queue.framesDropped() == 1);

    REQUIRE(queue.capture(5, 1, true).choice ==
            OutputCaptureChoice::replaced_oldest);
    const auto no_ready = queue.capture(6, 1, true);
    CHECK(no_ready.choice == OutputCaptureChoice::dropped_new);
    CHECK_FALSE(no_ready.slot);
    CHECK(queue.state(0) == OutputSlotState::sending);
    CHECK(queue.state(2) == OutputSlotState::gpu_pending);
    CHECK(queue.framesDropped() == 3);
    CHECK(queue.queueDepth() == Pelican::output_slot_count);
}

TEST_CASE("VO-2 wait blocks on four slots and wakes with the freed reservation",
          "[vo2][output-slot-queue]") {
    OutputSlotQueue queue;
    std::mutex mutex;
    std::condition_variable condition;
    bool stopping = false;
    std::uint64_t generation = 1;
    for (std::uint64_t frame = 0; frame < Pelican::output_slot_count; ++frame)
        REQUIRE(queue.capture(frame, generation, false).choice ==
                OutputCaptureChoice::captured);

    bool entered = false;
    bool captured = false;
    std::thread producer([&] {
        std::unique_lock lock{mutex};
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] {
            return !queue.entries().empty() &&
                       std::any_of(queue.entries().begin(), queue.entries().end(),
                                   [](const auto &entry) {
                                       return entry.slot == 0;
                                   }) == false ||
                   stopping || generation != 1;
        });
        if (!stopping)
            captured = queue.capture(4, generation, false).choice ==
                       OutputCaptureChoice::captured;
    });
    {
        std::unique_lock lock{mutex};
        REQUIRE(condition.wait_for(lock, std::chrono::seconds{5}, [&] {
            return entered;
        }));
        CHECK(queue.capture(4, generation, false).choice ==
              OutputCaptureChoice::full);
        REQUIRE(queue.markSending(0));
        REQUIRE(queue.finish(0));
        condition.notify_all();
    }
    producer.join();
    CHECK(captured);
    CHECK(frames(queue) == std::vector<std::uint64_t>{1, 2, 3, 4});
}

TEST_CASE("VO-2 reservations bound max_frames and drops release them",
          "[vo2][output-slot-queue]") {
    OutputSlotQueue queue;
    std::uint64_t written = 0;
    constexpr std::uint64_t limit = 4;
    for (std::uint64_t frame = 0; frame < limit; ++frame) {
        REQUIRE(written + queue.size() < limit);
        REQUIRE(queue.capture(frame, 1, true).choice ==
                OutputCaptureChoice::captured);
    }
    CHECK(written + queue.size() == limit);
    REQUIRE(queue.markCpuReady(0));
    REQUIRE(queue.capture(4, 1, true).choice ==
            OutputCaptureChoice::replaced_oldest);
    CHECK(written + queue.size() == limit);
    for (std::size_t slot = 0; slot < Pelican::output_slot_count; ++slot) {
        REQUIRE(queue.markSending(slot));
        REQUIRE(queue.finish(slot));
        ++written;
    }
    CHECK(written == limit);
}

TEST_CASE("VO-2 ready entries discard after a stop without touching sending",
          "[vo2][output-slot-queue]") {
    OutputSlotQueue queue;
    for (std::uint64_t frame = 0; frame < 3; ++frame)
        REQUIRE(queue.capture(frame, 1, false).choice ==
                OutputCaptureChoice::captured);
    REQUIRE(queue.markSending(0));
    REQUIRE(queue.markCpuReady(1));
    REQUIRE(queue.markCpuReady(2));
    CHECK(queue.discardReady(1));
    CHECK(queue.finish(0));
    CHECK(queue.state(0) == OutputSlotState::free);
    CHECK(queue.state(1) == OutputSlotState::free);
    CHECK(queue.state(2) == OutputSlotState::cpu_ready);
    CHECK(queue.framesDiscarded() == 1);
    CHECK(frames(queue) == std::vector<std::uint64_t>{2});
}

TEST_CASE("VO-2 stopped teardown discards queued work without waiting for the producer",
          "[vo2][output-slot-queue]") {
    OutputSlotQueue queue;
    for (std::uint64_t frame = 0; frame < Pelican::output_slot_count; ++frame) {
        REQUIRE(queue.capture(frame, 1, false).choice ==
                OutputCaptureChoice::captured);
        REQUIRE(queue.markCpuReady(static_cast<std::size_t>(frame)));
    }
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t discarded = 0;
    for (std::size_t slot = 0; slot < Pelican::output_slot_count; ++slot)
        if (queue.discardReady(slot)) ++discarded;
    CHECK(discarded == Pelican::output_slot_count);
    CHECK(queue.queueDepth() == 0);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{10});
}
