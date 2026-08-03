#pragma once

#include <QSize>

#include <optional>

namespace PelicanStudio {

// Qt widget sizes are device-independent. A foreign native child must instead
// be sized in the physical pixels used by its Vulkan framebuffer.
QSize embeddedViewportPixelExtent(const QSize &logical_extent, qreal device_pixel_ratio) noexcept;

// SetWindowPos wakes the player's framebuffer/swapchain path. Keep the native
// child frozen until panel dimensions have stayed unchanged for this long.
inline constexpr int EmbeddedViewportResizeDebounceMs = 50;

enum class ViewportExtentChangeKind {
    resize,
    device_pixel_ratio,
};

struct ViewportResizeDecision {
    std::optional<QSize> extent_to_apply;
    std::optional<int> next_wakeup_ms;
};

// Deterministic trailing-edge debounce policy. The caller supplies a monotonic
// timestamp and owns the timer and native resize side effects.
class ViewportResizeCoalescer {
  public:
    explicit ViewportResizeCoalescer(
        int debounce_ms = EmbeddedViewportResizeDebounceMs) noexcept;

    ViewportResizeDecision request(const QSize &extent, qint64 now_ms,
                                   ViewportExtentChangeKind kind) noexcept;
    ViewportResizeDecision timerExpired(qint64 now_ms) noexcept;
    void reset() noexcept;
    void reset(const QSize &applied_extent) noexcept;

  private:
    int debounce_ms_;
    std::optional<QSize> pending_extent_;
    std::optional<QSize> last_released_extent_;
    qint64 last_request_ms_ = 0;

    ViewportResizeDecision releasePendingIfSettled(qint64 now_ms) noexcept;
    ViewportResizeDecision release(const QSize &extent) noexcept;
};

} // namespace PelicanStudio
