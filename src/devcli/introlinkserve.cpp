#include "introlink.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Pelican::DevCli::IntroLink {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---- wire (v1, little-endian; frame = u32 type, u32 payload_bytes, payload) ----

constexpr std::uint32_t kMagic = 0x4B4E4C50; // 'PLNK'
constexpr std::uint32_t kVersion = 1;
enum : std::uint32_t {
    kHello = 0,
    kTrack = 1,
    kShader = 2,
    kShaderError = 3,
    kTime = 4,
    kSnapshotEnd = 5,
};

void putU16(std::string &out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>(value >> 8));
}
void putU32(std::string &out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}
void putF32(std::string &out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    putU32(out, bits);
}
void putName(std::string &out, const std::string &name) {
    putU16(out, static_cast<std::uint16_t>(name.size()));
    out += name;
}
std::uint32_t getU32(const unsigned char *p) {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) |
           (std::uint32_t{p[3]} << 24);
}

std::string frame(std::uint32_t type, const std::string &payload) {
    std::string out;
    putU32(out, type);
    putU32(out, static_cast<std::uint32_t>(payload.size()));
    return out + payload;
}

std::string trackFrame(const std::string &name, const Track &track) {
    std::string payload;
    putName(payload, name);
    putU32(payload, static_cast<std::uint32_t>(track.keys.size()));
    for (const auto &key : track.keys) {
        putF32(payload, key.time);
        putF32(payload, key.value);
        putF32(payload, key.in_tangent);
        putF32(payload, key.out_tangent);
        putU32(payload, key.interpolation);
    }
    return frame(kTrack, payload);
}

std::string shaderFrame(const std::string &name, const std::vector<std::uint32_t> &words) {
    std::string payload;
    putName(payload, name);
    putU32(payload, static_cast<std::uint32_t>(words.size()));
    for (const auto word : words) putU32(payload, word);
    return frame(kShader, payload);
}

std::string shaderErrorFrame(const std::string &name, const std::string &text) {
    std::string payload;
    putName(payload, name);
    putU32(payload, static_cast<std::uint32_t>(text.size()));
    payload += text;
    return frame(kShaderError, payload);
}

std::string timeFrame(std::uint32_t flags, float time) {
    std::string payload;
    putU32(payload, flags);
    putF32(payload, time);
    return frame(kTime, payload);
}

// ---- sockets ----

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void closeSocket(SocketHandle socket) { closesocket(socket); }
int lastSocketError() { return WSAGetLastError(); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void closeSocket(SocketHandle socket) { ::close(socket); }
int lastSocketError() { return errno; }
#endif

class SocketLibrary {
  public:
    SocketLibrary() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
#endif
    }
    ~SocketLibrary() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
    SocketLibrary(const SocketLibrary &) = delete;
    SocketLibrary &operator=(const SocketLibrary &) = delete;
};

bool sendAll(SocketHandle socket, const std::string &bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto chunk = static_cast<int>(std::min<std::size_t>(bytes.size() - sent, 1 << 20));
        const auto result = ::send(socket, bytes.data() + sent, chunk, 0);
        if (result <= 0) return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

// ---- stdin commands (read on their own thread; EOF just ends the thread) ----

struct CommandQueue {
    std::mutex mutex;
    std::deque<std::string> lines;
};

void readStdin(std::shared_ptr<CommandQueue> queue) {
    std::string pending;
    std::array<char, 4096> buffer{};
    for (;;) {
#ifdef _WIN32
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (input == nullptr || input == INVALID_HANDLE_VALUE) return;
        DWORD read = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) {
            break;
        }
        const std::size_t count = read;
#else
        const auto result = ::read(STDIN_FILENO, buffer.data(), buffer.size());
        if (result <= 0) break;
        const auto count = static_cast<std::size_t>(result);
#endif
        pending.append(buffer.data(), count);
        std::vector<std::string> lines;
        for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
            lines.push_back(pending.substr(0, end));
            pending.erase(0, end + 1);
        }
        if (!lines.empty()) {
            const std::lock_guard lock{queue->mutex};
            for (auto &line : lines) queue->lines.push_back(std::move(line));
        }
    }
    if (!pending.empty()) {
        const std::lock_guard lock{queue->mutex};
        queue->lines.push_back(pending);
    }
}

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// ---- server state ----

struct TimeState {
    bool paused = false;
    bool seeked = false;
    double anchor = 0;
    Clock::time_point anchor_at = Clock::now();

