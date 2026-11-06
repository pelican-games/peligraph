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

OutputSpoutBackendFactory outputSpoutBackendFactory() {
#if PELICAN_OUTPUT_TEST_SEAMS
    if (const char *mode = std::getenv(test_backend_variable)) {
        const bool fail_open = std::string{mode} == "fail_open";
        return [fail_open] { return std::make_unique<TestBackend>(fail_open); };
    }
#endif
#if PELICAN_WITH_SPOUT
    return outputSpoutRealBackendFactory();
#else
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
          clock(std::move(options.clock)), on_refused(std::move(options.on_refused)) {
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

    void sendLoop(OutputSpoutBackend &backend) {
        while (true) {
            OutputSpoutFrame frame;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, [&] {
                    return stopping || current.state != "running" || mailbox.has_value();
                });
                if (stopping || current.state != "running") return;
                frame = std::move(*mailbox);
                mailbox.reset();
                current.mailbox_depth = 0;
                current.sending = true;
            }
            if (!frame.bgra)
                outputSpoutSwapRedBlue(frame.pixels.data(), frame.pixels.size());
            const auto result = backend.send(frame.pixels.data(), frame.width, frame.height);
            noteResult(result, frame.width, frame.height);
            std::lock_guard lock{mutex};
            free_buffers.push_back(std::move(frame.pixels));
            current.sending = false;
        }
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
void OutputSpoutSender::requestStop() { impl_->requestStop(); }
void OutputSpoutSender::refuse(std::string error) { impl_->refuse(std::move(error)); }
const std::string &OutputSpoutSender::name() const { return impl_->name; }
bool OutputSpoutSender::finished() const { return impl_->released.done(); }
OutputSpoutRelease OutputSpoutSender::release() const { return impl_->released; }
OutputSpoutStatus OutputSpoutSender::status() const {
    std::lock_guard lock{impl_->mutex};
    return impl_->current;
}

#if PELICAN_OUTPUT_TEST_SEAMS
nlohmann::json outputSpoutTestBackendControl(const nlohmann::json &params) {
    auto &state = testBackendState();
    std::lock_guard lock{state.mutex};
    if (params.contains("block_send")) state.block_send = params.at("block_send").get<bool>();
    if (params.contains("block_close")) state.block_close = params.at("block_close").get<bool>();
    state.cv.notify_all();
    return {{"opens", state.opens}, {"sends", state.sends}, {"closes", state.closes},
            {"send_blocked", state.send_blocked}, {"close_blocked", state.close_blocked},
            {"events", state.events}, {"last_pixels", state.last_pixels},
            {"last_width", state.last_width}, {"last_height", state.last_height}};
}
#endif

} // namespace Pelican
