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

ViewportResizeDecision ViewportResizeCoalescer::request(const QSize &extent, qint64 now_ms,
                                                        ViewportExtentChangeKind kind) noexcept {
    if (kind == ViewportExtentChangeKind::device_pixel_ratio ||
        kind == ViewportExtentChangeKind::interaction_finished) {
        // DPR transitions and the native end of a drag flush the latest size.
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

    if (!pending_extent_ && now_ms - last_request_ms_ >= EmbeddedViewportResizeQuietMs) {
        next_interval_ms_.reset();
    }
    if (!next_interval_ms_) {
        next_interval_ms_ = now_ms + EmbeddedViewportResizeIntervalMs;
    }
    if (pending_extent_ != extent) {
        pending_extent_ = extent;
        last_request_ms_ = now_ms;
    }
    return releasePendingIfDue(now_ms);
}

ViewportResizeDecision ViewportResizeCoalescer::timerExpired(qint64 now_ms) noexcept {
    return releasePendingIfDue(now_ms);
}

void ViewportResizeCoalescer::reset() noexcept {
    pending_extent_.reset();
    last_released_extent_.reset();
    next_interval_ms_.reset();
    last_request_ms_ = 0;
}

void ViewportResizeCoalescer::reset(const QSize &applied_extent) noexcept {
    reset();
    last_released_extent_ = applied_extent;
}

ViewportResizeDecision ViewportResizeCoalescer::releasePendingIfDue(qint64 now_ms) noexcept {
    if (!pending_extent_) {
        return {};
    }

    const qint64 deadline = std::min(last_request_ms_ + EmbeddedViewportResizeQuietMs,
                                     *next_interval_ms_);
    if (now_ms < deadline) {
        return {.next_wakeup_ms = static_cast<int>(deadline - now_ms)};
    }

    const QSize extent = *pending_extent_;
    pending_extent_.reset();
    return release(extent, now_ms);
}

ViewportResizeDecision ViewportResizeCoalescer::release(const QSize &extent, qint64 now_ms) noexcept {
    last_released_extent_ = extent;
    next_interval_ms_ = now_ms + EmbeddedViewportResizeIntervalMs;
    return {.extent_to_apply = extent};
}

} // namespace PelicanStudio
