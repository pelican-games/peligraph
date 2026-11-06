// VO-4: everything above the Spout boundary, with a fake backend, on every
// platform: the mailbox, the two-buffer bound, the threads, stop, failures and
// status. Nothing here sleeps; a wait is a bounded poll of what it waits for.
#include "../src/core/output/outputspout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Pelican::OutputSpoutBackend;
using Pelican::OutputSpoutFrame;
using Pelican::OutputSpoutResult;
using Pelican::OutputSpoutResultKind;
using Pelican::OutputSpoutSender;
using TimePoint = OutputSpoutSender::TimePoint;

template <class Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds limit = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// What every fake backend of one test saw and was told to do.
struct FakeSpout {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> events;
    std::set<std::thread::id> threads;
    std::vector<std::vector<std::uint8_t>> sent;
    std::deque<OutputSpoutResult> results;
    OutputSpoutResult open_result;
    std::string actual_name;
    bool block_send = false;
    bool block_close = false;
    int in_send = 0;
    int in_close = 0;
    int attempts = 0;
    int opens = 0;
    int closes = 0;
    int factories = 0;
    int alive = 0;

    void setBlockSend(bool value) {
        std::lock_guard lock{mutex};
        block_send = value;
        cv.notify_all();
    }
    void setBlockClose(bool value) {
        std::lock_guard lock{mutex};
        block_close = value;
        cv.notify_all();
    }
    template <class T> T read(T FakeSpout::*member) const {
        std::lock_guard lock{mutex};
        return this->*member;
    }
    std::vector<std::string> log() const {
        std::lock_guard lock{mutex};
        return events;
    }
    std::vector<std::vector<std::uint8_t>> frames() const {
        std::lock_guard lock{mutex};
        return sent;
    }
    std::set<std::thread::id> threadIds() const {
        std::lock_guard lock{mutex};
        return threads;
    }
    void pushResult(OutputSpoutResult result) {
        std::lock_guard lock{mutex};
        results.push_back(std::move(result));
    }
    Pelican::OutputSpoutBackendFactory factory();
};

class FakeBackend final : public OutputSpoutBackend {
  public:
    explicit FakeBackend(FakeSpout &spout) : spout_(spout) {
        std::lock_guard lock{spout_.mutex};
        spout_.threads.insert(std::this_thread::get_id());
        ++spout_.alive;
    }
    ~FakeBackend() override {
        std::lock_guard lock{spout_.mutex};
        spout_.threads.insert(std::this_thread::get_id());
        --spout_.alive;
    }
    OutputSpoutResult open(const std::string &name) override {
        std::lock_guard lock{spout_.mutex};
        spout_.threads.insert(std::this_thread::get_id());
        ++spout_.opens;
        spout_.events.push_back("open:" + name);
        name_ = spout_.actual_name.empty() ? name : spout_.actual_name;
        return spout_.open_result;
    }
    OutputSpoutResult send(const std::uint8_t *bgra, std::uint32_t width,
                           std::uint32_t height) override {
        std::unique_lock lock{spout_.mutex};
        spout_.threads.insert(std::this_thread::get_id());
        ++spout_.in_send;
        spout_.cv.notify_all();
        spout_.cv.wait(lock, [&] { return !spout_.block_send; });
        --spout_.in_send;
        ++spout_.attempts;
        spout_.events.push_back("send:" + std::to_string(width) + "x" +
                                std::to_string(height));
        OutputSpoutResult result;
        if (!spout_.results.empty()) {
            result = std::move(spout_.results.front());
            spout_.results.pop_front();
        }
        if (result.kind == OutputSpoutResultKind::ok) {
            spout_.sent.emplace_back(
                bgra, bgra + static_cast<std::size_t>(width) * height * 4);
        }
        return result;
    }
    std::string actualName() const override {
        std::lock_guard lock{spout_.mutex};
        spout_.threads.insert(std::this_thread::get_id());
        return name_;
    }
    void close() override {
        std::unique_lock lock{spout_.mutex};
        spout_.threads.insert(std::this_thread::get_id());
        spout_.events.push_back("close_begin");
        ++spout_.in_close;
        spout_.cv.notify_all();
        spout_.cv.wait(lock, [&] { return !spout_.block_close; });
        --spout_.in_close;
        ++spout_.closes;
        spout_.events.push_back("close");
    }

