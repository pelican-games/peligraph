#include "graphfoldverify.hpp"

#include <stb_image.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Pelican {
namespace {

using OrderedJson = nlohmann::ordered_json;

FoldVerifyVerdict refusal(std::string_view code, std::string message, OrderedJson fields) {
    return {.passed = false, .code = std::string{code}, .message = std::move(message), .fields = std::move(fields)};
}

OrderedJson failedRunDetail(const FoldVerifyRun &run) {
    OrderedJson detail{{"exit_code", run.exit_code}};
    detail["message"] = run.message;
    return detail;
}

std::string readWholeFile(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) return {};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void writeWholeFile(const std::filesystem::path &path, std::string_view bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output.is_open()) throw std::runtime_error("fold verify could not write " + path.string());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("fold verify could not write " + path.string());
}

std::uint64_t countOccurrences(std::string_view text, std::string_view needle) {
    std::uint64_t count = 0;
    for (auto at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}

// The last part of a child's stderr, for a message.
std::string tail(std::string_view text, std::size_t limit = 2000) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.remove_suffix(1);
    if (text.size() > limit) text = text.substr(text.size() - limit);
    return std::string{text};
}

std::optional<FoldVerifyFrame> readFrame(const std::filesystem::path &path) {
    const auto bytes = readWholeFile(path);
    if (bytes.empty()) return std::nullopt;
    int width = 0;
    int height = 0;
    int channels = 0;
    auto *pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()),
                                         static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (pixels == nullptr) return std::nullopt;
    FoldVerifyFrame frame{.width = static_cast<std::uint32_t>(width), .height = static_cast<std::uint32_t>(height)};
    frame.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    stbi_image_free(pixels);
    return frame;
}

std::string pathUtf8(const std::filesystem::path &path) {
    const auto encoded = path.generic_u8string();
    return {encoded.begin(), encoded.end()};
}

std::string decimal(double value) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text.precision(17);
    text << value;
    return text.str();
}

struct ChildResult {
    int exit_code = -1;
    bool timed_out = false;
    bool cancelled = false;
    std::string error;
};

#ifdef _WIN32

class Handle {
    HANDLE handle_ = nullptr;

  public:
    Handle() = default;
    explicit Handle(HANDLE handle) : handle_{handle == INVALID_HANDLE_VALUE ? nullptr : handle} {}
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    ~Handle() { reset(); }
    HANDLE get() const noexcept { return handle_; }
    void reset() noexcept {
        if (handle_ != nullptr) CloseHandle(handle_);
        handle_ = nullptr;
    }
    explicit operator bool() const noexcept { return handle_ != nullptr; }
};

