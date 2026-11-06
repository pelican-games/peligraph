// VO-1a: output.file through a real headless player. A 4x4 UNORM render
// target is filled by a fullscreen pass so the raw pixels, the row order, the
// file numbering and the frame the pixels came from are all visible without a
// capture transform. The player is driven over line-delimited JSON-RPC.
#include "../src/project/projecttemplate.hpp"
#if PELICAN_WITH_SPOUT
#include "spout_test_receiver.hpp"
#endif

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

constexpr const char *root_reference = "passes/main_rendering_config.json";
constexpr const char *output_prefix = "shot";

std::string readFile(const fs::path &path) {
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

void writeFile(const fs::path &path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << text;
}

Json sceneColorType() {
    return {{"semantic", "pelican.render.color_signal@1"},
            {"arguments",
             {{"reference", "scene"},
              {"transfer", "linear"},
              {"range", "extended"}}}};
}

struct TempProject {
    fs::path root;

    explicit TempProject(std::string_view name) {
        root = fs::temp_directory_path() /
               ("pelican_vo1a_" + std::string{name} + "_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        (void)Pelican::initializeProjectTemplate(
            root, Pelican::ProjectTemplate::Blank);
    }
    ~TempProject() {
        if (std::getenv("PELICAN_VO1A_KEEP") != nullptr) return;
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    fs::path rootPath() const { return root / root_reference; }
    fs::path logPath(std::string_view name) const {
        return root / ("vo1a." + std::string{name} + ".log");
    }
    fs::path outputPath() const {
        const auto path = root / "output";
        fs::create_directories(path);
        return path;
    }
    Json readRoot() const { return Json::parse(readFile(rootPath())); }
    void writeRoot(const Json &document) const {
        writeFile(rootPath(), document.dump(2) + "\n");
    }
    void writeShader() const {
        writeFile(root / "shaders" / "vo1a_frame.frag", R"glsl(#version 460
#extension GL_GOOGLE_include_directive : enable
#include "pelican_sets.glsl"
#include "pelican_view.glsl"
#include "pelican_frame.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_VARIADIC_INPUTS(0, 8);

void main() {
    uint frame = pelicanFrame.frame_index.x & 255u;
    // Alpha varies with both axes, so a writer that drops or reorders the
    // fourth channel is visible in the raw file.
    outColor = vec4(inUV.x, inUV.y, float(frame) / 255.0,
                    0.25 + 0.25 * (inUV.x + inUV.y));
}
)glsl");
    }
    void writeExactPatternShader() const {
        writeFile(root / "shaders" / "vo1a_frame.frag", R"glsl(#version 460
#extension GL_GOOGLE_include_directive : enable
#include "pelican_sets.glsl"
#include "pelican_view.glsl"
#include "pelican_frame.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_VARIADIC_INPUTS(0, 8);

void main() {
    // The four UV cells are exactly 0, 1, 2 and 3 after the scale. Dividing
    // the byte values by 255 makes the UNORM result exact, so the expected
    // PNG can be computed on the CPU without guessing a rounding rule.
    int x = int(inUV.x * 4.0);
    int y = int(inUV.y * 4.0);
    outColor = vec4(float(10 + x) / 255.0,
                    float(20 + y) / 255.0,
                    float(30 + x * y) / 255.0,
                    float(40 + x + y) / 255.0);
}
)glsl");
    }
    void writeConstantBlueShader() const {
        writeFile(root / "shaders" / "vo1a_frame.frag", R"glsl(#version 460
#extension GL_GOOGLE_include_directive : enable
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_VARIADIC_INPUTS(0, 8);

void main() {
    outColor = vec4(inUV.x, inUV.y, 0.25, 1.0);
}
)glsl");
    }
};

Json targetNode(std::string id, std::string format) {
    return {{"id", std::move(id)},
            {"op", "render.image"},
            {"parameters",
             {{"type", sceneColorType()},
              {"format", std::move(format)},
              {"width", 4},
              {"height", 4},
              {"extent_scale", 1.0},
              {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}},
            {"outputs", Json::array({"resource"})}};
}

Json fillNode(std::string id, std::string target_id,
              std::string input_node = {}, std::string input_port = "resource") {
    (void)target_id;
    Json inputs = Json::object();
    if (!input_node.empty()) {
        inputs["color"] = {{"node", std::move(input_node)},
                           {"port", std::move(input_port)}};
    }
    return {{"id", std::move(id)},
            {"op", "render.fullscreen"},
            {"parameters",
             {{"pass_set", "main_render"},
              {"shader",
               {{"vertex", "engine://fullscreen"},
                {"fragment", "project://shaders/vo1a_frame"}}},
              {"ports",
               {{"color",
                 {{"kind", "image"},
                  {"type", sceneColorType()},
                  {"access", "write"},
                  {"attachment", "color"},
                  {"location", 0}}}}}}},
            {"inputs", std::move(inputs)},
            {"outputs", Json::array({"color"})}};
}

Json sinkNode(std::string name, std::string source = "fill",
              std::string port = "color", std::uint64_t every_n = 1,
              bool enabled = true, std::string format = "raw",
              std::string when_full = {}) {
    Json node{{"id", "sink"},
            {"op", "output.file"},
            {"parameters",
             {{"name", std::move(name)},
              {"every_n", every_n},
              {"enabled", enabled},
              {"format", std::move(format)}}},
            {"inputs",
             {{"source", {{"node", std::move(source)}, {"port", std::move(port)}}}}},
            {"outputs", Json::array()}};
    if (!when_full.empty()) node["parameters"]["when_full"] = std::move(when_full);
    return node;
}

Json videoSink(std::string name, std::string mode = "offline") {
    return {{"id", "sink"},
            {"op", "output.video"},
            {"parameters",
             {{"name", std::move(name)}, {"fps_num", 60},
              {"fps_den", 1}, {"mode", std::move(mode)}}},
            {"inputs",
             {{"source", {{"node", "fill"}, {"port", "color"}}}}},
            {"outputs", Json::array()}};
}

void attachMainGraph(Json &root, Json target, Json fill, Json sink) {
    auto &nodes = root["nodes"];
    nodes.push_back(std::move(target));
    nodes.push_back(std::move(fill));
    nodes.push_back(std::move(sink));
}

struct FrameFile {
    fs::path path;
    std::uint64_t frame = 0;
    std::vector<std::uint8_t> bytes;

    std::uint32_t le32(std::size_t at) const {
        REQUIRE(bytes.size() >= at + 4);
        return static_cast<std::uint32_t>(bytes[at]) |
               (static_cast<std::uint32_t>(bytes[at + 1]) << 8u) |
               (static_cast<std::uint32_t>(bytes[at + 2]) << 16u) |
               (static_cast<std::uint32_t>(bytes[at + 3]) << 24u);
    }
    std::uint32_t width() const { return le32(4); }
    std::uint32_t height() const { return le32(8); }
    std::uint32_t format() const { return le32(12); }
    const std::uint8_t *pixel(std::uint32_t x, std::uint32_t y) const {
        return bytes.data() + 16 + (static_cast<std::size_t>(y) * width() + x) * 4;
    }
};

std::optional<std::uint64_t> fileFrame(
    const fs::path &path, std::string_view extension = ".rgba") {
    const auto name = path.filename().string();
    if (!name.starts_with(output_prefix)) return std::nullopt;
    if (name.size() <= extension.size() ||
        name.substr(name.size() - extension.size()) != extension)
        return std::nullopt;
    const auto underscore = name.rfind('_');
    if (underscore == std::string::npos) return std::nullopt;
    const auto digits = name.substr(
        underscore + 1,
        name.size() - underscore - extension.size() - 1);
    if (digits.size() != 6) return std::nullopt;
    if (digits.find_first_not_of("0123456789") != std::string::npos)
        return std::nullopt;
    return static_cast<std::uint64_t>(std::stoull(digits));
}

std::vector<FrameFile> frameFiles(const fs::path &directory,
                                  std::string_view extension = ".rgba") {
    std::vector<fs::path> paths;
    std::error_code ignored;
    for (const auto &entry : fs::directory_iterator{directory, ignored}) {
        if (!entry.is_regular_file()) continue;
        if (fileFrame(entry.path(), extension)) paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    std::vector<FrameFile> files;
    for (const auto &path : paths) {
        const auto frame = fileFrame(path, extension);
        REQUIRE(frame);
        FrameFile file;
        file.path = path;
        file.frame = *frame;
        const auto bytes = readFile(path);
        file.bytes.assign(bytes.begin(), bytes.end());
        files.push_back(std::move(file));
    }
    return files;
}

struct PngFile {
    fs::path path;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    const std::uint8_t *pixel(std::uint32_t x, std::uint32_t y) const {
        return pixels.data() +
               (static_cast<std::size_t>(y) * width + x) * 4;
    }
};

PngFile decodePng(const fs::path &path) {
    const auto bytes = readFile(path);
    REQUIRE(bytes.size() <= static_cast<std::size_t>(
                                (std::numeric_limits<int>::max)()));
    int width = 0;
    int height = 0;
    int channels = 0;
    auto *pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc *>(bytes.data()),
        static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    REQUIRE(pixels != nullptr);
    PngFile png;
    png.path = path;
    png.width = width;
    png.height = height;
    png.pixels.assign(pixels, pixels + static_cast<std::size_t>(width) *
                                            height * 4);
    stbi_image_free(pixels);
    return png;
}

bool pngHasChunk(const std::vector<std::uint8_t> &bytes,
                 std::string_view type) {
    if (bytes.size() < 8) return false;
    std::size_t at = 8;
    while (at + 12 <= bytes.size()) {
        const auto length = (static_cast<std::uint32_t>(bytes[at]) << 24) |
                            (static_cast<std::uint32_t>(bytes[at + 1]) << 16) |
                            (static_cast<std::uint32_t>(bytes[at + 2]) << 8) |
                            static_cast<std::uint32_t>(bytes[at + 3]);
        if (at + 12 + length > bytes.size()) return false;
        if (std::string_view{
                reinterpret_cast<const char *>(bytes.data() + at + 4), 4} ==
            type)
            return true;
        at += 12 + length;
    }
    return false;
}

std::uint8_t pngColorType(const std::vector<std::uint8_t> &bytes) {
    REQUIRE(bytes.size() >= 26);
    REQUIRE(bytes.at(12) == 'I');
    REQUIRE(bytes.at(13) == 'H');
    REQUIRE(bytes.at(14) == 'D');
    REQUIRE(bytes.at(15) == 'R');
    return bytes.at(25);
}

std::vector<std::uint8_t> expectedPattern() {
    std::vector<std::uint8_t> pixels(4 * 4 * 4);
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const auto at = (y * 4 + x) * 4;
            pixels[at + 0] = static_cast<std::uint8_t>(10 + x);
            pixels[at + 1] = static_cast<std::uint8_t>(20 + y);
            pixels[at + 2] = static_cast<std::uint8_t>(30 + x * y);
            pixels[at + 3] = static_cast<std::uint8_t>(40 + x + y);
        }
    }
    return pixels;
}

std::uint64_t frameNumber(const fs::path &path) {
    const auto frame = fileFrame(path);
    REQUIRE(frame);
    return *frame;
}

std::vector<fs::path> allFilesWithSuffix(const fs::path &root,
                                         std::string_view suffix) {
    std::vector<fs::path> matches;
    std::error_code ignored;
    for (const auto &entry : fs::recursive_directory_iterator{root, ignored}) {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        if (name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            matches.push_back(entry.path());
    }
    return matches;
}

void requirePng(const PngFile &png, std::uint32_t width = 4,
                std::uint32_t height = 4) {
    INFO(png.path.string());
    CHECK(png.width == static_cast<int>(width));
    CHECK(png.height == static_cast<int>(height));
    CHECK(png.pixels.size() ==
          static_cast<std::size_t>(width) * height * 4);
    CHECK_FALSE(fs::exists(png.path.string() + ".partial"));
    const auto bytes = readFile(png.path);
    CHECK(pngColorType(std::vector<std::uint8_t>(bytes.begin(), bytes.end())) ==
          6);
    const std::vector<std::uint8_t> chunks(bytes.begin(), bytes.end());
    CHECK_FALSE(pngHasChunk(chunks, "sRGB"));
    CHECK_FALSE(pngHasChunk(chunks, "gAMA"));
}

void requirePvo1(const FrameFile &file, std::uint32_t format,
                 std::uint32_t width = 4, std::uint32_t height = 4) {
    INFO(file.path.string());
    REQUIRE(file.bytes.size() == 16 + static_cast<std::size_t>(width) * height * 4);
    CHECK(file.bytes.at(0) == 'P');
    CHECK(file.bytes.at(1) == 'V');
    CHECK(file.bytes.at(2) == 'O');
    CHECK(file.bytes.at(3) == '1');
    CHECK(file.width() == width);
    CHECK(file.height() == height);
    CHECK(file.format() == format);
    CHECK_FALSE(fs::exists(file.path.string() + ".partial"));
}

#ifdef _WIN32
std::wstring quoteWindowsArgument(const std::wstring &argument) {
    if (argument.empty()) return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}
#endif

// Drives the real pelican_player over stdin/stdout JSON-RPC.  Both platform
// branches use a blocking reader thread; the parent keeps only the pipe ends
// it needs, closes stdin to ask for a clean exit, and terminates a child that
// does not exit in time.  CreateProcessW receives a quoted command line so a
// project or output directory containing spaces still reaches the player as
// one argument.
class PlayerRpc {
  public:
    enum class Display : std::uint8_t { headless, windowed };

    PlayerRpc(const TempProject &project, std::vector<std::string> extra,
              std::string_view name, Display display = Display::headless) {
        log_ = project.logPath(name);
        if (std::getenv("PELICAN_VO1A_VULKAN_VALIDATION") != nullptr) {
            extra.emplace_back("--vulkan-validation");
            extra.emplace_back("on");
        }
#ifdef _WIN32
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        REQUIRE(::CreatePipe(&stdin_read_, &stdin_write_, &attributes, 0));
        REQUIRE(::CreatePipe(&stdout_read_, &stdout_write_, &attributes, 0));
        REQUIRE(::SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0));
        REQUIRE(::SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0));
        log_handle_ =
            ::CreateFileW(log_.wstring().c_str(), GENERIC_WRITE,
                          FILE_SHARE_READ, &attributes, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(log_handle_ != INVALID_HANDLE_VALUE);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdin_read_;
        startup.hStdOutput = stdout_write_;
        startup.hStdError = log_handle_;

        std::vector<std::string> arguments{
            PELICAN_VO1A_PLAYER, "--project", project.root.string(),
            "--rpc",             "--headless", "--size", "64x64",
            "--log-file",        log_.string()};
        if (display == Display::windowed) {
            arguments.erase(std::find(arguments.begin(), arguments.end(), "--headless"));
        }
        for (auto &argument : extra) arguments.push_back(std::move(argument));
        std::wstring command;
        for (const auto &argument : arguments) {
            if (!command.empty()) command.push_back(L' ');
            command += quoteWindowsArgument(fs::path{argument}.wstring());
        }
        REQUIRE(::CreateProcessW(nullptr, command.data(), nullptr, nullptr,
                                 TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                 &startup, &process_));
        ::CloseHandle(stdin_read_);
        stdin_read_ = INVALID_HANDLE_VALUE;
        ::CloseHandle(stdout_write_);
        stdout_write_ = INVALID_HANDLE_VALUE;
#else
        int input[2]{};
        int output[2]{};
        REQUIRE(::pipe(input) == 0);
        REQUIRE(::pipe(output) == 0);
        process_ = ::fork();
        REQUIRE(process_ >= 0);
        if (process_ == 0) {
            (void)::dup2(input[0], STDIN_FILENO);
            (void)::dup2(output[1], STDOUT_FILENO);
            const int error_fd =
                ::open(log_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (error_fd >= 0) (void)::dup2(error_fd, STDERR_FILENO);
            (void)::close(input[0]);
            (void)::close(input[1]);
            (void)::close(output[0]);
            (void)::close(output[1]);
            if (error_fd >= 0) (void)::close(error_fd);
            const fs::path player{PELICAN_VO1A_PLAYER};
            const auto working = player.parent_path();
            if (!working.empty()) (void)::chdir(working.c_str());
            std::vector<std::string> arguments{
                player.string(), "--project", project.root.string(),
                "--rpc",         "--headless", "--size", "64x64",
                "--log-file",    log_.string()};
            if (display == Display::windowed) {
                arguments.erase(std::find(arguments.begin(), arguments.end(), "--headless"));
            }
            for (auto &argument : extra) arguments.push_back(std::move(argument));
            std::vector<char *> argv;
            argv.reserve(arguments.size() + 1);
            for (auto &argument : arguments) argv.push_back(argument.data());
            argv.push_back(nullptr);
            ::execv(argv.front(), argv.data());
            ::_exit(127);
        }
        (void)::close(input[0]);
        (void)::close(output[1]);
        input_ = input[1];
        output_ = output[0];
#endif
        reader_ = std::thread([this] { readLoop(); });
    }
    ~PlayerRpc() { close(); }

    PlayerRpc(const PlayerRpc &) = delete;
    PlayerRpc &operator=(const PlayerRpc &) = delete;

    Json result(std::string_view method,
                const Json &params = Json::object()) {
        const int id = next_id_++;
        Json request{{"jsonrpc", "2.0"},
                     {"id", id},
                     {"method", std::string{method}},
                     {"params", params}};
        writeAll(request.dump() + "\n");
        CAPTURE(std::string{method});
        const auto response = nextLine();
        INFO(response.dump());
        REQUIRE(response.contains("id"));
        CHECK(response.at("id") == id);
        if (response.contains("error")) {
            FAIL("JSON-RPC " << method
                             << " failed: " << response.at("error").dump());
        }
        REQUIRE(response.contains("result"));
        return response.at("result");
    }

    Json outputStatus() {
        const auto status = result("get_status");
        REQUIRE(status.contains("output"));
        return status.at("output");
    }

    // A render edit or reload is a ticket; the editor settles it at a frame
    // boundary, which a headless player reaches by stepping.
    Json settle(const Json &accepted) {
        INFO(accepted.dump());
        if (accepted.value("status", std::string{}) == "rejected")
            return accepted;
        REQUIRE(accepted.at("status") == "accepted");
        for (int attempt = 0; attempt < 32; ++attempt) {
            (void)result("step_frame");
            auto settled = result("get_edit_result",
                                  {{"ticket", accepted.at("ticket")}});
            if (settled.value("status", std::string{}) != "pending")
                return settled;
        }
        FAIL("the edit ticket stayed pending");
        return Json{};
    }

    void close() {
        if (close_started_) return;
        close_started_ = true;
        closed_ = true;
#ifdef _WIN32
        if (stdin_write_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(stdin_write_);
            stdin_write_ = INVALID_HANDLE_VALUE;
        }
        if (process_.hProcess != nullptr) {
            if (::WaitForSingleObject(process_.hProcess, 10000) ==
                WAIT_TIMEOUT) {
                (void)::TerminateProcess(process_.hProcess, 1);
                (void)::WaitForSingleObject(process_.hProcess, 5000);
            }
            DWORD exit_code = 0;
            if (::GetExitCodeProcess(process_.hProcess, &exit_code))
                exit_code_ = static_cast<int>(exit_code);
        }
        if (reader_.joinable()) reader_.join();
        if (stdout_read_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(stdout_read_);
            stdout_read_ = INVALID_HANDLE_VALUE;
        }
        if (process_.hThread != nullptr) {
            ::CloseHandle(process_.hThread);
            process_.hThread = nullptr;
        }
        if (process_.hProcess != nullptr) {
            ::CloseHandle(process_.hProcess);
            process_.hProcess = nullptr;
        }
        if (log_handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(log_handle_);
            log_handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (input_ >= 0) {
            (void)::close(input_);
            input_ = -1;
        }
        if (process_ > 0) {
            int status = 0;
            bool reaped = false;
            for (int attempt = 0; attempt < 100; ++attempt) {
                const auto waited = ::waitpid(process_, &status, WNOHANG);
                if (waited == process_) {
                    reaped = true;
                    break;
                }
                if (waited < 0 && errno != EINTR) break;
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
            if (!reaped) {
                (void)::kill(process_, SIGTERM);
                for (int attempt = 0; attempt < 50; ++attempt) {
                    if (::waitpid(process_, &status, WNOHANG) == process_) {
                        reaped = true;
                        break;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds{100});
                }
            }
            if (!reaped) {
                (void)::kill(process_, SIGKILL);
                (void)::waitpid(process_, &status, 0);
                reaped = true;
            }
            if (reaped) {
                if (WIFEXITED(status))
                    exit_code_ = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    exit_code_ = 128 + WTERMSIG(status);
            }
            process_ = -1;
        }
        if (reader_.joinable()) reader_.join();
        if (output_ >= 0) {
            (void)::close(output_);
            output_ = -1;
        }
#endif
        if (!diagnostics_.empty()) {
            std::ofstream log{log_, std::ios::binary | std::ios::app};
            log << diagnostics_;
        }
    }

    int exitCode() const { return exit_code_; }

  private:
    void writeAll(const std::string &line) {
        std::size_t offset = 0;
        while (offset < line.size()) {
#ifdef _WIN32
            DWORD written = 0;
            const auto chunk = static_cast<DWORD>(line.size() - offset);
            if (!::WriteFile(stdin_write_, line.data() + offset, chunk,
                             &written, nullptr) ||
                written == 0) {
                FAIL("cannot write to the player");
            }
#else
            const auto written = ::write(input_, line.data() + offset,
                                         line.size() - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                FAIL("cannot write to the player: " << std::strerror(errno));
            }
#endif
            offset += static_cast<std::size_t>(written);
        }
    }

    Json nextLine() {
        std::unique_lock lock{mutex_};
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{120};
        if (!arrived_.wait_until(lock, deadline, [this] {
                return !lines_.empty() || closed_;
            })) {
            lock.unlock();
            FAIL("the player did not answer within 120s; see "
                 << log_.string());
        }
        if (lines_.empty()) {
            lock.unlock();
            FAIL("the player exited before answering; see " << log_.string());
        }
        auto line = std::move(lines_.front());
        lines_.pop_front();
        return Json::parse(line);
    }

    void readLoop() {
        std::string pending;
        char buffer[4096];
        for (;;) {
#ifdef _WIN32
            DWORD read_bytes = 0;
            if (!::ReadFile(stdout_read_, buffer, sizeof buffer, &read_bytes,
                            nullptr) ||
                read_bytes == 0)
                break;
#else
            const auto read_bytes = ::read(output_, buffer, sizeof buffer);
            if (read_bytes < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (read_bytes == 0) break;
#endif
            pending.append(buffer, static_cast<std::size_t>(read_bytes));
            std::size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                auto line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                if (capture_diagnostics_ && line.front() != '{') {
                    diagnostics_ += line + "\n";
                    continue;
                }
                {
                    std::lock_guard lock{mutex_};
                    lines_.push_back(std::move(line));
                }
                arrived_.notify_all();
            }
        }
        {
            std::lock_guard lock{mutex_};
            closed_ = true;
        }
        arrived_.notify_all();
    }

    fs::path log_;
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<std::string> lines_;
    std::string diagnostics_;
    bool capture_diagnostics_ =
        std::getenv("PELICAN_VO1A_VULKAN_VALIDATION") != nullptr;
    std::atomic<bool> closed_ = false;
    bool close_started_ = false;
    int next_id_ = 1;
    int exit_code_ = -1;
#ifdef _WIN32
    HANDLE stdin_read_ = INVALID_HANDLE_VALUE;
    HANDLE stdin_write_ = INVALID_HANDLE_VALUE;
    HANDLE stdout_read_ = INVALID_HANDLE_VALUE;
    HANDLE stdout_write_ = INVALID_HANDLE_VALUE;
    HANDLE log_handle_ = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION process_{};
#else
    int input_ = -1;
    int output_ = -1;
    pid_t process_ = -1;
#endif
};

std::vector<std::string> outputFlags(
    const fs::path &directory, std::uint64_t max_frames = 1000,
    std::uint64_t max_bytes = 4ULL * 1024 * 1024 * 1024) {
    return {"--output-dir", directory.string(),
            "--output-max-frames", std::to_string(max_frames),
            "--output-max-bytes", std::to_string(max_bytes)};
}

Json oneSink(const Json &status) {
    REQUIRE(status.at("sinks").is_array());
    REQUIRE(status.at("sinks").size() == 1);
    return status.at("sinks").at(0);
}

Json outputProblems(PlayerRpc &player) {
    const auto all = player.result("get_problems");
    Json problems = Json::array();
    for (const auto &problem : all.at("problems")) {
        if (problem.value("code", std::string{}).starts_with("output."))
            problems.push_back(problem);
    }
    return problems;
}

} // namespace

TEST_CASE("VO-1a every_n writes numbered PVO1 files with the target's pixels and rows",
          "[vo1a-gpu][gpu]") {
    TempProject project{"pattern"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 2));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "pattern"};

    for (int frame = 0; frame < 6; ++frame) (void)player.result("step_frame");

    // The lowering's retain becomes the target plan's readback reason.
    {
        const auto plan = player.result("get_frame_plan");
        const auto &resources =
            plan.at("physical_target_plan").at("lowering_graph").at("resources");
        bool found = false;
        for (const auto &resource : resources) {
            if (resource.at("name") != "fill_target") continue;
            found = true;
            CHECK(resource.at("lifetime").at("retain") ==
                  Json::array({"readback"}));
        }
        CHECK(found);
    }

    const auto status = player.outputStatus();
    INFO(status.dump());
    CHECK(status.at("active") == true);
    const auto sink = oneSink(status);
    CHECK(sink.at("name") == "shot");
    CHECK(sink.at("kind") == "file");
    CHECK(sink.at("state") == "running");
    CHECK(sink.at("last_error") == "");
    // Six published frames at every_n 2 are exactly frames 0, 2 and 4.
    const auto files = frameFiles(output_dir);
    INFO(files.size());
    REQUIRE(files.size() == 3);
    CHECK(files.at(0).frame == 0);
    CHECK(files.at(1).frame == 2);
    CHECK(files.at(2).frame == 4);
    CHECK(sink.at("frames_written") == files.size());
    REQUIRE(sink.at("frames_written").get<std::uint64_t>() == 3);

    std::vector<std::uint8_t> blues;
    for (const auto &file : files) {
        requirePvo1(file, 0);
        CHECK(file.frame % 2 == 0);
        const auto blue = file.pixel(0, 0)[2];
        blues.push_back(blue);
        // The shader writes the engine's evaluation index (1 on the first
        // step), while the file name is the zero-based logical frame.
        CHECK(static_cast<std::uint64_t>(blue) == file.frame + 1);
        std::array<std::uint8_t, 4> reds{};
        std::array<std::uint8_t, 4> greens{};
        for (std::uint32_t y = 0; y < 4; ++y) {
            greens[y] = file.pixel(0, y)[1];
            for (std::uint32_t x = 0; x < 4; ++x) {
                const auto *pixel = file.pixel(x, y);
                INFO(file.path.string() << " pixel " << x << "," << y);
                if (y == 0) reds[x] = pixel[0];
                CHECK(pixel[0] == reds[x]);
                CHECK(pixel[1] == greens[y]);
                // The shader's alpha varies with x and y and is never 255:
                // raw pixels must keep it, not force opaque.
                CHECK(pixel[3] < 255);
                CHECK(pixel[3] > 0);
                CHECK(pixel[2] == blue);
                if (x > 0) {
                    CHECK(pixel[0] > file.pixel(x - 1, y)[0]);
                    CHECK(pixel[3] > file.pixel(x - 1, y)[3]);
                }
                if (y > 0) CHECK(pixel[3] > file.pixel(x, y - 1)[3]);
            }
        }
        // Row 0 is the first row in the file, and UV y grows with it.
        CHECK(greens[0] < greens[1]);
        CHECK(greens[1] < greens[2]);
        CHECK(greens[2] < greens[3]);
        CHECK(reds[0] < reds[1]);
        CHECK(reds[1] < reds[2]);
        CHECK(reds[2] < reds[3]);
    }
    // Every captured file carries that frame's pixels, not the first frame's.
    for (std::size_t index = 1; index < blues.size(); ++index)
        CHECK(blues[index] != blues[index - 1]);
    for (std::size_t index = 1; index < files.size(); ++index)
        CHECK(files[index].frame == files[index - 1].frame + 2);
    CHECK(allFilesWithSuffix(output_dir, ".partial").empty());
}

TEST_CASE("VO-1a without --output-dir the sink table is inactive and writes nothing",
          "[vo1a-gpu][gpu]") {
    TempProject project{"inactive"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot"));
    project.writeRoot(root);
    PlayerRpc player{project, {}, "inactive"};

    for (int frame = 0; frame < 3; ++frame) (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    CHECK(status.at("active") == false);
    const auto sink = oneSink(status);
    CHECK(sink.at("name") == "shot");
    CHECK(sink.at("state") == "inactive");
    CHECK(sink.at("frames_written") == 0);
    CHECK(sink.at("last_error") == "");
    CHECK(allFilesWithSuffix(project.root, ".rgba").empty());
}

TEST_CASE("VO-1a a disabled sink writes nothing and asks for no retain",
          "[vo1a-gpu][gpu]") {
    TempProject project{"disabled"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 1, false));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "disabled"};

    for (int frame = 0; frame < 3; ++frame) (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    CHECK(status.at("active") == true);
    CHECK(status.at("sinks").empty());
    CHECK(allFilesWithSuffix(project.root, ".rgba").empty());

    const auto plan = player.result("get_frame_plan");
    const auto &resources =
        plan.at("physical_target_plan").at("lowering_graph").at("resources");
    for (const auto &resource : resources) {
        if (!resource.contains("lifetime") ||
            !resource["lifetime"].contains("retain"))
            continue;
        const auto &retain = resource["lifetime"]["retain"];
        CHECK(std::find(retain.begin(), retain.end(), "readback") == retain.end());
    }
}

TEST_CASE("VO-1a --output-max-frames finishes the sink after one file",
          "[vo1a-gpu][gpu]") {
    TempProject project{"limit"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir, 1), "limit"};

    for (int frame = 0; frame < 3; ++frame) (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    const auto sink = oneSink(status);
    CHECK(sink.at("state") == "finished");
    CHECK(sink.at("stop_reason") == "max_frames");
    CHECK(sink.at("stop_value") == 1);
    CHECK(sink.at("frames_written") == 1);
    const auto files = frameFiles(output_dir);
    REQUIRE(files.size() == 1);
    requirePvo1(files.front(), 0);
}

TEST_CASE("VO-1a --output-max-bytes counts the header and finishes before one file",
          "[vo1a-gpu][gpu]") {
    TempProject project{"bytes"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    // One 4x4 RGBA8 file is a 16-byte PVO1 header plus 64 pixel bytes. A
    // 65-byte budget admits no complete file, so the header must be counted.
    PlayerRpc player{project, outputFlags(output_dir, 1000, 65), "bytes"};

    for (int frame = 0; frame < 3; ++frame) (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    const auto sink = oneSink(status);
    CHECK(sink.at("state") == "finished");
    CHECK(sink.at("stop_reason") == "max_bytes");
    CHECK(sink.at("stop_value") == 65);
    CHECK(sink.at("frames_written") == 0);
    CHECK(sink.at("last_error").get<std::string>().starts_with(
        "output.limit_reached@1"));
    CHECK(frameFiles(output_dir).empty());
    CHECK(allFilesWithSuffix(output_dir, ".partial").empty());
}

TEST_CASE("VO-1a an existing final file is untouched and refused without ending the frame",
          "[vo1a-gpu][gpu]") {
    TempProject project{"exists"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    const auto occupied = output_dir / "shot_000000.rgba";
    writeFile(occupied, "this file is not a PVO1 frame");
    PlayerRpc player{project, outputFlags(output_dir), "exists"};

    // The request must succeed: a sink refusal is reported, it does not end
    // the frame or the engine.
    (void)player.result("step_frame");
    (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    const auto sink = oneSink(status);
    CHECK(sink.at("state") == "refused");
    CHECK(sink.at("frames_written") == 0);
    CHECK(sink.at("last_error").get<std::string>().starts_with("output.exists@1"));
    CHECK(readFile(occupied) == "this file is not a PVO1 frame");
    CHECK(frameFiles(output_dir).size() == 1);
    CHECK(allFilesWithSuffix(output_dir, ".partial").empty());
}

TEST_CASE("VO-1a a non-RGBA8 target is refused by the readback without a file",
          "[vo1a-gpu][gpu]") {
    TempProject project{"format"};
    project.writeShader();
    auto root = project.readRoot();
    root["rendering"]["render_targets"] = Json::array(
        {{{"name", "external_hdr"},
          {"width", 4},
          {"height", 4},
          {"extent_scale", 1.0},
          {"format", "R16G16B16A16_SFLOAT"},
          {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}});
    Json external{{"id", "external"},
                  {"op", "render.target_ref"},
                  {"parameters",
                   {{"name", "external_hdr"}, {"type", sceneColorType()}}},
                  {"inputs", Json::object()},
                  {"outputs", Json::array({"resource"})}};
    attachMainGraph(root, std::move(external),
                    fillNode("fill", "external_hdr", "external"),
                    sinkNode("shot"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "format"};

    (void)player.result("step_frame");
    (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    const auto sink = oneSink(status);
    CHECK(sink.at("state") == "refused");
    CHECK(sink.at("frames_written") == 0);
    CHECK(sink.at("last_error").get<std::string>().starts_with(
        "output.format_unsupported@1"));
    CHECK(frameFiles(output_dir).empty());
}

TEST_CASE("VO-1a a later writer of the target refuses the sink before any pixel is read",
          "[vo1a-gpu][gpu]") {
    TempProject project{"overwritten"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot"));
    root["nodes"].push_back(
        fillNode("later_fill", "fill_target", "fill", "color"));
    // Keep both writers in the executed graph: a pass that only writes an
    // otherwise unconsumed target is dead and would not reach the plan.
    root["rendering"]["rendering_passes"][0]["passes"][0]["input"] =
        Json::array({"fill_target"});
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "overwritten"};

    (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    const auto sink = oneSink(status);
    CHECK(sink.at("state") == "refused");
    CHECK(sink.at("frames_written") == 0);
    const auto error = sink.at("last_error").get<std::string>();
    CHECK(error.starts_with("output.source_overwritten@1"));
    CHECK(error.find("later_fill") != std::string::npos);
    CHECK(frameFiles(output_dir).empty());

    // This refusal was known when the generation was published, before the
    // sink had a frame to read. It still reaches the problem list once the
    // runtime can take it, and it uses the runtime's own code.
    const auto problems = outputProblems(player);
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().at("code") == "output.source_overwritten@1");
    CHECK(problems.front().at("scope") == "main_render");
    CHECK(problems.front().at("node") == "shot");
    CHECK(problems.front().at("message") == sink.at("last_error"));
}

TEST_CASE("VO-1a reloading a changed every_n keeps the counters and writes every frame",
          "[vo1a-gpu][gpu]") {
    TempProject project{"reload"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 2));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "reload"};

    for (int frame = 0; frame < 4; ++frame) (void)player.result("step_frame");
    const auto before = frameFiles(output_dir);
    REQUIRE(before.size() >= 2);
    const auto before_status = player.outputStatus();
    const auto before_written =
        oneSink(before_status).at("frames_written").get<std::uint64_t>();
    CHECK(before_written == before.size());

    auto changed = project.readRoot();
    for (auto &node : changed.at("nodes")) {
        if (node.at("op") == "output.file") node["parameters"]["every_n"] = 1;
    }
    project.writeRoot(changed);
    const auto settled = player.settle(player.result("reload_render_documents"));
    INFO(settled.dump(2));
    REQUIRE(settled.at("status") == "committed");
    CHECK(settled.at("operation") == "reload_render_documents");

    for (int frame = 0; frame < 3; ++frame) (void)player.result("step_frame");
    const auto after = frameFiles(output_dir);
    REQUIRE(after.size() > before.size());
    bool saw_odd = false;
    for (const auto &file : after) {
        requirePvo1(file, 0);
        if (file.frame % 2 == 1) saw_odd = true;
    }
    CHECK(saw_odd);
    const auto after_status = player.outputStatus();
    INFO(after_status.dump());
    const auto after_written =
        oneSink(after_status).at("frames_written").get<std::uint64_t>();
    CHECK(after_written == after.size());
    CHECK(after_written > before_written);
}

TEST_CASE("VO-1a a BGRA8 target writes its memory order and format code",
          "[vo1a-gpu][gpu]") {
    TempProject project{"bgra"};
    project.writeConstantBlueShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "B8G8R8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "bgra"};

    for (int frame = 0; frame < 2; ++frame) (void)player.result("step_frame");
    const auto files = frameFiles(output_dir);
    REQUIRE(files.size() >= 1);
    const auto &file = files.front();
    requirePvo1(file, 2);
    std::array<std::uint8_t, 4> reds{};
    std::array<std::uint8_t, 4> greens{};
    for (std::uint32_t y = 0; y < 4; ++y) {
        greens[y] = file.pixel(0, y)[1];
        for (std::uint32_t x = 0; x < 4; ++x) {
            const auto *pixel = file.pixel(x, y);
            INFO(file.path.string() << " pixel " << x << "," << y);
            // B8G8R8A8_UNORM stores B, G, R, A.
            if (y == 0) reds[x] = pixel[2];
            CHECK(pixel[0] == 64);
            CHECK(pixel[2] == reds[x]);
            CHECK(pixel[1] == greens[y]);
            CHECK(pixel[3] == 255);
        }
    }
    CHECK(greens[0] < greens[3]);
    CHECK(reds[0] < reds[3]);
}

TEST_CASE("VO-1b PNG frames hold the rendered RGBA bytes with row 0 first",
          "[vo1b-gpu][gpu]") {
    TempProject project{"png"};
    project.writeExactPatternShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 1, true, "png"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "png"};

    for (int frame = 0; frame < 2; ++frame)
        (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    const auto sink = oneSink(status);
    CHECK(sink.at("format") == "png");
    CHECK(sink.at("state") == "running");
    CHECK(sink.at("stop_reason") == "none");
    CHECK(sink.at("frames_written") == 2);

    const auto files = frameFiles(output_dir, ".png");
    REQUIRE(files.size() == 2);
    CHECK(files.at(0).frame == 0);
    CHECK(files.at(1).frame == 1);
    const auto expected = expectedPattern();
    for (const auto &file : files) {
        const auto png = decodePng(file.path);
        requirePng(png);
        INFO(file.path.string());
        CHECK(png.pixels == expected);
    }
    CHECK(frameFiles(output_dir).empty());
    CHECK(allFilesWithSuffix(output_dir, ".partial").empty());
}

TEST_CASE("VO-1b a BGRA8 target is swapped to RGBA in the PNG",
          "[vo1b-gpu][gpu]") {
    TempProject project{"bgra_png"};
    project.writeConstantBlueShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "B8G8R8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 1, true, "png"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "bgra_png"};

    (void)player.result("step_frame");

    const auto files = frameFiles(output_dir, ".png");
    REQUIRE(files.size() == 1);
    const auto png = decodePng(files.front().path);
    requirePng(png);
    // The shader writes (inUV.x, inUV.y, 0.25, 1.0). B8G8R8A8 readback stores
    // those bytes as B,G,R,A; the PNG must store them as R,G,B,A. If the
    // writer copied the memory order, the red at (0,0) would be 64.
    const auto *top_left = png.pixel(0, 0);
    CHECK(top_left[0] < 64);
    CHECK(top_left[2] == 64);
    CHECK(top_left[3] == 255);
    CHECK(png.pixel(3, 0)[0] > png.pixel(0, 0)[0]);
    CHECK(png.pixel(0, 3)[1] > png.pixel(0, 0)[1]);
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 4; ++x)
            CHECK(png.pixel(x, y)[3] == 255);
}

TEST_CASE("VO-1b a max_frames stop is one limit problem, not one per frame",
          "[vo1b-gpu][gpu]") {
    TempProject project{"frames_problem"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir, 1), "frames_problem"};

    for (int frame = 0; frame < 3; ++frame)
        (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    const auto sink = oneSink(status);
    CHECK(sink.at("state") == "finished");
    CHECK(sink.at("stop_reason") == "max_frames");
    CHECK(sink.at("stop_value") == 1);
    CHECK(sink.at("frames_written") == 1);
    CHECK(frameFiles(output_dir).size() == 1);

    auto problems = outputProblems(player);
    REQUIRE(problems.size() == 1);
    INFO(problems.dump(2));
    const auto &problem = problems.front();
    CHECK(problem.at("code") == "output.limit_reached@1");
    CHECK(problem.at("severity") == "error");
    CHECK(problem.at("source") == "runtime");
    CHECK(problem.at("scope") == "main_render");
    CHECK(problem.at("node") == "shot");
    CHECK(problem.at("fields").at("graph") == "main_render");
    CHECK(problem.at("fields").at("sink") == "shot");
    CHECK(problem.at("fields").at("limit") == "max_frames");
    CHECK(problem.at("fields").at("value") == 1);
    CHECK(problem.at("message").get<std::string>().find("max_frames") !=
          std::string::npos);
    const auto revision = player.result("get_problems").at("revision");

    for (int frame = 0; frame < 2; ++frame)
        (void)player.result("step_frame");
    problems = outputProblems(player);
    CHECK(problems.size() == 1);
    CHECK(player.result("get_problems").at("revision") == revision);
    CHECK(frameFiles(output_dir).size() == 1);
}

TEST_CASE("VO-1b a PNG max_bytes stop counts the real encoded size",
          "[vo1b-gpu][gpu]") {
    TempProject project{"png_bytes_problem"};
    project.writeConstantBlueShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 1, true, "png"));
    project.writeRoot(root);

    // One probe file gives the exact compressed size. The budget then admits
    // exactly two files and part of a third, so the third has to be written
    // as a partial and measured before it can be refused.
    const auto probe_dir = project.root / "probe";
    fs::create_directories(probe_dir);
    {
        PlayerRpc probe{project, outputFlags(probe_dir), "png_bytes_probe"};
        (void)probe.result("step_frame");
    }
    const auto probe_files = frameFiles(probe_dir, ".png");
    REQUIRE(probe_files.size() == 1);
    const auto encoded_size = fs::file_size(probe_files.front().path);
    REQUIRE(encoded_size > 0);
    const auto budget = encoded_size * 2 + encoded_size / 2;

    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir, 1000, budget),
                     "png_bytes_problem"};
    for (int frame = 0; frame < 3; ++frame)
        (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    const auto sink = oneSink(status);
    CHECK(sink.at("state") == "finished");
    CHECK(sink.at("stop_reason") == "max_bytes");
    CHECK(sink.at("stop_value") == budget);
    CHECK(sink.at("frames_written") == 2);
    const auto files = frameFiles(output_dir, ".png");
    REQUIRE(files.size() == 2);
    std::uint64_t total = 0;
    for (const auto &file : files) total += fs::file_size(file.path);
    CHECK(total == encoded_size * 2);
    CHECK(sink.at("last_error").get<std::string>().find("needs") !=
          std::string::npos);
    CHECK(allFilesWithSuffix(output_dir, ".partial").empty());

    const auto problems = outputProblems(player);
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().at("code") == "output.limit_reached@1");
    CHECK(problems.front().at("fields").at("limit") == "max_bytes");
    CHECK(problems.front().at("fields").at("value") == budget);
    CHECK(problems.front().at("message").get<std::string>().find("needs") !=
          std::string::npos);
}

TEST_CASE("VO-1b a refused sink has one problem and keeps it across frames",
          "[vo1b-gpu][gpu]") {
    TempProject project{"refused_problem"};
    project.writeExactPatternShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 1, true, "png"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    const auto blocker = output_dir / "shot_000000.png.partial";
    writeFile(blocker, "not mine");
    PlayerRpc player{project, outputFlags(output_dir), "refused_problem"};

    for (int frame = 0; frame < 3; ++frame)
        (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    const auto sink = oneSink(status);
    CHECK(sink.at("state") == "refused");
    CHECK(sink.at("stop_reason") == "none");
    CHECK(sink.at("frames_written") == 0);
    const auto error = sink.at("last_error").get<std::string>();
    CHECK(error.starts_with("output.write_failed@1"));
    CHECK(error.find("shot_000000.png.partial") != std::string::npos);
    CHECK(readFile(blocker) == "not mine");
    CHECK(frameFiles(output_dir, ".png").empty());
    CHECK(allFilesWithSuffix(output_dir, ".png").empty());

    const auto problems = outputProblems(player);
    REQUIRE(problems.size() == 1);
    INFO(problems.dump(2));
    const auto &problem = problems.front();
    CHECK(problem.at("code") == "output.write_failed@1");
    CHECK(problem.at("scope") == "main_render");
    CHECK(problem.at("node") == "shot");
    CHECK(problem.at("fields").at("graph") == "main_render");
    CHECK(problem.at("fields").at("sink") == "shot");
    CHECK(problem.at("message") == sink.at("last_error"));
    CHECK_FALSE(problem.at("id").get<std::string>().empty());
    const auto revision = player.result("get_problems").at("revision");

    for (int frame = 0; frame < 2; ++frame)
        (void)player.result("step_frame");
    const auto later = outputProblems(player);
    CHECK(later.size() == 1);
    CHECK(player.result("get_problems").at("revision") == revision);
    CHECK(readFile(blocker) == "not mine");
}

TEST_CASE("VO-1b a reload withdraws the problem when the sink writes again",
          "[vo1b-gpu][gpu]") {
    TempProject project{"problem_recovery"};
    project.writeExactPatternShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 2, true, "png"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    const auto blocker = output_dir / "shot_000000.png.partial";
    writeFile(blocker, "not mine");
    PlayerRpc player{project, outputFlags(output_dir), "problem_recovery"};

    (void)player.result("step_frame");
    REQUIRE(outputProblems(player).size() == 1);
    CHECK(oneSink(player.outputStatus()).at("state") == "refused");

    fs::remove(blocker);
    auto changed = project.readRoot();
    for (auto &node : changed.at("nodes"))
        if (node.at("op") == "output.file") node["parameters"]["every_n"] = 1;
    project.writeRoot(changed);
    const auto settled = player.settle(player.result("reload_render_documents"));
    INFO(settled.dump(2));
    REQUIRE(settled.at("status") == "committed");

    const auto recovered = player.outputStatus();
    INFO(recovered.dump());
    const auto sink = oneSink(recovered);
    CHECK(sink.at("state") == "running");
    CHECK(sink.at("last_error") == "");
    CHECK(outputProblems(player).empty());
    CHECK(allFilesWithSuffix(output_dir, ".partial").empty());

    const auto before = frameFiles(output_dir, ".png").size();
    (void)player.result("step_frame");
    CHECK(frameFiles(output_dir, ".png").size() > before);
    CHECK(outputProblems(player).empty());
}

TEST_CASE("VO-1b removing the sink withdraws its problem",
          "[vo1b-gpu][gpu]") {
    TempProject project{"problem_removal"};
    project.writeExactPatternShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 1, true, "png"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    writeFile(output_dir / "shot_000000.png.partial", "not mine");
    PlayerRpc player{project, outputFlags(output_dir), "problem_removal"};

    (void)player.result("step_frame");
    REQUIRE(outputProblems(player).size() == 1);

    auto changed = project.readRoot();
    auto &nodes = changed.at("nodes");
    const auto sink = std::find_if(
        nodes.begin(), nodes.end(),
        [](const Json &node) { return node.at("op") == "output.file"; });
    REQUIRE(sink != nodes.end());
    nodes.erase(sink);
    project.writeRoot(changed);
    const auto settled = player.settle(player.result("reload_render_documents"));
    INFO(settled.dump(2));
    REQUIRE(settled.at("status") == "committed");

    const auto status = player.outputStatus();
    INFO(status.dump());
    CHECK(status.at("sinks").empty());
    CHECK(outputProblems(player).empty());
    CHECK(allFilesWithSuffix(output_dir, ".png").empty());
}

TEST_CASE("VO-1b an inactive sink is not a problem even with a stopping error code",
          "[vo1b-gpu][gpu]") {
    SECTION("a source_overwritten error without --output-dir") {
        TempProject project{"inactive_error"};
        project.writeShader();
        auto root = project.readRoot();
        attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                        fillNode("fill", "fill_target", "fill_target"),
                        sinkNode("shot"));
        root["nodes"].push_back(
            fillNode("later_fill", "fill_target", "fill", "color"));
        root["rendering"]["rendering_passes"][0]["passes"][0]["input"] =
            Json::array({"fill_target"});
        project.writeRoot(root);
        PlayerRpc player{project, {}, "inactive_error"};

        (void)player.result("step_frame");

        const auto status = player.outputStatus();
        INFO(status.dump());
        CHECK(status.at("active") == false);
        const auto sink = oneSink(status);
        CHECK(sink.at("state") == "inactive");
        CHECK(sink.at("last_error") == "");
        CHECK(sink.at("frames_written") == 0);
        CHECK(outputProblems(player).empty());
    }
    SECTION("a disabled sink without --output-dir") {
        TempProject project{"inactive_disabled"};
        project.writeShader();
        auto root = project.readRoot();
        attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                        fillNode("fill", "fill_target", "fill_target"),
                        sinkNode("shot", "fill", "color", 1, false));
        project.writeRoot(root);
        PlayerRpc player{project, {}, "inactive_disabled"};

        (void)player.result("step_frame");

        const auto status = player.outputStatus();
        INFO(status.dump());
        CHECK(status.at("active") == false);
        CHECK(status.at("sinks").empty());
        CHECK(outputProblems(player).empty());
    }
}

TEST_CASE("VO-1b thinned PNG file n holds frame_index n+1 in the headless fixture",
          "[vo1b-gpu][gpu]") {
    TempProject project{"file_number"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 2, true, "png"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "file_number"};

    for (int frame = 0; frame < 6; ++frame) (void)player.result("step_frame");
    const auto files = frameFiles(output_dir, ".png");
    REQUIRE(files.size() == 3);
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto expected_number = index * 2;
        CHECK(files.at(index).frame == expected_number);
        // In this ordinary headless fixture the shader's blue is the one-based
        // frame_index. File numbers come only from the published frame count.
        const auto png = decodePng(files.at(index).path);
        requirePng(png);
        CHECK(png.pixel(0, 0)[2] == expected_number + 1);
    }
}

TEST_CASE("VO-1b the cross-platform driver quotes a project path with spaces and reaps the player",
          "[vo1b-gpu][gpu]") {
    TempProject project{"driver path with spaces"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "driver spaces"};

    (void)player.result("step_frame");
    const auto status = player.outputStatus();
    INFO(status.dump());
    CHECK(status.at("active") == true);
    const auto sink = oneSink(status);
    CHECK(sink.at("state") == "running");
    CHECK(frameFiles(output_dir).size() == 1);

    player.close();
    CHECK(player.exitCode() == 0);
}


TEST_CASE("VO-2 drop never waits for a paused worker and preserves file frame pixels",
          "[vo2][vo2-gpu][gpu]") {
    TempProject project{"vo2_drop"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 1, true, "png", "drop"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "vo2_drop"};
    (void)player.result("step_frame");
    (void)player.result("test_output_worker_pause", {{"paused", true}});

    for (int frame = 0; frame < 16; ++frame) {
        const auto begin = std::chrono::steady_clock::now();
        (void)player.result("step_frame");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin);
        CHECK(elapsed < std::chrono::seconds{5});
    }
    const auto paused_sink = oneSink(player.outputStatus());
    INFO(paused_sink.dump());
    CHECK(paused_sink.at("queue_depth").get<std::uint64_t>() <= 4);
    CHECK(paused_sink.at("frames_dropped").get<std::uint64_t>() > 0);

    (void)player.result("test_output_worker_pause", {{"paused", false}});
    const auto sink = oneSink(player.outputStatus());
    CHECK(sink.at("queue_depth") == 0);
    const auto files = frameFiles(output_dir, ".png");
    REQUIRE(files.size() == sink.at("frames_written").get<std::uint64_t>());
    REQUIRE(files.size() >= 2);
    for (const auto &file : files) {
        const auto png = decodePng(file.path);
        requirePng(png);
        CHECK(static_cast<std::uint64_t>(png.pixel(0, 0)[2]) == file.frame + 1);
    }
}