  private:
    FakeSpout &spout_;
    std::string name_;
};

Pelican::OutputSpoutBackendFactory FakeSpout::factory() {
    return [this]() -> std::unique_ptr<OutputSpoutBackend> {
        {
            std::lock_guard lock{mutex};
            ++factories;
        }
        return std::make_unique<FakeBackend>(*this);
    };
}

OutputSpoutSender::Options options(FakeSpout &spout, std::string name = "fake") {
    OutputSpoutSender::Options result;
    result.name = std::move(name);
    result.factory = spout.factory();
    return result;
}

// A worker's hand-off: one frame whose bytes all start with `mark`.
bool hand(OutputSpoutSender &sender, std::uint8_t mark, std::uint32_t width = 2,
          std::uint32_t height = 2, bool bgra = true) {
    auto frame = sender.acquire(width, height, bgra);
    if (!frame) return false;
    std::fill(frame->pixels.begin(), frame->pixels.end(), mark);
    sender.publish(std::move(*frame));
    return true;
}

bool sendSettled(const OutputSpoutSender &sender, std::uint64_t written,
                 std::uint64_t dropped) {
    return waitUntil([&] {
        const auto status = sender.status();
        return status.frames_written == written && status.frames_dropped == dropped;
    });
}

struct ManualClock {
    std::atomic<std::int64_t> seconds_x10{0};
    void at(double seconds) {
        seconds_x10.store(static_cast<std::int64_t>(seconds * 10.0 + 0.5));
    }
    std::function<TimePoint()> function() {
        return [this] {
            return TimePoint{} + std::chrono::milliseconds(seconds_x10.load() * 100);
        };
    }
};

void failAt(FakeSpout &spout, OutputSpoutSender &sender, ManualClock &clock,
            double seconds) {
    const auto before = sender.status().frames_dropped;
    clock.at(seconds);
    spout.pushResult({OutputSpoutResultKind::failed, "injected"});
    REQUIRE(hand(sender, 1));
    REQUIRE(waitUntil([&] { return sender.status().frames_dropped == before + 1; }));
}

void succeedAt(FakeSpout &spout, OutputSpoutSender &sender, ManualClock &clock,
               double seconds) {
    const auto before = sender.status().frames_written;
    clock.at(seconds);
    REQUIRE(hand(sender, 1));
    REQUIRE(waitUntil([&] { return sender.status().frames_written == before + 1; }));
    (void)spout;
}

bool hasPrefix(const std::string &text, const std::string &prefix) {
    return text.rfind(prefix, 0) == 0;
}

} // namespace

TEST_CASE("VO-4 activation precedence is authored, flag, offline, then availability",
          "[vo4]") {
    using Pelican::outputSpoutActivation;
    const auto not_enabled = [](const Pelican::OutputSpoutActivation &value) {
        return !value.start && value.state == "inactive" &&
               value.stop_reason == "not_enabled" && value.error.empty();
    };
    // Whatever else is true, a disabled node or a missing flag opens nothing.
    CHECK(not_enabled(outputSpoutActivation(false, true, false, true)));
    CHECK(not_enabled(outputSpoutActivation(true, false, false, true)));
    CHECK(not_enabled(outputSpoutActivation(true, false, true, false)));
    CHECK(not_enabled(outputSpoutActivation(false, true, true, false)));

    // An offline run never sends, even armed, and even where Spout isn't built.
    for (const bool available : {true, false}) {
        const auto offline = outputSpoutActivation(true, true, true, available);
        CHECK_FALSE(offline.start);
        CHECK(offline.state == "inactive");
        CHECK(offline.stop_reason == "offline");
        CHECK(offline.error.empty());
    }

    const auto unavailable = outputSpoutActivation(true, true, false, false);
    CHECK_FALSE(unavailable.start);
    CHECK(unavailable.state == "refused");
    CHECK(hasPrefix(unavailable.error, "output.spout_unavailable@1"));

    const auto armed = outputSpoutActivation(true, true, false, true);
    CHECK(armed.start);
    CHECK(armed.state == "running");
}

