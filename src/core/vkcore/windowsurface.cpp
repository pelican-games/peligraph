#include "windowsurface.hpp"

#include "../log.hpp"
#include "../os/window.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace Pelican {

WindowSurfaceFactory::WindowSurfaceFactory(
    vk::Instance instance, Window &window) noexcept
    : instance_{instance}, window_{&window} {}

PreparedWindowSurface
WindowSurfaceFactory::create() const {
    if (!instance_ || window_ == nullptr ||
        window_->nativeHandle() == nullptr) {
        throw std::logic_error(
            "window surface factory is not initialized");
    }

    VkSurfaceKHR raw_surface = VK_NULL_HANDLE;
    const auto result = static_cast<vk::Result>(
        glfwCreateWindowSurface(
            static_cast<VkInstance>(instance_),
            window_->nativeHandle(), nullptr,
            &raw_surface));
    if (result != vk::Result::eSuccess) {
        throw vk::SystemError{
            vk::make_error_code(result),
            "glfwCreateWindowSurface"};
    }

    LOG_INFO(logger, "fresh Vulkan window surface created");
    return PreparedWindowSurface{
        .surface = vk::UniqueSurfaceKHR{
            vk::SurfaceKHR{raw_surface}, instance_},
    };
}

SurfaceQueueBinding selectSurfacePresentationQueue(
    std::uint32_t current_presentation_family,
    std::span<const std::uint32_t>
        created_queue_families,
    std::span<const std::uint8_t>
        presentation_support) noexcept {
    const auto is_created =
        [&](std::uint32_t family) {
            return std::find(
                       created_queue_families.begin(),
                       created_queue_families.end(),
                       family) !=
                   created_queue_families.end();
        };
    const auto is_supported =
        [&](std::uint32_t family) {
            return family <
                       presentation_support.size() &&
                   presentation_support[family] != 0;
        };

    if (is_created(current_presentation_family) &&
        is_supported(current_presentation_family)) {
        return {
            .kind =
                SurfaceQueueBindingKind::compatible,
            .presentation_queue_family =
                current_presentation_family,
            .rebuild_reason =
                SurfaceDeviceRebuildReason::none,
        };
    }

    for (const auto family :
         created_queue_families) {
        if (is_supported(family)) {
            return {
                .kind =
                    SurfaceQueueBindingKind::
                        compatible,
                .presentation_queue_family = family,
                .rebuild_reason =
                    SurfaceDeviceRebuildReason::none,
            };
        }
    }

    const bool any_supported = std::ranges::any_of(
        presentation_support,
        [](std::uint8_t supported) {
            return supported != 0;
        });
    return {
        .kind =
            SurfaceQueueBindingKind::
                device_rebuild_required,
        .rebuild_reason =
            any_supported
                ? SurfaceDeviceRebuildReason::
                      presentation_queue_not_created
                : SurfaceDeviceRebuildReason::
                      no_presentation_support,
    };
}

SurfaceQueueBinding querySurfacePresentationQueue(
    vk::PhysicalDevice physical_device,
    vk::SurfaceKHR surface,
    std::uint32_t current_presentation_family,
    std::span<const std::uint32_t>
        created_queue_families) {
    const auto queue_properties =
        physical_device.getQueueFamilyProperties();
    std::vector<std::uint8_t> support(
        queue_properties.size(), 0);
    for (std::uint32_t family = 0;
         family < queue_properties.size(); ++family) {
        if (queue_properties[family].queueCount == 0) {
            continue;
        }
        support[family] =
            physical_device.getSurfaceSupportKHR(
                family, surface)
                ? 1
                : 0;
    }
    return selectSurfacePresentationQueue(
        current_presentation_family,
        created_queue_families, support);
}

std::string_view surfaceDeviceRebuildReasonName(
    SurfaceDeviceRebuildReason reason) noexcept {
    switch (reason) {
    case SurfaceDeviceRebuildReason::none:
        return "none";
    case SurfaceDeviceRebuildReason::
        no_presentation_support:
        return "no_presentation_support";
    case SurfaceDeviceRebuildReason::
        presentation_queue_not_created:
        return "presentation_queue_not_created";
    }
    return "unknown";
}

} // namespace Pelican
