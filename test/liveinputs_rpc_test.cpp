#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/os/liveinputrecording.hpp"
#include "../src/core/osc/osclistener.hpp"
#include "../src/core/osc/osclistenerconfig.hpp"
#include "../src/core/osc/oscparser.hpp"
#include "../src/core/os/inputsequence.hpp"
#include "../src/core/userpublic/userinput.hpp"
#include "../src/core/watch/reloadgate.hpp"
#include "../src/project/projectdocumenttransaction.hpp"
#include "osc_test_udp.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <bit>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

using Json = nlohmann::json;
using Catch::Matchers::ContainsSubstring;

std::string readFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void writeFile(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << text;
}

struct TempDir {
    std::filesystem::path root;
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

// posegraph_demo, with its animation clock driven by a declared live input
// instead of scene_time, so the published pose is a visible function of the
// frozen snapshot.
std::filesystem::path copyLiveProject(const TempDir &dir, bool with_actions = true,
                                      bool with_output_sink = false) {
    const auto shipped = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" / "posegraph_demo";
    const auto root = dir.root / "live_posegraph_demo";
    std::filesystem::create_directories(root);
    std::filesystem::copy(shipped, root,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    std::filesystem::create_directories(root / "shaders");

    auto project = Json::parse(readFile(root / "project.json"));
    project["basic_config"]["live_inputs_json"] = "live/inputs.json";
    if (with_actions)
        project["basic_config"]["input_actions_json"] = "live/actions.json";
    else
        project["basic_config"].erase("input_actions_json");
    project["basic_config"]["input_profiles"] =
        Json{{"live", "live/profile.json"}, {"plain", "live/plain.json"}};
    // The no-actions child test passes --input-profile live explicitly, so
    // make the project default a profile without live bindings.  Existing
    // action-based tests keep the project default as before.
    project["basic_config"]["input_profile"] =
        with_actions ? "live" : "plain";
    writeFile(root / "project.json", project.dump(2));

    writeFile(root / "live" / "actions.json", Json{
        {"schema", "pelican.input_actions"},
        {"version", 1},
        {"action_sets", Json::array()},
    }.dump(2));
    writeFile(root / "live" / "profile.json", Json{
        {"schema", "pelican.input_profile"},
        {"version", 1},
        {"name", "live"},
        {"bindings", Json::array()},
        {"live", Json::array({
            {{"address", "/phase"}, {"input", "phase"}},
        })},
    }.dump(2));
    writeFile(root / "live" / "plain.json", Json{
        {"schema", "pelican.input_profile"},
        {"version", 1},
        {"name", "plain"},
        {"bindings", Json::array()},
    }.dump(2));

    writeFile(root / "live" / "inputs.json", Json{
        {"schema", "pelican.live_inputs"},
        {"version", 1},
        {"inputs", Json::array({
            {{"name", "phase"}, {"type", "float"}, {"default", 0.0}, {"min", 0.0}, {"max", 4.0}},
            {{"name", "steps"}, {"type", "int"}, {"default", 1}, {"min", 0}, {"max", 8}},
            {{"name", "enabled"}, {"type", "bool"}, {"default", false}},
        })},
    }.dump(2));

    auto graph = Json::parse(readFile(root / "passes" / "main.json"));
    // Both the definition's clip clock and the top-level blend clock read the
    // same live value, so the published pose is a direct sample of it.
    for (auto &[name, definition] : graph["definitions"].items()) {
        (void)name;
        for (auto &node : definition["nodes"])
            if (node["id"] == "clock")
                node["parameters"]["name"] = "live:phase";
    }
    for (auto &node : graph["nodes"])
        if (node["id"] == "clock")
            node["parameters"]["name"] = "live:phase";
    if (with_output_sink) {
        const Json color_type{{"semantic", "pelican.render.color_signal@1"},
            {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
        graph["nodes"].push_back({{"id", "osc1b_target"}, {"op", "render.image"},
            {"parameters", {{"type", color_type}, {"format", "R8G8B8A8_UNORM"},
                {"width", 64}, {"height", 64}, {"extent_scale", 1.0},
                {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}},
            {"outputs", Json::array({"resource"})}});
        graph["nodes"].push_back({{"id", "osc1b_fill"}, {"op", "render.fullscreen"},
            {"parameters", {{"pass_set", "main_render"},
                {"shader", {{"vertex", "engine://fullscreen"},
                            {"fragment", "project://shaders/osc1b_fill"}}},
                {"ports", {{"color", {{"kind", "image"}, {"type", color_type},
                    {"access", "write"}, {"attachment", "color"}, {"location", 0}}}}}}},
            {"inputs", {{"color", {{"node", "osc1b_target"}, {"port", "resource"}}}}},
            {"outputs", Json::array({"color"})}});
        graph["nodes"].push_back({{"id", "osc1b_sink"}, {"op", "output.file"},
            {"parameters", {{"name", "shot"}, {"every_n", 1}, {"enabled", true}, {"format", "png"}}},
            {"inputs", {{"source", {{"node", "osc1b_fill"}, {"port", "color"}}}}},
            {"outputs", Json::array()}});
        writeFile(root / "shaders" / "osc1b_fill.frag", R"glsl(#version 460
#extension GL_GOOGLE_include_directive : enable
#include "pelican_sets.glsl"
#include "pelican_view.glsl"
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(inUV.x, inUV.y, 0.25, 1.0); }
)glsl");
    }
    writeFile(root / "passes" / "main.json", graph.dump(2));
    return root;
}

#if PELICAN_WITH_OSC
void addOscSendNodes(const std::filesystem::path &root) {
    auto graph = Json::parse(readFile(root / "passes" / "main.json"));
    graph["nodes"].push_back({
        {"id", "osc_phase"}, {"op", "osc.send"},
        {"parameters", {{"address", "/out/phase"}}},
        {"inputs", {{"value", {{"node", "clock"}, {"port", "value"}}}}},
        {"outputs", Json::array()},
    });
    graph["nodes"].push_back({
        {"id", "live_steps"}, {"op", "value.input"},
        {"parameters", {{"name", "live:steps"}}}, {"outputs", Json::array({"value"})},
    });
    graph["nodes"].push_back({
        {"id", "osc_steps"}, {"op", "osc.send"},
        {"parameters", {{"address", "/out/steps"}, {"type", "int"}}},
        {"inputs", {{"value", {{"node", "live_steps"}, {"port", "value"}}}}},
        {"outputs", Json::array()},
    });
    writeFile(root / "passes" / "main.json", graph.dump(2));
}
#endif

void configureHeadless(const std::filesystem::path &root) {
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setProjectData(readFile(root / "project.json"));
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.rpc = true;
    launch.headless_extent = vk::Extent2D{64, 64};
    launch.shader_hot_reload = false;
    GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    TestSupport::requireVulkanDevice("OSC-1a live inputs over RPC");
    GET_MODULE(ECSPredefinedRegistration).reg();
}

struct Rpc {
    std::istringstream input;
    std::ostringstream output;
    EngineRpcEndpoint endpoint{input, output};
    std::uint64_t next_id = 9300;

    Json call(std::string_view method, Json params = Json::object()) {
        return Json::parse(endpoint.processLine(
            Json{{"jsonrpc", "2.0"}, {"id", ++next_id}, {"method", method}, {"params", std::move(params)}}
                .dump()));
    }
    Json result(std::string_view method, Json params = Json::object()) {
        const auto response = call(method, std::move(params));
        INFO(response.dump(2));
        REQUIRE(response.contains("result"));
        return response.at("result");
    }
    Json error(std::string_view method, Json params = Json::object()) {
        const auto response = call(method, std::move(params));
        INFO(response.dump(2));
        REQUIRE(response.contains("error"));
        return response.at("error");
    }
};

Json inputNamed(const Json &snapshot, std::string_view name) {
    for (const auto &input : snapshot.at("inputs"))
        if (input.at("name").get<std::string>() == name) return input;
    FAIL("no live input " << name);
    return {};
}

const Json &firstSink(const Json &status) {
    const auto &animation = status.at("animation");
    REQUIRE(animation.at("available") == true);
    REQUIRE_FALSE(animation.at("sinks").empty());
    return animation.at("sinks").at(0);
}

#if defined(_WIN32)
struct WinsockScope {
    WinsockScope() {
        WSADATA data{};
        ok = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockScope() {
        if (ok) ::WSACleanup();
    }
    bool ok = false;
};
#endif

#if defined(PELICAN_LIVEINPUTS_PLAYER)
// Drives the real pelican_player executable over its stdin/stdout JSON-RPC
// transport.  The OSC port is chosen by the player from --osc-port 0; nothing
// in this test injects a parsed listener config into the engine.
class PlayerProcess {
  public:
    explicit PlayerProcess(const std::filesystem::path &project,
                           std::vector<std::string> arguments = {}) {
        std::vector<std::string> player_arguments{"--rpc", "--headless", "--size", "64x64"};
#if PELICAN_WITH_OSC
        player_arguments.insert(player_arguments.end(), {"--osc-port", "0"});
#endif
        player_arguments.insert(player_arguments.end(),
            {"--input-profile", "live", "--project", project.string()});
        arguments.insert(arguments.begin(), player_arguments.begin(), player_arguments.end());
        log_path_ = std::filesystem::temp_directory_path() /
            ("pelican_osc2c_player_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
#if defined(_WIN32)
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        if (!::CreatePipe(&stdin_read_, &stdin_write_, &attributes, 0) ||
            !::CreatePipe(&stdout_read_, &stdout_write_, &attributes, 0)) {
            throw std::runtime_error("CreatePipe failed");
        }
        ::SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);
        ::SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0);
        log_ = ::CreateFileW(log_path_.wstring().c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ, &attributes, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdin_read_;
        startup.hStdOutput = stdout_write_;
        startup.hStdError = log_ != INVALID_HANDLE_VALUE ? log_ : stdout_write_;

        std::wstring command = L"\"" +
            std::filesystem::path{PELICAN_LIVEINPUTS_PLAYER}.wstring() + L"\"";
        for (const auto &argument : arguments) {
            command += L" \"";
            command += std::filesystem::path{argument}.wstring();
            command += L"\"";
        }
        if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                              &process_)) {
            throw std::runtime_error("CreateProcessW failed");
        }
        ::CloseHandle(stdin_read_);
        stdin_read_ = INVALID_HANDLE_VALUE;
        ::CloseHandle(stdout_write_);
        stdout_write_ = INVALID_HANDLE_VALUE;
#else
        int input_pipe[2]{};
        int output_pipe[2]{};
        if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0)
            throw std::runtime_error("pipe failed");
        std::vector<char *> child_arguments;
        child_arguments.push_back(const_cast<char *>(PELICAN_LIVEINPUTS_PLAYER));
        for (auto &argument : arguments) child_arguments.push_back(argument.data());
        child_arguments.push_back(nullptr);
        pid_ = ::fork();
        if (pid_ == 0) {
            (void)::dup2(input_pipe[0], STDIN_FILENO);
            (void)::dup2(output_pipe[1], STDOUT_FILENO);
            const int log_fd = ::open(log_path_.c_str(),
                                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log_fd >= 0) {
                (void)::dup2(log_fd, STDERR_FILENO);
                (void)::close(log_fd);
            }
            (void)::close(input_pipe[1]);
            (void)::close(output_pipe[0]);
            ::execv(PELICAN_LIVEINPUTS_PLAYER, child_arguments.data());
            ::_exit(127);
        }
        (void)::close(input_pipe[0]);
        (void)::close(output_pipe[1]);
        write_fd_ = input_pipe[1];
        read_fd_ = output_pipe[0];
#endif
    }

    PlayerProcess(const PlayerProcess &) = delete;
    PlayerProcess &operator=(const PlayerProcess &) = delete;

    int finish() {
        int exit_code = 0;
#if defined(_WIN32)
        if (stdin_write_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(stdin_write_);
            stdin_write_ = INVALID_HANDLE_VALUE;
        }
        if (::WaitForSingleObject(process_.hProcess, 120000) == WAIT_TIMEOUT) {
            (void)::TerminateProcess(process_.hProcess, 124);
            exit_code = 124;
        }
        DWORD code = 1;
        (void)::GetExitCodeProcess(process_.hProcess, &code);
        if (exit_code == 0) exit_code = static_cast<int>(code);
        ::CloseHandle(process_.hThread);
        ::CloseHandle(process_.hProcess);
        process_ = {};
        while (!readLine().empty()) {}
#else
        if (write_fd_ >= 0) {
            (void)::close(write_fd_);
            write_fd_ = -1;
        }
        int status = 0;
        pid_t waited = 0;
        for (int attempt = 0; attempt < 1200 && waited == 0; ++attempt) {
            waited = ::waitpid(pid_, &status, WNOHANG);
            if (waited == 0) std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        if (waited == 0) {
            (void)::kill(pid_, SIGTERM);
            (void)::waitpid(pid_, &status, 0);
            exit_code = 124;
        } else if (waited < 0) {
            throw std::runtime_error("player wait failed");
        } else if (WIFEXITED(status)) {
            exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            exit_code = 128 + WTERMSIG(status);
        }
        pid_ = -1;
        while (!readLine().empty()) {}
        (void)::close(read_fd_);
        read_fd_ = -1;
#endif
        return exit_code;
    }

    const std::filesystem::path &logPath() const { return log_path_; }

    ~PlayerProcess() {
#if defined(_WIN32)
        if (stdin_write_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(stdin_write_);
            stdin_write_ = INVALID_HANDLE_VALUE;
        }
        if (process_.hProcess != nullptr) {
            if (::WaitForSingleObject(process_.hProcess, 10000) ==
                WAIT_TIMEOUT) {
                (void)::TerminateProcess(process_.hProcess, 1);
            }
            ::CloseHandle(process_.hThread);
            ::CloseHandle(process_.hProcess);
        }
        if (stdout_read_ != INVALID_HANDLE_VALUE)
            ::CloseHandle(stdout_read_);
        if (log_ != INVALID_HANDLE_VALUE) ::CloseHandle(log_);
#else
        if (write_fd_ >= 0) {
            (void)::close(write_fd_);
            write_fd_ = -1;
        }
        if (pid_ > 0) {
            bool exited = false;
            for (int i = 0; i < 100; ++i) {
                int status = 0;
                if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                    exited = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
            if (!exited) {
                (void)::kill(pid_, SIGTERM);
                (void)::waitpid(pid_, nullptr, 0);
            }
        }
        if (read_fd_ >= 0) (void)::close(read_fd_);
#endif
    }

    Json call(std::string_view method, Json params = Json::object()) {
        const auto id = ++next_id_;
        writeLine(
            Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method},
                 {"params", std::move(params)}}
                .dump());
        for (;;) {
            const auto line = readLine();
            if (line.empty())
                throw std::runtime_error("player closed its RPC output");
            const auto response = Json::parse(line);
            if (response.value("id", 0) == id) return response;
        }
    }

    Json result(std::string_view method, Json params = Json::object()) {
        const auto response = call(method, std::move(params));
        INFO(response.dump(2));
        REQUIRE(response.contains("result"));
        return response.at("result");
    }

  private:
    void writeLine(const std::string &line) {
        const std::string payload = line + "\n";
#if defined(_WIN32)
        std::size_t offset = 0;
        while (offset < payload.size()) {
            DWORD written = 0;
            const auto chunk = static_cast<DWORD>(payload.size() - offset);
            if (!::WriteFile(stdin_write_, payload.data() + offset, chunk,
                             &written, nullptr) ||
                written == 0)
                throw std::runtime_error("player stdin write failed");
            offset += written;
        }
#else
        std::size_t offset = 0;
        while (offset < payload.size()) {
            const auto written =
                ::write(write_fd_, payload.data() + offset,
                        payload.size() - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("player stdin write failed");
            }
            offset += static_cast<std::size_t>(written);
        }
#endif
    }

    std::string readLine() {
        std::string line;
        for (;;) {
            char byte = 0;
#if defined(_WIN32)
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds{180};
            for (;;) {
                DWORD available = 0;
                if (!::PeekNamedPipe(stdout_read_, nullptr, 0, nullptr,
                                     &available, nullptr))
                    return {};
                if (available > 0) break;
                if (::WaitForSingleObject(process_.hProcess, 0) ==
                    WAIT_OBJECT_0)
                    return {};
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("player RPC response timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            DWORD read = 0;
            if (!::ReadFile(stdout_read_, &byte, 1, &read, nullptr) ||
                read == 0)
                return {};
#else
            struct pollfd poll_fd {
                read_fd_, POLLIN, 0
            };
            const int ready = ::poll(&poll_fd, 1, 180000);
            if (ready <= 0)
                throw std::runtime_error("player RPC response timeout");
            const auto read = ::read(read_fd_, &byte, 1);
            if (read == 0) return {};
            if (read < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("player stdout read failed");
            }
#endif
            if (byte == '\n') return line;
            line.push_back(byte);
        }
    }

    std::uint64_t next_id_ = 4200;
    std::filesystem::path log_path_;
#if defined(_WIN32)
    HANDLE stdin_read_ = INVALID_HANDLE_VALUE;
    HANDLE stdin_write_ = INVALID_HANDLE_VALUE;
    HANDLE stdout_read_ = INVALID_HANDLE_VALUE;
    HANDLE stdout_write_ = INVALID_HANDLE_VALUE;
    HANDLE log_ = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION process_{};
#else
    int read_fd_ = -1;
    int write_fd_ = -1;
    pid_t pid_ = -1;
#endif
};

struct PlayerRun {
    int exit_code = -1;
    std::filesystem::path log_path;
};

PlayerRun runPlayer(const std::filesystem::path &project,
                    std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), {"--headless", "--size", "64x64", "--frames", "6",
        "--fps", "60", "--input-profile", "plain", "--project", project.string()});
    const auto log_path = std::filesystem::temp_directory_path() /
        ("pelican_osc1b_player_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    const auto log = ::CreateFileW(log_path.wstring().c_str(), GENERIC_WRITE,
        FILE_SHARE_READ, &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) throw std::runtime_error("player log creation failed");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = log;
    startup.hStdError = log;
    std::wstring command = L"\"" + std::filesystem::path{PELICAN_LIVEINPUTS_PLAYER}.wstring() + L"\"";
    for (const auto &argument : arguments) {
        command += L" \"";
        command += std::filesystem::path{argument}.wstring();
        command += L"\"";
    }
    PROCESS_INFORMATION process{};
    const auto created = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    ::CloseHandle(log);
    if (!created) throw std::runtime_error("CreateProcessW failed");
    const auto waited = ::WaitForSingleObject(process.hProcess, 120000);
    if (waited == WAIT_TIMEOUT) (void)::TerminateProcess(process.hProcess, 124);
    DWORD exit_code = 1;
    (void)::GetExitCodeProcess(process.hProcess, &exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return {static_cast<int>(exit_code), log_path};
#else
    std::vector<char *> child_arguments;
    child_arguments.push_back(const_cast<char *>(PELICAN_LIVEINPUTS_PLAYER));
    for (auto &argument : arguments) child_arguments.push_back(argument.data());
    child_arguments.push_back(nullptr);
    const auto pid = ::fork();
    if (pid == 0) {
        const int log_fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            (void)::dup2(log_fd, STDOUT_FILENO);
            (void)::dup2(log_fd, STDERR_FILENO);
            (void)::close(log_fd);
        }
        ::execv(PELICAN_LIVEINPUTS_PLAYER, child_arguments.data());
        ::_exit(127);
    }
    if (pid < 0) throw std::runtime_error("fork failed");
    int status = 0;
    for (int attempt = 0; attempt < 1200; ++attempt) {
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            if (WIFEXITED(status)) return {WEXITSTATUS(status), log_path};
            if (WIFSIGNALED(status)) return {128 + WTERMSIG(status), log_path};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    (void)::kill(pid, SIGTERM);
    (void)::waitpid(pid, nullptr, 0);
    return {124, log_path};
#endif
}
#endif

void appendOscString(std::vector<std::uint8_t> &bytes, std::string_view text) {
    bytes.insert(bytes.end(), text.begin(), text.end());
    bytes.push_back(0);
    while (bytes.size() % 4 != 0) bytes.push_back(0);
}

bool sendOscMessage(std::uint16_t port, std::string_view address,
                    std::string_view tags, const std::vector<double> &values = {}) {
#if defined(_WIN32)
    static WinsockScope winsock;
    if (!winsock.ok) return false;
    const auto socket_handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) return false;
#else
    const int socket_handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle < 0) return false;
#endif
    std::vector<std::uint8_t> bytes;
    appendOscString(bytes, address);
    appendOscString(bytes, "," + std::string{tags});
    std::size_t index = 0;
    for (const char tag : tags) {
        const double value = index < values.size() ? values[index++] : 0.0;
        if (tag == 's') {
            appendOscString(bytes, "bad");
            continue;
        }
        if (tag != 'f' && tag != 'i') continue;
        const auto bits = tag == 'f'
                              ? std::bit_cast<std::uint32_t>(static_cast<float>(value))
                              : static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
        bytes.push_back(static_cast<std::uint8_t>(bits >> 24));
        bytes.push_back(static_cast<std::uint8_t>(bits >> 16));
        bytes.push_back(static_cast<std::uint8_t>(bits >> 8));
        bytes.push_back(static_cast<std::uint8_t>(bits));
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    destination.sin_addr.s_addr = htonl(0x7f000001u);
#if defined(_WIN32)
    const int sent = ::sendto(socket_handle, reinterpret_cast<const char *>(bytes.data()),
                              static_cast<int>(bytes.size()), 0,
                              reinterpret_cast<const sockaddr *>(&destination), sizeof(destination));
    ::closesocket(socket_handle);
#else
    const auto sent = ::sendto(socket_handle, bytes.data(), bytes.size(), 0,
                               reinterpret_cast<const sockaddr *>(&destination), sizeof(destination));
    ::close(socket_handle);
#endif
    return sent >= 0 && static_cast<std::size_t>(sent) == bytes.size();
}

bool sendOscPhase(std::uint16_t port, float value) {
    return sendOscMessage(port, "/phase", "f", {value});
}

const Json &recentFor(const Json &snapshot, std::string_view address) {
    const auto &recent = snapshot.at("osc").at("recent");
    const auto found = std::find_if(recent.rbegin(), recent.rend(), [&](const Json &entry) {
        return entry.value("address", std::string{}) == address;
    });
    REQUIRE(found != recent.rend());
    return *found;
}

} // namespace

TEST_CASE("OSC-1a liveinputs RPC stages a typed live value, freezes it next frame, and publishes it",
          "[osc1a][osc3b][liveinputs][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_liveinputs_rpc_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir);

    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("render_frame");

    // The project declaration is the source, with declared defaults and types.
    auto snapshot = rpc.result("get_live_inputs");
    REQUIRE(snapshot.at("dropped_during_replay") == 0);
    auto phase = inputNamed(snapshot, "phase");
    REQUIRE(phase.at("type") == "float");
    REQUIRE(phase.at("value") == Catch::Approx(0.0));
    REQUIRE(phase.at("default") == Catch::Approx(0.0));
    REQUIRE(phase.at("source") == "default");
    REQUIRE(phase.at("changed_frame") == 0);
    REQUIRE(phase.at("min").is_number_float());
    REQUIRE(phase.at("max").is_number_float());
    REQUIRE(phase.at("min") == Catch::Approx(0.0));
    REQUIRE(phase.at("max") == Catch::Approx(4.0));
    auto steps = inputNamed(snapshot, "steps");
    REQUIRE(steps.at("type") == "int");
    REQUIRE(steps.at("value") == 1);
    REQUIRE(steps.at("min").is_number_integer());
    REQUIRE(steps.at("max").is_number_integer());
    REQUIRE(steps.at("min") == 0);
    REQUIRE(steps.at("max") == 8);
    auto enabled = inputNamed(snapshot, "enabled");
    REQUIRE(enabled.at("type") == "bool");
    REQUIRE(enabled.at("value") == false);
    CHECK_FALSE(enabled.contains("min"));
    CHECK_FALSE(enabled.contains("max"));

    // Freeze a stable baseline: a paused frame whose game time does not move.
    REQUIRE(rpc.result("set_play_state", {{"state", "paused"}}) ==
            Json{{"state", "paused"}, {"previous", "playing"}});
    const auto baseline = rpc.result("step_frame");
    const auto baseline_stats = rpc.result("get_status");
    const auto baseline_hash = firstSink(baseline_stats).at("pose_hash");
    const auto baseline_frame = baseline.at("frame").get<std::uint64_t>();

    // Staged values are not visible until the next frame boundary.
    REQUIRE(rpc.result("set_live_input", {{"values", {{"phase", 0.25}}}}) ==
            Json{{"accepted", 1}, {"applies_at_frame", baseline_frame + 1}});
    phase = inputNamed(rpc.result("get_live_inputs"), "phase");
    REQUIRE(phase.at("value") == Catch::Approx(0.0));
    REQUIRE(phase.at("source") == "default");

    // The next drawn frame freezes it and the graph's published pose follows.
    const auto changed = rpc.result("step_frame");
    REQUIRE(changed.at("frame").get<std::uint64_t>() == baseline_frame + 1);
    snapshot = rpc.result("get_live_inputs");
    phase = inputNamed(snapshot, "phase");
    REQUIRE(phase.at("value") == Catch::Approx(0.25));
    REQUIRE(phase.at("source") == "rpc");
    REQUIRE(phase.at("changed_frame").get<std::uint64_t>() == baseline_frame + 1);
    const auto changed_stats = rpc.result("get_status");
    REQUIRE(changed_stats.at("frame") == changed.at("frame"));
    REQUIRE(firstSink(changed_stats).at("pose_hash") != baseline_hash);

    // Re-sending the same value changes neither the value nor its changed
    // frame; the pose is the same paused pose, not an advancing one.
    REQUIRE(rpc.result("set_live_input", {{"values", {{"phase", 0.25}}}}).at("accepted") == 1);
    const auto resend = rpc.result("step_frame");
    phase = inputNamed(rpc.result("get_live_inputs"), "phase");
    REQUIRE(phase.at("changed_frame").get<std::uint64_t>() == baseline_frame + 1);
    const auto resend_stats = rpc.result("get_status");
    REQUIRE(resend_stats.at("frame") == resend.at("frame"));
    REQUIRE(firstSink(resend_stats).at("pose_hash") == firstSink(changed_stats).at("pose_hash"));

    // Every value is validated before any commits; a bad name leaves the
    // earlier accepted values untouched.
    const auto before_bad = inputNamed(rpc.result("get_live_inputs"), "phase");
    auto bad = rpc.error("set_live_input", {{"values", {{"phase", 0.1}, {"missing", 1}}}});
    REQUIRE_THAT(bad.at("message").get<std::string>(), ContainsSubstring("live_input.unknown@1"));
    REQUIRE(inputNamed(rpc.result("get_live_inputs"), "phase") == before_bad);
    bad = rpc.error("set_live_input", {{"values", {{"steps", 1.5}}}});
    REQUIRE_THAT(bad.at("message").get<std::string>(), ContainsSubstring("live_input.type@1"));
    bad = rpc.error("set_live_input", {{"values", {{"enabled", 1}}}});
    REQUIRE_THAT(bad.at("message").get<std::string>(), ContainsSubstring("live_input.type@1"));

    // Integers and booleans keep their declared JSON types; a declared bound
    // clamps and marks the value.
    REQUIRE(rpc.result("set_live_input",
                       {{"values", {{"steps", 99}, {"enabled", true}}}}).at("accepted") == 2);
    (void)rpc.result("step_frame");
    snapshot = rpc.result("get_live_inputs");
    steps = inputNamed(snapshot, "steps");
    REQUIRE(steps.at("value").is_number_integer());
    REQUIRE(steps.at("value") == 8);
    REQUIRE(steps.at("clamped") == true);
    enabled = inputNamed(snapshot, "enabled");
    REQUIRE(enabled.at("value").is_boolean());
    REQUIRE(enabled.at("value") == true);

    // A project whose published output reads live:phase cannot start a replay
    // that does not record it.
    const auto replay = rpc.error("start_input_replay",
                                  {{"path", (dir.root / "recording.pelican").generic_string()}});
    REQUIRE_THAT(replay.at("message").get<std::string>(),
                 ContainsSubstring("offline.live_input_unrecorded@1"));
}

TEST_CASE("OSC-2b a headless player listens on the option-configured UDP port and freezes packets",
          "[osc2b][osc][liveinputs][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_osc2b_rpc_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir);

    FastModuleContainer modules;
    configureHeadless(root);

    // Project declarations must exist before input profiles are parsed: the
    // live binding names an input and is validated against the declaration.
    internal::prepareInputActionsRuntime();
    internal::prepareOscListener();
    auto &listener = GET_MODULE(OscListener);
    REQUIRE(listener.statusJson().at("state").get<std::string>() == "off");
    REQUIRE_FALSE(listener.socketOpen());

    // Exactly what --osc-port 0 (with the default interface and no filter)
    // hands the engine after option parsing.
    const auto config =
        parseOscListenerConfig("0", "127.0.0.1", std::nullopt);
    REQUIRE(config.allow.empty());
    GET_MODULE(EngineLaunchConfig).osc_listener = config;
    internal::prepareOscListener();
    REQUIRE(listener.statusJson().at("state").get<std::string>() ==
            "listening");
    const auto port = listener.statusJson().at("port").get<std::uint16_t>();
    REQUIRE(port != 0);

    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("render_frame");

    auto snapshot = rpc.result("get_live_inputs");
    REQUIRE(snapshot.at("osc").at("listener").at("state").get<std::string>() ==
            "listening");
    REQUIRE(snapshot.at("osc").at("listener").at("port").get<std::uint16_t>() ==
            port);
    REQUIRE(snapshot.at("osc").at("bindings") == 1);
    REQUIRE(inputNamed(snapshot, "phase").at("value") == Catch::Approx(0.0));

    REQUIRE(sendOscPhase(port, 0.625f));

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{30};
    double phase = 0.0;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)rpc.result("step_frame");
        snapshot = rpc.result("get_live_inputs");
        phase = inputNamed(snapshot, "phase").at("value").get<double>();
        if (phase == Catch::Approx(0.625)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    REQUIRE(phase == Catch::Approx(0.625));
    REQUIRE(inputNamed(snapshot, "phase").at("source").get<std::string>() ==
            "osc");
    REQUIRE(snapshot.at("osc").at("received").get<std::uint64_t>() >= 1);
    REQUIRE(snapshot.at("osc").at("listener").at("sender_not_allowed")
                .get<std::uint64_t>() == 0);
}

#if defined(PELICAN_LIVEINPUTS_PLAYER)
TEST_CASE("OSC-2c the real --osc-port option listens and freezes a packet",
          "[osc2c][osc][liveinputs][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_osc2c_rpc_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    // No input_actions_json: the live-only profile still loads, and the real
    // --input-profile option selects it in the child player.
    const auto root = copyLiveProject(dir, false);
    PlayerProcess player{root};

    (void)player.result("load_scene", {{"name", "default_scene"}});
    (void)player.result("render_frame");

    auto snapshot = player.result("get_live_inputs");
    const auto listener = snapshot.at("osc").at("listener");
    REQUIRE(listener.at("state").get<std::string>() == "listening");
    const auto port = listener.at("port").get<std::uint16_t>();
    REQUIRE(port != 0);
    REQUIRE(snapshot.at("osc").at("bindings") == 1);
    REQUIRE(inputNamed(snapshot, "phase").at("value") == Catch::Approx(0.0));

    // Nothing was injected into the engine: --osc-port 0 selected this port
    // and the default loopback listener accepted the datagram.
    REQUIRE(sendOscPhase(port, 0.625f));

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{30};
    double phase = 0.0;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)player.result("step_frame");
        snapshot = player.result("get_live_inputs");
        phase = inputNamed(snapshot, "phase").at("value").get<double>();
        if (phase == Catch::Approx(0.625)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    REQUIRE(phase == Catch::Approx(0.625));
    REQUIRE(inputNamed(snapshot, "phase").at("source").get<std::string>() ==
            "osc");
    REQUIRE(snapshot.at("osc").at("received").get<std::uint64_t>() >= 1);

    // A profile without live declarations reports zero bindings, and packets
    // no longer reach the live input.
    REQUIRE(player.result("set_input_profile", {{"name", "plain"}}).at("name") ==
            "plain");
    snapshot = player.result("get_live_inputs");
    REQUIRE(snapshot.at("osc").at("bindings") == 0);
    REQUIRE(sendOscPhase(port, 0.875f));
    (void)player.result("step_frame");
    snapshot = player.result("get_live_inputs");
    REQUIRE(inputNamed(snapshot, "phase").at("value") == Catch::Approx(0.625));

    // Selecting the live-only profile again restores the table.
    REQUIRE(player.result("set_input_profile", {{"name", "live"}}).at("name") ==
            "live");
    REQUIRE(player.result("get_live_inputs").at("osc").at("bindings") == 1);
    REQUIRE(sendOscPhase(port, 0.375f));
    const auto second_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (std::chrono::steady_clock::now() < second_deadline) {
        (void)player.result("step_frame");
        snapshot = player.result("get_live_inputs");
        phase = inputNamed(snapshot, "phase").at("value").get<double>();
        if (phase == Catch::Approx(0.375)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    REQUIRE(phase == Catch::Approx(0.375));
}
#endif

#if defined(PELICAN_LIVEINPUTS_PLAYER) && PELICAN_WITH_OSC
TEST_CASE("OSC-5b the real player publishes graph scalars over UDP",
          "[osc5b][osc][liveinputs][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
        ("pelican_osc5b_rpc_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir, false);
    addOscSendNodes(root);
    OscTestUdp::Receiver receiver;
    REQUIRE(receiver.port() != 0);
    PlayerProcess player{root, {"--osc-send", "127.0.0.1:" + std::to_string(receiver.port())}};
    (void)player.result("load_scene", {{"name", "default_scene"}});
    (void)player.result("render_frame");
    (void)player.result("set_live_input", {{"values", {{"phase", 0.25}, {"steps", 3}}}});
    (void)player.result("step_frame");

    bool phase_arrived = false;
    bool steps_arrived = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (std::chrono::steady_clock::now() < deadline && !(phase_arrived && steps_arrived)) {
        (void)player.result("step_frame");
        while (const auto bytes = receiver.receive(std::chrono::milliseconds{50})) {
            const auto packet = Osc::parsePacket(*bytes);
            REQUIRE(packet.drop == Osc::PacketDrop::none);
            REQUIRE(packet.messages.size() == 1);
            const auto &message = packet.messages[0];
            if (message.address == "/out/phase" && !message.arguments.empty() &&
                message.arguments[0].number == Catch::Approx(0.25))
                phase_arrived = true;
            if (message.address == "/out/steps" && !message.arguments.empty() &&
                message.arguments[0].integer == 3)
                steps_arrived = true;
            if (phase_arrived && steps_arrived) break;
        }
    }
    REQUIRE(phase_arrived);
    REQUIRE(steps_arrived);
    auto status = player.result("get_live_inputs").at("osc").at("send");
    REQUIRE(status.at("sinks") == 2);
    const auto sent = status.at("sent").get<std::uint64_t>();
    for (int frame = 0; frame < 5; ++frame) (void)player.result("step_frame");
    status = player.result("get_live_inputs").at("osc").at("send");
    REQUIRE(status.at("sent").get<std::uint64_t>() == sent);
}

TEST_CASE("OSC-5b opening a project without the player option sends nothing",
          "[osc5b][osc][liveinputs][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
        ("pelican_osc5b_no_flag_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir, false);
    addOscSendNodes(root);
    PlayerProcess player{root};
    (void)player.result("load_scene", {{"name", "default_scene"}});
    (void)player.result("render_frame");
    (void)player.result("step_frame");
    const auto status = player.result("get_live_inputs").at("osc").at("send");
    REQUIRE(status.at("state") == "inactive");
    REQUIRE(status.at("socket_open") == false);
    REQUIRE(status.at("destination") == "");
    REQUIRE(status.at("sent") == 0);
}

TEST_CASE("OSC-5b CLI live replay keeps the sender offline",
          "[osc5b][osc][liveinputs][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
        ("pelican_osc5b_offline_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir, false);
    addOscSendNodes(root);
    const auto recording = dir.root / "offline.live_seq";
    {
        PlayerProcess recorder{root, {"--record-live", recording.generic_string()}};
        (void)recorder.result("load_scene", {{"name", "default_scene"}});
        (void)recorder.result("render_frame");
        for (const double phase : {0.1, 0.2, 0.3}) {
            (void)recorder.result("set_live_input", {{"values", {{"phase", phase}}}});
            (void)recorder.result("step_frame");
        }
    }
    REQUIRE(std::filesystem::exists(recording));
    OscTestUdp::Receiver receiver;
    REQUIRE(receiver.port() != 0);
    PlayerProcess player{root, {"--replay-live", recording.generic_string(),
                                "--osc-send", "127.0.0.1:" + std::to_string(receiver.port())}};
    (void)player.result("load_scene", {{"name", "default_scene"}});
    (void)player.result("render_frame");
    std::size_t datagrams = 0;
    for (int frame = 0; frame < 10; ++frame) {
        (void)player.result("step_frame");
        if (receiver.receive(std::chrono::milliseconds{50})) ++datagrams;
    }
    const auto status = player.result("get_live_inputs").at("osc").at("send");
    REQUIRE(status.at("state") == "offline");
    REQUIRE(status.at("socket_open") == false);
    REQUIRE(status.at("sent") == 0);
    REQUIRE(datagrams == 0);
}

TEST_CASE("OSC-5b RPC replay quiesces taken work and a fresh frame resumes",
          "[osc5b][osc][liveinputs][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
        ("pelican_osc5b_replay_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir, false);
    addOscSendNodes(root);
    const auto recording = dir.root / "rpc.live_seq";
    OscTestUdp::Receiver receiver;
    REQUIRE(receiver.port() != 0);
    PlayerProcess player{root, {"--record-live", recording.generic_string(),
                                "--osc-send", "127.0.0.1:" + std::to_string(receiver.port())}};
    (void)player.result("load_scene", {{"name", "default_scene"}});
    (void)player.result("render_frame");
    for (const double phase : {0.1, 0.2, 0.3}) {
        (void)player.result("set_live_input", {{"values", {{"phase", phase}, {"steps", 2}}}});
        (void)player.result("step_frame");
    }
    (void)player.result("stop_live_record");
    while (receiver.receive(std::chrono::milliseconds{50})) {}

    (void)player.result("start_live_replay", {{"path", recording.generic_string()}});
    auto status = player.result("get_live_inputs").at("osc").at("send");
    REQUIRE(status.at("state") == "replay");
    const auto sent = status.at("sent").get<std::uint64_t>();
    const auto resent = status.at("resent").get<std::uint64_t>();
    std::size_t replay_datagrams = 0;
    for (int frame = 0; frame < 6; ++frame) {
        (void)player.result("step_frame");
        status = player.result("get_live_inputs").at("osc").at("send");
        REQUIRE(status.at("sent").get<std::uint64_t>() == sent);
        REQUIRE(status.at("resent").get<std::uint64_t>() == resent);
        if (receiver.receive(std::chrono::milliseconds{50})) ++replay_datagrams;
    }
    REQUIRE(replay_datagrams == 0);

    (void)player.result("stop_live_replay");
    (void)player.result("set_live_input", {{"values", {{"phase", 0.5}, {"steps", 2}}}});
    bool recovered = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (std::chrono::steady_clock::now() < deadline && !recovered) {
        (void)player.result("step_frame");
        while (const auto bytes = receiver.receive(std::chrono::milliseconds{50})) {
            const auto packet = Osc::parsePacket(*bytes);
            if (!packet.messages.empty() && packet.messages[0].address == "/out/phase" &&
                !packet.messages[0].arguments.empty() &&
                packet.messages[0].arguments[0].number == Catch::Approx(0.5)) {
                recovered = true;
                break;
            }
        }
    }
    REQUIRE(recovered);
    REQUIRE(player.result("get_live_inputs").at("osc").at("send").at("state") == "sending");
}
#endif

TEST_CASE("OSC-4a RPC records frozen values and replays them by relative frame",
          "[osc4a][live_recording][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_osc4a_rpc_" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir);
    const auto recording_path = dir.root / "show.live_seq";
    FastModuleContainer modules;
    configureHeadless(root);
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("render_frame");
    (void)rpc.result("step_frame");
    (void)rpc.result("step_frame");

    (void)rpc.result("start_live_record", {{"path", recording_path.generic_string()}});
    const auto second_record = rpc.error("start_live_record",
        {{"path", (dir.root / "second.live_seq").generic_string()}});
    CHECK_THAT(second_record.at("message").get<std::string>(),
               ContainsSubstring("live_input.record_active@1"));
    CHECK(rpc.result("get_status").at("live_inputs").at("recording").at("lines") == 1);
    const auto first = rpc.result("step_frame");
    CHECK(first.at("frame") == 3);
    CHECK(first.at("t") == Catch::Approx(3.0 / 60.0));
    std::vector<double> expected{0.0};
    for (const double phase : {0.25, 0.75, 0.75, 1.25}) {
        (void)rpc.result("set_live_input", {{"values", {{"phase", phase}}}});
        (void)rpc.result("step_frame");
        expected.push_back(phase);
    }
    const auto stop = rpc.result("stop_live_record");
    CHECK(stop.at("lines") == 5);
    CHECK(stop.at("evaluations") == expected.size());
    CHECK(stop.at("mean_write_ms").get<double>() >= 0.0);
    CHECK(stop.at("max_write_ms").get<double>() >= 0.0);
    CHECK(rpc.result("get_status").at("live_inputs").at("recording").is_null());
    const auto recording = LiveInputRecording::loadFile(recording_path);
    REQUIRE(recording.frames.size() == 4);
    CHECK(recording.end.evaluations == expected.size());
    CHECK(recording.frames.front().values.size() == 3);
    CHECK(recording.frames.back().values.size() == 1);
    const auto stopped_record = rpc.error("stop_live_record");
    CHECK_THAT(stopped_record.at("message").get<std::string>(),
               ContainsSubstring("live_input.record_inactive@1"));

    (void)rpc.result("set_time", {{"t", 2.0 / 60.0}});
    (void)rpc.result("set_live_input", {{"values", {{"phase", 2.0}}}});
    (void)rpc.result("start_live_replay", {{"path", recording_path.generic_string()}});
    const auto active_status = rpc.result("get_status").at("live_inputs");
    CHECK(active_status.at("replaying") == true);
    CHECK(active_status.at("replay_lines") == 4);
    const auto set_error = rpc.error("set_live_input", {{"values", {{"phase", 3.0}}}});
    CHECK_THAT(set_error.at("message").get<std::string>(),
               ContainsSubstring("live_input.replay_active@1"));
    CHECK(rpc.result("get_live_inputs").at("dropped_during_replay") == 1);
    const LiveInputUpdate osc_update{"phase", 3.0};
    const auto osc = GET_MODULE(LiveInputs).stageBatch({&osc_update, 1}, LiveInputSource::osc);
    CHECK(osc.replay == 1);
    CHECK(rpc.result("get_live_inputs").at("dropped_during_replay") == 2);
    const auto time_error = rpc.error("set_time", {{"t", 10.0}});
    CHECK_THAT(time_error.at("message").get<std::string>(),
               ContainsSubstring("live_input.time_change_refused@1"));
    std::vector<std::uint64_t> changed_frames;
    for (std::size_t index = 0; index < expected.size() + 1; ++index) {
        (void)rpc.result("step_frame");
        const auto snapshot = rpc.result("get_live_inputs");
        const auto phase = inputNamed(snapshot, "phase");
        const double wanted = expected[std::min(index, expected.size() - 1)];
        CHECK(phase.at("value") == Catch::Approx(wanted));
        CHECK(phase.at("source") == "replay");
        CHECK(phase.at("clamped") == false);
        changed_frames.push_back(phase.at("changed_frame").get<std::uint64_t>());
    }
    CHECK(changed_frames[0] == 0);
    CHECK(changed_frames[1] != changed_frames[0]);
    CHECK(changed_frames[2] != changed_frames[1]);
    CHECK(changed_frames[3] == changed_frames[2]);
    CHECK(changed_frames[4] != changed_frames[3]);
    CHECK(changed_frames[5] == changed_frames[4]);

    const auto replay_error = rpc.error("start_live_replay", {{"path", recording_path.generic_string()}});
    CHECK_THAT(replay_error.at("message").get<std::string>(), ContainsSubstring("live_input.replay_active@1"));
    const auto record_error = rpc.error("start_live_record",
        {{"path", (dir.root / "overlap.live_seq").generic_string()}});
    CHECK_THAT(record_error.at("message").get<std::string>(),
               ContainsSubstring("live_input.record_while_replaying@1"));
    CHECK(rpc.result("get_status").at("live_inputs").at("replaying") == true);
    CHECK(inputNamed(rpc.result("get_live_inputs"), "phase").at("source") == "replay");
    const auto reload_error = rpc.error("reload_render_documents");
    CHECK_THAT(reload_error.at("message").get<std::string>(),
               ContainsSubstring("live_input.replay_active@1"));
    CHECK(GET_MODULE(watch::ReloadGate).snapshot().reason == "live replay");
    GET_MODULE(EngineLaunchConfig).start_time_explicit = true;
    const auto offline_stop = rpc.error("stop_live_replay");
    CHECK_THAT(offline_stop.at("message").get<std::string>(),
               ContainsSubstring("live_input.replay_required@1"));
    GET_MODULE(EngineLaunchConfig).start_time_explicit = false;
    (void)rpc.result("stop_live_replay");
    auto stopped = inputNamed(rpc.result("get_live_inputs"), "phase");
    CHECK(stopped.at("value") == 0.0);
    CHECK(stopped.at("source") == "default");
    CHECK(rpc.result("get_status").at("live_inputs").at("replaying") == false);
    CHECK(rpc.result("get_live_inputs").at("dropped_during_replay") == 2);
    const auto stop_error = rpc.error("stop_live_replay");
    CHECK_THAT(stop_error.at("message").get<std::string>(), ContainsSubstring("live_input.replay_inactive@1"));
    (void)rpc.result("set_live_input", {{"values", {{"phase", 0.5}}}});
    (void)rpc.result("step_frame");
    CHECK(inputNamed(rpc.result("get_live_inputs"), "phase").at("value") == 0.5);
}

TEST_CASE("OSC-4a RPC replay resamples a recording at another fixed fps",
          "[osc4a][live_recording][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_osc4a_other_fps_" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir);
    const auto recording_path = dir.root / "show.live_seq";
    {
        FastModuleContainer modules;
        configureHeadless(root);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("render_frame");
        (void)rpc.result("start_live_record", {{"path", recording_path.generic_string()}});
        (void)rpc.result("step_frame");
        for (const double phase : {0.25, 0.75, 0.75, 1.25}) {
            (void)rpc.result("set_live_input", {{"values", {{"phase", phase}}}});
            (void)rpc.result("step_frame");
        }
        const auto stop = rpc.result("stop_live_record");
        std::cout << "OSC-4a recording writes: mean=" << stop.at("mean_write_ms")
                  << " ms, max=" << stop.at("max_write_ms") << " ms, lines="
                  << stop.at("lines") << '\n';
    }
    const auto recording = LiveInputRecording::loadFile(recording_path);
    {
        FastModuleContainer modules;
        configureHeadless(root);
        GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 30.0);
        Rpc rpc;
        (void)rpc.result("load_scene", {{"name", "default_scene"}});
        (void)rpc.result("render_frame");
        (void)rpc.result("start_live_replay", {{"path", recording_path.generic_string()}});
        for (std::uint64_t index = 1; index <= 6; ++index) {
            (void)rpc.result("step_frame");
            const auto expected_at_time = recording.valuesAtTime(
                static_cast<double>(index) / 30.0).at("phase");
            CHECK(inputNamed(rpc.result("get_live_inputs"), "phase").at("value") ==
                  Catch::Approx(expected_at_time));
        }
        (void)rpc.result("stop_live_replay");
    }
}

TEST_CASE("OSC-4a RPC combines input sequence and live replay without opening the reload gate",
          "[osc4a][live_recording][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_osc4a_combined_" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()))};
    FastModuleContainer modules;
    configureHeadless(copyLiveProject(dir));
    Rpc rpc;
    (void)rpc.result("render_frame");

    const auto recording_path = dir.root / "combined.live_seq";
    (void)rpc.result("start_live_record", {{"path", recording_path.generic_string()}});
    (void)rpc.result("step_frame");
    (void)rpc.result("set_live_input", {{"values", {{"phase", 0.5}}}});
    (void)rpc.result("step_frame");
    (void)rpc.result("stop_live_record");
    const auto input_path = dir.root / "empty.input_seq";
    {
        std::ofstream output{input_path};
        output << "{\"schema\":\"pelican.input_seq\",\"version\":1,\"fps\":60}\n"
                  "{\"frame\":0}\n";
    }

    const auto input_first = rpc.error("start_input_replay", {{"path", input_path.generic_string()}});
    CHECK_THAT(input_first.at("message").get<std::string>(),
               ContainsSubstring("offline.live_input_unrecorded@1"));
    (void)rpc.result("start_live_replay", {{"path", recording_path.generic_string()}});
    (void)rpc.result("start_input_replay", {{"path", input_path.generic_string()}});
    (void)rpc.result("step_frame");
    CHECK(inputNamed(rpc.result("get_live_inputs"), "phase").at("source") == "replay");
    const auto live_stop_first = rpc.error("stop_live_replay");
    CHECK_THAT(live_stop_first.at("message").get<std::string>(),
               ContainsSubstring("live_input.replay_required@1"));
    CHECK(GET_MODULE(watch::ReloadGate).snapshot().reason == "replay");
    (void)rpc.result("stop_input_replay");
    CHECK(GET_MODULE(watch::ReloadGate).snapshot().reason == "live replay");
    (void)rpc.result("stop_live_replay");
    CHECK(GET_MODULE(watch::ReloadGate).snapshot().reason == "rpc driver");
    (void)rpc.result("set_live_input", {{"values", {{"phase", 0.75}}}});
    (void)rpc.result("step_frame");
    CHECK(inputNamed(rpc.result("get_live_inputs"), "phase").at("value") == 0.75);
}

TEST_CASE("OSC-4a recording failures are named and never fail the frame",
          "[osc4a][live_recording][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_osc4a_failure_" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()))};
    FastModuleContainer modules;
    configureHeadless(copyLiveProject(dir));
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("render_frame");
    const auto open = rpc.error("start_live_record",
        {{"path", (dir.root / "absent" / "bad.live_seq").generic_string()}});
    CHECK_THAT(open.at("message").get<std::string>(), ContainsSubstring("live_input.record_failed@1"));

    const auto path = dir.root / "failure.live_seq";
    (void)rpc.result("start_live_record", {{"path", path.generic_string()}});
    const auto inactive = rpc.error("stop_live_replay");
    CHECK_THAT(inactive.at("message").get<std::string>(), ContainsSubstring("live_input.replay_inactive@1"));
    (void)rpc.result("step_frame");
    GET_MODULE(LiveInputRecordingRuntime).failRecordingAfterLinesForTesting(2);
    (void)rpc.result("set_live_input", {{"values", {{"phase", 0.5}}}});
    const auto frame = rpc.result("step_frame");
    REQUIRE(frame.contains("frame"));
    CHECK(rpc.result("get_status").at("live_inputs").at("recording").is_null());
    CHECK(inputNamed(rpc.result("get_live_inputs"), "phase").at("value") == 0.5);
    const auto text = Json{{"problems", frame.at("problems")}}.dump();
    CHECK_THAT(text, ContainsSubstring("live_input.record_failed@1"));
    CHECK_THAT(text, ContainsSubstring("injected write failure"));
    (void)rpc.result("step_frame");
    CHECK(rpc.result("get_status").at("live_inputs").at("recording").is_null());
}

