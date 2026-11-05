// MD-1a0: the diagnostic output window. A second window shows a frame-number
// pattern while the main target keeps rendering the project's own picture.
// Everything here goes through the production path: a real windowed
// `pelican_player --rpc` child, line by line over its stdin/stdout.

#include "../src/project/projecttemplate.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>
// The decoder is this executable's own: it links no engine library that has it.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr std::uint32_t kBarCount = 8;
constexpr int kMainWidth = 320;
constexpr int kMainHeight = 240;
constexpr int kOutputWidth = 200;
constexpr int kOutputHeight = 150;

std::string readFile(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// A fresh Blank project: one fullscreen pass drawing a fixed UV gradient. The
// picture does not depend on the frame number, so two runs can compare it.
struct TempProject {
    std::filesystem::path root;
    std::filesystem::path user_dir;

    TempProject() {
        static std::atomic<int> serial{0};
        root = std::filesystem::temp_directory_path() /
               ("pelican_md1a0_" + std::to_string(::getpid()) + "_" +
                std::to_string(serial.fetch_add(1)) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        user_dir = root / "user";
        (void)Pelican::initializeProjectTemplate(root, Pelican::ProjectTemplate::Blank);

        // --size selects the headless render extent; the windowed main window
        // takes its extent from the project. Keep the main and output extents
        // different so the camera-extent assertion can catch a second writer.
        const auto config_path = root / "project.json";
        std::ifstream config_input{config_path};
        Json config;
        config_input >> config;
        config["basic_config"]["window_size"] = {{"width", kMainWidth}, {"height", kMainHeight}};
        std::ofstream config_output{config_path, std::ios::trunc};
        config_output << config.dump(2) << '\n';
        if (!config_input || !config_output) throw std::runtime_error("could not write project window size");
    }
    ~TempProject() {
        if (std::getenv("PELICAN_MD1A_KEEP_TEMP_PROJECT") != nullptr) {
            std::cerr << "PELICAN_MD1A_TEMP_PROJECT=" << root << '\n';
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void writeDisplayShader(const char *fragment) const {
        std::filesystem::create_directories(root / "shaders");
        std::ofstream output{root / "shaders" / "md1a_display.frag", std::ios::trunc};
        output << fragment;
        output.close();
        const auto path = root / "passes" / "main_rendering_config.json";
        std::ifstream input{path};
        Json config;
        input >> config;
        auto &passes = config.at("rendering").at("rendering_passes").at(0).at("passes");
        passes.at(0).at("shader").at("fragment") = "project://shaders/md1a_display";
        std::ofstream rewritten{path, std::ios::trunc};
        rewritten << config.dump(2) << '\n';
        REQUIRE(output.good());
    }

    TempProject(const TempProject &) = delete;
    TempProject &operator=(const TempProject &) = delete;
};

std::vector<std::string> playerArguments(const TempProject &project, const std::vector<std::string> &extra) {
    std::vector<std::string> arguments{
        PELICAN_MD1A_PLAYER,
        "--project", project.root.generic_string(),
        "--user-dir", project.user_dir.generic_string(),
        "--size", std::to_string(kMainWidth) + "x" + std::to_string(kMainHeight),
    };
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

struct ExitResult {
    int code = -1;
    std::string output;
};

// A launch refused before RPC: stdin is closed at once and both output streams
// are read until the child leaves.
ExitResult runToExit(const TempProject &project, const std::vector<std::string> &extra,
                     const std::filesystem::path &log) {
    auto arguments = playerArguments(project, extra);
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);

    int pipe_fds[2]{};
    REQUIRE(::pipe(pipe_fds) == 0);
    const auto log_path = log.generic_string();
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        (void)::dup2(pipe_fds[1], STDOUT_FILENO);
        (void)::dup2(pipe_fds[1], STDERR_FILENO);
        (void)::close(pipe_fds[0]);
        (void)::close(pipe_fds[1]);
        const int error_fd = ::open(log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (error_fd >= 0) {
            (void)::dup2(error_fd, STDERR_FILENO);
            (void)::close(error_fd);
        }
        (void)::chdir(project.root.c_str());
        (void)::execv(argv[0], argv.data());
        _exit(127);
    }
    (void)::close(pipe_fds[1]);

    ExitResult result;
    char buffer[4096];
    for (;;) {
        const auto read = ::read(pipe_fds[0], buffer, sizeof buffer);
        if (read > 0) {
            result.output.append(buffer, static_cast<std::size_t>(read));
        } else if (read == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }
    (void)::close(pipe_fds[0]);

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    const auto wait = [&] {
        const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) return true;
        if (waited < 0 && errno != EINTR) return true;
        if (std::chrono::steady_clock::now() > deadline) {
            (void)::kill(child, SIGTERM);
            (void)::waitpid(child, &status, 0);
            return true;
        }
        std::this_thread::sleep_for(10ms);
        return false;
    };
    while (!wait()) {
    }
    if (WIFEXITED(status)) result.code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.code = 128 + WTERMSIG(status);
    // The rejected launch's named error is on stderr; the helper reads it back
    // so a caller can assert on both streams at once.
    result.output += readFile(log);
    return result;
}

// One windowed RPC player. Requests go out one line at a time; the reader
// thread collects answers so a missing answer is a timed-out test, not a hang.
class PlayerRpc {
    pid_t pid_ = -1;
    int stdin_write_ = -1;
    int stdout_read_ = -1;
    std::filesystem::path stderr_log_;
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<std::string> lines_;
    bool closed_ = false;
    int next_id_ = 1;
    std::optional<int> exit_code_;

    void readLoop() {
        std::string pending;
        char buffer[8192];
        for (;;) {
            const auto read = ::read(stdout_read_, buffer, sizeof buffer);
            if (read > 0) {
                pending.append(buffer, static_cast<std::size_t>(read));
                for (auto end = pending.find('\n'); end != std::string::npos;
                     end = pending.find('\n')) {
                    auto line = pending.substr(0, end);
                    pending.erase(0, end + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;
                    std::scoped_lock lock{mutex_};
                    lines_.push_back(std::move(line));
                    arrived_.notify_all();
                }
            } else if (read == 0) {
                break;
            } else if (errno != EINTR) {
                break;
            }
        }
        std::scoped_lock lock{mutex_};
        closed_ = true;
        arrived_.notify_all();
    }

  public:
    PlayerRpc(const TempProject &project, const std::filesystem::path &stderr_log,
              const std::vector<std::string> &extra,
              const std::vector<std::pair<std::string, std::string>> &environment = {})
        : stderr_log_(stderr_log) {
        auto arguments = playerArguments(project, extra);
        arguments.insert(arguments.begin() + 1, "--rpc");
        std::vector<char *> argv;
        argv.reserve(arguments.size() + 1);
        for (auto &argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);

        int input_fds[2]{};
        int output_fds[2]{};
        REQUIRE(::pipe(input_fds) == 0);
        REQUIRE(::pipe(output_fds) == 0);
        const auto log_path = stderr_log_.generic_string();
        const auto root_path = project.root.generic_string();

        // Python's subprocess would have hidden this; a player writing to a
        // pipe whose test has already left must not kill the test process.
        (void)::signal(SIGPIPE, SIG_IGN);

        pid_ = ::fork();
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            (void)::dup2(input_fds[0], STDIN_FILENO);
            (void)::dup2(output_fds[1], STDOUT_FILENO);
            (void)::close(input_fds[0]);
            (void)::close(input_fds[1]);
            (void)::close(output_fds[0]);
            (void)::close(output_fds[1]);
            const int error_fd = ::open(log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (error_fd >= 0) {
                (void)::dup2(error_fd, STDERR_FILENO);
                (void)::close(error_fd);
            }
            for (const auto &[name, value] : environment) (void)::setenv(name.c_str(), value.c_str(), 1);
            (void)::chdir(root_path.c_str());
            (void)::execv(argv[0], argv.data());
            _exit(127);
        }
        (void)::close(input_fds[0]);
        (void)::close(output_fds[1]);
        stdin_write_ = input_fds[1];
        stdout_read_ = output_fds[0];
        reader_ = std::thread{[this] { readLoop(); }};
    }

    ~PlayerRpc() {
        close();
        if (reader_.joinable()) reader_.join();
        if (stdout_read_ >= 0) (void)::close(stdout_read_);
    }
    PlayerRpc(const PlayerRpc &) = delete;
    PlayerRpc &operator=(const PlayerRpc &) = delete;

    // Ends the input and waits for the player to leave. A player that does not
    // leave is this test's own child, stopped so the run cannot hang.
    void close(int timeout_seconds = 180) {
        if (stdin_write_ >= 0) {
            (void)::close(stdin_write_);
            stdin_write_ = -1;
        }
        if (pid_ <= 0 || exit_code_) return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{timeout_seconds};
        int status = 0;
        for (;;) {
            const auto waited = ::waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) break;
            if (waited < 0 && errno != EINTR) {
                status = 0;
                break;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                (void)::kill(pid_, SIGTERM);
                for (int attempt = 0; attempt < 50; ++attempt) {
                    if (::waitpid(pid_, &status, WNOHANG) == pid_) break;
                    std::this_thread::sleep_for(100ms);
                }
                if (::waitpid(pid_, &status, WNOHANG) == 0) (void)::kill(pid_, SIGKILL);
                (void)::waitpid(pid_, &status, 0);
                break;
            }
            std::this_thread::sleep_for(10ms);
        }
        if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) exit_code_ = 128 + WTERMSIG(status);
        else exit_code_ = 1;
        pid_ = -1;
    }

    [[nodiscard]] int exitCode() const {
        REQUIRE(exit_code_.has_value());
        return *exit_code_;
    }

    [[nodiscard]] std::string stderrText() const { return readFile(stderr_log_); }

    Json call(const std::string &method, const Json &params = Json::object(),
              std::chrono::seconds timeout = 180s) {
        REQUIRE(stdin_write_ >= 0);
        const int id = next_id_++;
        const auto line =
            Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump() + "\n";
        std::size_t written = 0;
        while (written < line.size()) {
            const auto chunk = ::write(stdin_write_, line.data() + written, line.size() - written);
            if (chunk > 0) {
                written += static_cast<std::size_t>(chunk);
            } else if (chunk < 0 && errno == EINTR) {
                continue;
            } else {
                FAIL("writing " << method << " to the player failed: " << std::strerror(errno));
            }
        }
        std::unique_lock lock{mutex_};
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            while (!lines_.empty()) {
                auto response = Json::parse(lines_.front());
                lines_.pop_front();
                if (response.value("id", -1) == id) return response;
            }
            INFO("waiting for the answer to " << method);
            REQUIRE_FALSE(closed_);
            REQUIRE(arrived_.wait_until(lock, deadline) != std::cv_status::timeout);
        }
    }

    Json result(const std::string &method, const Json &params = Json::object(),
                std::chrono::seconds timeout = 180s) {
        auto response = call(method, params, timeout);
        if (!response.contains("result")) INFO(method << " -> " << response.dump());
        REQUIRE(response.contains("result"));
        return response.at("result");
    }

    Json status(std::chrono::seconds timeout = 180s) {
        return result("get_status", Json::object(), timeout);
    }

    Json waitFor(const std::string &what, const std::function<bool(const Json &)> &predicate,
                 std::chrono::seconds timeout = 30s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto remaining = std::max(
                1s, std::chrono::duration_cast<std::chrono::seconds>(deadline - std::chrono::steady_clock::now()));
            auto current = status(remaining);
            if (predicate(current)) return current;
            if (std::chrono::steady_clock::now() > deadline) {
                FAIL("timed out waiting for " << what << "; last status: " << current.dump());
            }
            std::this_thread::sleep_for(20ms);
        }
    }
};

// The Blank picture is fixed; this is the capture the existing RPC exposes.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
};

