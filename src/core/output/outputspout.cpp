#include "outputspout.hpp"

#include "../log.hpp"
#include "outputsinktable.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace Pelican {
namespace {

// A sender publishes lazily, so two threads asking Spout about one name could
// both be told it's free. This set makes the question atomic inside the process.
std::mutex reservation_mutex;
std::set<std::string> &reservedNames() {
    static std::set<std::string> names;
    return names;
}
bool reserveName(const std::string &name) {
    std::lock_guard lock{reservation_mutex};
    return reservedNames().insert(name).second;
}
void releaseName(const std::string &name) {
    std::lock_guard lock{reservation_mutex};
    reservedNames().erase(name);
}

std::optional<std::size_t> frameBytes(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return std::nullopt;
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > (std::numeric_limits<std::size_t>::max)() / 4) return std::nullopt;
    return static_cast<std::size_t>(pixels * 4);
}

#if PELICAN_OUTPUT_TEST_SEAMS
constexpr const char *test_backend_variable = "PELICAN_OUTPUT_SPOUT_TEST_BACKEND";

struct TestBackendState {
    std::mutex mutex;
    std::condition_variable cv;
    bool block_send = false;
    bool block_close = false;
    bool send_blocked = false;
    bool close_blocked = false;
    std::uint64_t opens = 0;
    std::uint64_t sends = 0;
    std::uint64_t closes = 0;
    nlohmann::json events = nlohmann::json::array();
    std::vector<std::uint8_t> last_pixels;
    std::uint32_t last_width = 0;
    std::uint32_t last_height = 0;
};
TestBackendState &testBackendState() {
    static TestBackendState state;
    return state;
}

class TestBackend final : public OutputSpoutBackend {
  public:
    explicit TestBackend(bool fail_open) : fail_open_(fail_open) {}
    OutputSpoutResult open(const std::string &name) override {
        auto &state = testBackendState();
        std::lock_guard lock{state.mutex};
        name_ = name;
        ++state.opens;
        state.events.push_back("open:" + name);
        if (fail_open_)
            return {OutputSpoutResultKind::open_failed, "test backend"};
        return {};
    }
    OutputSpoutResult send(const std::uint8_t *bgra, std::uint32_t width,
                           std::uint32_t height) override {
        auto &state = testBackendState();
        std::unique_lock lock{state.mutex};
        state.send_blocked = state.block_send;
        state.cv.notify_all();
        state.cv.wait(lock, [&] { return !state.block_send; });
        state.send_blocked = false;
        ++state.sends;
        state.events.push_back("send:" + std::to_string(width) + "x" + std::to_string(height));
        state.last_pixels.assign(bgra, bgra + static_cast<std::size_t>(width) * height * 4);
        state.last_width = width;
        state.last_height = height;
        return {};
    }
    std::string actualName() const override { return name_; }
    void close() override {
        auto &state = testBackendState();
        std::unique_lock lock{state.mutex};
        state.events.push_back("close_begin:" + name_);
        state.close_blocked = state.block_close;
        state.cv.notify_all();
        state.cv.wait(lock, [&] { return !state.block_close; });
        state.close_blocked = false;
        ++state.closes;
        state.events.push_back("close:" + name_);
    }

  private:
    bool fail_open_ = false;
    std::string name_;
};
#endif
} // namespace

OutputSpoutActivation outputSpoutActivation(bool enabled, bool armed,
                                            bool offline, bool available) {
    if (!enabled || !armed)
        return {.start = false, .state = "inactive", .stop_reason = "not_enabled"};
    if (offline)
        return {.start = false, .state = "inactive", .stop_reason = "offline"};
    if (!available)
        return {.start = false, .state = "refused",
                .error = "output.spout_unavailable@1: this build has no Spout"};
    return {.start = true, .state = "running"};
}

bool outputSpoutAvailable() {
#if PELICAN_OUTPUT_TEST_SEAMS
    if (std::getenv(test_backend_variable) != nullptr) return true;
#endif
    return PELICAN_WITH_SPOUT != 0;
}

OutputSpoutGpuFailure outputSpoutClassifyVkResult(std::int32_t result) {
    switch (result) {
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_TOO_MANY_OBJECTS:
    case VK_ERROR_FRAGMENTATION:
    case VK_ERROR_OUT_OF_POOL_MEMORY:
        return OutputSpoutGpuFailure::transient;
    default:
        return OutputSpoutGpuFailure::permanent;
    }
}

