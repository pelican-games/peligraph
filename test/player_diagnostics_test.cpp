#include <catch2/catch_test_macros.hpp>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <string>
#include <cstdlib>
namespace {
int calls = 0;
std::wstring body, title;
int captureMessageBox(HWND, LPCWSTR text, LPCWSTR caption, UINT) {
    ++calls;
    body = text;
    title = caption;
    return IDOK;
}
}
#define MessageBoxW captureMessageBox
#include "../src/core/playerdiagnostics.hpp"
#undef MessageBoxW

TEST_CASE("player_arguments fatal diagnostics respect standalone RPC embedded and suppression", "[player_arguments]") {
    const auto *previous = std::getenv("PELICAN_NO_MESSAGEBOX");
    const std::string saved = previous ? previous : "";
    _putenv_s("PELICAN_NO_MESSAGEBOX", "");
    calls = 0;
    Pelican::reportPlayerFatalError("fatal: missing project", false, false);
    CHECK(calls == 1);
    CHECK(body == L"fatal: missing project");
    CHECK(title == L"Pelican");
    Pelican::reportPlayerFatalError("rpc", true, false);
    Pelican::reportPlayerFatalError("embedded", false, true);
    CHECK(calls == 1);
    _putenv_s("PELICAN_NO_MESSAGEBOX", "1");
    Pelican::reportPlayerFatalError("suppressed", false, false);
    CHECK(calls == 1);
    _putenv_s("PELICAN_NO_MESSAGEBOX", "");
    Pelican::reportPlayerFatalError("起動エラー", false, false);
    CHECK(calls == 2);
    CHECK(body == L"起動エラー");
    _putenv_s("PELICAN_NO_MESSAGEBOX", saved.c_str());
}
#endif
