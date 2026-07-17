#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace Pelican {

class InputActionMap;
struct InputActionFrame;

enum class KeyCode : std::uint16_t {
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    Space,
    Enter,
    Escape,
    Tab,
    Backspace,
    LeftShift,
    RightShift,
    LeftControl,
    RightControl,
    LeftAlt,
    RightAlt,
    LeftSuper,
    RightSuper,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    MouseLeft,
    MouseRight,
    MouseMiddle,
    MouseButton4,
    MouseButton5,
    MouseButton6,
    MouseButton7,
    MouseButton8,
    Count,
};

inline constexpr std::size_t key_code_count = static_cast<std::size_t>(KeyCode::Count);

class UserInput {
  public:
    static bool getKey(KeyCode code);
    static bool isKeyPushed(KeyCode code);
    static bool isKeyReleased(KeyCode code);
};

struct ActionAxis2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct ActionPose {
    float position[3]{};
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    bool valid = false;
};

class Actions {
  public:
    static bool isConfigured();

    static bool isPressed(std::string_view action_name);
    static bool isReleased(std::string_view action_name);
    static bool isHeld(std::string_view action_name);
    static float axis1(std::string_view action_name);
    static ActionAxis2 axis2(std::string_view action_name);
    static ActionPose pose(std::string_view action_name);

    static void setActionSetStack(const std::vector<std::string> &action_set_stack);
    static void pushActionSet(std::string action_set_name);
    static bool popActionSet();
    static void clearActionSetStack();
    static std::vector<std::string> actionSetStack();
};

namespace internal {

// Composition-root hook: constructs the action runtime before module creation
// is frozen. Public Actions calls remain lightweight service lookups afterward.
void prepareInputActionsRuntime();

// Phase 3 of the frame contract. Later Actions queries read this frozen frame
// instead of re-evaluating the bindings.
void freezeInputActionsFrame();
std::uint64_t inputActionsEvaluationCount();
bool gamepadPollingEnabled();
std::optional<std::string> activeInputProfile();
std::vector<std::string> availableInputProfiles();
void selectInputProfile(std::string_view profile_name);
const InputActionMap *inputActionMap();
void setInputActionBackendFrame(InputActionFrame frame);

} // namespace internal

} // namespace Pelican