TEST_CASE("VO-4 the mailbox holds one frame, the newest wins and each replacement counts once",
          "[vo4]") {
    FakeSpout spout;
    spout.setBlockSend(true);
    OutputSpoutSender sender{options(spout)};
    REQUIRE(hand(sender, 10));
    REQUIRE(waitUntil([&] { return spout.read(&FakeSpout::in_send) == 1; }));

    REQUIRE(hand(sender, 20));
    CHECK(sender.status().mailbox_depth == 1);
    CHECK(sender.status().frames_dropped == 0);
    REQUIRE(hand(sender, 30));
    CHECK(sender.status().mailbox_depth == 1);
    CHECK(sender.status().frames_dropped == 1);
    REQUIRE(hand(sender, 40));
    CHECK(sender.status().frames_dropped == 2);

    const auto bounded = sender.status();
    CHECK(bounded.peak_mailbox_depth == 1);
    CHECK(bounded.peak_live_buffers == 2);

    spout.setBlockSend(false);
    REQUIRE(sendSettled(sender, 2, 2));
    const auto frames = spout.frames();
    REQUIRE(frames.size() == 2);
    CHECK(frames.at(0).at(0) == 10);
    CHECK(frames.at(1).at(0) == 40);
}

TEST_CASE("VO-4 at most two pixel buffers exist, through replacement and size changes",
          "[vo4]") {
    FakeSpout spout;
    spout.setBlockSend(true);
    OutputSpoutSender sender{options(spout)};
    REQUIRE(hand(sender, 1, 4, 4));
    REQUIRE(waitUntil([&] { return spout.read(&FakeSpout::in_send) == 1; }));

    // One buffer is being sent, so the worker gets the other, and with both out
    // a further frame is dropped rather than a third allocated.
    auto second = sender.acquire(4, 4, true);
    REQUIRE(second);
    CHECK(sender.status().live_buffers == 2);
    const auto dropped = sender.status().frames_dropped;
    CHECK_FALSE(sender.acquire(4, 4, true));
    CHECK(sender.status().frames_dropped == dropped + 1);
    sender.publish(std::move(*second));

    for (std::uint32_t size = 2; size < 40; size += 3) {
        const auto extent = size * 2;
        REQUIRE(hand(sender, static_cast<std::uint8_t>(size), extent, extent));
        CHECK(sender.status().live_buffers <= 2);
    }
    spout.setBlockSend(false);
    REQUIRE(waitUntil([&] { return sender.status().mailbox_depth == 0; }));
    REQUIRE(hand(sender, 200, 8, 8));
    REQUIRE(waitUntil([&] {
        const auto frames = spout.frames();
        return !frames.empty() && frames.back().at(0) == 200;
    }));
    const auto status = sender.status();
    CHECK(status.peak_live_buffers == 2);
    CHECK(status.peak_mailbox_depth == 1);
}

TEST_CASE("VO-4 zero and overflowing sizes are refused before allocation", "[vo4]") {
    FakeSpout spout;
    OutputSpoutSender sender{options(spout)};
    CHECK_FALSE(sender.acquire(0, 4, true));
    CHECK_FALSE(sender.acquire(4, 0, true));
    if (sizeof(std::size_t) == 4) {
        CHECK_FALSE(sender.acquire(0x10000, 0x4000, true));
    } else {
        CHECK_FALSE(sender.acquire(0xFFFFFFFFu, 0xFFFFFFFFu, true));
    }
    CHECK(sender.status().live_buffers == 0);
    CHECK(sender.status().frames_discarded >= 3);
    CHECK(spout.read(&FakeSpout::attempts) == 0);
}

