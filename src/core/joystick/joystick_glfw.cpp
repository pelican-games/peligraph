#include "joystickruntime.hpp"

#include <GLFW/glfw3.h>

#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {
class GlfwJoystickSource;
GlfwJoystickSource *active_source = nullptr;

class GlfwJoystickSource final : public JoystickSource {
  public:
    ~GlfwJoystickSource() override {
        if (active_source == this) active_source = nullptr;
    }

    const char *backend() const override { return "glfw"; }
    std::optional<JoystickLimits> limits() const override {
#ifdef _WIN32
        return JoystickLimits{8, 32, 4};
#else
        return std::nullopt;
#endif
    }

    void startWatching() override {
        if (active_source != nullptr && active_source != this) {
            throw std::logic_error("joystick.callback_owned@1: another joystick source owns the GLFW callback");
        }
        active_source = this;
        previous_callback_ = glfwSetJoystickCallback(&onConnection);
        if (previous_callback_ == &onConnection) previous_callback_ = nullptr;
    }

    std::vector<JoystickConnectionChange> drainChanges() override {
        return std::exchange(pending_, {});
    }

    bool present(std::size_t slot) override {
        return glfwJoystickPresent(static_cast<int>(slot)) == GLFW_TRUE;
    }

    bool identify(std::size_t slot, JoystickIdentity &out) override {
        const int jid = static_cast<int>(slot);
        const char *name = glfwGetJoystickName(jid);
        if (name == nullptr) return false;
        std::string owned_name{name};
        const char *guid = glfwGetJoystickGUID(jid);
        if (guid == nullptr) return false;
        std::string owned_guid{guid};
        out.name = std::move(owned_name);
        out.guid = std::move(owned_guid);
        out.is_gamepad = glfwJoystickIsGamepad(jid) == GLFW_TRUE;
        return true;
    }

    bool sample(std::size_t slot, JoystickSample &out) override {
        const int jid = static_cast<int>(slot);
        if (glfwJoystickPresent(jid) != GLFW_TRUE) return false;
        int hat_count = 0;
        const unsigned char *hats = glfwGetJoystickHats(jid, &hat_count);
        if (hat_count < 0) hat_count = 0;
        out.hats.clear();
        if (hats != nullptr) out.hats.assign(hats, hats + hat_count);

        int axis_count = 0;
        const float *axes = glfwGetJoystickAxes(jid, &axis_count);
        if (axis_count < 0) axis_count = 0;
        out.axes.clear();
        if (axes != nullptr) out.axes.assign(axes, axes + axis_count);

        int reported_buttons = 0;
        const unsigned char *buttons = glfwGetJoystickButtons(jid, &reported_buttons);
        const auto real_buttons = joystickRealButtonCount(reported_buttons, hat_count);
        out.buttons.clear();
        if (buttons != nullptr) {
            out.buttons.reserve(real_buttons);
            for (std::size_t index = 0; index < real_buttons; ++index) {
                out.buttons.push_back(buttons[index] == GLFW_PRESS ? 1 : 0);
            }
        }
        return true;
    }

  private:
    static void onConnection(int jid, int event) {
        if (active_source == nullptr) return;
        active_source->pending_.push_back(
            {static_cast<std::size_t>(jid), event == GLFW_CONNECTED});
        if (active_source->previous_callback_ != nullptr) {
            active_source->previous_callback_(jid, event);
        }
    }

    std::vector<JoystickConnectionChange> pending_;
    GLFWjoystickfun previous_callback_ = nullptr;
};

static_assert(GLFW_JOYSTICK_HAT_BUTTONS == 0x00050001);
// The engine never changes this GLFW init hint from its default GLFW_TRUE.
// Its policy is centralized here: real buttons exclude four synthetic hat
// buttons per hat. A future initialization change must update this contract.
inline constexpr int glfw_hat_buttons_policy = GLFW_TRUE;
static_assert(glfw_hat_buttons_policy == GLFW_TRUE);
} // namespace

std::unique_ptr<JoystickSource> makeGlfwJoystickSource() {
    return std::make_unique<GlfwJoystickSource>();
}
} // namespace Pelican
