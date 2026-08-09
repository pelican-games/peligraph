#include "nativewindowhost.hpp"

#ifdef Q_OS_WIN

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>

namespace PelicanStudio {
namespace {

HWND asHwnd(NativeWindowHandle handle) noexcept {
    return reinterpret_cast<HWND>(handle);
}

NativeWindowHandle asHandle(HWND window) noexcept {
    return reinterpret_cast<NativeWindowHandle>(window);
}

QString windowsError(const QString &operation, DWORD error_code = GetLastError()) {
    std::array<wchar_t, 512> buffer{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error_code, 0,
        buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    QString detail = length == 0 ? QStringLiteral("unknown Windows error")
                                 : QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(length)).trimmed();
    return QStringLiteral("%1 failed (%2): %3").arg(operation).arg(error_code).arg(detail);
}

void setError(QString *error, const QString &message) {
    if (error != nullptr) {
        *error = message;
    }
}

bool setWindowStyle(HWND window, int index, LONG_PTR style, const QString &operation, QString *error) {
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(window, index, style);
    if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
        setError(error, windowsError(operation));
        return false;
    }
    return true;
}

struct WindowSearch {
    DWORD process_id = 0;
    HWND visible_window = nullptr;
};

BOOL CALLBACK findProcessWindow(HWND window, LPARAM parameter) {
    auto &search = *reinterpret_cast<WindowSearch *>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != search.process_id || GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }

    // GLFW creates an invisible helper HWND before the render window. Wait for
    // the visible top-level window instead of accidentally embedding that helper.
    if (IsWindowVisible(window)) {
        search.visible_window = window;
        return FALSE;
    }
    return TRUE;
}

QSize clientExtent(HWND window) noexcept {
    RECT client{};
    if (!GetClientRect(window, &client)) {
        return {};
    }
    return {std::max(client.right - client.left, 0L), std::max(client.bottom - client.top, 0L)};
}

int windowDpiAwareness(HWND window) noexcept {
    const DPI_AWARENESS_CONTEXT context = GetWindowDpiAwarenessContext(window);
    if (context == nullptr) {
        return -1;
    }
    return static_cast<int>(GetAwarenessFromDpiAwarenessContext(context));
}

} // namespace

bool NativeWindowHost::isSupported() noexcept {
    return true;
}

NativeWindowHandle NativeWindowHost::findTopLevelWindow(qint64 process_id) noexcept {
    if (process_id <= 0 || static_cast<quint64>(process_id) > MAXDWORD) {
        return 0;
    }

    WindowSearch search{static_cast<DWORD>(process_id)};
    EnumWindows(findProcessWindow, reinterpret_cast<LPARAM>(&search));
    return asHandle(search.visible_window);
}

