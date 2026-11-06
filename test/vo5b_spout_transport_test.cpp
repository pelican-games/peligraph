// VO-5b: everything above the GPU boundary with a fake backend, on every platform:
// routes, bridge states, the mailbox, fallback, retry, quarantine, stop and status.
// Nothing here sleeps; a wait is a bounded poll of what it waits for. All of this
// is a fake: it says nothing about real GPU work.
#include "../src/core/output/outputbridgecommands.hpp"
#include "../src/core/output/outputspout.hpp"
#include "../src/core/output/outputsinktable.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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
using Pelican::OutputSpoutFrame;
using Pelican::OutputSpoutGpuFailure;
using Pelican::OutputSpoutGpuFrame;
using Pelican::OutputSpoutGpuStep;
using Pelican::OutputSpoutResult;
using Pelican::OutputSpoutResultKind;
using Pelican::OutputSpoutRoute;
using Pelican::OutputSpoutSender;
using Pelican::OutputSpoutSentBridge;
using Pelican::OutputSpoutSentState;
using Pelican::OutputSpoutSourceFormat;
using TimePoint = OutputSpoutSender::TimePoint;
constexpr auto bgra = OutputSpoutSourceFormat::bgra8_unorm;

template <class Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds limit = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// What one test's fake saw and was told to do.
struct Fake {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> events;
    std::set<std::thread::id> threads;
    std::deque<OutputSpoutGpuStep> prepare_results;
    std::deque<OutputSpoutGpuStep> create_results;
    std::deque<OutputSpoutSentBridge> send_results;
    std::deque<OutputSpoutSentState> sent_states;
    OutputSpoutResult open_result;
    std::string actual_name;
    bool block_send_bridge = false;
    bool block_wait_sent = false;
    bool block_create = false;
    bool block_cpu_send = false;
    int in_cpu_send = 0;
    // waitSent blocks from this call on, counting from 0, until it's lowered.
    int block_wait_from = 1 << 30;
    bool cpu_only = false;
    int in_send_bridge = 0;
    int in_wait_sent = 0;
    int in_create = 0;
    int prepares = 0;
    int creates = 0;
    int bridge_sends = 0;
    int wait_sents = 0;
    int cpu_sends = 0;
    int destroys = 0;
    int releases = 0;
    int abandons = 0;
    int opens = 0;
    int closes = 0;
    std::vector<std::uint64_t> sent_values;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> created;

    void note(const std::string &event) {
        std::lock_guard lock{mutex};
        events.push_back(event);
        threads.insert(std::this_thread::get_id());
    }
    template <class T> T read(T Fake::*member) const {
        std::lock_guard lock{mutex};
        return this->*member;
    }
    void set(bool Fake::*member, bool value) {
        std::lock_guard lock{mutex};
        this->*member = value;
        cv.notify_all();
    }
    std::vector<std::string> log() const {
        std::lock_guard lock{mutex};
        return events;
    }
    bool wasIn(const std::string &event) const {
        const auto all = log();
        return std::find(all.begin(), all.end(), event) != all.end();
    }
    std::ptrdiff_t indexOf(const std::string &event) const {
        const auto all = log();
        return std::find(all.begin(), all.end(), event) - all.begin();
    }
};

VkImage fakeImage(std::size_t index) {
    return reinterpret_cast<VkImage>(static_cast<std::uintptr_t>(0x1000 + index));
}

class FakeBackend final : public Pelican::OutputSpoutBackend, public Pelican::OutputSpoutGpuBackend {
  public:
    explicit FakeBackend(Fake &fake) : fake_(fake) {}
    OutputSpoutGpuBackend *gpu() override { return fake_.cpu_only ? nullptr : this; }

    OutputSpoutResult open(const std::string &name) override {
        fake_.note("open");
        std::lock_guard lock{fake_.mutex};
        ++fake_.opens;
        name_ = name;
        return fake_.open_result;
    }
    OutputSpoutResult send(const std::uint8_t *, std::uint32_t, std::uint32_t) override {
        fake_.note("cpu_send");
        std::unique_lock lock{fake_.mutex};
        ++fake_.in_cpu_send;
        fake_.cv.notify_all();
        fake_.cv.wait(lock, [&] { return !fake_.block_cpu_send; });
        --fake_.in_cpu_send;
        ++fake_.cpu_sends;
        return {};
    }
    std::string actualName() const override {
        std::lock_guard lock{fake_.mutex};
        return fake_.actual_name.empty() ? name_ : fake_.actual_name;
    }
    void close() override {
        fake_.note("close");
        std::lock_guard lock{fake_.mutex};
        ++fake_.closes;
    }

    OutputSpoutGpuStep prepare(const Pelican::OutputSpoutGpuRequest &, VkSemaphore &semaphore) override {
        fake_.note("prepare");
        std::lock_guard lock{fake_.mutex};
        ++fake_.prepares;
        semaphore = reinterpret_cast<VkSemaphore>(static_cast<std::uintptr_t>(0x77));
        if (fake_.prepare_results.empty()) return {};
        auto result = fake_.prepare_results.front();
        fake_.prepare_results.pop_front();
        return result;
    }
    OutputSpoutGpuStep createBridges(
        std::uint32_t width, std::uint32_t height,
        std::array<VkImage, Pelican::output_spout_bridge_count> &images) override {
        fake_.note("create");
        std::unique_lock lock{fake_.mutex};
        ++fake_.in_create;
        fake_.cv.notify_all();
        fake_.cv.wait(lock, [&] { return !fake_.block_create; });
        --fake_.in_create;
        ++fake_.creates;
        fake_.created.emplace_back(width, height);
        for (std::size_t index = 0; index < images.size(); ++index) images[index] = fakeImage(index);
        if (fake_.create_results.empty()) return {};
        auto result = fake_.create_results.front();
        fake_.create_results.pop_front();
        return result;
    }
    OutputSpoutSentBridge sendBridge(std::size_t, std::uint64_t wait_value) override {
        fake_.note("send_bridge");
        std::unique_lock lock{fake_.mutex};
        ++fake_.in_send_bridge;
        fake_.cv.notify_all();
        fake_.cv.wait(lock, [&] { return !fake_.block_send_bridge; });
        --fake_.in_send_bridge;
        ++fake_.bridge_sends;
        fake_.sent_values.push_back(wait_value);
        if (fake_.send_results.empty()) return {{}, true};
        auto result = fake_.send_results.front();
        fake_.send_results.pop_front();
        return result;
    }
    OutputSpoutSentState waitSent(std::chrono::milliseconds) override {
        fake_.note("wait_sent");
        std::unique_lock lock{fake_.mutex};
        ++fake_.in_wait_sent;
        fake_.cv.notify_all();
        const auto this_call = fake_.wait_sents;
        fake_.cv.wait(lock, [&] {
            return !fake_.block_wait_sent && this_call < fake_.block_wait_from;
        });
        --fake_.in_wait_sent;
        ++fake_.wait_sents;
        if (fake_.sent_states.empty()) return OutputSpoutSentState::done;
        const auto state = fake_.sent_states.front();
        fake_.sent_states.pop_front();
        return state;
    }
    void destroyBridges() override {
        fake_.note("destroy");
        std::lock_guard lock{fake_.mutex};
        ++fake_.destroys;
    }
    void releaseGpu() override {
        fake_.note("release_gpu");
        std::lock_guard lock{fake_.mutex};
        ++fake_.releases;
    }
    void abandonGpu() override {
        fake_.note("abandon");
        std::lock_guard lock{fake_.mutex};
        ++fake_.abandons;
    }