Image readPng(const std::filesystem::path &path) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    REQUIRE(pixels != nullptr);
    Image image{width, height, std::vector<unsigned char>(
                               pixels, pixels + static_cast<std::size_t>(width) * height * 4)};
    stbi_image_free(pixels);
    return image;
}

Image captureMain(PlayerRpc &player, const std::filesystem::path &path) {
    std::filesystem::remove(path);
    (void)player.result("capture", {{"path", path.generic_string()}});
    return readPng(path);
}

struct Ppm {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<unsigned char> rgb;
};

std::string readPpmToken(std::istream &input) {
    std::string token;
    while (input >> token) {
        if (!token.empty() && token.front() == '#') {
            std::string ignored;
            std::getline(input, ignored);
            continue;
        }
        return token;
    }
    return {};
}

Ppm readPpm(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.good());
    REQUIRE(readPpmToken(input) == "P6");
    const auto width = std::stoul(readPpmToken(input));
    const auto height = std::stoul(readPpmToken(input));
    REQUIRE(readPpmToken(input) == "255");
    input.get(); // the single whitespace byte after maxval
    Ppm ppm{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
            std::vector<unsigned char>(static_cast<std::size_t>(width) * height * 3)};
    input.read(reinterpret_cast<char *>(ppm.rgb.data()), static_cast<std::streamsize>(ppm.rgb.size()));
    REQUIRE(input.gcount() == static_cast<std::streamsize>(ppm.rgb.size()));
    return ppm;
}

std::array<unsigned char, 3> ppmAt(const Ppm &ppm, std::uint32_t x, std::uint32_t y) {
    const auto offset = (static_cast<std::size_t>(y) * ppm.width + x) * 3;
    return {ppm.rgb[offset], ppm.rgb[offset + 1], ppm.rgb[offset + 2]};
}