OutputSpoutGpuFailure outputSpoutClassifyHresult(std::int32_t result) {
    // Spelled out so this file sees no Windows header.
    constexpr auto hresult = [](std::uint32_t value) {
        return static_cast<std::int32_t>(value);
    };
    if (result == hresult(0x887A0005u) /* DXGI_ERROR_DEVICE_REMOVED */ ||
        result == hresult(0x887A0006u) /* DXGI_ERROR_DEVICE_HUNG */ ||
        result == hresult(0x887A0007u) /* DXGI_ERROR_DEVICE_RESET */ ||
        result == hresult(0x887A0020u) /* DXGI_ERROR_DRIVER_INTERNAL_ERROR */)
        return OutputSpoutGpuFailure::device_removed;
    if (result == hresult(0x8007000Eu) /* E_OUTOFMEMORY */ ||
        result == hresult(0x887C0001u) /* D3D11 too many unique state objects */ ||
        result == hresult(0x887A000Au) /* DXGI_ERROR_WAS_STILL_DRAWING */)
        return OutputSpoutGpuFailure::transient;
    return OutputSpoutGpuFailure::permanent;
}

OutputSpoutBackendFactory outputSpoutBackendFactory(OutputSpoutGpuRequest request) {
#if PELICAN_OUTPUT_TEST_SEAMS
    if (const char *mode = std::getenv(test_backend_variable)) {
        const bool fail_open = std::string{mode} == "fail_open";
        return [fail_open] { return std::make_unique<TestBackend>(fail_open); };
    }
#endif
#if PELICAN_WITH_SPOUT
    return outputSpoutRealBackendFactory(std::move(request));
#else
    (void)request;
    return {};
#endif
}

void outputSpoutSwapRedBlue(std::uint8_t *pixels, std::size_t bytes) {
    for (std::size_t at = 0; at + 3 < bytes; at += 4)
        std::swap(pixels[at], pixels[at + 2]);
}

struct OutputSpoutRelease::State {
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    bool done = false;
};
OutputSpoutRelease::OutputSpoutRelease() : state_(std::make_shared<State>()) {}
void OutputSpoutRelease::acknowledge() {
    {
        std::lock_guard lock{state_->mutex};
        state_->done = true;
    }
    state_->cv.notify_all();
}
void OutputSpoutRelease::wait() const {
    std::unique_lock lock{state_->mutex};
    state_->cv.wait(lock, [&] { return state_->done; });
}
bool OutputSpoutRelease::done() const {
    std::lock_guard lock{state_->mutex};
    return state_->done;
}

struct OutputSpoutSender::Impl {
    const OutputSpoutSender &owner;
    const std::string name;
    OutputSpoutBackendFactory factory;
    std::optional<OutputSpoutRelease> predecessor;
    std::function<TimePoint()> clock;
    std::function<void(const OutputSpoutSender &)> on_refused;
    OutputSpoutRelease released;
    const OutputSpoutGpuRequest gpu_request;

    // Sender's thread only, once it's open.
    OutputSpoutGpuBackend *gpu_backend = nullptr;

    // Under the mutex.
    enum class BridgeState : std::uint8_t { free, gpu_pending, ready, sending };
    struct Bridge {
        BridgeState state = BridgeState::free;
        VkImage image = VK_NULL_HANDLE;
        TimePoint reserved_at{};
    };
    std::array<Bridge, output_spout_bridge_count> bridges{};
    bool bridges_live = false;
    std::uint32_t bridge_width = 0;
    std::uint32_t bridge_height = 0;
    std::uint64_t bridge_generation = 0;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    std::uint64_t next_signal_value = 1;
    std::size_t outstanding = 0;
    std::optional<OutputSpoutGpuFrame> gpu_mailbox;
    bool gpu_permanent = true; // until the sender's thread has opened the gate
    bool gpu_prepared = false;
    bool teardown_pending = false;
    bool reopen_pending = false;
    bool reopened = false;
    bool awaiting_cpu_confirm = false;
    bool abandoned = false;
    bool format_needs_cpu = false;
    std::uint32_t wanted_width = 0;
    std::uint32_t wanted_height = 0;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> bad_extent;
    std::size_t transient_failures = 0;
    std::optional<TimePoint> next_attempt;
    std::optional<TimePoint> gpu_since;

    mutable std::mutex mutex;
    std::condition_variable cv;
    OutputSpoutStatus current;
    bool stopping = false;
    std::optional<OutputSpoutFrame> mailbox;
    std::vector<std::vector<std::uint8_t>> free_buffers;
    std::optional<TimePoint> failing_since;
    TimePoint last_failure{};
    std::thread thread;

    Impl(const OutputSpoutSender &sender, Options options)
        : owner(sender), name(std::move(options.name)),
          factory(std::move(options.factory)),
          predecessor(std::move(options.predecessor)),
          clock(std::move(options.clock)), on_refused(std::move(options.on_refused)),
          gpu_request(std::move(options.gpu)) {
        if (!clock) clock = [] { return std::chrono::steady_clock::now(); };
    }