TEST_CASE("VO-2 wait blocks the fifth capture briefly and writes every frame",
          "[vo2][vo2-gpu][gpu]") {
    TempProject project{"vo2_wait"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 1, true, "png", "wait"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "vo2_wait"};
    for (int frame = 0; frame < 2; ++frame) (void)player.result("step_frame");
    (void)player.result("test_output_worker_pause",
        {{"paused", true}, {"resume_after_ms", 300}});

    for (int frame = 0; frame < 4; ++frame) (void)player.result("step_frame");
    const auto begin = std::chrono::steady_clock::now();
    (void)player.result("step_frame");
    const auto blocked = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    CHECK(blocked >= std::chrono::milliseconds{250});
    CHECK(blocked < std::chrono::seconds{5});
    const auto sink = oneSink(player.outputStatus());
    INFO(sink.dump());
    CHECK(sink.at("render_wait_ms").get<double>() > 0.0);
    CHECK(sink.at("queue_depth").get<std::uint64_t>() <= 4);

    const auto files = frameFiles(output_dir, ".png");
    REQUIRE(files.size() == 7);
    CHECK(sink.at("frames_written") == files.size());
    CHECK(sink.at("frames_dropped") == 0);
    for (const auto &file : files) {
        const auto png = decodePng(file.path);
        requirePng(png);
        CHECK(static_cast<std::uint64_t>(png.pixel(0, 0)[2]) == file.frame + 1);
    }
}