// The output window's title, read from the X server rather than inferred from
// engine state: state "off" must mean no second top-level window exists.
int outputWindowCount() {
    const char *display = std::getenv("DISPLAY");
    if (display == nullptr || *display == '\0') return -1;
    const std::string command = "xwininfo -root -tree -display \"" + std::string{display} + "\" 2>/dev/null";
    FILE *pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) return -1;
    std::string tree;
    char buffer[4096];
    while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) tree += buffer;
    (void)::pclose(pipe);
    // xwininfo repeats the WM_NAME as the instance and class name on the
    // window's own line, so count lines that name it, not raw occurrences.
    int count = 0;
    std::istringstream lines{tree};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("\"Pelican output\"") != std::string::npos) ++count;
    }
    return count;
}

// `get_frame_plan` carries per-run handles and byte counts. The MD-1a0 claim
// is that the output window adds no graph work, so compare the authored
// identity and delivery of every node, not the raw report.
Json normalizedFramePlan(const Json &plan) {
    Json nodes = Json::array();
    for (const auto &node : plan.at("nodes")) {
        nodes.push_back(Json{
            {"kind", node.at("kind")},
            {"name", node.at("name")},
            {"order", node.at("order")},
            {"reads", node.at("reads")},
            {"writes", node.at("writes")},
            {"reads_history", node.at("reads_history")},
            {"schedule", node.contains("schedule") ? node.at("schedule") : Json{}},
            {"declaration_index", node.at("declaration_index")},
            {"pass_inputs", node.contains("pass_inputs") ? node.at("pass_inputs") : Json{}},
            {"resource_bindings", node.contains("resource_bindings") ? node.at("resource_bindings") : Json{}},
            {"incoming_barriers", node.contains("incoming_barriers") ? node.at("incoming_barriers") : Json{}},
            {"view_family", node.contains("view_family") ? node.at("view_family") : Json{}},
            {"execution", node.contains("execution") ? node.at("execution") : Json{}},
        });
    }
    return Json{
        {"version", plan.at("version")},
        {"graph", plan.at("graph")},
        {"nodes", std::move(nodes)},
    };
}

bool hasRegisteredTarget(const Json &plan, std::string_view name) {
    if (!plan.contains("gpu_resource_arena")) return false;
    for (const auto &scope : plan.at("gpu_resource_arena").at("scopes")) {
        for (const auto &resource : scope.at("resources")) {
            if (resource.value("kind", "") == "render_target" &&
                resource.value("name", "") == name) return true;
        }
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> outputEnvironment(
    bool dump_pattern = false, const std::filesystem::path &dump_dir = {}, int dump_count = 2) {
    std::vector<std::pair<std::string, std::string>> environment;
    if (dump_pattern) {
        environment.emplace_back("PELICAN_MD1A_PATTERN_DUMP_DIR", dump_dir.generic_string());
        environment.emplace_back("PELICAN_MD1A_PATTERN_DUMP_COUNT", std::to_string(dump_count));
    }
    return environment;
}

const std::vector<std::string> output_flags{
    "--output-window", "0", "--output-window-size",
    std::to_string(kOutputWidth) + "x" + std::to_string(kOutputHeight),
    "--output-window-pattern"};

std::vector<std::string> outputFlagsWithFrames(int frames) {
    auto arguments = output_flags;
    arguments.push_back("--frames");
    arguments.push_back(std::to_string(frames));
    return arguments;
}

std::vector<std::string> displayOutputFlagsWithFrames(int frames) {
    auto arguments = outputFlagsWithFrames(frames);
    arguments.erase(std::find(arguments.begin(), arguments.end(), "--output-window-pattern"));
    return arguments;
}

} // namespace

TEST_CASE("MD-1a0 output_window option errors are named and headless/XR are refused", "[md1a]") {
    TempProject project;
    const auto log = project.root / "options.stderr.log";

    SECTION("headless is checked before the missing pattern") {
        const auto result =
            runToExit(project, {"--headless", "--output-window", "0", "--output-window-pattern"}, log);
        INFO(result.output);
        CHECK(result.code != 0);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("output_window.headless@1"));
    }

    SECTION("XR active is refused, not silently ignored") {
        const auto result = runToExit(
            project, {"--xr", "on", "--output-window", "0", "--output-window-pattern"}, log);
        INFO(result.output);
        CHECK(result.code != 0);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("output_window.xr_unsupported@1"));
    }

    SECTION("a bad monitor index is a named configuration error") {
        const auto result =
            runToExit(project, {"--output-window", "not-a-number", "--output-window-pattern"}, log);
        INFO(result.output);
        CHECK(result.code != 0);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("output_window.config_invalid@1"));
    }

    SECTION("a bad size is a named configuration error") {
        const auto result = runToExit(
            project, {"--output-window", "0", "--output-window-size", "0x10", "--output-window-pattern"},
            log);
        INFO(result.output);
        CHECK(result.code != 0);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("output_window.config_invalid@1"));
    }

    SECTION("a size without an output window is a named configuration error") {
        const auto result =
            runToExit(project, {"--output-window-size", "320x240", "--output-window-pattern"}, log);
        INFO(result.output);
        CHECK(result.code != 0);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("output_window.config_invalid@1"));
    }

    SECTION("a pattern without an output window is a named configuration error") {
        const auto result = runToExit(project, {"--output-window-pattern"}, log);
        INFO(result.output);
        CHECK(result.code != 0);
        CHECK_THAT(result.output, Catch::Matchers::ContainsSubstring("output_window.config_invalid@1"));
    }
}

