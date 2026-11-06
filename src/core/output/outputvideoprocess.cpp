#include "outputvideoprocess.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace Pelican {
namespace {
constexpr std::size_t stderr_tail_size = 2048;

void appendTail(std::string &tail, const char *data, std::size_t size) {
    if (size >= stderr_tail_size) {
        tail.assign(data + size - stderr_tail_size, stderr_tail_size);
        return;
    }
    if (tail.size() + size > stderr_tail_size)
        tail.erase(0, tail.size() + size - stderr_tail_size);
    tail.append(data, size);
}

} // namespace

std::vector<std::string> outputVideoEncoderArguments(
    const OutputVideoEncoderOptions &options) {
    std::vector<std::string> arguments{options.executable.string(),
        "-hide_banner", "-nostdin", "-f", "rawvideo", "-pix_fmt",
        options.bgra ? "bgra" : "rgba", "-video_size",
        std::to_string(options.width) + "x" + std::to_string(options.height),
        "-framerate", std::to_string(options.fps_num) + "/" +
                          std::to_string(options.fps_den),
        "-i", "pipe:0", "-an", "-c:v", "libx264", "-vf",
        "scale=in_range=full:out_range=tv:out_color_matrix=bt709,format=yuv420p",
        "-pix_fmt", "yuv420p", "-color_range", "tv", "-colorspace", "bt709", "-color_primaries",
        "bt709", "-color_trc", "bt709", "-f", "mp4", "-n",
        options.partial_path.string()};
    return arguments;
}

std::string quoteOutputVideoWindowsCommandLine(
    const std::vector<std::string> &arguments) {
    std::string command;
    for (const auto &argument : arguments) {
        if (!command.empty()) command.push_back(' ');
        command.push_back('"');
        std::size_t slashes = 0;
        for (const char c : argument) {
            if (c == '\\') {
                ++slashes;
            } else if (c == '"') {
                command.append(slashes * 2 + 1, '\\');
                command.push_back('"');
                slashes = 0;
            } else {
                command.append(slashes, '\\');
                slashes = 0;
                command.push_back(c);
            }
        }
        command.append(slashes * 2, '\\');
        command.push_back('"');
    }
    return command;
}

