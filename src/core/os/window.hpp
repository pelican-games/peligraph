#pragma once


#include "inputstate.hpp"

#include <vulkan/vulkan.hpp>
// include order must NOT be swap
#include <GLFW/glfw3.h>

#include "../container.hpp"
#include <vector>

namespace Pelican {

DECLARE_MODULE(Window) {
    GLFWwindow *window;
    std::vector<InputEvent> input_events;

  public:
    Window();
    ~Window();

    vk::Extent2D waitFramebufferExtent() const;
    vk::Extent2D framebufferExtent() const;
    vk::Extent2D logicalExtent() const;
    GLFWwindow *nativeHandle() const noexcept { return window; }
    vk::UniqueSurfaceKHR getVulkanSurface(vk::Instance instance);
    std::vector<const char *> getRequiredVulkanInstanceExts();

    // false to close
    bool process();
    std::vector<InputEvent> drainInputEvents();
};

} // namespace Pelican
