#include <catch2/catch_test_macros.hpp>
#ifdef _WIN32
#include "../src/core/playerdiagnostics.hpp"

TEST_CASE("player_arguments fatal dialog requires a standalone launch without output", "[player_arguments]") {
    using Pelican::shouldShowPlayerFatalDialog;
    // Double-click equivalent: no console and neither standard output handle.
    CHECK(shouldShowPlayerFatalDialog(false, false, false, false, false,
                                     FILE_TYPE_UNKNOWN, FILE_TYPE_UNKNOWN));
    // Each suppression must work on its own, without relying on captured output.
    CHECK_FALSE(shouldShowPlayerFatalDialog(true, false, false, false, false,
                                           FILE_TYPE_UNKNOWN, FILE_TYPE_UNKNOWN));
    CHECK_FALSE(shouldShowPlayerFatalDialog(false, true, false, false, false,
                                           FILE_TYPE_UNKNOWN, FILE_TYPE_UNKNOWN));
    CHECK_FALSE(shouldShowPlayerFatalDialog(false, false, true, false, false,
                                           FILE_TYPE_UNKNOWN, FILE_TYPE_UNKNOWN));
    CHECK_FALSE(shouldShowPlayerFatalDialog(false, false, false, true, false,
                                           FILE_TYPE_UNKNOWN, FILE_TYPE_UNKNOWN));
    CHECK_FALSE(shouldShowPlayerFatalDialog(false, false, false, false, true,
                                           FILE_TYPE_UNKNOWN, FILE_TYPE_UNKNOWN));
    for (DWORD type : {FILE_TYPE_PIPE, FILE_TYPE_DISK, FILE_TYPE_CHAR}) {
        INFO("connected handle type=" << type);
        CHECK_FALSE(shouldShowPlayerFatalDialog(false, false, false, false, false,
                                               type, FILE_TYPE_UNKNOWN));
        CHECK_FALSE(shouldShowPlayerFatalDialog(false, false, false, false, false,
                                               FILE_TYPE_UNKNOWN, type));
    }
}
#endif
