#include "oscsender.hpp"

#include "oscaddress.hpp"
#include "oscsocket.hpp"
#include "../launchconfig.hpp"
#include "../log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace Pelican {
namespace {

using Osc::Clock;
using Osc::SendTable;
using Osc::SocketHandle;

std::string destinationText(const OscSendConfig &config) {
    return config.host + ":" + std::to_string(config.port);
}

std::uint32_t validatedHost(const OscSendConfig &config) {
    std::uint32_t host = 0;
    if (config.port == 0 || !parseOscListenerIpv4(config.host, host) ||
        (host >> 24) == 0 || (host >> 24) >= 224)
        throw std::runtime_error("osc.config_invalid@1: invalid --osc-send " +
                                 destinationText(config));
    return host;
}

} // namespace

struct OscSender::Impl {
    enum class State { inactive, offline, sending, failed };

    mutable std::mutex status_mutex;
    mutable std::mutex table_mutex;
    mutable std::mutex io_mutex;
    std::condition_variable cv;
    SendTable table{Clock::now()};
    std::thread worker;
    OscSendTransport transport;
    SocketHandle socket = Osc::invalid_socket;
    bool winsock_started = false;
    State state = State::inactive;
    OscSendConfig config;
    std::string error;
    std::string logged_error;

    std::atomic_bool stop_requested{false};
    std::atomic_bool stopping{false};
    std::atomic_bool worker_running{false};
    std::atomic_bool socket_open{false};
    std::atomic_bool replaying{false};
    std::atomic_bool retirement_pending{false};
    std::atomic_uint64_t epoch{0};
    std::atomic_size_t sinks{0};
    std::atomic_size_t values_reading{0};
    std::atomic_uint64_t sent{0}, resent{0}, dropped{0}, send_errors{0};
    std::atomic_uint64_t non_finite{0}, out_of_range{0}, frames_busy{0};

    void reportError(std::string text) {
        {
            std::scoped_lock lock{status_mutex};
            error = "osc.send_failed@1: " + text;
            ++send_errors;
            if (logged_error == text) return;
            logged_error = text;
        }
        std::fprintf(stderr, "Pelican: OSC sender: %s\n", text.c_str());
    }

    void sendBatch(Osc::Work &work, std::uint64_t batch_epoch) {
        std::scoped_lock io{io_mutex};
        for (int pass = 0; pass < 2; ++pass) {
            auto &entries = pass == 0 ? work.changes : work.resends;
            std::size_t index = 0;
            for (const auto &entry : entries) {
                if (stop_requested.load(std::memory_order_acquire) ||
                    replaying.load(std::memory_order_acquire) ||
                    epoch.load(std::memory_order_acquire) != batch_epoch) {
                    dropped.fetch_add(entries.size() - index, std::memory_order_relaxed);
                    return;
                }
                ++index;
                try {
                    const auto bytes = Osc::encodeMessage(entry.address, entry.value);
                    const auto outcome = transport(bytes);
                    if (outcome.kind == OscSendOutcome::Kind::sent) {
                        (pass == 0 ? sent : resent).fetch_add(
                            1, std::memory_order_relaxed);
                    } else if (outcome.kind == OscSendOutcome::Kind::would_block) {
                        dropped.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        reportError(outcome.error.empty() ? "unknown transport error"
                                                         : outcome.error);
                    }
                } catch (const std::exception &exception) {
                    reportError(exception.what());
                }
            }
        }
    }

    void runWorker() {
        worker_running.store(true, std::memory_order_release);
        std::unique_lock lock{table_mutex};
        while (!stop_requested.load(std::memory_order_acquire)) {
            if (retirement_pending.exchange(false, std::memory_order_acq_rel))
                table.clear();
            cv.wait_until(lock, table.nextResend(), [&] {
                return stop_requested.load(std::memory_order_acquire) ||
                       retirement_pending.load(std::memory_order_acquire) ||
                       table.hasDirty();
            });
            if (stop_requested.load(std::memory_order_acquire)) break;
            if (retirement_pending.exchange(false, std::memory_order_acq_rel)) {
                table.clear();
                continue;
            }
            if (replaying.load(std::memory_order_acquire)) {
                table.clear();
                continue;
            }
            const auto batch_epoch = epoch.load(std::memory_order_acquire);
            auto work = table.take(Clock::now());
            lock.unlock();
            sendBatch(work, batch_epoch);
            lock.lock();
        }
        worker_running.store(false, std::memory_order_release);
    }
};