#if defined(PELICAN_LIVEINPUTS_PLAYER)
TEST_CASE("OSC-4a CLI replay reproduces frozen values and simple graph pixels",
          "[osc4a][live_recording][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_osc4a_cli_" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir, false);
    const auto recording_path = dir.root / "cli.live_seq";
    const std::vector<double> phases{0.0, 0.25, 0.75, 0.75, 1.25};
    std::vector<double> recorded_values;
    std::vector<std::string> recorded_pixels;
    std::vector<double> recorded_changes;
    {
        PlayerProcess player{root, {"--fps", "60", "--record-live",
                                    recording_path.generic_string()}};
        (void)player.result("load_scene", {{"name", "default_scene"}});
        (void)player.result("render_frame");
        for (std::size_t index = 0; index < phases.size(); ++index) {
            (void)player.result("set_live_input", {{"values", {{"phase", phases[index]}}}});
            (void)player.result("step_frame");
            const auto input = inputNamed(player.result("get_live_inputs"), "phase");
            recorded_values.push_back(input.at("value").get<double>());
            recorded_changes.push_back(input.at("changed_frame").get<double>());
            const auto capture = dir.root / ("recorded-" + std::to_string(index) + ".png");
            (void)player.result("capture", {{"path", capture.generic_string()}});
            recorded_pixels.push_back(readFile(capture));
        }
    }
    REQUIRE(recorded_values == phases);
    const auto recording = LiveInputRecording::loadFile(recording_path);
    REQUIRE(recording.end.evaluations == phases.size());
    for (std::size_t index = 0; index < phases.size(); ++index) {
        const auto replayed = recording.valuesAtFrame(index + 1).at("phase");
        CHECK(replayed == recorded_values[index]);
    }

    const auto replay = [&](std::string_view label) {
        INFO(label);
        PlayerProcess player{root, {"--fps", "60", "--replay-live",
                                    recording_path.generic_string()}};
        (void)player.result("load_scene", {{"name", "default_scene"}});
        (void)player.result("render_frame");
        std::vector<double> previous_changes;
        for (std::size_t index = 0; index < phases.size(); ++index) {
            (void)player.result("step_frame");
            const auto input = inputNamed(player.result("get_live_inputs"), "phase");
            CHECK(input.at("value").get<double>() == recorded_values[index]);
            CHECK(input.at("changed_frame").get<double>() == recorded_changes[index]);
            CHECK(input.at("source").get<std::string>() == "replay");
            const auto capture = dir.root / (std::string{label} + "-" +
                std::to_string(index) + ".png");
            (void)player.result("capture", {{"path", capture.generic_string()}});
            CHECK(readFile(capture) == recorded_pixels[index]);
        }
    };
    replay("replay-one");
    replay("replay-two");
}

