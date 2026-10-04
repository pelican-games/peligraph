// BS-1: a rendering configuration the engine cannot compose at start does not
// stop a player launched with --render-fallback. It runs the engine's built-in
// fallback generation (one flat colour), says why in get_status.render_pipeline
// and takes the fixed configuration through reload_render_documents or a render
// edit. Driven through the production path: a headless `pelican_player --rpc`
// child, line by line over its stdin/stdout.

#include "../src/project/projecttemplate.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>
// The decoder is this executable's own: it links no engine library that has it.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

std::string readFile(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void writeFile(const std::filesystem::path &path, const std::string &text) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << text;
}

constexpr const char *root_reference = "passes/main_rendering_config.json";

// The case CT-1 names (UX-9's): a compute task's buffer port written by hand
// without the element it holds. Composing the graph refuses it as
// graph.port_field_missing@1 before any shader is read.
Json brokenNode() {
    return {{"id", "sim"},
            {"op", "render.compute"},
            {"parameters",
             {{"ports", {{"data", {{"kind", "buffer"}, {"access", "read_write"}}}}},
              {"shader", "shaders/sim.comp"}}},
            {"outputs", Json::array()}};
}

struct TempProject {
    std::filesystem::path root;
    std::string fixed_root;

    explicit TempProject(std::string_view name) {
        root = std::filesystem::temp_directory_path() /
               ("pelican_bs1_" + std::string{name} + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        (void)Pelican::initializeProjectTemplate(root, Pelican::ProjectTemplate::Blank);
        fixed_root = readFile(rootPath());
        auto broken = Json::parse(fixed_root);
        broken.at("nodes").push_back(brokenNode());
        writeFile(rootPath(), broken.dump(2) + "\n");
    }
    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::filesystem::path rootPath() const { return root / root_reference; }
    std::filesystem::path stderrLog(std::string_view run) const {
        return root / ("bs1." + std::string{run} + ".stderr.log");
    }
};

#ifdef _WIN32
std::wstring playerCommand(const std::filesystem::path &project, const std::wstring &flags) {
    const std::filesystem::path player{PELICAN_BS1_PLAYER};
    return L"\"" + player.wstring() + L"\" " + flags + L" --size 160x90 --project \"" + project.wstring() + L"\"";
}

// A player whose stdin is closed at once: the exit code of a launch that is
// refused before RPC.
DWORD runToExit(const std::filesystem::path &project, const std::wstring &flags,
                const std::filesystem::path &stderr_log) {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    REQUIRE(CreatePipe(&stdin_read, &stdin_write, &inherit, 0));
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    HANDLE error_log = CreateFileW(stderr_log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(error_log != INVALID_HANDLE_VALUE);
    auto command = playerCommand(project, flags);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = error_log;
    startup.hStdError = error_log;
    PROCESS_INFORMATION process{};
    const auto working = std::filesystem::path{PELICAN_BS1_PLAYER}.parent_path().wstring();
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        working.c_str(), &startup, &process);
    CloseHandle(stdin_read);
    CloseHandle(stdin_write);
    CloseHandle(error_log);
    REQUIRE(started);
    DWORD code = 0;
    if (WaitForSingleObject(process.hProcess, 180000) != WAIT_OBJECT_0) {
        // This test's own child, stopped so the run cannot hang.
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 10000);
    }
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}

// One headless player with --rpc --render-fallback. Requests are written one
// line at a time and each answer is read before the next request goes out.
class PlayerRpc {
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    PROCESS_INFORMATION process_{};
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<std::string> lines_;
    bool closed_ = false;
    int next_id_ = 1;

