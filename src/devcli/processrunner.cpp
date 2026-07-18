#include "processrunner.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Pelican::DevCli {
namespace {

std::string pathUtf8(const std::filesystem::path &path) {
    const auto encoded = path.generic_u8string();
    return {encoded.begin(), encoded.end()};
}

std::string commandForLog(const ProcessOptions &options) {
    std::ostringstream command;
    const auto append = [&](const std::filesystem::path &argument) {
        const auto value = pathUtf8(argument);
        command << (command.tellp() == std::streampos{0} ? "" : " ") << '"';
        for (const char ch : value) command << (ch == '"' ? "\\\"" : std::string(1, ch));
        command << '"';
    };
    append(options.executable);
    for (const auto &argument : options.arguments) append(argument);
    return command.str();
}

void appendLog(const ProcessOptions &options, const ProcessResult &result) {
    if (options.log_path.empty()) return;
    std::error_code error;
    if (!options.log_path.parent_path().empty()) {
        std::filesystem::create_directories(options.log_path.parent_path(), error);
        if (error) {
            throw std::runtime_error("failed to create process log directory: " +
                                     pathUtf8(options.log_path.parent_path()) + " (" +
                                     error.message() + ")");
        }
    }
    std::ofstream log{options.log_path, std::ios::binary | std::ios::app};
    if (!log.is_open()) {
        throw std::runtime_error("failed to open process log: " + pathUtf8(options.log_path));
    }
    log << "=== " << options.name << " ===\n"
        << "command: " << commandForLog(options) << "\n"
        << "pid: " << result.process_id << "\n"
        << "result: exit=" << result.exit_code << " timeout=" << (result.timed_out ? 1 : 0)
        << " cancelled=" << (result.cancelled ? 1 : 0) << "\n"
        << "[stdout]\n"
        << result.stdout_text;
    if (!result.stdout_text.empty() && result.stdout_text.back() != '\n') log << '\n';
    log << "[stderr]\n" << result.stderr_text;
    if (!result.stderr_text.empty() && result.stderr_text.back() != '\n') log << '\n';
    log << "=== end ===\n";
    if (!log) {
        throw std::runtime_error("failed to write process log: " + pathUtf8(options.log_path));
    }
}

bool cancellationRequested(const ProcessOptions &options) {
    return options.cancelled && options.cancelled();
}

#ifdef _WIN32

class UniqueHandle {
    HANDLE handle_ = nullptr;

  public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_{handle} {}
    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    UniqueHandle(UniqueHandle &&other) noexcept : handle_{other.release()} {}
    UniqueHandle &operator=(UniqueHandle &&other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    ~UniqueHandle() { reset(); }

    HANDLE get() const noexcept { return handle_; }
    HANDLE release() noexcept {
        const auto result = handle_;
        handle_ = nullptr;
        return result;
    }
    void reset(HANDLE replacement = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = replacement;
    }
    explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
};

std::wstring windowsArgument(std::wstring_view value) {
    if (value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring{value};
    std::wstring result{L"\""};
    std::size_t backslashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result += L'"';
        } else {
            result.append(backslashes, L'\\');
            result += ch;
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}

std::string windowsError(DWORD code) {
    return "Windows error " + std::to_string(code);
}

void readPipe(HANDLE pipe, std::string &destination) {
    std::array<char, 4096> buffer{};
    DWORD read = 0;
    while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
        destination.append(buffer.data(), read);
    }
}

ProcessResult runPlatformProcess(const ProcessOptions &options) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdout_read_raw = nullptr;
    HANDLE stdout_write_raw = nullptr;
    HANDLE stderr_read_raw = nullptr;
    HANDLE stderr_write_raw = nullptr;
    if (!CreatePipe(&stdout_read_raw, &stdout_write_raw, &security, 0) ||
        !CreatePipe(&stderr_read_raw, &stderr_write_raw, &security, 0)) {
        const auto error = GetLastError();
        if (stdout_read_raw) CloseHandle(stdout_read_raw);
        if (stdout_write_raw) CloseHandle(stdout_write_raw);
        if (stderr_read_raw) CloseHandle(stderr_read_raw);
        if (stderr_write_raw) CloseHandle(stderr_write_raw);
        throw std::runtime_error("failed to create process capture pipes (" + windowsError(error) + ")");
    }
    UniqueHandle stdout_read{stdout_read_raw};
    UniqueHandle stdout_write{stdout_write_raw};
    UniqueHandle stderr_read{stderr_read_raw};
    UniqueHandle stderr_write{stderr_write_raw};
    if (!SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("failed to protect process capture handles (" +
                                 windowsError(GetLastError()) + ")");
    }

