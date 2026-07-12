#pragma once

#include "../container.hpp"
#include "../userpublic/userinput.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Pelican {

bool isValidKeyCode(KeyCode code) noexcept;

inline constexpr std::size_t gamepad_slot_count = 16;

enum class GamepadButton : std::uint8_t {
    A,
    B,
    X,
    Y,
    LeftBumper,
    RightBumper,
    Back,
    Start,
    Guide,
    LeftThumb,
    RightThumb,
    DpadUp,
    DpadRight,
    DpadDown,
    DpadLeft,
    Count,
};

enum class GamepadAxis : std::uint8_t {
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
    Count,
};

inline constexpr std::size_t gamepad_button_count = static_cast<std::size_t>(GamepadButton::Count);
inline constexpr std::size_t gamepad_axis_count = static_cast<std::size_t>(GamepadAxis::Count);

std::string_view gamepadButtonName(GamepadButton button) noexcept;
std::string_view gamepadAxisName(GamepadAxis axis) noexcept;
std::optional<GamepadButton> gamepadButtonFromName(std::string_view name) noexcept;
std::optional<GamepadAxis> gamepadAxisFromName(std::string_view name) noexcept;

struct InputSnapshot {
    std::array<std::uint8_t, key_code_count> down{};
    std::array<std::uint8_t, key_code_count> pushed{};
    std::array<std::uint8_t, key_code_count> released{};
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    float mouse_delta_x = 0.0f;
    float mouse_delta_y = 0.0f;
    std::array<std::array<std::uint8_t, gamepad_button_count>, gamepad_slot_count> gamepad_down{};
    std::array<std::array<std::uint8_t, gamepad_button_count>, gamepad_slot_count> gamepad_pushed{};
    std::array<std::array<std::uint8_t, gamepad_button_count>, gamepad_slot_count> gamepad_released{};
    std::array<std::array<float, gamepad_axis_count>, gamepad_slot_count> gamepad_axes{};

    bool getKey(KeyCode code) const noexcept;
    bool isKeyPushed(KeyCode code) const noexcept;
    bool isKeyReleased(KeyCode code) const noexcept;
    std::size_t downCount() const noexcept;
    std::size_t pushedCount() const noexcept;
    std::size_t releasedCount() const noexcept;
    bool getGamepadButton(std::size_t pad, GamepadButton button) const noexcept;
    bool isGamepadButtonPushed(std::size_t pad, GamepadButton button) const noexcept;
    bool isGamepadButtonReleased(std::size_t pad, GamepadButton button) const noexcept;
    float getGamepadAxis(std::size_t pad, GamepadAxis axis) const noexcept;
};

struct InputEvent {
    enum class Type : std::uint8_t {
        button,
        cursor_move,
        axis,
        scroll,
        character,
        gamepad_button,
        gamepad_axis,
    };

    static constexpr std::uint64_t unassigned_sequence = std::numeric_limits<std::uint64_t>::max();

    std::uint64_t event_seq = unassigned_sequence;
    Type type = Type::button;
    KeyCode code = KeyCode::Count;
    std::uint8_t pressed = 0;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    float axis_x = 0.0f;
    float axis_y = 0.0f;
    std::uint32_t codepoint = 0;
    std::uint8_t gamepad = 0;
    GamepadButton pad_button = GamepadButton::Count;
    GamepadAxis pad_axis = GamepadAxis::Count;
    float pad_value = 0.0f;

    static InputEvent button(KeyCode code, bool pressed) noexcept;
    static InputEvent cursorMove(float x, float y) noexcept;
    static InputEvent axis(float x, float y) noexcept;
    static InputEvent scroll(float x, float y) noexcept;
    static InputEvent character(std::uint32_t codepoint) noexcept;
    static InputEvent gamepadButton(std::uint8_t pad, GamepadButton button, bool pressed) noexcept;
    static InputEvent gamepadAxis(std::uint8_t pad, GamepadAxis axis, float value) noexcept;
};

struct GamepadState {
    std::array<std::uint8_t, gamepad_button_count> buttons{};
    std::array<float, gamepad_axis_count> axes{};
};

class GamepadStateSource {
  public:
    virtual ~GamepadStateSource() = default;
    virtual bool read(std::size_t slot, GamepadState &state) = 0;
};

class GamepadEventPoller {
    std::array<std::uint8_t, gamepad_slot_count> connected{};
    std::array<GamepadState, gamepad_slot_count> previous{};

  public:
    std::vector<InputEvent> poll(GamepadStateSource &source, bool enabled);
};

struct InputConsumptionMask {
    std::array<std::uint8_t, key_code_count> controls{};
    bool pointer_motion = false;