namespace {

bool startSender(OscSender::Impl &state, const OscSendConfig &config,
                 OscSendTransport transport, bool real_socket) {
    const auto host = validatedHost(config);
    {
        std::scoped_lock lock{state.status_mutex};
        state.config = config;
        state.error.clear();
        state.logged_error.clear();
        state.state = OscSender::Impl::State::sending;
        state.transport = std::move(transport);
    }
    state.stop_requested.store(false, std::memory_order_release);
    state.stopping.store(false, std::memory_order_release);
    state.replaying.store(false, std::memory_order_release);
    state.retirement_pending.store(false, std::memory_order_release);
    {
        std::scoped_lock lock{state.table_mutex};
        state.table.clear();
        state.table = SendTable{Clock::now()};
    }

    const auto fail = [&](std::string text) {
        if (state.socket != Osc::invalid_socket) {
            Osc::closeSocket(state.socket);
            state.socket = Osc::invalid_socket;
        }
#if defined(_WIN32)
        if (state.winsock_started) {
            ::WSACleanup();
            state.winsock_started = false;
        }
#endif
        state.socket_open.store(false, std::memory_order_release);
        {
            std::scoped_lock lock{state.status_mutex};
            state.state = OscSender::Impl::State::failed;
            state.error = "osc.send_start_failed@1: " + text;
        }
        std::fprintf(stderr, "Pelican: OSC sender start failed: %s\n", text.c_str());
        return false;
    };

    if (real_socket) {
#if defined(_WIN32)
        WSADATA data{};
        const int startup = ::WSAStartup(MAKEWORD(2, 2), &data);
        if (startup != 0)
            return fail("cannot initialize Winsock: " + Osc::socketErrorText(startup));
        state.winsock_started = true;
#endif
        state.socket = Osc::createDatagramSocket();
        if (state.socket == Osc::invalid_socket)
            return fail("cannot create UDP socket: " +
                        Osc::socketErrorText(Osc::lastSocketError()));
        if (!Osc::makeSocketNonBlocking(state.socket))
            return fail("cannot make UDP socket non-blocking: " +
                        Osc::socketErrorText(Osc::lastSocketError()));
        state.socket_open.store(true, std::memory_order_release);
        state.transport = [socket = state.socket, host, port = config.port](
                              std::span<const std::uint8_t> bytes) -> OscSendOutcome {
            sockaddr_in destination{};
            destination.sin_family = AF_INET;
            destination.sin_port = htons(port);
            destination.sin_addr.s_addr = htonl(host);
            const auto count = ::sendto(
                socket, reinterpret_cast<const char *>(bytes.data()),
                static_cast<int>(bytes.size()), 0,
                reinterpret_cast<const sockaddr *>(&destination), sizeof(destination));
            if (count >= 0) {
                if (static_cast<std::size_t>(count) == bytes.size()) return OscSendOutcome{};
                return {OscSendOutcome::Kind::error,
                        "short UDP send: " + std::to_string(count) + " of " +
                            std::to_string(bytes.size()) + " bytes"};
            }
            const int code = Osc::lastSocketError();
            if (Osc::interruptedError(code) || Osc::wouldBlockError(code))
                return {OscSendOutcome::Kind::would_block,
                        Osc::socketErrorText(code)};
            return {OscSendOutcome::Kind::error,
                    Osc::socketErrorText(code)};
        };
    }
    state.worker_running.store(true, std::memory_order_release);
    try {
        state.worker = std::thread([&state] { state.runWorker(); });
    } catch (const std::exception &exception) {
        state.worker_running.store(false, std::memory_order_release);
        return fail("cannot start sender thread: " + std::string{exception.what()});
    }
    return true;
}

} // namespace

OscSender::OscSender() : impl_(std::make_unique<Impl>()) {}
OscSender::~OscSender() { stop(); }

bool OscSender::start(const OscSendConfig &config) {
    stop();
    return startSender(*impl_, config, {}, true);
}

bool OscSender::startWithTransportForTesting(const OscSendConfig &config,
                                             OscSendTransport transport) {
    if (!transport) throw std::invalid_argument("OSC test transport is empty");
    stop();
    return startSender(*impl_, config, std::move(transport), false);
}

void OscSender::markOffline(const OscSendConfig &config) {
    (void)validatedHost(config);
    stop();
    {
        std::scoped_lock lock{impl_->status_mutex};
        impl_->config = config;
        impl_->state = Impl::State::offline;
        impl_->error.clear();
    }
    if (logger != nullptr)
        LOG_INFO(logger, "OSC sender offline for deterministic/replay launch: {}",
                 destinationText(config));
}

void OscSender::stop() {
    auto &state = *impl_;
    state.stopping.store(true, std::memory_order_release);
    state.stop_requested.store(true, std::memory_order_release);
    {
        std::scoped_lock lock{state.table_mutex};
        state.stop_requested.store(true, std::memory_order_release);
        state.epoch.fetch_add(1, std::memory_order_acq_rel);
        state.table.clear();
    }
    state.cv.notify_one();
    if (state.worker.joinable()) state.worker.join();
    state.worker_running.store(false, std::memory_order_release);
    if (state.socket != Osc::invalid_socket) {
        Osc::closeSocket(state.socket);
        state.socket = Osc::invalid_socket;
    }
    state.socket_open.store(false, std::memory_order_release);
#if defined(_WIN32)
    if (state.winsock_started) {
        ::WSACleanup();
        state.winsock_started = false;
    }
#endif
    std::scoped_lock lock{state.status_mutex};
    state.state = Impl::State::inactive;
    state.config = {};
    state.error.clear();
    state.transport = {};
    state.replaying.store(false, std::memory_order_release);
    state.stopping.store(false, std::memory_order_release);
}

