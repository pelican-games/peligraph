#include "window.hpp"
#include "../launchconfig.hpp"
#include "../loader/basicconfig.hpp"
#include "../log.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace Pelican {

namespace {

struct WindowCreateInfo {
    int width = 0;
    int height = 0;
    GLFWmonitor *monitor = nullptr;
};

class GlfwGamepadStateSource final : public GamepadStateSource {
  public:
    bool read(std::size_t slot, GamepadState &state) override {
        const int joystick = GLFW_JOYSTICK_1 + static_cast<int>(slot);
        if (glfwJoystickIsGamepad(joystick) != GLFW_TRUE) {
            return false;
        }
        GLFWgamepadstate glfw_state{};
        if (glfwGetGamepadState(joystick, &glfw_state) != GLFW_TRUE) {
            return false;
        }
        static_assert(GLFW_GAMEPAD_BUTTON_LAST + 1 == gamepad_button_count);
        static_assert(GLFW_GAMEPAD_AXIS_LAST + 1 == gamepad_axis_count);
        for (std::size_t i = 0; i < gamepad_button_count; ++i) {
            state.buttons[i] = static_cast<std::uint8_t>(glfw_state.buttons[i] == GLFW_PRESS ? 1 : 0);
        }
        for (std::size_t i = 0; i < gamepad_axis_count; ++i) {
            state.axes[i] = glfw_state.axes[i];
        }
        return true;
    }
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

std::optional<KeyCode> keyCodeFromGlfwKey(int key) {
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + (key - GLFW_KEY_A));
    }
    if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::Num0) + (key - GLFW_KEY_0));
    }

    switch (key) {
    case GLFW_KEY_UP:
        return KeyCode::ArrowUp;
    case GLFW_KEY_DOWN:
        return KeyCode::ArrowDown;
    case GLFW_KEY_LEFT:
        return KeyCode::ArrowLeft;
    case GLFW_KEY_RIGHT:
        return KeyCode::ArrowRight;
    case GLFW_KEY_SPACE:
        return KeyCode::Space;
    case GLFW_KEY_ENTER:
        return KeyCode::Enter;
    case GLFW_KEY_ESCAPE:
        return KeyCode::Escape;
    case GLFW_KEY_TAB:
        return KeyCode::Tab;
    case GLFW_KEY_BACKSPACE:
        return KeyCode::Backspace;
    case GLFW_KEY_LEFT_SHIFT:
        return KeyCode::LeftShift;
    case GLFW_KEY_RIGHT_SHIFT:
        return KeyCode::RightShift;
    case GLFW_KEY_LEFT_CONTROL:
        return KeyCode::LeftControl;
    case GLFW_KEY_RIGHT_CONTROL:
        return KeyCode::RightControl;
    case GLFW_KEY_LEFT_ALT:
        return KeyCode::LeftAlt;
    case GLFW_KEY_RIGHT_ALT:
        return KeyCode::RightAlt;
    case GLFW_KEY_LEFT_SUPER:
        return KeyCode::LeftSuper;
    case GLFW_KEY_RIGHT_SUPER:
        return KeyCode::RightSuper;
    case GLFW_KEY_F1:
        return KeyCode::F1;
    case GLFW_KEY_F2:
        return KeyCode::F2;
    case GLFW_KEY_F3:
        return KeyCode::F3;
    case GLFW_KEY_F4:
        return KeyCode::F4;
    case GLFW_KEY_F5:
        return KeyCode::F5;
    case GLFW_KEY_F6:
        return KeyCode::F6;
    case GLFW_KEY_F7:
        return KeyCode::F7;
    case GLFW_KEY_F8:
        return KeyCode::F8;
    case GLFW_KEY_F9:
        return KeyCode::F9;
    case GLFW_KEY_F10:
        return KeyCode::F10;
    case GLFW_KEY_F11:
        return KeyCode::F11;
    case GLFW_KEY_F12:
        return KeyCode::F12;
    default:
        return std::nullopt;
    }
}

