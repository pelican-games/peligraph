#pragma once

#include <QSize>

namespace PelicanStudio {

// Qt widget sizes are device-independent. A foreign native child must instead
// be sized in the physical pixels used by its Vulkan framebuffer.
QSize embeddedViewportPixelExtent(const QSize &logical_extent, qreal device_pixel_ratio) noexcept;

} // namespace PelicanStudio