std::wstring quotedArgument(std::wstring_view value) {
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring{value};
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

Handle inheritableFile(const std::filesystem::path &path, bool write) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    return Handle{CreateFileW(path.c_str(), write ? GENERIC_WRITE : GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                              write ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
}

// One child. Its standard handles are files (stdin NUL), and it inherits only
// those three. It runs in a job object of its own that ends everything in it
// when the job's handle is closed: the way the child is stopped when it runs
// too long or the engine shuts down. No other process is ever looked up.
ChildResult runChild(const std::filesystem::path &executable, const std::vector<std::filesystem::path> &arguments,
                     const std::filesystem::path &directory, const std::filesystem::path &stdout_path,
                     const std::filesystem::path &stderr_path, const FoldVerifyProcessOptions &options,
                     const std::atomic<bool> &cancelled) {
    ChildResult result;
    Handle input = inheritableFile(L"NUL", false);
    Handle output = inheritableFile(stdout_path, true);
    Handle error = inheritableFile(stderr_path, true);
    if (!input || !output || !error) {
        result.error = "could not open the child's standard handles (Windows error " +
                       std::to_string(GetLastError()) + ")";
        return result;
    }
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    if (!job) {
        result.error = "could not create a job object (Windows error " + std::to_string(GetLastError()) + ")";
        return result;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        result.error = "could not configure the job object (Windows error " + std::to_string(GetLastError()) + ")";
        return result;
    }

    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<std::byte> attribute_storage(attribute_size);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
        result.error = "could not prepare the child's attributes (Windows error " +
                       std::to_string(GetLastError()) + ")";
        return result;
    }
    HANDLE inherited[] = {input.get(), output.get(), error.get()};
    const bool listed = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                                  sizeof(inherited), nullptr, nullptr) != FALSE;
    if (!listed) {
        DeleteProcThreadAttributeList(attributes);
        result.error = "could not limit the child's handles (Windows error " + std::to_string(GetLastError()) + ")";
        return result;
    }

    std::wstring command = quotedArgument(executable.native());
    for (const auto &argument : arguments) {
        command += L' ';
        command += quotedArgument(argument.native());
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = output.get();
    startup.StartupInfo.hStdError = error.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process_info{};
    constexpr DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
    const bool created = CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE, flags,
                                        nullptr, directory.c_str(), &startup.StartupInfo, &process_info) != FALSE;
    const auto create_error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!created) {
        result.error = "could not start " + pathUtf8(executable) + " (Windows error " +
                       std::to_string(create_error) + ")";
        return result;
    }
    Handle process{process_info.hProcess};
    Handle thread{process_info.hThread};
    // The child's copies are its own now.
    input.reset();
    output.reset();
    error.reset();
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        const auto assign_error = GetLastError();
        // Not started yet (suspended), and not in our job: end it through
        // its own handle and wait for it.
        TerminateProcess(process.get(), 126);
        WaitForSingleObject(process.get(), 10000);
        result.error = "could not put the child in its job object (Windows error " +
                       std::to_string(assign_error) + ")";
        return result;
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        result.error = "could not resume the child (Windows error " + std::to_string(GetLastError()) + ")";
        job.reset();
        WaitForSingleObject(process.get(), 10000);
        return result;
    }
    thread.reset();

    const auto started = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> asked_to_stop;
    while (WaitForSingleObject(process.get(), 100) == WAIT_TIMEOUT) {
        const auto now = std::chrono::steady_clock::now();
        // A headless child without --rpc has no channel to be told to stop:
        // it draws a fixed number of frames and exits. Give it the grace to
        // do so, then close its job.
        if (cancelled.load() && !asked_to_stop) asked_to_stop = now;
        if (asked_to_stop && now - *asked_to_stop >= options.stop_grace) {
            result.cancelled = true;
            break;
        }
        if (now - started >= options.run_timeout) {
            result.timed_out = true;
            break;
        }
    }
    if (result.cancelled || result.timed_out) {
        // KILL_ON_JOB_CLOSE: closing the one handle of the job ends the
        // child and anything it started. Then wait for it.
        job.reset();
        WaitForSingleObject(process.get(), 30000);
    }
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process.get(), &exit_code)) exit_code = 1;
    result.exit_code = static_cast<int>(exit_code);
    return result;
}

#endif