  private:
    Fake &fake_;
    std::string name_;
};

struct Clock {
    std::shared_ptr<std::atomic<std::int64_t>> ms = std::make_shared<std::atomic<std::int64_t>>(0);
    void set(std::int64_t value) { ms->store(value); }
    std::function<TimePoint()> function() const {
        return [now = ms] { return TimePoint{} + std::chrono::milliseconds(now->load()); };
    }
};

std::atomic<int> unique_name{0};

OutputSpoutSender::Options options(Fake &fake, Clock clock = {}) {
    OutputSpoutSender::Options result;
    result.name = "vo5b_" + std::to_string(++unique_name);
    result.factory = [&fake] { return std::make_unique<FakeBackend>(fake); };
    result.clock = clock.function();
    result.gpu.allowed = true;
    result.gpu.max_width = 8192;
    result.gpu.max_height = 8192;
    result.gpu.rgba8_blit_exact = true;
    return result;
}

// Asks until the sender has bridges of this size.
OutputSpoutGpuFrame reserveUntilGpu(OutputSpoutSender &sender, std::uint32_t width = 16,
                                    std::uint32_t height = 16) {
    std::optional<OutputSpoutGpuFrame> frame;
    const auto got = waitUntil([&] {
        auto reservation = sender.reserveGpu(width, height, bgra);
        if (reservation.route != OutputSpoutRoute::gpu) return false;
        frame = reservation.frame;
        return true;
    });
    REQUIRE(got);
    return *frame;
}

void sendCpu(OutputSpoutSender &sender) {
    auto frame = sender.acquire(16, 16, true);
    REQUIRE(frame.has_value());
    sender.publish(std::move(*frame));
}

bool idleAtBridges(OutputSpoutSender &sender) {
    return !sender.status().sending && sender.status().mailbox_depth == 0;
}

OutputSpoutGpuStep failure(OutputSpoutGpuFailure kind, const std::string &reason) {
    return {kind, reason, "the fake's " + reason};
}
} // namespace

TEST_CASE("VO-5b frames go by CPU until bridges exist, then by GPU, and a new size is CPU until remade",
          "[vo5b]") {
    Fake fake;
    OutputSpoutSender sender{options(fake)};
    const auto first = sender.reserveGpu(16, 16, bgra);
    CHECK((first.route == OutputSpoutRoute::cpu || first.route == OutputSpoutRoute::gpu));
    if (first.frame) sender.cancelGpu(*first.frame);
    auto frame = reserveUntilGpu(sender);
    CHECK(sender.status().transport == "gpu");
    CHECK(sender.status().transport_reason == "none");
    CHECK(frame.width == 16);
    CHECK(frame.image != VK_NULL_HANDLE);
    sender.completeGpu(frame);
    REQUIRE(waitUntil([&] { return sender.status().frames_gpu == 1; }));

    // A new size: CPU at once, and the bridges come back for it.
    CHECK(sender.reserveGpu(32, 32, bgra).route == OutputSpoutRoute::cpu);
    auto resized = reserveUntilGpu(sender, 32, 32);
    CHECK(resized.width == 32);
    CHECK(resized.bridge_generation != frame.bridge_generation);
    sender.cancelGpu(resized);
    CHECK(sender.status().gpu_fallbacks == 0);
}

TEST_CASE("VO-5b with both bridges out a frame is busy, dropped and counted once each", "[vo5b]") {
    Fake fake;
    OutputSpoutSender sender{options(fake)};
    const auto a = reserveUntilGpu(sender);
    const auto b = sender.reserveGpu(16, 16, bgra);
    REQUIRE(b.route == OutputSpoutRoute::gpu);
    const auto before = sender.status().frames_dropped;
    const auto busy = sender.reserveGpu(16, 16, bgra);
    CHECK(busy.route == OutputSpoutRoute::busy);
    CHECK_FALSE(busy.frame.has_value());
    CHECK(sender.status().bridge_busy == 1);
    CHECK(sender.status().frames_dropped == before + 1);
    sender.cancelGpu(a);
    sender.cancelGpu(*b.frame);
}

