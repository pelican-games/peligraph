#pragma once

#include <cerrno>
#include <cstdint>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Pelican::Osc {

#if defined(_WIN32)
using SocketHandle = SOCKET;
inline constexpr SocketHandle invalid_socket = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle invalid_socket = -1;
#endif

inline int lastSocketError() noexcept {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

inline std::string socketErrorText(int code) {
    if (code == 0) return "unknown socket error";
    return std::error_code{code, std::system_category()}.message() + " (" +
           std::to_string(code) + ")";
}

inline bool interruptedError(int code) noexcept {
#if defined(_WIN32)
    return code == WSAEINTR;
#else
    return code == EINTR;
#endif
}

inline bool wouldBlockError(int code) noexcept {
#if defined(_WIN32)
    return code == WSAEWOULDBLOCK;
#else
    return code == EAGAIN || code == EWOULDBLOCK;
#endif
}

inline bool harmlessDatagramError(int code) noexcept {
#if defined(_WIN32)
    return code == WSAECONNRESET || code == WSAEMSGSIZE;
#else
    (void)code;
    return false;
#endif
}

inline void closeSocket(SocketHandle socket) noexcept {
#if defined(_WIN32)
    if (socket != invalid_socket) ::closesocket(socket);
#else
    if (socket != invalid_socket) ::close(socket);
#endif
}

inline std::string ipv4Text(std::uint32_t host_order) {
    return std::to_string((host_order >> 24) & 0xffu) + "." +
           std::to_string((host_order >> 16) & 0xffu) + "." +
           std::to_string((host_order >> 8) & 0xffu) + "." +
           std::to_string(host_order & 0xffu);
}

inline SocketHandle createDatagramSocket() noexcept {
#if defined(_WIN32)
    return ::WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                        WSA_FLAG_NO_HANDLE_INHERIT);
#else
    SocketHandle socket = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (socket >= 0) return socket;
    socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket >= 0 && ::fcntl(socket, F_SETFD, FD_CLOEXEC) == -1) {
        const int code = errno;
        closeSocket(socket);
        errno = code;
        return invalid_socket;
    }
    return socket;
#endif
}

inline bool makeSocketNonBlocking(SocketHandle socket) noexcept {
#if defined(_WIN32)
    u_long enabled = 1;
    return ::ioctlsocket(socket, FIONBIO, &enabled) != SOCKET_ERROR;
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

} // namespace Pelican::Osc
