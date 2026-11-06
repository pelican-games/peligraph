#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace OscTestUdp {

#if defined(_WIN32)
struct WinsockScope {
    WinsockScope() { ok = ::WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~WinsockScope() { if (ok) ::WSACleanup(); }
    WSADATA data{};
    bool ok = false;
};
using Socket = SOCKET;
inline constexpr Socket invalid_socket = INVALID_SOCKET;
inline void closeSocket(Socket socket) { ::closesocket(socket); }
#else
using Socket = int;
inline constexpr Socket invalid_socket = -1;
inline void closeSocket(Socket socket) { ::close(socket); }
#endif

class Receiver {
  public:
    Receiver() {
#if defined(_WIN32)
        static WinsockScope winsock;
        if (!winsock.ok) return;
#endif
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == invalid_socket) return;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        local.sin_addr.s_addr = htonl(0x7f000001u);
        if (::bind(socket_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
            closeSocket(socket_);
            socket_ = invalid_socket;
        }
    }
    ~Receiver() { if (socket_ != invalid_socket) closeSocket(socket_); }
    Receiver(const Receiver &) = delete;
    Receiver &operator=(const Receiver &) = delete;

    std::uint16_t port() const {
        if (socket_ == invalid_socket) return 0;
        sockaddr_in local{};
#if defined(_WIN32)
        int length = sizeof(local);
#else
        socklen_t length = sizeof(local);
#endif
        if (::getsockname(socket_, reinterpret_cast<sockaddr *>(&local), &length) != 0)
            return 0;
        return ntohs(local.sin_port);
    }

    std::optional<std::vector<std::uint8_t>> receive(std::chrono::milliseconds timeout) {
        if (socket_ == invalid_socket) return std::nullopt;
#if defined(_WIN32)
        fd_set readers;
        FD_ZERO(&readers);
        FD_SET(socket_, &readers);
        timeval wait{};
        wait.tv_sec = static_cast<long>(timeout.count() / 1000);
        wait.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
        if (::select(0, &readers, nullptr, nullptr, &wait) <= 0) return std::nullopt;
#else
        pollfd reader{socket_, POLLIN, 0};
        if (::poll(&reader, 1, static_cast<int>(timeout.count())) <= 0) return std::nullopt;
#endif
        std::vector<std::uint8_t> bytes(65536);
#if defined(_WIN32)
        const int count = ::recv(socket_, reinterpret_cast<char *>(bytes.data()),
                                 static_cast<int>(bytes.size()), 0);
#else
        const auto count = ::recv(socket_, bytes.data(), bytes.size(), 0);
#endif
        if (count < 0) return std::nullopt;
        bytes.resize(static_cast<std::size_t>(count));
        return bytes;
    }

  private:
    Socket socket_ = invalid_socket;
};

class Sender {
  public:
    Sender() {
#if defined(_WIN32)
        static WinsockScope winsock;
        if (!winsock.ok) return;
#endif
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == invalid_socket) return;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        local.sin_addr.s_addr = htonl(0x7f000001u);
        if (::bind(socket_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
            closeSocket(socket_);
            socket_ = invalid_socket;
        }
    }
    ~Sender() { if (socket_ != invalid_socket) closeSocket(socket_); }
    bool valid() const { return socket_ != invalid_socket; }
    bool sendTo(std::uint16_t port, const std::vector<std::uint8_t> &bytes) const {
        if (socket_ == invalid_socket) return false;
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(port);
        destination.sin_addr.s_addr = htonl(0x7f000001u);
#if defined(_WIN32)
        const auto sent = ::sendto(socket_, reinterpret_cast<const char *>(bytes.data()),
                                   static_cast<int>(bytes.size()), 0,
                                   reinterpret_cast<sockaddr *>(&destination), sizeof(destination));
#else
        const auto sent = ::sendto(socket_, bytes.data(), bytes.size(), 0,
                                   reinterpret_cast<sockaddr *>(&destination), sizeof(destination));
#endif
        return sent >= 0 && static_cast<std::size_t>(sent) == bytes.size();
    }
  private:
    Socket socket_ = invalid_socket;
};

} // namespace OscTestUdp
