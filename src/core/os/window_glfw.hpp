#pragma once

// Desktop only: the GLFW window behind Window's opaque native handle, for the
// code that talks to GLFW itself (window_glfw.cpp, windowsurface_glfw.cpp and
// the ImGui GLFW backend).

#include "window.hpp"

struct GLFWwindow;

namespace Pelican {

inline GLFWwindow *glfwWindowOf(NativeWindow *window) noexcept {
    return reinterpret_cast<GLFWwindow *>(window);
}

inline GLFWwindow *glfwWindowOf(const Window &window) noexcept {
    return glfwWindowOf(window.nativeHandle());
}

inline NativeWindow *nativeWindowOf(GLFWwindow *window) noexcept {
    return reinterpret_cast<NativeWindow *>(window);
}

} // namespace Pelican
