#pragma once
#include <android/configuration.h>
struct ANativeActivity {};
struct android_app { ANativeActivity *activity=nullptr; AConfiguration *config=nullptr; };
inline void ANativeActivity_finish(ANativeActivity *){}