TEST_CASE("VO-2 resize reallocates only free slots and keeps file identities",
          "[vo2][vo2-gpu][gpu]") {
    TempProject project{"vo2_resize"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 1, true, "png", "wait"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "vo2_resize"};
    for (int frame = 0; frame < 2; ++frame) (void)player.result("step_frame");
    const auto old_files = frameFiles(output_dir, ".png").size();
    (void)player.result("test_output_worker_pause", {{"paused", true}});
    for (int frame = 0; frame < 4; ++frame) (void)player.result("step_frame");

    auto resized = project.readRoot();
    for (auto &node : resized.at("nodes")) {
        if (node.at("op") == "render.image") {
            node["parameters"]["width"] = 8;
            node["parameters"]["height"] = 8;
        }
        if (node.at("op") == "output.file") node["parameters"]["every_n"] = 1;
    }
    project.writeRoot(resized);
    const auto settled = player.settle(player.result("reload_render_documents"));
    REQUIRE(settled.at("status") == "committed");
    for (int frame = 0; frame < 2; ++frame) (void)player.result("step_frame");
    (void)player.result("test_output_worker_pause", {{"paused", false}});
    const auto sink = oneSink(player.outputStatus());
    INFO(sink.dump());
    CHECK(sink.at("queue_depth") == 0);
    CHECK(sink.at("frames_discarded").get<std::uint64_t>() > 0);

    const auto files = frameFiles(output_dir, ".png");
    CHECK(files.size() > old_files);
    CHECK(files.size() == sink.at("frames_written").get<std::uint64_t>());
    bool saw_new_size = false;
    for (const auto &file : files) {
        const auto png = decodePng(file.path);
        const auto size = file.frame < old_files ? 4u : 8u;
        requirePng(png, size, size);
        CHECK(static_cast<std::uint64_t>(png.pixel(0, 0)[2]) == file.frame + 1);
        saw_new_size = saw_new_size || size == 8;
    }
    CHECK(saw_new_size);
}

