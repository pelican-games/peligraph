#include "osclistener.hpp"

#include "oscinput.hpp"

#include "../log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Pelican {
namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
inline constexpr SocketHandle invalid_socket = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle invalid_socket = -1;
#endif

constexpr unsigned receive_error_limit = 100;

int lastSocketError() noexcept {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string socketErrorText(int code) {
    if (code == 0) return "unknown socket error";
    return std::error_code{code, std::system_category()}.message() + " (" +
           std::to_string(code) + ")";
}

bool interruptedError(int code) noexcept {
#if defined(_WIN32)
    return code == WSAEINTR;
#else
    return code == EINTR;
#endif
}

bool wouldBlockError(int code) noexcept {
#if defined(_WIN32)
    return code == WSAEWOULDBLOCK;
#else
    return code == EAGAIN || code == EWOULDBLOCK;
#endif
}

bool harmlessDatagramError(int code) noexcept {
#if defined(_WIN32)
    return code == WSAECONNRESET || code == WSAEMSGSIZE;
#else
    return false;
#endif
}

void closeSocket(SocketHandle socket) noexcept {
#if defined(_WIN32)
    if (socket != invalid_socket) ::closesocket(socket);
#else
    if (socket != invalid_socket) ::close(socket);
#endif
}

std::string ipv4Text(std::uint32_t host_order) {
    return std::to_string((host_order >> 24) & 0xffu) + "." +
           std::to_string((host_order >> 16) & 0xffu) + "." +
           std::to_string((host_order >> 8) & 0xffu) + "." +
           std::to_string(host_order & 0xffu);
}

enum class WaitResult {
    readable,
    timeout,
    error,
};

WaitResult waitReadable(SocketHandle socket, int timeout_ms) noexcept {
#if defined(_WIN32)
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket, &read_set);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    const int result =
        ::select(0, &read_set, nullptr, nullptr, &timeout);
    if (result == 0) return WaitResult::timeout;
    if (result == SOCKET_ERROR) {
        const int code = WSAGetLastError();
        if (code == WSAEINTR) return WaitResult::timeout;
        return WaitResult::error;
    }
    return FD_ISSET(socket, &read_set) ? WaitResult::readable
                                       : WaitResult::error;
#else
    pollfd descriptor{};
    descriptor.fd = socket;
    descriptor.events = POLLIN;
    const int result = ::poll(&descriptor, 1, timeout_ms);
    if (result == 0) return WaitResult::timeout;
    if (result < 0) return WaitResult::error;
    if ((descriptor.revents & POLLIN) != 0) return WaitResult::readable;
    return WaitResult::error;
#endif
}

} // namespace

struct OscListener::Impl {
    enum class State {
        off,
        listening,
        failed,
    };

    mutable std::mutex status_mutex;
    std::atomic_bool stop_requested{false};
    std::thread worker;
    SocketHandle socket = invalid_socket;
    bool winsock_started = false;

    State state = State::off;
    std::string interface_address = "127.0.0.1";
    std::uint16_t port = 0;
    std::string error;
    std::uint64_t sender_not_allowed = 0;
    std::uint64_t receive_errors = 0;

    std::uint32_t interface_host = 0;
    std::vector<std::uint32_t> allow_hosts;

    void publishFailure(std::string message) {
        std::string text;
        {
            std::scoped_lock lock{status_mutex};
            state = State::failed;
            error = std::move(message);
            text = error;
        }
        std::fprintf(stderr, "Pelican: OSC listener failed: %s\n",
                     text.c_str());
    }

    void countReceiveError() {
        std::scoped_lock lock{status_mutex};
        ++receive_errors;
    }

