#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Pelican {

inline bool processUsesUtf8CodePage() {
#ifdef _WIN32
    return GetACP() == CP_UTF8;
#else
    return true;
#endif
}

inline constexpr const char *nonUtf8CodePageWarning =
    "Process code page is not UTF-8; 日本語の path で失敗することがある "
    "(Windows 10 1903 or later is required).";

} // namespace Pelican