TEST_CASE("VO-5b reserveGpu calls no backend and returns while the backend is blocked", "[vo5b]") {
    Fake fake;
    OutputSpoutSender sender{options(fake)};
    SECTION("in sendBridge") {
        fake.set(&Fake::block_send_bridge, true);
        const auto frame = reserveUntilGpu(sender);
        sender.completeGpu(frame);
        REQUIRE(waitUntil([&] { return fake.read(&Fake::in_send_bridge) == 1; }));
        const auto calls = fake.log().size();
        const auto next = sender.reserveGpu(16, 16, bgra);
        CHECK(next.route == OutputSpoutRoute::gpu);
        CHECK(sender.reserveGpu(16, 16, bgra).route == OutputSpoutRoute::busy);
        CHECK(fake.log().size() == calls);
        sender.cancelGpu(*next.frame);
        fake.set(&Fake::block_send_bridge, false);
    }
    SECTION("in waitSent") {
        fake.set(&Fake::block_wait_sent, true);
        const auto frame = reserveUntilGpu(sender);
        sender.completeGpu(frame);
        REQUIRE(waitUntil([&] { return fake.read(&Fake::in_wait_sent) == 1; }));
        const auto calls = fake.log().size();
        const auto next = sender.reserveGpu(16, 16, bgra);
        CHECK(next.route == OutputSpoutRoute::gpu);
        CHECK(fake.log().size() == calls);
        sender.cancelGpu(*next.frame);
        fake.set(&Fake::block_wait_sent, false);
    }
    SECTION("in createBridges") {
        fake.set(&Fake::block_create, true);
        CHECK(sender.reserveGpu(16, 16, bgra).route == OutputSpoutRoute::cpu);
        REQUIRE(waitUntil([&] {
            (void)sender.reserveGpu(16, 16, bgra);
            return fake.read(&Fake::in_create) == 1;
        }));
        CHECK(sender.reserveGpu(16, 16, bgra).route == OutputSpoutRoute::cpu);
        fake.set(&Fake::block_create, false);
    }
}

TEST_CASE("VO-5b every GPU backend call is on one thread, and not the caller's", "[vo5b]") {
    Fake fake;
    {
        OutputSpoutSender sender{options(fake)};
        const auto frame = reserveUntilGpu(sender);
        sender.completeGpu(frame);
        REQUIRE(waitUntil([&] { return sender.status().frames_gpu == 1; }));
    }
    std::lock_guard lock{fake.mutex};
    CHECK(fake.threads.size() == 1);
    CHECK_FALSE(fake.threads.contains(std::this_thread::get_id()));
}

TEST_CASE("VO-5b signal values strictly increase, and a cancelled one leaves a gap", "[vo5b]") {
    Fake fake;
    OutputSpoutSender sender{options(fake)};
    const auto a = reserveUntilGpu(sender);
    sender.cancelGpu(a);
    const auto b = reserveUntilGpu(sender);
    CHECK(b.signal_value > a.signal_value);
    sender.completeGpu(b);
    REQUIRE(waitUntil([&] { return sender.status().frames_gpu == 1; }));
    const auto c = reserveUntilGpu(sender);
    CHECK(c.signal_value > b.signal_value);
    sender.completeGpu(c);
    REQUIRE(waitUntil([&] { return sender.status().frames_gpu == 2; }));
    CHECK(fake.read(&Fake::sent_values) == std::vector<std::uint64_t>{b.signal_value, c.signal_value});
}

TEST_CASE("VO-5b the mailbox keeps the newest frame of either kind and frees what it replaces",
          "[vo5b]") {
    Fake fake;
    OutputSpoutSender sender{options(fake)};
    // Hold the sender thread inside one send, so frames wait in the mailbox.
    fake.set(&Fake::block_send_bridge, true);
    const auto held = reserveUntilGpu(sender);
    sender.completeGpu(held);
    REQUIRE(waitUntil([&] { return fake.read(&Fake::in_send_bridge) == 1; }));

    SECTION("a GPU frame replaces a GPU frame and frees its bridge, with no send") {
        const auto a = sender.reserveGpu(16, 16, bgra);
        REQUIRE(a.route == OutputSpoutRoute::gpu);
        const auto dropped = sender.status().frames_dropped;
        sender.completeGpu(*a.frame);
        CHECK(sender.status().mailbox_depth == 1);
        // Its bridge is taken: the held one and this one are both out.
        CHECK(sender.reserveGpu(16, 16, bgra).route == OutputSpoutRoute::busy);
        // The CPU frame replaces it: the bridge comes back at once.
        sendCpu(sender);
        CHECK(sender.status().frames_dropped == dropped + 2);
        CHECK(sender.status().mailbox_depth == 1);
        const auto again = sender.reserveGpu(16, 16, bgra);
        CHECK(again.route == OutputSpoutRoute::gpu);
        if (again.frame) sender.cancelGpu(*again.frame);
        fake.set(&Fake::block_send_bridge, false);
        REQUIRE(waitUntil([&] { return fake.read(&Fake::cpu_sends) == 1; }));
        CHECK(fake.read(&Fake::bridge_sends) == 1);
    }
    SECTION("a GPU frame replaces a CPU frame") {
        sendCpu(sender);
        const auto a = sender.reserveGpu(16, 16, bgra);
        REQUIRE(a.route == OutputSpoutRoute::gpu);
        sender.completeGpu(*a.frame);
        CHECK(sender.status().mailbox_depth == 1);
        CHECK(sender.status().peak_mailbox_depth == 1);
        fake.set(&Fake::block_send_bridge, false);
        REQUIRE(waitUntil([&] { return sender.status().frames_gpu == 2; }));
        CHECK(fake.read(&Fake::cpu_sends) == 0);
    }
}

TEST_CASE("VO-5b a GPU frame replacing a GPU frame frees the older bridge and sends the newest",
          "[vo5b]") {
    Fake fake;
    OutputSpoutSender sender{options(fake)};
    // The sender's thread sits in a CPU send, so both bridges can reach the mailbox.
    fake.set(&Fake::block_cpu_send, true);
    const auto first = reserveUntilGpu(sender);
    sender.cancelGpu(first);
    sendCpu(sender);
    REQUIRE(waitUntil([&] { return fake.read(&Fake::in_cpu_send) == 1; }));
    const auto older = sender.reserveGpu(16, 16, bgra);
    const auto newer = sender.reserveGpu(16, 16, bgra);
    REQUIRE(older.route == OutputSpoutRoute::gpu);
    REQUIRE(newer.route == OutputSpoutRoute::gpu);
    const auto dropped = sender.status().frames_dropped;
    sender.completeGpu(*older.frame);
    sender.completeGpu(*newer.frame);
    CHECK(sender.status().frames_dropped == dropped + 1);
    CHECK(sender.status().mailbox_depth == 1);
    // The older one's bridge is back already, with no send.
    const auto freed = sender.reserveGpu(16, 16, bgra);
    REQUIRE(freed.route == OutputSpoutRoute::gpu);
    CHECK(freed.frame->bridge == older.frame->bridge);
    sender.cancelGpu(*freed.frame);
    fake.set(&Fake::block_cpu_send, false);
    REQUIRE(waitUntil([&] { return sender.status().frames_gpu == 1; }));
    CHECK(fake.read(&Fake::sent_values) == std::vector<std::uint64_t>{newer.frame->signal_value});
}

