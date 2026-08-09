#pragma once

#include <QPoint>
#include <QSize>
#include <QString>
#include <QtTypes>

#include <optional>

namespace PelicanStudio {

using NativeWindowHandle = quintptr;

struct NativeViewportDiagnostics {
    bool embedded = false;
    bool child_style = false;
    bool child_has_focus = false;
    bool child_is_top_sibling = false;
    QSize host_client_extent;
    QSize child_client_extent;
    unsigned int host_dpi = 0;
    unsigned int child_dpi = 0;
    int host_dpi_awareness = -1;
    int child_dpi_awareness = -1;
};

struct NativePrimaryPointerState {
    bool button_down = false;
    bool over_child = false;
    std::optional<QPoint> child_client_position;
};

class NativeWindowHost {
  public:
    static bool isSupported() noexcept;
    static NativeWindowHandle findTopLevelWindow(qint64 process_id) noexcept;
    static bool embed(NativeWindowHandle child, NativeWindowHandle parent,
                      const QSize &pixel_extent, QString *error = nullptr);
    static bool resize(NativeWindowHandle child, const QSize &pixel_extent,
                       QString *error = nullptr);
    static bool focus(NativeWindowHandle child) noexcept;
    static bool pointerButtonDownOver(NativeWindowHandle child) noexcept;
    static NativePrimaryPointerState
    primaryPointerState(NativeWindowHandle child) noexcept;
    static bool requestClose(NativeWindowHandle child) noexcept;
    static bool isWindow(NativeWindowHandle window) noexcept;
    static NativeViewportDiagnostics diagnostics(NativeWindowHandle child,
                                                 NativeWindowHandle parent) noexcept;
};

} // namespace PelicanStudio
