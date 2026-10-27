// BS-1: a rendering configuration the engine cannot compose at start does not
// stop a player launched with --render-fallback. It runs the engine's built-in
// fallback generation (one flat colour), says why in get_status.render_pipeline
// and takes the fixed configuration through reload_render_documents or a render
// edit. Driven through the production path: a headless `pelican_player --rpc`
// child, line by line over its stdin/stdout.

#include "../src/project/projecttemplate.hpp"
#include "../src/project/renderconfigdocument.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>
// The decoder is this executable's own: it links no engine library that has it.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
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
#include <string_view>
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

// BS-1b: the root as a hand edit leaves it when the closing brace is lost:
// not JSON at all.
std::string withoutLastBrace(std::string text) {
    text.erase(text.rfind('}'), 1);
    return text;
}

// RR-1: a fullscreen pass in the root graph that writes an image of its own
// with the engine's passthrough (Blank's present still draws the gradient).
// Without `location` its color port is what CT-1 names:
// graph.port_field_missing@1 {node: rr1_fill, port: color, field: location}.
Json rr1FillParameters(bool location) {
    const Json color_type{{"semantic", "pelican.render.color_signal@1"},
                          {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
    Json color{{"kind", "image"}, {"type", color_type}, {"access", "write"}, {"attachment", "color"}};
    if (location) color["location"] = 0;
    return {{"pass_set", "main_render"},
            {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}},
            {"ports", {{"color", color}}}};
}

std::string withFillNodes(const std::string &blank_root, bool location) {
    const Json color_type{{"semantic", "pelican.render.color_signal@1"},
                          {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
    auto document = Json::parse(blank_root);
    document.at("nodes").push_back({{"id", "rr1_canvas"},
                                    {"op", "render.image"},
                                    {"parameters",
                                     {{"type", color_type},
                                      {"format", "R16G16B16A16_SFLOAT"},
                                      {"extent_scale", 1.0},
                                      {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}},
                                    {"outputs", Json::array({"resource"})}});
    document.at("nodes").push_back({{"id", "rr1_fill"},
                                    {"op", "render.fullscreen"},
                                    {"parameters", rr1FillParameters(location)},
                                    {"inputs", {{"color", {{"node", "rr1_canvas"}, {"port", "resource"}}}}},
                                    {"outputs", Json::array({"color"})}});
    return document.dump(2) + "\n";
}

struct TempProject {
    std::filesystem::path root;
    std::string fixed_root;
    std::string broken_root;

    // `syntax`: the root loses its last `}` (BS-1b) instead of gaining the
    // node CT-1 names.
    explicit TempProject(std::string_view name, bool syntax = false) {
        root = std::filesystem::temp_directory_path() /
               ("pelican_bs1_" + std::string{name} + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        (void)Pelican::initializeProjectTemplate(root, Pelican::ProjectTemplate::Blank);
        fixed_root = readFile(rootPath());
        if (syntax) {
            broken_root = withoutLastBrace(fixed_root);
        } else {
            auto broken = Json::parse(fixed_root);
            broken.at("nodes").push_back(brokenNode());
            broken_root = broken.dump(2) + "\n";
        }
        writeFile(rootPath(), broken_root);
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

// BS-1b: the refusal of a root that is not JSON, where it is reported (the
// startup refusal, or a rejected edit's error).
void requireSyntaxRefusal(const Json &refusal) {
    INFO(refusal.dump(2));
    CHECK(refusal.at("code") == "render_document_syntax@1");
    CHECK(refusal.at("document") == std::string{"project://"} + root_reference);
    // Where the parser stopped: past the last line, the brace it expected.
    CHECK(refusal.at("line").get<int>() > 1);
    CHECK(refusal.at("column").get<int>() >= 1);
    CHECK_THAT(refusal.at("message").get<std::string>(),
               Catch::Matchers::ContainsSubstring("render_document_syntax@1") &&
                   Catch::Matchers::ContainsSubstring("is not valid JSON"));
}

// The fallback's frames: they run, and are one flat colour (the engine's, not
// Blank's gradient).
void requireFallbackFrames(PlayerRpc &player, const std::filesystem::path &capture_path) {
    const auto before = player.result("get_status").at("frame").get<std::uint64_t>();
    (void)player.result("step_frame");
    (void)player.result("step_frame");
    CHECK(player.result("get_status").at("frame").get<std::uint64_t>() > before);
    const auto frame = capture(player, capture_path);
    INFO("fallback frame " << frame.top_left);
    CHECK(frame.flat);
    CHECK(frame.top_left.r > frame.top_left.g);
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

TEST_CASE("BS-1b a root that is not JSON starts the fallback, is listed as its bytes, and a reload of the fixed "
          "file or a restore of fixed content brings the project's frames back",
          "[bs1][render-fallback][rpc]") {
    TempProject project{"syntax", /*syntax=*/true};
    const auto capture_path = project.root / "capture.png";
    {
        PlayerRpc player{project.root, project.stderrLog("syntax")};

        // 1. Up on the fallback, refused by the syntax's name, frames run.
        const auto status = player.result("get_status").at("render_pipeline");
        INFO(status.dump(2));
        REQUIRE(status.at("state") == "fallback");
        CHECK(status.at("root") == root_reference);
        requireSyntaxRefusal(status.at("refusal"));
        requireFallbackFrames(player, capture_path);

        // The root is listed as the bytes on disk, with their digest.
        const auto root = player.result("get_render_documents").at("documents").at(0);
        CHECK(root.at("kind") == "root");
        CHECK(root.at("content").get<std::string>() == project.broken_root);
        const auto digest = root.at("digest").get<std::string>();
        CHECK(digest.size() == 64u);

        // A structural edit of it is refused by the same name.
        {
            const auto result = player.result(
                "edit_graph_document",
                {{"base_source_digest", digest},
                 {"ops", Json::array({{{"op", "remove_node"}, {"node", "nothing"}}})}});
            INFO(result.dump(2));
            REQUIRE(result.at("status") == "rejected");
            requireSyntaxRefusal(result.at("error"));
            CHECK(result.at("error").at("method") == "edit_graph_document");
        }
        // So is a reload of the file as it still is; the fallback stays.
        {
            const auto result = player.settle(player.result("reload_render_documents"));
            INFO(result.dump(2));
            REQUIRE(result.at("status") == "rejected");
            requireSyntaxRefusal(result.at("error"));
            CHECK(player.result("get_status").at("render_pipeline").at("state") == "fallback");
        }

        // 2. Fixed on disk and reloaded: committed, ready, Blank's gradient.
        writeFile(project.rootPath(), project.fixed_root);
        {
            const auto result = player.settle(player.result("reload_render_documents"));
            INFO(result.dump(2));
            REQUIRE(result.at("status") == "committed");
        }
        {
            const auto ready = player.result("get_status").at("render_pipeline");
            INFO(ready.dump(2));
            CHECK(ready.at("state") == "ready");
            CHECK_FALSE(ready.contains("refusal"));
        }
        requireBlankGradient(capture(player, capture_path));
        player.close();
        CHECK(player.exit_code == 0u);
    }

    // 3. Broken again and started again: the root replaced whole through
    // restore_render_documents. Content that is not JSON is refused by name;
    // the fixed content is committed and written.
    writeFile(project.rootPath(), project.broken_root);
    PlayerRpc player{project.root, project.stderrLog("syntax_restore")};
    REQUIRE(player.result("get_status").at("render_pipeline").at("state") == "fallback");
    const auto root = player.result("get_render_documents").at("documents").at(0);
    const auto restore = [&](const std::string &content) {
        return player.settle(player.result(
            "restore_render_documents",
            {{"documents", Json::array({{{"reference", root.at("reference")},
                                         {"base_digest", root.at("digest")},
                                         {"content", content}}})}}));
    };
    {
        const auto result = restore(withoutLastBrace(project.fixed_root) + " ");
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "rejected");
        requireSyntaxRefusal(result.at("error"));
        CHECK(result.at("error").at("method") == "restore_render_documents");
    }
    {
        const auto result = restore(project.fixed_root);
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
    }
    CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");
    requireBlankGradient(capture(player, capture_path));
    CHECK(readFile(project.rootPath()) == project.fixed_root);
    player.close();
    CHECK(player.exit_code == 0u);
}

namespace {

// RR-1: the problems get_problems lists now.
Json problemsOf(PlayerRpc &player) {
    return player.result("get_problems").at("problems");
}

std::uint64_t publishedGeneration(PlayerRpc &player) {
    return player.result("get_frame_node_order").at("published_generation").get<std::uint64_t>();
}

// RR-1: a player started normally on `live_root` (rr1_fill with its location),
// whose disk root is then saved as `broken` and reloaded. The reload is refused
// for the documents; the generation before goes on drawing, and the editor
// holds what is on disk: the reads return it, marked not live. Returns the held
// root's digest.
std::string requireRefusedReloadHeld(PlayerRpc &player, const TempProject &project, const std::string &broken,
                                     const std::filesystem::path &capture_path, std::uint64_t generation) {
    writeFile(project.rootPath(), broken);
    const auto result = player.settle(player.result("reload_render_documents"));
    INFO(result.dump(2));
    REQUIRE(result.at("status") == "rejected");
    const auto refusal = result.at("error");
    CHECK(refusal.at("code") == "graph.port_field_missing@1");
    CHECK(refusal.at("node") == "rr1_fill");
    CHECK(refusal.at("port") == "color");
    CHECK(refusal.at("field") == "location");

    // The live generation is the one before: ready, not republished, Blank's
    // gradient.
    CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");
    CHECK(publishedGeneration(player) == generation);
    requireBlankGradient(capture(player, capture_path));

    // The reads are the disk's version, marked not live.
    const auto documents = player.result("get_render_documents");
    CHECK(documents.at("live") == false);
    const auto root = documents.at("documents").at(0);
    CHECK(root.at("content").get<std::string>() == broken);
    const auto digest = root.at("digest").get<std::string>();
    const auto graph = player.result("get_graph_instances");
    CHECK(graph.at("live") == false);
    CHECK_FALSE(graph.contains("held_unparsed"));
    CHECK(graph.at("source_digest").at("hex") == digest);
    bool found = false;
    for (const auto &node : graph.at("nodes"))
        if (node.at("id") == "rr1_fill") {
            found = true;
            CHECK_FALSE(node.at("parameters").at("ports").at("color").contains("location"));
        }
    CHECK(found);

    // PB-1: the refusal is listed.
    const auto problems = problemsOf(player);
    INFO(problems.dump(2));
    REQUIRE(problems.size() == 1u);
    CHECK(problems.at(0).at("code") == "graph.port_field_missing@1");
    CHECK(problems.at(0).at("source") == "reload_rpc");
    return digest;
}

// RR-1: what the documents published again look like: no problem, the reads
// live, the project's own frames with rr1_fill in the plan.
void requireFixedAndLive(PlayerRpc &player, const std::filesystem::path &capture_path, std::uint64_t generation) {
    CHECK(problemsOf(player).empty());
    CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");
    const auto graph = player.result("get_graph_instances");
    CHECK(graph.at("live") == true);
    CHECK_FALSE(graph.contains("held_unparsed"));
    CHECK(player.result("get_render_documents").at("live") == true);
    CHECK(publishedGeneration(player) > generation);
    requireBlankGradient(capture(player, capture_path));
    CHECK(player.result("get_frame_plan").dump().find("rr1_fill") != std::string::npos);
}

} // namespace

TEST_CASE("RR-1 a reload refused after a normal start makes the disk's root the editor's base: the reads return it "
          "marked not live, a reload as it is stays refused, and a restore on its digest fixes it",
          "[rr1][render-fallback][rpc]") {
    TempProject project{"rr1_restore"};
    const auto capture_path = project.root / "capture.png";
    const auto live_root = withFillNodes(project.fixed_root, true);
    writeFile(project.rootPath(), live_root);
    PlayerRpc player{project.root, project.stderrLog("rr1_restore")};
    REQUIRE(player.result("get_status").at("render_pipeline").at("state") == "ready");
    CHECK(player.result("get_graph_instances").at("live") == true);
    const auto generation = publishedGeneration(player);

    const auto broken = withFillNodes(project.fixed_root, false);
    const auto digest = requireRefusedReloadHeld(player, project, broken, capture_path, generation);

    // Reloaded as it still is: composed again (what the editor holds is not
    // live), refused again, still listed. Not a no_change that would clear it.
    {
        const auto result = player.settle(player.result("reload_render_documents"));
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "rejected");
        CHECK(result.at("error").at("code") == "graph.port_field_missing@1");
        CHECK(problemsOf(player).size() == 1u);
    }

    // The root restored on the held digest: committed, written, published.
    const auto result = player.settle(player.result(
        "restore_render_documents",
        {{"documents", Json::array({{{"reference", root_reference},
                                     {"base_digest", digest},
                                     {"content", live_root}}})}}));
    INFO(result.dump(2));
    REQUIRE(result.at("status") == "committed");
    CHECK(readFile(project.rootPath()) == live_root);
    requireFixedAndLive(player, capture_path, generation);
    player.close();
    CHECK(player.exit_code == 0u);
}

TEST_CASE("RR-1 a structural edit on the refused root's digest adds the missing field, publishes, and the problem "
          "goes",
          "[rr1][render-fallback][rpc]") {
    TempProject project{"rr1_edit"};
    const auto capture_path = project.root / "capture.png";
    writeFile(project.rootPath(), withFillNodes(project.fixed_root, true));
    PlayerRpc player{project.root, project.stderrLog("rr1_edit")};
    REQUIRE(player.result("get_status").at("render_pipeline").at("state") == "ready");
    const auto generation = publishedGeneration(player);
    const auto digest = requireRefusedReloadHeld(player, project, withFillNodes(project.fixed_root, false),
                                                 capture_path, generation);

    // An edit that leaves the field out is refused by name and writes nothing.
    {
        const auto result = player.settle(player.result(
            "edit_graph_document",
            {{"base_source_digest", digest},
             {"ops", Json::array({{{"op", "set_node_label"}, {"node", "rr1_fill"}, {"label", "Fill"}}})}}));
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "rejected");
        CHECK(result.at("error").at("code") == "graph.port_field_missing@1");
        CHECK(readFile(project.rootPath()) == withFillNodes(project.fixed_root, false));
        CHECK(problemsOf(player).size() == 1u);
    }

    Json fix = Json::object();
    fix["op"] = "set_node_parameters";
    fix["node"] = "rr1_fill";
    fix["parameters"] = rr1FillParameters(true);
    Json params = Json::object();
    params["base_source_digest"] = digest;
    params["ops"] = Json::array({fix});
    const auto accepted = player.result("edit_graph_document", params);
    INFO(accepted.dump(2));
    const auto result = player.settle(accepted);
    INFO(result.dump(2));
    REQUIRE(result.at("status") == "committed");
    const auto written = Json::parse(readFile(project.rootPath()));
    for (const auto &node : written.at("nodes"))
        if (node.at("id") == "rr1_fill")
            CHECK(node.at("parameters").at("ports").at("color").at("location") == 0);
    requireFixedAndLive(player, capture_path, generation);
    player.close();
    CHECK(player.exit_code == 0u);
}

TEST_CASE("RR-1 a reload refused for a root that is not JSON holds its bytes, the Graph reads keep the root before, "
          "and a restore on the bytes' digest fixes it",
          "[rr1][render-fallback][rpc]") {
    TempProject project{"rr1_syntax"};
    const auto capture_path = project.root / "capture.png";
    const auto live_root = withFillNodes(project.fixed_root, true);
    writeFile(project.rootPath(), live_root);
    PlayerRpc player{project.root, project.stderrLog("rr1_syntax")};
    REQUIRE(player.result("get_status").at("render_pipeline").at("state") == "ready");
    const auto generation = publishedGeneration(player);
    const auto live_digest =
        player.result("get_graph_instances").at("source_digest").at("hex").get<std::string>();

    const auto broken = withoutLastBrace(live_root);
    writeFile(project.rootPath(), broken);
    {
        const auto result = player.settle(player.result("reload_render_documents"));
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "rejected");
        requireSyntaxRefusal(result.at("error"));
    }
    // The generation before goes on.
    CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");
    CHECK(publishedGeneration(player) == generation);
    requireBlankGradient(capture(player, capture_path));
    // The documents are the bytes on disk, not live; the structural reads go on
    // answering from the root before (the live one), marked held_unparsed.
    const auto documents = player.result("get_render_documents");
    CHECK(documents.at("live") == false);
    const auto root = documents.at("documents").at(0);
    CHECK(root.at("content").get<std::string>() == broken);
    {
        const auto graph = player.result("get_graph_instances");
        INFO(graph.dump(2).substr(0, 2000));
        CHECK(graph.at("live") == true);
        CHECK(graph.at("held_unparsed") == true);
        CHECK(graph.at("held_reason") == "syntax");
        CHECK(graph.at("source_digest").at("hex") == live_digest);
        CHECK(graph.at("nodes").dump().find("rr1_fill") != std::string::npos);
    }
    CHECK(problemsOf(player).size() == 1u);

    // A structural edit writes nothing: on the held bytes' digest it is
    // refused for their syntax (BS-1b's name); on the live root's digest the
    // reads return it is external_modification, which Studio reads as the
    // file on disk that could not be loaded (RL-1s decision 5).
    for (const auto &base : {root.at("digest").get<std::string>(), live_digest}) {
        const auto result = player.result(
            "edit_graph_document", {{"base_source_digest", base},
                                    {"ops", Json::array({{{"op", "remove_node"}, {"node", "rr1_fill"}}})}});
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "rejected");
        if (base == live_digest)
            CHECK(result.at("error").at("code") == "external_modification");
        else
            requireSyntaxRefusal(result.at("error"));
        CHECK(readFile(project.rootPath()) == broken);
    }

    const auto result = player.settle(player.result(
        "restore_render_documents",
        {{"documents", Json::array({{{"reference", root.at("reference")},
                                     {"base_digest", root.at("digest")},
                                     {"content", live_root}}})}}));
    INFO(result.dump(2));
    REQUIRE(result.at("status") == "committed");
    CHECK(readFile(project.rootPath()) == live_root);
    requireFixedAndLive(player, capture_path, generation);
    player.close();
    CHECK(player.exit_code == 0u);
}

TEST_CASE("EB-7 a root broken on disk, refused by a reload, then saved back as the live bytes: the next reload "
          "commits, the problem goes and the reads are live again",
          "[eb7][rr1][render-fallback][rpc]") {
    TempProject project{"eb7_back"};
    const auto capture_path = project.root / "capture.png";
    const auto live_root = withFillNodes(project.fixed_root, true);
    writeFile(project.rootPath(), live_root);
    PlayerRpc player{project.root, project.stderrLog("eb7_back")};
    REQUIRE(player.result("get_status").at("render_pipeline").at("state") == "ready");
    const auto generation = publishedGeneration(player);
    const auto live_digest =
        player.result("get_graph_instances").at("source_digest").at("hex").get<std::string>();
    const auto live_problems_revision =
        player.result("get_status").at("problems").at("revision").get<std::uint64_t>();

    (void)requireRefusedReloadHeld(player, project, withFillNodes(project.fixed_root, false), capture_path,
                                   generation);
    const auto refused_revision =
        player.result("get_status").at("problems").at("revision").get<std::uint64_t>();
    CHECK(refused_revision > live_problems_revision);

    // Put back outside, byte for byte what is live: nothing is wrong any more.
    writeFile(project.rootPath(), live_root);
    const auto result = player.settle(player.result("reload_render_documents"));
    INFO(result.dump(2));
    REQUIRE(result.at("status") == "committed");
    {
        const auto problems = problemsOf(player);
        INFO(problems.dump(2));
        CHECK(problems.empty());
        const auto status = player.result("get_status");
        CHECK(status.at("problems").at("count") == 0);
        CHECK(status.at("problems").at("revision").get<std::uint64_t>() > refused_revision);
        CHECK(status.at("render_pipeline").at("state") == "ready");
    }
    const auto graph = player.result("get_graph_instances");
    CHECK(graph.at("live") == true);
    CHECK_FALSE(graph.contains("held_unparsed"));
    CHECK(graph.at("source_digest").at("hex") == live_digest);
    CHECK(player.result("get_render_documents").at("live") == true);
    requireBlankGradient(capture(player, capture_path));

    // A reload of the same bytes again finds nothing to do.
    const auto again = player.settle(player.result("reload_render_documents"));
    INFO(again.dump(2));
    REQUIRE(again.at("status") == "committed");
    CHECK(again.value("no_change", false));
    CHECK(problemsOf(player).empty());

    // An edit on the live digest goes through (the editor's base is the live
    // root again, not the refused one).
    const auto edited = player.settle(player.result(
        "edit_graph_document",
        {{"base_source_digest", live_digest},
         {"ops", Json::array({{{"op", "set_node_label"}, {"node", "rr1_fill"}, {"label", "Fill"}}})}}));
    INFO(edited.dump(2));
    CHECK(edited.at("status") == "committed");
    player.close();
    CHECK(player.exit_code == 0u);
}

namespace {

// EB-7: RTL-3s's scenario fixture (studio_gui_rt_checks), copied: the root
// graph's ray tracing task `trace` (render.compute) with a ray group layout.
struct RtChecksProject {
    std::filesystem::path root;
    std::string live_root;

    RtChecksProject() {
        root = std::filesystem::temp_directory_path() /
               ("pelican_eb7_rt_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::copy(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" /
                                  "studio_gui_rt_checks",
                              root, std::filesystem::copy_options::recursive);
        live_root = readFile(rootPath());
    }
    ~RtChecksProject() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::filesystem::path rootPath() const { return root / root_reference; }
    std::filesystem::path stderrLog(std::string_view run) const {
        return root / ("eb7." + std::string{run} + ".stderr.log");
    }
    // The primary ray's mesh cell given the closest hit whose shader record is
    // not the layout's (the scenario's wrong_record).
    std::string refusedRoot() const {
        auto text = live_root;
        const std::string cell = "\"closesthit\": \"project://shaders/start\"";
        const auto at = text.find(cell);
        REQUIRE(at != std::string::npos);
        text.replace(at, cell.size(), "\"closesthit\": \"project://shaders/wrong_record\"");
        return text;
    }
};

// The record refusal of the root graph's node `trace`, where a refusal or a
// problem carries its fields.
void requireTraceRecordRefusal(const Json &fields) {
    INFO(fields.dump(2));
    CHECK(fields.at("task") == "trace");
    CHECK(fields.at("pipeline_stage") == "runtime");
    CHECK(fields.at("stage") == "closesthit");
    CHECK(fields.at("ray_type") == "primary");
    CHECK(fields.at("group") == "mesh");
    CHECK_THAT(fields.at("shader").get<std::string>(), Catch::Matchers::ContainsSubstring("wrong_record"));
}

// The root graph's node's problem: document_kind root_graph and the node.
void requireTraceNodeProblem(const Json &problem, std::string_view source) {
    INFO(problem.dump(2));
    CHECK(problem.at("source") == std::string{source});
    CHECK(problem.at("code") == "pelican.ray_tracing.shader_record_abi_mismatch@1");
    CHECK(problem.at("document") == std::string{"project://"} + root_reference);
    CHECK(problem.at("document_kind") == "root_graph");
    CHECK(problem.at("node") == "trace");
    requireTraceRecordRefusal(problem.at("fields"));
}

} // namespace

TEST_CASE("EB-7 the root graph's ray tracing task refused by a reload or at start is its node's problem, and the "
          "root saved back as the live bytes clears it",
          "[eb7][rr1][render-fallback][rpc]") {
    RtChecksProject project;
    const auto refused_root = project.refusedRoot();
    {
        PlayerRpc player{project.root, project.stderrLog("reload")};
        REQUIRE(player.result("get_status").at("render_pipeline").at("state") == "ready");
        (void)player.result("step_frame");
        (void)player.result("step_frame");
        const auto live_digest =
            player.result("get_graph_instances").at("source_digest").at("hex").get<std::string>();

        // Broken outside: the reload is refused by the record's code, at the
        // node, and listed as the node's problem.
        writeFile(project.rootPath(), refused_root);
        {
            const auto result = player.settle(player.result("reload_render_documents"));
            INFO(result.dump(2));
            REQUIRE(result.at("status") == "rejected");
            const auto &error = result.at("error");
            CHECK(error.at("code") == "pelican.ray_tracing.shader_record_abi_mismatch@1");
            // The editor names the root as RTL-1c fixed it (the source
            // reference as the project names it).
            CHECK(error.at("document") == root_reference);
            CHECK(error.at("node") == "trace");
            requireTraceRecordRefusal(error);
        }
        {
            const auto problems = problemsOf(player);
            INFO(problems.dump(2));
            REQUIRE(problems.size() == 1u);
            requireTraceNodeProblem(problems.at(0), "reload_rpc");
        }
        CHECK(player.result("get_render_documents").at("live") == false);
        CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");

        // Put back outside, byte for byte what is live: the next reload takes
        // it in, and nothing is wrong any more.
        writeFile(project.rootPath(), project.live_root);
        const auto result = player.settle(player.result("reload_render_documents"));
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
        CHECK(problemsOf(player).empty());
        CHECK(player.result("get_status").at("problems").at("count") == 0);
        const auto graph = player.result("get_graph_instances");
        CHECK(graph.at("live") == true);
        CHECK(graph.at("source_digest").at("hex") == live_digest);
        CHECK(player.result("get_render_documents").at("live") == true);
        CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");
        player.close();
        CHECK(player.exit_code == 0u);
    }

    // At start (the fallback): the same problem, by the same node.
    writeFile(project.rootPath(), refused_root);
    PlayerRpc player{project.root, project.stderrLog("startup")};
    {
        const auto status = player.result("get_status").at("render_pipeline");
        INFO(status.dump(2));
        REQUIRE(status.at("state") == "fallback");
        CHECK(status.at("refusal").at("code") == "pelican.ray_tracing.shader_record_abi_mismatch@1");
        CHECK(status.at("refusal").at("node") == "trace");
        // At start, as CT-2's graph refusals name the root.
        CHECK(status.at("refusal").at("document") == std::string{"project://"} + root_reference);
    }
    {
        const auto problems = problemsOf(player);
        INFO(problems.dump(2));
        REQUIRE(problems.size() == 1u);
        requireTraceNodeProblem(problems.at(0), "startup");
    }
    writeFile(project.rootPath(), project.live_root);
    const auto result = player.settle(player.result("reload_render_documents"));
    INFO(result.dump(2));
    REQUIRE(result.at("status") == "committed");
    CHECK(problemsOf(player).empty());
    CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");
    player.close();
    CHECK(player.exit_code == 0u);
}
#endif

#ifdef _WIN32
namespace {

// FO-1: GL-5d's studio_gui_graph_order root: `left` and `right` draw the
// gradient into images of their own and read nothing; `present` reads both and
// shows the first (Blank's gradient).
std::string fo1OrderRoot() {
    const auto target = [](const char *name) {
        return Json{{"name", name},
                    {"extent_scale", 1},
                    {"format", "R16G16B16A16_SFLOAT"},
                    {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
                    {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}};
    };
    const auto pass = [](const char *name, Json input, const char *output) {
        return Json{{"name", name},
                    {"type", "fullscreen"},
                    {"input", std::move(input)},
                    {"output", {{"color", {output}}, {"depth", nullptr}}},
                    {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}};
    };
    return Json{{"schema", "pelican.graph"},
                {"version", 1},
                {"definitions", Json::object()},
                {"nodes", Json::array()},
                {"outputs", Json::object()},
                {"rendering",
                 {{"render_targets", Json::array({target("left_color"), target("right_color")})},
                  {"rendering_passes",
                   Json::array({{{"name", "main_render"},
                                 {"passes", Json::array({pass("left", Json::array(), "left_color"),
                                                         pass("right", Json::array(), "right_color"),
                                                         pass("present", {"left_color", "right_color"},
                                                              "display")})}}})}}}}
               .dump(2) +
           "\n";
}

std::vector<std::string> framePlanNames(PlayerRpc &player) {
    std::vector<std::string> names;
    const auto plan = player.result("get_frame_plan");
    for (const auto &node : plan.at("nodes"))
        names.push_back(node.at("name").get<std::string>());
    return names;
}

std::ptrdiff_t positionOf(const std::vector<std::string> &names, std::string_view name) {
    const auto found = std::find(names.begin(), names.end(), name);
    REQUIRE(found != names.end());
    return found - names.begin();
}

} // namespace

TEST_CASE("FO-1 Order... swaps two passes of the rendering section with no dependency through edit_frame_node_order, "
          "and an order against a read is refused by name",
          "[fo1][frame-node-order][rpc]") {
    TempProject project{"fo1_order"};
    writeFile(project.rootPath(), fo1OrderRoot());
    const auto capture_path = project.root / "capture.png";
    PlayerRpc player{project.root, project.stderrLog("fo1_order")};
    REQUIRE(player.result("get_status").at("render_pipeline").at("state") == "ready");
    auto names = framePlanNames(player);
    REQUIRE(positionOf(names, "left") < positionOf(names, "right"));
    REQUIRE(positionOf(names, "right") < positionOf(names, "present"));
    const auto generation = publishedGeneration(player);
    const auto edit = [&](const Json &orders) {
        const auto context = player.result("get_frame_node_order");
        return player.settle(player.result(
            "edit_frame_node_order",
            {{"base_source_digest", context.at("source_digest").at("hex")}, {"orders", orders}}));
    };

    // 1. The plan's whole list with left and right swapped, as Order... sends
    // it: committed, published, and the plan runs right first.
    auto swapped = names;
    std::iter_swap(swapped.begin() + positionOf(names, "left"), swapped.begin() + positionOf(names, "right"));
    {
        const auto result = edit({{"main_render", swapped}});
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
    }
    CHECK(publishedGeneration(player) > generation);
    names = framePlanNames(player);
    CHECK(positionOf(names, "right") < positionOf(names, "left"));
    CHECK(positionOf(names, "left") < positionOf(names, "present"));
    CHECK(Json::parse(readFile(project.rootPath())).at("rendering").at("frame_node_order") ==
          Json{{"main_render", swapped}});
    requireBlankGradient(capture(player, capture_path));

    // 2. present before left, whose image it reads: refused by name, nothing
    // written, the plan as it was.
    const auto written = readFile(project.rootPath());
    {
        const auto result = edit({{"main_render", {"present", "left"}}});
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "rejected");
        const auto &error = result.at("error");
        CHECK(error.at("code") == "frame_node_order.conflicts_dependency@1");
        CHECK(error.at("graph") == "main_render");
        CHECK(error.at("first") == "present");
        CHECK(error.at("second") == "left");
        CHECK(error.at("reason") == "reads");
        CHECK(error.at("resource") == "left_color");
    }
    CHECK(readFile(project.rootPath()) == written);
    names = framePlanNames(player);
    CHECK(positionOf(names, "right") < positionOf(names, "left"));

    // 3. Started again on the written order: the same plan.
    player.close();
    CHECK(player.exit_code == 0u);
    PlayerRpc again{project.root, project.stderrLog("fo1_order_again")};
    REQUIRE(again.result("get_status").at("render_pipeline").at("state") == "ready");
    names = framePlanNames(again);
    CHECK(positionOf(names, "right") < positionOf(names, "left"));
    again.close();
    CHECK(again.exit_code == 0u);
}
#endif

#ifdef _WIN32
TEST_CASE("RC-3e1 A1 production RPC authoring recovery and publication", "[rc3e1][render-fallback][rpc]") {
    using Pelican::renderConfigSourceDigest;
    TempProject project{"rc3e1"};
    const auto good = fo1OrderRoot();
    const auto hash = [](const std::string &bytes) { return renderConfigSourceDigest(bytes); };
    const auto rootOf = [](PlayerRpc &player) {
        return player.result("get_render_documents").at("documents").at(0);
    };
    const auto restore = [&](PlayerRpc &player, const std::string &bytes, const std::string &digest) {
        return player.result("restore_render_documents", {{"documents", Json::array({
            {{"reference", root_reference}, {"base_digest", digest}, {"content", bytes}}})}});
    };
    const auto checkRefusal = [&](const Json &error, const char *code, const char *pipeline_stage, const std::string &bytes) {
        INFO(error.dump(2));
        CHECK(error.at("code") == code);
        CHECK(error.at("pipeline_stage") == pipeline_stage);
        CHECK(error.at("document_digest") == hash(bytes));
        if (error.contains("document")) CHECK(error.at("document").is_string());
    };
    // A1-1/2: normal publication, then pre-acceptance and actual apply refusal.
    writeFile(project.rootPath(), good);
    {
        PlayerRpc player{project.root, project.stderrLog("a1_publish")};
        REQUIRE(player.result("get_status").at("render_pipeline").at("state") == "ready");
        const auto generation = publishedGeneration(player);
        auto changed = Json::parse(good);
        changed["rendering"]["frame_node_order"] = {{"main_render", {"right", "left", "present"}}};
        const auto changed_bytes = changed.dump(2) + "\n";
        const auto accepted = restore(player, changed_bytes, rootOf(player).at("digest"));
        REQUIRE(accepted.at("status") == "accepted");
        REQUIRE(player.settle(accepted).at("status") == "committed");
        (void)player.result("step_frame");
        CHECK(publishedGeneration(player) > generation);
        CHECK(readFile(project.rootPath()) == changed_bytes);
        CHECK(rootOf(player).at("digest") == hash(changed_bytes));
        const auto published = publishedGeneration(player);
        const auto plan = player.result("get_frame_plan");
        auto compose_bad = changed;
        compose_bad["rendering"]["snapshots"] = Json::array({{{"name", "rc3e_snapshot"}, {"after", "rc3e_absent"}}});
        const auto compose_bytes = compose_bad.dump(2);
        const auto refused = restore(player, compose_bytes, hash(changed_bytes));
        REQUIRE(refused.at("status") == "rejected");
        CHECK_FALSE(refused.contains("ticket"));
        checkRefusal(refused.at("error"), "snapshot.after_unknown@1", "compose", compose_bytes);
        CHECK(refused.at("error").at("after") == "rc3e_absent");
        auto apply_bad = changed;
        apply_bad["rendering"]["frame_node_order"] = {{"main_render", {"present", "left"}}};
        const auto apply_bytes = apply_bad.dump(2);
        const auto queued = restore(player, apply_bytes, hash(changed_bytes));
        INFO(queued.dump(2));
        REQUIRE(queued.at("status") == "accepted");
        const auto applied = player.settle(queued);
        REQUIRE(applied.at("status") == "rejected");
        checkRefusal(applied.at("error"), "frame_node_order.conflicts_dependency@1", "runtime", apply_bytes);
        CHECK(applied.at("error").at("first") == "present");
        CHECK(applied.at("error").at("second") == "left");
        (void)player.result("step_frame");
        (void)player.result("step_frame");
        CHECK(publishedGeneration(player) == published);
        CHECK(player.result("get_frame_plan") == plan);
        CHECK(readFile(project.rootPath()) == changed_bytes);
        // Accepted, but the source CAS refuses before runtime apply. The
        // refusal identifies the candidate, rather than the held or disk root.
        const auto cas_candidate = good + "\n";
        const auto cas_queued = restore(player, cas_candidate, hash(changed_bytes));
        REQUIRE(cas_queued.at("status") == "accepted");
        const auto external_bytes = changed_bytes + " ";
        writeFile(project.rootPath(), external_bytes);
        const auto cas_refused = player.settle(cas_queued);
        REQUIRE(cas_refused.at("status") == "rejected");
        checkRefusal(cas_refused.at("error"), "external_modification", "runtime", cas_candidate);
        CHECK(cas_refused.at("error").at("expected_source_digest") == hash(changed_bytes));
        CHECK(cas_refused.at("error").at("actual_source_digest") == hash(external_bytes));
        (void)player.result("step_frame");
        CHECK(publishedGeneration(player) == published);
        CHECK(player.result("get_frame_plan") == plan);
        CHECK(readFile(project.rootPath()) == external_bytes);
        player.close();
        CHECK(player.exit_code == 0u);
    }
    // A1-3: preset-only references remain readable even when snapshots fail.
    auto composed_root = Json::parse(good);
    auto preset_config = composed_root.at("rendering");
    preset_config["features"] = Json::array({"engine://features/debug_draw.json"});
    const auto preset_path = project.root / "passes/rc3e-preset.json";
    writeFile(preset_path, Json{{"schema", "pelican.render_pipeline"}, {"version", 1},
        {"name", "rc3e"}, {"config", preset_config}}.dump(2));
    composed_root["rendering"] = {{"pipeline", {{"preset", "project://passes/rc3e-preset.json"}}},
        {"features", Json::array()},
        {"snapshots", Json::array({{{"name", "rc3e_snapshot"}, {"after", "rc3e_absent"}}})}};
    const auto compose_bytes = composed_root.dump(2) + "\n";
    writeFile(project.rootPath(), compose_bytes);
    {
        PlayerRpc player{project.root, project.stderrLog("a1_compose")};
        const auto status = player.result("get_status").at("render_pipeline");
        REQUIRE(status.at("state") == "fallback");
        checkRefusal(status.at("refusal"), "snapshot.after_unknown@1", "compose", compose_bytes);
        CHECK(status.at("refusal").at("after") == "rc3e_absent");
        const auto answer = player.result("get_render_documents");
        const auto &documents = answer.at("documents");
        const auto &other = answer.at("other_documents");
        REQUIRE(documents.size() == 2);
        CHECK(documents.at(1).at("reference") == "project://passes/rc3e-preset.json");
        CHECK(documents.at(0).at("content") == compose_bytes);
        CHECK(documents.at(0).at("reference") == root_reference);
        CHECK(documents.at(0).at("digest") == hash(compose_bytes));
        const auto feature = std::find_if(other.begin(), other.end(), [](const auto &row) {
            return row.at("reference") == "engine://features/debug_draw.json";
        });
        REQUIRE(feature != other.end());
        CHECK(feature->at("writable") == false);
        CHECK_FALSE(feature->at("content").get<std::string>().empty());
        // F1 errors are observations, not a failed whole getter. Deliberately
        // use disk bytes distinct from held bytes; put them back before restore.
        auto unreadable = composed_root;
        unreadable["rendering"]["features"] = Json::array({"missing-scheme://rc3e.json",
            "project://passes/rc3e-directory.json", "project://passes/rc3e-missing.json"});
        std::filesystem::create_directory(project.root / "passes/rc3e-directory.json");
        writeFile(project.rootPath(), unreadable.dump(2));
        const auto unreadable_documents = player.result("get_render_documents").at("other_documents");
        for (const auto *reference : {"missing-scheme://rc3e.json", "project://passes/rc3e-directory.json",
                                     "project://passes/rc3e-missing.json"}) {
            const auto row = std::find_if(unreadable_documents.begin(), unreadable_documents.end(), [&](const auto &entry) {
                return entry.at("reference") == reference;
            });
            REQUIRE(row != unreadable_documents.end());
            CHECK(row->at("writable") == false);
            CHECK(row->contains("error"));
        }
        writeFile(project.rootPath(), compose_bytes);
        const auto generation = publishedGeneration(player);
        REQUIRE(player.settle(restore(player, good, hash(compose_bytes))).at("status") == "committed");
        (void)player.result("step_frame");
        CHECK(publishedGeneration(player) > generation);
        CHECK(readFile(project.rootPath()) == good);
        player.close();
        CHECK(player.exit_code == 0u);
    }
    // A1-4: compose succeeds on current authored input, strict IR refuses it.
    auto ir = Json::parse(good);
    ir["rendering"]["render_targets"].push_back({{"name", "depth"}, {"extent_scale", 1},
        {"format", "D32_SFLOAT"}, {"usage", {"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"}}});
    ir["rendering"]["rendering_passes"][0]["passes"][0] = {
        {"name", "left"}, {"type", "material"}, {"material_contract", "forward_transparent_v1"},
        {"screen_inputs", {{"opaque_color", "left_color"}}},
        {"output", {{"color", "left_color"}, {"depth", "depth"}}}};
    const auto ir_bytes = ir.dump(2) + "\n";
    writeFile(project.rootPath(), ir_bytes);
    {
        PlayerRpc player{project.root, project.stderrLog("a1_ir")};
        const auto status = player.result("get_status").at("render_pipeline");
        REQUIRE(status.at("state") == "fallback");
        checkRefusal(status.at("refusal"), "render_graph.pass_shape@1", "parse", ir_bytes);
        CHECK(status.at("refusal").at("node") == "left");
        CHECK(rootOf(player).at("content") == ir_bytes);
        const auto context = player.result("get_render_authoring_context");
        CHECK(context.at("source_digest").at("hex") == hash(ir_bytes));
        bool found_left = false;
        for (const auto &graph : context.at("graphs")) for (const auto &pass : graph.at("passes")) {
            if (pass.at("name") != "left") continue;
            found_left = true;
            CHECK(pass.at("provenance").at("source") == "project");
            CHECK(pass.at("root_declaration").at("screen_inputs").at("opaque_color") == "left_color");
        }
        CHECK(found_left);
        REQUIRE(player.settle(restore(player, good, hash(ir_bytes))).at("status") == "committed");
        (void)player.result("step_frame");
        CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");
        player.close();
        CHECK(player.exit_code == 0u);
    }
    // A1-5/6: invalid envelopes at start and repeated reload, readable fallback,
    // structural gate and the two independent CAS comparisons H and D.
    const std::string schema_bytes = R"({"schema":"other"})";
    writeFile(project.rootPath(), schema_bytes);
    {
        PlayerRpc player{project.root, project.stderrLog("a1_schema")};
        checkRefusal(player.result("get_status").at("render_pipeline").at("refusal"),
                     "render_config.legacy_root@1", "decode", schema_bytes);
        CHECK(rootOf(player).at("content") == schema_bytes);
        CHECK(rootOf(player).at("digest") == hash(schema_bytes));
        const auto start_restore = restore(player, good, hash(schema_bytes));
        INFO(start_restore.dump(2));
        REQUIRE(start_restore.at("status") == "accepted");
        const auto start_result = player.settle(start_restore);
        INFO(start_result.dump(2));
        REQUIRE(start_result.at("status") == "committed");
        writeFile(project.rootPath(), schema_bytes);
        const auto refused = player.settle(player.result("reload_render_documents"));
        REQUIRE(refused.at("status") == "rejected");
        checkRefusal(refused.at("error"), "render_config.legacy_root@1", "decode", schema_bytes);
        CHECK(rootOf(player).at("content") == schema_bytes);
        const auto generation = publishedGeneration(player);
        const auto reload_restore = restore(player, good, hash(schema_bytes));
        INFO(reload_restore.dump(2));
        REQUIRE(reload_restore.at("status") == "accepted");
        const auto reload_result = player.settle(reload_restore);
        INFO(reload_result.dump(2));
        REQUIRE(reload_result.at("status") == "committed");
        (void)player.result("step_frame");
        CHECK(publishedGeneration(player) > generation);
        CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");
        CHECK(readFile(project.rootPath()) == good);
        player.close();
        CHECK(player.exit_code == 0u);
    }
    auto envelope = Json::parse(good);
    envelope["rendering"]["features"] = 17;
    const auto envelope_bytes = envelope.dump(2);
    writeFile(project.rootPath(), envelope_bytes);
    {
        PlayerRpc player{project.root, project.stderrLog("a1_envelope_start")};
        checkRefusal(player.result("get_status").at("render_pipeline").at("refusal"),
                     "render_document_envelope@1", "decode", envelope_bytes);
        CHECK(rootOf(player).at("content") == envelope_bytes);
        REQUIRE(player.settle(restore(player, good, hash(envelope_bytes))).at("status") == "committed");
        const auto readable = player.result("get_graph_instances");
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto bytes = envelope_bytes + std::string(attempt + 1, ' ');
            writeFile(project.rootPath(), bytes);
            const auto refusal = player.settle(player.result("reload_render_documents"));
            REQUIRE(refusal.at("status") == "rejected");
            checkRefusal(refusal.at("error"), "render_document_envelope@1", "decode", bytes);
            CHECK(rootOf(player).at("content") == bytes);
            const auto graph = player.result("get_graph_instances");
            CHECK(graph.at("held_unparsed") == true);
            CHECK(graph.at("held_reason") == "envelope");
            CHECK(graph.at("source_digest") == readable.at("source_digest"));
            const auto edit = player.result("edit_graph_document", {{"base_source_digest", hash(bytes)},
                {"ops", Json::array({{{"op", "set_node_label"}, {"node", "absent"}, {"label", "x"}}})}});
            checkRefusal(edit.at("error"), "render_document_envelope@1", "decode", bytes);
        }
        const auto held = rootOf(player).at("digest").get<std::string>();
        const auto disk = envelope_bytes + "   ";
        writeFile(project.rootPath(), disk);
        const auto reread = rootOf(player);
        CHECK(reread.at("held_digest") == held);
        CHECK(reread.at("digest") == hash(disk));
        for (const auto &digest : {held, hash(disk)}) {
            const auto refused = restore(player, good, digest);
            CHECK(refused.at("status") == "rejected");
            CHECK(refused.at("error").at("code") == "external_modification");
        }
        const auto reloaded = player.settle(player.result("reload_render_documents"));
        REQUIRE(reloaded.at("status") == "rejected");
        CHECK_FALSE(rootOf(player).contains("held_digest"));
        REQUIRE(player.settle(restore(player, good, hash(disk))).at("status") == "committed");
        (void)player.result("step_frame");
        CHECK(readFile(project.rootPath()) == good);
        // No features array remains accepted; a structural rendering edit works.
        const auto ordered = player.result("edit_frame_node_order", {{"base_source_digest", hash(good)},
            {"orders", {{"main_render", {"right", "left", "present"}}}}});
        REQUIRE(player.settle(ordered).at("status") == "committed");
        auto sectionless = Json::parse(good);
        sectionless.erase("rendering");
        const auto sectionless_bytes = sectionless.dump(2);
        writeFile(project.rootPath(), sectionless_bytes);
        (void)player.settle(player.result("reload_render_documents"));
        const auto root_edit = player.result("edit_graph_document", {
            {"base_source_digest", hash(sectionless_bytes)},
            {"ops", Json::array({{{"op", "set_layout"}, {"scope", "root"},
                                  {"layout", {{"nodes", Json::object()}}}}})}});
        REQUIRE(player.settle(root_edit).at("status") == "committed");
        const auto section_edit = player.result("edit_frame_node_order", {
            {"base_source_digest", rootOf(player).at("digest")}, {"orders", Json::object()}});
        CHECK(section_edit.at("status") == "rejected");
        CHECK(section_edit.at("error").at("code") == "render_config_section_unlocated");
        player.close();
        CHECK(player.exit_code == 0u);
    }
}
#endif

#ifdef _WIN32
namespace {
Json rc5bPasses(PlayerRpc &player) {
    const auto context = player.result("get_render_authoring_context");
    Json passes = Json::object();
    for (const auto &graph : context.at("graphs"))
        for (const auto &pass : graph.at("passes")) passes[pass.at("name").get<std::string>()] = pass.at("declaration");
    return passes;
}
Json rc5bLighting(PlayerRpc &player) {
    const auto context = player.result("get_render_authoring_context");
    for (const auto &g : context.at("graphs"))
        for (const auto &p : g.at("passes")) if (p.at("name") == "deferred_lighting") return p;
    FAIL("deferred_lighting is missing");
    return {};
}
}
TEST_CASE("RC5b B1 B2 production player materializes, edits, restores and follows fragments", "[rc5b][rpc][render-fallback]") {
    using Pelican::renderConfigSourceDigest;
    TempProject project{"rc5b"};
    auto root = Json::parse(project.fixed_root);
    root["nodes"] = Json::array();
    root["rendering"] = {{"pipeline", {{"preset", "engine://render_pipelines/experimental/hybrid_recipe.json"}}}};
    const auto before = root.dump(2);
    writeFile(project.rootPath(), before);
    const std::string destination = "project://passes/lighting-fragment.json";
    const auto path = project.root / "passes/lighting-fragment.json";
    std::string branched_root, branched_fragment;
    Json expected_passes;
    {
        PlayerRpc player{project.root, project.stderrLog("rc5b_materialize")};
        REQUIRE(player.result("get_status").at("render_pipeline").at("state") == "ready");
        const auto context = player.result("get_render_authoring_context");
        const auto rows = player.result("get_render_features").at("base_fragments");
        REQUIRE(rows.size() == 5);
        for (const auto &row : rows) {
            CHECK(row.at("state") == "inherited"); CHECK(row.at("materializable") == true);
            CHECK(row.at("revertible") == false);
        }
        const auto all_before = rc5bPasses(player);
        const auto engine_docs = player.result("get_render_documents").at("other_documents");
        std::string source;
        for (const auto &doc : engine_docs)
            if (doc.at("reference") == "engine://render_fragments/deferred_lighting.json") source = doc.at("content");
        REQUIRE_FALSE(source.empty());
        Json request{{"name", "deferred_lighting"}, {"base_source_digest", context.at("source_digest").at("hex")},
            {"base_preset_digest", context.at("pipeline_preset").at("source_digest").at("hex")},
            {"base_fragment_digest", rows.at(2).at("digest").at("hex")}, {"destination_reference", destination}};
        const auto refused = [&](Json params, const char *code) {
            const auto answer = player.result("materialize_render_fragment", params);
            INFO(answer.dump()); CHECK(answer.at("error").at("code") == code);
            CHECK(readFile(project.rootPath()) == before);
            CHECK_FALSE(std::filesystem::exists(path));
        };
        auto bad = request; bad["base_fragment_digest"] = std::string(64, '0');
        refused(bad, "render_fragment_source_changed");
        bad = request; bad["base_preset_digest"] = std::string(64, '0'); refused(bad, "external_modification");
        bad = request; bad["base_source_digest"] = std::string(64, '0'); refused(bad, "external_modification");
        writeFile(path, "occupied");
        CHECK(player.result("materialize_render_fragment", request).at("error").at("code") == "fragment_destination_exists");
        CHECK(readFile(path) == "occupied"); CHECK(readFile(project.rootPath()) == before);
        std::filesystem::remove(path);
        const auto engine_pass = rc5bLighting(player);
        auto edit = Json{{"base_source_digest", context.at("source_digest").at("hex")},
            {"base_fragment_digest", rows.at(2).at("digest").at("hex")},
            {"graph", "main_render"}, {"pass_name", "deferred_lighting"},
            {"input", engine_pass.at("fragment_declaration").at("input")},
            {"output", engine_pass.at("fragment_declaration").at("output")},
            {"shader", engine_pass.at("fragment_declaration").at("shader")}, {"push_constants", "none"}};
        CHECK(player.result("edit_preset_pass", edit).at("error").at("code") == "render_fragment_not_materialized");
        bad = edit; bad["base_preset_digest"] = request.at("base_preset_digest");
        const auto syntax = player.call("edit_preset_pass", bad);
        CHECK(syntax.at("error").at("code") == -32602);
        REQUIRE(player.settle(player.result("materialize_render_fragment", request)).at("committed") == true);
        CHECK(readFile(path) == source);
        auto after = readFile(project.rootPath());
        CHECK(Json::parse(after).at("rendering").at("fragment_overrides").size() == 1);
        CHECK(rc5bPasses(player) == all_before);
        const auto restore = [&](Json documents) {
            const auto answer = player.settle(player.result("restore_render_documents", {{"documents", documents}}));
            INFO(answer.dump()); REQUIRE(answer.at("committed") == true);
        };
        const auto doc = [](const std::string &ref, Json digest, Json content) { return Json{{"reference", ref}, {"base_digest", digest}, {"content", content}}; };
        restore(Json::array({doc(root_reference, renderConfigSourceDigest(after), before), doc(destination, renderConfigSourceDigest(source), nullptr)}));
        CHECK_FALSE(std::filesystem::exists(path)); CHECK(readFile(project.rootPath()) == before);
        restore(Json::array({doc(root_reference, renderConfigSourceDigest(before), after), doc(destination, nullptr, source)}));
        CHECK(readFile(path) == source); CHECK(readFile(project.rootPath()) == after);
        request["base_source_digest"] = player.result("get_render_features").at("source_digest").at("hex");
        CHECK(player.result("materialize_render_fragment", request).at("error").at("code") == "render_fragment_already_materialized");
        CHECK(readFile(path) == source); CHECK(readFile(project.rootPath()) == after);
        edit["base_source_digest"] = request.at("base_source_digest");
        edit["base_fragment_digest"] = rc5bLighting(player).at("fragment").at("digest").at("hex");
        bad = edit; bad["base_fragment_digest"] = std::string(64, '0');
        CHECK(player.result("edit_preset_pass", bad).at("error").at("code") == "external_modification");
        REQUIRE(player.settle(player.result("edit_preset_pass", edit)).at("committed") == true);
        branched_fragment = readFile(path);
        auto expected = all_before; expected["deferred_lighting"]["push_constants"] = "none";
        CHECK(rc5bPasses(player) == expected);
        restore(Json::array({doc(destination, renderConfigSourceDigest(branched_fragment), source)})); CHECK(readFile(path) == source);
        restore(Json::array({doc(destination, renderConfigSourceDigest(source), branched_fragment)})); CHECK(readFile(path) == branched_fragment);
        REQUIRE(player.settle(player.result("edit_rendering_keys", {{"base_source_digest", renderConfigSourceDigest(after)},
            {"remove", Json::array({"fragment_overrides"})}})).at("committed") == true);
        const auto inherited = readFile(project.rootPath());
        CHECK(rc5bPasses(player) == all_before); CHECK(readFile(path) == branched_fragment);
        restore(Json::array({doc(root_reference, renderConfigSourceDigest(inherited), after)})); CHECK(rc5bPasses(player) == expected);
        restore(Json::array({doc(root_reference, renderConfigSourceDigest(after), inherited)})); CHECK(rc5bPasses(player) == all_before);
        // Save in the branched state and reopen the real player.
        restore(Json::array({doc(root_reference, renderConfigSourceDigest(inherited), after)}));
        branched_root = after; expected_passes = expected;
        (void)player.result("load_scene", {{"name", "default_scene"}});
        (void)player.result("save_scene");
        player.close(); CHECK(player.exit_code == 0u);
    }
    {
        PlayerRpc player{project.root, project.stderrLog("rc5b_reopen")};
        REQUIRE(player.result("get_status").at("render_pipeline").at("state") == "ready");
        CHECK(rc5bPasses(player) == expected_passes);
        CHECK(player.result("get_render_features").at("base_fragments").at(2).at("state") == "overridden");
        CHECK(readFile(project.rootPath()) == branched_root); CHECK(readFile(path) == branched_fragment);
        // Project source documents provide a reloadable upstream. No engine
        // replacement endpoint is added to the player.
        const auto resources = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "src/core/resources";
        auto preset = Json::parse(readFile(resources / "render_pipelines/experimental/hybrid_recipe.json"));
        auto &refs = preset["config"]["base"][0]["fragments"];
        for (auto &ref : refs) {
            const auto original = ref.get<std::string>();
            if (original.ends_with("/ssao.json") || original.ends_with("/deferred_lighting.json")) {
                const auto filename = std::filesystem::path{original}.filename().string();
                writeFile(project.root / "passes" / ("upstream-" + filename), readFile(resources / original.substr(9)));
                ref = "project://passes/upstream-" + filename;
            }
        }
        writeFile(project.root / "passes/upstream-recipe.json", preset.dump(2));
        auto local = Json::parse(branched_root); local["rendering"]["pipeline"]["preset"] = "project://passes/upstream-recipe.json";
        writeFile(project.rootPath(), local.dump(2));
        REQUIRE(player.settle(player.result("reload_render_documents")).at("committed") == true);
        auto sourceLighting = Json::parse(readFile(project.root / "passes/upstream-deferred_lighting.json"));
        sourceLighting["passes"][0]["clear_colors"] = {{"lit_color", Json::array({0.2,0.3,0.4,1.0})}};
        writeFile(project.root / "passes/upstream-deferred_lighting.json", sourceLighting.dump(2));
        auto ssao = Json::parse(readFile(project.root / "passes/upstream-ssao.json"));
        ssao["passes"][0]["shader"]["defines"] = Json::array({"RC5B_FOLLOW=1"});
        writeFile(project.root / "passes/upstream-ssao.json", ssao.dump(2));
        REQUIRE(player.settle(player.result("reload_render_documents")).at("committed") == true);
        auto following = rc5bPasses(player);
        CHECK(following.at("deferred_lighting") == expected_passes.at("deferred_lighting"));
        CHECK(following.at("ssao_pass").at("shader").at("defines") == Json::array({"RC5B_FOLLOW=1"}));
        expected_passes["ssao_pass"] = following.at("ssao_pass"); CHECK(following == expected_passes);
        const auto rows = player.result("get_render_features").at("base_fragments");
        CHECK(rows.at(1).at("state") == "inherited"); CHECK(rows.at(1).at("materializable") == false);
        CHECK(rows.at(2).at("state") == "overridden");
        REQUIRE(player.settle(player.result("edit_rendering_keys", {{"base_source_digest", renderConfigSourceDigest(readFile(project.rootPath()))},
            {"remove", Json::array({"fragment_overrides"})}})).at("committed") == true);
        CHECK(rc5bLighting(player).at("editable_fragment_pass") == true);
        CHECK(rc5bLighting(player).at("fragment_declaration") == sourceLighting.at("passes").at(0));
        CHECK(readFile(path) == branched_fragment);
        player.close(); CHECK(player.exit_code == 0u);
    }
}
#endif
