#pragma once
// CPU-only NDK boundary doubles. Production builds use the actual NDK headers.
#include <array>
#include <vector>
#include <cstdint>
#include <cstddef>
enum {
 AINPUT_SOURCE_KEYBOARD=0x101, AINPUT_SOURCE_GAMEPAD=0x401, AINPUT_SOURCE_JOYSTICK=0x1000010,
 AINPUT_SOURCE_TOUCHSCREEN=0x1002, AINPUT_SOURCE_MOUSE=0x2002, AINPUT_SOURCE_STYLUS=0x4002,
 AINPUT_EVENT_TYPE_KEY=1, AINPUT_EVENT_TYPE_MOTION=2,
 AKEYCODE_BACK=4, AKEYCODE_SPACE=62, AKEYCODE_A=29, AKEYCODE_0=7, AKEYCODE_F1=131,
 AKEYCODE_BUTTON_A=96, AKEYCODE_BUTTON_MODE=110, AKEY_EVENT_ACTION_DOWN=0, AKEY_EVENT_ACTION_UP=1,
 AMOTION_EVENT_ACTION_MASK=255, AMOTION_EVENT_ACTION_DOWN=0, AMOTION_EVENT_ACTION_UP=1,
 AMOTION_EVENT_ACTION_MOVE=2, AMOTION_EVENT_ACTION_CANCEL=3, AMOTION_EVENT_ACTION_HOVER_MOVE=7,
 AMOTION_EVENT_ACTION_SCROLL=8, AMOTION_EVENT_AXIS_X=0, AMOTION_EVENT_AXIS_Y=1,
 AMOTION_EVENT_AXIS_VSCROLL=9, AMOTION_EVENT_AXIS_HSCROLL=10, AMOTION_EVENT_AXIS_Z=11,
 AMOTION_EVENT_AXIS_RZ=14, AMOTION_EVENT_AXIS_HAT_X=15, AMOTION_EVENT_AXIS_HAT_Y=16,
 AMOTION_EVENT_AXIS_LTRIGGER=17, AMOTION_EVENT_AXIS_RTRIGGER=18, AMOTION_EVENT_AXIS_GAS=22, AMOTION_EVENT_AXIS_BRAKE=23
};
struct AInputEvent {
 int source=AINPUT_SOURCE_TOUCHSCREEN, type=AINPUT_EVENT_TYPE_MOTION, action=0, device=77, code=96, repeat=0, flags=0;
 struct Pointer { int id=0; float x=10,y=10; };
 std::vector<Pointer> pointers{{}};
 std::vector<std::vector<Pointer>> history;
 std::array<float,24> axes{};
};
inline int AInputEvent_getSource(const AInputEvent *e){return e->source;}
inline int AInputEvent_getType(const AInputEvent *e){return e->type;}
inline int AInputEvent_getDeviceId(const AInputEvent *e){return e->device;}
inline int AKeyEvent_getKeyCode(const AInputEvent *e){return e->code;}
inline int AKeyEvent_getAction(const AInputEvent *e){return e->action;}
inline int AKeyEvent_getRepeatCount(const AInputEvent *e){return e->repeat;}
inline int AMotionEvent_getAction(const AInputEvent *e){return e->action;}
inline int AMotionEvent_getFlags(const AInputEvent *e){return e->flags;}
inline std::size_t AMotionEvent_getPointerCount(const AInputEvent *e){return e->pointers.size();}
inline int AMotionEvent_getPointerId(const AInputEvent *e,std::size_t i){return e->pointers.at(i).id;}
inline float AMotionEvent_getX(const AInputEvent *e,std::size_t i){return e->pointers.at(i).x;}
inline float AMotionEvent_getY(const AInputEvent *e,std::size_t i){return e->pointers.at(i).y;}
inline float AMotionEvent_getAxisValue(const AInputEvent *e,int axis,std::size_t){return e->axes.at(axis);}
inline std::size_t AMotionEvent_getHistorySize(const AInputEvent *e){return e->history.size();}
inline float AMotionEvent_getHistoricalX(const AInputEvent *e,std::size_t i,std::size_t h){return e->history.at(h).at(i).x;}
inline float AMotionEvent_getHistoricalY(const AInputEvent *e,std::size_t i,std::size_t h){return e->history.at(h).at(i).y;}