TEST_CASE("OSC-4b baked tables reproduce fixed-step replay pixels without live input",
          "[osc4b][live_recording][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_osc4b_pixels_" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir, false);
    const auto recording_path = root / "show.live_seq";
    const std::vector<double> phases{0.0, 0.25, 0.75, 0.75, 1.25};
    std::vector<std::string> replay_pixels;
    {
        PlayerProcess player{root, {"--fps", "60", "--record-live",
                                    recording_path.generic_string()}};
        (void)player.result("load_scene", {{"name", "default_scene"}});
        (void)player.result("render_frame");
        for (const double phase : phases) {
            (void)player.result("set_live_input", {{"values", {{"phase", phase}}}});
            (void)player.result("step_frame");
        }
    }
    {
        PlayerProcess player{root, {"--fps", "60"}};
        (void)player.result("load_scene", {{"name", "default_scene"}});
        (void)player.result("render_frame");
        const auto accepted = player.result("bake_live_recording",
            {{"recording", "show.live_seq"},
             {"out", "tables/show.ktx2"}, {"rate", 60}});
        bool committed = false;
        for (int attempt = 0; attempt != 2000; ++attempt) {
            const auto result = player.result("get_edit_result",
                {{"ticket", accepted.at("ticket")}});
            if (result.at("status") == "running") {
                (void)player.result("step_frame");
                continue;
            }
            REQUIRE(result.at("status") == "committed");
            CHECK(result.at("interpolation") == "hold");
            REQUIRE(result.at("features").is_array());
            const auto phase = std::find(result.at("features").begin(),
                result.at("features").end(), "phase");
            REQUIRE(phase != result.at("features").end());
            CHECK(result.at("rows").at(static_cast<std::size_t>(
                std::distance(result.at("features").begin(), phase))) == 0);
            committed = true;
            break;
        }
        REQUIRE(committed);
    }
    const auto recording = LiveInputRecording::loadFile(recording_path);
    REQUIRE(recording.clock.fixed_step == Catch::Approx(1.0 / 60.0));
    CHECK(recording.clock.start_time > 0.0);
    REQUIRE(recording.end.evaluations == phases.size());
    {
        PlayerProcess player{root, {"--fps", "60", "--replay-live",
                                    recording_path.generic_string()}};
        (void)player.result("load_scene", {{"name", "default_scene"}});
        (void)player.result("render_frame");
        for (std::size_t index = 0; index < phases.size(); ++index) {
            (void)player.result("step_frame");
            const auto capture = dir.root / ("table-replay-" + std::to_string(index) + ".png");
            (void)player.result("capture", {{"path", capture.generic_string()}});
            replay_pixels.push_back(readFile(capture));
        }
    }
    CHECK(replay_pixels[0] != replay_pixels[1]);
    CHECK(replay_pixels[1] != replay_pixels[2]);
    CHECK(replay_pixels[2] == replay_pixels[3]);
    CHECK(replay_pixels[3] != replay_pixels[4]);

    auto graph = Json::parse(readFile(root / "passes" / "main.json"));
    const auto replace_clock = [&](Json &nodes) {
        for (auto &node : nodes) {
            if (node.at("id") != "clock") continue;
            REQUIRE(node.at("op") == "value.input");
            REQUIRE(node.at("parameters").at("name") == "live:phase");
            node["op"] = "value.sample_table";
            node["parameters"] = {{"asset", "tables/show.ktx2"}, {"row", "phase"},
                                  {"interpolation", "hold"}};
            node.erase("inputs");
        }
    };
    for (auto &[name, definition] : graph.at("definitions").items()) {
        (void)name;
        replace_clock(definition.at("nodes"));
    }
    replace_clock(graph.at("nodes"));
    writeFile(root / "passes" / "main.json", graph.dump(2));
    const auto removed_recording = dir.root / "removed.live_seq";
    std::filesystem::rename(recording_path, removed_recording);
    {
        PlayerProcess player{root, {"--fps", "60"}};
        (void)player.result("load_scene", {{"name", "default_scene"}});
        (void)player.result("render_frame");
        for (std::size_t index = 0; index < phases.size(); ++index) {
            (void)player.result("step_frame");
            CHECK(inputNamed(player.result("get_live_inputs"), "phase").at("value") == 0.0);
            const auto capture = dir.root / ("table-baked-" + std::to_string(index) + ".png");
            (void)player.result("capture", {{"path", capture.generic_string()}});
            INFO("scene time stays absolute; the sampler subtracts recorded t0=" <<
                 recording.clock.start_time << " at step " << index);
            CHECK(readFile(capture) == replay_pixels[index]);
        }
    }
}