    // The caller holds the lock.
    void refuseLocked(std::string error) {
        if (current.state != "running") return;
        current.state = "refused";
        current.last_error = std::move(error);
        if (mailbox) {
            free_buffers.push_back(std::move(mailbox->pixels));
            mailbox.reset();
            ++current.frames_discarded;
            current.mailbox_depth = 0;
        }
        cv.notify_all();
    }
    void refuse(std::string error) {
        std::lock_guard lock{mutex};
        refuseLocked(std::move(error));
    }

    std::optional<OutputSpoutFrame> acquire(std::uint32_t width, std::uint32_t height,
                                            bool bgra) {
        const auto bytes = frameBytes(width, height);
        std::vector<std::uint8_t> storage;
        {
            std::lock_guard lock{mutex};
            if (stopping || current.state != "running" || !bytes) {
                ++current.frames_discarded;
                return std::nullopt;
            }
            if (!free_buffers.empty()) {
                storage = std::move(free_buffers.back());
                free_buffers.pop_back();
            } else if (mailbox) {
                storage = std::move(mailbox->pixels);
                mailbox.reset();
                current.mailbox_depth = 0;
                ++current.frames_dropped;
            } else if (current.live_buffers < 2) {
                ++current.live_buffers;
                current.peak_live_buffers =
                    (std::max)(current.peak_live_buffers, current.live_buffers);
            } else {
                ++current.frames_dropped;
                return std::nullopt;
            }
        }
        // Growing frees first, so a size change never holds a third allocation.
        if (storage.capacity() < *bytes) std::vector<std::uint8_t>{}.swap(storage);
        storage.resize(*bytes);
        return OutputSpoutFrame{std::move(storage), width, height, bgra};
    }

    void publish(OutputSpoutFrame frame) {
        {
            std::lock_guard lock{mutex};
            if (stopping || current.state != "running") {
                free_buffers.push_back(std::move(frame.pixels));
                ++current.frames_discarded;
                return;
            }
            if (mailbox) {
                free_buffers.push_back(std::move(mailbox->pixels));
                ++current.frames_dropped;
            }
            if (gpu_mailbox) {
                bridges[gpu_mailbox->bridge].state = BridgeState::free;
                gpu_mailbox.reset();
                ++current.frames_dropped;
            }
            mailbox = std::move(frame);
            current.mailbox_depth = 1;
            current.peak_mailbox_depth =
                (std::max)(current.peak_mailbox_depth, current.mailbox_depth);
        }
        cv.notify_all();
    }

    void requestStop() {
        {
            std::lock_guard lock{mutex};
            stopping = true;
        }
        cv.notify_all();
    }

    void noteResult(const OutputSpoutResult &result, std::uint32_t width,
                    std::uint32_t height) {
        const auto now = clock();
        std::lock_guard lock{mutex};
        switch (result.kind) {
        case OutputSpoutResultKind::ok:
            ++current.frames_written;
            current.registered = true;
            current.width = width;
            current.height = height;
            current.last_error.clear();
            failing_since.reset();
            return;
        case OutputSpoutResultKind::failed: {
            ++current.frames_dropped;
            current.last_error = "output.spout_send_failed@1: " + result.detail;
            // A silence as long as the limit means nothing was attempted, so it
            // proves nothing about Spout: the streak starts again here.
            if (!failing_since || now - last_failure >= output_spout_failure_limit)
                failing_since = now;
            last_failure = now;
            if (now - *failing_since >= output_spout_failure_limit)
                refuseLocked(current.last_error);
            return;
        }
        case OutputSpoutResultKind::open_failed:
            ++current.frames_dropped;
            refuseLocked("output.spout_open_failed@1: " + result.detail);
            return;
        case OutputSpoutResultKind::device_removed:
            ++current.frames_dropped;
            refuseLocked("output.spout_send_failed@1: " + result.detail);
            return;
        }
    }

    static void addTiming(OutputSpoutTiming &timing, double ms) {
        ++timing.count;
        timing.total_ms += ms;
        timing.max_ms = (std::max)(timing.max_ms, ms);
    }
    static double msSince(TimePoint begin) {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
    }

