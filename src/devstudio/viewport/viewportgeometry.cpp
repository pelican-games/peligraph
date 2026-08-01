#include "viewportgeometry.hpp"

#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace PelicanStudio {
namespace {

int scaleDimension(int logical, qreal device_pixel_ratio) noexcept {
    if (logical <= 0 || !std::isfinite(device_pixel_ratio) || device_pixel_ratio <= 0.0) {
        return 0;
    }

    const qreal scaled = static_cast<qreal>(logical) * device_pixel_ratio;
    const qreal maximum = static_cast<qreal>(std::numeric_limits<int>::max());
    return static_cast<int>(qRound64(std::min(scaled, maximum)));
}

} // namespace

QSize embeddedViewportPixelExtent(const QSize &logical_extent, qreal device_pixel_ratio) noexcept {
    return {
        scaleDimension(logical_extent.width(), device_pixel_ratio),
        scaleDimension(logical_extent.height(), device_pixel_ratio),
    };
}

ViewportResizeCoalescer::ViewportResizeCoalescer(int minimum_interval_ms) noexcept
    : minimum_interval_ms_(std::max(minimum_interval_ms, 1)) {}

ViewportResizeDecision ViewportResizeCoalescer::request(const QSize &extent, qint64 now_ms,
                                                        ViewportExtentChangeKind kind) noexcept {
    if (kind == ViewportExtentChangeKind::device_pixel_ratio) {
        // DPR transitions are rare and stale physical-pixel dimensions visibly
        // break the embedded output, so they deliberately bypass coalescing.
        pending_extent_.reset();
        if (last_released_extent_ == extent) {
            return {};
        }
        return release(extent, now_ms);
    }

    if (last_released_extent_ == extent) {
        pending_extent_.reset();
        return {};
    }

    pending_extent_ = extent;
    return releasePendingIfDue(now_ms);
}

ViewportResizeDecision ViewportResizeCoalescer::timerExpired(qint64 now_ms) noexcept {
    return releasePendingIfDue(now_ms);
}

void ViewportResizeCoalescer::reset() noexcept {
    pending_extent_.reset();
    last_released_extent_.reset();
    last_release_ms_ = 0;
}

void ViewportResizeCoalescer::reset(const QSize &applied_extent, qint64 now_ms) noexcept {
    pending_extent_.reset();
    last_released_extent_ = applied_extent;
    last_release_ms_ = now_ms;
}

ViewportResizeDecision ViewportResizeCoalescer::releasePendingIfDue(qint64 now_ms) noexcept {
    if (!pending_extent_) {
        return {};
    }

    const qint64 elapsed_ms =
        now_ms >= last_release_ms_ ? now_ms - last_release_ms_ : 0;
    if (last_released_extent_ && elapsed_ms < minimum_interval_ms_) {
        return {
            .next_wakeup_ms = minimum_interval_ms_ - static_cast<int>(elapsed_ms),
        };
    }

    const QSize extent = *pending_extent_;
    pending_extent_.reset();
    return release(extent, now_ms);
}

ViewportResizeDecision ViewportResizeCoalescer::release(const QSize &extent,
                                                        qint64 now_ms) noexcept {
    last_released_extent_ = extent;
    last_release_ms_ = now_ms;
    return {.extent_to_apply = extent};
}

} // namespace PelicanStudio