TEST_CASE("VO-4 RGBA8 and BGRA8 give the backend the same bytes, with alpha kept", "[vo4]") {
    FakeSpout spout;
    OutputSpoutSender sender{options(spout)};
    // Two rows that differ, in the source's own channel order.
    const std::vector<std::uint8_t> rgba{10, 20, 30, 40, 11, 21, 31, 41,
                                         12, 22, 32, 42, 13, 23, 33, 43};
    const std::vector<std::uint8_t> bgra{30, 20, 10, 40, 31, 21, 11, 41,
                                         32, 22, 12, 42, 33, 23, 13, 43};
    auto red_first = sender.acquire(2, 2, false);
    REQUIRE(red_first);
    red_first->pixels = rgba;
    sender.publish(std::move(*red_first));
    REQUIRE(sendSettled(sender, 1, 0));
    auto blue_first = sender.acquire(2, 2, true);
    REQUIRE(blue_first);
    blue_first->pixels = bgra;
    sender.publish(std::move(*blue_first));
    REQUIRE(sendSettled(sender, 2, 0));

    const auto frames = spout.frames();
    REQUIRE(frames.size() == 2);
    CHECK(frames.at(0) == bgra);
    CHECK(frames.at(1) == bgra);
}

TEST_CASE("VO-4 every backend call is on one thread, not the render thread or the worker",
          "[vo4]") {
    FakeSpout spout;
    {
        OutputSpoutSender sender{options(spout)};
        REQUIRE(waitUntil([&] { return !sender.status().sender_name.empty(); }));
        std::thread::id worker_id;
        // The render thread here is this one, the readback worker that one.
        std::thread worker{[&] {
            worker_id = std::this_thread::get_id();
            REQUIRE(hand(sender, 1, 2, 2, false));
        }};
        worker.join();
        REQUIRE(sendSettled(sender, 1, 0));
        const auto ids = spout.threadIds();
        REQUIRE(ids.size() == 1);
        CHECK(*ids.begin() != std::this_thread::get_id());
        CHECK(*ids.begin() != worker_id);
    }
    // Including the close and the destructor.
    CHECK(spout.read(&FakeSpout::closes) == 1);
    CHECK(spout.read(&FakeSpout::alive) == 0);
    CHECK(spout.threadIds().size() == 1);
    CHECK_FALSE(spout.threadIds().contains(std::this_thread::get_id()));
}

TEST_CASE("VO-4 a blocked send delays only the join, and teardown is safe", "[vo4]") {
    FakeSpout spout;
    spout.setBlockSend(true);
    auto sender = std::make_unique<OutputSpoutSender>(options(spout));
    const auto release = sender->release();
    REQUIRE(hand(*sender, 1));
    REQUIRE(waitUntil([&] { return spout.read(&FakeSpout::in_send) == 1; }));
    // The worker's hand-off and the stop both return with the send stuck.
    REQUIRE(hand(*sender, 2));
    sender->requestStop();
    CHECK(sender->status().state == "running");
    CHECK_FALSE(sender->finished());
    CHECK_FALSE(release.done());
    // A late completion can't revive a stopped sender.
    CHECK_FALSE(sender->acquire(2, 2, true));

    std::atomic<bool> joined{false};
    std::thread joiner{[&] {
        sender.reset();
        joined = true;
    }};
    CHECK_FALSE(joined.load());
    CHECK(spout.read(&FakeSpout::closes) == 0);
    spout.setBlockSend(false);
    joiner.join();
    CHECK(joined.load());
    CHECK(release.done());
    // The send finished, nothing else was sent, and only then did it close.
    CHECK(spout.frames().size() == 1);
    CHECK(spout.read(&FakeSpout::closes) == 1);
    CHECK(spout.read(&FakeSpout::alive) == 0);
    const auto log = spout.log();
    REQUIRE(log.size() == 4);
    CHECK(log.at(1) == "send:2x2");
    CHECK(log.at(2) == "close_begin");
}