    // The caller holds the lock. A failure at any stage: this sink goes on by CPU.
    void gpuFailureLocked(OutputSpoutGpuFailure failure, std::string reason,
                          std::string detail) {
        if (failure == OutputSpoutGpuFailure::this_extent) {
            bad_extent = std::make_pair(wanted_width, wanted_height);
            current.transport = "cpu";
            current.transport_reason = std::move(reason);
            current.transport_detail = std::move(detail);
            return;
        }
        ++current.gpu_fallbacks;
        current.transport = "cpu";
        current.transport_reason = std::move(reason);
        current.transport_detail = std::move(detail);
        gpu_since.reset();
        if (logger) LOG_WARNING(logger, "Spout sender {} went to the CPU path: {} ({})", name,
                                current.transport_reason, current.transport_detail);
        switch (failure) {
        case OutputSpoutGpuFailure::device_removed:
            if (reopened) {
                refuseLocked("output.spout_send_failed@1: " + current.transport_detail);
            } else {
                reopen_pending = true;
            }
            break;
        case OutputSpoutGpuFailure::transient:
            if (transient_failures >= output_spout_retry_after.size()) {
                gpu_permanent = true;
                next_attempt.reset();
            } else {
                next_attempt = clock() + output_spout_retry_after[transient_failures];
                ++transient_failures;
            }
            teardown_pending = true;
            break;
        default:
            gpu_permanent = true;
            next_attempt.reset();
            teardown_pending = true;
            break;
        }
        cv.notify_all();
    }

    // The caller holds the lock.
    bool bridgesIdleLocked() const {
        return outstanding == 0 && !gpu_mailbox &&
               std::all_of(bridges.begin(), bridges.end(), [](const Bridge &bridge) {
                   return bridge.state == BridgeState::free;
               });
    }
    bool gpuWorkDueLocked() const {
        if (gpu_backend == nullptr || abandoned) return false;
        if (reopen_pending || teardown_pending) return bridgesIdleLocked();
        if (gpu_permanent || wanted_width == 0) return false;
        if (next_attempt && clock() < *next_attempt) return false;
        if (bad_extent && bad_extent->first == wanted_width &&
            bad_extent->second == wanted_height) return false;
        if (bridges_live && bridge_width == wanted_width && bridge_height == wanted_height)
            return false;
        // Rebuilding waits for every bridge of the old size to be idle.
        return bridgesIdleLocked();
    }

    OutputSpoutReservation reserveGpu(std::uint32_t width, std::uint32_t height,
                                      OutputSpoutSourceFormat format) {
        std::lock_guard lock{mutex};
        if (stopping || current.state != "running" || gpu_permanent) return {};
        const auto blit = format == OutputSpoutSourceFormat::rgba8_unorm;
        format_needs_cpu = format == OutputSpoutSourceFormat::rgba8_srgb ||
                           (blit && !gpu_request.rgba8_blit_exact);
        if (format_needs_cpu) return {};
        if (width != wanted_width || height != wanted_height) {
            wanted_width = width;
            wanted_height = height;
            if (bad_extent && (bad_extent->first != width || bad_extent->second != height)) {
                bad_extent.reset();
                if (current.transport_reason == "extent_unsupported") {
                    current.transport_reason = "starting";
                    current.transport_detail.clear();
                }
            }
        }
        if (gpuWorkDueLocked()) cv.notify_all();
        if (current.transport != "gpu" || !bridges_live || teardown_pending ||
            reopen_pending || bridge_width != width || bridge_height != height)
            return {};
        const auto found = std::find_if(bridges.begin(), bridges.end(), [](const Bridge &bridge) {
            return bridge.state == BridgeState::free;
        });
        if (found == bridges.end()) {
            ++current.bridge_busy;
            ++current.frames_dropped;
            return {OutputSpoutRoute::busy, std::nullopt};
        }
        const auto now = std::chrono::steady_clock::now();
        found->state = BridgeState::gpu_pending;
        found->reserved_at = now;
        ++outstanding;
        OutputSpoutGpuFrame frame;
        frame.bridge = static_cast<std::size_t>(found - bridges.begin());
        frame.image = found->image;
        frame.semaphore = semaphore;
        frame.signal_value = next_signal_value++;
        frame.bridge_generation = bridge_generation;
        frame.width = width;
        frame.height = height;
        frame.blit = blit;
        frame.captured_at = now;
        return {OutputSpoutRoute::gpu, frame};
    }

    void completeGpu(const OutputSpoutGpuFrame &frame) {
        {
            std::lock_guard lock{mutex};
            --outstanding;
            if (stopping || current.state != "running" || current.transport != "gpu" ||
                frame.bridge_generation != bridge_generation) {
                bridges[frame.bridge].state = BridgeState::free;
                ++current.frames_discarded;
            } else {
                bridges[frame.bridge].state = BridgeState::ready;
                if (gpu_mailbox) {
                    bridges[gpu_mailbox->bridge].state = BridgeState::free;
                    ++current.frames_dropped;
                }
                if (mailbox) {
                    free_buffers.push_back(std::move(mailbox->pixels));
                    mailbox.reset();
                    current.mailbox_depth = 0;
                    ++current.frames_dropped;
                }
                gpu_mailbox = frame;
                current.mailbox_depth = 1;
                current.peak_mailbox_depth =
                    (std::max)(current.peak_mailbox_depth, current.mailbox_depth);
            }
        }
        cv.notify_all();
    }