TEST_CASE("VO-2 removal discards queued generations and paused shutdown drains safely",
          "[vo2][vo2-gpu][gpu]") {
    TempProject project{"vo2_removal"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 1, true, "png", "drop"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "vo2_removal"};
    for (int frame = 0; frame < 2; ++frame) (void)player.result("step_frame");
    const auto files_before = frameFiles(output_dir, ".png");
    (void)player.result("test_output_worker_pause", {{"paused", true}});
    for (int frame = 0; frame < 4; ++frame) (void)player.result("step_frame");

    auto removed = project.readRoot();
    auto &nodes = removed.at("nodes");
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [](const Json &node) {
        return node.at("op") == "output.file";
    }), nodes.end());
    project.writeRoot(removed);
    const auto settled = player.settle(player.result("reload_render_documents"));
    REQUIRE(settled.at("status") == "committed");
    CHECK(player.outputStatus().at("sinks").empty());
    (void)player.result("test_output_worker_pause", {{"paused", false}});
    (void)player.result("get_status");
    const auto files_after = frameFiles(output_dir, ".png");
    CHECK(files_after.size() == files_before.size());
    for (const auto &file : files_after) CHECK(file.frame < files_before.size());

    (void)player.result("test_output_worker_pause", {{"paused", true}});
    player.close();
    CHECK(player.exitCode() == 0);
    const auto log = readFile(project.logPath("vo2_removal"));
    CHECK(log.find("VUID-") == std::string::npos);
    CHECK(log.find("Assertion") == std::string::npos);
}


