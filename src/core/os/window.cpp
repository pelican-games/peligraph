#include "window.hpp"
#include "../loader/basicconfig.hpp"
#include "../log.hpp"

namespace Pelican {

Window::Window(DependencyContainer &container) {
    LOG_INFO(logger, "GLFW initializing...");

    const auto &config = container.get<ProjectBasicConfig>();

    const auto window_size = config.initialWindowSize();
    const auto title = config.windowTitle();
    const auto fullscreen = config.initialFullScreenState();

    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window = glfwCreateWindow(window_size.width, window_size.height, title.c_str(), nullptr, nullptr);
    glfwSetWindowUserPointer(window, this);

    LOG_INFO(logger, "GLFW window initialized");

    glfwSetKeyCallback(window, [](GLFWwindow *window, int key, int scancode, int action, int mods) {
        if (action == GLFW_REPEAT)
            return;
        const auto thiz = static_cast<Window *>(glfwGetWindowUserPointer(window));

        // action: GLFW_PRESS == 1, GLFW_RELEASE == 0
        thiz->key_state.pressing.set(button_id_offset_keyboard + key, action & 1);
    });
    glfwSetMouseButtonCallback(window, [](GLFWwindow *window, int button, int action, int mods) {
        const auto thiz = static_cast<Window *>(glfwGetWindowUserPointer(window));

        // action: GLFW_PRESS == 1, GLFW_RELEASE == 0
        thiz->key_state.pressing.set(button_id_offset_mouse_button + button, action & 1);
    });
    glfwSetCursorPosCallback(window, [](GLFWwindow *window, double xpos, double ypos) {
        const auto thiz = static_cast<Window *>(glfwGetWindowUserPointer(window));

        thiz->cursor_x = static_cast<float>(xpos);
        thiz->cursor_y = static_cast<float>(ypos);
    });
}

Window::~Window() {}

bool Window::process() {
    key_state.pressing_old = key_state.pressing;
    glfwPollEvents();

    {
        GLFWgamepadstate state;
        glfwGetGamepadState(GLFW_JOYSTICK_1, &state);
        for (int i = 0; i <= GLFW_GAMEPAD_BUTTON_LAST; i++) {
            // GLFW_PRESS == 1, GLFW_RELEASE == 0
            key_state.pressing.set(button_id_offset_gamepad + i, state.buttons[i] & 1);
        }
        for (int i = 0; i <= GLFW_GAMEPAD_AXIS_LAST; i++) {
            gamepad_axis[i] = state.axes[i];
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
    uint32_t count;
    const char **ext_names_raw = glfwGetRequiredInstanceExtensions(&count);

    std::vector<const char *> ext_names;
    std::copy(ext_names_raw, ext_names_raw + count, std::back_inserter(ext_names));

    return ext_names;
}

} // namespace Pelican