    void cancelGpu(const OutputSpoutGpuFrame &frame) {
        {
            std::lock_guard lock{mutex};
            --outstanding;
            bridges[frame.bridge].state = BridgeState::free;
            ++current.frames_discarded;
        }
        cv.notify_all();
    }

    void gpuSubmitFailed(const OutputSpoutGpuFrame &frame, std::int32_t vk_result,
                         std::string detail) {
        {
            std::lock_guard lock{mutex};
            --outstanding;
            bridges[frame.bridge].state = BridgeState::free;
            ++current.frames_dropped;
            gpuFailureLocked(outputSpoutClassifyVkResult(vk_result), "capture_submit_failed",
                             std::move(detail));
        }
        cv.notify_all();
    }

    // Calls the backend with no lock held: reserveGpu must never wait on it.
    void runGpuWork(OutputSpoutBackend &backend) {
        enum class Plan : std::uint8_t { none, reopen, teardown, build };
        Plan plan = Plan::none;
        bool prepared = false;
        bool live = false;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        {
            std::lock_guard lock{mutex};
            if (stopping || current.state != "running" || !gpuWorkDueLocked()) return;
            plan = reopen_pending ? Plan::reopen
                 : teardown_pending ? Plan::teardown : Plan::build;
            prepared = gpu_prepared;
            live = bridges_live;
            width = wanted_width;
            height = wanted_height;
            if (plan != Plan::build) bridges_live = false;
        }
        if (plan != Plan::build) {
            if (live) gpu_backend->destroyBridges();
            if (prepared) gpu_backend->releaseGpu();
            {
                std::lock_guard lock{mutex};
                gpu_prepared = false;
                semaphore = VK_NULL_HANDLE;
                teardown_pending = false;
                ++bridge_generation;
            }
            if (plan == Plan::reopen) reopenBackend(backend);
            return;
        }
        if (!prepared) {
            VkSemaphore prepared_semaphore = VK_NULL_HANDLE;
            const auto step = gpu_backend->prepare(gpu_request, prepared_semaphore);
            std::lock_guard lock{mutex};
            if (step.failure != OutputSpoutGpuFailure::none) {
                gpuFailureLocked(step.failure, step.reason, step.detail);
                return;
            }
            semaphore = prepared_semaphore;
            gpu_prepared = true;
        }
        if (live) {
            {
                std::lock_guard lock{mutex};
                bridges_live = false;
                ++bridge_generation;
            }
            gpu_backend->destroyBridges();
        }
        if (width > gpu_request.max_width || height > gpu_request.max_height) {
            std::lock_guard lock{mutex};
            gpuFailureLocked(OutputSpoutGpuFailure::this_extent, "extent_unsupported",
                             std::to_string(width) + "x" + std::to_string(height) +
                                 " is over the import limit " +
                                 std::to_string(gpu_request.max_width) + "x" +
                                 std::to_string(gpu_request.max_height));
            return;
        }
        std::array<VkImage, output_spout_bridge_count> images{};
        const auto step = gpu_backend->createBridges(width, height, images);
        std::lock_guard lock{mutex};
        if (step.failure != OutputSpoutGpuFailure::none) {
            gpuFailureLocked(step.failure, step.reason, step.detail);
            return;
        }
        for (std::size_t index = 0; index < images.size(); ++index) {
            bridges[index].state = BridgeState::free;
            bridges[index].image = images[index];
        }
        bridges_live = true;
        bridge_width = width;
        bridge_height = height;
        ++bridge_generation;
        current.transport = "gpu";
        current.transport_reason = "none";
        current.transport_detail.clear();
        gpu_since = clock();
        next_attempt.reset();
    }

    // D3D11's device is gone, so the CPU path can't use it either. Once.
    void reopenBackend(OutputSpoutBackend &backend) {
        {
            std::lock_guard lock{mutex};
            reopen_pending = false;
            reopened = true;
            current.registered = false;
        }
        backend.close();
        const auto opened = backend.open(name);
        if (opened.kind != OutputSpoutResultKind::ok) {
            refuse("output.spout_open_failed@1: " + opened.detail);
            return;
        }
        const auto actual = backend.actualName();
        if (actual != name) {
            refuse("output.spout_name_taken@1: '" + name +
                   "' is already a running sender; Spout offered '" + actual + "'");
            return;
        }
        std::lock_guard lock{mutex};
        // Not recovered until a CPU send says so.
        awaiting_cpu_confirm = true;
        current.transport_detail = "reopened after the D3D11 device was removed";
        if (transient_failures >= output_spout_retry_after.size()) {
            gpu_permanent = true;
            next_attempt.reset();
        } else {
            next_attempt = clock() + output_spout_retry_after[transient_failures];
            ++transient_failures;
        }
    }