    UniqueHandle job{CreateJobObjectW(nullptr, nullptr)};
    if (!job) {
        throw std::runtime_error("failed to create process job (" + windowsError(GetLastError()) + ")");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
    job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &job_limits,
                                 sizeof(job_limits))) {
        throw std::runtime_error("failed to configure process job (" + windowsError(GetLastError()) + ")");
    }

    std::wstring command = windowsArgument(options.executable.native());
    for (const auto &argument : options.arguments) {
        command += L' ';
        command += windowsArgument(argument.native());
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdout_write.get();
    startup.hStdError = stderr_write.get();
    PROCESS_INFORMATION process_info{};
    constexpr DWORD creation_flags = CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
    if (!CreateProcessW(options.executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                        creation_flags, nullptr, nullptr, &startup, &process_info)) {
        throw std::runtime_error("failed to start " + options.name + " (" +
                                 windowsError(GetLastError()) + ")");
    }
    UniqueHandle process{process_info.hProcess};
    UniqueHandle thread{process_info.hThread};
    ProcessResult result;
    result.process_id = process_info.dwProcessId;

    if (!AssignProcessToJobObject(job.get(), process.get())) {
        const auto error = GetLastError();
        TerminateProcess(process.get(), 126);
        WaitForSingleObject(process.get(), 5000);
        throw std::runtime_error("failed to assign " + options.name + " to its process job (" +
                                 windowsError(error) + ")");
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        const auto error = GetLastError();
        TerminateJobObject(job.get(), 126);
        WaitForSingleObject(process.get(), 5000);
        throw std::runtime_error("failed to resume " + options.name + " (" +
                                 windowsError(error) + ")");
    }
    thread.reset();
    stdout_write.reset();
    stderr_write.reset();

    std::thread stdout_reader{readPipe, stdout_read.get(), std::ref(result.stdout_text)};
    std::thread stderr_reader{readPipe, stderr_read.get(), std::ref(result.stderr_text)};
    const auto started = std::chrono::steady_clock::now();
    while (WaitForSingleObject(process.get(), 10) == WAIT_TIMEOUT) {
        if (cancellationRequested(options)) {
            result.cancelled = true;
            break;
        }
        if (options.timeout && std::chrono::steady_clock::now() - started >= *options.timeout) {
            result.timed_out = true;
            break;
        }
    }
    if (result.cancelled || result.timed_out) {
        const DWORD termination_code = result.timed_out ? 124U : 125U;
        TerminateJobObject(job.get(), termination_code);
        if (WaitForSingleObject(process.get(), 5000) == WAIT_TIMEOUT) {
            TerminateProcess(process.get(), termination_code);
            if (WaitForSingleObject(process.get(), 5000) == WAIT_TIMEOUT) {
                job.reset();
                stdout_read.reset();
                stderr_read.reset();
                stdout_reader.join();
                stderr_reader.join();
                throw std::runtime_error("failed to terminate " + options.name);
            }
        }
    }
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process.get(), &exit_code)) exit_code = 1;
    result.exit_code = static_cast<int>(exit_code);

    // Closing a kill-on-close job also removes descendants left behind after a
    // nominal parent exit, allowing both capture pipes to reach EOF.
    job.reset();
    stdout_reader.join();
    stderr_reader.join();
    return result;
}