TEST_CASE("OSC-1b deterministic snapshots and real-time output follow the launch table",
          "[osc1b][live_recording][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
        ("pelican_osc1b_launch_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir, false);
    const auto recording_path = dir.root / "complete.live_seq";
    LiveInputRecording recording;
    recording.inputs = {{"phase", LiveInputType::floating, 0.0, 0.0, 4.0}};
    recording.clock = {LiveRecordingClockMode::fixed, 1.0 / 60.0, 1, 1.0 / 60.0, 0};
    recording.frames = {{1, 1.0 / 60.0, {{"phase", 1.0}}},
                        {2, 2.0 / 60.0, {{"phase", 1.0}}}};
    recording.end = {2, 2.0 / 60.0, 2};
    writeFile(recording_path, recording.toJsonLines());
    const auto missing_path = dir.root / "missing.live_seq";
    LiveInputRecording missing = recording;
    missing.inputs = {{"steps", LiveInputType::integer, 1.0, 0.0, 8.0}};
    for (auto &frame : missing.frames) frame.values = {{"steps", 1.0}};
    writeFile(missing_path, missing.toJsonLines());

    const auto snapshot = [&](std::string_view name, std::vector<std::string> extra) {
        INFO("launch case: " + std::string{name});
        const auto path = dir.root / (std::string{name} + ".png");
        extra.insert(extra.begin(), {"--render-out", path.generic_string()});
        const auto run = runPlayer(root, std::move(extra));
        INFO(readFile(run.log_path));
        return std::tuple{run, path, readFile(path)};
    };
    const auto refused = [&](std::string_view name, std::vector<std::string> extra) {
        INFO("refused launch case: " + std::string{name});
        auto [run, path, pixels] = snapshot(name, std::move(extra));
        CHECK(run.exit_code != 0);
        CHECK_THAT(readFile(run.log_path), ContainsSubstring("offline.live_input_unrecorded@1"));
        CHECK_THAT(readFile(run.log_path), ContainsSubstring("live:phase"));
        CHECK_FALSE(std::filesystem::exists(path));
        CHECK(pixels.empty());
        std::filesystem::remove(run.log_path);
    };
    const auto allowed = [&](std::string_view name, std::vector<std::string> extra, bool defaults = false) {
        auto [run, path, pixels] = snapshot(name, std::move(extra));
        CHECK(run.exit_code == 0);
        REQUIRE(std::filesystem::exists(path));
        REQUIRE(pixels.size() > 8);
        CHECK(pixels.substr(1, 3) == "PNG");
        if (defaults) {
            CHECK_THAT(readFile(run.log_path), ContainsSubstring("declared default"));
            CHECK_THAT(readFile(run.log_path), ContainsSubstring("live:phase"));
        }
        std::filesystem::remove(run.log_path);
        return pixels;
    };

    refused("snapshot", {});
    refused("snapshot-rpc", {"--rpc"});
    refused("snapshot-missing-replay", {"--replay-live", missing_path.generic_string()});
    const auto replay_pixels = allowed("snapshot-replay", {"--replay-live", recording_path.generic_string()});
    const auto default_pixels = allowed("snapshot-defaults", {"--live-inputs-at-default"}, true);
    const auto default_pixels_again = allowed("snapshot-defaults-again", {"--live-inputs-at-default"}, true);
    CHECK(replay_pixels.substr(1, 3) == "PNG");
    CHECK(default_pixels == default_pixels_again);
    {
        const auto run = runPlayer(root, {"--render-out", (dir.root / "conflict.png").generic_string(),
            "--replay-live", recording_path.generic_string(), "--live-inputs-at-default"});
        INFO(readFile(run.log_path));
        CHECK(run.exit_code != 0);
        CHECK_THAT(readFile(run.log_path), ContainsSubstring("live_input.defaults_replay_conflict@1"));
        CHECK_FALSE(std::filesystem::exists(dir.root / "conflict.png"));
        std::filesystem::remove(run.log_path);
    }

    auto plain = Json::parse(readFile(root / "passes" / "main.json"));
    for (auto &[name, definition] : plain["definitions"].items()) {
        (void)name;
        for (auto &node : definition["nodes"])
            if (node["id"] == "clock") node["parameters"]["name"] = "scene_time";
    }
    for (auto &node : plain["nodes"])
        if (node["id"] == "clock") node["parameters"]["name"] = "scene_time";
    const auto plain_root = dir.root / "plain_project";
    std::filesystem::copy(root, plain_root, std::filesystem::copy_options::recursive);
    writeFile(plain_root / "passes" / "main.json", plain.dump(2));

    const auto saveOutput = [&](const std::filesystem::path &project,
                                const std::filesystem::path &out, bool rpc) {
        std::filesystem::create_directories(out);
        if (!rpc) return runPlayer(project, {"--output-dir", out.generic_string()});
        PlayerProcess player{project, {"--fps", "60", "--output-dir", out.generic_string()}};
        (void)player.result("load_scene", {{"name", "default_scene"}});
        (void)player.result("render_frame");
        for (int index = 0; index < 4; ++index) (void)player.result("step_frame");
        return PlayerRun{player.finish(), player.logPath()};
    };
    (void)copyLiveProject(dir, false, true);
    for (const bool rpc : {false, true}) {
        CAPTURE(rpc);
        const auto out = dir.root / (rpc ? "output-rpc" : "output");
        const auto run = saveOutput(root, out, rpc);
        INFO(readFile(run.log_path));
        CHECK(run.exit_code == 0);
        CHECK_THAT(readFile(run.log_path), ContainsSubstring("real-time output saving"));
        CHECK_THAT(readFile(run.log_path), ContainsSubstring("live:phase"));
        std::filesystem::path output;
        for (const auto &entry : std::filesystem::directory_iterator{out})
            if (entry.path().extension() == ".png") output = entry.path();
        REQUIRE(!output.empty());
        const auto pixels = readFile(output);
        REQUIRE(pixels.size() > 8);
        CHECK(pixels.substr(1, 3) == "PNG");
        std::filesystem::remove(run.log_path);
    }

    for (const std::vector<std::string> extra : {
        std::vector<std::string>{}, {"--rpc"},
        {"--replay-live", recording_path.generic_string()}, {"--live-inputs-at-default"}}) {
        auto args = extra;
        const auto path = dir.root / ("plain-" + std::to_string(args.size()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".png");
        args.insert(args.begin(), {"--render-out", path.generic_string()});
        PlayerRun run;
        if (std::find(args.begin(), args.end(), "--rpc") != args.end()) {
            PlayerProcess player{plain_root, {"--render-out", path.generic_string()}};
            (void)player.result("load_scene", {{"name", "default_scene"}});
            (void)player.result("render_frame");
            (void)player.result("capture", {{"path", path.generic_string()}});
            run = PlayerRun{player.finish(), player.logPath()};
        } else {
            run = runPlayer(plain_root, std::move(args));
        }
        INFO(readFile(run.log_path));
        CHECK(run.exit_code == 0);
        REQUIRE(std::filesystem::exists(path));
        const auto pixels = readFile(path);
        REQUIRE(pixels.size() > 8);
        CHECK(pixels.substr(1, 3) == "PNG");
        std::filesystem::remove(run.log_path);
    }
    {
        const auto run = runPlayer(plain_root, {"--replay-live", recording_path.generic_string(),
                                                "--live-inputs-at-default"});
        INFO(readFile(run.log_path));
        CHECK(run.exit_code != 0);
        CHECK_THAT(readFile(run.log_path), ContainsSubstring("live_input.defaults_replay_conflict@1"));
        std::filesystem::remove(run.log_path);
    }
    auto plain_output = Json::parse(readFile(root / "passes" / "main.json"));
    for (auto &[name, definition] : plain_output["definitions"].items()) {
        (void)name;
        for (auto &node : definition["nodes"])
            if (node["id"] == "clock") node["parameters"]["name"] = "scene_time";
    }
    for (auto &node : plain_output["nodes"])
        if (node["id"] == "clock") node["parameters"]["name"] = "scene_time";
    std::filesystem::copy_file(root / "shaders" / "osc1b_fill.frag",
        plain_root / "shaders" / "osc1b_fill.frag",
        std::filesystem::copy_options::overwrite_existing);
    writeFile(plain_root / "passes" / "main.json", plain_output.dump(2));
    for (const bool rpc : {false, true}) {
        CAPTURE(rpc);
        const auto out = dir.root / (rpc ? "plain-output-rpc" : "plain-output");
        const auto run = saveOutput(plain_root, out, rpc);
        INFO(readFile(run.log_path));
        CHECK(run.exit_code == 0);
        std::filesystem::path output;
        for (const auto &entry : std::filesystem::directory_iterator{out})
            if (entry.path().extension() == ".png") output = entry.path();
        REQUIRE(!output.empty());
        const auto pixels = readFile(output);
        REQUIRE(pixels.size() > 8);
        CHECK(pixels.substr(1, 3) == "PNG");
        std::filesystem::remove(run.log_path);
    }
}

TEST_CASE("OSC-1b live edits redraw identical stopped times in Pause and edit",
          "[osc1b][live_recording][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
        ("pelican_osc1b_stopped_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir);
    for (const std::string state : {std::string{"paused"}, std::string{"edit"}}) {
        CAPTURE(state);
        PlayerProcess player{root, {"--start-state", state == "edit" ? "edit" : "playing"}};
        (void)player.result("load_scene", {{"name", "default_scene"}});
        (void)player.result("render_frame");
        if (state == "paused") (void)player.result("set_play_state", {{"state", "paused"}});
        (void)player.result("step_frame");
        const auto before_status = player.result("get_status");
        const auto before_path = dir.root / (state + "-before.png");
        (void)player.result("capture", {{"path", before_path.generic_string()}});
        REQUIRE(player.result("set_live_input", {{"values", {{"phase", 0.25}}}}).at("accepted") == 1);
        (void)player.result("step_frame");
        const auto after_status = player.result("get_status");
        const auto after_path = dir.root / (state + "-after.png");
        (void)player.result("capture", {{"path", after_path.generic_string()}});
        REQUIRE(after_status.at("time") == before_status.at("time"));
        REQUIRE(after_status.at("frame").get<std::uint64_t>() >
                before_status.at("frame").get<std::uint64_t>());
        const auto before = readFile(before_path);
        const auto after = readFile(after_path);
        REQUIRE(before.size() > 8);
        REQUIRE(after.size() > 8);
        CHECK(before != after);
    }
}

TEST_CASE("OSC-1b at-default locks RPC replay and OSC in a real player",
          "[osc1b][live_recording][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
        ("pelican_osc1b_locked_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir, false);
    PlayerProcess player{root, {"--live-inputs-at-default"}};
    (void)player.result("load_scene", {{"name", "default_scene"}});
    (void)player.result("render_frame");
    CHECK(player.result("get_status").at("live_inputs").at("at_default") == true);
    auto snapshot = player.result("get_live_inputs");
    REQUIRE(snapshot.at("osc").at("listener").at("state").get<std::string>() == "listening");
    const auto port = snapshot.at("osc").at("listener").at("port").get<std::uint16_t>();
    const auto response = player.call("set_live_input", {{"values", {{"phase", 2.0}}}});
    REQUIRE(response.contains("error"));
    CHECK_THAT(response.at("error").at("message").get<std::string>(),
               ContainsSubstring("live_input.defaults_active@1"));
    REQUIRE(sendOscPhase(port, 2.0f));
    for (int index = 0; index < 3; ++index) (void)player.result("step_frame");
    snapshot = player.result("get_live_inputs");
    CHECK(inputNamed(snapshot, "phase").at("value") == Catch::Approx(0.0));
    CHECK(inputNamed(snapshot, "phase").at("source").get<std::string>() == "default");
    CHECK(snapshot.at("osc").at("received").get<std::uint64_t>() >= 1);
}
#endif

} // namespace Pelican

namespace Pelican {

TEST_CASE("OSC-3d recent traffic keeps unbound values and rejects bad argument shapes",
          "[osc3d][osc][liveinputs][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_osc3d_recent_" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir);
    FastModuleContainer modules;
    configureHeadless(root);
    internal::prepareInputActionsRuntime();
    GET_MODULE(EngineLaunchConfig).osc_listener =
        parseOscListenerConfig("0", "127.0.0.1", std::nullopt);
    internal::prepareOscListener();
    const auto port = GET_MODULE(OscListener).statusJson().at("port").get<std::uint16_t>();
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("render_frame");

    struct Packet { const char *address; const char *tags; std::vector<double> values; };
    const std::array packets{
        Packet{"/unbound/f", "f", {0.25}}, Packet{"/unbound/i", "i", {7}},
        Packet{"/unbound/t", "T", {}}, Packet{"/unbound/off", "F", {}},
        Packet{"/unbound/none", "", {}}, Packet{"/unbound/many", "ff", {1, 2}},
        Packet{"/unbound/string", "s", {}},
    };
    for (const auto &packet : packets) {
        const auto before = rpc.result("get_live_inputs").at("osc").at("received").get<std::uint64_t>();
        REQUIRE(sendOscMessage(port, packet.address, packet.tags, packet.values));
        Json snapshot;
        for (int attempt = 0; attempt < 100; ++attempt) {
            (void)rpc.result("step_frame");
            snapshot = rpc.result("get_live_inputs");
            if (snapshot.at("osc").at("received").get<std::uint64_t>() > before) break;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        REQUIRE(snapshot.at("osc").at("received").get<std::uint64_t>() > before);
        const auto &recent = recentFor(snapshot, packet.address);
        CHECK(recent.at("bound") == false);
        CHECK(recent.at("reason") == "unbound");
        CHECK(recent.at("sequence") == snapshot.at("osc").at("last_sequence"));
        if (std::string_view{packet.address} == "/unbound/f") {
            CHECK(recent.at("arg_type") == "f");
            CHECK(recent.at("value").get<double>() == Catch::Approx(0.25));
        } else if (std::string_view{packet.address} == "/unbound/i") {
            CHECK(recent.at("arg_type") == "i");
            CHECK(recent.at("value") == 7);
        } else if (std::string_view{packet.address} == "/unbound/t") {
            CHECK(recent.at("arg_type") == "T");
            CHECK(recent.at("value") == 1);
        } else if (std::string_view{packet.address} == "/unbound/off") {
            CHECK(recent.at("arg_type") == "F");
            CHECK(recent.at("value") == 0);
        } else {
            CHECK(recent.at("arg_type") == "");
            CHECK(recent.at("value").is_null());
        }
    }
}


TEST_CASE("OSC-3d checked binding edits keep file cache and runtime in agreement",
          "[osc3d][osc][liveinputs][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_osc3d_edit_" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir);
    auto project = Json::parse(readFile(root / "project.json"));
    project["basic_config"]["input_profiles"]["alias"] = "live/profile.json";
    writeFile(root / "project.json", project.dump(2));
    writeFile(root / "live" / "profile.json", R"json({
  "schema": "pelican.input_profile",
  "version": 1,
  "name": "live",
  "custom": {"keep": [1, 2, 3]},
  "bindings": [],
  "live": [{"address": "/phase", "input": "phase", "map": {"from": [0, 1], "to": [0, 4]}}]
}
)json");
    writeFile(root / "live" / "plain.json", readFile(root / "live" / "profile.json"));
    const auto original_profile_bytes = readFile(root / "live" / "plain.json");
    FastModuleContainer modules;
    configureHeadless(root);
    internal::prepareInputActionsRuntime();
    GET_MODULE(EngineLaunchConfig).osc_listener =
        parseOscListenerConfig("0", "127.0.0.1", std::nullopt);
    internal::prepareOscListener();
    const auto port = GET_MODULE(OscListener).statusJson().at("port").get<std::uint16_t>();
    Rpc rpc;
    (void)rpc.result("load_scene", {{"name", "default_scene"}});
    (void)rpc.result("render_frame");

    auto bindings = rpc.result("get_osc_bindings");
    REQUIRE(bindings.at("profile").at("name") == "live");
    REQUIRE(bindings.at("profile").at("reference") == "live/profile.json");
    REQUIRE(bindings.at("live").size() == 1);
    REQUIRE(bindings.at("live") == bindings.at("applied_live"));
    const auto original_digest = bindings.at("digest").get<std::string>();
    Json next = Json::array({
        {{"address", "/fader/1"}, {"input", "phase"}},
    });
    const auto reload_before = rpc.result("get_status").at("reload");
    const Json plain = rpc.result("get_osc_bindings");
    (void)rpc.result("set_input_profile", {{"name", "plain"}});
    auto profile_refused = rpc.error("edit_osc_bindings", {
        {"profile", plain.at("profile")}, {"base_digest", plain.at("digest")}, {"live", next}});
    CHECK(plain.at("digest") == original_digest);
    CHECK(profile_refused.at("data").at("error_code") == "input_profile.profile_changed@1");
    (void)rpc.result("set_input_profile", {{"name", "live"}});
    auto edited = rpc.result("edit_osc_bindings", {
        {"profile", plain.at("profile")}, {"base_digest", original_digest}, {"live", next}});
    REQUIRE(edited.at("committed") == true);
    REQUIRE(edited.at("changed") == true);
    REQUIRE(edited.at("previous_digest") == original_digest);
    REQUIRE(edited.at("digest").get<std::string>() != original_digest);
    REQUIRE(edited.at("applied") == next);
    CHECK(rpc.result("get_status").at("reload") == reload_before);
    CHECK(readFile(root / "live" / "plain.json") == original_profile_bytes);

    const auto written = readFile(root / "live" / "profile.json");
    CHECK(written.find("\"custom\": {") < written.find("\"bindings\":"));
    CHECK(written.find("\"bindings\": [") < written.find("\"live\":"));
    CHECK(written.find("\"keep\": [") != std::string::npos);
    const auto &config = GET_MODULE(ProjectBasicConfig);
    CHECK(config.inputProfileJsons().at("live") == written);
    CHECK(config.inputProfileJsons().at("alias") == written);
    bindings = rpc.result("get_osc_bindings");
    CHECK(bindings.at("live") == next);
    CHECK(bindings.at("applied_live") == next);

    const auto sendAndRead = [&](std::string_view address, double value) {
        const auto before = rpc.result("get_live_inputs").at("osc").at("received").get<std::uint64_t>();
        REQUIRE(sendOscMessage(port, address, "f", {value}));
        Json snapshot;
        for (int attempt = 0; attempt < 100; ++attempt) {
            (void)rpc.result("step_frame");
            snapshot = rpc.result("get_live_inputs");
            if (snapshot.at("osc").at("received").get<std::uint64_t>() > before) break;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        REQUIRE(snapshot.at("osc").at("received").get<std::uint64_t>() > before);
        return snapshot;
    };
    auto snapshot = sendAndRead("/fader/1", 0.625);
    CHECK(inputNamed(snapshot, "phase").at("value").get<double>() == Catch::Approx(0.625));
    CHECK(inputNamed(snapshot, "phase").at("source") == "osc");
    snapshot = sendAndRead("/phase", 0.75);
    CHECK(recentFor(snapshot, "/phase").at("reason") == "unbound");
    CHECK(inputNamed(snapshot, "phase").at("value").get<double>() == Catch::Approx(0.625));

    (void)rpc.result("set_input_profile", {{"name", "alias"}});
    CHECK(rpc.result("get_osc_bindings").at("live") == next);
    snapshot = sendAndRead("/fader/1", 0.5);
    CHECK(inputNamed(snapshot, "phase").at("value").get<double>() == Catch::Approx(0.5));
    CHECK(inputNamed(snapshot, "phase").at("source") == "osc");
    (void)rpc.result("set_input_profile", {{"name", "live"}});

    const auto unchanged = rpc.result("edit_osc_bindings", {
        {"profile", bindings.at("profile")}, {"base_digest", bindings.at("digest")}, {"live", next}});
    CHECK(unchanged.at("changed") == false);
    CHECK(unchanged.at("digest") == bindings.at("digest"));
    CHECK(readFile(root / "live" / "profile.json") == written);

    auto refused = rpc.error("edit_osc_bindings", {
        {"profile", bindings.at("profile")}, {"base_digest", original_digest}, {"live", Json::array()}});
    CHECK(refused.at("data").at("error_code") == "external_modification");
    refused = rpc.error("edit_osc_bindings", {
        {"profile", bindings.at("profile")}, {"base_digest", bindings.at("digest")},
        {"live", Json::array({{{"address", "/bad"}, {"input", "missing"}}})}});
    CHECK(refused.at("data").at("error_code") == "input_profile.bindings_invalid@1");
    CHECK(readFile(root / "live" / "profile.json") == written);

    {
        auto lock = ProjectDocumentTransactionLock::acquire(root);
        refused = rpc.error("edit_osc_bindings", {
            {"profile", bindings.at("profile")}, {"base_digest", bindings.at("digest")},
            {"live", Json::array()}});
        CHECK(refused.at("data").at("error_code") == "input_profile.transaction_busy@1");
    }
    CHECK(readFile(root / "live" / "profile.json") == written);

    (void)rpc.result("set_input_profile", {{"name", "plain"}});
    refused = rpc.error("edit_osc_bindings", {
        {"profile", bindings.at("profile")}, {"base_digest", bindings.at("digest")}, {"live", next}});
    CHECK(refused.at("data").at("error_code") == "input_profile.profile_changed@1");
    (void)rpc.result("set_input_profile", {{"name", "live"}});
    CHECK(rpc.result("get_osc_bindings").at("applied_live") == next);
    snapshot = sendAndRead("/fader/1", 0.75);
    CHECK(inputNamed(snapshot, "phase").at("value").get<double>() == Catch::Approx(0.75));

    GET_MODULE(InputSequenceRuntime).startRecording(dir.root / "capture", 60.0);
    refused = rpc.error("edit_osc_bindings", {
        {"profile", bindings.at("profile")}, {"base_digest", bindings.at("digest")},
        {"live", Json::array()}});
    CHECK(refused.at("data").at("error_code") == "input_profile.edit_during_replay@1");
    (void)GET_MODULE(InputSequenceRuntime).stopRecording();
    CHECK(readFile(root / "live" / "profile.json") == written);
    CHECK(config.inputProfileJsons().at("live") == written);
    CHECK(rpc.result("get_osc_bindings").at("applied_live") == next);

    (void)rpc.result("start_live_record", {{"path", (dir.root / "live_capture.jsonl").string()}});
    refused = rpc.error("edit_osc_bindings", {
        {"profile", bindings.at("profile")}, {"base_digest", bindings.at("digest")},
        {"live", Json::array()}});
    CHECK(refused.at("data").at("error_code") == "input_profile.edit_during_replay@1");
    (void)rpc.result("stop_live_record");
    CHECK(readFile(root / "live" / "profile.json") == written);

    writeFile(dir.root / "capture",
              "{\"schema\":\"pelican.input_seq\",\"version\":1,\"fps\":60}\n{\"frame\":0}\n");
    GET_MODULE(InputSequenceRuntime).startReplay(dir.root / "capture");
    refused = rpc.error("edit_osc_bindings", {
        {"profile", bindings.at("profile")}, {"base_digest", bindings.at("digest")},
        {"live", Json::array()}});
    CHECK(refused.at("data").at("error_code") == "input_profile.edit_during_replay@1");
    GET_MODULE(InputSequenceRuntime).stopReplay();
    CHECK(readFile(root / "live" / "profile.json") == written);
    CHECK(config.inputProfileJsons().at("live") == written);
    CHECK(rpc.result("get_osc_bindings").at("applied_live") == next);

    auto outside = Json::parse(written);
    outside["live"] = Json::array({{{"address", "/outside"}, {"input", "phase"}}});
    writeFile(root / "live" / "profile.json", outside.dump(2));
    const auto observed = rpc.result("get_osc_bindings");
    CHECK(observed.at("live") == outside.at("live"));
    CHECK(observed.at("applied_live") == next);
}

TEST_CASE("OSC-3d binding edits refuse missing and non-project profiles",
          "[osc3d][osc][liveinputs][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
                      ("pelican_osc3d_paths_" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()))};

    SECTION("no active profile") {
        const auto root = copyLiveProject(dir);
        auto project = Json::parse(readFile(root / "project.json"));
        project["basic_config"].erase("input_profile");
        writeFile(root / "project.json", project.dump(2));
        FastModuleContainer modules;
        configureHeadless(root);
        internal::prepareInputActionsRuntime();
        Rpc rpc;
        const auto bindings = rpc.result("get_osc_bindings");
        CHECK(bindings.at("profile").is_null());
        CHECK(bindings.at("digest").is_null());
        CHECK(bindings.at("live").empty());
        const auto refused = rpc.error("edit_osc_bindings", {
            {"profile", {{"name", "live"}, {"reference", "live/profile.json"}}},
            {"base_digest", "unused"}, {"live", Json::array()}});
        CHECK(refused.at("data").at("error_code") == "input_profile.none_active@1");
    }

    SECTION("parent traversal") {
        const auto root = copyLiveProject(dir);
        writeFile(dir.root / "outside.json", readFile(root / "live" / "profile.json"));
        auto project = Json::parse(readFile(root / "project.json"));
        project["basic_config"]["input_profiles"]["live"] = "live/../live/profile.json";
        writeFile(root / "project.json", project.dump(2));
        FastModuleContainer modules;
        configureHeadless(root);
        internal::prepareInputActionsRuntime();
        Rpc rpc;
        const auto refused = rpc.error("edit_osc_bindings", {
            {"profile", {{"name", "live"}, {"reference", "live/../live/profile.json"}}},
            {"base_digest", projectDocumentDigest(readFile(dir.root / "outside.json"))},
            {"live", Json::array()}});
        CHECK(refused.at("data").at("error_code") == "input_profile.not_writable@1");
        CHECK(Json::parse(readFile(dir.root / "outside.json")).at("live").size() == 1);
    }

#ifndef _WIN32
    SECTION("symlink leaving the project") {
        const auto root = copyLiveProject(dir);
        writeFile(dir.root / "outside.json", readFile(root / "live" / "profile.json"));
        FastModuleContainer modules;
        configureHeadless(root);
        internal::prepareInputActionsRuntime();
        std::filesystem::remove(root / "live" / "profile.json");
        std::filesystem::create_symlink(dir.root / "outside.json", root / "live" / "profile.json");
        Rpc rpc;
        const auto refused = rpc.error("edit_osc_bindings", {
            {"profile", {{"name", "live"}, {"reference", "live/profile.json"}}},
            {"base_digest", projectDocumentDigest(readFile(dir.root / "outside.json"))},
            {"live", Json::array()}});
        CHECK(refused.at("data").at("error_code") == "input_profile.not_writable@1");
        CHECK(Json::parse(readFile(dir.root / "outside.json")).at("live").size() == 1);
    }
#endif

    SECTION("read-only regular file") {
        const auto root = copyLiveProject(dir);
        FastModuleContainer modules;
        configureHeadless(root);
        internal::prepareInputActionsRuntime();
        Rpc rpc;
        const auto path = root / "live" / "profile.json";
        const auto before = readFile(path);
        std::filesystem::permissions(path, std::filesystem::perms::owner_read);
        const auto refused = rpc.error("edit_osc_bindings", {
            {"profile", {{"name", "live"}, {"reference", "live/profile.json"}}},
            {"base_digest", projectDocumentDigest(before)}, {"live", Json::array()}});
        std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                               std::filesystem::perms::owner_write);
        CHECK(refused.at("data").at("error_code") == "input_profile.not_writable@1");
        CHECK(readFile(path) == before);
    }
}


} // namespace Pelican