void OscSender::publishFrame(std::span<const ValueGraph::OscSendValue> items,
                             bool replaying) {
    auto &state = *impl_;
    state.sinks.store(items.size(), std::memory_order_release);
    state.replaying.store(replaying || state.replaying.load(std::memory_order_acquire),
                          std::memory_order_release);
    if (!state.worker_running.load(std::memory_order_acquire)) return;
    std::unique_lock lock{state.table_mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        state.frames_busy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool was_replaying = state.replaying.exchange(replaying,
                                                        std::memory_order_acq_rel);
    if (replaying) {
        if (!was_replaying) state.epoch.fetch_add(1, std::memory_order_acq_rel);
        state.table.clear();
    } else {
        if (was_replaying) state.epoch.fetch_add(1, std::memory_order_acq_rel);
        state.retirement_pending.store(false, std::memory_order_release);
        const auto applied = state.table.applyFrame(items);
        state.dropped.fetch_add(applied.dropped, std::memory_order_relaxed);
        state.non_finite.fetch_add(applied.non_finite, std::memory_order_relaxed);
        state.out_of_range.fetch_add(applied.out_of_range, std::memory_order_relaxed);
    }
    const bool dirty = state.table.hasDirty();
    state.values_reading.store(state.table.size(), std::memory_order_release);
    lock.unlock();
    if (dirty) state.cv.notify_one();
}

void OscSender::pauseForReplay() {
    auto &state = *impl_;
    {
        std::scoped_lock lock{state.table_mutex};
        state.replaying.store(true, std::memory_order_release);
        state.epoch.fetch_add(1, std::memory_order_acq_rel);
        state.table.clear();
        state.values_reading.store(0, std::memory_order_release);
    }
    state.cv.notify_one();
    std::scoped_lock io{state.io_mutex};
}

void OscSender::retire() {
    auto &state = *impl_;
    state.sinks.store(0, std::memory_order_release);
    state.epoch.fetch_add(1, std::memory_order_acq_rel);
    std::unique_lock lock{state.table_mutex, std::try_to_lock};
    if (lock.owns_lock()) {
        state.retirement_pending.store(false, std::memory_order_release);
        state.table.clear();
        state.values_reading.store(0, std::memory_order_release);
    } else {
        state.retirement_pending.store(true, std::memory_order_release);
    }
    state.cv.notify_one();
}

nlohmann::json OscSender::statusJson() const {
    auto &state = *impl_;
    std::string name;
    std::string destination;
    std::string error;
    {
        std::scoped_lock lock{state.status_mutex};
        switch (state.state) {
        case Impl::State::inactive: name = "inactive"; break;
        case Impl::State::offline: name = "offline"; break;
        case Impl::State::failed: name = "failed"; break;
        case Impl::State::sending:
            name = state.replaying.load(std::memory_order_acquire) ? "replay" : "sending";
            break;
        }
        if (state.state != Impl::State::inactive) destination = destinationText(state.config);
        error = state.error;
    }
    if (std::unique_lock lock{state.table_mutex, std::try_to_lock}; lock.owns_lock())
        state.values_reading.store(state.table.size(), std::memory_order_release);
    return {
        {"state", name},
        {"stopping", state.stopping.load(std::memory_order_acquire)},
        {"destination", destination},
        {"socket_open", state.socket_open.load(std::memory_order_acquire)},
        {"sinks", state.sinks.load(std::memory_order_acquire)},
        {"values", state.values_reading.load(std::memory_order_acquire)},
        {"sent", state.sent.load(std::memory_order_acquire)},
        {"resent", state.resent.load(std::memory_order_acquire)},
        {"dropped", state.dropped.load(std::memory_order_acquire)},
        {"send_errors", state.send_errors.load(std::memory_order_acquire)},
        {"non_finite", state.non_finite.load(std::memory_order_acquire)},
        {"out_of_range", state.out_of_range.load(std::memory_order_acquire)},
        {"frames_busy", state.frames_busy.load(std::memory_order_acquire)},
        {"last_error", error},
    };
}

bool OscSender::socketOpen() const {
    return impl_->socket_open.load(std::memory_order_acquire);
}

bool OscSender::workerStarted() const {
    return impl_->worker.joinable() &&
           impl_->worker_running.load(std::memory_order_acquire);
}

std::unique_lock<std::mutex> OscSender::lockTableForTesting() {
    return std::unique_lock<std::mutex>{impl_->table_mutex};
}

bool oscSendOfflineLaunch(const EngineLaunchConfig &launch) noexcept {
    return launch.start_time_explicit || launch.start_frames > 0 ||
           launch.camera_bake_output.has_value() || launch.input_replay ||
           launch.input_replay_path.has_value() || launch.live_replay_path.has_value() ||
           launch.render_out.has_value();
}

} // namespace Pelican