TEST_CASE("VO-2 max_frames completes its reservations without accepting more",
          "[vo2][vo2-gpu][gpu]") {
    TempProject project{"vo2_max_frames"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 1, true, "png", "drop"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir, 2), "vo2_max_frames"};
    (void)player.result("step_frame");
    (void)player.result("test_output_worker_pause", {{"paused", true}});
    for (int frame = 0; frame < 8; ++frame) (void)player.result("step_frame");
    const auto paused_sink = oneSink(player.outputStatus());
    CHECK(paused_sink.at("queue_depth").get<std::uint64_t>() <= 2);
    (void)player.result("test_output_worker_pause", {{"paused", false}});

    const auto sink = oneSink(player.outputStatus());
    INFO(sink.dump());
    CHECK(sink.at("state") == "finished");
    CHECK(sink.at("stop_reason") == "max_frames");
    CHECK(sink.at("stop_value") == 2);
    CHECK(sink.at("frames_written") == 2);
    CHECK(sink.at("frames_discarded") == 0);
    const auto files = frameFiles(output_dir, ".png");
    REQUIRE(files.size() == 2);
    CHECK(files.at(0).frame == 0);
    for (const auto &file : files) {
        const auto png = decodePng(file.path);
        requirePng(png);
        CHECK(static_cast<std::uint64_t>(png.pixel(0, 0)[2]) == file.frame + 1);
    }
}


TEST_CASE("VO-2 reload preserves a completed max_frames limit",
          "[vo2][vo2-gpu][gpu]") {
    TempProject project{"vo2_max_frames_reload_done"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 1, true, "png", "wait"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir, 1),
                     "vo2_max_frames_reload_done"};
    (void)player.result("step_frame");
    REQUIRE(oneSink(player.outputStatus()).at("state") == "finished");

    root["nodes"].back()["parameters"]["every_n"] = 2;
    project.writeRoot(root);
    const auto settled = player.settle(player.result("reload_render_documents"));
    REQUIRE(settled.at("status") == "committed");
    const auto sink = oneSink(player.outputStatus());
    INFO(sink.dump());
    CHECK(sink.at("state") == "finished");
    CHECK(sink.at("stop_reason") == "max_frames");
    CHECK(sink.at("frames_written") == 1);
    (void)player.result("step_frame");
    CHECK(frameFiles(output_dir, ".png").size() == 1);
}

TEST_CASE("VO-2 reload releases unfinished max_frames reservations",
          "[vo2][vo2-gpu][gpu]") {
    TempProject project{"vo2_max_frames_reload_pending"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 1, true, "png", "wait"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir, 2),
                     "vo2_max_frames_reload_pending"};
    (void)player.result("step_frame");
    (void)player.result("test_output_worker_pause", {{"paused", true}});
    (void)player.result("step_frame");

    root["nodes"].back()["parameters"]["every_n"] = 2;
    project.writeRoot(root);
    const auto settled = player.settle(player.result("reload_render_documents"));
    REQUIRE(settled.at("status") == "committed");
    (void)player.result("test_output_worker_pause", {{"paused", false}});
    auto sink = oneSink(player.outputStatus());
    CHECK(sink.at("state") == "running");
    CHECK(sink.at("frames_written") == 1);
    CHECK(sink.at("frames_discarded") == 1);

    (void)player.result("step_frame");
    (void)player.result("step_frame");
    sink = oneSink(player.outputStatus());
    INFO(sink.dump());
    CHECK(sink.at("state") == "finished");
    CHECK(sink.at("frames_written") == 2);
    const auto files = frameFiles(output_dir, ".png");
    REQUIRE(files.size() == 2);
    CHECK(files.at(0).frame == 0);
    CHECK(files.at(1).frame == 4);
}


namespace {
Json durationStats(std::vector<double> values) {
    if (values.empty()) return nullptr;
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double fraction) {
        const auto index = static_cast<std::size_t>(
            fraction * static_cast<double>(values.size() - 1));
        return values.at(index);
    };
    return {{"count", values.size()},
            {"median_ms", percentile(0.50)},
            {"p95_ms", percentile(0.95)},
            {"p99_ms", percentile(0.99)}};
}
}

