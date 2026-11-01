// Android: VK_KHR_android_surface over the activity's ANativeWindow.
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif

#include "windowsurface.hpp"

#include "../log.hpp"
#include "../os/window.hpp"
#include "../../platform/android/runtime.hpp"

#include <android/native_window.h>

#include <stdexcept>
#include <utility>

namespace Pelican {
WindowSurfaceHandle::WindowSurfaceHandle(vk::UniqueSurfaceKHR surface, std::uint64_t revision)
    : surface_{std::move(surface)}, revision_{revision} {}
WindowSurfaceHandle::WindowSurfaceHandle(WindowSurfaceHandle &&other) noexcept
    : surface_{std::move(other.surface_)}, revision_{std::exchange(other.revision_, 0)},
      quarantined_{std::exchange(other.quarantined_, false)} {}
WindowSurfaceHandle &WindowSurfaceHandle::operator=(WindowSurfaceHandle &&other) noexcept {
    if (this != &other) {
        reset();
        surface_ = std::move(other.surface_);
        revision_ = std::exchange(other.revision_, 0);
        quarantined_ = std::exchange(other.quarantined_, false);
    }
    return *this;
}
WindowSurfaceHandle::~WindowSurfaceHandle() { reset(); }
void WindowSurfaceHandle::reset() noexcept {
    if (!surface_) return;
    surface_.reset(); // the only decrement is after the actual Vulkan deleter
    auto &slot = Android::nativeWindowSlot();
    std::scoped_lock lock{slot.mutex};
    --slot.live[revision_];
    if (quarantined_) --slot.quarantined[revision_];
    Android::acknowledgeSurfaceReleaseLocked(slot);
    slot.changed.notify_all();
    revision_ = 0;
}
void WindowSurfaceHandle::quarantine() {
    if (!surface_ || quarantined_) return;
    auto &slot = Android::nativeWindowSlot();
    std::scoped_lock lock{slot.mutex};
    ++slot.quarantined[revision_];
    quarantined_ = true;
    Android::acknowledgeSurfaceReleaseLocked(slot);
}

PreparedWindowSurface
WindowSurfaceFactory::create() const {
    if (!instance_ || window_ == nullptr) {
        throw std::logic_error(
            "window surface factory is not initialized");
    }
    auto &slot = Android::nativeWindowSlot();
    std::unique_lock native_lock{slot.mutex};
    if (!slot.valid || slot.release_requested.load() || Android::closeRequested()) {
        return {}; // revoked native revision; never call the native API
    }
    for (const auto &[revision, count] : slot.live) {
        if (revision != slot.revision && count != 0)
            throw vk::SystemError{vk::make_error_code(vk::Result::eErrorNativeWindowInUseKHR),
                "old native revision still owns a surface"};
    }

    // Instance-level extension entry points are read through
    // vkGetInstanceProcAddr like the other extensions (core.cpp).
    const auto create_surface =
        reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
            vkGetInstanceProcAddr(
                static_cast<VkInstance>(instance_),
                "vkCreateAndroidSurfaceKHR"));
    if (create_surface == nullptr) {
        throw std::runtime_error(
            "vkCreateAndroidSurfaceKHR is unavailable "
            "(VK_KHR_android_surface not enabled)");
    }

    VkAndroidSurfaceCreateInfoKHR create_info{};
    create_info.sType =
        VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    create_info.window = slot.window;
    const auto revision = slot.revision;
    ANativeWindow_acquire(create_info.window);
    ++slot.live[revision]; // reserve before vkCreateAndroidSurfaceKHR
    native_lock.unlock(); // TERM_WINDOW must be able to start its bounded wait

    VkSurfaceKHR raw_surface = VK_NULL_HANDLE;
    const auto result = static_cast<vk::Result>(create_surface(
        static_cast<VkInstance>(instance_), &create_info,
        nullptr, &raw_surface));
    ANativeWindow_release(create_info.window);
    if (result != vk::Result::eSuccess) {
        native_lock.lock();
        --slot.live[revision];
        Android::acknowledgeSurfaceReleaseLocked(slot);
        slot.changed.notify_all();
        throw vk::SystemError{
            vk::make_error_code(result),
            "vkCreateAndroidSurfaceKHR"};
    }

    LOG_INFO(logger, "fresh Vulkan window surface created: native_revision={}", revision);
    return PreparedWindowSurface{
        .surface = WindowSurfaceHandle{vk::UniqueSurfaceKHR{
            vk::SurfaceKHR{raw_surface}, instance_}, revision},
        .native_window_revision = revision,
    };
}

} // namespace Pelican
