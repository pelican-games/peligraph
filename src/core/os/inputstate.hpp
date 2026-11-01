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
#include <string>
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

inline constexpr std::size_t touch_count = 10;
inline constexpr std::size_t touch_id_count = 32;
enum class TouchPhase : std::uint8_t { down, move, up, cancel };
std::string_view touchPhaseName(TouchPhase phase) noexcept;
std::optional<TouchPhase> touchPhaseFromName(std::string_view name) noexcept;
struct TouchState {
    std::int32_t id = -1;
    bool down = false, pushed = false, released = false, cancelled = false;
    bool previous_down = false;
    float x = 0, y = 0, previous_x = 0, previous_y = 0, down_x = 0, down_y = 0;
    float max_distance_sq = 0, slop_px = 8;
    double down_time = 0;
    std::uint64_t down_order = 0;
};

struct TouchTap {
    std::int32_t id = -1;
    float down_x = 0, down_y = 0, x = 0, y = 0, distance_sq = 0, slop_px = 8;
    double duration = 0;
};
struct InputSnapshot {
    // Completed taps remain available even when an id is reused this frame.
    std::array<TouchTap, touch_count> touch_taps{};
    std::array<TouchState, touch_count> touches{};
    double time_seconds = 0;
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
        touch,
    };

    static constexpr std::uint64_t unassigned_sequence = std::numeric_limits<std::uint64_t>::max();

    std::uint64_t event_seq = unassigned_sequence;
    Type type = Type::button;
    KeyCode code = KeyCode::Count;
    std::uint8_t pressed = 0;
    // Android mouse cancellation releases state without a UI click.
    bool canceled = false;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    float axis_x = 0.0f;
    float axis_y = 0.0f;
    std::uint32_t codepoint = 0;
    std::uint8_t gamepad = 0;
    GamepadButton pad_button = GamepadButton::Count;
    GamepadAxis pad_axis = GamepadAxis::Count;
    float pad_value = 0.0f;

    std::int32_t pointer_id = -1;
    TouchPhase touch_phase = TouchPhase::down;
    float touch_slop_px = 8;
    static InputEvent touch(std::int32_t id, TouchPhase phase, float x, float y, float slop_px = 8) noexcept;

    static InputEvent button(KeyCode code, bool pressed) noexcept;
    static InputEvent cursorMove(float x, float y) noexcept;
    static InputEvent axis(float x, float y) noexcept;
    static InputEvent scroll(float x, float y) noexcept;
    static InputEvent character(std::uint32_t codepoint) noexcept;
    static InputEvent gamepadButton(std::uint8_t pad, GamepadButton button, bool pressed) noexcept;
    static InputEvent gamepadAxis(std::uint8_t pad, GamepadAxis axis, float value) noexcept;
};

// Pose samples are frame-typed L1 data, not ordered InputEvents. pelican.input_seq
// v1 continues to serialize ordered_events only and rejects a pose-enabled
// record/replay session explicitly.
struct InputPoseSample {
    std::string action_name;
    ActionPose pose;
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

// GP-4: how the window coordinates of a frame's cursor events become the
// framebuffer pixels the public pointer (GameContext::input().pointer()) and
// the UI router read. InputSnapshot and InputEvent keep window coordinates;
// only those two convert, and ImGui reads window coordinates as it always
// did.
//
// scale_x / scale_y are the content scale per axis, framebuffer size / window
// size (1 on a display that is not scaled, and wherever there is no window).
// The framebuffer is the one the pixels are measured in; 0 x 0 means it is not
// known, which only a pelican.input_seq v1 recording leaves it.
struct PointerTransform {
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    std::uint32_t framebuffer_width = 0;
    std::uint32_t framebuffer_height = 0;

