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

} // namespace PelicanStudio
