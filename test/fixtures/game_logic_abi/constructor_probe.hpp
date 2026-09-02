#pragma once

#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace wp381_fixture {

inline std::filesystem::path moduleDirectory(const void *address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module)) {
        return {};
    }
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(
        module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) return {};
    buffer.resize(length);
    return std::filesystem::path{buffer}.parent_path();
}

inline void writeProbe(const void *address, std::uint64_t nonce) noexcept {
    try {
        const auto directory = moduleDirectory(address);
        if (directory.empty()) return;
        const auto value = std::to_string(nonce);
        const auto path = directory / "wp381_constructor_probe.txt";
        const auto file = CreateFileW(
            path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        (void)WriteFile(file, value.data(), static_cast<DWORD>(value.size()),
                        &written, nullptr);
        CloseHandle(file);
    } catch (...) {
    }
}

} // namespace wp381_fixture
