#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Pelican::DevCli {

struct ProcessOptions {
    std::filesystem::path executable;
    std::vector<std::filesystem::path> arguments;
    std::optional<std::chrono::milliseconds> timeout;
    std::function<bool()> cancelled;
    std::filesystem::path log_path;
    std::string name = "external process";
};

struct ProcessResult {
    int exit_code = -1;
    bool timed_out = false;
    bool cancelled = false;
    std::uint64_t process_id = 0;
    std::string stdout_text;
    std::string stderr_text;
};

// Runs one child in its own process group, captures stdout/stderr, and kills the
// complete group on timeout or cancellation. Arguments are filesystem::path so
// Windows callers retain their native UTF-16 representation through CreateProcessW.
ProcessResult runProcess(const ProcessOptions &options);

// Exposed for the termination fixture and for callers that need to audit a
// previously returned process id.
bool processIsRunning(std::uint64_t process_id) noexcept;

} // namespace Pelican::DevCli
