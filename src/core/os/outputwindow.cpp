#include "outputwindow.hpp"

#include "../log.hpp"
#include "window_glfw.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <string>

namespace Pelican {
namespace {

vk::Extent2D readFramebuffer(GLFWwindow *window) {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    return {
        static_cast<std::uint32_t>(std::max(width, 1)),
        static_cast<std::uint32_t>(std::max(height, 1)),
    };
}

} // namespace

OutputWindow::OutputWindow(
    std::uint32_t monitor,
    std::optional<vk::Extent2D> size)
    : monitor_{monitor} {
    if (!glfwInit()) {
        throw OutputWindowMonitorUnavailable{
            "output_window.monitor_unavailable@1: GLFW is not initialized"};
    }
    int monitor_count = 0;
    GLFWmonitor **monitors = glfwGetMonitors(&monitor_count);
    if (monitors == nullptr || monitor >= static_cast<std::uint32_t>(monitor_count)) {
        throw OutputWindowMonitorUnavailable{
            "output_window.monitor_unavailable@1: monitor " +
            std::to_string(monitor) + " does not exist (GLFW reports " +
            std::to_string(monitor_count) + ")"};
    }
    GLFWmonitor *selected = monitors[monitor];
    const GLFWvidmode *mode = glfwGetVideoMode(selected);
    if (mode == nullptr) {
        throw OutputWindowMonitorUnavailable{
            "output_window.monitor_unavailable@1: monitor " +
            std::to_string(monitor) + " has no video mode"};
    }

    int monitor_x = 0;
    int monitor_y = 0;
    glfwGetMonitorPos(selected, &monitor_x, &monitor_y);
    int width = mode->width;
    int height = mode->height;
    int x = monitor_x;
    int y = monitor_y;
    const bool borderless = !size.has_value();
    if (size) {
        width = static_cast<int>(size->width);
        height = static_cast<int>(size->height);
        int work_x = monitor_x;
        int work_y = monitor_y;
        int work_width = mode->width;
        int work_height = mode->height;
        glfwGetMonitorWorkarea(selected, &work_x, &work_y, &work_width, &work_height);
        x = work_x + std::max((work_width - width) / 2, 0);
        y = work_y + std::max((work_height - height) / 2, 0);
    }

    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_DECORATED, borderless ? GLFW_FALSE : GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
    window_ = glfwCreateWindow(width, height, "Pelican output", nullptr, nullptr);
    if (window_ == nullptr) {
        throw std::runtime_error{
            "output_window.config_invalid@1: could not create the output window"};
    }
    glfwSetWindowPos(window_, x, y);
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, [](GLFWwindow *handle, int w, int h) {
        auto *self = static_cast<OutputWindow *>(glfwGetWindowUserPointer(handle));
        if (self == nullptr) return;
        const FramebufferExtentSnapshot next{
            .extent = {
                static_cast<std::uint32_t>(std::max(w, 1)),
                static_cast<std::uint32_t>(std::max(h, 1)),
            },
            .revision = self->snapshot_.revision + 1,
        };
        self->snapshot_ = next;
    });
    snapshot_ = {
        .extent = readFramebuffer(window_),
        .revision = 0,
    };
    glfwShowWindow(window_);
    LOG_INFO(logger,
             "diagnostic output window created on monitor {} ({}x{})",
             monitor, snapshot_.extent.width,
             snapshot_.extent.height);
}

OutputWindow::~OutputWindow() {
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
}

FramebufferExtentSnapshot OutputWindow::framebufferSnapshot() const {
    return snapshot_;
}

NativeWindow *OutputWindow::nativeHandle() const noexcept {
    return nativeWindowOf(window_);
}

vk::Extent2D OutputWindow::framebufferExtent() const {
    return snapshot_.extent;
}

bool OutputWindow::shouldClose() const {
    return window_ == nullptr || glfwWindowShouldClose(window_) != GLFW_FALSE;
}

void OutputWindow::requestClose() {
    if (window_ != nullptr) glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

void OutputWindow::process() {
    if (window_ == nullptr) return;
    const auto extent = readFramebuffer(window_);
    if (extent.width != snapshot_.extent.width ||
        extent.height != snapshot_.extent.height) {
        snapshot_ = {
            .extent = extent,
            .revision = snapshot_.revision + 1,
        };
    }
}

} // namespace Pelican