    void consumeControl(KeyCode code) noexcept;
    void consumePointerMotion() noexcept;
    void consumePointer() noexcept;
    bool consumesControl(KeyCode code) const noexcept;
    InputSnapshot apply(const InputSnapshot &source) const noexcept;
};

namespace internal {

struct FrameInputBorrowState {
    std::uint64_t generation = 0;
    std::size_t borrowers = 0;
    bool active = false;
};

} // namespace internal

// InputState owns the event storage. A FrameInput (and its ordered_events span) is
// valid only until the next beginFrame(). Do not detach or retain the span. In a
// debug build, retaining the FrameInput borrow itself across beginFrame() asserts.
struct FrameInput {
    std::span<const InputEvent> ordered_events;
    InputSnapshot snapshot;

    FrameInput() = default;
    FrameInput(const FrameInput &other) noexcept;
    FrameInput(FrameInput &&other) noexcept;
    FrameInput &operator=(const FrameInput &other) noexcept;
    FrameInput &operator=(FrameInput &&other) noexcept;
    ~FrameInput();

    bool isValid() const noexcept;
    std::uint64_t generation() const noexcept;

  private:
    std::shared_ptr<internal::FrameInputBorrowState> borrow_state;
    std::uint64_t borrowed_generation = 0;

    FrameInput(std::span<const InputEvent> events, const InputSnapshot &snapshot,
               std::shared_ptr<internal::FrameInputBorrowState> state) noexcept;
    void acquire() noexcept;
    void release() noexcept;

    friend class InputStateCore;
};

static_assert(std::is_trivially_copyable_v<InputSnapshot>);
static_assert(std::is_standard_layout_v<InputSnapshot>);
static_assert(std::is_trivially_copyable_v<InputEvent>);
static_assert(std::is_standard_layout_v<InputEvent>);

class InputStateCore {
    InputSnapshot snapshot{};
    std::array<std::uint8_t, key_code_count> current_down{};
    std::array<std::array<std::uint8_t, gamepad_button_count>, gamepad_slot_count> current_gamepad_down{};
    std::array<std::array<float, gamepad_axis_count>, gamepad_slot_count> current_gamepad_axes{};
    std::vector<InputEvent> pending_events;
    std::vector<InputEvent> frame_events;
    InputConsumptionMask consumption_mask;
    std::shared_ptr<internal::FrameInputBorrowState> borrow_state =
        std::make_shared<internal::FrameInputBorrowState>();
    // InputStateCore is the sole sequence owner. The production InputState owns
    // one core for the process lifetime; clear() intentionally does not reset it.
    std::uint64_t next_event_seq = 0;
    std::uint64_t frame_generation = 0;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    bool mouse_position_known = false;
    bool frame_active = false;
    bool actions_frozen = false;

  public:
    InputStateCore() = default;
    InputStateCore(const InputStateCore &) = delete;
    InputStateCore &operator=(const InputStateCore &) = delete;
    InputStateCore(InputStateCore &&) = delete;
    InputStateCore &operator=(InputStateCore &&) = delete;
    ~InputStateCore();

    void queueButtonEvent(KeyCode code, bool pressed);
    void queueCursorMove(float x, float y);
    void queueAxisEvent(float x, float y);
    void queueEvent(InputEvent event);
    void queueEvents(const std::vector<InputEvent> &events);
    void beginFrame();
    void clear();

    const InputSnapshot &currentSnapshot() const noexcept;
    FrameInput currentFrameInput() const noexcept;
    void consumeControlForActions(KeyCode code) noexcept;
    void consumePointerMotionForActions() noexcept;
    void consumePointerForActions() noexcept;
    const InputConsumptionMask &consumptionMask() const noexcept;
    InputSnapshot freezeActionsSnapshot() noexcept;
    std::uint64_t frameGeneration() const noexcept;
    std::size_t frameInputBorrowCount() const noexcept;
    std::size_t pendingEventCount() const noexcept;
    std::uint64_t nextEventSequence() const noexcept;
};

DECLARE_MODULE(InputState) {
    InputStateCore core;

  public:
    void queueEvent(InputEvent event);
    void queueEvents(const std::vector<InputEvent> &events);
    void beginFrame();
    void clear();
    const InputSnapshot &currentSnapshot() const noexcept;
    FrameInput currentFrameInput() const noexcept;
    void consumeControlForActions(KeyCode code) noexcept;
    void consumePointerMotionForActions() noexcept;
    void consumePointerForActions() noexcept;
    const InputConsumptionMask &consumptionMask() const noexcept;
    InputSnapshot freezeActionsSnapshot() noexcept;
    std::uint64_t frameGeneration() const noexcept;
    std::uint64_t nextEventSequence() const noexcept;
    std::size_t pendingEventCount() const noexcept;
};

} // namespace Pelican