TEST_CASE("VO-4 a frame published after the stop goes nowhere", "[vo4]") {
    FakeSpout spout;
    OutputSpoutSender sender{options(spout)};
    REQUIRE(hand(sender, 1));
    REQUIRE(sendSettled(sender, 1, 0));
    auto late = sender.acquire(2, 2, true);
    REQUIRE(late);
    sender.requestStop();
    REQUIRE(waitUntil([&] { return sender.finished(); }));
    const auto discarded = sender.status().frames_discarded;
    sender.publish(std::move(*late));
    const auto status = sender.status();
    CHECK(status.frames_discarded == discarded + 1);
    CHECK(status.mailbox_depth == 0);
    CHECK(status.state == "finished");
    CHECK(status.stop_reason == "stop");
    CHECK_FALSE(status.registered);
    CHECK(spout.frames().size() == 1);
}

TEST_CASE("VO-4 a new sender with the same name opens only after the old one's close is acknowledged",
          "[vo4]") {
    FakeSpout spout;
    spout.setBlockClose(true);
    auto old_sender = std::make_unique<OutputSpoutSender>(options(spout, "same"));
    REQUIRE(hand(*old_sender, 1));
    REQUIRE(sendSettled(*old_sender, 1, 0));
    old_sender->requestStop();
    REQUIRE(waitUntil([&] { return spout.read(&FakeSpout::in_close) == 1; }));

    auto next_options = options(spout, "same");
    next_options.predecessor = old_sender->release();
    OutputSpoutSender next{std::move(next_options)};
    CHECK(next.status().state == "running");
    // The stop was asked for and the old send is over, and that isn't enough.
    const auto opened_early =
        waitUntil([&] { return spout.read(&FakeSpout::opens) > 1; }, 200ms);
    CHECK_FALSE(opened_early);
    CHECK(next.status().state == "running");
    CHECK_FALSE(next.status().registered);

    spout.setBlockClose(false);
    REQUIRE(waitUntil([&] { return spout.read(&FakeSpout::opens) == 2; }));
    CHECK(next.status().state == "running");
    const auto log = spout.log();
    const auto close_at = std::find(log.begin(), log.end(), "close");
    REQUIRE(close_at != log.end());
    CHECK(std::count(close_at, log.end(), "open:same") == 1);
    old_sender.reset();
}

TEST_CASE("VO-4 a pending replacement that stops never opens and holds the next in line",
          "[vo4]") {
    FakeSpout spout;
    spout.setBlockClose(true);
    OutputSpoutSender first{options(spout, "chain")};
    REQUIRE(waitUntil([&] { return first.status().sender_name == "chain"; }));
    first.requestStop();
    REQUIRE(waitUntil([&] { return spout.read(&FakeSpout::in_close) == 1; }));
    auto second_options = options(spout, "chain");
    second_options.predecessor = first.release();
    OutputSpoutSender second{std::move(second_options)};
    second.requestStop();
    auto third_options = options(spout, "chain");
    third_options.predecessor = second.release();
    OutputSpoutSender third{std::move(third_options)};

    CHECK_FALSE(waitUntil([&] { return second.finished(); }, 100ms));
    spout.setBlockClose(false);
    REQUIRE(waitUntil([&] { return second.finished(); }));
    REQUIRE(waitUntil([&] { return third.status().sender_name == "chain"; }));
    CHECK(spout.read(&FakeSpout::opens) == 2);
    CHECK(third.status().state == "running");
}

TEST_CASE("VO-4 a changed name is refused before anything is sent", "[vo4]") {
    FakeSpout spout;
    spout.actual_name = "taken_1";
    OutputSpoutSender sender{options(spout, "taken")};
    (void)waitUntil([&] { return sender.finished(); }, 2s);
    const auto status = sender.status();
    CHECK(status.state == "refused");
    CHECK(hasPrefix(status.last_error, "output.spout_name_taken@1"));
    CHECK_FALSE(status.registered);
    CHECK(status.sender_name.empty());
    CHECK_FALSE(sender.acquire(2, 2, true));
    CHECK(spout.read(&FakeSpout::attempts) == 0);
    CHECK(spout.read(&FakeSpout::closes) == 1);
}