TEST_CASE("MD-1a0 output_window is off without the option and does not change the frame plan",
          "[md1a]") {
    TempProject project;

    Json off_status;
    Json off_plan;
    {
        PlayerRpc player{project, project.root / "off.stderr.log", {"--frames", "120"}};
        off_status = player.waitFor("the main loop", [](const Json &status) {
            return status.at("window_output").at("submits").get<std::uint64_t>() >= 3;
        });
        off_plan = player.result("get_frame_plan");
        CHECK(outputWindowCount() == 0);
        player.close();
        CHECK(player.exitCode() == 0);
    }

    const auto &off = off_status.at("output_window");
    CHECK(off.at("state") == "off");
    CHECK(off.at("monitor").is_null());
    CHECK(off.at("width") == 0);
    CHECK(off.at("height") == 0);
    CHECK(off.at("present_mode").is_null());
    CHECK(off.at("presented") == 0);
    CHECK(off.at("dropped") == 0);
    CHECK(off.at("captures") == 0);
    CHECK(off.at("capture_skipped") == 0);
    CHECK(off.at("capture_fence_waits") == 0);
    CHECK(off.at("last_error").is_null());
    CHECK(off.at("present_ms_max") == 0.0);
    CHECK_FALSE(hasRegisteredTarget(off_plan, "output_window_bridge"));
    CHECK(off_plan.at("nodes").dump().find("output_window_capture") == std::string::npos);
    CHECK(off_status.at("play").at("camera").at("viewport_width") == kMainWidth);
    CHECK(off_status.at("play").at("camera").at("viewport_height") == kMainHeight);

    Json on_status;
    Json on_plan;
    {
        PlayerRpc player{project, project.root / "on.stderr.log", outputFlagsWithFrames(120)};
        on_status = player.waitFor("the output window to present", [](const Json &status) {
            const auto &window = status.at("output_window");
            return window.at("state") == "running" &&
                   window.at("presented").get<std::uint64_t>() >= 3;
        });
        on_plan = player.result("get_frame_plan");
        CHECK(outputWindowCount() == 1);
        player.close();
        CHECK(player.exitCode() == 0);
    }

    const auto &on = on_status.at("output_window");
    CHECK(on.at("state") == "running");
    CHECK(on.at("monitor") == 0);
    CHECK(on.at("width") == kOutputWidth);
    CHECK(on.at("height") == kOutputHeight);
    const auto present_mode = on.at("present_mode").get<std::string>();
    INFO("present mode " << present_mode);
    CHECK((present_mode == "FIFO" || present_mode == "MAILBOX"));
    CHECK(present_mode != "IMMEDIATE");
    CHECK(on.at("presented").get<std::uint64_t>() >= 3);
    CHECK(on.at("captures") == 0);
    CHECK(on.at("capture_skipped") == 0);
    CHECK(on.at("last_error").is_null());
    CHECK(on_status.at("play").at("camera").at("viewport_width") == kMainWidth);
    CHECK(on_status.at("play").at("camera").at("viewport_height") == kMainHeight);
    CHECK_FALSE(hasRegisteredTarget(on_plan, "output_window_bridge"));
    CHECK(normalizedFramePlan(on_plan) == normalizedFramePlan(off_plan));

    Json display_plan;
    {
        PlayerRpc player{project, project.root / "display-plan.stderr.log",
                         displayOutputFlagsWithFrames(600)};
        (void)player.waitFor("display captures", [](const Json &status) {
            return status.at("output_window").at("captures").get<std::uint64_t>() >= 3;
        });
        display_plan = player.result("get_frame_plan");
        player.close();
        CHECK(player.exitCode() == 0);
    }
    CHECK(hasRegisteredTarget(display_plan, "output_window_bridge"));
    CHECK(normalizedFramePlan(display_plan) == normalizedFramePlan(off_plan));
    CHECK(display_plan.at("physical_target_plan").dump().find("output_window_bridge") ==
          std::string::npos);
}

TEST_CASE("MD-1a0 output_window pattern bars follow the frame number and main pixels stay unchanged",
          "[md1a]") {
    TempProject project;

    // The main picture without the second window.
    Image without;
    {
        PlayerRpc player{project, project.root / "without.stderr.log", {"--frames", "80"}};
        (void)player.waitFor("the main loop", [](const Json &status) {
            return status.at("window_output").at("submits").get<std::uint64_t>() >= 5;
        });
        const auto capture_status = player.status();
        const auto &capture = capture_status.at("color");
        INFO(capture.dump());
        if (!capture.contains("capture") || capture.at("capture") != "available") {
            SKIP("capture unavailable_windowed: the main target has no transfer source");
        }
        without = captureMain(player, project.root / "main-without.png");
        player.close();
        CHECK(player.exitCode() == 0);
    }

    Image with;
    {
        const auto dump_dir = project.root / "pattern";
        std::filesystem::create_directories(dump_dir);
        PlayerRpc player{project, project.root / "with.stderr.log", outputFlagsWithFrames(120),
                         outputEnvironment(true, dump_dir)};
        const auto status = player.waitFor("the pattern to be written", [](const Json &status) {
            const auto &window = status.at("output_window");
            return window.at("state") == "running" &&
                   window.at("presented").get<std::uint64_t>() >= 3;
        });
        INFO(status.at("output_window").dump());

        std::vector<std::filesystem::path> dumps;
        for (const auto &entry : std::filesystem::directory_iterator{dump_dir}) {
            if (entry.is_regular_file() && entry.path().extension() == ".ppm") {
                dumps.push_back(entry.path());
            }
        }
        std::sort(dumps.begin(), dumps.end());
        if (dumps.size() < 2) {
            const auto stderr = player.stderrText();
            if (stderr.find("PELICAN_MD1A_PATTERN_CAPTURE_SKIPPED") != std::string::npos) {
                SKIP("PATTERN_CAPTURE_SKIPPED: capture unavailable_windowed: "
                     "the output surface lacks TRANSFER_SRC support");
            }
        }
        REQUIRE(dumps.size() >= 2);
        for (const auto &dump : dumps) {
            const auto name = dump.stem().string();
            REQUIRE(name.starts_with("pattern_"));
            const auto frame = std::stoul(name.substr(std::strlen("pattern_")));
            const auto ppm = readPpm(dump);
            INFO("dump " << dump << " frame " << frame);
            REQUIRE(ppm.width == kOutputWidth);
            REQUIRE(ppm.height == kOutputHeight);
            for (std::uint32_t bar = 0; bar < kBarCount; ++bar) {
                const auto x = bar * ppm.width / kBarCount + ppm.width / (2 * kBarCount);
                const auto expected =
                    ((bar + frame) & 1u) == 0u ? std::array<unsigned char, 3>{255, 255, 255}
                                               : std::array<unsigned char, 3>{0, 0, 0};
                CHECK(ppmAt(ppm, x, ppm.height / 2) == expected);
            }
        }

        (void)player.waitFor("the dump to finish", [](const Json &status) {
            return status.at("output_window").at("presented").get<std::uint64_t>() >= 5;
        });
        with = captureMain(player, project.root / "main-with.png");
        player.close();
        CHECK(player.exitCode() == 0);
    }

    INFO("main capture " << without.width << "x" << without.height << " vs " << with.width << "x"
                         << with.height);
    REQUIRE(without.width == with.width);
    REQUIRE(without.height == with.height);
    REQUIRE(without.width == kMainWidth);
    REQUIRE(without.height == kMainHeight);
    REQUIRE(without.rgba.size() == with.rgba.size());
    std::size_t first_difference = without.rgba.size();
    std::size_t difference_count = 0;
    for (std::size_t index = 0; index < without.rgba.size(); ++index) {
        if (without.rgba[index] == with.rgba[index]) continue;
        if (first_difference == without.rgba.size()) first_difference = index;
        ++difference_count;
    }
    if (difference_count != 0) {
        const auto pixel = first_difference / 4;
        INFO(difference_count << " differing bytes; first at byte " << first_difference << " ("
                              << (pixel % kMainWidth) << ',' << (pixel / kMainWidth) << ") without="
                              << static_cast<int>(without.rgba[first_difference]) << " with="
                              << static_cast<int>(with.rgba[first_difference]));
    }
    CHECK(difference_count == 0);
}

