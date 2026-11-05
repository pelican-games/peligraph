#include "windowsurface.hpp"

#include "../log.hpp"
#include "../os/renderwindow.hpp"
#include "../os/window_glfw.hpp"

#include <stdexcept>

// After vulkan.hpp, so that GLFW declares glfwCreateWindowSurface.
#include <GLFW/glfw3.h>

namespace Pelican {

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
            glfwWindowOf(*window_), nullptr,
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

} // namespace Pelican