TEST_CASE("VO-4 two senders of one name in this process don't share it", "[vo4]") {
    FakeSpout spout;
    OutputSpoutSender holder{options(spout, "reserved")};
    REQUIRE(hand(holder, 1));
    REQUIRE(sendSettled(holder, 1, 0));
    OutputSpoutSender second{options(spout, "reserved")};
    REQUIRE(waitUntil([&] { return second.finished(); }));
    CHECK(second.status().state == "refused");
    CHECK(hasPrefix(second.status().last_error, "output.spout_name_taken@1"));
    CHECK(spout.read(&FakeSpout::opens) == 1);
    CHECK(holder.status().state == "running");
}

TEST_CASE("VO-4 open failures refuse by name and nothing is sent", "[vo4]") {
    SECTION("the backend can't open") {
        FakeSpout spout;
        spout.open_result = {OutputSpoutResultKind::open_failed, "no D3D11 device"};
        OutputSpoutSender sender{options(spout)};
        REQUIRE(waitUntil([&] { return sender.finished(); }));
        CHECK(sender.status().last_error ==
              "output.spout_open_failed@1: no D3D11 device");
        CHECK(sender.status().state == "refused");
        CHECK(spout.read(&FakeSpout::attempts) == 0);
        CHECK(spout.read(&FakeSpout::alive) == 0);
    }
    SECTION("no backend at all") {
        OutputSpoutSender::Options empty;
        empty.name = "nothing";
        OutputSpoutSender sender{std::move(empty)};
        REQUIRE(waitUntil([&] { return sender.finished(); }));
        CHECK(hasPrefix(sender.status().last_error, "output.spout_open_failed@1"));
    }
    SECTION("names Spout's buffers can't hold never reach a backend") {
        for (const auto &name : {std::string{}, std::string(256, 'a')}) {
            FakeSpout spout;
            OutputSpoutSender sender{options(spout, name)};
            REQUIRE(waitUntil([&] { return sender.finished(); }));
            CHECK(hasPrefix(sender.status().last_error, "output.spout_open_failed@1"));
            CHECK(spout.read(&FakeSpout::factories) == 0);
        }
    }
    SECTION("the longest name that fits opens") {
        FakeSpout spout;
        OutputSpoutSender sender{options(spout, std::string(255, 'a'))};
        REQUIRE(waitUntil([&] { return spout.read(&FakeSpout::opens) == 1; }));
        REQUIRE(waitUntil([&] { return !sender.status().sender_name.empty(); }));
        CHECK(sender.status().state == "running");
    }
}

TEST_CASE("VO-4 a send that finds no shared texture or registration is an open failure",
          "[vo4]") {
    FakeSpout spout;
    spout.pushResult({OutputSpoutResultKind::open_failed, "no shared texture"});
    OutputSpoutSender sender{options(spout)};
    REQUIRE(hand(sender, 1));
    REQUIRE(waitUntil([&] { return sender.finished(); }));
    const auto status = sender.status();
    CHECK(status.state == "refused");
    CHECK(status.last_error == "output.spout_open_failed@1: no shared texture");
    CHECK_FALSE(status.registered);
    CHECK(status.frames_written == 0);
    CHECK(status.width == 0);
}

