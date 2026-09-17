#pragma once

#include <QtTypes>

namespace PelicanStudio {

// Q-4. A window exists long before the thread that owns it starts taking
// messages. The engine creates its window in the first fifth of a second and
// then loads the project on that same thread, reaching its message loop only
// when the load is over -- 1.7 s for a small project, 16 s for `example`, in a
// Debug build. Re-parenting a window sends WM_STYLECHANGING to the thread that
// owns it, so embedding before that moment blocks Studio's UI thread for the
// whole of the remaining load. These are the numbers and the states of waiting
// for that moment instead.

// No window at all, and nothing in the engine's log either: something is wrong
// rather than slow. The budget is measured from the last sign of life, not
// from launch, so an engine that is still talking is still starting.
inline constexpr int EngineWindowDiscoveryTimeoutMs = 15000;
// A window that is there but silent is an engine that is loading. That is
// allowed to take a long time; what is not allowed is taking it in silence.
inline constexpr int EngineLoadTimeoutMs = 120000;
// How often the window is looked for, and how often its thread is asked
// whether it is taking messages yet. The eager period is what makes a small
// project embed promptly; the backoff is what keeps the probe's own cost off
// the UI thread during a long load.
inline constexpr int EngineWindowSearchIntervalMs = 25;
inline constexpr int EngineWindowProbeIntervalMs = 25;
inline constexpr int EngineWindowProbeBackoffMs = 150;
inline constexpr int EngineWindowProbeEagerMs = 1000;
// The ceiling on one probe's cost to the UI thread. SendMessageTimeoutW blocks
// for at most this long, and returns at once once Windows has decided the
// receiving thread is not answering.
inline constexpr int EngineWindowProbeTimeoutMs = 10;

enum class EngineAttachStep {
    // No window of the engine's yet; keep looking.
    search_window,
    // None appeared, and the engine has stopped saying anything.
    give_up_no_window,
    // There is a window: ask its thread whether it is taking messages.
    probe_window,
    // It never answered and the log stopped moving too.
    give_up_no_response,
};

struct EngineAttachDecision {
    EngineAttachStep step = EngineAttachStep::search_window;
    // 0 means "stop the timer": the two give-up steps are terminal.
    int next_interval_ms = EngineWindowSearchIntervalMs;
    // Since the window appeared (or, before that, since the engine started).
    qint64 waiting_ms = 0;
    // Since the last sign of life -- a log line, or the window appearing.
    qint64 silent_ms = 0;
};

// Deterministic policy. The caller owns the timer, the window search, the
// probe and the killing; it supplies a monotonic clock and what it observed.
class EngineAttachPolicy {
  public:
    EngineAttachPolicy() noexcept = default;
    EngineAttachPolicy(int window_timeout_ms, int load_timeout_ms) noexcept;

    // The engine process has just started: forget everything about the last.
    void engineStarted(qint64 now_ms) noexcept;
    // The engine wrote a line. Loading is not silence.
    void engineSpoke(qint64 now_ms) noexcept;

    EngineAttachDecision decide(bool window_present, qint64 now_ms) noexcept;

    [[nodiscard]] bool windowSeen() const noexcept { return window_ms_ >= 0; }

  private:
    int window_timeout_ms_ = EngineWindowDiscoveryTimeoutMs;
    int load_timeout_ms_ = EngineLoadTimeoutMs;
    qint64 started_ms_ = 0;
    qint64 spoke_ms_ = 0;
    qint64 window_ms_ = -1;
};

} // namespace PelicanStudio
