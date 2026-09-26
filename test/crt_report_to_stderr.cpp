// Linked into every test executable by test/crt_report_to_stderr.cmake; never
// into pelican_player, pelican_studio or pelican_cli.
//
// Under the Debug CRT an _ASSERTE, an STL debug-iterator check, a heap check
// or abort() reports through CrtDbgReport, whose default for asserts and
// errors is a modal MessageBox. Nobody dismisses it in a ctest run, so the
// test holds its resources (the GPU lease included) until the ctest timeout.
// Here the report goes to stderr instead, and an assertion or error ends the
// process on the spot with exit code 3 (the code abort() uses), so ctest fails
// the test at once and prints the message.
//
// Only Windows Debug builds do anything; elsewhere this file is empty.

#if defined(_WIN32) && defined(_DEBUG)

#include <crtdbg.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <windows.h>

namespace {

// Writes straight to the handle: the report can arrive while another thread
// holds a stdio lock, or during static destruction.
void writeStderr(const char *text) {
    const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || text == nullptr) {
        return;
    }
    DWORD written = 0;
    WriteFile(handle, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
}

// Every CrtDbgReport, narrow or wide, calls the narrow hook list first.
int __cdecl reportHook(int reportType, char *message, int *returnValue) {
    if (reportType == _CRT_WARN) {
        return FALSE; // warnings keep going to stderr through the file mode
    }
    writeStderr(reportType == _CRT_ASSERT ? "CRT assertion (test build): "
                                          : "CRT error (test build): ");
    writeStderr(message);
    writeStderr("\n");
    TerminateProcess(GetCurrentProcess(), 3);
    *returnValue = 0;
    return TRUE;
}

void __cdecl abortSignalHandler(int) {
    // abort() then ends the process with exit code 3 once this returns.
    writeStderr("abort() has been called (test build)\n");
}

struct RouteCrtReportsToStderr {
    RouteCrtReportsToStderr() {
        const int types[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
        for (const int type : types) {
            _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
        }
        _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, reportHook);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        std::signal(SIGABRT, abortSignalHandler);
        SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    }
};

} // namespace

// Library-level initialisation runs before the test's own static objects, so
// a report raised while those are constructed is already routed.
#pragma warning(push)
#pragma warning(disable : 4073)
#pragma init_seg(lib)
#pragma warning(pop)
static RouteCrtReportsToStderr routeCrtReportsToStderr;

#endif