    double current(Clock::time_point now) const {
        if (paused) return anchor;
        return anchor + std::chrono::duration<double>(now - anchor_at).count();
    }
};

struct ShaderState {
    ShaderSource source;
    std::optional<std::vector<std::uint32_t>> words; // last good SPIR-V
    std::optional<std::string> error;                // last failed compile
};

struct Connection {
    SocketHandle socket = kInvalidSocket;
    std::vector<unsigned char> input;
    bool greeted = false;
    bool first = false;
};

// A file set's change signature: path -> (mtime, size).
using Signature = std::map<fs::path, std::pair<fs::file_time_type::rep, std::uintmax_t>>;

void addSignature(Signature &signature, const fs::path &path) {
    std::error_code error;
    const auto time = fs::last_write_time(path, error);
    const auto size = fs::file_size(path, error);
    signature[path] = {error ? 0 : time.time_since_epoch().count(), error ? 0 : size};
}

Signature graphSignature(const fs::path &root) {
    Signature signature;
    std::error_code error;
    for (fs::directory_iterator it{root / "passes", error}, end; !error && it != end; it.increment(error)) {
        if (it->path().extension() == ".json") addSignature(signature, it->path());
    }
    return signature;
}

Signature shaderFileSignature(const fs::path &shaders_dir) {
    Signature signature;
    std::error_code error;
    for (fs::directory_iterator it{shaders_dir, error}, end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error)) addSignature(signature, it->path());
    }
    return signature;
}

Signature includeSignature(const fs::path &shaders_dir) {
    Signature signature;
    std::error_code error;
    for (fs::recursive_directory_iterator it{shaders_dir, error}, end; !error && it != end;
         it.increment(error)) {
        // Sub-directories are include search space only; any file there can
        // be included by any top-level shader.
        if (it.depth() > 0 && it->is_regular_file(error)) addSignature(signature, it->path());
    }
    return signature;
}

class Server {
  public:
    explicit Server(const ServeOptions &options) : options_(options) {}

    int run() {
        tracks_ = collectTracks(options_.root);
        for (auto &[name, source] : collectShaders(options_.root, options_.shaders_dir)) {
            shaders_.emplace(name, ShaderState{source, std::nullopt, std::nullopt});
            compile(name);
        }
        graph_signature_ = graphSignature(options_.root);
        shader_signature_ = shaderFileSignature(options_.shaders_dir);
        include_signature_ = includeSignature(options_.shaders_dir);

        SocketLibrary library;
        listen();
        std::cout << "port " << port_ << std::endl; // first stdout line; flushed

        std::thread{readStdin, commands_}.detach();
        auto next_watch = Clock::now();
        for (;;) {
            const auto now = Clock::now();
            if (!drainCommands(now)) break;
            if (!pump()) break;
            if (now >= next_watch) {
                watch();
                next_watch = Clock::now() + std::chrono::milliseconds{100};
            }
        }
        for (auto &connection : connections_) closeSocket(connection.socket);
        closeSocket(listener_);
        return 0;
    }

  private:
    void listen() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == kInvalidSocket) {
            throw Error("intro_link.port_unavailable@1",
                        "could not create a socket (error " + std::to_string(lastSocketError()) + ")");
        }
#ifdef _WIN32
        BOOL exclusive = TRUE;
        setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive),
                   sizeof(exclusive));
#else
        int reuse = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(options_.port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 8) != 0) {
            const auto error = lastSocketError();
            closeSocket(listener_);
            throw Error("intro_link.port_unavailable@1", "127.0.0.1:" + std::to_string(options_.port) +
                                                              " (error " + std::to_string(error) + ")");
        }
        sockaddr_in bound{};
#ifdef _WIN32
        int length = sizeof(bound);
#else
        socklen_t length = sizeof(bound);