TEST_CASE("VO-5b a bridge is free only after waitSent, never when sendBridge returns", "[vo5b]") {
    Fake fake;
    OutputSpoutSender sender{options(fake)};
    fake.set(&Fake::block_wait_sent, true);
    const auto first = reserveUntilGpu(sender);
    sender.completeGpu(first);
    REQUIRE(waitUntil([&] { return fake.read(&Fake::in_wait_sent) == 1; }));
    CHECK(fake.read(&Fake::bridge_sends) == 1);
    // Bridge 0 has been sent and D3D11 hasn't finished: only bridge 1 is left.
    const auto second = sender.reserveGpu(16, 16, bgra);
    REQUIRE(second.route == OutputSpoutRoute::gpu);
    CHECK(second.frame->bridge != first.bridge);
    CHECK(sender.reserveGpu(16, 16, bgra).route == OutputSpoutRoute::busy);
    sender.cancelGpu(*second.frame);
    fake.set(&Fake::block_wait_sent, false);
    REQUIRE(waitUntil([&] { return sender.status().frames_gpu == 1; }));
}

TEST_CASE("VO-5b a failure at each stage goes on by CPU, once, with its reason and no refusal",
          "[vo5b]") {
    const auto check_fallback = [](Fake &fake, OutputSpoutSender &sender, const std::string &reason) {
        REQUIRE(waitUntil([&] { return sender.status().transport_reason == reason; }));
        const auto status = sender.status();
        INFO(status.transport_detail);
        CHECK(status.transport == "cpu");
        CHECK(status.state == "running");
        CHECK(status.gpu_fallbacks == 1);
        CHECK_FALSE(status.transport_detail.empty());
        CHECK(sender.reserveGpu(16, 16, bgra).route == OutputSpoutRoute::cpu);
        const auto cpu_before = fake.read(&Fake::cpu_sends);
        sendCpu(sender);
        REQUIRE(waitUntil([&] { return fake.read(&Fake::cpu_sends) == cpu_before + 1; }));
        CHECK(sender.status().frames_cpu >= 1);
    };
    SECTION("from prepare, permanent") {
        for (const auto *reason : {"sender_device_mismatch", "d3d11_fence_unavailable",
                                   "semaphore_import_failed"}) {
            CAPTURE(reason);
            Fake fake;
            fake.prepare_results.push_back(failure(OutputSpoutGpuFailure::permanent, reason));
            OutputSpoutSender sender{options(fake)};
            (void)sender.reserveGpu(16, 16, bgra);
            REQUIRE(waitUntil([&] {
                (void)sender.reserveGpu(16, 16, bgra);
                return sender.status().gpu_fallbacks == 1;
            }));
            check_fallback(fake, sender, reason);
        }
    }
    SECTION("from createBridges") {
        for (const auto *reason : {"bridge_create_failed", "bridge_import_failed",
                                   "memory_type_mismatch"}) {
            CAPTURE(reason);
            Fake fake;
            fake.create_results.push_back(failure(OutputSpoutGpuFailure::permanent, reason));
            OutputSpoutSender sender{options(fake)};
            REQUIRE(waitUntil([&] {
                (void)sender.reserveGpu(16, 16, bgra);
                return sender.status().gpu_fallbacks == 1;
            }));
            check_fallback(fake, sender, reason);
        }
    }
    SECTION("a queue submit that threw, with its VkResult deciding") {
        Fake fake;
        OutputSpoutSender sender{options(fake)};
        const auto frame = reserveUntilGpu(sender);
        sender.gpuSubmitFailed(frame, VK_ERROR_OUT_OF_DEVICE_MEMORY, "the fake's submit");
        check_fallback(fake, sender, "capture_submit_failed");
        CHECK(sender.status().gpu_next_retry_ms.has_value());
    }
    SECTION("SendTexture returning false, with its completion still tracked") {
        Fake fake;
        fake.send_results.push_back(
            {{OutputSpoutResultKind::failed, "the fake's false"}, true});
        OutputSpoutSender sender{options(fake)};
        const auto frame = reserveUntilGpu(sender);
        sender.completeGpu(frame);
        check_fallback(fake, sender, "send_failed");
        CHECK(fake.read(&Fake::wait_sents) == 1);
        CHECK(sender.status().frames_gpu == 0);
    }
}

TEST_CASE("VO-5b a timed-out wait frees nothing, and a late finish lets the sink go on by CPU",
          "[vo5b]") {
    Fake fake;
    fake.sent_states = {OutputSpoutSentState::timeout, OutputSpoutSentState::timeout,
                        OutputSpoutSentState::done};
    Clock clock;
    OutputSpoutSender sender{options(fake, clock)};
    const auto frame = reserveUntilGpu(sender);
    sender.completeGpu(frame);
    REQUIRE(waitUntil([&] { return sender.status().sync_timeouts == 1; }));
    REQUIRE(waitUntil([&] { return sender.status().transport_reason == "sync_timeout"; }));
    const auto status = sender.status();
    CHECK(status.state == "running");
    CHECK_FALSE(status.gpu_quarantined);
    CHECK(status.gpu_fallbacks == 1);
    CHECK(fake.read(&Fake::wait_sents) == 3);
    sendCpu(sender);
    REQUIRE(waitUntil([&] { return fake.read(&Fake::cpu_sends) == 1; }));
    // Only after the work finished, and only by the timer.
    CHECK(status.gpu_next_retry_ms.has_value());
    CHECK(fake.read(&Fake::prepares) == 1);
}

