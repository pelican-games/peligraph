#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Pelican {
inline void reportPlayerFatalError(std::string_view message, bool rpc, bool embedded) {
    std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
#ifdef _WIN32
    const auto *suppressed = std::getenv("PELICAN_NO_MESSAGEBOX");
    if (rpc || embedded || (suppressed && std::string_view(suppressed) == "1")) return;
    const auto length = MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0);
    std::wstring text(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), text.data(), length);
    MessageBoxW(nullptr, text.c_str(), L"Pelican", MB_OK | MB_ICONERROR);
#endif
}
} // namespace Pelican
