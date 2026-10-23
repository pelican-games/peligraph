#pragma once
#include <chrono>
#include <cstdint>

namespace PelicanStudio {
// GUI-thread only, allocation-free sink. No dependency on a particular view.
struct GuiTrace {
    using Sink = void (*)(void *, const char *, int64_t, int64_t, double, double);
    static inline thread_local Sink sink = nullptr;
    static inline thread_local void *owner = nullptr;
    static int64_t now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};
class GuiTraceSpan {
public:
    explicit GuiTraceSpan(const char *stage, double detail = 0)
        : sink_(GuiTrace::sink), owner_(GuiTrace::owner), stage_(stage),
          begin_(sink_ ? GuiTrace::now() : 0), detail_(detail) {}
    ~GuiTraceSpan() {
        if (sink_ && GuiTrace::owner == owner_ && GuiTrace::sink == sink_)
            sink_(owner_, stage_, begin_, GuiTrace::now(), detail_, 0);
    }
private:
    GuiTrace::Sink sink_;
    void *owner_;
    const char *stage_;
    int64_t begin_;
    double detail_;
};
}