#else

void readPipe(int pipe, std::string &destination) {
    std::array<char, 4096> buffer{};
    ssize_t count = 0;
    while ((count = ::read(pipe, buffer.data(), buffer.size())) > 0) {
        destination.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(pipe);
}

ProcessResult runPlatformProcess(const ProcessOptions &options) {
    int stdout_pipe[2]{};
    int stderr_pipe[2]{};
    if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
        const auto message = std::string{std::strerror(errno)};
        if (stdout_pipe[0]) ::close(stdout_pipe[0]);
        if (stdout_pipe[1]) ::close(stdout_pipe[1]);
        if (stderr_pipe[0]) ::close(stderr_pipe[0]);
        if (stderr_pipe[1]) ::close(stderr_pipe[1]);
        throw std::runtime_error("failed to create process capture pipes: " + message);
    }

    const auto pid = ::fork();
    if (pid < 0) {
        const auto message = std::string{std::strerror(errno)};
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        throw std::runtime_error("failed to fork " + options.name + ": " + message);
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);

        std::vector<std::string> storage;
        storage.reserve(options.arguments.size() + 1);
        storage.push_back(options.executable.native());
        for (const auto &argument : options.arguments) storage.push_back(argument.native());
        std::vector<char *> argv;
        argv.reserve(storage.size() + 1);
        for (auto &argument : storage) argv.push_back(argument.data());
        argv.push_back(nullptr);
        ::execv(storage.front().c_str(), argv.data());
        const auto message = "failed to exec " + options.name + ": " + std::strerror(errno) + "\n";
        ::write(STDERR_FILENO, message.data(), message.size());
        ::_exit(127);
    }

    ::setpgid(pid, pid);
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    ProcessResult result;
    result.process_id = static_cast<std::uint64_t>(pid);
    std::thread stdout_reader{readPipe, stdout_pipe[0], std::ref(result.stdout_text)};
    std::thread stderr_reader{readPipe, stderr_pipe[0], std::ref(result.stderr_text)};

    const auto started = std::chrono::steady_clock::now();
    int status = 0;
    bool exited = false;
    while (!exited) {
        const auto waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            exited = true;
            break;
        }
        if (waited < 0) {
            ::kill(-pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            stdout_reader.join();
            stderr_reader.join();
            throw std::runtime_error("failed to wait for " + options.name + ": " +
                                     std::strerror(errno));
        }
        if (cancellationRequested(options)) {
            result.cancelled = true;
            break;
        }
        if (options.timeout && std::chrono::steady_clock::now() - started >= *options.timeout) {
            result.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    if (!exited) {
        ::kill(-pid, SIGTERM);
        const auto grace_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
        while (std::chrono::steady_clock::now() < grace_deadline) {
            if (::waitpid(pid, &status, WNOHANG) == pid) {
                exited = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        if (!exited) {
            ::kill(-pid, SIGKILL);
            ::waitpid(pid, &status, 0);
        } else {
            // The direct child may exit on SIGTERM while one of its descendants
            // remains in the inherited process group and holds a capture pipe.
            ::kill(-pid, SIGKILL);
        }
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    stdout_reader.join();
    stderr_reader.join();
    return result;
}

#endif

} // namespace

ProcessResult runProcess(const ProcessOptions &options) {
    if (options.executable.empty()) throw std::invalid_argument("process executable must not be empty");
    if (options.timeout && *options.timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("process timeout must be positive");
    }
    auto result = runPlatformProcess(options);
    appendLog(options, result);
    return result;
}

bool processIsRunning(std::uint64_t process_id) noexcept {
    if (process_id == 0) return false;
#ifdef _WIN32
    UniqueHandle process{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                     static_cast<DWORD>(process_id))};
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
    return WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
#else
    if (::kill(static_cast<pid_t>(process_id), 0) == 0) return true;
    return errno == EPERM;
#endif
}

} // namespace Pelican::DevCli
