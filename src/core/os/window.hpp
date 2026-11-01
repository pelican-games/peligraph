#pragma once


#include "framebuffersnapshot.hpp"
#include "inputstate.hpp"

#include <vulkan/vulkan.hpp>

#include "../container.hpp"
#include <mutex>
#include <vector>

namespace Pelican {

// The platform's window, never defined: a GLFWwindow on the desktop
// (window_glfw.cpp) and an ANativeWindow on Android (window_android.cpp).
struct NativeWindow;

DECLARE_MODULE(Window) {
    NativeWindow *window;
    std::vector<InputEvent> input_events;
    GamepadEventPoller gamepad_poller;
    mutable std::mutex framebuffer_snapshot_mutex;
    FramebufferExtentSnapshot framebuffer_snapshot;
    CursorMode applied_cursor_mode = CursorMode::normal;

  public:
    Window();
    ~Window();

    FramebufferExtentSnapshot framebufferSnapshot() const;
    vk::Extent2D framebufferExtent() const;
    vk::Extent2D logicalExtent() const;
    // GP-4: this window's live window-to-framebuffer conversion.
    PointerTransform pointerTransform() const;
    // Applies the cursor mode game code asked for (GLFW's input mode), the
    // lock ignored while the window is embedded in another (effectiveCursorMode).
    // Call on the main thread; it changes the GLFW mode only when the
    // effective mode changes. Returns what is in effect.
    CursorModeStatus applyCursorMode(CursorMode requested, bool rpc);
    NativeWindow *nativeHandle() const noexcept { return window; }
    void *renderDocCaptureHandle() const noexcept;
    std::vector<const char *> getRequiredVulkanInstanceExts();

    // false to close
    bool process();
    std::vector<InputEvent> drainInputEvents();
};

} // namespace Pelican