bool NativeWindowHost::embed(NativeWindowHandle child_handle, NativeWindowHandle parent_handle,
                             const QSize &pixel_extent, QString *error) {
    HWND child = asHwnd(child_handle);
    HWND parent = asHwnd(parent_handle);
    if (!IsWindow(child) || !IsWindow(parent)) {
        setError(error, QStringLiteral("The viewport child or Qt host HWND is no longer valid."));
        return false;
    }

    const int child_awareness = windowDpiAwareness(child);
    const int parent_awareness = windowDpiAwareness(parent);
    if (child_awareness < 0 || parent_awareness < 0 || child_awareness != parent_awareness) {
        setError(error,
                 QStringLiteral("The player and Qt host must use the same DPI awareness "
                                "(player %1, host %2).")
                     .arg(child_awareness)
                     .arg(parent_awareness));
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR current_style = GetWindowLongPtrW(child, GWL_STYLE);
    if (current_style == 0 && GetLastError() != ERROR_SUCCESS) {
        setError(error, windowsError(QStringLiteral("GetWindowLongPtrW(GWL_STYLE)")));
        return false;
    }
    const LONG_PTR child_style =
        (current_style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                           WS_MAXIMIZEBOX | WS_SYSMENU)) |
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    if (!setWindowStyle(child, GWL_STYLE, child_style,
                        QStringLiteral("SetWindowLongPtrW(GWL_STYLE)"), error)) {
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR current_extended_style = GetWindowLongPtrW(child, GWL_EXSTYLE);
    if (current_extended_style == 0 && GetLastError() != ERROR_SUCCESS) {
        setError(error, windowsError(QStringLiteral("GetWindowLongPtrW(GWL_EXSTYLE)")));
        return false;
    }
    const LONG_PTR child_extended_style =
        current_extended_style & ~(WS_EX_APPWINDOW | WS_EX_TOPMOST | WS_EX_WINDOWEDGE |
                                   WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);
    if (!setWindowStyle(child, GWL_EXSTYLE, child_extended_style,
                        QStringLiteral("SetWindowLongPtrW(GWL_EXSTYLE)"), error)) {
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    const HWND previous_parent = SetParent(child, parent);
    if (previous_parent == nullptr && GetLastError() != ERROR_SUCCESS) {
        setError(error, windowsError(QStringLiteral("SetParent")));
        return false;
    }

    // The HWND is unchanged, so the player's Vulkan surface remains the one
    // created during standalone startup. Resizing below only feeds GLFW's
    // normal framebuffer callback and therefore the WP215-WP217 epoch path.
    SendNotifyMessageW(child, WM_CHANGEUISTATE, MAKEWPARAM(UIS_INITIALIZE, 0), 0);
    return resize(child_handle, pixel_extent, error);
}

bool NativeWindowHost::resize(NativeWindowHandle child_handle, const QSize &pixel_extent,
                              QString *error) {
    HWND child = asHwnd(child_handle);
    if (!IsWindow(child)) {
        setError(error, QStringLiteral("The embedded engine HWND is no longer valid."));
        return false;
    }

    const int width = std::max(pixel_extent.width(), 0);
    const int height = std::max(pixel_extent.height(), 0);
    const UINT flags = SWP_ASYNCWINDOWPOS | SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW;
    if (!SetWindowPos(child, HWND_TOP, 0, 0, width, height, flags)) {
        setError(error, windowsError(QStringLiteral("SetWindowPos")));
        return false;
    }
    return true;
}

bool NativeWindowHost::focus(NativeWindowHandle child_handle) noexcept {
    HWND child = asHwnd(child_handle);
    if (!IsWindow(child)) {
        return false;
    }

    const DWORD current_thread = GetCurrentThreadId();
    const DWORD child_thread = GetWindowThreadProcessId(child, nullptr);
    const bool attach_required = current_thread != child_thread;
    const bool attached = !attach_required || AttachThreadInput(current_thread, child_thread, TRUE);
    if (!attached) {
        return false;
    }
    SetFocus(child);
    const bool focused = GetFocus() == child || IsChild(child, GetFocus());
    if (attach_required) {
        AttachThreadInput(current_thread, child_thread, FALSE);
    }
    return focused;
}

bool NativeWindowHost::pointerButtonDownOver(NativeWindowHandle child_handle) noexcept {
    HWND child = asHwnd(child_handle);
    if (!IsWindow(child)) {
        return false;
    }
    GUITHREADINFO thread_info{sizeof(GUITHREADINFO)};
    const DWORD child_thread = GetWindowThreadProcessId(child, nullptr);
    if (GetGUIThreadInfo(child_thread, &thread_info) &&
        (thread_info.hwndCapture == child || IsChild(child, thread_info.hwndCapture))) {
        return true;
    }
    const bool button_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                             (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0 ||
                             (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
                             (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0 ||
                             (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
    if (!button_down) {
        return false;
    }
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        return false;
    }
    const HWND pointed_window = WindowFromPoint(cursor);
    return pointed_window == child || IsChild(child, pointed_window);
}

NativePrimaryPointerState
NativeWindowHost::primaryPointerState(NativeWindowHandle child_handle) noexcept {
    NativePrimaryPointerState result;
    result.button_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    HWND child = asHwnd(child_handle);
    POINT cursor{};
    if (!IsWindow(child) || !GetCursorPos(&cursor)) {
        return result;
    }
    const HWND pointed_window = WindowFromPoint(cursor);
    result.over_child =
        pointed_window == child || IsChild(child, pointed_window);
    GUITHREADINFO thread_info{sizeof(GUITHREADINFO)};
    const DWORD child_thread = GetWindowThreadProcessId(child, nullptr);
    if (GetGUIThreadInfo(child_thread, &thread_info) &&
        (thread_info.hwndCapture == child ||
         IsChild(child, thread_info.hwndCapture))) {
        result.over_child = true;
    }
    if (!ScreenToClient(child, &cursor)) {
        return result;
    }
    result.child_client_position =
        QPoint{static_cast<int>(cursor.x), static_cast<int>(cursor.y)};
    return result;
}

bool NativeWindowHost::requestClose(NativeWindowHandle child_handle) noexcept {
    HWND child = asHwnd(child_handle);
    return IsWindow(child) && PostMessageW(child, WM_CLOSE, 0, 0);
}

bool NativeWindowHost::isWindow(NativeWindowHandle window) noexcept {
    return IsWindow(asHwnd(window));
}

NativeViewportDiagnostics NativeWindowHost::diagnostics(
    NativeWindowHandle child_handle, NativeWindowHandle parent_handle) noexcept {
    NativeViewportDiagnostics result;
    HWND child = asHwnd(child_handle);
    HWND parent = asHwnd(parent_handle);
    if (!IsWindow(child) || !IsWindow(parent)) {
        return result;
    }

    result.embedded = GetParent(child) == parent;
    result.child_style = (GetWindowLongPtrW(child, GWL_STYLE) & WS_CHILD) != 0;
    result.child_is_top_sibling = GetWindow(child, GW_HWNDPREV) == nullptr;
    result.host_client_extent = clientExtent(parent);
    result.child_client_extent = clientExtent(child);
    result.host_dpi = GetDpiForWindow(parent);
    result.child_dpi = GetDpiForWindow(child);
    result.host_dpi_awareness = windowDpiAwareness(parent);
    result.child_dpi_awareness = windowDpiAwareness(child);

    GUITHREADINFO thread_info{sizeof(GUITHREADINFO)};
    const DWORD child_thread = GetWindowThreadProcessId(child, nullptr);
    if (GetGUIThreadInfo(child_thread, &thread_info)) {
        result.child_has_focus =
            thread_info.hwndFocus == child || IsChild(child, thread_info.hwndFocus);
    }
    return result;
}

} // namespace PelicanStudio

#else

namespace PelicanStudio {
namespace {

void unsupported(QString *error) {
    if (error != nullptr) {
        *error = QStringLiteral("Native pelican_player window embedding is currently supported only on Windows.");
    }
}

} // namespace

bool NativeWindowHost::isSupported() noexcept { return false; }
NativeWindowHandle NativeWindowHost::findTopLevelWindow(qint64) noexcept { return 0; }
bool NativeWindowHost::embed(NativeWindowHandle, NativeWindowHandle, const QSize &, QString *error) {
    unsupported(error);
    return false;
}
bool NativeWindowHost::resize(NativeWindowHandle, const QSize &, QString *error) {
    unsupported(error);
    return false;
}
bool NativeWindowHost::focus(NativeWindowHandle) noexcept { return false; }
bool NativeWindowHost::pointerButtonDownOver(NativeWindowHandle) noexcept { return false; }
NativePrimaryPointerState
NativeWindowHost::primaryPointerState(NativeWindowHandle) noexcept {
    return {};
}
bool NativeWindowHost::requestClose(NativeWindowHandle) noexcept { return false; }
bool NativeWindowHost::isWindow(NativeWindowHandle) noexcept { return false; }
NativeViewportDiagnostics NativeWindowHost::diagnostics(NativeWindowHandle, NativeWindowHandle) noexcept {
    return {};
}

} // namespace PelicanStudio

#endif