TEST_CASE("VO-5b no CPU send and no teardown while a bridge's D3D11 work is unresolved", "[vo5b]") {
    Fake fake;
    fake.sent_states = {OutputSpoutSentState::timeout};
    fake.block_wait_from = 1; // the quarantine's own wait is held
    OutputSpoutSender sender{options(fake)};
    const auto frame = reserveUntilGpu(sender);
    sender.completeGpu(frame);
    REQUIRE(waitUntil([&] { return fake.read(&Fake::in_wait_sent) == 1 &&
                                   sender.status().sync_timeouts == 1; }));
    CHECK(sender.status().gpu_quarantined);
    sendCpu(sender);
    for (int index = 0; index < 1000; ++index) std::this_thread::yield();
    CHECK(fake.read(&Fake::cpu_sends) == 0);
    CHECK(fake.read(&Fake::destroys) == 0);
    CHECK(fake.read(&Fake::prepares) == 1);
    {
        std::lock_guard lock{fake.mutex};
        fake.block_wait_from = 1 << 30;
        fake.cv.notify_all();
    }
    REQUIRE(waitUntil([&] { return fake.read(&Fake::cpu_sends) == 1; }));
    REQUIRE(waitUntil([&] { return fake.read(&Fake::destroys) == 1; }));
    CHECK_FALSE(sender.status().gpu_quarantined);
}

TEST_CASE("VO-5b an operation that never finishes refuses the sink and gives up its device",
          "[vo5b]") {
    Fake fake;
    for (int index = 0; index < 32; ++index)
        fake.sent_states.push_back(OutputSpoutSentState::timeout);
    Clock clock;
    {
        OutputSpoutSender sender{options(fake, clock)};
        const auto frame = reserveUntilGpu(sender);
        sender.completeGpu(frame);
        REQUIRE(waitUntil([&] { return sender.status().state == "refused"; }));
        const auto status = sender.status();
        CHECK_THAT(status.last_error,
                   Catch::Matchers::ContainsSubstring("output.spout_send_failed@1"));
        CHECK(status.gpu_quarantined);
        CHECK(status.transport == "cpu");
        CHECK(status.transport_reason == "sync_timeout");
        CHECK(fake.read(&Fake::wait_sents) == 1 + Pelican::output_spout_quarantine_tries);
        // A CPU frame isn't sent on that context: there's no fallback to report.
        CHECK_FALSE(sender.acquire(16, 16, true).has_value());
        CHECK(fake.read(&Fake::cpu_sends) == 0);
    }
    CHECK(fake.read(&Fake::abandons) == 1);
    CHECK(fake.read(&Fake::destroys) == 0);
    CHECK(fake.read(&Fake::releases) == 0);
    // Nothing retried on resources that may still be referenced.
    CHECK(fake.read(&Fake::prepares) == 1);
    CHECK(fake.read(&Fake::creates) == 1);
}

TEST_CASE("VO-5b a permanent reason is never tried again", "[vo5b]") {
    Fake fake;
    fake.prepare_results.push_back(
        failure(OutputSpoutGpuFailure::permanent, "semaphore_import_failed"));
    Clock clock;
    OutputSpoutSender sender{options(fake, clock)};
    REQUIRE(waitUntil([&] {
        (void)sender.reserveGpu(16, 16, bgra);
        return sender.status().gpu_fallbacks == 1;
    }));
    for (std::int64_t minutes = 1; minutes <= 30; ++minutes) {
        clock.set(minutes * 60000);
        CHECK(sender.reserveGpu(16, 16, bgra).route == OutputSpoutRoute::cpu);
    }
    sendCpu(sender);
    REQUIRE(waitUntil([&] { return fake.read(&Fake::cpu_sends) == 1; }));
    CHECK(fake.read(&Fake::prepares) == 1);
    CHECK_FALSE(sender.status().gpu_next_retry_ms.has_value());
}

TEST_CASE("VO-5b a transient reason retries at 30 s, 150 s and 630 s, then no more", "[vo5b]") {
    Fake fake;
    const auto oom = failure(OutputSpoutGpuFailure::transient, "bridge_create_failed");
    fake.create_results = {oom, oom, oom, oom};
    Clock clock;
    OutputSpoutSender sender{options(fake, clock)};
    REQUIRE(waitUntil([&] {
        (void)sender.reserveGpu(16, 16, bgra);
        return sender.status().gpu_fallbacks == 1;
    }));
    // Teardown done before the clock moves.
    REQUIRE(waitUntil([&] { return fake.read(&Fake::releases) == 1; }));
    CHECK(sender.status().gpu_next_retry_ms == std::uint64_t{30000});

    const auto at = [&](std::int64_t ms) {
        clock.set(ms);
        for (int index = 0; index < 2000; ++index) (void)sender.reserveGpu(16, 16, bgra);
        sendCpu(sender);
        const auto sends = fake.read(&Fake::cpu_sends);
        REQUIRE(waitUntil([&] { return fake.read(&Fake::cpu_sends) >= sends; }));
        REQUIRE(waitUntil([&] { return idleAtBridges(sender); }));
    };
    at(29900);
    CHECK(fake.read(&Fake::prepares) == 1);
    at(30000);
    REQUIRE(waitUntil([&] { return fake.read(&Fake::prepares) == 2; }));
    REQUIRE(waitUntil([&] { return sender.status().gpu_fallbacks == 2; }));
    REQUIRE(waitUntil([&] { return fake.read(&Fake::releases) == 2; }));
    at(149900);
    CHECK(fake.read(&Fake::prepares) == 2);
    at(150000);
    REQUIRE(waitUntil([&] { return fake.read(&Fake::prepares) == 3; }));
    REQUIRE(waitUntil([&] { return sender.status().gpu_fallbacks == 3; }));
    REQUIRE(waitUntil([&] { return fake.read(&Fake::releases) == 3; }));
    at(630000);
    REQUIRE(waitUntil([&] { return fake.read(&Fake::prepares) == 4; }));
    REQUIRE(waitUntil([&] { return sender.status().gpu_fallbacks == 4; }));
    REQUIRE(waitUntil([&] { return fake.read(&Fake::releases) == 4; }));
    CHECK_FALSE(sender.status().gpu_next_retry_ms.has_value());
    at(10000000);
    CHECK(fake.read(&Fake::prepares) == 4);
}

