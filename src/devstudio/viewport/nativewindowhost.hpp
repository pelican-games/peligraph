#pragma once

#include <QPoint>
#include <QSize>
#include <QString>
#include <QtTypes>

#include <optional>
#include <array>

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

// Studio shortcuts must also cross the separately hosted engine window.
inline constexpr std::array<int, 8> StudioShortcutKeys{
    Qt::Key_Z, Qt::Key_Y, Qt::Key_D, Qt::Key_Delete,
    Qt::Key_N, Qt::Key_O, Qt::Key_S, Qt::Key_P};

struct NativePrimaryPointerState {
    quint32 shortcut_keys_down = 0;
    Qt::KeyboardModifiers shortcut_modifiers = Qt::NoModifier;
    bool button_down = false;
    bool over_child = false;
    // The engine window (or a child of it) holds the keyboard focus, so a key
    // pressed now lands in the engine and not in the editor. Read from the
    // same GUITHREADINFO the hover test already asks for, so it costs no
    // extra call.
    bool has_focus = false;
    std::optional<QPoint> child_client_position;
};

class NativeWindowHost {
  public:
    static bool isSupported() noexcept;
    static NativeWindowHandle findTopLevelWindow(qint64 process_id) noexcept;
    // Q-4. Does the thread that owns this window take messages yet? Embedding
    // sends WM_STYLECHANGING to that thread and waits for it, so asking first
    // -- with a bounded wait, and giving up at once on a thread Windows has
    // already decided is not answering -- is what keeps Studio's UI thread out
    // of the engine's load.
    static bool windowThreadAnswers(NativeWindowHandle window,
                                    int timeout_ms) noexcept;
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
