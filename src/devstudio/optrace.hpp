#pragma once
// LT-1: the operation trace. A GuiTrace sink (guitrace.hpp) that keeps every
// span the GUI thread reports, installed only when PELICAN_STUDIO_OP_TRACE is
// set when Studio starts. Without it nothing is installed, GuiTrace::sink
// stays null and every GuiTraceSpan is one test of that pointer.
//
// The automation command `op_trace` writes what was kept to a file (and may
// clear it); tools/studio_latency.py lines it up with the driver's operation
// windows and the engine's PELICAN_RPC_TRACE lines on the steady clock.
#include "guitrace.hpp"

#include <QByteArray>
#include <QSaveFile>
#include <QString>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace PelicanStudio {

class OpTrace {
public:
    struct Record {
        const char *stage;
        int64_t begin;
        int64_t end;
        double x;
        double y;
    };
    static constexpr std::size_t Capacity = std::size_t{1} << 20;

    // The installed trace, or null (the default).
    static OpTrace *instance() { return instance_; }

    static void installFromEnvironment() {
        if (!qEnvironmentVariableIsSet("PELICAN_STUDIO_OP_TRACE")) return;
        static OpTrace trace;
        instance_ = &trace;
        trace.records_.resize(Capacity);
        GuiTrace::resting_owner = &trace;
        GuiTrace::resting_sink = &OpTrace::receive;
        if (GuiTrace::sink == nullptr) {
            GuiTrace::owner = &trace;
            GuiTrace::sink = &OpTrace::receive;
        }
    }

    // Takes the trace back out (a test that installed one leaves none behind).
    static void uninstall() {
        if (instance_ == nullptr) return;
        if (GuiTrace::owner == instance_) { GuiTrace::sink = nullptr; GuiTrace::owner = nullptr; }
        if (GuiTrace::resting_owner == instance_) {
            GuiTrace::resting_sink = nullptr;
            GuiTrace::resting_owner = nullptr;
        }
        instance_->written_ = instance_->first_ = instance_->overflow_ = 0;
        instance_ = nullptr;
    }

    std::size_t written() const { return written_; }
    std::size_t kept() const { return written_ - first_; }
    std::size_t overflow() const { return overflow_; }

    // One JSON object per line: {"s":stage,"b":ns,"e":ns,"x":..,"y":..}.
    bool write(const QString &path, bool clear) {
        QSaveFile file{path};
        if (!file.open(QIODevice::WriteOnly)) return false;
        QByteArray out;
        out.reserve(int(std::min<std::size_t>(kept(), 200000) * 80));
        char line[512];
        for (std::size_t i = first_; i < written_; ++i) {
            const Record &r = records_[i % Capacity];
            const int n = std::snprintf(line, sizeof line,
                "{\"s\":\"%s\",\"b\":%lld,\"e\":%lld,\"x\":%.17g,\"y\":%.17g}\n",
                r.stage, static_cast<long long>(r.begin), static_cast<long long>(r.end), r.x, r.y);
            if (n > 0) out.append(line, std::min<int>(n, int(sizeof line) - 1));
        }
        const bool ok = file.write(out) == out.size() && file.commit();
        if (ok && clear) first_ = written_;
        return ok;
    }

private:
    static void receive(void *owner, const char *stage, int64_t begin, int64_t end, double x, double y) {
        auto *trace = static_cast<OpTrace *>(owner);
        if (trace->written_ - trace->first_ >= Capacity) {
            ++trace->first_;
            ++trace->overflow_;
        }
        trace->records_[trace->written_++ % Capacity] = Record{stage, begin, end, x, y};
    }

    static inline OpTrace *instance_ = nullptr;
    std::vector<Record> records_;
    std::size_t written_ = 0;
    std::size_t first_ = 0;
    std::size_t overflow_ = 0;
};

} // namespace PelicanStudio
