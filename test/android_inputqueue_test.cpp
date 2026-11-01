#include "../src/platform/android/inputqueue.hpp"
#include "../src/core/os/actionmap.hpp"
#include "../src/core/os/inputsequence.hpp"
#include "../src/core/ui/inputrouter.hpp"
#include <android/input.h>
#include <android_native_app_glue.h>
#include <catch2/catch_test_macros.hpp>
using namespace Pelican;
namespace A=Pelican::Android;
namespace {
struct Boundary {
 AConfiguration config; android_app app{.config=&config}; GamepadEventPoller poller; InputStateCore input;
 Boundary(){ A::setInputReady(true); A::setInputPaused(true); A::setInputPaused(false); drain(); input.clear(); input.setPointerTransform({1,1,100,100}); }
 ~Boundary(){A::setInputReady(false);}
 int send(AInputEvent &e){return A::handleInput(&app,&e);}
 void drain(bool enabled=true){for(auto e:A::drainInput(enabled,poller)) input.queueEvent(e); input.setFrameTime(0); input.beginFrame();}
};
InputActionMap map(std::string_view binding){
 auto m=parseInputActionsJson({{"schema","pelican.input_actions"},{"version",1},{"action_sets",{{{"name","game"},{"actions",{{{"name","hit"},{"type","button"}}}}}}}});
 return applyInputProfile(m,parseInputProfileJson({{"schema","pelican.input_profile"},{"version",1},{"name","test"},{"bindings",{{{"action","hit"},{"binding",binding},{"region",{0,0,.5,1}}}}}},m));
}
}
TEST_CASE("Android drain releases disabled pad and synchronizes enabled pad", "[sa1f][fix1]") {
 Boundary b; AInputEvent e; e.source=AINPUT_SOURCE_GAMEPAD; e.type=AINPUT_EVENT_TYPE_KEY;
 b.send(e); b.drain(); REQUIRE(b.input.currentSnapshot().getGamepadButton(0,GamepadButton::A));
 b.drain(false); REQUIRE_FALSE(b.input.currentSnapshot().getGamepadButton(0,GamepadButton::A));
 REQUIRE(b.input.currentSnapshot().isGamepadButtonReleased(0,GamepadButton::A));
 e.action=1; b.send(e); b.drain(false); b.drain(); REQUIRE_FALSE(b.input.currentSnapshot().getGamepadButton(0,GamepadButton::A));
 b.drain(false); e.action=0; b.send(e); b.drain(false); b.drain();
 REQUIRE(b.input.currentSnapshot().isGamepadButtonPushed(0,GamepadButton::A));
 e.action=1; b.send(e); e.action=0; b.send(e); e.action=1; b.send(e); b.drain();
 REQUIRE(b.input.currentSnapshot().isGamepadButtonPushed(0,GamepadButton::A));
 REQUIRE(b.input.currentSnapshot().isGamepadButtonReleased(0,GamepadButton::A));
 REQUIRE_FALSE(b.input.currentSnapshot().getGamepadButton(0,GamepadButton::A));
 // Re-enable with an undrained release: synchronize final state, not stale down.
 e.action=0; b.send(e); b.drain(); b.drain(false); e.action=1; b.send(e); b.drain();
 REQUIRE_FALSE(b.input.currentSnapshot().getGamepadButton(0,GamepadButton::A));
}
TEST_CASE("Android overflow cancel followed by move cannot reactivate region", "[sa1f][fix2]") {
 Boundary b; AInputEvent e; b.send(e); b.drain();
 auto m=map("touch:region"); REQUIRE(evaluateInputActions(m,b.input.currentFrameInput(),{"game"}).get("hit").held);
 e.action=2; for(int i=0;i<2049;++i) b.send(e); b.send(e); b.drain();
 auto a=evaluateInputActions(m,b.input.currentFrameInput(),{"game"}).get("hit");
 REQUIRE(a.released); REQUIRE_FALSE(a.held); REQUIRE_FALSE(a.pressed);
 e.action=0; b.send(e); b.drain(); REQUIRE(evaluateInputActions(m,b.input.currentFrameInput(),{"game"}).get("hit").held);
 e.action=1; b.send(e); e.action=2; b.send(e); b.drain();
 a=evaluateInputActions(m,b.input.currentFrameInput(),{"game"}).get("hit"); REQUIRE_FALSE(a.held); REQUIRE_FALSE(a.pressed);
}
TEST_CASE("Android historical round trip prevents tap and preserves region path", "[sa1f][fix3]") {
 Boundary b; AInputEvent e; b.send(e); b.drain();
 e.action=2; e.history={{{0,70,10}},{{0,60,10}}}; b.send(e); e.history.clear(); e.action=1; b.send(e); b.drain();
 { const auto f=b.input.currentFrameInput();
 REQUIRE(f.ordered_events.size()==4); REQUIRE(f.ordered_events[0].mouse_x==70); REQUIRE(f.ordered_events[1].mouse_x==60);
 REQUIRE_FALSE(evaluateInputActions(map("touch:tap"),f,{"game"}).get("hit").pressed);
 const auto a=evaluateInputActions(map("touch:region"),f,{"game"}).get("hit"); REQUIRE(a.pressed); REQUIRE(a.released); REQUIRE_FALSE(a.held); }
 // Oversized history is bounded; current sample is still delivered.
 e.action=0; b.send(e); b.drain(); e.action=2; e.history.resize(500,{{0,12,13}}); b.send(e); b.drain();
 REQUIRE(b.input.currentFrameInput().ordered_events.size()<=66);
 REQUIRE(b.input.currentFrameInput().ordered_events.back().mouse_x==10);
}
TEST_CASE("Android stylus contact is touch and hover buttons are unhandled", "[sa1f][fix4]") {
 Boundary b; AInputEvent e; e.source=AINPUT_SOURCE_STYLUS;
 for(int action:{0,2,1,0,3}){ e.action=action; REQUIRE(b.send(e)==1); b.drain();
  REQUIRE(b.input.currentFrameInput().ordered_events.size()==1);
  REQUIRE(b.input.currentFrameInput().ordered_events[0].type==InputEvent::Type::touch);
  REQUIRE(b.input.currentFrameInput().ordered_events[0].touch_phase==*A::touchPhase(action,0));
 }
 for(int action:{7,9,10,11,12}){e.action=action; REQUIRE(b.send(e)==0);}
 e.type=AINPUT_EVENT_TYPE_KEY; e.code=331; REQUIRE(b.send(e)==0); b.drain(); REQUIRE(b.input.currentFrameInput().ordered_events.empty());
}
TEST_CASE("Android mouse cancel releases state and capture without click", "[sa1f][fix5]") {
 Boundary b; AInputEvent e; e.source=AINPUT_SOURCE_MOUSE;
 ui::WidgetArena arena; const auto w=arena.create({.stable_id="button",.type="button",.rect_ui={0,0,100,100},.clip_ui={0,0,100,100}});
 const std::array traversal{w}; auto viewport=ui::ViewportTransform::letterboxed({100,100},1); ui::InputRouter router;
 b.send(e); b.drain(); router.routeFrameInput(b.input.currentFrameInput(),arena,traversal,viewport); REQUIRE(router.hasCapture(w));
 e.action=3; REQUIRE(b.send(e)==1); b.drain(); REQUIRE_FALSE(b.input.currentSnapshot().getKey(KeyCode::MouseLeft));
 InputSequence seq; { const auto f=b.input.currentFrameInput(); seq.appendFrame(f.ordered_events,f.pointer_transform); }
 const auto replay=InputSequence::fromJsonLines(seq.toJsonLines()); REQUIRE(replay.frames()[0].events[0].canceled);
 const auto r=router.routeFrameInput(b.input.currentFrameInput(),arena,traversal,viewport);
 REQUIRE_FALSE(router.hasCapture(w));
 bool canceled=false; for(const auto &ev:r.events){canceled |= ev.input.kind==ui::PointerKind::Cancel;
  REQUIRE(std::find(ev.effects.begin(),ev.effects.end(),ui::PointerEffect::Click)==ev.effects.end());}
 REQUIRE(canceled);
 e.action=0; b.send(e); b.drain(); REQUIRE_NOTHROW(router.routeFrameInput(b.input.currentFrameInput(),arena,traversal,viewport)); REQUIRE(router.hasCapture(w));
}
