#include "inputqueue.hpp"
#include <android/input.h>
#include <android/configuration.h>
#include <android_native_app_glue.h>
#include <mutex>
namespace Pelican::Android {
static_assert(AINPUT_SOURCE_KEYBOARD == source_keyboard && AINPUT_SOURCE_GAMEPAD == source_gamepad &&
              AINPUT_SOURCE_JOYSTICK == source_joystick && AINPUT_SOURCE_TOUCHSCREEN == source_touchscreen && AINPUT_SOURCE_MOUSE == source_mouse);
static_assert(AKEYCODE_SPACE == 62 && AKEYCODE_A == 29 && AKEYCODE_0 == 7 && AKEYCODE_F1 == 131);

namespace {
struct InputQueue final : GamepadStateSource {
    std::mutex mutex;
    bool ready = false, paused = true;
    static constexpr std::size_t limit = 2048;
    std::vector<InputEvent> events;
    PadSlots slots;
    std::array<bool, gamepad_slot_count> present{};
    std::array<GamepadState, gamepad_slot_count> keys{}, states{};
    std::array<PadAxes, gamepad_slot_count> axes{};
    bool polling_enabled = true;
    std::array<GamepadState, gamepad_slot_count> delivered{};
    bool read(std::size_t slot, GamepadState &state) override {
        state = states[slot];
        // All button transitions (including HAT OR DPAD) come from the ordered queue.
        state.buttons = {};
        return present[slot];
    }
    void neutralize() {
        events.clear();
        for (std::size_t i=0; i<key_code_count; ++i) events.push_back(InputEvent::button(KeyCode(i), false));
        for (std::size_t p=0; p<gamepad_slot_count; ++p)
            for (std::size_t b=0; b<gamepad_button_count; ++b)
                events.push_back(InputEvent::gamepadButton(p, GamepadButton(b), false));
        for (int id=0; id<touch_id_count; ++id) events.push_back(InputEvent::touch(id,TouchPhase::cancel,0,0));
        keys = {}; axes = {};
        for (auto &s : states) s = padState({},{});
    }
    bool push(InputEvent event) {
        if (!ready || paused) return false;
        if (events.size() < limit) { events.push_back(event); return true; }
        // Bounded overload recovery: release every control, then resume.
        neutralize();
        return false;
    }
    void updatePad(std::size_t slot) {
        const auto next = padState(keys[slot], axes[slot]);
        for (std::size_t b=0; b<gamepad_button_count; ++b)
            if (next.buttons[b] != states[slot].buttons[b])
                if (!push(InputEvent::gamepadButton(slot, GamepadButton(b), next.buttons[b] != 0))) return;
        states[slot] = next;
    }
};
InputQueue &queue() { static InputQueue value; return value; }
}
void setInputReady(bool ready) {
    auto &q=queue(); std::scoped_lock lock{q.mutex}; q.ready=ready; q.events.clear();
    q.polling_enabled=true; q.delivered={};
}
void setInputPaused(bool paused) {
    auto &q=queue(); std::scoped_lock lock{q.mutex}; q.paused=paused;
    if (paused && q.ready) q.neutralize();
}
std::vector<InputEvent> drainInput(bool enabled, GamepadEventPoller &poller) {
    auto &q=queue(); std::scoped_lock lock{q.mutex};
    std::vector<InputEvent> events; events.swap(q.events);
    if (!enabled || !q.polling_enabled) {
        std::erase_if(events, [](const auto &e) { return e.type==InputEvent::Type::gamepad_button; });
        // A polling boundary uses current physical state; ordinary frames keep
        // every queued transition, including down/up within the same frame.
        for (std::size_t p=0; p<gamepad_slot_count; ++p)
            for (std::size_t b=0; b<gamepad_button_count; ++b) {
                const bool down=enabled && q.states[p].buttons[b];
                if (down != (q.delivered[p].buttons[b]!=0))
                    events.push_back(InputEvent::gamepadButton(p,GamepadButton(b),down));
            }
    }
    for (const auto &e : events) if (e.type==InputEvent::Type::gamepad_button)
        q.delivered[e.gamepad].buttons[std::size_t(e.pad_button)]=e.pressed;
    q.polling_enabled=enabled;
    const auto axes = poller.poll(q,enabled);
    events.insert(events.end(),axes.begin(),axes.end());
    return events;
}
int32_t handleInput(android_app *app, AInputEvent *event) {
    const auto source=AInputEvent_getSource(event);
    const auto type=AInputEvent_getType(event);
    if (type==AINPUT_EVENT_TYPE_KEY && AKeyEvent_getKeyCode(event)==AKEYCODE_BACK) {
        if (AKeyEvent_getAction(event)==AKEY_EVENT_ACTION_UP) ANativeActivity_finish(app->activity);
        return 1;
    }
    auto &q=queue(); std::scoped_lock lock{q.mutex};
    const bool pad=isPadSource(source);
    if(type==AINPUT_EVENT_TYPE_KEY) {
        const int code=AKeyEvent_getKeyCode(event), action=AKeyEvent_getAction(event);
        if (action!=AKEY_EVENT_ACTION_DOWN && action!=AKEY_EVENT_ACTION_UP) return 0;
        if(pad) {
            const auto button=padButton(code);
            // Known Android pad keys with no GLFW counterpart are still handled,
            // e.g. BUTTON_B must never become the framework's fallback BACK.
            if (!button) return code>=AKEYCODE_BUTTON_A && code<=AKEYCODE_BUTTON_MODE;
            if (!q.ready || q.paused || AKeyEvent_getRepeatCount(event)>0) return 1;
            const auto slot=q.slots.assign(AInputEvent_getDeviceId(event));
            if(!slot) return 1;
            if(!q.present[*slot]) { q.present[*slot]=true; q.states[*slot]=padState({},{}); }
            q.keys[*slot].buttons[std::size_t(*button)]=action==AKEY_EVENT_ACTION_DOWN;
            q.updatePad(*slot); return 1;
        }
        if ((source & source_keyboard)!=source_keyboard) return 0;
        const auto key=keyCode(code); if(!key) return 0;
        if (AKeyEvent_getRepeatCount(event)==0) q.push(InputEvent::button(*key,action==AKEY_EVENT_ACTION_DOWN));
        return 1;
    }
    if(type!=AINPUT_EVENT_TYPE_MOTION) return 0;
    const auto action=AMotionEvent_getAction(event);
    if(pad) {
        if(!q.ready || q.paused) return 1;
        const auto slot=q.slots.assign(AInputEvent_getDeviceId(event)); if(!slot) return 1;
        q.present[*slot]=true;
        const auto axis=[&](int id) { return AMotionEvent_getAxisValue(event,id,0); };
        q.axes[*slot]={axis(AMOTION_EVENT_AXIS_X),axis(AMOTION_EVENT_AXIS_Y),axis(AMOTION_EVENT_AXIS_Z),axis(AMOTION_EVENT_AXIS_RZ),
            axis(AMOTION_EVENT_AXIS_LTRIGGER),axis(AMOTION_EVENT_AXIS_RTRIGGER),axis(AMOTION_EVENT_AXIS_BRAKE),axis(AMOTION_EVENT_AXIS_GAS),
            axis(AMOTION_EVENT_AXIS_HAT_X),axis(AMOTION_EVENT_AXIS_HAT_Y)};
        q.updatePad(*slot); return 1;
    }
    if(source==source_touchscreen || source==AINPUT_SOURCE_STYLUS) {
        bool handled=false;
        const auto density=AConfiguration_getDensity(app->config);
        const float slop=8.0f * (density>0 && density<ACONFIGURATION_DENSITY_ANY ? float(density)/160.0f : 1.0f);
        if ((action & AMOTION_EVENT_ACTION_MASK)==AMOTION_EVENT_ACTION_MOVE) {
            constexpr std::size_t history_limit=64;
            const auto count=AMotionEvent_getHistorySize(event);
            for (std::size_t h=0; h<std::min(count,history_limit); ++h)
                for (std::size_t i=0; i<AMotionEvent_getPointerCount(event); ++i)
                    q.push(InputEvent::touch(AMotionEvent_getPointerId(event,i),TouchPhase::move,
                        AMotionEvent_getHistoricalX(event,i,h),AMotionEvent_getHistoricalY(event,i,h),slop));
            // Do not turn a truncated excursion into a tap.
            if (count>history_limit)
                for (std::size_t i=0; i<AMotionEvent_getPointerCount(event); ++i)
                    q.push(InputEvent::touch(AMotionEvent_getPointerId(event,i),TouchPhase::cancel,
                        AMotionEvent_getX(event,i),AMotionEvent_getY(event,i),slop));
        }
        for(std::size_t i=0; i<AMotionEvent_getPointerCount(event); ++i) {
            if(const auto phase=touchPhase(action,i)) {
                q.push(InputEvent::touch(AMotionEvent_getPointerId(event,i),*phase,
                    AMotionEvent_getX(event,i),AMotionEvent_getY(event,i),slop)); handled=true;
            }
        }
        return handled;
    }
    if(source==source_mouse) {
        const auto masked=action & AMOTION_EVENT_ACTION_MASK;
        if (masked==AMOTION_EVENT_ACTION_CANCEL) {
            auto release=InputEvent::button(KeyCode::MouseLeft,false);
            release.canceled=true;
            q.push(release);
            return 1;
        }
        if (masked!=AMOTION_EVENT_ACTION_DOWN && masked!=AMOTION_EVENT_ACTION_UP && masked!=AMOTION_EVENT_ACTION_SCROLL &&
            masked!=AMOTION_EVENT_ACTION_MOVE && masked!=AMOTION_EVENT_ACTION_HOVER_MOVE) return 0;
        q.push(InputEvent::cursorMove(AMotionEvent_getX(event,0),AMotionEvent_getY(event,0)));
        if(masked==AMOTION_EVENT_ACTION_DOWN || masked==AMOTION_EVENT_ACTION_UP)
            q.push(InputEvent::button(KeyCode::MouseLeft,masked==AMOTION_EVENT_ACTION_DOWN));
        else if(masked==AMOTION_EVENT_ACTION_SCROLL)
            q.push(InputEvent::scroll(AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_HSCROLL,0),
                                      AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_VSCROLL,0)));
        else if(masked!=AMOTION_EVENT_ACTION_MOVE && masked!=AMOTION_EVENT_ACTION_HOVER_MOVE) return 0;
        return 1;
    }
    return 0;
}
}