    void runWorker() {
        std::array<std::uint8_t, 65536> buffer{};
        unsigned consecutive_errors = 0;

        while (!stop_requested.load(std::memory_order_acquire)) {
            auto &input = GET_MODULE(OscInput);
            input.releaseDue(ntpNowUtc());
            const auto wait_ms = static_cast<int>(
                input.listenerWait(ntpNowUtc()).count());
            const auto wait = waitReadable(socket, wait_ms);
            if (wait == WaitResult::timeout) continue;
            if (wait == WaitResult::error) {
                const int code = lastSocketError();
                if (interruptedError(code)) continue;
                countReceiveError();
                if (++consecutive_errors >= receive_error_limit) {
                    publishFailure(
                        "osc.receive_failed@1: 100 consecutive receive errors "
                        "on the OSC socket: " +
                        socketErrorText(code));
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                continue;
            }

            sockaddr_in sender{};
#if defined(_WIN32)
            int sender_length = sizeof(sender);
#else
            socklen_t sender_length = sizeof(sender);
#endif
            const auto received =
                ::recvfrom(socket, reinterpret_cast<char *>(buffer.data()),
                           static_cast<int>(buffer.size()), 0,
                           reinterpret_cast<sockaddr *>(&sender),
                           &sender_length);
            if (received < 0) {
                const int code = lastSocketError();
                if (interruptedError(code) || wouldBlockError(code)) continue;
                if (harmlessDatagramError(code)) {
                    countReceiveError();
                    continue;
                }
                countReceiveError();
                if (++consecutive_errors >= receive_error_limit) {
                    publishFailure(
                        "osc.receive_failed@1: 100 consecutive receive errors "
                        "on the OSC socket: " +
                        socketErrorText(code));
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                continue;
            }
            consecutive_errors = 0;

            const auto sender_host = ntohl(sender.sin_addr.s_addr);
            if (!allow_hosts.empty() &&
                std::find(allow_hosts.begin(), allow_hosts.end(), sender_host) ==
                    allow_hosts.end()) {
                std::scoped_lock lock{status_mutex};
                ++sender_not_allowed;
                continue;
            }

            const std::string sender_text =
                ipv4Text(sender_host) + ":" +
                std::to_string(ntohs(sender.sin_port));
            GET_MODULE(OscInput).receive(
                std::span<const std::uint8_t>{buffer.data(),
                                              static_cast<std::size_t>(received)},
                ntpNowUtc(), sender_text);
        }
    }
};

OscListener::OscListener() : impl_(std::make_unique<Impl>()) {
    // The worker thread must never be the first to construct a module.
    (void)GET_MODULE(OscInput);
}

OscListener::~OscListener() { stop(); }

bool OscListener::start(const OscListenerConfig &config) {
    auto &state = *impl_;
    {
        std::scoped_lock lock{state.status_mutex};
        if (state.state == Impl::State::listening) return true;
    }

    // Clear leftovers from an earlier failed start.  This also joins a worker
    // that already ended on its own.
    stop();

    std::uint32_t interface_host = 0;
    if (!parseOscListenerIpv4(config.interface_address, interface_host)) {
        throw std::runtime_error(
            "osc.config_invalid@1: --osc-interface must be a dotted-decimal "
            "IPv4 address: " +
            config.interface_address);
    }
    std::vector<std::uint32_t> allow_hosts;
    allow_hosts.reserve(config.allow.size());
    for (const auto &address : config.allow) {
        std::uint32_t host = 0;
        if (!parseOscListenerIpv4(address, host)) {
            throw std::runtime_error(
                "osc.config_invalid@1: --osc-allow must contain only "
                "dotted-decimal IPv4 addresses: " +
                address);
        }
        allow_hosts.push_back(host);
    }

    {
        std::scoped_lock lock{state.status_mutex};
        state.state = Impl::State::off;
        state.interface_address = config.interface_address;
        state.port = config.port;
        state.error.clear();
        state.interface_host = interface_host;
        state.allow_hosts = std::move(allow_hosts);
    }
    state.stop_requested.store(false, std::memory_order_release);

    const auto fail = [&](std::string message) {
        std::string text;
        {
            std::scoped_lock lock{state.status_mutex};
            state.state = Impl::State::failed;
            state.error = std::move(message);
            state.socket = invalid_socket;
            text = state.error;
        }
        std::fprintf(stderr, "Pelican: OSC listener failed: %s\n",
                     text.c_str());
        return false;
    };

    bool winsock_started = false;
#if defined(_WIN32)
    WSADATA winsock_data{};
    const int startup = ::WSAStartup(MAKEWORD(2, 2), &winsock_data);
    if (startup != 0) {
        return fail("osc.bind_failed@1: cannot initialize Winsock on " +
                    config.interface_address + ":" +
                    std::to_string(config.port) + ": " +
                    socketErrorText(startup));
    }
    winsock_started = true;
#endif

    SocketHandle socket = invalid_socket;
#if defined(_WIN32)
    socket = ::WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                          WSA_FLAG_NO_HANDLE_INHERIT);
#else
    socket = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (socket < 0) {
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket >= 0 &&
            ::fcntl(socket, F_SETFD, FD_CLOEXEC) == -1) {
            const int code = errno;
            closeSocket(socket);
            socket = invalid_socket;
            errno = code;
        }
    }
#endif
    if (socket == invalid_socket) {
        const int code = lastSocketError();
        if (winsock_started) {
#if defined(_WIN32)
            ::WSACleanup();
#endif
        }
        return fail("osc.bind_failed@1: cannot create a UDP socket on " +
                    config.interface_address + ":" +
                    std::to_string(config.port) + ": " +
                    socketErrorText(code));
    }

#if defined(_WIN32)
    BOOL exclusive = TRUE;
    if (::setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char *>(&exclusive),
                     sizeof(exclusive)) == SOCKET_ERROR) {
        const int code = WSAGetLastError();
        closeSocket(socket);
        ::WSACleanup();
        return fail("osc.bind_failed@1: cannot reserve the OSC port on " +
                    config.interface_address + ":" +
                    std::to_string(config.port) + ": " +
                    socketErrorText(code));
    }
#else
    // Best effort: a larger receive buffer does not change the promise, which
    // is only bounded memory, not flood resistance.
    int receive_buffer = 1 << 20;
    (void)::setsockopt(socket, SOL_SOCKET, SO_RCVBUF,
                       &receive_buffer, sizeof(receive_buffer));