    void readLoop() {
        std::string pending;
        char buffer[4096];
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(stdout_read_, buffer, sizeof buffer, &read, nullptr) || read == 0) break;
            pending.append(buffer, read);
            for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
                auto line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::scoped_lock lock{mutex_};
                lines_.push_back(std::move(line));
                arrived_.notify_all();
            }
        }
        std::scoped_lock lock{mutex_};
        closed_ = true;
        arrived_.notify_all();
    }

  public:
    std::optional<DWORD> exit_code;

    PlayerRpc(const std::filesystem::path &project, const std::filesystem::path &stderr_log,
              const std::wstring &extra_flags = {}) {
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE stdin_read = nullptr;
        HANDLE stdout_write = nullptr;
        REQUIRE(CreatePipe(&stdin_read, &stdin_write_, &inherit, 0));
        REQUIRE(CreatePipe(&stdout_read_, &stdout_write, &inherit, 0));
        SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0);
        HANDLE error_log = CreateFileW(stderr_log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(error_log != INVALID_HANDLE_VALUE);
        auto command = playerCommand(project, L"--rpc --headless --render-fallback" + extra_flags);
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdin_read;
        startup.hStdOutput = stdout_write;
        startup.hStdError = error_log;
        const auto working = std::filesystem::path{PELICAN_BS1_PLAYER}.parent_path().wstring();
        const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                            nullptr, working.c_str(), &startup, &process_);
        CloseHandle(stdin_read);
        CloseHandle(stdout_write);
        CloseHandle(error_log);
        REQUIRE(started);
        reader_ = std::thread{[this] { readLoop(); }};
    }

    // Ends the input and waits for the player to leave.
    void close() {
        if (stdin_write_ != nullptr) {
            CloseHandle(stdin_write_);
            stdin_write_ = nullptr;
        }
        if (process_.hProcess != nullptr && !exit_code) {
            // The player leaves when its input ends; one that does not is
            // this test's own child, stopped so the run cannot hang.
            if (WaitForSingleObject(process_.hProcess, 120000) != WAIT_OBJECT_0) {
                TerminateProcess(process_.hProcess, 1);
                WaitForSingleObject(process_.hProcess, 10000);
            }
            DWORD code = 0;
            GetExitCodeProcess(process_.hProcess, &code);
            exit_code = code;
        }
    }

    ~PlayerRpc() {
        close();
        if (process_.hProcess != nullptr) {
            CloseHandle(process_.hThread);
            CloseHandle(process_.hProcess);
        }
        if (reader_.joinable()) reader_.join();
        if (stdout_read_ != nullptr) CloseHandle(stdout_read_);
    }
    PlayerRpc(const PlayerRpc &) = delete;
    PlayerRpc &operator=(const PlayerRpc &) = delete;

    Json call(const std::string &method, const Json &params = Json::object()) {
        const int id = next_id_++;
        const auto line = Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump() + "\n";
        DWORD written = 0;
        REQUIRE(WriteFile(stdin_write_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr));
        REQUIRE(written == line.size());
        std::unique_lock lock{mutex_};
        const auto deadline = std::chrono::steady_clock::now() + 180s;
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

    Json result(const std::string &method, const Json &params = Json::object()) {
        auto response = call(method, params);
        INFO(method << " -> " << response.dump());
        REQUIRE(response.contains("result"));
        return response.at("result");
    }

    // A ticket the editor accepted (or refused at once), until it settles.
    Json settle(const Json &accepted) {
        INFO(accepted.dump());
        if (accepted.value("status", std::string{}) == "rejected") return accepted;
        REQUIRE(accepted.at("status") == "accepted");
        for (int attempt = 0; attempt < 16; ++attempt) {
            (void)result("step_frame");
            auto settled = result("get_edit_result", {{"ticket", accepted.at("ticket")}});
            if (settled.value("status", std::string{}) != "pending") return settled;
        }
        FAIL("the ticket stayed pending");
        return Json{};
    }
};

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
    bool operator==(const Rgb &) const = default;
};

std::ostream &operator<<(std::ostream &out, const Rgb &rgb) {
    return out << "rgb(" << rgb.r << ", " << rgb.g << ", " << rgb.b << ")";
}

// The next frame's pixels through the capture RPC: its four corners' insides
// and whether every pixel is the same colour.
struct Frame {
    Rgb top_left;
    Rgb top_right;
    Rgb bottom_left;
    Rgb bottom_right;
    bool flat = false;
};

Frame capture(PlayerRpc &player, const std::filesystem::path &path) {
    (void)player.result("render_frame");
    std::filesystem::remove(path);
    (void)player.result("capture", {{"path", path.generic_string()}});
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    REQUIRE(pixels != nullptr);
    const auto at = [&](int x, int y) {
        const auto *pixel = pixels + (static_cast<std::size_t>(y) * width + x) * 4;
        return Rgb{pixel[0], pixel[1], pixel[2]};
    };
    Frame frame{at(4, 4), at(width - 5, 4), at(4, height - 5), at(width - 5, height - 5), true};
    const auto first = at(0, 0);
    for (int y = 0; y < height && frame.flat; ++y)
        for (int x = 0; x < width; ++x)
            if (!(at(x, y) == first)) {
                frame.flat = false;
                break;
            }
    stbi_image_free(pixels);
    return frame;
}

// The Blank template's present pass: engine://passthrough with no input draws
// the UV gradient (red grows to the right, green downwards).
void requireBlankGradient(const Frame &frame) {
    INFO("top left " << frame.top_left << ", top right " << frame.top_right << ", bottom left "
                     << frame.bottom_left << ", bottom right " << frame.bottom_right);
    CHECK_FALSE(frame.flat);
    CHECK(frame.top_right.r > frame.top_left.r + 100);
    CHECK(frame.bottom_left.g > frame.top_left.g + 100);
}

void requireFallbackRefusal(const Json &status) {
    INFO(status.dump(2));
    REQUIRE(status.at("state") == "fallback");
    CHECK(status.at("root") == root_reference);
    const auto &refusal = status.at("refusal");
    CHECK(refusal.at("code") == "graph.port_field_missing@1");
    CHECK(refusal.at("node") == "sim");
    CHECK(refusal.at("port") == "data");
    CHECK(refusal.at("field") == "element");
    // CT-2: the document the node was written in.
    CHECK(refusal.at("document") == std::string{"project://"} + root_reference);
    CHECK_THAT(refusal.at("message").get<std::string>(),
               Catch::Matchers::ContainsSubstring("graph.port_field_missing@1"));
}
#endif

} // namespace

