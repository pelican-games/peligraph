#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <cstring>

namespace Pelican {
inline bool validStandardHandle(DWORD which) {
    const auto handle = GetStdHandle(which);
    return handle && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
}
inline void preparePlayerConsole(int argc, char **argv) {
    bool requested = false;
    for (int i = 1; i < argc; ++i) requested |= std::strcmp(argv[i], "--console") == 0;
    const bool redirected = validStandardHandle(STD_OUTPUT_HANDLE);
    if (!requested && !redirected) return;
    // Preserve pipes used by Studio and command-line capture across AttachConsole.
    const DWORD ids[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    HANDLE saved[3]{};
    for (int i = 0; i < 3; ++i) {
        if (validStandardHandle(ids[i])) saved[i] = GetStdHandle(ids[i]);
    }
    AttachConsole(ATTACH_PARENT_PROCESS);
    for (int i = 0; i < 3; ++i) {
        if (saved[i]) SetStdHandle(ids[i], saved[i]);
    }
    FILE *streams[] = {stdin, stdout, stderr};
    const char *devices[] = {"CONIN$", "CONOUT$", "CONOUT$"};
    for (int i = 0; i < 3; ++i) {
        if (_fileno(streams[i]) >= 0) continue;
        FILE *opened = nullptr;
        if (!saved[i]) {
            freopen_s(&opened, devices[i], i == 0 ? "r" : "w", streams[i]);
        } else {
            HANDLE duplicate = nullptr;
            if (DuplicateHandle(GetCurrentProcess(), saved[i], GetCurrentProcess(),
                                &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(duplicate), _O_TEXT);
                // Reopen the CRT stream before replacing its descriptor.
                freopen_s(&opened, "NUL", i == 0 ? "r" : "w", streams[i]);
                if (fd >= 0) { _dup2(fd, _fileno(streams[i])); _close(fd); }
                else CloseHandle(duplicate);
            }
        }
    }
}
}
#else
namespace Pelican { inline void preparePlayerConsole(int, char **) {} }
#endif