#endif

    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons(config.port);
    bind_address.sin_addr.s_addr = htonl(interface_host);
    if (::bind(socket, reinterpret_cast<const sockaddr *>(&bind_address),
               sizeof(bind_address)) != 0) {
        const int code = lastSocketError();
        closeSocket(socket);
#if defined(_WIN32)
        ::WSACleanup();
#endif
        return fail("osc.bind_failed@1: cannot bind " +
                    config.interface_address + ":" +
                    std::to_string(config.port) + ": " +
                    socketErrorText(code));
    }

    sockaddr_in actual{};
#if defined(_WIN32)
    int actual_length = sizeof(actual);
#else
    socklen_t actual_length = sizeof(actual);
#endif
    if (::getsockname(socket, reinterpret_cast<sockaddr *>(&actual),
                      &actual_length) != 0) {
        const int code = lastSocketError();
        closeSocket(socket);
#if defined(_WIN32)
        ::WSACleanup();
#endif
        return fail("osc.bind_failed@1: cannot read the bound OSC endpoint: " +
                    socketErrorText(code));
    }

    {
        std::scoped_lock lock{state.status_mutex};
        state.socket = socket;
        state.port = ntohs(actual.sin_port);
        state.winsock_started = winsock_started;
    }

    try {
        state.worker = std::thread([this] { impl_->runWorker(); });
    } catch (const std::exception &error) {
        {
            std::scoped_lock lock{state.status_mutex};
            state.winsock_started = false;
        }
        closeSocket(socket);
#if defined(_WIN32)
        if (winsock_started) ::WSACleanup();
#endif
        return fail("osc.bind_failed@1: cannot start the OSC listener "
                    "thread: " +
                    std::string{error.what()});
    }

    {
        std::scoped_lock lock{state.status_mutex};
        // The worker can already have reported a receive failure.  Never
        // overwrite that with "listening".
        if (state.state != Impl::State::failed)
            state.state = Impl::State::listening;
    }

    if ((interface_host & 0xff000000u) != 0x7f000000u &&
        state.allow_hosts.empty() && logger != nullptr) {
        LOG_WARNING(logger,
                    "OSC listener on non-loopback interface {} has no "
                    "--osc-allow filter; any sender is accepted",
                    config.interface_address);
    }
    return true;
}

void OscListener::stop() {
    auto &state = *impl_;
    state.stop_requested.store(true, std::memory_order_release);
    if (state.worker.joinable()) state.worker.join();
    const auto discarded = GET_MODULE(OscInput).discardScheduled();
    if (discarded != 0 && logger != nullptr) {
        LOG_INFO(logger, "OSC listener stop discarded {} scheduled messages",
                 discarded);
    }

    SocketHandle socket = invalid_socket;
    bool winsock_started = false;
    {
        std::scoped_lock lock{state.status_mutex};
        socket = state.socket;
        state.socket = invalid_socket;
        winsock_started = state.winsock_started;
        state.winsock_started = false;
        if (state.state == Impl::State::listening) {
            state.state = Impl::State::off;
            state.port = 0;
        }
    }
    closeSocket(socket);
#if defined(_WIN32)
    if (winsock_started) ::WSACleanup();
#else
    (void)winsock_started;
#endif
}

nlohmann::json OscListener::statusJson() const {
    const auto &state = *impl_;
    std::scoped_lock lock{state.status_mutex};
    const char *name = "off";
    if (state.state == Impl::State::listening) name = "listening";
    if (state.state == Impl::State::failed) name = "failed";
    nlohmann::json result{
        {"state", name},
        {"interface", state.interface_address},
        {"port", state.port},
        {"error", state.error},
        {"sender_not_allowed", state.sender_not_allowed},
        {"receive_errors", state.receive_errors},
    };
    return result;
}

bool OscListener::socketOpen() const {
    const auto &state = *impl_;
    std::scoped_lock lock{state.status_mutex};
    return state.socket != invalid_socket;
}

bool OscListener::workerStarted() const {
    const auto &state = *impl_;
    std::scoped_lock lock{state.status_mutex};
    return state.worker.joinable();
}

bool OscListener::localEndpoint(std::string &address,
                                std::uint16_t &port) const {
    const auto &state = *impl_;
    std::scoped_lock lock{state.status_mutex};
    if (state.socket == invalid_socket) return false;

    sockaddr_in actual{};
#if defined(_WIN32)
    int actual_length = sizeof(actual);
#else
    socklen_t actual_length = sizeof(actual);
#endif
    if (::getsockname(state.socket, reinterpret_cast<sockaddr *>(&actual),
                      &actual_length) != 0)
        return false;
    address = ipv4Text(ntohl(actual.sin_addr.s_addr));
    port = ntohs(actual.sin_port);
    return true;
}

bool OscListener::reuseAddressOptionForTesting() const {
#if defined(_WIN32)
    return false;
#else
    const auto &state = *impl_;
    std::scoped_lock lock{state.status_mutex};
    if (state.socket == invalid_socket) return false;
    int value = 0;
    socklen_t length = sizeof(value);
    if (::getsockopt(state.socket, SOL_SOCKET, SO_REUSEADDR, &value, &length) !=
        0)
        return false;
    return value != 0;
#endif
}

} // namespace Pelican
