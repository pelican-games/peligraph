#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderWindow;

using SurfaceEpochId = std::uint64_t;

#if defined(__ANDROID__)
// Count a surface until *after* vkDestroySurfaceKHR, regardless of which
// thread destroys it. A quarantined surface remains alive until process exit.
class WindowSurfaceHandle {
    vk::UniqueSurfaceKHR surface_;
    std::uint64_t revision_ = 0;
    bool quarantined_ = false;
  public:
    WindowSurfaceHandle() = default;
    WindowSurfaceHandle(vk::UniqueSurfaceKHR surface, std::uint64_t revision);
    WindowSurfaceHandle(WindowSurfaceHandle &&other) noexcept;
    WindowSurfaceHandle &operator=(WindowSurfaceHandle &&other) noexcept;
    ~WindowSurfaceHandle();
    void reset() noexcept;
    void quarantine();
    vk::SurfaceKHR get() const noexcept { return surface_.get(); }
    explicit operator bool() const noexcept { return bool(surface_); }
};
#else
using WindowSurfaceHandle = vk::UniqueSurfaceKHR;
#endif
struct PreparedWindowSurface {
    WindowSurfaceHandle surface;
#if defined(__ANDROID__)
    std::uint64_t native_window_revision = 0;
#endif
};

// The platform (GLFW on the desktop, the Android activity) owns the native
// window, while the returned Vulkan surface is owned by
// the caller. The factory deliberately keeps no active-surface state so a
// surface-lost recovery always creates a fresh object.
class WindowSurfaceFactory {
    vk::Instance instance_;
    RenderWindow *window_ = nullptr;

  public:
    WindowSurfaceFactory(
        vk::Instance instance, RenderWindow &window) noexcept;

    PreparedWindowSurface create() const;
};

enum class SurfaceQueueBindingKind {
    compatible,
    device_rebuild_required,
};

enum class SurfaceDeviceRebuildReason {
    none,
    no_presentation_support,
    presentation_queue_not_created,
    presentation_completion_unavailable,
};

struct SurfaceQueueBinding {
    SurfaceQueueBindingKind kind =
        SurfaceQueueBindingKind::
            device_rebuild_required;
    std::uint32_t presentation_queue_family =
        VK_QUEUE_FAMILY_IGNORED;
    SurfaceDeviceRebuildReason rebuild_reason =
        SurfaceDeviceRebuildReason::
            no_presentation_support;
};

// Pure policy used by bootstrap-independent recovery tests. The current
// presentation family wins when possible; otherwise an already-created queue
// may be rebound. Device queues cannot be added after VkDevice creation.
SurfaceQueueBinding selectSurfacePresentationQueue(
    std::uint32_t current_presentation_family,
    std::span<const std::uint32_t>
        created_queue_families,
    std::span<const std::uint8_t>
        presentation_support) noexcept;

SurfaceQueueBinding querySurfacePresentationQueue(
    vk::PhysicalDevice physical_device,
    vk::SurfaceKHR surface,
    std::uint32_t current_presentation_family,
    std::span<const std::uint32_t>
        created_queue_families);

std::string_view surfaceDeviceRebuildReasonName(
    SurfaceDeviceRebuildReason reason) noexcept;

} // namespace Pelican
