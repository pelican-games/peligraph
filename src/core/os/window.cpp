#include "window.hpp"
#include "../launchconfig.hpp"
#include "../loader/basicconfig.hpp"
#include "../log.hpp"
#include <algorithm>
#include <stdexcept>

namespace Pelican {

namespace {

struct WindowCreateInfo {
    int width = 0;
    int height = 0;
    GLFWmonitor *monitor = nullptr;
};

WindowCreateInfo makeWindowCreateInfo(ProjectBasicConfig::window_size window_size, bool fullscreen) {
    if (!fullscreen) {
        if (window_size.width <= 0 || window_size.height <= 0) {
            throw std::runtime_error("Window size must be positive");
        }
        return WindowCreateInfo{window_size.width, window_size.height, nullptr};
    }

    auto *monitor = glfwGetPrimaryMonitor();
    if (monitor == nullptr) {
        throw std::runtime_error("Fullscreen mode requires a primary monitor");
    }

    const auto *mode = glfwGetVideoMode(monitor);
    if (mode == nullptr) {
        throw std::runtime_error("Failed to get primary monitor video mode");
    }

    glfwWindowHint(GLFW_RED_BITS, mode->redBits);
    glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
    glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
    glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);

    return WindowCreateInfo{mode->width, mode->height, monitor};
}

bool isKeyboardKeyTracked(int key) {
    return key >= 0 && key <= GLFW_KEY_LAST;
}

bool isMouseButtonTracked(int button) {
    return button >= 0 && button <= GLFW_MOUSE_BUTTON_LAST;
}

} // namespace

Window::Window() : window{nullptr}, gamepad_axis{}, cursor_x{0.0f}, cursor_y{0.0f} {
    if (GET_MODULE(EngineLaunchConfig).headless) {
        throw std::runtime_error("Window module is unavailable in headless mode");
    }

    LOG_INFO(logger, "GLFW initializing...");

    const auto &config = GET_MODULE(ProjectBasicConfig);

    const auto window_size = config.initialWindowSize();
    const auto title = config.windowTitle();
    const auto fullscreen = config.initialFullScreenState();

    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("glfwInit failed");
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    try {
        const auto create_info = makeWindowCreateInfo(window_size, fullscreen);
        window = glfwCreateWindow(create_info.width, create_info.height, title.c_str(), create_info.monitor, nullptr);
        if (window == nullptr) {
            throw std::runtime_error("glfwCreateWindow failed");
        }
    } catch (...) {
        glfwTerminate();
        throw;
    }
    glfwSetWindowUserPointer(window, this);

    LOG_INFO(logger, "GLFW window initialized");

    glfwSetKeyCallback(window, [](GLFWwindow *window, int key, int scancode, int action, int mods) {
        if (action == GLFW_REPEAT || !isKeyboardKeyTracked(key)) {
            return;
        }
        const auto thiz = static_cast<Window *>(glfwGetWindowUserPointer(window));

        // action: GLFW_PRESS == 1, GLFW_RELEASE == 0
        thiz->key_state.pressing.set(button_id_offset_keyboard + key, action == GLFW_PRESS);
    });
    glfwSetMouseButtonCallback(window, [](GLFWwindow *window, int button, int action, int mods) {
        if (!isMouseButtonTracked(button)) {
            return;
        }
        const auto thiz = static_cast<Window *>(glfwGetWindowUserPointer(window));

        // action: GLFW_PRESS == 1, GLFW_RELEASE == 0
        thiz->key_state.pressing.set(button_id_offset_mouse_button + button, action == GLFW_PRESS);
    });
    glfwSetCursorPosCallback(window, [](GLFWwindow *window, double xpos, double ypos) {
        const auto thiz = static_cast<Window *>(glfwGetWindowUserPointer(window));

        thiz->cursor_x = static_cast<float>(xpos);
        thiz->cursor_y = static_cast<float>(ypos);
    });
}

Window::~Window() {
    if (window != nullptr) {
        glfwDestroyWindow(window);
        window = nullptr;
    }
    glfwTerminate();
}

vk::Extent2D Window::waitFramebufferExtent() const {
    int width = 0;
    int height = 0;
    while (true) {
        glfwGetFramebufferSize(window, &width, &height);
        if (width > 0 && height > 0) {
            return vk::Extent2D{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        }
        if (glfwWindowShouldClose(window)) {
            throw std::runtime_error("Window closed while waiting for non-zero framebuffer size");
        }
        glfwWaitEvents();
    }
}

bool Window::process() {
    key_state.pressing_old = key_state.pressing;
    glfwPollEvents();

    {
        GLFWgamepadstate state{};
        const bool gamepad_connected = glfwGetGamepadState(GLFW_JOYSTICK_1, &state) == GLFW_TRUE;
        for (int i = 0; i <= GLFW_GAMEPAD_BUTTON_LAST; i++) {
            // GLFW_PRESS == 1, GLFW_RELEASE == 0
            key_state.pressing.set(button_id_offset_gamepad + i,
                                   gamepad_connected && state.buttons[i] == GLFW_PRESS);
        }
        for (int i = 0; i <= GLFW_GAMEPAD_AXIS_LAST; i++) {
            gamepad_axis[i] = gamepad_connected ? state.axes[i] : 0.0f;
        }
    }

    key_state.just_pressed = key_state.pressing & ~key_state.pressing_old;
    key_state.just_released = ~key_state.pressing & key_state.pressing_old;

    return !glfwWindowShouldClose(window);
}

vk::UniqueSurfaceKHR Window::getVulkanSurface(vk::Instance instance) {
    VkSurfaceKHR surface;
    if (auto result = glfwCreateWindowSurface(instance, window, nullptr, &surface); result != VK_SUCCESS) {
        throw std::runtime_error("glfwCreateWindowSurface failed: " + vk::to_string(vk::Result{result}));
    }

    LOG_INFO(logger, "vulkan surface created");

    return vk::UniqueSurfaceKHR{surface, instance};
}

std::vector<const char *> Window::getRequiredVulkanInstanceExts() {
    uint32_t count = 0;
    const char **ext_names_raw = glfwGetRequiredInstanceExtensions(&count);
    if (ext_names_raw == nullptr) {
        throw std::runtime_error("glfwGetRequiredInstanceExtensions failed");
    }

    std::vector<const char *> ext_names;
    std::copy(ext_names_raw, ext_names_raw + count, std::back_inserter(ext_names));

    return ext_names;
}

} // namespace Pelican