TEST_CASE("MD-1a1 output_window copies display before the UI", "[md1a1]") {
    TempProject project;
    Image main_picture;
    {
        PlayerRpc player{project, project.root / "display-main.stderr.log", {"--frames", "120"}};
        (void)player.waitFor("the main loop", [](const Json &status) {
            return status.at("window_output").at("submits").get<std::uint64_t>() >= 5;
        });
        main_picture = captureMain(player, project.root / "display-main.png");
        player.close();
        CHECK(player.exitCode() == 0);
    }
    const auto dump_dir = project.root / "display-output";
    std::filesystem::create_directories(dump_dir);
    PlayerRpc player{project, project.root / "display.stderr.log", displayOutputFlagsWithFrames(120),
                     outputEnvironment(true, dump_dir)};
    const auto status = player.waitFor("display captures and output presents", [](const Json &s) {
        const auto &window = s.at("output_window");
        return window.at("state") == "running" && window.at("captures").get<std::uint64_t>() >= 3 &&
               window.at("presented").get<std::uint64_t>() >= 3;
    });
    INFO(status.at("output_window").dump());
    CHECK(status.at("output_window").at("source") == "display");
    CHECK(status.at("output_window").at("last_error").is_null());
    std::vector<std::filesystem::path> dumps;
    for (const auto &entry : std::filesystem::directory_iterator{dump_dir}) {
        if (entry.path().extension() == ".ppm") dumps.push_back(entry.path());
    }
    REQUIRE(dumps.size() >= 2);
    std::sort(dumps.begin(), dumps.end());
    const auto output = readPpm(dumps.back());
    REQUIRE(output.width == kOutputWidth);
    REQUIRE(output.height == kOutputHeight);
    for (const auto [x, y] : {std::pair{30u, 30u}, {100u, 75u}, {170u, 120u}}) {
        const auto expected = ppmAt({static_cast<std::uint32_t>(main_picture.width),
                                     static_cast<std::uint32_t>(main_picture.height),
                                     [&] {
                                         std::vector<unsigned char> rgb;
                                         for (std::size_t i = 0; i < main_picture.rgba.size(); i += 4) {
                                             rgb.insert(rgb.end(), main_picture.rgba.begin() + static_cast<std::ptrdiff_t>(i),
                                                        main_picture.rgba.begin() + static_cast<std::ptrdiff_t>(i + 3));
                                         }
                                         return rgb;
                                     }()},
                                    x * kMainWidth / kOutputWidth, y * kMainHeight / kOutputHeight);
        const auto actual = ppmAt(output, x, y);
        INFO("pixel " << x << ',' << y << " expected=" << static_cast<int>(expected[0]) << ','
                      << static_cast<int>(expected[1]) << ',' << static_cast<int>(expected[2]) << " actual="
                      << static_cast<int>(actual[0]) << ',' << static_cast<int>(actual[1]) << ','
                      << static_cast<int>(actual[2]));
        for (int channel = 0; channel < 3; ++channel) {
            CHECK(std::abs(static_cast<int>(actual[channel]) - static_cast<int>(expected[channel])) <= 3);
        }
    }
    const auto with_option = captureMain(player, project.root / "display-main-with.png");
    CHECK(with_option.rgba == main_picture.rgba);
    player.close();
    CHECK(player.exitCode() == 0);
}

TEST_CASE("MD-1a1 output_window known colors use the output surface encoding", "[md1a1]") {
    const auto shader = GENERATE(
        "outColor = vec4(0.0, 0.0, 0.0, 1.0);",
        "outColor = vec4(1.0, 1.0, 1.0, 1.0);",
        "outColor = vec4(0.18, 0.18, 0.18, 1.0);");
    const int expected = std::string{shader}.find("0.18") != std::string::npos
        ? 117
        : (std::string{shader}.find("1.0,") != std::string::npos ? 255 : 0);
    TempProject project;
    const std::string fragment =
        "#version 460\nlayout(location = 0) out vec4 outColor;\nvoid main() { " +
        std::string{shader} + " }\n";
    project.writeDisplayShader(fragment.c_str());
    const auto dump_dir = project.root / "known-color";
    std::filesystem::create_directories(dump_dir);
    PlayerRpc player{project, project.root / "known-color.stderr.log",
                     displayOutputFlagsWithFrames(120), outputEnvironment(true, dump_dir)};
    const auto status = player.waitFor("known-color readback", [](const Json &s) {
        return s.at("output_window").at("presented").get<std::uint64_t>() >= 3;
    });
    INFO(status.at("output_window").dump());
    const auto output = readPpm(dump_dir / "pattern_0.ppm");
    const auto pixel = ppmAt(output, output.width / 2, output.height / 2);
    for (const auto channel : pixel) {
        CHECK(std::abs(static_cast<int>(channel) - expected) <= 2);
    }
    CHECK((status.at("output_window").at("surface_format") == "UNORM" ||
           status.at("output_window").at("surface_format") == "SRGB"));
    CHECK(status.at("output_window").at("copy_gpu_samples").get<std::uint64_t>() >= 1);
    CHECK(std::isfinite(status.at("output_window").at("copy_gpu_ms_max").get<double>()));
    CHECK(status.at("output_window").at("copy_gpu_ms_max").get<double>() >= 0.0);
    player.close();
    CHECK(player.exitCode() == 0);
}

TEST_CASE("MD-1a1 output_window generations move forward without replay", "[md1a1]") {
    TempProject project;
    project.writeDisplayShader(R"glsl(#version 460
#extension GL_GOOGLE_include_directive : enable
#include "pelican_sets.glsl"
#include "pelican_view.glsl"
#include "pelican_frame.glsl"
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_VARIADIC_INPUTS(0, 8);
void main() {
    outColor = vec4(0.0, float(pelicanFrame.frame_index.x & 127u) / 255.0,
                    inUV.x, 1.0);
}
)glsl");
    const auto dump_dir = project.root / "generations";
    std::filesystem::create_directories(dump_dir);
    PlayerRpc player{project, project.root / "generations.stderr.log",
                     displayOutputFlagsWithFrames(500), outputEnvironment(true, dump_dir, 8)};
    const auto status = player.waitFor("eight distinct output generations", [](const Json &s) {
        return s.at("output_window").at("captures").get<std::uint64_t>() >= 8 &&
               s.at("output_window").at("presented").get<std::uint64_t>() >= 8;
    });
    uint64_t previous = 0;
    for (int generation = 0; generation < 8; ++generation) {
        const auto output = readPpm(dump_dir / ("pattern_" + std::to_string(generation) + ".ppm"));
        const auto current = ppmAt(output, output.width / 2, output.height / 2)[1];
        INFO("generation " << generation << " green " << static_cast<int>(current));
        if (generation != 0) CHECK(current > previous);
        previous = current;
    }
    CHECK(status.at("output_window").at("presented").get<std::uint64_t>() <=
          status.at("output_window").at("captures").get<std::uint64_t>());
    player.close();
    CHECK(player.exitCode() == 0);
}