FoldVerifyRun drawOnce(const FoldVerifyJob &job, const FoldVerifyProcessOptions &options,
                       const std::filesystem::path &project, const std::filesystem::path &run_directory,
                       const std::atomic<bool> &cancelled, bool &was_cancelled) {
    FoldVerifyRun run;
    std::error_code error;
    std::filesystem::create_directories(run_directory, error);
    if (error) {
        run.message = "could not create " + pathUtf8(run_directory) + ": " + error.message();
        return run;
    }
    std::vector<std::filesystem::path> arguments{
        "--headless", "--project", project, "--frames", std::to_string(job.frames), "--fps", decimal(options.fps)};
    if (options.width > 0 && options.height > 0) {
        arguments.emplace_back("--size");
        arguments.emplace_back(std::to_string(options.width) + "x" + std::to_string(options.height));
    }
    if (job.warmup_seconds > 0.0) {
        arguments.emplace_back("--start-time");
        arguments.emplace_back(decimal(job.warmup_seconds));
    }
    arguments.emplace_back("--render-out");
    arguments.emplace_back(run_directory / "frame_%d.png");
    arguments.emplace_back("--vulkan-validation");
    arguments.emplace_back("on");
    arguments.emplace_back("--log-file");
    arguments.emplace_back(run_directory / "pelican.log");
    for (const auto &argument : options.extra_arguments) arguments.emplace_back(argument);
    const auto stdout_path = run_directory / "stdout.txt";
    const auto stderr_path = run_directory / "stderr.txt";
#ifdef _WIN32
    const auto child = runChild(options.executable, arguments, run_directory, stdout_path, stderr_path, options,
                                cancelled);
#else
    (void)cancelled;
    const ChildResult child{.error = "verify_fold starts its children on Windows only"};
#endif
    if (child.cancelled) {
        was_cancelled = true;
        run.message = "the engine shut down";
        return run;
    }
    run.exit_code = child.exit_code;
    const auto out = readWholeFile(stdout_path);
    const auto err = readWholeFile(stderr_path);
    run.validation_errors = countOccurrences(out, "Validation Error") + countOccurrences(err, "Validation Error");
    if (!child.error.empty()) {
        run.message = child.error;
        return run;
    }
    if (child.timed_out) {
        run.message = "the run did not end within " + std::to_string(options.run_timeout.count()) + " s";
        return run;
    }
    if (child.exit_code != 0) {
        run.message = tail(err.empty() ? out : err);
        return run;
    }
    for (std::uint32_t frame = 1; frame <= job.frames; ++frame) {
        auto decoded = readFrame(run_directory / ("frame_" + std::to_string(frame) + ".png"));
        if (!decoded) {
            run.message = "frame " + std::to_string(frame) + " was not written";
            run.frames.clear();
            return run;
        }
        run.frames.push_back(std::move(*decoded));
    }
    run.finished = true;
    return run;
}

std::filesystem::path relativeInside(const std::filesystem::path &root, const std::filesystem::path &path) {
    const auto relative = path.lexically_normal().lexically_relative(root.lexically_normal());
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        throw std::runtime_error("fold verify: " + pathUtf8(path) + " is not inside the project");
    return relative;
}

std::filesystem::path uniqueTemporaryDirectory() {
    static std::atomic<std::uint64_t> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const auto process = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const std::uint64_t process = 0;
#endif
    return std::filesystem::temp_directory_path() /
           ("pelican_fold_verify_" + std::to_string(process) + "_" + std::to_string(stamp) + "_" +
            std::to_string(counter.fetch_add(1)));
}

} // namespace

std::uint64_t foldVerifyDifferingPixels(const FoldVerifyFrame &left, const FoldVerifyFrame &right) {
    if (left.width != right.width || left.height != right.height || left.rgba.size() != right.rgba.size()) {
        return std::max(static_cast<std::uint64_t>(left.width) * left.height,
                        static_cast<std::uint64_t>(right.width) * right.height);
    }
    std::uint64_t differing = 0;
    for (std::size_t at = 0; at + 4 <= left.rgba.size(); at += 4) {
        if (!std::equal(left.rgba.begin() + static_cast<std::ptrdiff_t>(at),
                        left.rgba.begin() + static_cast<std::ptrdiff_t>(at + 4),
                        right.rgba.begin() + static_cast<std::ptrdiff_t>(at)))
            ++differing;
    }
    return differing;
}

