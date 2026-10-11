#pragma once

#include <QSize>

#include <optional>

namespace PelicanStudio {

// Qt widget sizes are device-independent. A foreign native child must instead
// be sized in the physical pixels used by its Vulkan framebuffer.
QSize embeddedViewportPixelExtent(const QSize &logical_extent, qreal device_pixel_ratio) noexcept;

// Keep intermediate native resizes sparse, then deliver the final dimensions
// after a quiet interval (also used for dock drags without a native release).
inline constexpr int EmbeddedViewportResizeIntervalMs = 150;
inline constexpr int EmbeddedViewportResizeQuietMs = 100;

enum class ViewportExtentChangeKind {
    resize,
    device_pixel_ratio,
    interaction_finished,
};

struct ViewportResizeDecision {
    std::optional<QSize> extent_to_apply;
    std::optional<int> next_wakeup_ms;
};

// Deterministic throttle with a trailing flush. The caller supplies a monotonic
// timestamp and owns the timer and native resize side effects.
class ViewportResizeCoalescer {
  public:
    ViewportResizeDecision request(const QSize &extent, qint64 now_ms,
                                   ViewportExtentChangeKind kind) noexcept;
    ViewportResizeDecision timerExpired(qint64 now_ms) noexcept;
    void reset() noexcept;
    void reset(const QSize &applied_extent) noexcept;

  private:
    std::optional<QSize> pending_extent_;
    std::optional<QSize> last_released_extent_;
    qint64 last_request_ms_ = 0;
    std::optional<qint64> next_interval_ms_;

    ViewportResizeDecision releasePendingIfDue(qint64 now_ms) noexcept;
    ViewportResizeDecision release(const QSize &extent, qint64 now_ms) noexcept;
};

} // namespace PelicanStudio