TEST_CASE("VO-2 explicit asynchronous capture benchmark",
          "[.][vo2-benchmark][gpu]") {
    const char *result_path = std::getenv("PELICAN_VO2_BENCHMARK_JSON");
    REQUIRE(result_path != nullptr);
    constexpr int warmup_frames = 24;
    constexpr int measured_frames = 300;
    constexpr int history_batch = 60;

    Json measurements = Json::array();
    for (const std::string_view mode : {"none", "wait", "drop"}) {
        TempProject project{"vo2_benchmark_" + std::string{mode}};
        project.writeShader();
        auto root = project.readRoot();
        root["rendering"]["features"].push_back("engine://features/gpu_timing.json");
        auto target = targetNode("fill_target", "R8G8B8A8_UNORM");
        target["parameters"]["width"] = 1920;
        target["parameters"]["height"] = 1080;
        attachMainGraph(root, std::move(target),
            fillNode("fill", "fill_target", "fill_target"),
            sinkNode("shot", "fill", "color", 1, true, "png",
                mode == "none" ? "wait" : std::string{mode}));
        if (mode == "none") {
            auto &nodes = root.at("nodes");
            nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                [](const Json &node) { return node.at("op") == "output.file"; }),
                nodes.end());
        }
        project.writeRoot(root);
        const auto output_dir = project.outputPath();
        PlayerRpc player{project, outputFlags(output_dir, 2000,
            32ULL * 1024 * 1024 * 1024), "vo2_benchmark"};
        for (int frame = 0; frame < warmup_frames; ++frame)
            (void)player.result("step_frame");
        const auto initial_output = player.result("get_status").at("output").at("sinks");
        (void)player.result("test_output_capture_timings");
        (void)player.result("test_output_step_status", {{"enabled", false}});

        std::vector<double> cpu_ms;
        std::vector<double> gpu_ms;
        std::vector<std::uint64_t> gpu_frames;
        for (int frame = 0; frame < measured_frames; ++frame) {
            const auto begin = std::chrono::steady_clock::now();
            (void)player.result("step_frame");
            cpu_ms.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - begin).count());
            if ((frame + 1) % history_batch != 0 &&
                frame + 1 != measured_frames) continue;
            const auto timing = player.result("test_render_gpu_timing");
            for (const auto &row : timing.at("logical_frame_history")) {
                const auto logical = row.at("logical_frame").get<std::uint64_t>();
                if (logical < static_cast<std::uint64_t>(warmup_frames) ||
                    logical >= static_cast<std::uint64_t>(
                        warmup_frames + measured_frames) ||
                    std::find(gpu_frames.begin(), gpu_frames.end(), logical) !=
                        gpu_frames.end()) continue;
                gpu_frames.push_back(logical);
                gpu_ms.push_back(row.at("total_ms").get<double>());
            }
        }
        const auto timings = player.result("test_output_capture_timings");
        (void)player.result("test_output_step_status", {{"enabled", true}});
        const auto final_output = player.result("get_status").at("output").at("sinks");
        for (int frame = 0; frame < 3; ++frame)
            (void)player.result("step_frame");
        const auto tail_timing = player.result("test_render_gpu_timing");
        for (const auto &row : tail_timing.at("logical_frame_history")) {
            const auto logical = row.at("logical_frame").get<std::uint64_t>();
            if (logical < static_cast<std::uint64_t>(warmup_frames) ||
                logical >= static_cast<std::uint64_t>(
                    warmup_frames + measured_frames) ||
                std::find(gpu_frames.begin(), gpu_frames.end(), logical) !=
                    gpu_frames.end()) continue;
            gpu_frames.push_back(logical);
            gpu_ms.push_back(row.at("total_ms").get<double>());
        }
        REQUIRE(gpu_ms.size() == measured_frames);

        std::vector<double> selection_ms;
        std::vector<double> record_submit_ms;
        std::vector<double> capture_ms;
        for (const auto &sample : timings.at("samples")) {
            selection_ms.push_back(sample.at("selection_ms").get<double>());
            record_submit_ms.push_back(sample.at("record_submit_ms").get<double>());
            capture_ms.push_back(sample.at("total_ms").get<double>());
        }
        if (mode != "none") REQUIRE(capture_ms.size() == measured_frames);

        const auto counter_delta = [&](std::string_view key) -> std::uint64_t {
            if (mode == "none") return 0;
            const auto &before = initial_output.at(0);
            const auto &after = final_output.at(0);
            return after.at(key).get<std::uint64_t>() -
                before.at(key).get<std::uint64_t>();
        };
        const auto wait_delta = [&]() -> double {
            if (mode == "none") return 0.0;
            return final_output.at(0).at("render_wait_ms").get<double>() -
                initial_output.at(0).at("render_wait_ms").get<double>();
        };
        if (mode != "none") {
            const auto files = frameFiles(output_dir, ".png");
            REQUIRE(!files.empty());
            const auto png = decodePng(files.back().path);
            requirePng(png, 1920, 1080);
        }
        measurements.push_back({
            {"mode", mode},
            {"capture_selection", durationStats(std::move(selection_ms))},
            {"capture_record_submit", durationStats(std::move(record_submit_ms))},
            {"capture_total", durationStats(std::move(capture_ms))},
            {"whole_frame_cpu", durationStats(std::move(cpu_ms))},
            {"whole_frame_gpu", durationStats(std::move(gpu_ms))},
            {"frames_written", counter_delta("frames_written")},
            {"frames_dropped", counter_delta("frames_dropped")},
            {"frames_discarded", counter_delta("frames_discarded")},
            {"render_wait_ms", wait_delta()},
            {"maximum_queue_depth", timings.at("maximum_queue_depth")},
        });
    }
    writeFile(result_path, measurements.dump(2) + "\n");
}

namespace {

Json spoutSink(std::string name, std::string id = "spout") {
    return {{"id", std::move(id)},
            {"op", "output.spout"},
            {"parameters", {{"name", std::move(name)}}},
            {"inputs", {{"source", {{"node", "fill"}, {"port", "color"}}}}},
            {"outputs", Json::array()}};
}

// The player's sinks use a recording backend while this is alive. The child
// process inherits it.
struct SpoutTestBackendEnvironment {
    explicit SpoutTestBackendEnvironment(const char *mode = "1") { set(mode); }
    ~SpoutTestBackendEnvironment() { set(""); }
    static void set(const char *value) {
#ifdef _WIN32
        REQUIRE(_putenv_s("PELICAN_OUTPUT_SPOUT_TEST_BACKEND", value) == 0);
#else
        if (value[0] == 0) {
            REQUIRE(unsetenv("PELICAN_OUTPUT_SPOUT_TEST_BACKEND") == 0);
        } else {
            REQUIRE(setenv("PELICAN_OUTPUT_SPOUT_TEST_BACKEND", value, 1) == 0);
        }
#endif
    }
};

Json sinkNamed(const Json &status, std::string_view name) {
    for (const auto &sink : status.at("sinks")) {
        if (sink.at("name").get<std::string>() == name) return sink;
    }
    FAIL("no sink named " << std::string{name} << " in " << status.dump());
    return Json{};
}

template <class Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds limit = std::chrono::seconds(15)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

void attachSpoutGraph(TempProject &project, std::vector<Json> sinks) {
    auto root = project.readRoot();
    auto &nodes = root["nodes"];
    nodes.push_back(targetNode("fill_target", "R8G8B8A8_UNORM"));
    nodes.push_back(fillNode("fill", "fill_target", "fill_target"));
    for (auto &sink : sinks) nodes.push_back(std::move(sink));
    project.writeRoot(root);
}

// The 4x4 pattern of writeExactPatternShader, in the order Spout carries it.
std::vector<std::uint8_t> exactPatternBgra() {
    std::vector<std::uint8_t> bytes;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            bytes.push_back(static_cast<std::uint8_t>(30 + x * y));
            bytes.push_back(static_cast<std::uint8_t>(20 + y));
            bytes.push_back(static_cast<std::uint8_t>(10 + x));
            bytes.push_back(static_cast<std::uint8_t>(40 + x + y));
        }
    }
    return bytes;
}

} // namespace

TEST_CASE("VO-4 without --output-spout nothing opens, even with --output-dir",
          "[vo4][gpu]") {
    SpoutTestBackendEnvironment backend;
    TempProject project{"vo4_no_flag"};
    project.writeExactPatternShader();
    attachSpoutGraph(project, {spoutSink("send"), sinkNode("shot")});
    const auto output_dir = project.outputPath();

    SECTION("no flags at all") {
        PlayerRpc player{project, {}, "vo4_no_flag"};
        for (int frame = 0; frame < 3; ++frame) (void)player.result("step_frame");
        const auto status = player.outputStatus();
        const auto spout = sinkNamed(status, "send");
        INFO(spout.dump());
        CHECK(spout.at("kind") == "spout");
        CHECK(spout.at("state") == "inactive");
        CHECK(spout.at("stop_reason") == "not_enabled");
        CHECK(spout.at("registered") == false);
        CHECK(spout.at("frames_written") == 0);
        CHECK_FALSE(spout.contains("receivers"));
        CHECK(sinkNamed(status, "shot").at("state") == "inactive");
        const auto seen = player.result("test_output_spout_backend");
        CHECK(seen.at("opens") == 0);
        CHECK(seen.at("sends") == 0);
        CHECK(outputProblems(player).empty());
    }
    SECTION("--output-dir alone arms only the file sink") {
        PlayerRpc player{project, outputFlags(output_dir), "vo4_dir_only"};
        for (int frame = 0; frame < 3; ++frame) (void)player.result("step_frame");
        const auto status = player.outputStatus();
        const auto spout = sinkNamed(status, "send");
        INFO(status.dump());
        CHECK(spout.at("state") == "inactive");
        CHECK(spout.at("stop_reason") == "not_enabled");
        CHECK(sinkNamed(status, "shot").at("state") == "running");
        CHECK(sinkNamed(status, "shot").at("frames_written") == 3);
        CHECK(player.result("test_output_spout_backend").at("opens") == 0);
        CHECK(frameFiles(output_dir).size() == 3);
    }
}

TEST_CASE("VO-4 --output-spout alone sends and leaves the file sink inactive",
          "[vo4][gpu]") {
    SpoutTestBackendEnvironment backend;
    TempProject project{"vo4_flag_only"};
    project.writeExactPatternShader();
    attachSpoutGraph(project, {spoutSink("send"), sinkNode("shot")});
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, {"--output-spout"}, "vo4_flag_only"};
    Json status;
    const auto published = waitFor([&] {
        (void)player.result("step_frame");
        status = player.outputStatus();
        return sinkNamed(status, "send").at("registered").get<bool>();
    });
    INFO(status.dump());
    REQUIRE(published);
    const auto spout = sinkNamed(status, "send");
    CHECK(spout.at("state") == "running");
    CHECK(spout.at("stop_reason") == "none");
    CHECK(spout.at("sender_name") == "send");
    CHECK(spout.at("width") == 4);
    CHECK(spout.at("height") == 4);
    CHECK(spout.at("last_error") == "");
    CHECK(sinkNamed(status, "shot").at("state") == "inactive");
    CHECK(frameFiles(output_dir).empty());
    const auto seen = player.result("test_output_spout_backend");
    CHECK(seen.at("opens") == 1);
    // RGBA8 source, BGRA8 bytes.
    CHECK(seen.at("last_pixels").get<std::vector<std::uint8_t>>() == exactPatternBgra());
}

TEST_CASE("VO-4 file, video and Spout sinks run together, and limits don't stop Spout",
          "[vo4][gpu]") {
    SpoutTestBackendEnvironment backend;
    TempProject project{"vo4_all"};
    project.writeExactPatternShader();
    auto video = videoSink("movie");
    video["id"] = "movie_sink";
    attachSpoutGraph(project, {spoutSink("send"), sinkNode("shot"), std::move(video)});
    const auto output_dir = project.outputPath();
    auto flags = outputFlags(output_dir, 3);
    flags.insert(flags.end(), {"--ffmpeg", PELICAN_FAKE_FFMPEG, "--output-spout"});
    PlayerRpc player{project, flags, "vo4_all"};
    Json status;
    const auto all_done = waitFor([&] {
        (void)player.result("step_frame");
        status = player.outputStatus();
        return sinkNamed(status, "shot").at("state") == "finished" &&
               sinkNamed(status, "movie").at("state") == "finished" &&
               sinkNamed(status, "send").at("frames_written").get<std::uint64_t>() > 3;
    });
    INFO(status.dump());
    CHECK(all_done);
    CHECK(sinkNamed(status, "shot").at("frames_written") == 3);
    CHECK(sinkNamed(status, "movie").at("frames_written") == 3);
    CHECK(sinkNamed(status, "send").at("state") == "running");
    CHECK(frameFiles(output_dir).size() == 3);
    // Never a file for a Spout frame.
    std::size_t named_send = 0;
    for (const auto &entry : fs::directory_iterator{output_dir})
        if (entry.path().filename().string().starts_with("send")) ++named_send;
    CHECK(named_send == 0);
}

TEST_CASE("VO-4 an offline run never opens, even with the flag", "[vo4][gpu]") {
    SpoutTestBackendEnvironment backend;
    TempProject project{"vo4_offline"};
    project.writeExactPatternShader();
    attachSpoutGraph(project, {spoutSink("send")});
    const auto render_out = project.root / "render.png";
    PlayerRpc player{project, {"--output-spout", "--render-out", render_out.string()},
                     "vo4_offline"};
    for (int frame = 0; frame < 3; ++frame) (void)player.result("step_frame");
    const auto spout = oneSink(player.outputStatus());
    INFO(spout.dump());
    CHECK(spout.at("state") == "inactive");
    CHECK(spout.at("stop_reason") == "offline");
    CHECK(spout.at("registered") == false);
    const auto seen = player.result("test_output_spout_backend");
    CHECK(seen.at("opens") == 0);
    CHECK(seen.at("sends") == 0);
    CHECK(outputProblems(player).empty());
}

TEST_CASE("VO-4 an authored-disabled sink stays inactive with the flag", "[vo4][gpu]") {
    SpoutTestBackendEnvironment backend;
    TempProject project{"vo4_disabled"};
    project.writeExactPatternShader();
    auto sink = spoutSink("send");
    sink["parameters"]["enabled"] = false;
    attachSpoutGraph(project, {std::move(sink)});
    PlayerRpc player{project, {"--output-spout"}, "vo4_disabled"};
    for (int frame = 0; frame < 2; ++frame) (void)player.result("step_frame");
    const auto spout = oneSink(player.outputStatus());
    INFO(spout.dump());
    CHECK(spout.at("state") == "inactive");
    CHECK(spout.at("stop_reason") == "not_enabled");
    CHECK(player.result("test_output_spout_backend").at("opens") == 0);
}