std::optional<KeyCode> keyCodeFromGlfwMouseButton(int button) {
    switch (button) {
    case GLFW_MOUSE_BUTTON_LEFT:
        return KeyCode::MouseLeft;
    case GLFW_MOUSE_BUTTON_RIGHT:
        return KeyCode::MouseRight;
    case GLFW_MOUSE_BUTTON_MIDDLE:
        return KeyCode::MouseMiddle;
    case GLFW_MOUSE_BUTTON_4:
        return KeyCode::MouseButton4;
    case GLFW_MOUSE_BUTTON_5:
        return KeyCode::MouseButton5;
    case GLFW_MOUSE_BUTTON_6:
        return KeyCode::MouseButton6;
    case GLFW_MOUSE_BUTTON_7:
        return KeyCode::MouseButton7;
    case GLFW_MOUSE_BUTTON_8:
        return KeyCode::MouseButton8;
    default:
        return std::nullopt;
    }
}

} // namespace

Window::Window() : window{nullptr} {
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
        (void)scancode;
        (void)mods;
        if (action != GLFW_PRESS && action != GLFW_RELEASE) {
            return;
        }
        const auto code = keyCodeFromGlfwKey(key);
        if (!code) {
            return;
        }
        const auto thiz = static_cast<Window *>(glfwGetWindowUserPointer(window));

        thiz->input_events.push_back(InputEvent::button(*code, action == GLFW_PRESS));
    });
    glfwSetMouseButtonCallback(window, [](GLFWwindow *window, int button, int action, int mods) {
        (void)mods;
        if (action != GLFW_PRESS && action != GLFW_RELEASE) {
            return;
        }
        const auto code = keyCodeFromGlfwMouseButton(button);
        if (!code) {
            return;
        }
        const auto thiz = static_cast<Window *>(glfwGetWindowUserPointer(window));

        thiz->input_events.push_back(InputEvent::button(*code, action == GLFW_PRESS));
    });
    glfwSetCursorPosCallback(window, [](GLFWwindow *window, double xpos, double ypos) {
        const auto thiz = static_cast<Window *>(glfwGetWindowUserPointer(window));

        thiz->input_events.push_back(InputEvent::cursorMove(static_cast<float>(xpos), static_cast<float>(ypos)));
    });
    glfwSetScrollCallback(window, [](GLFWwindow *window, double xoffset, double yoffset) {
        const auto thiz = static_cast<Window *>(glfwGetWindowUserPointer(window));
        thiz->input_events.push_back(InputEvent::scroll(static_cast<float>(xoffset),
                                                       static_cast<float>(yoffset)));
    });
    glfwSetCharCallback(window, [](GLFWwindow *window, unsigned int codepoint) {
        const auto thiz = static_cast<Window *>(glfwGetWindowUserPointer(window));
        thiz->input_events.push_back(InputEvent::character(codepoint));
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

vk::Extent2D Window::framebufferExtent() const {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    return vk::Extent2D{static_cast<uint32_t>(std::max(width, 0)),
                        static_cast<uint32_t>(std::max(height, 0))};
}

vk::Extent2D Window::logicalExtent() const {
    int width = 0;
    int height = 0;
    glfwGetWindowSize(window, &width, &height);
    return vk::Extent2D{static_cast<uint32_t>(std::max(width, 0)),
                        static_cast<uint32_t>(std::max(height, 0))};
}

void *Window::renderDocCaptureHandle() const noexcept {
#ifdef _WIN32
    return reinterpret_cast<void *>(glfwGetWin32Window(window));
#else
    return nullptr;
#endif
}

bool Window::process() {
    glfwPollEvents();
    GlfwGamepadStateSource source;
    auto gamepad_events = gamepad_poller.poll(source, internal::gamepadPollingEnabled());
    input_events.insert(input_events.end(), gamepad_events.begin(), gamepad_events.end());

    return !glfwWindowShouldClose(window);
}

std::vector<InputEvent> Window::drainInputEvents() {
    std::vector<InputEvent> events;
    events.swap(input_events);
    return events;
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
