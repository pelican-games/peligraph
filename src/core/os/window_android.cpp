// Android: the Window module over the activity's ANativeWindow.
#include "window.hpp"
#include "../launchconfig.hpp"
#include "../log.hpp"
#include "../../platform/android/runtime.hpp"

#include <android/native_window.h>

#include <algorithm>
#include <stdexcept>

namespace Pelican {

namespace Android {
NativeWindowSlot &nativeWindowSlot() {
    static NativeWindowSlot slot;
    return slot;
}
void acknowledgeSurfaceReleaseLocked(NativeWindowSlot &slot) {
    const auto revision = slot.release_revision;
    if (!slot.release_requested.load() || slot.acked_revision >= revision ||
        slot.live[revision] != slot.quarantined[revision]) return;
    slot.acked_revision = revision;
    // The acknowledgement is established before releasing the slot's lease.
    if (slot.window) ANativeWindow_release(slot.window);
    slot.window = nullptr;
    slot.changed.notify_all();
}
bool nativeRevisionCurrent(std::uint64_t revision) {
    auto &slot = nativeWindowSlot();
    std::scoped_lock lock{slot.mutex};
    return slot.valid && !slot.release_requested.load() && slot.revision == revision;
}
bool waitForNativeWindow() {
    return waitForNativeWindow(std::chrono::steady_clock::now() + std::chrono::seconds(3));
}
bool waitForNativeWindow(std::chrono::steady_clock::time_point deadline) {
    auto &slot = nativeWindowSlot();
    std::unique_lock lock{slot.mutex};
    while ((!slot.valid || slot.release_requested.load()) && !closeRequested()) {
        if (slot.changed.wait_until(lock, deadline) == std::cv_status::timeout) break;
    }
    return slot.valid && !slot.release_requested.load() && !closeRequested() &&
        std::chrono::steady_clock::now() < deadline;
}
} // namespace Android

Window::Window() : window{nullptr} {
    if (GET_MODULE(EngineLaunchConfig).headless) {
        throw std::runtime_error("Window module is unavailable in headless mode");
    }
    framebuffer_snapshot = {vk::Extent2D{1280, 720}, 1};
    (void)Android::waitForNativeWindow();
    (void)process();
    LOG_INFO(logger, "Android Window: extent={}x{}; ALooper is pumped by android_main",
             framebuffer_snapshot.extent.width, framebuffer_snapshot.extent.height);
}

Window::~Window() = default;

FramebufferExtentSnapshot
Window::framebufferSnapshot() const {
    std::scoped_lock lock{
        framebuffer_snapshot_mutex};
    return framebuffer_snapshot;
}

vk::Extent2D Window::framebufferExtent() const {
    return framebufferSnapshot().extent;
}

vk::Extent2D Window::logicalExtent() const {
    return framebufferExtent();
}

PointerTransform Window::pointerTransform() const {
    const auto framebuffer = framebufferExtent();
    const auto logical = logicalExtent();
    return pointerTransformFor(framebuffer.width, framebuffer.height, logical.width, logical.height);
}

CursorModeStatus Window::applyCursorMode(CursorMode requested, bool rpc) {
    (void)rpc;
    // There is no cursor to hide or lock on a touch screen.
    const auto effective = effectiveCursorMode(requested, false);
    applied_cursor_mode = effective;
    return CursorModeStatus{.requested = requested, .effective = effective, .embedded = false};
}

void *Window::renderDocCaptureHandle() const noexcept {
    return nullptr;
}

bool Window::process() {
    // The glue looper belongs exclusively to android_main. Never poll it on
    // this engine thread: project loading / shader compilation must not stop
    // lifecycle acknowledgements. Input is deferred to SA1f.
    Android::processLifecycle();
    if (Android::closeRequested()) return false;
    auto &slot = Android::nativeWindowSlot();
    std::scoped_lock native_lock{slot.mutex};
    // No native-window API after TERM_WINDOW, and no synthetic zero extent.
    if (!slot.valid || slot.release_requested.load()) return true;
    const vk::Extent2D extent{
        static_cast<std::uint32_t>(std::max(ANativeWindow_getWidth(slot.window), 0)),
        static_cast<std::uint32_t>(std::max(ANativeWindow_getHeight(slot.window), 0))};
    std::scoped_lock lock{framebuffer_snapshot_mutex};
    if (extent != framebuffer_snapshot.extent) {
        framebuffer_snapshot.extent = extent;
        ++framebuffer_snapshot.revision;
    }
    return true;
}

std::vector<InputEvent> Window::drainInputEvents() {
    std::vector<InputEvent> events;
    events.swap(input_events);
    return events;
}

std::vector<const char *> Window::getRequiredVulkanInstanceExts() {
    return {"VK_KHR_surface", "VK_KHR_android_surface"};
}

} // namespace Pelican