    // A timed-out wait cancelled nothing. CPU sending uses the same D3D11 context,
    // so it waits too, until that context is known to make progress.
    OutputSpoutSentState resolveQuarantine() {
        {
            std::lock_guard lock{mutex};
            current.gpu_quarantined = true;
            ++current.sync_timeouts;
        }
        auto state = OutputSpoutSentState::timeout;
        for (int attempt = 0; attempt < output_spout_quarantine_tries; ++attempt) {
            state = gpu_backend->waitSent(output_spout_sync_limit);
            if (state == OutputSpoutSentState::done ||
                state == OutputSpoutSentState::device_removed) break;
        }
        std::lock_guard lock{mutex};
        if (state == OutputSpoutSentState::done ||
            state == OutputSpoutSentState::device_removed) {
            current.gpu_quarantined = false;
        }
        return state;
    }

    void sendGpuFrame(const OutputSpoutGpuFrame &frame) {
        {
            std::lock_guard lock{mutex};
            if (frame.bridge_generation != bridge_generation ||
                current.transport != "gpu") {
                bridges[frame.bridge].state = BridgeState::free;
                ++current.frames_discarded;
                return;
            }
            bridges[frame.bridge].state = BridgeState::sending;
        }
        const auto sent = gpu_backend->sendBridge(frame.bridge, frame.signal_value);
        const auto returned = msSince(frame.captured_at);
        const auto completion_begin = std::chrono::steady_clock::now();
        auto state = sent.completion_enqueued
                         ? gpu_backend->waitSent(output_spout_sync_limit)
                         : OutputSpoutSentState::done;
        const auto first_wait = msSince(completion_begin);
        if (state == OutputSpoutSentState::timeout || state == OutputSpoutSentState::failed) {
            state = resolveQuarantine();
            if (state != OutputSpoutSentState::done &&
                state != OutputSpoutSentState::device_removed) {
                // Never known to finish: the bridge stays in the sending state,
                // nothing frees or reuses it, and its device is given up.
                std::lock_guard lock{mutex};
                abandoned = true;
                current.transport = "cpu";
                current.transport_reason = "sync_timeout";
                current.transport_detail = "D3D11 never finished a bridge copy";
                ++current.gpu_fallbacks;
                refuseLocked("output.spout_send_failed@1: D3D11 never finished a bridge copy, "
                             "so its device can't be used for the CPU path either");
                return;
            }
            if (state == OutputSpoutSentState::done) {
                std::lock_guard lock{mutex};
                bridges[frame.bridge].state = BridgeState::free;
                ++current.frames_dropped;
                gpuFailureLocked(OutputSpoutGpuFailure::transient, "sync_timeout",
                                 "D3D11 finished a bridge copy late");
                return;
            }
        }
        std::lock_guard lock{mutex};
        if (state == OutputSpoutSentState::device_removed ||
            sent.result.kind == OutputSpoutResultKind::device_removed) {
            bridges[frame.bridge].state = BridgeState::free;
            ++current.frames_dropped;
            gpuFailureLocked(OutputSpoutGpuFailure::device_removed, "d3d11_device_removed",
                             sent.result.detail.empty() ? "the D3D11 device was removed"
                                                        : sent.result.detail);
            return;
        }
        addTiming(current.d3d11_completion, first_wait);
        addTiming(current.bridge_cycle,
                  std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() -
                      bridges[frame.bridge].reserved_at).count());
        bridges[frame.bridge].state = BridgeState::free;
        if (sent.result.kind != OutputSpoutResultKind::ok) {
            ++current.frames_dropped;
            current.last_error = "output.spout_send_failed@1: " + sent.result.detail;
            gpuFailureLocked(OutputSpoutGpuFailure::transient, "send_failed",
                             sent.result.detail);
            return;
        }
        addTiming(current.capture_to_send_gpu, returned);
        ++current.frames_written;
        ++current.frames_gpu;
        current.registered = true;
        current.width = frame.width;
        current.height = frame.height;
        current.last_error.clear();
        failing_since.reset();
        if (gpu_since && clock() - *gpu_since >= output_spout_gpu_clear_after)
            transient_failures = 0;
        cv.notify_all();
    }

    void sendLoop(OutputSpoutBackend &backend) {
        {
            std::lock_guard lock{mutex};
            gpu_backend = backend.gpu();
            if (!gpu_request.allowed) {
                current.transport_reason = gpu_request.gate_reason;
                current.transport_detail = gpu_request.gate_detail;
            } else if (gpu_backend == nullptr) {
                current.transport_reason = "backend_cpu_only";
            } else {
                gpu_permanent = false;
            }
        }
        while (true) {
            runGpuWork(backend);
            OutputSpoutFrame frame;
            std::optional<OutputSpoutGpuFrame> gpu_frame;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, [&] {
                    return stopping || current.state != "running" || mailbox.has_value() ||
                           gpu_mailbox.has_value() || gpuWorkDueLocked();
                });
                if (stopping || current.state != "running") return;
                if (!mailbox && !gpu_mailbox) continue;
                if (gpu_mailbox) {
                    gpu_frame = gpu_mailbox;
                    gpu_mailbox.reset();
                } else {
                    frame = std::move(*mailbox);
                    mailbox.reset();
                }
                current.mailbox_depth = 0;
                current.sending = true;
            }
            if (gpu_frame) {
                sendGpuFrame(*gpu_frame);
                std::lock_guard lock{mutex};
                current.sending = false;
                continue;
            }
            if (!frame.bgra)
                outputSpoutSwapRedBlue(frame.pixels.data(), frame.pixels.size());
            const auto result = backend.send(frame.pixels.data(), frame.width, frame.height);
            noteResult(result, frame.width, frame.height);
            std::lock_guard lock{mutex};
            if (result.kind == OutputSpoutResultKind::ok) {
                ++current.frames_cpu;
                if (frame.captured_at != std::chrono::steady_clock::time_point{})
                    addTiming(current.capture_to_send_cpu, msSince(frame.captured_at));
                if (awaiting_cpu_confirm) {
                    awaiting_cpu_confirm = false;
                    current.transport_detail =
                        "reopened after the D3D11 device was removed; CPU sending works";
                }
            }
            free_buffers.push_back(std::move(frame.pixels));
            current.sending = false;
        }
    }

    // After the loop: everything reserved reaches completeGpu or cancelGpu by the
    // worker's drain, and only then can a bridge go. This can wait, on this thread.
    void finishGpu() {
        if (gpu_backend == nullptr) return;
        bool give_up = false;
        bool live = false;
        bool prepared = false;
        {
            std::unique_lock lock{mutex};
            if (gpu_mailbox) {
                bridges[gpu_mailbox->bridge].state = BridgeState::free;
                gpu_mailbox.reset();
                current.mailbox_depth = 0;
                ++current.frames_discarded;
            }
            cv.wait(lock, [&] { return outstanding == 0; });
            give_up = abandoned;
            live = bridges_live;
            prepared = gpu_prepared;
            bridges_live = false;
            gpu_prepared = false;
            ++bridge_generation;
        }
        if (give_up) {
            gpu_backend->abandonGpu();
            return;
        }
        if (live) gpu_backend->destroyBridges();
        if (prepared) gpu_backend->releaseGpu();
    }

    void openAndSend(std::unique_ptr<OutputSpoutBackend> &backend) {
        if (name.empty() || name.size() > output_spout_name_bytes) {
            refuse("output.spout_open_failed@1: the sender name is empty or over " +
                   std::to_string(output_spout_name_bytes) + " bytes");
            return;
        }
        backend = factory ? factory() : nullptr;
        if (!backend) {
            refuse("output.spout_open_failed@1: no backend");
            return;
        }
        const auto opened = backend->open(name);
        if (opened.kind != OutputSpoutResultKind::ok) {
            refuse("output.spout_open_failed@1: " + opened.detail);
            return;
        }
        const auto actual = backend->actualName();
        if (actual != name) {
            refuse("output.spout_name_taken@1: '" + name +
                   "' is already a running sender; Spout offered '" + actual + "'");
            return;
        }
        {
            std::lock_guard lock{mutex};
            current.sender_name = actual;
        }
        sendLoop(*backend);
    }

    void run() {
        // A sender that stops while pending still waits, so that a third sender
        // with this name, waiting on this one, can't overtake the first.
        if (predecessor) predecessor->wait();
        bool stopped_before_open = false;
        {
            std::lock_guard lock{mutex};
            stopped_before_open = stopping || current.state != "running";
        }
        if (!stopped_before_open) {
            if (!reserveName(name)) {
                refuse("output.spout_name_taken@1: '" + name +
                       "' is already a sender of this process");
            } else {
                std::unique_ptr<OutputSpoutBackend> backend;
                try {
                    openAndSend(backend);
                } catch (const std::exception &e) {
                    refuse(std::string{"output.spout_open_failed@1: "} + e.what());
                } catch (...) {
                    refuse("output.spout_open_failed@1: unknown backend error");
                }
                if (backend) {
                    try {
                        finishGpu();
                    } catch (const std::exception &e) {
                        if (logger) LOG_WARNING(logger, "Spout sender {} GPU cleanup failed: {}",
                                                name, e.what());
                    }
                }
                if (backend) {
                    try {
                        backend->close();
                        backend.reset();
                    } catch (const std::exception &e) {
                        if (logger) LOG_WARNING(logger, "Spout sender {} close failed: {}",
                                                name, e.what());
                    }
                }
                releaseName(name);
            }
        }
        bool refused = false;
        {
            std::lock_guard lock{mutex};
            current.registered = false;
            if (mailbox) {
                free_buffers.push_back(std::move(mailbox->pixels));
                mailbox.reset();
                current.mailbox_depth = 0;
                ++current.frames_discarded;
            }
            refused = current.state == "refused";
            if (current.state == "running") {
                current.state = "finished";
                current.stop_reason = "stop";
            }
        }
        released.acknowledge();
        if (refused && on_refused) on_refused(owner);
    }
};

