#pragma once
#include "inputmap.hpp"
struct android_app;
struct AInputEvent;
namespace Pelican::Android {
int32_t handleInput(android_app *app, AInputEvent *event);
void setInputReady(bool ready);
void setInputPaused(bool paused);
std::vector<InputEvent> drainInput(bool gamepad_enabled, GamepadEventPoller &poller);
}