TEST_CASE("VO-4 a blocked send holds no staging slot and stops neither the render thread nor a file sink",
          "[vo4][gpu]") {
    SpoutTestBackendEnvironment backend;
    TempProject project{"vo4_blocked"};
    project.writeExactPatternShader();
    attachSpoutGraph(project, {spoutSink("send"),
                               sinkNode("shot", "fill", "color", 1, true, "raw", "drop")});
    const auto output_dir = project.outputPath();
    auto flags = outputFlags(output_dir);
    flags.emplace_back("--output-spout");
    PlayerRpc player{project, flags, "vo4_blocked"};
    // A step's answer normally includes a status, which waits for file work: a
    // worker stuck behind the send would then show up as a hung step.
    (void)player.result("test_output_step_status", {{"enabled", false}});
    (void)player.result("test_output_spout_backend", {{"block_send", true}});
    const auto send_is_stuck = waitFor([&] {
        (void)player.result("step_frame");
        return player.result("test_output_spout_backend").at("send_blocked").get<bool>();
    });
    REQUIRE(send_is_stuck);

    // More captures than the sink has slots. Disk is the witness here, because a
    // worker stuck behind the send would hold get_status too.
    const std::size_t captures = 12;
    for (std::size_t frame = 0; frame < captures; ++frame) (void)player.result("step_frame");
    const auto file_sink_ran = waitFor([&] {
        return frameFiles(output_dir).size() >= captures / 2;
    });
    CHECK(file_sink_ran);
    // One frame in the stuck send and one in the mailbox; every slot is free.
    const auto slots_free = waitFor([&] {
        return file_sink_ran &&
               sinkNamed(player.outputStatus(), "send").at("queue_depth") == 2;
    });
    CHECK(slots_free);
    const auto seen = player.result("test_output_spout_backend", {{"block_send", false}});
    CHECK(seen.at("sends") == 0);
    if (file_sink_ran) {
        const auto status = player.outputStatus();
        INFO(status.dump());
        const auto spout = sinkNamed(status, "send");
        CHECK(spout.at("state") == "running");
        CHECK(spout.at("frames_dropped").get<std::uint64_t>() > 0);
        CHECK(sinkNamed(status, "shot").at("state") == "running");
    }
}

TEST_CASE("VO-4 a reload to the same name waits for the old sender's close, without the render thread",
          "[vo4][gpu]") {
    SpoutTestBackendEnvironment backend;
    TempProject project{"vo4_reload"};
    project.writeExactPatternShader();
    attachSpoutGraph(project, {spoutSink("send")});
    PlayerRpc player{project, {"--output-spout"}, "vo4_reload"};
    const auto published = waitFor([&] {
        (void)player.result("step_frame");
        return oneSink(player.outputStatus()).at("registered").get<bool>();
    });
    REQUIRE(published);

    (void)player.result("test_output_spout_backend", {{"block_close", true}});
    auto changed = project.readRoot();
    for (auto &node : changed.at("nodes")) {
        if (node.at("op") == "render.image") {
            node["id"] = "other_target";
        }
        if (node.at("id") == "fill") {
            node["inputs"]["color"]["node"] = "other_target";
        }
    }
    project.writeRoot(changed);
    const auto settled = player.settle(player.result("reload_render_documents"));
    REQUIRE(settled.at("status") == "committed");
    const auto closing = waitFor([&] {
        return player.result("test_output_spout_backend").at("close_blocked").get<bool>();
    });
    REQUIRE(closing);

    // The render thread keeps going, and the new sender hasn't opened.
    for (int frame = 0; frame < 3; ++frame) (void)player.result("step_frame");
    auto seen = player.result("test_output_spout_backend");
    CHECK(seen.at("opens") == 1);
    CHECK(oneSink(player.outputStatus()).at("registered") == false);

    seen = player.result("test_output_spout_backend", {{"block_close", false}});
    const auto reopened = waitFor([&] {
        (void)player.result("step_frame");
        return player.result("test_output_spout_backend").at("opens") == 2 &&
               oneSink(player.outputStatus()).at("registered").get<bool>();
    });
    CHECK(reopened);
    seen = player.result("test_output_spout_backend");
    INFO(seen.dump());
    Json events = Json::array();
    for (const auto &event : seen.at("events")) {
        if (!event.get<std::string>().starts_with("send:")) events.push_back(event);
    }
    REQUIRE(events.size() == 4);
    CHECK(events.at(0) == "open:send");
    CHECK(events.at(1) == "close_begin:send");
    CHECK(events.at(2) == "close:send");
    CHECK(events.at(3) == "open:send");
    CHECK(oneSink(player.outputStatus()).at("state") == "running");
}

TEST_CASE("VO-4 a frame of a generation that's gone never reaches the backend",
          "[vo4][gpu]") {
    SpoutTestBackendEnvironment backend;
    TempProject project{"vo4_stale"};
    project.writeExactPatternShader();
    attachSpoutGraph(project, {spoutSink("send")});
    PlayerRpc player{project, {"--output-spout"}, "vo4_stale"};
    const auto published = waitFor([&] {
        (void)player.result("step_frame");
        return oneSink(player.outputStatus()).at("registered").get<bool>();
    });
    REQUIRE(published);
    const auto sendsOf = [&](const char *size) {
        const auto seen = player.result("test_output_spout_backend");
        return std::count(seen.at("events").begin(), seen.at("events").end(),
                          Json(std::string{"send:"} + size));
    };
    const auto idle = waitFor([&] {
        return oneSink(player.outputStatus()).at("queue_depth") == 0;
    });
    REQUIRE(idle);
    const auto old_sends = sendsOf("4x4");
    REQUIRE(old_sends > 0);

    // Frames captured with the worker parked, then a reload that keeps the
    // sender. The old frames are 4x4 and every later one is 8x8.
    (void)player.result("test_output_worker_pause", {{"paused", true}});
    (void)player.result("step_frame");
    (void)player.result("step_frame");
    auto resized = project.readRoot();
    for (auto &node : resized.at("nodes")) {
        if (node.at("op") == "render.image") {
            node["parameters"]["width"] = 8;
            node["parameters"]["height"] = 8;
        }
    }
    project.writeRoot(resized);
    const auto settled = player.settle(player.result("reload_render_documents"));
    REQUIRE(settled.at("status") == "committed");
    (void)player.result("test_output_worker_pause", {{"paused", false}});
    const auto new_frames = waitFor([&] {
        (void)player.result("step_frame");
        return sendsOf("8x8") > 0 &&
               oneSink(player.outputStatus()).at("queue_depth") == 0;
    });
    REQUIRE(new_frames);
    CHECK(sendsOf("4x4") == old_sends);
    const auto spout = oneSink(player.outputStatus());
    INFO(spout.dump());
    CHECK(spout.at("frames_discarded").get<std::uint64_t>() >= 2);
    CHECK(spout.at("state") == "running");
    CHECK(spout.at("width") == 8);
    CHECK(player.result("test_output_spout_backend").at("opens") == 1);
}

TEST_CASE("VO-4 an asynchronous refusal is a Problem with no further frame, and a reload retries",
          "[vo4][gpu]") {
    SpoutTestBackendEnvironment backend{"fail_open"};
    TempProject project{"vo4_problem"};
    project.writeExactPatternShader();
    attachSpoutGraph(project, {spoutSink("send")});
    PlayerRpc player{project, {"--output-spout"}, "vo4_problem"};
    // No step_frame: the sender's thread met this by itself.
    Json problems;
    const auto listed = waitFor([&] {
        problems = outputProblems(player);
        return !problems.empty();
    });
    REQUIRE(listed);
    INFO(problems.dump());
    REQUIRE(problems.size() == 1);
    CHECK(problems.at(0).at("code") == "output.spout_open_failed@1");
    CHECK(problems.at(0).at("node") == "send");
    const auto sink = oneSink(player.outputStatus());
    CHECK(sink.at("state") == "refused");
    CHECK(sink.at("registered") == false);
    (void)player.result("step_frame");
    (void)player.result("step_frame");
    CHECK(oneSink(player.outputStatus()).at("state") == "refused");
    CHECK(player.result("test_output_spout_backend").at("opens") == 1);

    auto resized = project.readRoot();
    for (auto &node : resized.at("nodes")) {
        if (node.at("op") == "render.image") node["parameters"]["width"] = 8;
    }
    project.writeRoot(resized);
    const auto settled = player.settle(player.result("reload_render_documents"));
    REQUIRE(settled.at("status") == "committed");
    const auto reopened = waitFor([&] {
        return player.result("test_output_spout_backend").at("opens") == 2;
    });
    CHECK(reopened);
}

TEST_CASE("VO-4 a source that isn't RGBA8 or BGRA8 refuses by the file sink's code",
          "[vo4][gpu]") {
    SpoutTestBackendEnvironment backend;
    TempProject project{"vo4_format"};
    project.writeExactPatternShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R16G16B16A16_SFLOAT"),
                    fillNode("fill", "fill_target", "fill_target"), spoutSink("send"));
    project.writeRoot(root);
    PlayerRpc player{project, {"--output-spout"}, "vo4_format"};
    (void)player.result("step_frame");
    const auto refused = waitFor([&] {
        return oneSink(player.outputStatus()).at("state") == "refused";
    });
    CHECK(refused);
    const auto sink = oneSink(player.outputStatus());
    INFO(sink.dump());
    CHECK(sink.at("last_error").get<std::string>().starts_with("output.format_unsupported@1"));
    CHECK(player.result("test_output_spout_backend").at("sends") == 0);
    CHECK(outputProblems(player).at(0).at("code") == "output.format_unsupported@1");
}

TEST_CASE("VO-4 UNORM and SRGB sources send the bytes the file sink writes, only swapped",
          "[vo4][gpu]") {
    struct Source {
        const char *format;
        bool bgra;
    };
    for (const auto source : {Source{"R8G8B8A8_UNORM", false}, Source{"R8G8B8A8_SRGB", false},
                              Source{"B8G8R8A8_UNORM", true}, Source{"B8G8R8A8_SRGB", true}}) {
        CAPTURE(std::string{source.format});
        SpoutTestBackendEnvironment backend;
        TempProject project{std::string{"vo4_"} + source.format};
        project.writeExactPatternShader();
        auto root = project.readRoot();
        auto &nodes = root["nodes"];
        nodes.push_back(targetNode("fill_target", source.format));
        nodes.push_back(fillNode("fill", "fill_target", "fill_target"));
        nodes.push_back(spoutSink("send"));
        nodes.push_back(sinkNode("shot"));
        project.writeRoot(root);
        const auto output_dir = project.outputPath();
        auto flags = outputFlags(output_dir);
        flags.emplace_back("--output-spout");
        PlayerRpc player{project, flags, source.format};
        const auto published = waitFor([&] {
            (void)player.result("step_frame");
            return sinkNamed(player.outputStatus(), "send").at("registered").get<bool>();
        });
        REQUIRE(published);
        const auto files = frameFiles(output_dir);
        REQUIRE_FALSE(files.empty());
        const auto &file = files.front();
        std::vector<std::uint8_t> expected(file.bytes.begin() + 16, file.bytes.end());
        if (!source.bgra) {
            for (std::size_t at = 0; at + 3 < expected.size(); at += 4)
                std::swap(expected[at], expected[at + 2]);
        }
        const auto seen = player.result("test_output_spout_backend");
        CHECK(seen.at("last_pixels").get<std::vector<std::uint8_t>>() == expected);
    }
}

#if PELICAN_WITH_SPOUT
TEST_CASE("VO-4 a real player's --output-spout is read by a Spout receiver here, pixel for pixel",
          "[vo4][gpu]") {
    const auto name = "pelican_vo4_player_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    SpoutTestReceiver receiver{name};
    if (!receiver.deviceAvailable())
        SKIP("no D3D11 device in this session: the integration test did not run");
    TempProject project{"vo4_real"};
    project.writeExactPatternShader();
    attachSpoutGraph(project, {spoutSink(name)});
    PlayerRpc player{project, {"--output-spout"}, "vo4_real"};
    const auto expected = exactPatternBgra();
    const auto received = waitFor([&] {
        (void)player.result("step_frame");
        (void)receiver.poll();
        return receiver.width() == 4 && receiver.height() == 4 &&
               receiver.pixels() == expected;
    }, std::chrono::seconds(30));
    const auto sink = oneSink(player.outputStatus());
    INFO(sink.dump());
    CHECK(received);
    CHECK(sink.at("state") == "running");
    CHECK(sink.at("registered") == true);
    CHECK(sink.at("sender_name") == name);
    CHECK(sink.at("width") == 4);
    CHECK(sink.at("height") == 4);
    CHECK(sink.at("frames_written").get<std::uint64_t>() >= 1);
    CHECK(spoutTestSenderListed(name));
    player.close();
    CHECK_FALSE(spoutTestSenderListed(name));
}
#endif