TEST_CASE("MD-1a1 output_window excludes a real ImGui overlay", "[md1a1]") {
    TempProject project;
    project.writeDisplayShader(R"(#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(0.2, 0.4, 0.6, 1.0); }
)");
    Image graph_only;
    {
        PlayerRpc player{project, project.root / "ui-off-main.stderr.log", {"--frames", "120"}};
        (void)player.waitFor("main without UI", [](const Json &s) {
            return s.at("window_output").at("submits").get<std::uint64_t>() >= 5;
        });
        graph_only = captureMain(player, project.root / "ui-off-main.png");
        player.close();
        CHECK(player.exitCode() == 0);
    }
    const auto dump_dir = project.root / "ui-output";
    std::filesystem::create_directories(dump_dir);
    auto environment = outputEnvironment(true, dump_dir);
    environment.emplace_back("PELICAN_MD1A_FORCE_IMGUI", "1");
    PlayerRpc player{project, project.root / "ui-on.stderr.log",
                     displayOutputFlagsWithFrames(120), environment};
    (void)player.waitFor("UI output captures", [](const Json &s) {
        return s.at("output_window").at("captures").get<std::uint64_t>() >= 3 &&
               s.at("output_window").at("presented").get<std::uint64_t>() >= 3;
    });
    const auto plan = player.result("get_frame_plan");
    REQUIRE(plan.at("nodes").dump().find("imgui") != std::string::npos);
    const auto with_ui = captureMain(player, project.root / "ui-on-main.png");
    REQUIRE(with_ui.rgba.size() == graph_only.rgba.size());
    std::size_t changed = 0;
    for (std::size_t i = 0; i < with_ui.rgba.size(); i += 4) {
        changed += !std::equal(with_ui.rgba.begin() + static_cast<std::ptrdiff_t>(i),
                               with_ui.rgba.begin() + static_cast<std::ptrdiff_t>(i + 3),
                               graph_only.rgba.begin() + static_cast<std::ptrdiff_t>(i));
    }
    CHECK(changed > 0);
    const auto output = readPpm(dump_dir / "pattern_1.ppm");
    for (std::uint32_t y = 0; y < output.height; y += 15) {
        for (std::uint32_t x = 0; x < output.width; x += 15) {
            const auto source_x = x * kMainWidth / kOutputWidth;
            const auto source_y = y * kMainHeight / kOutputHeight;
            const auto source_offset = (static_cast<std::size_t>(source_y) * kMainWidth + source_x) * 4;
            const auto actual = ppmAt(output, x, y);
            for (int channel = 0; channel < 3; ++channel) {
                CHECK(std::abs(static_cast<int>(actual[channel]) -
                               static_cast<int>(graph_only.rgba[source_offset + channel])) <= 3);
            }
        }
    }
    player.close();
    CHECK(player.exitCode() == 0);
}

TEST_CASE("MD-1a1 aborted producer reservations never publish a generation", "[md1a1]") {
    TempProject project;
    auto environment = outputEnvironment();
    environment.emplace_back("PELICAN_MD1A_ABORT_BEFORE_SUBMIT_FRAMES", "1000000");
    PlayerRpc player{project, project.root / "abort.stderr.log",
                     displayOutputFlagsWithFrames(120), environment};
    const auto status = player.waitFor("aborted capture reservations", [](const Json &s) {
        return s.at("output_window").at("capture_aborted").get<std::uint64_t>() >= 3;
    });
    CHECK(status.at("output_window").at("captures") == 0);
    CHECK(status.at("output_window").at("presented") == 0);
    CHECK(status.at("output_window").at("dropped").get<std::uint64_t>() >= 3);
    CHECK(status.at("output_window").at("capture_fence_waits") == 0);
    CHECK(status.at("window_output").at("submits") == 0);
    player.close();
    CHECK(player.exitCode() == 0);
}

TEST_CASE("MD-1a0 output_window liveness tracks main submits and dropped frames", "[md1a]") {
    TempProject project;
    constexpr std::uint64_t kForcedDrops = 30;
    double with_output_ms = 0.0;
    {
        std::vector<std::pair<std::string, std::string>> environment{
            {"PELICAN_MD1A_BEGIN_UNAVAILABLE_FRAMES", std::to_string(kForcedDrops)},
            {"PELICAN_MD1A_BEGIN_UNAVAILABLE_AFTER_FRAMES", "60"}};
        PlayerRpc player{project, project.root / "liveness.stderr.log", outputFlagsWithFrames(450), environment};

        const auto start = player.waitFor("the output window to start", [](const Json &status) {
            return status.at("output_window").at("state") == "running" &&
                   status.at("window_output").at("submits").get<std::uint64_t>() >= 1;
        });
        const auto timing_start = std::chrono::steady_clock::now();
        const auto start_submits = start.at("window_output").at("submits").get<std::uint64_t>();
        const auto start_presented = start.at("output_window").at("presented").get<std::uint64_t>();

        const auto end = player.waitFor("300 presented frames after the start", [&](const Json &status) {
            return status.at("output_window").at("presented").get<std::uint64_t>() - start_presented >= 300;
        }, 45s);
        const auto elapsed = std::chrono::steady_clock::now() - timing_start;

        const auto end_submits = end.at("window_output").at("submits").get<std::uint64_t>();
        const auto end_presented = end.at("output_window").at("presented").get<std::uint64_t>();
        const auto end_dropped = end.at("output_window").at("dropped").get<std::uint64_t>();
        const auto main_delta = end_submits - start_submits;
        INFO("submits " << start_submits << " -> " << end_submits << ", presented " << start_presented
                        << " -> " << end_presented << ", dropped=" << end_dropped);

        CHECK(end_presented - start_presented >= 300);
        REQUIRE(main_delta >= 300);
        CHECK(end_dropped == kForcedDrops);
        const auto present_ms_max = end.at("output_window").at("present_ms_max").get<double>();
        CHECK(present_ms_max > 0.0);
        with_output_ms = std::chrono::duration<double, std::milli>(elapsed).count() /
                         static_cast<double>(main_delta);
        std::cout << "MD1A_LIVENESS presented=" << end_presented - start_presented
                  << " main_submits=" << main_delta << " dropped=" << end_dropped
                  << " present_ms_max=" << present_ms_max << std::endl;
        player.close();
        CHECK(player.exitCode() == 0);
    }

    double without_output_ms = 0.0;
    {
        PlayerRpc player{project, project.root / "liveness-main-only.stderr.log", {"--frames", "360"}};
        const auto start = player.waitFor("the main-only timing start", [](const Json &status) {
            return status.at("window_output").at("submits").get<std::uint64_t>() >= 1;
        });
        const auto timing_start = std::chrono::steady_clock::now();
        const auto start_submits = start.at("window_output").at("submits").get<std::uint64_t>();
        const auto end = player.waitFor("300 main-only frames", [&](const Json &status) {
            return status.at("window_output").at("submits").get<std::uint64_t>() >= start_submits + 300;
        }, 45s);
        const auto elapsed = std::chrono::steady_clock::now() - timing_start;
        const auto end_submits = end.at("window_output").at("submits").get<std::uint64_t>();
        REQUIRE(end_submits > start_submits);
        without_output_ms = std::chrono::duration<double, std::milli>(elapsed).count() /
                            static_cast<double>(end_submits - start_submits);
        player.close();
        CHECK(player.exitCode() == 0);
    }
    std::cout << "MD1A_FRAME_TIME main_only_ms=" << without_output_ms
              << " with_output_ms=" << with_output_ms << std::endl;
}

