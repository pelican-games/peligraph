#include "../src/platform/android/inputmap.hpp"
#include <catch2/catch_test_macros.hpp>
#include <limits>
using namespace Pelican;
namespace A = Pelican::Android;
TEST_CASE("Android keyboard stable keycode table", "[android_input][sa1f]") {
    for(int i=0;i<26;++i) REQUIRE(A::keyCode(29+i) == static_cast<KeyCode>(int(KeyCode::A)+i));
    for(int i=0;i<10;++i) REQUIRE(A::keyCode(7+i) == static_cast<KeyCode>(int(KeyCode::Num0)+i));
    for(int i=0;i<12;++i) REQUIRE(A::keyCode(131+i) == static_cast<KeyCode>(int(KeyCode::F1)+i));
    const std::pair<int,KeyCode> table[] = {{19,KeyCode::ArrowUp},{20,KeyCode::ArrowDown},{21,KeyCode::ArrowLeft},{22,KeyCode::ArrowRight},
        {62,KeyCode::Space},{66,KeyCode::Enter},{111,KeyCode::Escape},{61,KeyCode::Tab},{67,KeyCode::Backspace},
        {59,KeyCode::LeftShift},{60,KeyCode::RightShift},{113,KeyCode::LeftControl},{114,KeyCode::RightControl},
        {57,KeyCode::LeftAlt},{58,KeyCode::RightAlt},{117,KeyCode::LeftSuper},{118,KeyCode::RightSuper}};
    for(const auto &[code,key]:table) REQUIRE(A::keyCode(code)==key);
    REQUIRE_FALSE(A::keyCode(4)); REQUIRE_FALSE(A::keyCode(-1)); REQUIRE_FALSE(A::keyCode(999));
    REQUIRE_FALSE(A::isPadSource(A::source_keyboard));
    REQUIRE(A::isPadSource(A::source_gamepad)); REQUIRE(A::isPadSource(A::source_joystick));
    REQUIRE(A::isPadSource(A::source_gamepad | A::source_keyboard));
}
TEST_CASE("Android pad buttons axes GLFW convention and stable slots", "[android_input][sa1f]") {
    const int codes[]={96,97,99,100,102,103,109,108,110,106,107,19,22,20,21};
    for(std::size_t i=0;i<gamepad_button_count;++i) REQUIRE(A::padButton(codes[i])==GamepadButton(i));
    REQUIRE_FALSE(A::padButton(62));
    const auto rest=A::padState({},{});
    REQUIRE(rest.axes[4]==-1); REQUIRE(rest.axes[5]==-1);
    A::PadAxes axes{.x=.5f,.y=-.75f,.z=-.5f,.rz=.75f,.left_trigger=.25f,.right_trigger=1,.brake=.75f,.gas=.5f,.hat_x=1,.hat_y=-1};
    auto keys=rest; keys.buttons[std::size_t(GamepadButton::DpadLeft)]=1;
    const auto state=A::padState(keys,axes);
    REQUIRE(state.axes == std::array<float,6>{.5f,-.75f,-.5f,.75f,.5f,1});
    REQUIRE(state.buttons[std::size_t(GamepadButton::DpadUp)]); REQUIRE(state.buttons[std::size_t(GamepadButton::DpadRight)]);
    REQUIRE(state.buttons[std::size_t(GamepadButton::DpadLeft)]);
    REQUIRE(A::trigger(0,0)==-1); REQUIRE(A::trigger(.5f,0)==0); REQUIRE(A::trigger(1,0)==1);
    REQUIRE(A::trigger(-2,2)==1); REQUIRE(A::trigger(std::numeric_limits<float>::quiet_NaN(),0)==-1);
    A::PadSlots slots;
    REQUIRE(slots.assign(-1)==0); REQUIRE(slots.assign(77)==1); REQUIRE(slots.assign(-1)==0);
    for(int i=2;i<16;++i) REQUIRE(slots.assign(i)==i);
    REQUIRE_FALSE(slots.assign(100)); REQUIRE(slots.assign(77)==1);
    InputStateCore input;
    input.queueEvent(InputEvent::gamepadButton(0,GamepadButton::A,true));
    input.queueEvent(InputEvent::gamepadButton(0,GamepadButton::A,false)); input.beginFrame();
    REQUIRE(input.currentSnapshot().isGamepadButtonPushed(0,GamepadButton::A));
    REQUIRE(input.currentSnapshot().isGamepadButtonReleased(0,GamepadButton::A));
    REQUIRE_FALSE(input.currentSnapshot().getGamepadButton(0,GamepadButton::A));
}
TEST_CASE("Android touch action pointer index and cancel phases", "[android_input][sa1f]") {
    REQUIRE(A::touchPhase(0,0)==TouchPhase::down); REQUIRE(A::touchPhase(1,0)==TouchPhase::up);
    REQUIRE(A::touchPhase(2,7)==TouchPhase::move); REQUIRE(A::touchPhase(3,7)==TouchPhase::cancel);
    REQUIRE(A::touchPhase(5 | (2<<8),2)==TouchPhase::down); REQUIRE_FALSE(A::touchPhase(5 | (2<<8),0));
    REQUIRE(A::touchPhase(6 | (2<<8),2)==TouchPhase::up); REQUIRE_FALSE(A::touchPhase(6 | (2<<8),1));
    REQUIRE_FALSE(A::touchPhase(8,0));
}