#endif
        getsockname(listener_, reinterpret_cast<sockaddr *>(&bound), &length);
        port_ = ntohs(bound.sin_port);
    }

    // Applies queued stdin commands. Returns false on `quit`.
    bool drainCommands(Clock::time_point now) {
        std::deque<std::string> lines;
        {
            const std::lock_guard lock{commands_->mutex};
            lines.swap(commands_->lines);
        }
        for (const auto &raw : lines) {
            const auto line = trim(raw);
            if (line.empty()) continue;
            std::istringstream words{line};
            std::string command;
            words >> command;
            if (command == "quit") return false;
            if (command == "play") {
                if (time_.paused) {
                    time_.paused = false;
                    time_.anchor_at = now;
                    broadcast(timeFrame(0, static_cast<float>(time_.current(now))));
                }
            } else if (command == "pause") {
                if (!time_.paused) {
                    time_.anchor = time_.current(now);
                    time_.paused = true;
                    broadcast(timeFrame(1, static_cast<float>(time_.anchor)));
                }
            } else if (command == "seek") {
                double seconds = 0;
                std::string rest;
                if (!(words >> seconds) || (words >> rest) || !std::isfinite(seconds)) {
                    std::cerr << "intro-link serve: seek needs one number of seconds: " << line << std::endl;
                    continue;
                }
                time_.anchor = seconds;
                time_.anchor_at = now;
                time_.seeked = true;
                broadcast(timeFrame((time_.paused ? 1u : 0u) | 2u, static_cast<float>(seconds)));
            } else {
                std::cerr << "intro-link serve: unknown command (play, pause, seek <seconds>, quit): " << line
                          << std::endl;
            }
        }
        return true;
    }

    // Waits up to 10 ms for sockets. Returns false when --exit-after-client
    // has seen its first client leave.
    bool pump() {
        bool keep_running = true;
        // Sockets a failed broadcast closed.
        for (auto it = connections_.begin(); it != connections_.end();) {
            if (it->socket != kInvalidSocket) {
                ++it;
                continue;
            }
            if (it->first && options_.exit_after_client) keep_running = false;
            it = connections_.erase(it);
        }
        if (!keep_running) return false;

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener_, &readable);
        SocketHandle highest = listener_;
        for (const auto &connection : connections_) {
            FD_SET(connection.socket, &readable);
            highest = std::max(highest, connection.socket);
        }
        timeval timeout{0, 10000};
        const int ready = ::select(static_cast<int>(highest + 1), &readable, nullptr, nullptr, &timeout);
        if (ready <= 0) return true;

        if (FD_ISSET(listener_, &readable)) {
            const auto socket = ::accept(listener_, nullptr, nullptr);
            if (socket != kInvalidSocket) {
                int no_delay = 1;
                setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&no_delay),
                           sizeof(no_delay));
                Connection connection;
                connection.socket = socket;
                connection.first = !had_client_;
                had_client_ = true;
                connections_.push_back(std::move(connection));
            }
        }
        for (auto it = connections_.begin(); it != connections_.end();) {
            bool alive = true;
            if (FD_ISSET(it->socket, &readable)) alive = receive(*it);
            if (!alive) {
                closeSocket(it->socket);
                if (it->first && options_.exit_after_client) keep_running = false;
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
        return keep_running;
    }

    bool receive(Connection &connection) {
        std::array<char, 4096> buffer{};
        const auto count = ::recv(connection.socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (count <= 0) return false;
        connection.input.insert(connection.input.end(), buffer.data(), buffer.data() + count);
        while (connection.input.size() >= 8) {
            const auto type = getU32(connection.input.data());
            const auto bytes = getU32(connection.input.data() + 4);
            if (bytes > (1u << 20)) return false;
            if (connection.input.size() < 8 + std::size_t{bytes}) break;
            if (type == kHello && !connection.greeted) {
                const auto magic = bytes >= 8 ? getU32(connection.input.data() + 8) : 0;
                const auto version = bytes >= 8 ? getU32(connection.input.data() + 12) : 0;
                if (magic != kMagic || version != kVersion) {
                    std::cerr << "intro-link serve: dropped a client whose HELLO is magic 0x" << std::hex << magic
                              << std::dec << " version " << version << " (this server speaks PLNK version "
                              << kVersion << ")" << std::endl;
                    return false;
                }
                connection.greeted = true;
                if (!sendAll(connection.socket, snapshot())) return false;
            }
            // Other client frames are not part of v1; skip them.
            connection.input.erase(connection.input.begin(), connection.input.begin() + 8 + bytes);
        }
        return true;
    }

    std::string snapshot() const {
        std::string bytes;
        for (const auto &[name, track] : tracks_) bytes += trackFrame(name, track);
        for (const auto &[name, shader] : shaders_) {
            if (shader.words) bytes += shaderFrame(name, *shader.words);
            if (shader.error) bytes += shaderErrorFrame(name, *shader.error);
        }
        const auto now = Clock::now();
        bytes += timeFrame((time_.paused ? 1u : 0u) | (time_.seeked ? 2u : 0u),
                           static_cast<float>(time_.current(now)));
        bytes += frame(kSnapshotEnd, {});
        return bytes;
    }

    void broadcast(const std::string &bytes) {
        for (auto &connection : connections_) {
            if (!connection.greeted || connection.socket == kInvalidSocket) continue;
            if (!sendAll(connection.socket, bytes)) {
                closeSocket(connection.socket);
                connection.socket = kInvalidSocket; // removed by the next pump
            }
        }
    }

    // Says a recurring problem once, until it changes or goes away.
    void report(std::optional<std::string> &last, const std::string &message) {
        if (last && *last == message) return;
        std::cerr << message << std::endl;
        last = message;
    }

    void compile(const std::string &name) {
        auto &shader = shaders_.at(name);
        const auto result = compileShader(options_.glslc, options_.shaders_dir, shader.source);
        if (result.ok) {
            shader.words = result.words;
            shader.error.reset();
        } else {
            shader.error = result.log.empty() ? std::string{"glslc failed"} : result.log;
            std::cerr << "intro-link serve: shader " << name << " (" << shader.source.relative
                      << ") failed to compile; clients keep the previous SPIR-V" << std::endl;
        }
    }

    void watch() {
        watchTracks();
        watchShaders();
    }

    void watchTracks() {
        auto signature = graphSignature(options_.root);
        if (signature == graph_signature_) return;
        std::map<std::string, Track> fresh;
        try {
            fresh = collectTracks(options_.root);
        } catch (const std::exception &error) {
            // Likely a half-written document; try again next tick.
            report(track_problem_, std::string{"intro-link serve: keeping the previous tracks: "} + error.what());
            return;
        }
        track_problem_.reset();
        graph_signature_ = std::move(signature);
        for (const auto &[name, track] : fresh) {
            const auto old = tracks_.find(name);
            if (old == tracks_.end() || old->second.keys != track.keys) broadcast(trackFrame(name, track));
        }
        for (const auto &[name, track] : tracks_) {
            if (!fresh.count(name)) {
                std::cerr << "intro-link serve: track " << name
                          << " is gone from the project; clients keep its last keys" << std::endl;
            }
        }
        tracks_ = std::move(fresh);
    }

    void watchShaders() {
        auto files = shaderFileSignature(options_.shaders_dir);
        auto includes = includeSignature(options_.shaders_dir);
        if (files == shader_signature_ && includes == include_signature_) return;
        std::map<std::string, ShaderSource> sources;
        try {
            sources = collectShaders(options_.root, options_.shaders_dir);
        } catch (const std::exception &error) {
            report(shader_problem_, std::string{"intro-link serve: shader set unchanged: "} + error.what());
            return;
        }
        shader_problem_.reset();
        const bool includes_changed = includes != include_signature_;
        for (const auto &[name, source] : sources) {
            auto existing = shaders_.find(name);
            const auto before = shader_signature_.find(source.path);
            const auto after = files.find(source.path);
            const bool changed = existing == shaders_.end() || includes_changed || before == shader_signature_.end() ||
                                 after == files.end() || before->second != after->second;
            if (!changed) continue;
            if (existing == shaders_.end()) {
                existing = shaders_.emplace(name, ShaderState{source, std::nullopt, std::nullopt}).first;
            }
            const auto previous = existing->second.words;
            const bool had_error = existing->second.error.has_value();
            compile(name);
            const auto &state = existing->second;
            if (state.error) {
                broadcast(shaderErrorFrame(name, *state.error));
            } else if (had_error || !previous || *previous != *state.words) {
                // A SHADER also clears the client's last error.
                broadcast(shaderFrame(name, *state.words));
            }
        }
        shader_signature_ = std::move(files);
        include_signature_ = std::move(includes);
    }

    ServeOptions options_;
    std::map<std::string, Track> tracks_;
    std::map<std::string, ShaderState> shaders_;
    TimeState time_;
    Signature graph_signature_;
    Signature shader_signature_;
    Signature include_signature_;
    std::optional<std::string> track_problem_;
    std::optional<std::string> shader_problem_;
    std::shared_ptr<CommandQueue> commands_ = std::make_shared<CommandQueue>();
    SocketHandle listener_ = kInvalidSocket;
    int port_ = 0;
    std::vector<Connection> connections_;
    bool had_client_ = false;
};

} // namespace

int runServe(const ServeOptions &options) {
    Server server{options};
    return server.run();
}

} // namespace Pelican::DevCli::IntroLink
