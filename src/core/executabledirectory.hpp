#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Pelican {
inline std::filesystem::path executableDirectory() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size > 0 && size < buffer.size()) {
        buffer.resize(size);
        return std::filesystem::path{buffer}.parent_path();
    }
    throw std::runtime_error("Cannot locate the running executable");
#else
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::current_path() : executable.parent_path();
#endif
}
} // namespace Pelican
