#pragma once

// MD-1a0: the desktop second window. It is a plain GLFW window with no input
// callbacks and no render-window property; only the output sink's Vulkan
// swapchain draws into it. The engine's Window module stays the app's one
// input/render window.

#include "renderwindow.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vulkan/vulkan.hpp>

struct GLFWwindow;

namespace Pelican {

struct NativeWindow;

// The monitor index does not exist (or GLFW could not give its mode). The sink
// turns this into state "failed", code output_window.monitor_unavailable@1.
class OutputWindowMonitorUnavailable : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class OutputWindow : public RenderWindow {
  public:
    OutputWindow(std::uint32_t monitor,
                 std::optional<vk::Extent2D> size);
    ~OutputWindow() override;

    OutputWindow(const OutputWindow &) = delete;
    OutputWindow &operator=(const OutputWindow &) = delete;

    FramebufferExtentSnapshot
    framebufferSnapshot() const override;
    NativeWindow *nativeHandle() const noexcept override;

    std::uint32_t monitorIndex() const noexcept {
        return monitor_;
    }
    vk::Extent2D framebufferExtent() const;
    GLFWwindow *handle() const noexcept { return window_; }

    bool shouldClose() const;
    void requestClose();
    // Reads the framebuffer size again. The sink calls this once per frame
    // before it begins a frame on the target.
    void process();

  private:
    std::uint32_t monitor_ = 0;
    GLFWwindow *window_ = nullptr;
    FramebufferExtentSnapshot snapshot_{};
};

} // namespace Pelican
