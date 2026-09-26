// Fails the Debug CRT on purpose, in the way named by argv[1]. The test
// executable gets no CRT setup of its own: whatever routing it has comes from
// crt_report_to_stderr.cpp, which test/crt_report_to_stderr.cmake adds to every
// test executable. run_crt_report_probe.cmake checks that each failure ends the
// process at once with the message on stderr, rather than at a dialog.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
#endif

int main(int argc, char **argv) {
#if defined(_WIN32) && defined(_DEBUG)
    const char *mode = argc > 1 ? argv[1] : "";
    if (std::strcmp(mode, "assert") == 0) {
        const bool pelicanCrtProbeAssert = false;
        _ASSERTE(pelicanCrtProbeAssert);
    } else if (std::strcmp(mode, "stl") == 0) {
        // The shape of the 2026-09-27 WP375b hang: a debug-iterator check.
        std::vector<int> values{1, 2, 3};
        values.erase(values.end());
    } else if (std::strcmp(mode, "abort") == 0) {
        std::abort();
    } else {
        std::fprintf(stderr, "unknown mode '%s'\n", mode);
        return 2;
    }
    // Reached only when the failure was reported and execution went on.
    std::printf("CRT_PROBE_CONTINUED\n");
    return 0;
#else
    (void)argc;
    (void)argv;
    std::printf("CRT_PROBE_SKIPPED: not a Windows Debug CRT\n");
    return 0;
#endif
}