std::optional<std::string> outputVideoEncoderVersion(
    const std::filesystem::path &executable, std::chrono::milliseconds timeout) {
    std::string output;
#ifndef _WIN32
    int descriptors[2] = {-1, -1};
    if (::pipe(descriptors) != 0) return std::nullopt;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (::posix_spawn_file_actions_init(&actions) != 0 ||
        ::posix_spawnattr_init(&attributes) != 0) {
        ::close(descriptors[0]);
        ::close(descriptors[1]);
        return std::nullopt;
    }
    (void)::posix_spawnattr_setflags(&attributes,
        POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    sigset_t default_signals;
    sigemptyset(&default_signals);
    sigaddset(&default_signals, SIGPIPE);
    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    (void)::posix_spawnattr_setsigdefault(&attributes, &default_signals);
    (void)::posix_spawnattr_setsigmask(&attributes, &empty_mask);
    (void)::posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
    (void)::posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
    for (const int fd : {descriptors[0], descriptors[1]})
        (void)::posix_spawn_file_actions_addclose(&actions, fd);
    const auto program = executable.string();
    char *const arguments[] = {const_cast<char *>(program.c_str()),
        const_cast<char *>("-version"), nullptr};
    pid_t child = -1;
    const auto spawned = ::posix_spawnp(&child, program.c_str(), &actions,
                                        &attributes, arguments, environ);
    (void)::posix_spawnattr_destroy(&attributes);
    (void)::posix_spawn_file_actions_destroy(&actions);
    ::close(descriptors[1]);
    if (spawned != 0) {
        ::close(descriptors[0]);
        return std::nullopt;
    }
    int status = 0;
    bool exited = false;
    bool eof = false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!exited) {
        if (!eof) {
            pollfd descriptor{descriptors[0], POLLIN, 0};
            const auto ready = ::poll(&descriptor, 1, 10);
            if (ready > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0) {
                std::array<char, 4096> buffer{};
                const auto count = ::read(descriptors[0], buffer.data(), buffer.size());
                if (count > 0) output.append(buffer.data(), static_cast<std::size_t>(count));
                else if (count == 0) eof = true;
            } else if (ready < 0 && errno != EINTR) {
                eof = true;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const auto waited = ::waitpid(child, &status, WNOHANG);
        exited = waited == child;
        if (!exited && std::chrono::steady_clock::now() >= deadline) {
            (void)::kill(child, SIGKILL);
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            exited = true;
        }
    }
    while (!eof) {
        pollfd descriptor{descriptors[0], POLLIN, 0};
        if (::poll(&descriptor, 1, 0) <= 0) break;
        std::array<char, 4096> buffer{};
        const auto count = ::read(descriptors[0], buffer.data(), buffer.size());
        if (count > 0) output.append(buffer.data(), static_cast<std::size_t>(count));
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
    ::close(descriptors[0]);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
#else
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &security, 0)) return std::nullopt;
    (void)SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    PROCESS_INFORMATION information{};
    const auto program = executable.wstring();
    const auto arguments = std::vector<std::string>{executable.string(), "-version"};
    const auto narrow = quoteOutputVideoWindowsCommandLine(arguments);
    const auto size = MultiByteToWideChar(CP_UTF8, 0, narrow.data(),
        static_cast<int>(narrow.size()), nullptr, 0);
    std::wstring command(static_cast<std::size_t>(size), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, 0, narrow.data(),
        static_cast<int>(narrow.size()), command.data(), size);
    const auto created = CreateProcessW(program.c_str(), command.data(), nullptr,
        nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &information);
    CloseHandle(write_end);
    if (!created) {
        CloseHandle(read_end);
        return std::nullopt;
    }
    CloseHandle(information.hThread);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    DWORD wait_result = WAIT_TIMEOUT;
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr) &&
            available != 0) {
            std::array<char, 4096> buffer{};
            DWORD count = 0;
            if (ReadFile(read_end, buffer.data(),
                         static_cast<DWORD>(std::min<std::size_t>(available, buffer.size())),
                         &count, nullptr) && count != 0)
                output.append(buffer.data(), count);
        }
        wait_result = WaitForSingleObject(information.hProcess, 10);
        if (wait_result == WAIT_OBJECT_0) break;
    }
    if (wait_result != WAIT_OBJECT_0) TerminateProcess(information.hProcess, 1);
    (void)WaitForSingleObject(information.hProcess, INFINITE);
    std::array<char, 4096> buffer{};
    DWORD count = 0;
    while (ReadFile(read_end, buffer.data(), static_cast<DWORD>(buffer.size()),
                    &count, nullptr) && count != 0)
        output.append(buffer.data(), count);
    DWORD exit_code = 1;
    const auto succeeded = GetExitCodeProcess(information.hProcess, &exit_code) &&
                           exit_code == 0;
    CloseHandle(read_end);
    CloseHandle(information.hProcess);
    if (!succeeded) return std::nullopt;
#endif
    const auto end = output.find_first_of("\r\n");
    if (end != std::string::npos) output.resize(end);
    if (output.empty()) return std::nullopt;
    return output;
}

std::uint64_t outputVideoPosition(std::int64_t elapsed_nanoseconds,
                                  std::uint64_t fps_num,
                                  std::uint64_t fps_den) {
    if (elapsed_nanoseconds <= 0) return 0;
    if (fps_num == 0 || fps_den == 0)
        throw std::invalid_argument("video rate must be positive");
    constexpr std::uint64_t billion = 1000000000;
    const auto elapsed = static_cast<std::uint64_t>(elapsed_nanoseconds);
    const auto common = std::gcd(fps_num, fps_den);
    fps_num /= common;
    fps_den /= common;
    const auto whole_seconds = elapsed / billion;
    if (whole_seconds > (std::numeric_limits<std::uint64_t>::max)() / fps_num)
        throw std::overflow_error("video position overflow");
    const auto whole = whole_seconds * fps_num;
    const auto denominator = fps_den * billion;
    const auto fraction = whole % fps_den * billion +
                          elapsed % billion * fps_num;
    return whole / fps_den + (fraction + denominator / 2) / denominator;
}

