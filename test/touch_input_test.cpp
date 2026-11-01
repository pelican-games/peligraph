#include "../src/core/os/inputstate.hpp"
#include "../src/core/os/actionmap.hpp"
#include "../src/core/os/inputsequence.hpp"
#include "../src/core/ui/inputrouter.hpp"
#include "../src/project/jsonrpc.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
using namespace Pelican;
namespace {
InputActionMap actions(std::string_view binding="touch:region", std::array<float,4> region={0,0,.5f,1}) {
    const auto map=parseInputActionsJson({{"schema","pelican.input_actions"},{"version",1},{"action_sets",{
        {{"name","game"},{"actions",{{{"name","left"},{"type","button"}},{{"name","right"},{"type","button"}}}}},
        {{"name","menu"},{"actions",{{{"name","menu_left"},{"type","button"}}}}}
    }}});
    const auto profile=parseInputProfileJson({{"schema","pelican.input_profile"},{"version",1},{"name","touch"},{"bindings",{
        {{"action","left"},{"binding",binding},{"region",region}},
        {{"action","right"},{"binding",binding},{"region",{.5,0,1,1}}},
        {{"action","menu_left"},{"binding",binding},{"region",region}}
    }}},map);
    return applyInputProfile(map,profile);
}
void frame(InputStateCore &input, double seconds) { input.setFrameTime(seconds); input.beginFrame(); }
void touch(InputStateCore &input,int id,TouchPhase phase,float x,float y,float slop=8) {
    input.queueEvent(InputEvent::touch(id,phase,x,y,slop));
}
const TouchState &finger(const InputSnapshot &s,int id) {
    return *std::find_if(s.touches.begin(),s.touches.end(),[&](const auto &f){return f.id==id;});
}
}
TEST_CASE("Touch snapshot retains edges primary pointer and individual consumption", "[touch_input][sa1f]") {
    InputStateCore input; input.setPointerTransform(pointerTransformFor(200,100,100,50));
    input.queueCursorMove(5,6); frame(input,0); REQUIRE(input.pointer().position_px.x==10);
    touch(input,8,TouchPhase::down,20,10); touch(input,2,TouchPhase::down,40,20); frame(input,1);
    REQUIRE(input.pointer().position_px.x==40); REQUIRE(input.pointer().inside);
    REQUIRE(finger(input.currentSnapshot(),8).pushed); REQUIRE(finger(input.currentSnapshot(),2).down);
    touch(input,2,TouchPhase::move,45,20); frame(input,1.1); REQUIRE(input.pointer().position_px.x==40);
    REQUIRE(finger(input.currentSnapshot(),2).previous_x==40);
    input.consumeTouchForActions(8); auto masked=input.freezeActionsSnapshot();
    REQUIRE_FALSE(std::any_of(masked.touches.begin(),masked.touches.end(),[](const auto &f){return f.id==8;}));
    REQUIRE(finger(masked,2).down); REQUIRE(finger(input.currentSnapshot(),8).down);
    input.queueAxisEvent(1,1); frame(input,1.15); REQUIRE(input.pointer().position_px.x==10);
    touch(input,2,TouchPhase::move,45,20); frame(input,1.16); REQUIRE(input.pointer().position_px.x==40);
    touch(input,8,TouchPhase::up,20,10); frame(input,1.2); REQUIRE(input.pointer().position_px.x==90);
    REQUIRE(finger(input.currentSnapshot(),8).released);
    touch(input,2,TouchPhase::cancel,45,20); frame(input,1.3); REQUIRE_FALSE(input.pointer().inside);
    REQUIRE(finger(input.currentSnapshot(),2).cancelled);
    input.queueCursorMove(7,8); frame(input,1.4); REQUIRE(input.pointer().position_px.x==14);
    touch(input,31,TouchPhase::down,10,10); touch(input,31,TouchPhase::up,10,10); frame(input,2);
    const auto &f=finger(input.currentSnapshot(),31); REQUIRE(f.pushed); REQUIRE(f.released); REQUIRE_FALSE(f.down);
    frame(input,3); REQUIRE(std::all_of(input.currentSnapshot().touches.begin(),input.currentSnapshot().touches.end(),[](auto f){return f.id<0;}));
    for(int id=0;id<11;++id) touch(input,id,TouchPhase::down,10,10);
    touch(input,32,TouchPhase::down,10,10); frame(input,4);
    REQUIRE(std::count_if(input.currentSnapshot().touches.begin(),input.currentSnapshot().touches.end(),[](auto f){return f.down;})==10);
    input.clear(); REQUIRE_FALSE(input.pointer().inside);
}
TEST_CASE("Touch region aggregates fingers crossing bounds same-frame edges and set consumption", "[touch_input][sa1f]") {
    const auto map=actions(); InputStateCore input; input.setPointerTransform({1,1,100,100});
    touch(input,3,TouchPhase::down,10,20); touch(input,7,TouchPhase::down,80,20); frame(input,0);
    auto a=evaluateInputActions(map,input.currentFrameInput(),{"game"}); REQUIRE(a.get("left").pressed); REQUIRE(a.get("right").held);
    a=evaluateInputActions(map,input.currentFrameInput(),{"game","menu"});
    REQUIRE(a.get("menu_left").held); REQUIRE_FALSE(a.get("left").pressed); REQUIRE(a.get("right").held);
    input.consumeTouchForActions(3); { auto f=input.currentFrameInput(); f.snapshot=input.freezeActionsSnapshot();
    a=evaluateInputActions(map,f,{"game"}); REQUIRE_FALSE(a.get("left").held); REQUIRE(a.get("right").held); }
    touch(input,9,TouchPhase::down,15,20); touch(input,3,TouchPhase::up,10,20); frame(input,.1);
    a=evaluateInputActions(map,input.currentFrameInput(),{"game"}); REQUIRE(a.get("left").held); REQUIRE_FALSE(a.get("left").released);
    touch(input,9,TouchPhase::move,50,20); frame(input,.2);
    a=evaluateInputActions(map,input.currentFrameInput(),{"game"}); REQUIRE(a.get("left").released); REQUIRE_FALSE(a.get("left").held);
    REQUIRE(a.get("right").held); REQUIRE_FALSE(a.get("right").pressed); // another finger already holds it
    touch(input,9,TouchPhase::cancel,50,20); touch(input,7,TouchPhase::up,80,20); frame(input,.3);
    a=evaluateInputActions(map,input.currentFrameInput(),{"game"}); REQUIRE(a.get("right").released);
    touch(input,1,TouchPhase::down,10,20); touch(input,1,TouchPhase::up,10,20); frame(input,.4);
    a=evaluateInputActions(map,input.currentFrameInput(),{"game"}); REQUIRE(a.get("left").pressed); REQUIRE(a.get("left").released); REQUIRE_FALSE(a.get("left").held);
    touch(input,2,TouchPhase::down,50,20); frame(input,.5);
    a=evaluateInputActions(map,input.currentFrameInput(),{"game"}); REQUIRE_FALSE(a.get("left").held); REQUIRE(a.get("right").held);
}
TEST_CASE("Touch tap clock density slop path cancel and down region", "[touch_input][sa1f]") {
    const auto map=actions("touch:tap");
    const auto run=[&](double duration,float moved,float slop,bool cancel=false,bool return_back=false,float start=20) {
        InputStateCore input; input.setPointerTransform({1,1,100,100});
        touch(input,1,TouchPhase::down,start,20,slop); frame(input,0);
        REQUIRE_FALSE(evaluateInputActions(map,input.currentFrameInput(),{"game"}).get("left").pressed);
        if(return_back) { touch(input,1,TouchPhase::move,start+moved,20); frame(input,duration/2); moved=0; }
        touch(input,1,cancel?TouchPhase::cancel:TouchPhase::up,start+moved,20); frame(input,duration);
        const auto a=evaluateInputActions(map,input.currentFrameInput(),{"game"}).get("left");
        REQUIRE_FALSE(a.held); REQUIRE(a.pressed==a.released); return a.pressed;
    };
    REQUIRE(run(.25,8,8)); REQUIRE_FALSE(run(.251,0,8)); REQUIRE_FALSE(run(.1,9,8));
    REQUIRE(run(.1,15,16)); REQUIRE_FALSE(run(.1,0,8,true)); REQUIRE_FALSE(run(.1,9,8,false,true));
    REQUIRE_FALSE(run(.1,-2,8,false,false,51)); // began outside, ended inside
    InputStateCore input; input.setPointerTransform({1,1,100,100});
    touch(input,1,TouchPhase::down,10,10); touch(input,1,TouchPhase::up,10,10); frame(input,0);
    const auto a=evaluateInputActions(map,input.currentFrameInput(),{"game"}).get("left"); REQUIRE(a.pressed); REQUIRE(a.released);
}
TEST_CASE("Touch profile rejects missing invalid or irrelevant region", "[touch_input][sa1f]") {
    auto map=actions();
    nlohmann::json p={{"schema","pelican.input_profile"},{"version",1},{"name","test"},{"bindings",{{{"action","left"},{"binding","touch:region"},{"region",{0,0,1,1}}}}}};
    REQUIRE_NOTHROW(parseInputProfileJson(p,map));
    for(const auto &r: std::vector<nlohmann::json>{{0,0,0,1},{0,1,1,0},{-1,0,1,1},{0,0,2,1},{0,0,1},{0,0,"x",1}}) {
        p["bindings"][0]["region"]=r; REQUIRE_THROWS(parseInputProfileJson(p,map));
    }
    p["bindings"][0].erase("region"); REQUIRE_THROWS(parseInputProfileJson(p,map));
    p["bindings"][0]["binding"]="kbd:space"; p["bindings"][0]["region"]={0,0,1,1}; REQUIRE_THROWS(parseInputProfileJson(p,map));
    p["bindings"][0]["binding"]="touch:other"; REQUIRE_THROWS(parseInputProfileJson(p,map));
    REQUIRE_THROWS(actions("touch:region",{0,0,1,0}));
}
TEST_CASE("Touch UI pointers capture independently consume idle and release cancel", "[touch_input][sa1f]") {
    ui::WidgetArena arena;
    const auto widget=arena.create({.stable_id="left",.type="button",.rect_ui={0,0,50,100},.clip_ui={0,0,100,100}});
    const std::array traversal{widget}; const auto viewport=ui::ViewportTransform::letterboxed({100,100},1);
    ui::InputRouter router; InputStateCore input; input.setPointerTransform({1,1,100,100});
    touch(input,0,TouchPhase::down,10,20); touch(input,4,TouchPhase::down,80,20); frame(input,0);
    auto result=router.routeFrameInput(input.currentFrameInput(),arena,traversal,viewport);
    REQUIRE(result.events.size()==2); REQUIRE(result.events[0].input.pointer_id==1); REQUIRE(result.events[1].input.pointer_id==5);
    REQUIRE(result.consumed_touches[0]); REQUIRE_FALSE(result.consumed_touches[4]); REQUIRE_FALSE(result.consumed_pointer);
    frame(input,.1); result=router.routeFrameInput(input.currentFrameInput(),arena,traversal,viewport); REQUIRE(result.consumed_touches[0]);
    touch(input,0,TouchPhase::move,90,20); touch(input,4,TouchPhase::move,15,20); frame(input,.2);
    result=router.routeFrameInput(input.currentFrameInput(),arena,traversal,viewport);
    REQUIRE(result.events[0].target==widget); REQUIRE(result.events[0].input.kind==ui::PointerKind::Move);
    touch(input,0,TouchPhase::up,90,20); touch(input,4,TouchPhase::cancel,15,20); frame(input,.3);
    result=router.routeFrameInput(input.currentFrameInput(),arena,traversal,viewport);
    REQUIRE(result.consumed_touches[0]); REQUIRE(result.events[1].input.kind==ui::PointerKind::Cancel); REQUIRE_FALSE(result.events[1].input.button);
    frame(input,.4); result=router.routeFrameInput(input.currentFrameInput(),arena,traversal,viewport); REQUIRE_FALSE(result.consumed_touches[0]);
    touch(input,3,TouchPhase::down,10,20); touch(input,7,TouchPhase::down,15,20); frame(input,.5);
    InputConsumptionMask mask; mask.consumeTouch(3);
    result=router.routeFrameInput(input.currentFrameInput(),arena,traversal,viewport,{},mask);
    REQUIRE(result.events.size()==1); REQUIRE(result.events[0].input.pointer_id==8);
}
TEST_CASE("Touch input_seq v3 roundtrip v2 compatibility and RPC touch", "[touch_input][sa1f]") {
    InputStateCore input; input.setPointerTransform({1,1,100,100}); touch(input,31,TouchPhase::down,10,20,16); frame(input,0);
    InputSequence seq; { const auto f=input.currentFrameInput(); seq.appendFrame(f.ordered_events,f.pointer_transform); }
    const auto text=seq.toJsonLines(); REQUIRE(text.find("\"version\":3")!=std::string::npos);
    const auto parsed=InputSequence::fromJsonLines(text); REQUIRE(parsed.sourceVersion()==3);
    const auto &e=parsed.frames()[0].events[0]; REQUIRE(e.pointer_id==31); REQUIRE(e.touch_phase==TouchPhase::down); REQUIRE(e.touch_slop_px==16);
    InputSequence plain; REQUIRE(plain.toJsonLines().find("\"version\":2")!=std::string::npos);
    auto legacy=text; legacy.replace(legacy.find("\"version\":3"),11,"\"version\":2"); REQUIRE_THROWS(InputSequence::fromJsonLines(legacy));
    nlohmann::json params={{"events",{{{"type","touch"},{"pointer_id",31},{"phase","cancel"},{"x",10},{"y",20}}}}};
    const auto rpc=parseInjectInputParams(params); REQUIRE(rpc[0].pointer_id==31); REQUIRE(rpc[0].phase=="cancel");
    params["events"][0]["pointer_id"]=32; REQUIRE_THROWS(parseInjectInputParams(params));
    params["events"][0]["pointer_id"]=0; params["events"][0]["phase"]="unknown"; REQUIRE_THROWS(parseInjectInputParams(params));
}

