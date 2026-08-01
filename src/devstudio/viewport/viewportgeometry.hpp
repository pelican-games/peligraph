#pragma once

#include <QSize>

#include <optional>

namespace PelicanStudio {

// Qt widget sizes are device-independent. A foreign native child must instead
// be sized in the physical pixels used by its Vulkan framebuffer.
QSize embeddedViewportPixelExtent(const QSize &logical_extent, qreal device_pixel_ratio) noexcept;

// SetWindowPos wakes the player's framebuffer/swapchain path, so continuous
// panel resizing is limited to 20 updates per second.
inline constexpr int EmbeddedViewportResizeIntervalMs = 50;

enum class ViewportExtentChangeKind {
    resize,
    device_pixel_ratio,
};

struct ViewportResizeDecision {
    std::optional<QSize> extent_to_apply;
    std::optional<int> next_wakeup_ms;
};

// Deterministic coalescing policy. The caller supplies a monotonic timestamp
// and owns the timer and native resize side effects.
class ViewportResizeCoalescer {
  public:
    explicit ViewportResizeCoalescer(
        int minimum_interval_ms = EmbeddedViewportResizeIntervalMs) noexcept;

    ViewportResizeDecision request(const QSize &extent, qint64 now_ms,
                                   ViewportExtentChangeKind kind) noexcept;
    ViewportResizeDecision timerExpired(qint64 now_ms) noexcept;
    void reset() noexcept;
    void reset(const QSize &applied_extent, qint64 now_ms) noexcept;

  private:
    int minimum_interval_ms_;
    std::optional<QSize> pending_extent_;
    std::optional<QSize> last_released_extent_;
    qint64 last_release_ms_ = 0;

    ViewportResizeDecision releasePendingIfDue(qint64 now_ms) noexcept;
    ViewportResizeDecision release(const QSize &extent, qint64 now_ms) noexcept;
};

} // namespace PelicanStudio
