#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

// AB-2: replacing a file a reader may hold open. A bake or a CLI writes its
// output beside the old one, closes it, and renames it over the old one; on
// Windows the rename fails while another handle (the file watcher's hash, a
// table or texture being read) is open on the destination, so it is tried
// again for a short while before the caller is told the output is locked.
namespace Pelican {

// The destination stayed held by another handle for the whole retry window.
class FileReplaceLocked : public std::runtime_error {
  public:
    FileReplaceLocked(std::filesystem::path path, const std::string &message)
        : std::runtime_error(message), path_(std::move(path)) {}
    const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

// Waits between attempts: first_wait, then doubling, cut so the waits never
// add up to more than `total`; one last attempt is made at the end.
struct FileReplaceRetry {
    std::chrono::milliseconds first_wait{50};
    std::chrono::milliseconds total{2000};
};

// Renames `staged` over `destination`. On Windows through a POSIX-semantics
// rename (FileRenameInfoEx), which supersedes a destination whose holders
// share delete, and MoveFileExW where that route does not exist; a sharing
// violation, access denied or lock violation is retried as `retry` says and
// then thrown as FileReplaceLocked. Any other failure is a std::runtime_error.
// `staged` is left where it is when the rename fails.
void replaceFile(const std::filesystem::path &staged, const std::filesystem::path &destination,
                 const FileReplaceRetry &retry = {});

// Writes `bytes` to `<path>.tmp` in the same directory (creating the
// directory), closes it, and replaceFile's it over `path`. On any failure the
// temporary file is removed and `path` is as it was.
void writeFileReplacing(const std::filesystem::path &path, std::span<const std::byte> bytes,
                        const FileReplaceRetry &retry = {});

#ifdef _WIN32
// The single attempts replaceFile is made of, shared with the project
// document transaction's own retry loop (which re-checks its digest between
// attempts). `last_error` is a Win32 error code.
namespace FileReplaceWin32 {
std::string systemErrorMessage(unsigned long code);
// A handle another process holds may go away on its own.
bool isTransientReplaceError(unsigned long code);
// FileRenameInfoEx does not exist on this Windows or filesystem.
bool isUnsupportedInformationClass(unsigned long code);
bool posixReplace(const std::filesystem::path &source, const std::filesystem::path &destination,
                  unsigned long &last_error, bool &staged_open_failed);
} // namespace FileReplaceWin32
#endif

} // namespace Pelican
