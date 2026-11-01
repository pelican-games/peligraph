#pragma once
// LT-1: an engine-side latency trace for the windowed RPC path. It records
// nothing unless PELICAN_RPC_TRACE names a file when the process starts; then
// every request handled at a frame boundary appends one JSON line (receipt,
// the boundary that took it, the handler, the reply), and Span appends the
// stages inside. Times are steady_clock nanoseconds -- QueryPerformanceCounter
// on Windows, the clock Studio's GuiTrace and Python's perf_counter read -- so
// a request and the Studio spans around it share one time axis.
//
// Disabled, every entry point is one test of RpcTrace::enabled() (a bool read
// once at static initialisation) and returns.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace Pelican::RpcTrace {

inline std::int64_t now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Sink {
    bool enabled = false;
    std::mutex mutex;
    std::ofstream file;
    Sink() {
#ifdef _MSC_VER
        char *path = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&path, &length, "PELICAN_RPC_TRACE") != 0 || path == nullptr) return;
        const std::string name{path};
        std::free(path);
#else
        const char *path = std::getenv("PELICAN_RPC_TRACE");
        if (path == nullptr) return;
        const std::string name{path};
#endif
        if (name.empty()) return;
        file.open(name, std::ios::binary | std::ios::app);
        enabled = file.is_open();
    }
    void write(const std::string &line) {
        const std::lock_guard lock{mutex};
        file << line << '\n';
        file.flush();
    }
};

inline Sink sink;

inline bool enabled() noexcept { return sink.enabled; }

// What handleLine saw of the request it answered last on this thread, for
// the host that called it. Written only while tracing.
struct HandledRequest {
    std::string method;
    std::string id;
    std::int64_t begin = 0;
    std::int64_t end = 0;
};
inline thread_local HandledRequest handled;

class HandlerMark {
  public:
    // A parsed JSON-RPC request (method, id); read only while tracing.
    template <class Request>
    explicit HandlerMark(const Request &request) : on_{enabled()} {
        if (!on_) return;
        handled.method = request.method;
        handled.id = request.id.dump();
        handled.begin = now();
        handled.end = 0;
    }
    ~HandlerMark() {
        if (on_) handled.end = now();
    }
    HandlerMark(const HandlerMark &) = delete;
    HandlerMark &operator=(const HandlerMark &) = delete;

  private:
    bool on_;
};

inline std::string jsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (static_cast<unsigned char>(c) < 0x20) out.push_back(' ');
        else out.push_back(c);
    }
    return out;
}

struct RequestTimes {
    std::int64_t received = 0;   // the reader thread had the whole line
    std::int64_t boundary = 0;   // the frame boundary that took the batch began
    std::int64_t dispatched = 0; // this request's turn in the batch began
    std::int64_t written = 0;    // the reply was flushed
    std::size_t request_bytes = 0;
    std::size_t response_bytes = 0;
    std::size_t batch_index = 0;
    std::size_t batch_size = 0;
};

inline void writeRequest(const RequestTimes &times) {
    if (!enabled()) return;
    std::string line = "{\"kind\":\"rpc\",\"method\":\"" + jsonEscape(handled.method) +
        "\",\"id\":\"" + jsonEscape(handled.id) +
        "\",\"received_ns\":" + std::to_string(times.received) +
        ",\"boundary_ns\":" + std::to_string(times.boundary) +
        ",\"dispatched_ns\":" + std::to_string(times.dispatched) +
        ",\"handler_begin_ns\":" + std::to_string(handled.begin) +
        ",\"handler_end_ns\":" + std::to_string(handled.end) +
        ",\"written_ns\":" + std::to_string(times.written) +
        ",\"request_bytes\":" + std::to_string(times.request_bytes) +
        ",\"response_bytes\":" + std::to_string(times.response_bytes) +
        ",\"batch_index\":" + std::to_string(times.batch_index) +
        ",\"batch_size\":" + std::to_string(times.batch_size) + "}";
    sink.write(line);
}

// A stage inside the engine (an edit's transform, a commit, an apply). Pass
// `when = false` to skip a span that would otherwise run every frame.
class Span {
  public:
    explicit Span(const char *stage, double detail = 0, bool when = true)
        : stage_{stage}, on_{enabled() && when}, begin_{on_ ? now() : 0}, detail_{detail} {}
    ~Span() {
        if (!on_) return;
        const auto end = now();
        sink.write(std::string{"{\"kind\":\"span\",\"stage\":\""} + stage_ +
                   "\",\"begin_ns\":" + std::to_string(begin_) +
                   ",\"end_ns\":" + std::to_string(end) +
                   ",\"detail\":" + std::to_string(detail_) + "}");
    }
    Span(const Span &) = delete;
    Span &operator=(const Span &) = delete;

  private:
    const char *stage_;
    bool on_;
    std::int64_t begin_;
    double detail_;
};

} // namespace Pelican::RpcTrace