std::optional<std::filesystem::path> findOutputVideoEncoder(
    const std::optional<std::filesystem::path> &configured) {
    namespace fs = std::filesystem;
    const auto usable = [](const fs::path &path) {
        std::error_code error;
        return fs::is_regular_file(path, error);
    };
    if (configured) return usable(*configured) ? configured : std::nullopt;
    const char *path_value = std::getenv("PATH");
    if (path_value == nullptr) return std::nullopt;
#ifdef _WIN32
    constexpr char separator = ';';
    const std::array<const char *, 2> names{"ffmpeg.exe", "ffmpeg"};
#else
    constexpr char separator = ':';
    const std::array<const char *, 1> names{"ffmpeg"};
#endif
    std::string paths{path_value};
    std::size_t begin = 0;
    while (begin <= paths.size()) {
        const auto end = paths.find(separator, begin);
        const auto directory = paths.substr(begin, end - begin);
        for (const auto *name : names) {
            auto candidate = fs::path{directory} / name;
            if (usable(candidate)) return fs::absolute(candidate);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return std::nullopt;
}

struct OutputVideoEncoderProcess::Impl {
    mutable std::mutex stderr_mutex;
    std::string stderr_tail;
    std::thread stderr_thread;
    std::atomic<bool> stderr_cancel{false};
    std::atomic<bool> running{false};
    std::atomic<bool> exited{false};
#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE input = nullptr;
    HANDLE stderr_read = nullptr;
    DWORD process_id = 0;
#else
    std::atomic<pid_t> child{-1};
    int input = -1;
    int stderr_read = -1;
    std::mutex wait_mutex;
    std::optional<int> exit_code;
#endif

    ~Impl() {
        terminate();
        closeInput();
#ifdef _WIN32
        if (process != nullptr) WaitForSingleObject(process, 1000);
#else
        wait(std::chrono::milliseconds(1000));
#endif
        closeStderr();
    }
    bool start(const OutputVideoEncoderOptions &options, std::string &error);
    bool write(const std::uint8_t *data, std::size_t size, std::string &error);
    void closeInput();
    void closeStderr();
    std::optional<int> wait(std::chrono::milliseconds timeout);
    void terminate();
};

OutputVideoEncoderProcess::OutputVideoEncoderProcess()
    : impl_(std::make_unique<Impl>()) {}
OutputVideoEncoderProcess::~OutputVideoEncoderProcess() = default;

#ifndef _WIN32
void OutputVideoEncoderProcess::Impl::closeInput() {
    if (input < 0) return;
    ::close(input);
    input = -1;
}

void OutputVideoEncoderProcess::Impl::closeStderr() {
    stderr_cancel = true;
    if (stderr_thread.joinable()) stderr_thread.join();
    if (stderr_read >= 0) {
        ::close(stderr_read);
        stderr_read = -1;
    }
}

std::optional<int> OutputVideoEncoderProcess::Impl::wait(
    std::chrono::milliseconds timeout) {
    std::lock_guard lock{wait_mutex};
    if (exit_code) return exit_code;
    const auto target = child.load();
    if (target < 0) return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        int status = 0;
        const auto result = ::waitpid(target, &status, WNOHANG);
        if (result == target) {
            running = false;
            exited = true;
            if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);
            else exit_code = -1;
            child = -1;
            return exit_code;
        }
        if (result < 0) {
            if (errno == EINTR) continue;
            running = false;
            return std::nullopt;
        }
        if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void OutputVideoEncoderProcess::Impl::terminate() {
    const auto target = child.load();
    if (target > 0) {
        running = false;
        (void)::kill(target, SIGKILL);
    }
}

bool OutputVideoEncoderProcess::Impl::start(const OutputVideoEncoderOptions &options,
                                      std::string &error) {
    const auto arguments = outputVideoEncoderArguments(options);
    int input_pipe[2] = {-1, -1};
    int error_pipe[2] = {-1, -1};
    if (::pipe(input_pipe) != 0 || ::pipe(error_pipe) != 0) {
        error = std::strerror(errno);
        if (input_pipe[0] >= 0) { ::close(input_pipe[0]); ::close(input_pipe[1]); }
        if (error_pipe[0] >= 0) { ::close(error_pipe[0]); ::close(error_pipe[1]); }
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    (void)::posix_spawn_file_actions_init(&actions);
    (void)::posix_spawnattr_init(&attributes);
    (void)::posix_spawnattr_setflags(&attributes,
        POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    (void)::posix_spawn_file_actions_adddup2(&actions, input_pipe[0], STDIN_FILENO);
    (void)::posix_spawn_file_actions_adddup2(&actions, error_pipe[1], STDOUT_FILENO);
    (void)::posix_spawn_file_actions_adddup2(&actions, error_pipe[1], STDERR_FILENO);
    for (const int fd : {input_pipe[0], input_pipe[1], error_pipe[0], error_pipe[1]})
        (void)::posix_spawn_file_actions_addclose(&actions, fd);
    sigset_t default_signals;
    sigemptyset(&default_signals);
    sigaddset(&default_signals, SIGPIPE);
    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    (void)::posix_spawnattr_setsigdefault(&attributes, &default_signals);
    (void)::posix_spawnattr_setsigmask(&attributes, &empty_mask);
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments)
        argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t spawned_child = -1;
    const auto result = ::posix_spawnp(&spawned_child, arguments.front().c_str(), &actions,
                                       &attributes, argv.data(), environ);
    child = spawned_child;
    (void)::posix_spawnattr_destroy(&attributes);
    (void)::posix_spawn_file_actions_destroy(&actions);
    ::close(input_pipe[0]);
    ::close(error_pipe[1]);
    if (result != 0) {
        error = std::strerror(result);
        ::close(input_pipe[1]);
        ::close(error_pipe[0]);
        child = -1;
        return false;
    }
    input = input_pipe[1];
    stderr_read = error_pipe[0];
    running = true;
    exited = false;
    exit_code.reset();
    stderr_cancel = false;
    stderr_thread = std::thread([this] {
        std::array<char, 4096> buffer{};
        while (!stderr_cancel) {
            pollfd descriptor{stderr_read, POLLIN, 0};
            const auto ready = ::poll(&descriptor, 1, 20);
            if (ready == 0 || (ready < 0 && errno == EINTR)) continue;
            if (ready < 0) break;
            const auto count = ::read(stderr_read, buffer.data(), buffer.size());
            if (count > 0) {
                std::lock_guard lock{stderr_mutex};
                appendTail(stderr_tail, buffer.data(), static_cast<std::size_t>(count));
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            break;
        }
    });
    return true;
}

bool OutputVideoEncoderProcess::Impl::write(const std::uint8_t *data, std::size_t size,
                                      std::string &error) {
    while (size != 0) {
        const auto count = ::write(input, data, size);
        if (count > 0) {
            data += count;
            size -= static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        error = count < 0 ? std::strerror(errno) : "short pipe write";
        return false;
    }
    return true;
}

std::optional<int> OutputVideoEncoderProcess::wait(
    std::chrono::milliseconds timeout) {
    return impl_->wait(timeout);
}
void OutputVideoEncoderProcess::terminate() { impl_->terminate(); }
void OutputVideoEncoderProcess::closeInput() { impl_->closeInput(); }
bool OutputVideoEncoderProcess::running() const { return impl_->running; }
std::uint64_t OutputVideoEncoderProcess::pid() const {
    const auto child = impl_->child.load();
    return child > 0 ? static_cast<std::uint64_t>(child) : 0;
}
std::string OutputVideoEncoderProcess::stderrTail() const {
    std::lock_guard lock{impl_->stderr_mutex};
    return impl_->stderr_tail;
}
#endif

#ifdef _WIN32
namespace {
std::wstring outputVideoWide(std::string_view text) {
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), result.data(), size);
    return result;
}
}

void OutputVideoEncoderProcess::Impl::closeInput() {
    if (input != nullptr) CloseHandle(input);
    input = nullptr;
}
void OutputVideoEncoderProcess::Impl::closeStderr() {
    if (stderr_read != nullptr) (void)CancelIoEx(stderr_read, nullptr);
    if (stderr_thread.joinable()) stderr_thread.join();
    if (stderr_read != nullptr) CloseHandle(stderr_read);
    stderr_read = nullptr;
    if (process != nullptr) CloseHandle(process);
    process = nullptr;
    if (job != nullptr) CloseHandle(job);
    job = nullptr;
}
std::optional<int> OutputVideoEncoderProcess::Impl::wait(
    std::chrono::milliseconds timeout) {
    if (process == nullptr) return std::nullopt;
    const auto millis = timeout.count() < 0 ? INFINITE :
        static_cast<DWORD>(std::min<std::int64_t>(timeout.count(), INFINITE - 1));
    if (WaitForSingleObject(process, millis) != WAIT_OBJECT_0) return std::nullopt;
    DWORD code = 1;
    if (!GetExitCodeProcess(process, &code)) return std::nullopt;
    running = false;
    exited = true;
    return static_cast<int>(code);
}
void OutputVideoEncoderProcess::Impl::terminate() {
    running = false;
    if (job != nullptr) (void)TerminateJobObject(job, 1);
    else if (process != nullptr) (void)TerminateProcess(process, 1);
}

bool OutputVideoEncoderProcess::Impl::start(const OutputVideoEncoderOptions &options,
                                      std::string &error) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE input_read = nullptr;
    HANDLE error_write = nullptr;
    if (!CreatePipe(&input_read, &input, &security, 0) ||
        !CreatePipe(&stderr_read, &error_write, &security, 0)) {
        error = "CreatePipe failed";
        return false;
    }
    (void)SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0);
    (void)SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input_read;
    startup.StartupInfo.hStdOutput = error_write;
    startup.StartupInfo.hStdError = error_write;
    SIZE_T attribute_bytes = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<std::uint8_t> attributes(attribute_bytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attributes.data());
    HANDLE inherited[] = {input_read, error_write};
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                           &attribute_bytes) ||
        !UpdateProcThreadAttribute(startup.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
            nullptr, nullptr)) {
        error = "procedure attribute setup failed";
        closeInput();
        CloseHandle(input_read);
        CloseHandle(error_write);
        closeStderr();
        return false;
    }
    const auto arguments = outputVideoEncoderArguments(options);
    auto executable = options.executable.wstring();
    auto command = outputVideoWide(quoteOutputVideoWindowsCommandLine(arguments));
    PROCESS_INFORMATION information{};
    const auto created = CreateProcessW(executable.c_str(), command.data(), nullptr,
        nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        nullptr, nullptr, &startup.StartupInfo, &information);
    CloseHandle(input_read);
    CloseHandle(error_write);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) {
        error = "CreateProcessW failed: " + std::to_string(GetLastError());
        closeInput();
        closeStderr();
        return false;
    }
    process = information.hProcess;
    process_id = information.dwProcessId;
    job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job == nullptr || !SetInformationJobObject(job,
            JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, process)) {
        error = "encoder job assignment failed: " + std::to_string(GetLastError());
        TerminateProcess(process, 1);
        CloseHandle(information.hThread);
        closeInput();
        closeStderr();
        return false;
    }
    if (ResumeThread(information.hThread) == static_cast<DWORD>(-1)) {
        error = "ResumeThread failed: " + std::to_string(GetLastError());
        CloseHandle(information.hThread);
        closeInput();
        closeStderr();
        return false;
    }
    CloseHandle(information.hThread);
    running = true;
    stderr_thread = std::thread([this] {
        std::array<char, 4096> buffer{};
        while (true) {
            DWORD count = 0;
            if (!ReadFile(stderr_read, buffer.data(),
                          static_cast<DWORD>(buffer.size()), &count, nullptr) ||
                count == 0)
                break;
            std::lock_guard lock{stderr_mutex};
            appendTail(stderr_tail, buffer.data(), count);
        }
    });
    return true;
}