    bool framebufferKnown() const noexcept { return framebuffer_width != 0 && framebuffer_height != 0; }
    bool operator==(const PointerTransform &) const = default;
};

// The live transform of a framebuffer drawn for a window of logical_width x
// logical_height: framebuffer / window per axis, 1 on an axis either side
// of which is 0 (a minimized window).
PointerTransform pointerTransformFor(std::uint32_t framebuffer_width, std::uint32_t framebuffer_height,
                                     std::uint32_t logical_width, std::uint32_t logical_height) noexcept;
// Window coordinates to framebuffer pixels.
vec2 windowToFramebufferPx(const PointerTransform &transform, float window_x, float window_y) noexcept;
// The pointer at window (window_x, window_y) under transform. Outside unless
// position_known and the pixel lies in a known framebuffer.
PointerState pointerStateFor(const PointerTransform &transform, float window_x, float window_y,
                             bool position_known) noexcept;

// A locked cursor in a window embedded in another (the Studio viewport) would
// take the host's cursor, so there the lock is not applied and the cursor
// stays normal. Every other request is applied as asked.
CursorMode effectiveCursorMode(CursorMode requested, bool embedded) noexcept;
std::string_view cursorModeName(CursorMode mode) noexcept;

struct CursorModeStatus {
    CursorMode requested = CursorMode::normal;
    CursorMode effective = CursorMode::normal;
    bool embedded = false;
};

struct InputConsumptionMask {
    std::array<std::uint8_t, key_code_count> controls{};
    bool pointer_motion = false;
    std::array<std::uint8_t, touch_id_count> touches{};
    void consumeTouch(std::int32_t id) noexcept;
    bool consumesTouch(std::int32_t id) const noexcept;

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
    std::span<const InputPoseSample> pose_samples;
    InputSnapshot snapshot;
    // The frame's window-to-framebuffer conversion (see PointerTransform).
    PointerTransform pointer_transform;

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

    FrameInput(std::span<const InputEvent> events, std::span<const InputPoseSample> poses,
               const InputSnapshot &snapshot, const PointerTransform &pointer_transform,
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
    std::vector<InputPoseSample> pending_pose_samples;
    std::vector<InputPoseSample> frame_pose_samples;
    InputConsumptionMask consumption_mask;
    // The transform the next beginFrame gives its frame, and that frame's.
    // clear() keeps both: they describe the window, not the input.
    PointerTransform next_pointer_transform;
    PointerTransform frame_pointer_transform;
    PointerState frame_pointer;
    std::shared_ptr<internal::FrameInputBorrowState> borrow_state =
        std::make_shared<internal::FrameInputBorrowState>();
    // InputStateCore is the sole sequence owner. The production InputState owns
    // one core for the process lifetime; clear() intentionally does not reset it.
    std::uint64_t next_event_seq = 0;
    std::uint64_t frame_generation = 0;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    bool mouse_position_known = false;
    bool last_pointer_touch = false;
    std::uint64_t next_touch_order = 0;
    std::optional<double> next_frame_time;
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
    void queuePoseSample(InputPoseSample sample);
    void queuePoseSamples(std::vector<InputPoseSample> samples);
    // The transform the next beginFrame converts that frame's cursor with.
    void setPointerTransform(const PointerTransform &transform) noexcept;
    // Optional deterministic clock for CPU tests / replay; live uses steady_clock.
    void setFrameTime(double seconds) noexcept;
    void beginFrame();
    void clear();

    const InputSnapshot &currentSnapshot() const noexcept;
    FrameInput currentFrameInput() const noexcept;
    const PointerTransform &pointerTransform() const noexcept;
    // The pointer of the current frame, in framebuffer pixels.
    const PointerState &pointer() const noexcept;
    void consumeTouchForActions(std::int32_t id) noexcept;
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
    CursorModeStatus cursor;

  public:
    void queueEvent(InputEvent event);
    void queueEvents(const std::vector<InputEvent> &events);
    void queuePoseSample(InputPoseSample sample);
    void queuePoseSamples(std::vector<InputPoseSample> samples);
    void setPointerTransform(const PointerTransform &transform) noexcept;
    // Optional deterministic clock for CPU tests / replay; live uses steady_clock.
    void setFrameTime(double seconds) noexcept;
    void beginFrame();
    void clear();
    const InputSnapshot &currentSnapshot() const noexcept;
    FrameInput currentFrameInput() const noexcept;
    const PointerTransform &pointerTransform() const noexcept;
    const PointerState &pointer() const noexcept;
    // What game code asked for (GameContext::input().setCursorMode) and what
    // the window applied. clear() keeps both.
    void requestCursorMode(CursorMode mode) noexcept { cursor.requested = mode; }
    void setAppliedCursorMode(CursorMode effective, bool embedded) noexcept {
        cursor.effective = effective;
        cursor.embedded = embedded;
    }
    const CursorModeStatus &cursorMode() const noexcept { return cursor; }
    void consumeTouchForActions(std::int32_t id) noexcept;
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