#ifdef _WIN32
TEST_CASE("BS-1 a refused rendering configuration starts the fallback, says why, and a reload of the fixed file "
          "brings the project's frames back",
          "[bs1][render-fallback][rpc]") {
    TempProject project{"reload"};
    const auto capture_path = project.root / "capture.png";
    // With the overlay Studio starts its engine with (gizmo, picking,
    // gpu_timing): the fixed configuration enables what it adds.
    PlayerRpc player{project.root, project.stderrLog("reload"),
                     L" --feature-overlay engine://features/editor.json"};

    // 1. The engine is up: the refusal is in get_status, frames run, one colour.
    const auto status = player.result("get_status");
    requireFallbackRefusal(status.at("render_pipeline"));
    const auto frame_before = status.at("frame").get<std::uint64_t>();
    (void)player.result("step_frame");
    (void)player.result("step_frame");
    CHECK(player.result("get_status").at("frame").get<std::uint64_t>() > frame_before);
    const auto fallback = capture(player, capture_path);
    INFO("fallback frame " << fallback.top_left);
    CHECK(fallback.flat);
    CHECK(fallback.top_left.r > fallback.top_left.g);
    {
        // The plan is the fallback generation's, marked as such.
        const auto plan = player.result("get_frame_plan");
        INFO(plan.dump().substr(0, 2000));
        REQUIRE(plan.contains("render_pipeline"));
        requireFallbackRefusal(plan.at("render_pipeline"));
    }
    // The render documents are the ones on disk, the broken root among them.
    const auto documents = player.result("get_render_documents").at("documents");
    REQUIRE_FALSE(documents.empty());
    CHECK(documents.at(0).at("content").get<std::string>() == readFile(project.rootPath()));

    // 2. Nothing fixed yet: a reload composes the root again (nothing on disk
    // is live), is refused by the same name, and the fallback stays.
    {
        const auto result = player.settle(player.result("reload_render_documents"));
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "rejected");
        CHECK(result.at("error").at("code") == "graph.port_field_missing@1");
        requireFallbackRefusal(player.result("get_status").at("render_pipeline"));
    }

    // 3. The file fixed on disk and reloaded: committed, ready, the project's
    // own frames.
    writeFile(project.rootPath(), project.fixed_root);
    {
        const auto result = player.settle(player.result("reload_render_documents"));
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        CHECK(result.at("operation") == "reload_render_documents");
    }
    {
        const auto ready = player.result("get_status").at("render_pipeline");
        INFO(ready.dump(2));
        CHECK(ready.at("state") == "ready");
        CHECK_FALSE(ready.contains("refusal"));
        CHECK_FALSE(player.result("get_frame_plan").contains("render_pipeline"));
    }
    requireBlankGradient(capture(player, capture_path));
    CHECK(readFile(project.rootPath()) == project.fixed_root); // the reload wrote nothing

    player.close();
    CHECK(player.exit_code == 0u);
}

TEST_CASE("BS-1 a render edit on the refused root's digest fixes it and publishes the project's configuration",
          "[bs1][render-fallback][rpc]") {
    TempProject project{"edit"};
    const auto capture_path = project.root / "capture.png";
    PlayerRpc player{project.root, project.stderrLog("edit")};
    requireFallbackRefusal(player.result("get_status").at("render_pipeline"));

    // The edit's base is the digest of the root as it is: the broken bytes.
    const auto document = player.result("get_render_documents").at("documents").at(0);
    const auto result = player.settle(player.result(
        "edit_graph_document", {{"base_source_digest", document.at("digest")},
                                {"ops", Json::array({{{"op", "remove_node"}, {"node", "sim"}}})}}));
    INFO(result.dump(2));
    REQUIRE(result.at("status") == "committed");
    CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");
    requireBlankGradient(capture(player, capture_path));
    // The edit wrote the root without the node.
    CHECK(Json::parse(readFile(project.rootPath())).at("nodes").dump().find("\"sim\"") == std::string::npos);
}

TEST_CASE("BS-1 without --render-fallback the refusal still stops the player at start, and the flag needs --rpc",
          "[bs1][render-fallback][rpc]") {
    TempProject project{"fatal"};
    const auto log = project.stderrLog("fatal");
    CHECK(runToExit(project.root, L"--rpc --headless", log) != 0u);
    const auto text = readFile(log);
    INFO(text);
    CHECK_THAT(text, Catch::Matchers::ContainsSubstring("Pelican fatal error : graph.port_field_missing@1"));

    const auto usage = project.stderrLog("usage");
    CHECK(runToExit(project.root, L"--headless --render-fallback", usage) != 0u);
    CHECK_THAT(readFile(usage), Catch::Matchers::ContainsSubstring("--render-fallback requires --rpc"));
}
#endif