OutputSpoutSender::OutputSpoutSender(Options options)
    : impl_(std::make_unique<Impl>(*this, std::move(options))) {
    impl_->thread = std::thread([this] { impl_->run(); });
}

OutputSpoutSender::~OutputSpoutSender() {
    impl_->requestStop();
    if (impl_->thread.joinable()) impl_->thread.join();
}

std::optional<OutputSpoutFrame> OutputSpoutSender::acquire(
    std::uint32_t width, std::uint32_t height, bool bgra) {
    return impl_->acquire(width, height, bgra);
}
void OutputSpoutSender::publish(OutputSpoutFrame frame) {
    impl_->publish(std::move(frame));
}
OutputSpoutReservation OutputSpoutSender::reserveGpu(std::uint32_t width, std::uint32_t height,
                                                     OutputSpoutSourceFormat format) {
    return impl_->reserveGpu(width, height, format);
}
void OutputSpoutSender::completeGpu(const OutputSpoutGpuFrame &frame) {
    impl_->completeGpu(frame);
}
void OutputSpoutSender::cancelGpu(const OutputSpoutGpuFrame &frame) {
    impl_->cancelGpu(frame);
}
void OutputSpoutSender::gpuSubmitFailed(const OutputSpoutGpuFrame &frame, std::int32_t vk_result,
                                        std::string detail) {
    impl_->gpuSubmitFailed(frame, vk_result, std::move(detail));
}
void OutputSpoutSender::requestStop() { impl_->requestStop(); }
void OutputSpoutSender::refuse(std::string error) { impl_->refuse(std::move(error)); }
const std::string &OutputSpoutSender::name() const { return impl_->name; }
bool OutputSpoutSender::finished() const { return impl_->released.done(); }
OutputSpoutRelease OutputSpoutSender::release() const { return impl_->released; }
OutputSpoutStatus OutputSpoutSender::status() const {
    std::lock_guard lock{impl_->mutex};
    auto status = impl_->current;
    // A format that stays on CPU isn't a failure, and says so only while nothing
    // more serious does.
    if (impl_->format_needs_cpu &&
        (status.transport_reason == "none" || status.transport_reason == "starting")) {
        status.transport = "cpu";
        status.transport_reason = "format_needs_cpu";
        status.transport_detail =
            "this source format can't reach the bridge with its bytes unchanged";
    }
    if (impl_->next_attempt && !impl_->gpu_permanent) {
        const auto now = impl_->clock();
        status.gpu_next_retry_ms = *impl_->next_attempt <= now
            ? std::uint64_t{0}
            : static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      *impl_->next_attempt - now).count());
    }
    return status;
}