TEST_CASE("MD-1a1 output_window skips a busy bridge without blocking or replaying generations", "[md1a1]") {
    TempProject project;
    const auto dump_dir = project.root / "busy-output";
    std::filesystem::create_directories(dump_dir);
    auto environment = outputEnvironment(true, dump_dir);
    environment.emplace_back("PELICAN_MD1A_CAPTURE_BUSY_FRAMES", "20");
    PlayerRpc player{project, project.root / "busy.stderr.log", displayOutputFlagsWithFrames(240), environment};
    const auto skipped = player.waitFor("deterministic busy capture skips", [](const Json &status) {
        return status.at("output_window").at("capture_skipped").get<std::uint64_t>() >= 10;
    });
    const auto main_submits = skipped.at("window_output").at("submits").get<std::uint64_t>();
    const auto skipped_status = skipped.at("output_window");
    CHECK(skipped_status.at("captures") == 0);
    CHECK(skipped_status.at("presented") == 0);
    CHECK(skipped_status.at("capture_fence_waits") == 0);
    const auto resumed = player.waitFor("display capture recovery", [&](const Json &status) {
        return status.at("window_output").at("submits").get<std::uint64_t>() >= main_submits + 10 &&
               status.at("output_window").at("captures").get<std::uint64_t>() >= 3 &&
               status.at("output_window").at("presented").get<std::uint64_t>() >= 3;
    });
    CHECK(resumed.at("output_window").at("capture_skipped") == 20);
    CHECK(resumed.at("output_window").at("capture_fence_waits") == 0);
    CHECK(resumed.at("output_window").at("presented").get<std::uint64_t>() <=
          resumed.at("output_window").at("captures").get<std::uint64_t>());
    REQUIRE(!std::filesystem::is_empty(dump_dir));
    const auto output = readPpm(dump_dir / "pattern_0.ppm");
    CHECK(output.width == kOutputWidth);
    CHECK(output.height == kOutputHeight);
    player.close();
    CHECK(player.exitCode() == 0);
}

TEST_CASE("MD-1a1 output_window capture follows the last of several graph passes",
          "[md1a1]") {
    TempProject project;
    project.writeDisplayShader(R"(#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }
)");
    std::ofstream second_shader{project.root / "shaders" / "md1a_second.frag", std::ios::trunc};
    second_shader << R"(#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(0.0, 1.0, 0.0, 1.0); }
)";
    second_shader.close();
    REQUIRE(second_shader.good());
    const auto config_path = project.root / "passes" / "main_rendering_config.json";
    std::ifstream input{config_path};
    Json config;
    input >> config;
    auto &passes = config.at("rendering").at("rendering_passes").at(0).at("passes");
    passes.push_back(passes.at(0));
    passes.back().at("name") = "present_second";
    passes.back().at("shader").at("fragment") = "project://shaders/md1a_second";
    config["rendering"]["frame_node_order"]["main_render"] = {"present", "present_second"};
    std::ofstream output_config{config_path, std::ios::trunc};
    output_config << config.dump(2) << '\n';
    output_config.close();
    REQUIRE(output_config.good());

    const auto dump_dir = project.root / "multiple-passes";
    auto player = PlayerRpc{project, project.root / "multiple-passes.stderr.log",
                            displayOutputFlagsWithFrames(400), outputEnvironment(true, dump_dir)};
    (void)player.waitFor("captures after multiple graph passes", [](const Json &status) {
        return status.at("output_window").at("presented").get<std::uint64_t>() >= 3;
    });
    const auto plan = player.result("get_frame_plan");
    CHECK(plan.at("nodes").dump().find("imgui") == std::string::npos);
    const auto image = readPpm(dump_dir / "pattern_0.ppm");
    const auto pixel = ppmAt(image, image.width / 2, image.height / 2);
    CHECK(std::abs(static_cast<int>(pixel[0])) <= 2);
    CHECK(std::abs(static_cast<int>(pixel[1]) - 255) <= 2);
    CHECK(std::abs(static_cast<int>(pixel[2])) <= 2);
    player.close();
    CHECK(player.exitCode() == 0);
}

TEST_CASE("MD-1a1 output_window real pending reads survive acquire drops, generations and teardown",
          "[md1a1]") {
    TempProject project;
    project.writeDisplayShader(R"(#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(0.0, 0.0, 1.0, 1.0); }
)");
    auto environment = outputEnvironment();
    environment.emplace_back("PELICAN_MD1A_DELAY_SINK_GPU_MS", "400");
    environment.emplace_back("PELICAN_MD1A_BEGIN_UNAVAILABLE_FRAMES", "5");
    environment.emplace_back("PELICAN_MD1A_BEGIN_UNAVAILABLE_AFTER_FRAMES", "2");
    {
        PlayerRpc player{project, project.root / "pending.stderr.log",
                         displayOutputFlagsWithFrames(0), environment};
        const auto pending = player.waitFor("a really pending sink submission", [](const Json &status) {
            return status.at("output_window").at("capture_skipped").get<std::uint64_t>() >= 1;
        }, 20s);
        CHECK(pending.at("output_window").at("captures") == 1);
        CHECK(pending.at("output_window").at("presented") == 1);
        CHECK(pending.at("output_window").at("capture_fence_waits") == 0);
        project.writeDisplayShader(R"(#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(1.0, 1.0, 1.0, 1.0); }
)");
        CHECK(player.result("reload_shaders").at("committed") == true);
        const auto resumed = player.waitFor("new-generation captures after the sink read completes", [](const Json &status) {
            return status.at("output_window").at("captures").get<std::uint64_t>() >= 3 &&
                   status.at("output_window").at("presented").get<std::uint64_t>() >= 3;
        }, 30s);
        CHECK(resumed.at("output_window").at("dropped").get<std::uint64_t>() >= 5);
        CHECK(resumed.at("output_window").at("last_error").is_null());
        player.close();
        CHECK(player.exitCode() == 0);
        CHECK(player.stderrText().find("PELICAN_MD1A_SINK_SUBMISSION_DELAY") != std::string::npos);
    }
    {
        auto teardown_environment = outputEnvironment();
        teardown_environment.emplace_back("PELICAN_MD1A_DELAY_SINK_GPU_MS", "20000");
        auto teardown_flags = displayOutputFlagsWithFrames(2);
        PlayerRpc player{project, project.root / "pending-teardown.stderr.log",
                         teardown_flags, teardown_environment};
        player.close(30);
        CHECK(player.exitCode() == 0);
        CHECK(player.stderrText().find("PELICAN_MD1A_SINK_SUBMISSION_DELAY") !=
              std::string::npos);
    }
}