TEST_CASE("Touch pointer id reuse preserves frame edges final hold and completed tap", "[touch_input][sa1f]") {
    InputStateCore input; input.setPointerTransform({1,1,100,100});
    touch(input,0,TouchPhase::down,10,10); touch(input,0,TouchPhase::up,10,10);
    touch(input,0,TouchPhase::down,80,10); frame(input,0);
    REQUIRE(finger(input.currentSnapshot(),0).down); REQUIRE(finger(input.currentSnapshot(),0).pushed);
    REQUIRE(finger(input.currentSnapshot(),0).released); REQUIRE(input.pointer().position_px.x==80);
    const auto a=evaluateInputActions(actions("touch:tap"),input.currentFrameInput(),{"game"});
    REQUIRE(a.get("left").pressed); REQUIRE(a.get("left").released);
    const auto region=evaluateInputActions(actions(),input.currentFrameInput(),{"game"});
    REQUIRE(region.get("left").pressed); REQUIRE(region.get("left").released); REQUIRE(region.get("right").held);
}

TEST_CASE("Touch replay uses recorded fps for tap duration", "[touch_input][sa1f]") {
    const auto path=std::filesystem::temp_directory_path() /
        ("pelican_sa1f_replay_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".jsonl");
    for (const auto up_frame : {15,16}) {
        InputSequence seq{60};
        auto down=InputEvent::touch(0,TouchPhase::down,10,10); down.event_seq=0;
        auto up=InputEvent::touch(0,TouchPhase::up,10,10); up.event_seq=1;
        seq.appendFrame(std::span{&down,1},{1,1,100,100});
        for (int i=1;i<up_frame;++i) seq.appendFrame({},{1,1,100,100});
        seq.appendFrame(std::span{&up,1},{1,1,100,100}); seq.writeFile(path);
        InputState input; InputSequenceRuntime runtime; runtime.startReplay(path);
        while (!runtime.replayComplete()) {
            runtime.prepareFrame(input,{1,1,200,200}); input.beginFrame();
            const auto action=evaluateInputActions(actions("touch:tap"),input.currentFrameInput(),{"game"}).get("left");
            REQUIRE(action.pressed == (runtime.replayComplete() && up_frame==15));
        }
        runtime.stopReplay();
    }
    std::filesystem::remove(path);
}

TEST_CASE("Touch UI remains independent of mouse tool consumption", "[touch_input][sa1f]") {
    ui::WidgetArena arena;
    const auto widget=arena.create({.stable_id="button",.type="button",.rect_ui={0,0,100,100},.clip_ui={0,0,100,100}});
    const std::array traversal{widget}; const auto viewport=ui::ViewportTransform::letterboxed({100,100},1);
    ui::InputRouter router; InputStateCore input; input.setPointerTransform({1,1,100,100});
    input.queueCursorMove(10,10); input.queueButtonEvent(KeyCode::MouseLeft,true);
    touch(input,0,TouchPhase::down,20,20); frame(input,0);
    InputConsumptionMask mask; mask.consumePointer();
    auto result=router.routeFrameInput(input.currentFrameInput(),arena,traversal,viewport,{},mask);
    REQUIRE(result.events.size()==1); REQUIRE(result.events[0].input.pointer_id==1); REQUIRE(result.consumed_touches[0]);
    REQUIRE_FALSE(result.consumed_pointer);
    ui::InputRouter mouse_router;
    std::array events{ui::UiPointerEvent{.event_seq=0,.kind=ui::PointerKind::Down,.pointer_id=0,.button=ui::PointerButton::Left,.position_px={10,10}}};
    REQUIRE(mouse_router.route(events,arena,traversal,viewport).consumed_pointer);
    REQUIRE_FALSE(mouse_router.route({},arena,traversal,viewport).consumed_pointer); // Preserve Windows idle behavior.
}