TEST_CASE("VO-4 device removal is terminal at once, with its HRESULT", "[vo4]") {
    FakeSpout spout;
    ManualClock clock;
    auto opts = options(spout);
    opts.clock = clock.function();
    OutputSpoutSender sender{std::move(opts)};
    REQUIRE(hand(sender, 1));
    REQUIRE(sendSettled(sender, 1, 0));
    spout.pushResult({OutputSpoutResultKind::device_removed,
                      "device removed, HRESULT 0x887A0005"});
    REQUIRE(hand(sender, 2));
    // No time passes: this isn't the five-second rule.
    (void)waitUntil([&] { return sender.finished(); }, 2s);
    const auto status = sender.status();
    CHECK(status.state == "refused");
    CHECK(status.last_error ==
          "output.spout_send_failed@1: device removed, HRESULT 0x887A0005");
    CHECK_FALSE(status.registered);
}

TEST_CASE("VO-4 failures at 0, 2 and 6 seconds refuse, and not before", "[vo4]") {
    FakeSpout spout;
    ManualClock clock;
    auto opts = options(spout);
    opts.clock = clock.function();
    OutputSpoutSender sender{std::move(opts)};
    failAt(spout, sender, clock, 0.0);
    CHECK(sender.status().state == "running");
    CHECK(hasPrefix(sender.status().last_error, "output.spout_send_failed@1"));
    failAt(spout, sender, clock, 2.0);
    CHECK(sender.status().state == "running");
    CHECK(sender.status().frames_dropped == 2);
    failAt(spout, sender, clock, 6.0);
    const auto refused = waitUntil([&] { return sender.status().state == "refused"; });
    CHECK(refused);
    CHECK(hasPrefix(sender.status().last_error, "output.spout_send_failed@1"));
    CHECK(sender.status().frames_dropped == 3);
    // Terminal until a reload makes a new sender.
    CHECK_FALSE(sender.acquire(2, 2, true));
    REQUIRE(waitUntil([&] { return sender.finished(); }));
    CHECK(sender.status().state == "refused");
}

TEST_CASE("VO-4 a success clears the failure streak and its error", "[vo4]") {
    FakeSpout spout;
    ManualClock clock;
    auto opts = options(spout);
    opts.clock = clock.function();
    OutputSpoutSender sender{std::move(opts)};
    failAt(spout, sender, clock, 0.0);
    failAt(spout, sender, clock, 4.0);
    CHECK_FALSE(sender.status().last_error.empty());
    succeedAt(spout, sender, clock, 4.5);
    CHECK(sender.status().last_error.empty());
    failAt(spout, sender, clock, 6.0);
    CHECK(sender.status().state == "running");
    failAt(spout, sender, clock, 10.0);
    CHECK(sender.status().state == "running");
    failAt(spout, sender, clock, 11.0);
    const auto refused = waitUntil([&] { return sender.status().state == "refused"; });
    CHECK(refused);
}

TEST_CASE("VO-4 a period with no attempted sends isn't a failure", "[vo4]") {
    FakeSpout spout;
    ManualClock clock;
    auto opts = options(spout);
    opts.clock = clock.function();
    OutputSpoutSender sender{std::move(opts)};
    failAt(spout, sender, clock, 0.0);
    // Time alone refuses nothing, and the next failure begins a new streak.
    clock.at(100.0);
    CHECK(sender.status().state == "running");
    failAt(spout, sender, clock, 100.0);
    CHECK(sender.status().state == "running");
    failAt(spout, sender, clock, 103.0);
    CHECK(sender.status().state == "running");
    failAt(spout, sender, clock, 105.5);
    const auto refused = waitUntil([&] { return sender.status().state == "refused"; });
    CHECK(refused);
}

TEST_CASE("VO-4 SendImage accepting a frame is what frames_written counts", "[vo4]") {
    // The real SendImage returns true when the mutex wait timed out and no pixels
    // moved (vo4_spout_windows_test holds it for real). All the sender can see
    // is the acceptance, and it must not read it as anything more.
    FakeSpout spout;
    OutputSpoutSender sender{options(spout)};
    REQUIRE(hand(sender, 1));
    REQUIRE(sendSettled(sender, 1, 0));
    const auto status = sender.status();
    CHECK(status.frames_written == 1);
    CHECK(status.frames_dropped == 0);
    CHECK(status.last_error.empty());
    CHECK(status.state == "running");
}

