#include "filereplace.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>

#include <cstring>

// Windows 10 RS1 named these. Spell out the values when the SDK in use
// predates them, so the file still compiles and the call simply fails with
// ERROR_INVALID_PARAMETER at runtime, which takes the named fallback below.
#ifndef FILE_RENAME_FLAG_REPLACE_IF_EXISTS
#define FILE_RENAME_FLAG_REPLACE_IF_EXISTS 0x00000001
#endif
#ifndef FILE_RENAME_FLAG_POSIX_SEMANTICS
#define FILE_RENAME_FLAG_POSIX_SEMANTICS 0x00000002
#endif
#endif

namespace Pelican {

#ifdef _WIN32
namespace FileReplaceWin32 {

std::string systemErrorMessage(unsigned long code) {
    const std::error_code error{static_cast<int>(code), std::system_category()};
    return error.message() + " (" + std::to_string(code) + ")";
}

bool isTransientReplaceError(unsigned long code) {
    return code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
}

// FileRenameInfoEx is a Windows 10 RS1 information class that a pre-RS1 kernel
// and a filesystem that does not implement it reject outright. These are the
// answers that mean "this route does not exist here", not "this replacement
// failed".
bool isUnsupportedInformationClass(unsigned long code) {
    return code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION ||
           code == ERROR_CALL_NOT_IMPLEMENTED;
}

// Replace `destination` with `source` in one directory operation that
// supersedes a destination another process still holds open.
//
// MoveFileExW(MOVEFILE_REPLACE_EXISTING) cannot do that. A real-time scanner
// opens the file it is inspecting with FILE_SHARE_READ | FILE_SHARE_WRITE |
// FILE_SHARE_DELETE -- sharing everything, precisely so it does not break the
// application -- and the classic rename still refuses to supersede a target
// that has any open handle, returning ERROR_ACCESS_DENIED for as long as that
// handle lives. POSIX semantics changes that answer: the link is dropped from
// the directory immediately, the superseded file stays alive under its open
// handles, and later openers see the new file. It does not bypass the share
// mode -- a holder that withheld FILE_SHARE_DELETE still yields
// ERROR_SHARING_VIOLATION -- so the retry loops around this call stay.
// "replaces a manifest a real-time scanner holds open" measures both halves.
bool posixReplace(const std::filesystem::path &source, const std::filesystem::path &destination,
                  unsigned long &last_error, bool &staged_open_failed) {
    // Ask for exactly what a rename needs and nothing more. MoveFileExW opens
    // its source with DELETE | SYNCHRONIZE, so this open cannot be refused
    // anywhere that one would have succeeded -- which matters when the handle
    // a scanner holds is on the staged file rather than on the destination.
    // Widening this (FILE_WRITE_DATA, to flush the staged bytes and recover
    // what MOVEFILE_WRITE_THROUGH stood for) would trade that guarantee away.
    // The sharing flags are the other direction: granting all three only makes
    // this open tolerate handles other processes already hold.
    const HANDLE handle = CreateFileW(source.c_str(), DELETE | SYNCHRONIZE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        // The staged file, not the destination. Say which, because the two
        // read identically in a failure message and lead to different causes.
        last_error = GetLastError();
        staged_open_failed = true;
        return false;
    }

    // FileRenameInfoEx wants a fully qualified target: RootDirectory is null,
    // so a relative path would be resolved against nothing.
    std::error_code absolute_error;
    const auto target_path =
        destination.is_absolute() ? destination : std::filesystem::absolute(destination, absolute_error);
    if (absolute_error) {
        CloseHandle(handle);
        last_error = ERROR_BAD_PATHNAME;
        return false;
    }
    const std::wstring target = target_path.wstring();
    const std::size_t name_bytes = target.size() * sizeof(wchar_t);
    std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + name_bytes + sizeof(wchar_t));
    auto *info = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(info->FileName, target.c_str(), name_bytes + sizeof(wchar_t));

    const BOOL renamed =
        SetFileInformationByHandle(handle, FileRenameInfoEx, info, static_cast<DWORD>(storage.size()));
    if (!renamed) last_error = GetLastError();
    CloseHandle(handle);
    return renamed != FALSE;
}

} // namespace FileReplaceWin32
#endif

void replaceFile(const std::filesystem::path &staged, const std::filesystem::path &destination,
                 const FileReplaceRetry &retry) {
    const auto where = destination.generic_string();
#ifdef _WIN32
    using namespace FileReplaceWin32;
    const auto start = std::chrono::steady_clock::now();
    auto wait = retry.first_wait;
    bool posix_available = true;
    std::string fallback_reason;
    unsigned long last_error = ERROR_SUCCESS;
    const char *route = "FileRenameInfoEx";
    for (;;) {
        if (posix_available) {
            unsigned long error = ERROR_SUCCESS;
            bool staged_open_failed = false;
            if (posixReplace(staged, destination, error, staged_open_failed)) return;
            last_error = error;
            route = staged_open_failed ? "opening the staged file for rename" : "FileRenameInfoEx";
            if (!isTransientReplaceError(error)) {
                // This route is not usable here: finish the old way, and say
                // so if that fails too.
                posix_available = false;
                fallback_reason = std::string{staged_open_failed ? "could not open the staged file for a POSIX rename: "
                                              : isUnsupportedInformationClass(error)
                                                  ? "FileRenameInfoEx is unavailable on this Windows or filesystem: "
                                                  : "POSIX rename failed: "} +
                                  systemErrorMessage(error);
            }
        }
        if (!posix_available) {
            if (MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return;
            last_error = GetLastError();
            route = "MoveFileExW";
            if (!isTransientReplaceError(last_error))
                throw std::runtime_error("cannot replace " + where + " via " + route + ": " +
                                         systemErrorMessage(last_error) + " [after fallback: " + fallback_reason +
                                         "]");
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        if (elapsed >= retry.total)
            throw FileReplaceLocked{destination, "cannot replace " + where + ": another handle held it for " +
                                                     std::to_string(elapsed.count()) + " ms (" + route + ": " +
                                                     systemErrorMessage(last_error) + ")"};
        std::this_thread::sleep_for((std::min)(wait, retry.total - elapsed));
        wait *= 2;
    }
#else
    (void)retry;
    std::error_code error;
    std::filesystem::rename(staged, destination, error);
    if (error) throw std::runtime_error("cannot replace " + where + ": " + error.message());
#endif
}

void writeFileReplacing(const std::filesystem::path &path, std::span<const std::byte> bytes,
                        const FileReplaceRetry &retry) {
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    auto temporary = path;
    temporary += ".tmp";
    try {
        {
            std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
            if (!output) throw std::runtime_error("cannot write " + temporary.generic_string());
            output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            output.close();
            if (!output) throw std::runtime_error("cannot write " + temporary.generic_string());
        }
        replaceFile(temporary, path, retry);
    } catch (...) {
        std::filesystem::remove(temporary, error);
        throw;
    }
}

} // namespace Pelican
