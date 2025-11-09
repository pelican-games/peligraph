#pragma once


#include <vulkan/vulkan.hpp>
// include order must NOT be swap
#include <GLFW/glfw3.h>

#include "../container.hpp"
#include <bitset>

namespace Pelican {

constexpr size_t buttons_num = (GLFW_KEY_LAST + 1) + (GLFW_GAMEPAD_BUTTON_LAST + 1) + (GLFW_MOUSE_BUTTON_LAST + 1);
constexpr int button_id_offset_keyboard = 0;
constexpr int button_id_offset_gamepad = (GLFW_KEY_LAST + 1);
constexpr int button_id_offset_mouse_button = (GLFW_KEY_LAST + 1) + (GLFW_GAMEPAD_BUTTON_LAST + 1);

struct KeyState {
    std::bitset<buttons_num> pressing, pressing_old, just_pressed, just_released;
};

class Window {
    GLFWwindow *window;

    KeyState key_state;
    float gamepad_axis[6];
    float cursor_x, cursor_y;

  public:
    Window(DependencyContainer &container);
    ~Window();

    vk::UniqueSurfaceKHR getVulkanSurface(vk::Instance instance);
    std::vector<const char *> getRequiredVulkanInstanceExts();

    // false to close
    bool process();
};

} // namespace Pelican
