#include "engineattach.hpp"

#include <algorithm>

namespace PelicanStudio {

EngineAttachPolicy::EngineAttachPolicy(int window_timeout_ms,
                                       int load_timeout_ms) noexcept
    : window_timeout_ms_{std::max(1, window_timeout_ms)},
      load_timeout_ms_{std::max(1, load_timeout_ms)} {}

void EngineAttachPolicy::engineStarted(qint64 now_ms) noexcept {
    started_ms_ = now_ms;
    spoke_ms_ = now_ms;
    window_ms_ = -1;
}

void EngineAttachPolicy::engineSpoke(qint64 now_ms) noexcept {
    spoke_ms_ = std::max(spoke_ms_, now_ms);
}

EngineAttachDecision EngineAttachPolicy::decide(bool window_present,
                                                qint64 now_ms) noexcept {
    const auto since = [now_ms](qint64 mark) {
        return std::max<qint64>(0, now_ms - mark);
    };

    if (!window_present) {
        // Deliberately measured from the last line the engine wrote rather
        // than from launch. An engine that is loading a large project writes
        // its log the whole way; killing it for being slow is the one thing
        // this timeout must never do.
        const qint64 silent = since(std::max(started_ms_, spoke_ms_));
        if (silent >= window_timeout_ms_) {
            return {.step = EngineAttachStep::give_up_no_window,
                    .next_interval_ms = 0,
                    .waiting_ms = since(started_ms_),
                    .silent_ms = silent};
        }
        return {.step = EngineAttachStep::search_window,
                .next_interval_ms = EngineWindowSearchIntervalMs,
                .waiting_ms = since(started_ms_),
                .silent_ms = silent};
    }

    if (window_ms_ < 0) {
        window_ms_ = now_ms;
    }
    const qint64 waiting = since(window_ms_);
    const qint64 silent = since(std::max(window_ms_, spoke_ms_));
    if (silent >= load_timeout_ms_) {
        return {.step = EngineAttachStep::give_up_no_response,
                .next_interval_ms = 0,
                .waiting_ms = waiting,
                .silent_ms = silent};
    }
    return {.step = EngineAttachStep::probe_window,
            .next_interval_ms = waiting < EngineWindowProbeEagerMs
                                    ? EngineWindowProbeIntervalMs
                                    : EngineWindowProbeBackoffMs,
            .waiting_ms = waiting,
            .silent_ms = silent};
}

} // namespace PelicanStudio