TEST_CASE("VO-5b thousands of reservations during the wait cause no attempt", "[vo5b]") {
    Fake fake;
    fake.create_results.push_back(failure(OutputSpoutGpuFailure::transient, "bridge_create_failed"));
    Clock clock;
    OutputSpoutSender sender{options(fake, clock)};
    REQUIRE(waitUntil([&] {
        (void)sender.reserveGpu(16, 16, bgra);
        return sender.status().gpu_fallbacks == 1;
    }));
    REQUIRE(waitUntil([&] { return fake.read(&Fake::releases) == 1; }));
    for (int index = 0; index < 5000; ++index) (void)sender.reserveGpu(16 + index % 3, 16, bgra);
    sendCpu(sender);
    REQUIRE(waitUntil([&] { return fake.read(&Fake::cpu_sends) == 1; }));
    CHECK(fake.read(&Fake::prepares) == 1);
    CHECK(fake.read(&Fake::creates) == 1);
}

TEST_CASE("VO-5b 60 s of GPU sending clears the failure count", "[vo5b]") {
    Fake fake;
    const auto oom = failure(OutputSpoutGpuFailure::transient, "bridge_create_failed");
    fake.create_results = {oom};
    Clock clock;
    OutputSpoutSender sender{options(fake, clock)};
    REQUIRE(waitUntil([&] {
        (void)sender.reserveGpu(16, 16, bgra);
        return sender.status().gpu_fallbacks == 1;
    }));
    REQUIRE(waitUntil([&] { return fake.read(&Fake::releases) == 1; }));
    clock.set(30000);
    const auto frame = reserveUntilGpu(sender);
    clock.set(90000);
    sender.completeGpu(frame);
    REQUIRE(waitUntil([&] { return sender.status().frames_gpu == 1; }));
    // A new failure waits 30 s again, not 120 s.
    const auto failing = reserveUntilGpu(sender);
    sender.gpuSubmitFailed(failing, VK_ERROR_OUT_OF_HOST_MEMORY, "the fake's submit");
    REQUIRE(sender.status().gpu_next_retry_ms.has_value());
    CHECK(*sender.status().gpu_next_retry_ms == 30000);
}

TEST_CASE("VO-5b an unsupported extent is looked at again only for a new extent", "[vo5b]") {
    Fake fake;
    auto settings = options(fake);
    settings.gpu.max_width = 64;
    settings.gpu.max_height = 64;
    OutputSpoutSender sender{std::move(settings)};
    REQUIRE(waitUntil([&] {
        (void)sender.reserveGpu(128, 16, bgra);
        return sender.status().transport_reason == "extent_unsupported";
    }));
    CHECK(sender.status().gpu_fallbacks == 0);
    CHECK(fake.read(&Fake::creates) == 0);
    for (int index = 0; index < 100; ++index) (void)sender.reserveGpu(128, 16, bgra);
    CHECK(fake.read(&Fake::creates) == 0);
    const auto frame = reserveUntilGpu(sender, 32, 16);
    CHECK(sender.status().transport_reason == "none");
    sender.cancelGpu(frame);
}

TEST_CASE("VO-5b a rebuild waits for the old bridges, and sizes that come meanwhile coalesce",
          "[vo5b]") {
    Fake fake;
    OutputSpoutSender sender{options(fake)};
    const auto held = reserveUntilGpu(sender);
    CHECK(sender.reserveGpu(32, 32, bgra).route == OutputSpoutRoute::cpu);
    sendCpu(sender);
    REQUIRE(waitUntil([&] { return fake.read(&Fake::cpu_sends) == 1; }));
    // A reservation of the old size is out: nothing is destroyed or built yet.
    CHECK(fake.read(&Fake::destroys) == 0);
    CHECK(fake.read(&Fake::creates) == 1);
    CHECK(sender.reserveGpu(48, 48, bgra).route == OutputSpoutRoute::cpu);
    CHECK(sender.reserveGpu(64, 64, bgra).route == OutputSpoutRoute::cpu);
    sender.cancelGpu(held);
    const auto frame = reserveUntilGpu(sender, 64, 64);
    CHECK(fake.read(&Fake::destroys) == 1);
    const auto created = fake.read(&Fake::created);
    CHECK(created.back() == std::make_pair(std::uint32_t{64}, std::uint32_t{64}));
    CHECK(created.size() == 2);
    sender.cancelGpu(frame);
    CHECK(sender.status().gpu_fallbacks == 0);
}

TEST_CASE("VO-5b a frame of an old bridge generation is cancelled, never sent", "[vo5b]") {
    Fake fake;
    OutputSpoutSender sender{options(fake)};
    const auto first = reserveUntilGpu(sender);
    auto stale = first;
    stale.bridge_generation -= 1;
    sender.completeGpu(stale);
    REQUIRE(waitUntil([&] { return sender.status().frames_discarded == 1; }));
    CHECK(fake.read(&Fake::bridge_sends) == 0);
}