TEST_CASE("VO-7 offline video drains through the real sink and closes on engine exit",
          "[vo7][gpu]") {
    TempProject project{"vo7_offline"};
    project.writeExactPatternShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    videoSink("movie"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    const auto side_path = output_dir / "encoder.txt";
#ifdef _WIN32
    REQUIRE(_putenv_s("PELICAN_FAKE_FFMPEG_SIDE", side_path.string().c_str()) == 0);
#else
    REQUIRE(setenv("PELICAN_FAKE_FFMPEG_SIDE", side_path.string().c_str(), 1) == 0);
#endif
    auto flags = outputFlags(output_dir, 4);
    flags.insert(flags.end(), {"--ffmpeg", PELICAN_FAKE_FFMPEG});
    PlayerRpc player{project, flags, "vo7_offline"};
    // max_frames is reached in frames, but the encoder exits on its own time.
    const auto finished_by = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    while (std::chrono::steady_clock::now() < finished_by) {
        (void)player.result("step_frame");
        const auto sink = oneSink(player.outputStatus());
        if (sink.at("state") == "finished") break;
    }
    const auto sink = oneSink(player.outputStatus());
    INFO(sink.dump());
    CHECK(sink.at("kind") == "video");
    CHECK(sink.at("mode") == "offline");
    CHECK(sink.at("state") == "finished");
    CHECK(sink.at("stop_reason") == "max_frames");
    CHECK(sink.at("frames_written") == 4);
    CHECK(sink.at("frames_repeated") == 0);
    CHECK(sink.at("encoder_state") == "exited");
    player.close();
    CHECK(fs::exists(output_dir / "movie.mp4"));
    REQUIRE(fs::exists(side_path));
    const auto side = readFile(side_path);
    CHECK(side.find("bytes 256") != std::string::npos);
    CHECK(side.find("frames 4") != std::string::npos);
#ifdef _WIN32
    (void)_putenv_s("PELICAN_FAKE_FFMPEG_SIDE", "");
#else
    (void)unsetenv("PELICAN_FAKE_FFMPEG_SIDE");
#endif
}

TEST_CASE("VO-7 an invalid offline rate and odd extent are refused at runtime",
          "[vo7][gpu]") {
    TempProject project{"vo7_refusal"};
    project.writeExactPatternShader();
    auto root = project.readRoot();
    auto sink = videoSink("movie");
    sink.at("parameters").at("fps_num") = 30;
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"), sink);
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    auto flags = outputFlags(output_dir);
    flags.insert(flags.end(), {"--ffmpeg", PELICAN_FAKE_FFMPEG});
    PlayerRpc player{project, flags, "vo7_refusal"};
    (void)player.result("step_frame");
    const auto state = oneSink(player.outputStatus());
    CHECK(state.at("state") == "refused");
    CHECK(state.at("last_error") == "output.video_rate_mismatch@1");
    CHECK(outputProblems(player).at(0).at("code") == "output.video_rate_mismatch@1");
    CHECK(!fs::exists(output_dir / "movie.mp4"));
}

TEST_CASE("VO-7 encoder death does not kill the engine or stall a file sink",
          "[vo7][gpu]") {
    TempProject project{"vo7_early"};
    project.writeExactPatternShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"), videoSink("movie"));
    auto file = sinkNode("shot");
    file.at("id") = "file_sink";
    root.at("nodes").push_back(file);
    project.writeRoot(root);
#ifdef _WIN32
    REQUIRE(_putenv_s("PELICAN_FAKE_FFMPEG_EARLY_EXIT", "23") == 0);
#else
    REQUIRE(setenv("PELICAN_FAKE_FFMPEG_EARLY_EXIT", "23", 1) == 0);
#endif
    const auto output_dir = project.outputPath();
    auto flags = outputFlags(output_dir);
    flags.insert(flags.end(), {"--ffmpeg", PELICAN_FAKE_FFMPEG});
    PlayerRpc player{project, flags, "vo7_early"};
    for (int index = 0; index < 8; ++index) (void)player.result("step_frame");
    const auto status = player.outputStatus();
    REQUIRE(status.at("sinks").size() == 2);
    const auto &video = status.at("sinks").at(0);
    const auto &file_state = status.at("sinks").at(1);
    CHECK(video.at("state") == "refused");
    CHECK(video.at("last_error").get<std::string>().find("exit_code=23") !=
          std::string::npos);
    CHECK(file_state.at("frames_written").get<std::uint64_t>() >= 2);
    const auto problems = outputProblems(player);
    CHECK(problems.at(0).at("code") == "output.video_encoder_failed@1");
    CHECK(player.result("get_frame_plan").is_object());
    player.close();
#ifdef _WIN32
    (void)_putenv_s("PELICAN_FAKE_FFMPEG_EARLY_EXIT", "");
#else
    (void)unsetenv("PELICAN_FAKE_FFMPEG_EARLY_EXIT");
#endif
    CHECK(!fs::exists(output_dir / "movie.mp4"));
    CHECK(fs::exists(output_dir / "shot_000000.rgba"));
    CHECK(fs::exists(output_dir / "shot_000001.rgba"));
}

TEST_CASE("VO-7 reload removal finalizes the retired writer before releasing slots",
          "[vo7][gpu]") {
    TempProject project{"vo7_remove"};
    project.writeExactPatternShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"), videoSink("movie"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    const auto side_path = output_dir / "retired.txt";
#ifdef _WIN32
    REQUIRE(_putenv_s("PELICAN_FAKE_FFMPEG_SIDE", side_path.string().c_str()) == 0);
#else
    REQUIRE(setenv("PELICAN_FAKE_FFMPEG_SIDE", side_path.string().c_str(), 1) == 0);
#endif
    auto flags = outputFlags(output_dir);
    flags.insert(flags.end(), {"--ffmpeg", PELICAN_FAKE_FFMPEG});
    PlayerRpc player{project, flags, "vo7_remove"};
    for (int index = 0; index < 3; ++index) (void)player.result("step_frame");
    auto changed = project.readRoot();
    auto &nodes = changed.at("nodes");
    nodes.erase(std::find_if(nodes.begin(), nodes.end(),
        [](const Json &node) { return node.at("op") == "output.video"; }));
    project.writeRoot(changed);
    REQUIRE(player.settle(player.result("reload_render_documents")).at("status") ==
            "committed");
    CHECK(player.outputStatus().at("sinks").empty());
    // The retired writer's encoder exits on its own time, not in frames.
    const auto finalized_by = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    while (std::chrono::steady_clock::now() < finalized_by) {
        if (fs::exists(output_dir / "movie.mp4") && fs::exists(side_path) &&
            readFile(side_path).find("frames 3") != std::string::npos) break;
        (void)player.result("step_frame");
    }
    CHECK(fs::exists(output_dir / "movie.mp4"));
    CHECK(readFile(side_path).find("frames 3") != std::string::npos);
    player.close();
#ifdef _WIN32
    (void)_putenv_s("PELICAN_FAKE_FFMPEG_SIDE", "");
#else
    (void)unsetenv("PELICAN_FAKE_FFMPEG_SIDE");
#endif
}

// VO-5a: the extension report and the GPU path check of a real player. The
// sends themselves are VO-4's cases above, which don't change.
namespace {

bool vo5aExpectsAvailable() { return std::getenv("PELICAN_VO5A_EXPECT_AVAILABLE") != nullptr; }

// One player's status. With PELICAN_VO1A_VULKAN_VALIDATION it proves validation
// was on before it takes "no VUID" for an answer.
Json vo5aRunPlayer(const TempProject &project, std::vector<std::string> flags,
                   std::string_view name,
                   PlayerRpc::Display display = PlayerRpc::Display::headless) {
    Json status;
    {
        PlayerRpc player{project, std::move(flags), name, display};
        status = player.result("get_status");
        player.close();
        // Closing stdin doesn't end a windowed player: close() terminates it.
        if (display == PlayerRpc::Display::headless) CHECK(player.exitCode() == 0);
    }
    INFO(status.at("vulkan_validation").dump());
    const auto log = readFile(project.logPath(name));
    if (std::getenv("PELICAN_VO1A_VULKAN_VALIDATION") != nullptr) {
        REQUIRE(status.at("vulkan_validation").at("enabled") == true);
    }
    CHECK(log.find("VUID-") == std::string::npos);
    return status;
}

std::vector<std::string> vo5aEnabled(const Json &status) {
    return status.at("vulkan_extensions").at("device").at("enabled").get<std::vector<std::string>>();
}

void vo5aCheckTheThreeAtTheEnd(const Json &without, const Json &with) {
    INFO(without.at("vulkan_extensions").dump());
    INFO(with.at("vulkan_extensions").dump());
    auto expected = vo5aEnabled(without);
    const std::vector<std::string> names{"VK_KHR_external_memory_win32",
                                         "VK_KHR_external_semaphore_win32",
                                         "VK_KHR_win32_keyed_mutex"};
    const auto &rows = with.at("vulkan_extensions").at("device").at("rows");
    REQUIRE(rows.size() == names.size());
    for (std::size_t index = 0; index < names.size(); ++index) {
        CHECK(rows[index].at("name") == names[index]);
        CHECK(rows[index].at("need") == "optional");
        CHECK(rows[index].at("requested_by") == Json::array({"output.spout"}));
        if (rows[index].at("outcome") == "enabled") expected.push_back(names[index]);
    }
    CHECK(vo5aEnabled(with) == expected);
    CHECK(with.at("vulkan_extensions").at("device_created_by") == "engine");
    CHECK(with.at("vulkan_extensions").at("list_scope") == "engine_requested");
    CHECK(with.at("vulkan_extensions").at("instance") ==
          without.at("vulkan_extensions").at("instance"));
    const auto &path = with.at("output").at("spout_gpu_path");
    INFO(path.dump());
    CHECK_FALSE(path.at("handles").is_null());
    if (vo5aExpectsAvailable()) {
        CHECK(expected.size() == vo5aEnabled(without).size() + names.size());
        CHECK(path.at("state") == "available");
        CHECK(path.at("reason") == "none");
    }
}

} // namespace

TEST_CASE("VO-5a a player without the flag has no rows and doesn't ask", "[vo5a][gpu]") {
    SpoutTestBackendEnvironment backend;
    TempProject project{"vo5a_no_flag"};
    project.writeExactPatternShader();
    attachSpoutGraph(project, {spoutSink("send")});
    const auto status = vo5aRunPlayer(project, {}, "vo5a_no_flag");
    INFO(status.at("vulkan_extensions").dump());
    const auto &extensions = status.at("vulkan_extensions");
    CHECK(extensions.at("device").at("rows").empty());
    CHECK(extensions.at("instance").at("rows").empty());
    CHECK(extensions.at("device_created_by") == "engine");
    const auto enabled = vo5aEnabled(status);
    for (const auto *name : {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32",
                             "VK_KHR_win32_keyed_mutex"}) {
        CHECK(std::find(enabled.begin(), enabled.end(), name) == enabled.end());
    }
    const auto &path = status.at("output").at("spout_gpu_path");
    INFO(path.dump());
    CHECK(path.at("state") == "unavailable");
#if !defined(_WIN32)
    CHECK(path.at("reason") == "platform_unsupported");
#elif !PELICAN_WITH_SPOUT
    CHECK(path.at("reason") == "build_without_spout");
#else
    CHECK(path.at("reason") == "not_requested");
#endif
    CHECK(path.at("handles").is_null());
}

#if PELICAN_WITH_SPOUT
TEST_CASE("VO-5a --output-spout adds the three at the end of the flag-off list", "[vo5a][gpu]") {
    SpoutTestBackendEnvironment backend;
    TempProject project{"vo5a_pair"};
    project.writeExactPatternShader();
    attachSpoutGraph(project, {spoutSink("send")});
    const auto without = vo5aRunPlayer(project, {}, "vo5a_without");
    const auto with = vo5aRunPlayer(project, {"--output-spout"}, "vo5a_with");
    vo5aCheckTheThreeAtTheEnd(without, with);
}

TEST_CASE("VO-5a a windowed player keeps the swapchain's place and adds the three last",
          "[vo5a][gpu]") {
    SpoutTestBackendEnvironment backend;
    TempProject project{"vo5a_window"};
    project.writeExactPatternShader();
    attachSpoutGraph(project, {spoutSink("send")});
    const auto without =
        vo5aRunPlayer(project, {}, "vo5a_win_without", PlayerRpc::Display::windowed);
    const auto with = vo5aRunPlayer(project, {"--output-spout"}, "vo5a_win_with",
                                    PlayerRpc::Display::windowed);
    vo5aCheckTheThreeAtTheEnd(without, with);
    REQUIRE_FALSE(vo5aEnabled(without).empty());
    CHECK(vo5aEnabled(without).front() == "VK_KHR_swapchain");
    CHECK(vo5aEnabled(with).front() == "VK_KHR_swapchain");
}

TEST_CASE("VO-5a the sink behaves as in VO-4, with the extensions on", "[vo5a][gpu]") {
    SpoutTestBackendEnvironment backend;
    TempProject project{"vo5a_sink"};
    project.writeExactPatternShader();
    attachSpoutGraph(project, {spoutSink("send")});
    PlayerRpc player{project, {"--output-spout"}, "vo5a_sink"};
    Json status;
    const auto published = waitFor([&] {
        (void)player.result("step_frame");
        status = player.outputStatus();
        return sinkNamed(status, "send").at("registered").get<bool>();
    });
    INFO(status.dump());
    REQUIRE(published);
    const auto spout = sinkNamed(status, "send");
    CHECK(spout.at("state") == "running");
    CHECK(spout.at("stop_reason") == "none");
    CHECK(spout.at("last_error") == "");
    CHECK(spout.at("frames_written").get<std::uint64_t>() >= 1);
    CHECK(player.result("test_output_spout_backend").at("last_pixels").get<std::vector<std::uint8_t>>() ==
          exactPatternBgra());
    // The GPU path's answer makes no Problem, whichever it is.
    CHECK(outputProblems(player).empty());
}
#endif