TEST_CASE("VO-4 status shows registered and the size only for what was accepted", "[vo4]") {
    FakeSpout spout;
    ManualClock clock;
    auto opts = options(spout, "status");
    opts.clock = clock.function();
    {
        OutputSpoutSender sender{std::move(opts)};
        REQUIRE(waitUntil([&] { return sender.status().sender_name == "status"; }));
        // Named and opened, and not yet published.
        CHECK_FALSE(sender.status().registered);
        CHECK(sender.status().width == 0);
        spout.pushResult({OutputSpoutResultKind::failed, "not yet"});
        REQUIRE(hand(sender, 1, 4, 2));
        REQUIRE(sendSettled(sender, 0, 1));
        CHECK_FALSE(sender.status().registered);
        CHECK(sender.status().width == 0);

        REQUIRE(hand(sender, 1, 4, 2));
        REQUIRE(sendSettled(sender, 1, 1));
        CHECK(sender.status().registered);
        CHECK(sender.status().width == 4);
        CHECK(sender.status().height == 2);

        spout.pushResult({OutputSpoutResultKind::failed, "resize"});
        REQUIRE(hand(sender, 1, 8, 8));
        REQUIRE(sendSettled(sender, 1, 2));
        CHECK(sender.status().width == 4);
        CHECK(sender.status().height == 2);

        REQUIRE(hand(sender, 1, 8, 8));
        REQUIRE(sendSettled(sender, 2, 2));
        CHECK(sender.status().width == 8);
        CHECK(sender.status().height == 8);
        CHECK(sender.status().last_error.empty());

        sender.requestStop();
        REQUIRE(waitUntil([&] { return sender.finished(); }));
        CHECK_FALSE(sender.status().registered);
    }
    CHECK(spout.read(&FakeSpout::closes) == 1);
}

TEST_CASE("VO-4 a refusal from outside stops sending and reports once, off the worker",
          "[vo4]") {
    FakeSpout spout;
    std::atomic<int> reports{0};
    std::atomic<bool> closed_before_report{false};
    std::thread::id report_thread;
    auto opts = options(spout);
    opts.on_refused = [&](const OutputSpoutSender &refused) {
        closed_before_report = spout.read(&FakeSpout::closes) == 1 &&
                               !refused.status().registered;
        report_thread = std::this_thread::get_id();
        ++reports;
    };
    {
        OutputSpoutSender sender{std::move(opts)};
        REQUIRE(hand(sender, 1));
        REQUIRE(sendSettled(sender, 1, 0));
        sender.refuse("output.format_unsupported@1: VK_FORMAT_R16G16B16A16_SFLOAT");
        REQUIRE(waitUntil([&] { return reports.load() == 1; }));
        const auto status = sender.status();
        CHECK(status.state == "refused");
        CHECK(status.last_error ==
              "output.format_unsupported@1: VK_FORMAT_R16G16B16A16_SFLOAT");
        CHECK(closed_before_report.load());
        CHECK(report_thread != std::this_thread::get_id());
        CHECK_FALSE(sender.acquire(2, 2, true));
    }
    CHECK(reports.load() == 1);
}

TEST_CASE("VO-4 one sender stuck in a send doesn't stop another", "[vo4]") {
    FakeSpout stuck;
    FakeSpout free_spout;
    stuck.setBlockSend(true);
    OutputSpoutSender blocked{options(stuck, "stuck")};
    OutputSpoutSender other{options(free_spout, "free")};
    REQUIRE(hand(blocked, 1));
    REQUIRE(waitUntil([&] { return stuck.read(&FakeSpout::in_send) == 1; }));
    for (std::uint64_t index = 1; index <= 8; ++index) {
        REQUIRE(hand(other, 2));
        REQUIRE(sendSettled(other, index, 0));
    }
    CHECK(blocked.status().frames_written == 0);
    stuck.setBlockSend(false);
    REQUIRE(sendSettled(blocked, 1, 0));
}
