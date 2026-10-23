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
#ifdef _WIN32
// Only a standalone launch without output needs a dialog as its error channel.
inline bool shouldShowPlayerFatalDialog(bool rpc, bool embedded, bool headless,
                                       bool suppressed, bool has_console,
                                       DWORD stdout_type, DWORD stderr_type) {
    const auto connected = [](DWORD type) {
        return type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK || type == FILE_TYPE_CHAR;
    };
    return !rpc && !embedded && !headless && !suppressed && !has_console &&
           !connected(stdout_type) && !connected(stderr_type);
}
#endif

inline void reportPlayerFatalError(std::string_view message, bool rpc, bool embedded, bool headless) {
    std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
#ifdef _WIN32
    const auto *suppressed = std::getenv("PELICAN_NO_MESSAGEBOX");
    if (!shouldShowPlayerFatalDialog(
            rpc, embedded, headless, suppressed && std::string_view(suppressed) == "1",
            GetConsoleWindow() != nullptr, GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)),
            GetFileType(GetStdHandle(STD_ERROR_HANDLE)))) return;
    const auto length = MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0);
    std::wstring text(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), text.data(), length);
    MessageBoxW(nullptr, text.c_str(), L"Pelican", MB_OK | MB_ICONERROR);
#endif
}
} // namespace Pelican