TEST_CASE("VO-5b a removed device: bridges go, close and open once, and only a CPU send confirms",
          "[vo5b]") {
    SECTION("recovers") {
        Fake fake;
        fake.send_results.push_back(
            {{OutputSpoutResultKind::device_removed, "the fake's removal"}, false});
        OutputSpoutSender sender{options(fake)};
        const auto frame = reserveUntilGpu(sender);
        sender.completeGpu(frame);
        REQUIRE(waitUntil([&] { return fake.read(&Fake::opens) == 2; }));
        const auto status = sender.status();
        CHECK(status.transport == "cpu");
        CHECK(status.transport_reason == "d3d11_device_removed");
        CHECK(status.state == "running");
        CHECK(fake.read(&Fake::closes) == 1);
        CHECK(fake.indexOf("destroy") < fake.indexOf("release_gpu"));
        CHECK(fake.indexOf("release_gpu") < fake.indexOf("close"));
        CHECK_THAT(status.transport_detail,
                   !Catch::Matchers::ContainsSubstring("CPU sending works"));
        sendCpu(sender);
        REQUIRE(waitUntil([&] { return fake.read(&Fake::cpu_sends) == 1; }));
        REQUIRE(waitUntil([&] {
            return sender.status().transport_detail.find("CPU sending works") != std::string::npos;
        }));
    }
    SECTION("a changed name refuses") {
        Fake fake;
        fake.send_results.push_back(
            {{OutputSpoutResultKind::device_removed, "the fake's removal"}, false});
        OutputSpoutSender sender{options(fake)};
        const auto frame = reserveUntilGpu(sender);
        {
            std::lock_guard lock{fake.mutex};
            fake.actual_name = "taken_1";
        }
        sender.completeGpu(frame);
        REQUIRE(waitUntil([&] { return sender.status().state == "refused"; }));
        CHECK_THAT(sender.status().last_error,
                   Catch::Matchers::ContainsSubstring("output.spout_name_taken@1"));
    }
    SECTION("from waitSent, and a second removal refuses") {
        Fake fake;
        fake.sent_states.push_back(OutputSpoutSentState::device_removed);
        fake.send_results.push_back({{}, true});
        fake.send_results.push_back(
            {{OutputSpoutResultKind::device_removed, "the second"}, false});
        Clock clock;
        OutputSpoutSender sender{options(fake, clock)};
        const auto frame = reserveUntilGpu(sender);
        sender.completeGpu(frame);
        REQUIRE(waitUntil([&] { return fake.read(&Fake::opens) == 2; }));
        clock.set(30000);
        const auto next = reserveUntilGpu(sender);
        sender.completeGpu(next);
        REQUIRE(waitUntil([&] { return sender.status().state == "refused"; }));
        CHECK(fake.read(&Fake::opens) == 2);
    }
}

TEST_CASE("VO-5b stop waits for a pending reservation, then bridges, GPU state and close in order",
          "[vo5b]") {
    Fake fake;
    auto sender = std::make_unique<OutputSpoutSender>(options(fake));
    const auto frame = reserveUntilGpu(*sender);
    sender->requestStop();
    // The stop request itself doesn't wait, and nothing goes while the frame is out.
    REQUIRE_FALSE(sender->finished());
    for (int index = 0; index < 1000; ++index) std::this_thread::yield();
    CHECK(fake.read(&Fake::destroys) == 0);
    CHECK(fake.read(&Fake::closes) == 0);
    CHECK_FALSE(sender->release().done());
    sender->cancelGpu(frame);
    REQUIRE(waitUntil([&] { return sender->finished(); }));
    CHECK(fake.indexOf("destroy") < fake.indexOf("release_gpu"));
    CHECK(fake.indexOf("release_gpu") < fake.indexOf("close"));
    CHECK(fake.read(&Fake::destroys) == 1);
    sender.reset();
    CHECK(fake.read(&Fake::closes) == 1);
}

TEST_CASE("VO-5b a new sender with the name waits for the old release, held by a reservation",
          "[vo5b]") {
    Fake old_fake;
    Fake new_fake;
    auto first_options = options(old_fake);
    const auto name = first_options.name;
    auto first = std::make_unique<OutputSpoutSender>(std::move(first_options));
    const auto frame = reserveUntilGpu(*first);
    first->requestStop();
    auto second_options = options(new_fake);
    second_options.name = name;
    second_options.predecessor = first->release();
    OutputSpoutSender second{std::move(second_options)};
    for (int index = 0; index < 1000; ++index) std::this_thread::yield();
    CHECK(new_fake.read(&Fake::opens) == 0);
    first->cancelGpu(frame);
    REQUIRE(waitUntil([&] { return new_fake.read(&Fake::opens) == 1; }));
    CHECK(old_fake.read(&Fake::closes) == 1);
    CHECK(old_fake.read(&Fake::destroys) == 1);
}

TEST_CASE("VO-5b each source format takes its route", "[vo5b]") {
    using Format = OutputSpoutSourceFormat;
    SECTION("with the blit verified") {
        Fake fake;
        OutputSpoutSender sender{options(fake)};
        const auto bgra_unorm = reserveUntilGpu(sender);
        CHECK_FALSE(bgra_unorm.blit);
        const auto bgra_srgb = sender.reserveGpu(16, 16, Format::bgra8_srgb);
        REQUIRE(bgra_srgb.route == OutputSpoutRoute::gpu);
        CHECK_FALSE(bgra_srgb.frame->blit);
        sender.cancelGpu(bgra_unorm);
        const auto rgba_unorm = sender.reserveGpu(16, 16, Format::rgba8_unorm);
        REQUIRE(rgba_unorm.route == OutputSpoutRoute::gpu);
        CHECK(rgba_unorm.frame->blit);
        sender.cancelGpu(*bgra_srgb.frame);
        sender.cancelGpu(*rgba_unorm.frame);
        CHECK(sender.reserveGpu(16, 16, Format::rgba8_srgb).route == OutputSpoutRoute::cpu);
        CHECK(sender.status().transport == "cpu");
        CHECK(sender.status().transport_reason == "format_needs_cpu");
        CHECK(sender.status().gpu_fallbacks == 0);
    }
    SECTION("RGBA8 sRGB never asks for bridges") {
        Fake fake;
        OutputSpoutSender sender{options(fake)};
        REQUIRE(waitUntil([&] {
            (void)sender.reserveGpu(16, 16, Format::rgba8_srgb);
            return sender.status().transport_reason == "format_needs_cpu";
        }));
        sendCpu(sender);
        REQUIRE(waitUntil([&] { return fake.read(&Fake::cpu_sends) == 1; }));
        CHECK(fake.read(&Fake::prepares) == 0);
        CHECK(fake.read(&Fake::creates) == 0);
    }
    SECTION("an unverified blit keeps RGBA8 UNORM on CPU, and BGRA8 isn't affected") {
        Fake fake;
        auto settings = options(fake);
        settings.gpu.rgba8_blit_exact = false;
        OutputSpoutSender sender{std::move(settings)};
        CHECK(sender.reserveGpu(16, 16, Format::rgba8_unorm).route == OutputSpoutRoute::cpu);
        REQUIRE(waitUntil([&] {
            (void)sender.reserveGpu(16, 16, Format::rgba8_unorm);
            return sender.status().transport_reason == "format_needs_cpu";
        }));
        const auto frame = reserveUntilGpu(sender);
        CHECK_FALSE(frame.blit);
        sender.cancelGpu(frame);
    }
}