TEST_CASE("MD-1a0 output_window closes independently while the main target keeps presenting",
          "[md1a]") {
    TempProject project;
    std::vector<std::pair<std::string, std::string>> environment{
        {"PELICAN_MD1A_REQUEST_CLOSE_AFTER_FRAMES", "20"}};
    PlayerRpc player{project, project.root / "close.stderr.log", outputFlagsWithFrames(100), environment};

    const auto closed = player.waitFor("the output window to close", [](const Json &status) {
        return status.at("output_window").at("state") == "closed";
    });
    const auto closed_presented = closed.at("output_window").at("presented").get<std::uint64_t>();
    const auto closed_submits = closed.at("window_output").at("submits").get<std::uint64_t>();

    const auto later = player.waitFor("the main target to keep presenting", [&](const Json &status) {
        return status.at("window_output").at("submits").get<std::uint64_t>() >= closed_submits + 10;
    });
    INFO("closed status " << closed.dump());
    CHECK(later.at("output_window").at("state") == "closed");
    CHECK(later.at("output_window").at("presented").get<std::uint64_t>() == closed_presented);
    CHECK(later.at("window_output").at("submits").get<std::uint64_t>() >= closed_submits + 10);

    player.close();
    CHECK(player.exitCode() == 0);
}

TEST_CASE("MD-1a0 output_window out-of-range monitor fails but the engine keeps running", "[md1a]") {
    TempProject project;
    PlayerRpc player{project, project.root / "monitor.stderr.log",
                     {"--output-window", "999999", "--output-window-pattern", "--frames", "60"}};
    const auto failed = player.waitFor("the monitor refusal", [](const Json &status) {
        return status.at("output_window").at("state") == "failed";
    });
    INFO(failed.at("output_window").dump());
    const auto &output = failed.at("output_window");
    CHECK(output.at("monitor") == 999999);
    REQUIRE(output.at("last_error").is_string());
    CHECK_THAT(output.at("last_error").get<std::string>(),
               Catch::Matchers::ContainsSubstring("output_window.monitor_unavailable@1"));
    CHECK(output.at("presented") == 0);

    const auto submits = failed.at("window_output").at("submits").get<std::uint64_t>();
    (void)player.waitFor("the main loop to keep running", [&](const Json &status) {
        return status.at("window_output").at("submits").get<std::uint64_t>() >= submits + 5;
    });
    player.close();
    CHECK(player.exitCode() == 0);
}

TEST_CASE("MD-1a0 output_window begin is nonblocking and the main loop keeps progressing",
          "[md1a]") {
    TempProject project;
    std::vector<std::pair<std::string, std::string>> environment{
        {"PELICAN_MD1A_UNSIGNALED_SLOT_FENCE", "1"}};
    PlayerRpc player{project, project.root / "watchdog.stderr.log", outputFlagsWithFrames(120), environment};
    const auto first = player.waitFor("the output window and the main loop", [](const Json &status) {
        return status.at("output_window").at("state") == "running" &&
               status.at("window_output").at("submits").get<std::uint64_t>() >= 1;
    });
    const auto start_submits = first.at("window_output").at("submits").get<std::uint64_t>();
    const auto start_dropped = first.at("output_window").at("dropped").get<std::uint64_t>();
    const auto later = player.waitFor(
        "the watchdog: main submits and drops advance with an unsignaled output slot fence",
        [&](const Json &status) {
            return status.at("window_output").at("submits").get<std::uint64_t>() >=
                       start_submits + 5 &&
                   status.at("output_window").at("dropped").get<std::uint64_t>() >=
                       start_dropped + 5;
        },
        3s);
    CHECK(later.at("output_window").at("presented") == 0);
    player.close();
    CHECK(player.exitCode() == 0);
}

TEST_CASE("MD-1a1 output_window display passes synchronization validation for 3000 frames",
          "[md1a1]") {
    TempProject project;
    const auto dump_dir = project.root / "validation-output";
    std::filesystem::create_directories(dump_dir);
    auto flags = displayOutputFlagsWithFrames(3000);
    flags.insert(flags.end(), {"--vulkan-validation", "on"});
    Json status;
    {
        PlayerRpc player{project, project.root / "validation.stderr.log", flags,
                         outputEnvironment(true, dump_dir)};
        status = player.waitFor("a captured, presented and read back output", [](const Json &s) {
            const auto &window = s.at("output_window");
            return window.at("captures").get<std::uint64_t>() >= 1 &&
                   window.at("presented").get<std::uint64_t>() >= 1;
        });
        player.close(360);
        CHECK(player.exitCode() == 0);
    }
    CHECK(status.at("output_window").at("captures").get<std::uint64_t>() >= 1);
    CHECK(status.at("output_window").at("presented").get<std::uint64_t>() >= 1);
    REQUIRE(!std::filesystem::is_empty(dump_dir));
    const auto output = readPpm(dump_dir / "pattern_0.ppm");
    CHECK(output.width == kOutputWidth);
    CHECK(output.height == kOutputHeight);

    const auto stderr = readFile(project.root / "validation.stderr.log");
    INFO("validation log size " << stderr.size());
    CHECK(stderr.find("available=true") != std::string::npos);
    CHECK(stderr.find("enabled=true") != std::string::npos);
    CHECK(stderr.find("synchronization=true") != std::string::npos);

    std::istringstream lines{stderr};
    std::string line;
    std::vector<std::string> messages;
    while (std::getline(lines, line)) {
        if (line.find("VUID-") != std::string::npos ||
            line.find("SYNC-") != std::string::npos ||
            line.find("Validation Error") != std::string::npos ||
            line.find("Validation Warning") != std::string::npos) {
            messages.push_back(line);
        }
    }
    for (const auto &message : messages) INFO(message);
    CHECK(messages.empty());
}