namespace Pelican {
#if defined(PELICAN_LIVEINPUTS_PLAYER) && PELICAN_WITH_JOYSTICK_INPUT
namespace {
Json injectedDevice(std::string op, int slot, std::string name = "Pad") {
    Json params{{"op", std::move(op)}, {"slot", slot}};
    if (params.at("op") == "connect") {
        params["name"] = std::move(name);
        params["guid"] = "shared-guid";
        params["is_gamepad"] = false;
    }
    if (params.at("op") != "disconnect") {
        params["axes"] = {0.25f, -0.5f};
        params["buttons"] = {1, 0};
        params["hats"] = {3};
    }
    return params;
}

void startDmapScene(PlayerProcess &player) {
    (void)player.result("load_scene", {{"name", "default_scene"}});
    (void)player.result("render_frame");
}
}

TEST_CASE("DMAP-1a R1 disabled RPC never starts joystick monitoring", "[dmap1a][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
        ("pelican_dmap1a_r1_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir, false);
    PlayerProcess player{root};
    startDmapScene(player);
    (void)player.result("get_input_devices", {{"values", true}});
    (void)player.result("step_frame");
    const auto reply = player.result("get_input_devices", {{"values", true}});
    CHECK_FALSE(reply.at("enabled").get<bool>());
    CHECK_FALSE(reply.at("monitoring").get<bool>());
    CHECK(reply.at("devices").empty());
    const auto injected = player.call("inject_device_state", injectedDevice("connect", 0));
    REQUIRE(injected.contains("error"));
    CHECK(injected.at("error").at("message").get<std::string>().find(
              "joystick.inject_unavailable@1") != std::string::npos);
}

TEST_CASE("DMAP-1a R2 injected devices expose values, reconnects, and lease expiry", "[dmap1a][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
        ("pelican_dmap1a_r2_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir, false);
    PlayerProcess player{root, {"--joystick-monitor"}};
    startDmapScene(player);
    const auto wrong_type = player.call("get_input_devices", {{"values", 1}});
    REQUIRE(wrong_type.contains("error"));
    CHECK(wrong_type.at("error").at("message").get<std::string>() ==
          "get_input_devices field 'values' must be a bool");

    (void)player.result("inject_device_state", injectedDevice("connect", 0, "First"));
    (void)player.result("inject_device_state", injectedDevice("connect", 1, "Second"));
    (void)player.result("get_input_devices", {{"values", true}});
    (void)player.result("step_frame");
    auto reply = player.result("get_input_devices", {{"values", true}});
    REQUIRE(reply.at("devices").size() == 2);
    CHECK(reply.at("backend") == "injected");
    CHECK(reply.at("engine_instance_id") == player.result("get_status").at("instance_id"));
    CHECK(reply.at("devices")[0].at("instance_id") != reply.at("devices")[1].at("instance_id"));
    CHECK(reply.at("devices")[0].at("axes") == 2);
    CHECK(reply.at("devices")[0].at("values").at("axes") == Json({0.25f, -0.5f}));
    CHECK(reply.at("devices")[0].at("values").at("buttons") == Json({1, 0}));
    CHECK(reply.at("devices")[0].at("values").at("hats") == Json({3}));

    auto update = injectedDevice("set", 0);
    update["axes"] = {0.75f, 1.0f};
    (void)player.result("inject_device_state", update);
    (void)player.result("step_frame");
    reply = player.result("get_input_devices", {{"values", true}});
    CHECK(reply.at("devices")[0].at("values").at("axes") == Json({0.75f, 1.0f}));

    const auto old_id = reply.at("devices")[0].at("instance_id").get<std::uint64_t>();
    const auto old_generation = reply.at("devices")[0].at("generation").get<std::uint64_t>();
    (void)player.result("inject_device_state", injectedDevice("disconnect", 0));
    (void)player.result("inject_device_state", injectedDevice("connect", 0));
    (void)player.result("step_frame");
    reply = player.result("get_input_devices", {{"values", true}});
    CHECK(reply.at("devices")[0].at("instance_id").get<std::uint64_t>() != old_id);
    CHECK(reply.at("devices")[0].at("generation").get<std::uint64_t>() > old_generation);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (reply.value("monitoring", false) && std::chrono::steady_clock::now() < deadline) {
        (void)player.result("step_frame");
        reply = player.result("get_input_devices");
    }
    CHECK_FALSE(reply.at("monitoring").get<bool>());
    const auto serial = reply.at("sample_serial").get<std::uint64_t>();
    (void)player.result("step_frame");
    const auto next = player.result("get_input_devices");
    CHECK(next.at("sample_serial").get<std::uint64_t>() == serial);
    for (const auto &device : next.at("devices")) CHECK_FALSE(device.contains("values"));
}

TEST_CASE("DMAP-1a R3 restarted processes restart device instance IDs", "[dmap1a][rpc][gpu]") {
    setupLogger(true);
    const TempDir dir{std::filesystem::temp_directory_path() /
        ("pelican_dmap1a_r3_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    const auto root = copyLiveProject(dir, false);
    std::string first_engine;
    {
        PlayerProcess player{root, {"--joystick-monitor"}};
        startDmapScene(player);
        first_engine = player.result("get_status").at("instance_id").get<std::string>();
    }
    PlayerProcess restarted{root, {"--joystick-monitor"}};
    startDmapScene(restarted);
    CHECK(restarted.result("get_status").at("instance_id").get<std::string>() != first_engine);
    (void)restarted.result("inject_device_state", injectedDevice("connect", 0));
    (void)restarted.result("get_input_devices", {{"values", true}});
    (void)restarted.result("step_frame");
    const auto reply = restarted.result("get_input_devices", {{"values", true}});
    REQUIRE(reply.at("devices").size() == 1);
    CHECK(reply.at("devices")[0].at("instance_id") == 1);
    CHECK(reply.at("devices")[0].at("sampled").get<bool>());
    CHECK(reply.at("devices")[0].contains("values"));
}
#endif
} // namespace Pelican