bool OutputVideoEncoderProcess::Impl::write(const std::uint8_t *data, std::size_t size,
                                      std::string &error) {
    while (size != 0) {
        DWORD written = 0;
        const auto amount = static_cast<DWORD>(std::min<std::size_t>(size,
            (std::numeric_limits<DWORD>::max)()));
        if (!WriteFile(input, data, amount, &written, nullptr)) {
            error = "WriteFile failed: " + std::to_string(GetLastError());
            return false;
        }
        if (written == 0) {
            error = "short pipe write";
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

std::optional<int> OutputVideoEncoderProcess::wait(
    std::chrono::milliseconds timeout) { return impl_->wait(timeout); }
void OutputVideoEncoderProcess::terminate() { impl_->terminate(); }
void OutputVideoEncoderProcess::closeInput() { impl_->closeInput(); }
bool OutputVideoEncoderProcess::running() const { return impl_->running; }
std::uint64_t OutputVideoEncoderProcess::pid() const { return impl_->process_id; }
std::string OutputVideoEncoderProcess::stderrTail() const {
    std::lock_guard lock{impl_->stderr_mutex};
    return impl_->stderr_tail;
}
#endif

bool OutputVideoEncoderProcess::start(const OutputVideoEncoderOptions &options,
                                      std::string &error) {
    return impl_->start(options, error);
}
bool OutputVideoEncoderProcess::write(const std::uint8_t *data, std::size_t size,
                                      std::string &error) {
    return impl_->write(data, size, error);
}

} // namespace Pelican