#if PELICAN_OUTPUT_TEST_SEAMS
std::atomic<bool> &outputSpoutGpuBlockSendForTesting() {
    static std::atomic<bool> block{true};
    return block;
}
std::atomic<bool> &outputSpoutGpuSendBlockedForTesting() {
    static std::atomic<bool> blocked{false};
    return blocked;
}
nlohmann::json outputSpoutTestBackendControl(const nlohmann::json &params) {
    auto &state = testBackendState();
    std::lock_guard lock{state.mutex};
    if (params.contains("block_gpu_send"))
        outputSpoutGpuBlockSendForTesting() = params.at("block_gpu_send").get<bool>();
    if (params.contains("block_send")) state.block_send = params.at("block_send").get<bool>();
    if (params.contains("block_close")) state.block_close = params.at("block_close").get<bool>();
    state.cv.notify_all();
    return {{"gpu_send_blocked", outputSpoutGpuSendBlockedForTesting().load()},
            {"opens", state.opens}, {"sends", state.sends}, {"closes", state.closes},
            {"send_blocked", state.send_blocked}, {"close_blocked", state.close_blocked},
            {"events", state.events}, {"last_pixels", state.last_pixels},
            {"last_width", state.last_width}, {"last_height", state.last_height}};
}
#endif

} // namespace Pelican