TEST_CASE("VO-5b a gate that's closed, or a CPU-only backend, means VO-4's behaviour", "[vo5b]") {
    SECTION("a CPU-only backend") {
        Fake fake;
        fake.cpu_only = true;
        OutputSpoutSender sender{options(fake)};
        REQUIRE(waitUntil([&] { return sender.status().transport_reason == "backend_cpu_only"; }));
        CHECK(sender.reserveGpu(16, 16, bgra).route == OutputSpoutRoute::cpu);
        sendCpu(sender);
        REQUIRE(waitUntil([&] { return fake.read(&Fake::cpu_sends) == 1; }));
        CHECK(fake.read(&Fake::prepares) == 0);
        CHECK(sender.status().frames_written == 1);
        CHECK(sender.status().frames_cpu == 1);
    }
    SECTION("the preflight or the flag") {
        Fake fake;
        auto settings = options(fake);
        settings.gpu.allowed = false;
        settings.gpu.gate_reason = "cpu_requested";
        OutputSpoutSender sender{std::move(settings)};
        REQUIRE(waitUntil([&] { return sender.status().transport_reason == "cpu_requested"; }));
        CHECK(sender.reserveGpu(16, 16, bgra).route == OutputSpoutRoute::cpu);
        CHECK(fake.read(&Fake::prepares) == 0);
        CHECK(sender.status().gpu_fallbacks == 0);
    }
}

TEST_CASE("VO-5b frames_written is the sum of the two routes' submissions", "[vo5b]") {
    Fake fake;
    OutputSpoutSender sender{options(fake)};
    sendCpu(sender);
    REQUIRE(waitUntil([&] { return fake.read(&Fake::cpu_sends) == 1; }));
    const auto frame = reserveUntilGpu(sender);
    sender.completeGpu(frame);
    REQUIRE(waitUntil([&] { return sender.status().frames_gpu == 1; }));
    const auto status = sender.status();
    CHECK(status.frames_written == status.frames_gpu + status.frames_cpu);
    CHECK(status.frames_cpu == 1);
    CHECK(status.bridge_cycle.count == 1);
    CHECK(status.d3d11_completion.count == 1);
    CHECK(status.capture_to_send_gpu.count == 1);
}

// The structure only: this GPU behaves the same without these, as the report says.
TEST_CASE("VO-5b the bridge's barriers are the external-ownership acquire and release",
          "[vo5b]") {
    const vk::Image image{fakeImage(1)};
    constexpr std::uint32_t family = 3;
    const vk::ImageSubresourceRange whole{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

    const auto acquire = Pelican::outputBridgeAcquireBarrier(image, family);
    CHECK(acquire.image.srcQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL);
    CHECK(acquire.image.dstQueueFamilyIndex == family);
    CHECK(acquire.image.oldLayout == vk::ImageLayout::eUndefined);
    CHECK(acquire.image.newLayout == vk::ImageLayout::eTransferDstOptimal);
    CHECK(acquire.image.srcAccessMask == vk::AccessFlags{});
    CHECK(acquire.image.dstAccessMask == vk::AccessFlagBits::eTransferWrite);
    CHECK(acquire.destination_stage == vk::PipelineStageFlagBits::eTransfer);
    CHECK(acquire.image.subresourceRange == whole);
    CHECK(acquire.image.image == image);

    const auto release = Pelican::outputBridgeReleaseBarrier(image, family);
    CHECK(release.image.srcQueueFamilyIndex == family);
    CHECK(release.image.dstQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL);
    CHECK(release.image.oldLayout == vk::ImageLayout::eTransferDstOptimal);
    CHECK(release.image.newLayout == vk::ImageLayout::eGeneral);
    CHECK(release.image.srcAccessMask == vk::AccessFlagBits::eTransferWrite);
    CHECK(release.image.dstAccessMask == vk::AccessFlags{});
    CHECK(release.source_stage == vk::PipelineStageFlagBits::eTransfer);
    CHECK(release.image.subresourceRange == whole);
}

TEST_CASE("VO-5b failures are classified by the value, not by the stage", "[vo5b]") {
    using Pelican::outputSpoutClassifyHresult;
    using Pelican::outputSpoutClassifyVkResult;
    constexpr auto hresult = [](std::uint32_t value) { return static_cast<std::int32_t>(value); };
    CHECK(outputSpoutClassifyVkResult(VK_ERROR_OUT_OF_HOST_MEMORY) == OutputSpoutGpuFailure::transient);
    CHECK(outputSpoutClassifyVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY) == OutputSpoutGpuFailure::transient);
    CHECK(outputSpoutClassifyVkResult(VK_ERROR_INVALID_EXTERNAL_HANDLE) == OutputSpoutGpuFailure::permanent);
    CHECK(outputSpoutClassifyVkResult(VK_ERROR_FORMAT_NOT_SUPPORTED) == OutputSpoutGpuFailure::permanent);
    CHECK(outputSpoutClassifyVkResult(VK_ERROR_INITIALIZATION_FAILED) == OutputSpoutGpuFailure::permanent);
    CHECK(outputSpoutClassifyHresult(hresult(0x8007000Eu)) == OutputSpoutGpuFailure::transient);
    CHECK(outputSpoutClassifyHresult(hresult(0x80070057u)) == OutputSpoutGpuFailure::permanent);
    CHECK(outputSpoutClassifyHresult(hresult(0x80004002u)) == OutputSpoutGpuFailure::permanent);
    CHECK(outputSpoutClassifyHresult(hresult(0x887A0005u)) == OutputSpoutGpuFailure::device_removed);
    CHECK(outputSpoutClassifyHresult(hresult(0x887A0006u)) == OutputSpoutGpuFailure::device_removed);
    CHECK(outputSpoutClassifyHresult(hresult(0x887A0007u)) == OutputSpoutGpuFailure::device_removed);
}