FoldVerifyVerdict judgeFoldVerifyRuns(const std::string &group, const FoldVerifyRuns &runs,
                                      std::uint64_t tolerance) {
    if (runs.setup_failure) {
        return refusal(foldVerifyFailedCode, "verify_fold could not prepare the copy of the project",
                       {{"group", group}, {"run", "setup"}, {"detail", {{"message", *runs.setup_failure}}}});
    }
    for (const auto *control : {&runs.open_first, &runs.open_second}) {
        if (!control->finished) {
            return refusal(foldVerifyFailedCode, "the open form of the fold group could not be drawn",
                           {{"group", group}, {"run", "open"}, {"detail", failedRunDetail(*control)}});
        }
    }
    const auto controls = std::min(runs.open_first.frames.size(), runs.open_second.frames.size());
    for (std::size_t frame = 0; frame < controls; ++frame) {
        const auto differing = foldVerifyDifferingPixels(runs.open_first.frames[frame], runs.open_second.frames[frame]);
        if (differing > tolerance) {
            return refusal(graphFoldVerifyUnstableCode,
                           "two runs of the open form drew different pictures; the decisions cannot be judged",
                           {{"group", group},
                            {"detail", {{"frame", frame + 1}, {"differing_pixels", differing}}}});
        }
    }
    if (!runs.decided.finished) {
        return refusal(graphFoldMismatchCode, "the form with the fold group's decisions could not be drawn",
                       {{"group", group}, {"what", "compile"}, {"detail", failedRunDetail(runs.decided)}});
    }
    const auto compared = std::min(runs.open_first.frames.size(), runs.decided.frames.size());
    for (std::size_t frame = 0; frame < compared; ++frame) {
        const auto differing = foldVerifyDifferingPixels(runs.open_first.frames[frame], runs.decided.frames[frame]);
        if (differing > tolerance) {
            return refusal(graphFoldMismatchCode,
                           "the form with the fold group's decisions drew a different picture",
                           {{"group", group},
                            {"what", "pixels"},
                            {"detail", {{"frame", frame + 1}, {"differing_pixels", differing}}}});
        }
    }
    const auto control_errors = std::max(runs.open_first.validation_errors, runs.open_second.validation_errors);
    if (runs.decided.validation_errors > control_errors) {
        return refusal(graphFoldMismatchCode,
                       "the form with the fold group's decisions gave Vulkan validation errors",
                       {{"group", group},
                        {"what", "validation"},
                        {"detail",
                         {{"validation_errors", runs.decided.validation_errors},
                          {"control_validation_errors", control_errors}}}});
    }
    return {.passed = true};
}

FoldVerifyRuns runFoldVerifyProcesses(const FoldVerifyJob &job, const FoldVerifyProcessOptions &options,
                                      const std::atomic<bool> &cancelled) {
    FoldVerifyRuns runs;
    std::filesystem::path temporary;
    try {
        temporary = uniqueTemporaryDirectory();
        const auto project = temporary / "project";
        std::filesystem::create_directories(project);
        // The whole project: scenes, assets and .pelican/local.json are read
        // by the run. A top-level .git is not.
        for (const auto &entry : std::filesystem::directory_iterator{job.project_root}) {
            if (entry.path().filename() == ".git") continue;
            std::filesystem::copy(entry.path(), project / entry.path().filename(),
                                  std::filesystem::copy_options::recursive);
        }
        // The documents as the editor holds them at base_source_digest.
        for (const auto &document : job.documents)
            writeWholeFile(project / relativeInside(job.project_root, document.path), document.bytes);
        const auto root = project / relativeInside(job.project_root, job.root_path);

        struct Form {
            FoldVerifyRun *run;
            const std::string *bytes;
            const char *directory;
        };
        const Form forms[] = {{&runs.open_first, &job.open_root_bytes, "open_1"},
                              {&runs.open_second, &job.open_root_bytes, "open_2"},
                              {&runs.decided, &job.decided_root_bytes, "decided"}};
        for (const auto &form : forms) {
            if (cancelled.load()) {
                runs.cancelled = true;
                break;
            }
            writeWholeFile(root, *form.bytes);
            bool was_cancelled = false;
            *form.run = drawOnce(job, options, project, temporary / form.directory, cancelled, was_cancelled);
            if (was_cancelled) {
                runs.cancelled = true;
                break;
            }
        }
    } catch (const std::exception &error) {
        runs.setup_failure = error.what();
    }
    if (!temporary.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(temporary, ignored);
    }
    return runs;
}

std::optional<std::filesystem::path> foldVerifyExecutablePath() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return std::nullopt;
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path{buffer};
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    return std::nullopt;
#endif
}

std::string foldVerifyUtcNow() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

} // namespace Pelican
