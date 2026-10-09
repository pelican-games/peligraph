// PB-1: the engine keeps the problems that are true now and returns them:
// get_status.problems {revision, count, errors, warnings} and get_problems
// {revision, problems: [...]}. Driven through the production path: a headless
// `pelican_player --rpc --render-fallback` child, line by line over its
// stdin/stdout.

#include "../src/project/projecttemplate.hpp"
#include "skeletal_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

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
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << text;
}

constexpr const char *root_reference = "passes/main_rendering_config.json";
const std::string root_document = std::string{"project://"} + root_reference;
constexpr const char *feature_reference = "features/pb1_text.json";
const std::string feature_document = std::string{"project://"} + feature_reference;

// CT-1's case: a compute task's buffer port written by hand without the
// element it holds (graph.port_field_missing@1 {node, port, field}).
Json brokenNode() {
    return {{"id", "sim"},
            {"op", "render.compute"},
            {"parameters",
             {{"ports", {{"data", {{"kind", "buffer"}, {"access", "read_write"}}}}},
              {"shader", "shaders/sim.comp"}}},
            {"outputs", Json::array()}};
}

std::string withNode(const std::string &root) {
    auto document = Json::parse(root);
    document.at("nodes").push_back(brokenNode());
    return document.dump(2) + "\n";
}

// A hand edit that lost the closing brace: not JSON.
std::string withoutLastBrace(std::string text) {
    text.erase(text.rfind('}'), 1);
    return text;
}

// A project feature document (the engine's debug_text pass, under the
// project's own name) the root enables, so a feature document can be broken.
const Json feature_json{
    {"schema", "pelican.render_feature"},
    {"version", 1},
    {"name", "debug_text"},
    {"passes",
     Json::array({{{"insert", "after:debug_text"},
                   {"pass",
                    {{"name", "debug_text"},
                     {"type", "debug_text"},
                     {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
                     {"color_load_op", "load"},
                     {"shader", {{"vertex", "engine://debug_text"}, {"fragment", "engine://debug_text"}}}}}}})},
};

struct TempProject {
    std::filesystem::path root;
    std::string fixed_root;

    explicit TempProject(std::string_view name) {
        root = std::filesystem::temp_directory_path() /
               ("pelican_pb1_" + std::string{name} + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        (void)Pelican::initializeProjectTemplate(root, Pelican::ProjectTemplate::Blank);
        fixed_root = readFile(rootPath());
    }
    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::filesystem::path rootPath() const { return root / root_reference; }
    std::filesystem::path featurePath() const { return root / feature_reference; }
    std::filesystem::path stderrLog(std::string_view run) const {
        return root / ("pb1." + std::string{run} + ".stderr.log");
    }
    // The root enables the project feature.
    void addFeature() {
        writeFile(featurePath(), feature_json.dump(2) + "\n");
        auto document = Json::parse(fixed_root);
        document.at("rendering")["features"] = Json::array({std::string{"project://"} + feature_reference});
        fixed_root = document.dump(2) + "\n";
        writeFile(rootPath(), fixed_root);
    }
};

#ifdef _WIN32
// One --rpc player. Requests are written one line at a time and each answer
// is read before the next request goes out.
class PlayerRpc {
    std::filesystem::path stderr_log_;
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
              const std::wstring &flags) {
        stderr_log_ = stderr_log;
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
        const std::filesystem::path player{PELICAN_PB1_PLAYER};
        auto command = L"\"" + player.wstring() + L"\" " + flags + L" --size 160x90 --project \"" +
                       project.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdin_read;
        startup.hStdOutput = stdout_write;
        startup.hStdError = error_log;
        const auto working = player.parent_path().wstring();
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
                auto response = Json::parse(lines_.front(), nullptr, false);
                lines_.pop_front();
                if (response.is_object() && response.value("id", -1) == id) return response;
            }
            INFO("waiting for the answer to " << method);
            INFO(readFile(stderr_log_));
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

const std::wstring headless_flags = L"--rpc --headless --render-fallback";

// The one problem of a list, with the summary get_status gives of it.
Json onlyProblem(PlayerRpc &player) {
    const auto list = player.result("get_problems");
    INFO(list.dump(2));
    REQUIRE(list.at("problems").size() == 1u);
    const auto summary = player.result("get_status").at("problems");
    INFO(summary.dump());
    CHECK(summary.at("count") == 1);
    CHECK(summary.at("errors") == 1);
    CHECK(summary.at("warnings") == 0);
    CHECK(summary.at("revision") == list.at("revision"));
    auto problem = list.at("problems").at(0);
    CHECK(problem.at("severity") == "error");
    CHECK(problem.at("id").get<std::string>().size() == 16u);
    CHECK_FALSE(problem.at("message").get<std::string>().empty());
    return problem;
}

std::uint64_t revisionOf(PlayerRpc &player) {
    return player.result("get_status").at("problems").at("revision").get<std::uint64_t>();
}

void requireNoProblems(PlayerRpc &player) {
    const auto list = player.result("get_problems");
    INFO(list.dump(2));
    CHECK(list.at("problems").empty());
    const auto summary = player.result("get_status").at("problems");
    CHECK(summary.at("count") == 0);
    CHECK(summary.at("errors") == 0);
}

// The document that is not JSON: where the parser stopped, and no node (the
// engine does not guess one).
void requireSyntaxProblem(const Json &problem, const std::string &document, std::string_view kind) {
    INFO(problem.dump(2));
    CHECK(problem.at("code") == "render_document_syntax@1");
    CHECK(problem.at("document") == document);
    CHECK(problem.at("document_kind") == std::string{kind});
    CHECK(problem.at("line").get<int>() > 1);
    CHECK(problem.at("column").get<int>() >= 1);
    CHECK_FALSE(problem.contains("node"));
    CHECK(problem.at("fields").at("document") == document);
    CHECK(problem.at("fields").contains("reason"));
    CHECK_THAT(problem.at("message").get<std::string>(), Catch::Matchers::ContainsSubstring("is not valid JSON"));
}

// CT-1's node in the root's graph: node, port and the missing field.
void requireNodeProblem(const Json &problem) {
    INFO(problem.dump(2));
    CHECK(problem.at("code") == "graph.port_field_missing@1");
    CHECK(problem.at("document") == root_document);
    CHECK(problem.at("document_kind") == "root_graph");
    CHECK(problem.at("node") == "sim");
    CHECK(problem.at("port") == "data");
    CHECK(problem.at("fields").at("field") == "element");
    CHECK_FALSE(problem.contains("line"));
}
#endif

} // namespace

#ifdef _WIN32
TEST_CASE("PB-1 a start refused for a root that is not JSON lists one startup problem; a reload of the file as "
          "it is lists it again by the same id, and a reload of the fixed file empties the list",
          "[pb1][problems][render-fallback][rpc]") {
    TempProject project{"syntax"};
    writeFile(project.rootPath(), withoutLastBrace(project.fixed_root));
    PlayerRpc player{project.root, project.stderrLog("syntax"), headless_flags};

    // 1. The refused start: one problem, from the start.
    REQUIRE(player.result("get_status").at("render_pipeline").at("state") == "fallback");
    const auto started = onlyProblem(player);
    CHECK(started.at("source") == "startup");
    requireSyntaxProblem(started, root_document, "root");
    // Met before the first frame: no frame to name.
    CHECK_FALSE(started.contains("at_frame"));
    const auto first_revision = revisionOf(player);
    CHECK(first_revision >= 1u);

    // 2. Reloaded as it still is: the same problem, now from the reload, by
    // the same id.
    {
        const auto result = player.settle(player.result("reload_render_documents"));
        REQUIRE(result.at("status") == "rejected");
    }
    const auto reloaded = onlyProblem(player);
    CHECK(reloaded.at("source") == "reload_rpc");
    requireSyntaxProblem(reloaded, root_document, "root");
    CHECK(reloaded.at("id") == started.at("id"));
    CHECK(reloaded.contains("at_frame"));
    const auto second_revision = revisionOf(player);
    CHECK(second_revision > first_revision);

    // 3. Fixed on disk and reloaded: committed, nothing listed, the revision
    // moved.
    writeFile(project.rootPath(), project.fixed_root);
    {
        const auto result = player.settle(player.result("reload_render_documents"));
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
    }
    requireNoProblems(player);
    CHECK(revisionOf(player) > second_revision);
    CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");

    // An edit's refusal is the answer's alone: the list stays empty.
    {
        const auto document = player.result("get_render_documents").at("documents").at(0);
        const auto result = player.settle(player.result(
            "edit_graph_document", {{"base_source_digest", document.at("digest")},
                                    {"ops", Json::array({{{"op", "remove_node"}, {"node", "nothing"}}})}}));
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "rejected");
    }
    requireNoProblems(player);
    player.close();
    CHECK(player.exit_code == 0u);
}

TEST_CASE("PB-1 a start refused for a node lists its node, port and document, and the problem is the root graph's",
          "[pb1][problems][render-fallback][rpc]") {
    TempProject project{"node"};
    writeFile(project.rootPath(), withNode(project.fixed_root));
    PlayerRpc player{project.root, project.stderrLog("node"), headless_flags};
    const auto problem = onlyProblem(player);
    CHECK(problem.at("source") == "startup");
    requireNodeProblem(problem);

    writeFile(project.rootPath(), project.fixed_root);
    REQUIRE(player.settle(player.result("reload_render_documents")).at("status") == "committed");
    requireNoProblems(player);
    player.close();
    CHECK(player.exit_code == 0u);
}

TEST_CASE("PB-1 a running engine whose feature document is broken on disk lists the reload's refusal as the "
          "feature's while the last generation goes on, and a reload of the fixed file takes it away",
          "[pb1][problems][rpc]") {
    // Under an RPC driver the watch leaves render documents to the client's
    // reload_render_documents (HW-1), so this is how a broken save reaches a
    // running engine.
    TempProject project{"feature"};
    project.addFeature();
    PlayerRpc player{project.root, project.stderrLog("feature"), headless_flags};
    {
        const auto status = player.result("get_status");
        INFO(status.at("render_pipeline").dump() << " " << status.at("problems").dump());
        REQUIRE(status.at("render_pipeline").at("state") == "ready");
        REQUIRE(status.at("problems").at("count") == 0);
    }

    // The feature document loses its last brace.
    writeFile(project.featurePath(), withoutLastBrace(feature_json.dump(2) + "\n"));
    REQUIRE(player.settle(player.result("reload_render_documents")).at("status") == "rejected");
    const auto problem = onlyProblem(player);
    CHECK(problem.at("source") == "reload_rpc");
    requireSyntaxProblem(problem, feature_document, "feature");
    // The generation before it goes on.
    CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");

    writeFile(project.featurePath(), feature_json.dump(2) + "\n");
    REQUIRE(player.settle(player.result("reload_render_documents")).at("status") == "committed");
    requireNoProblems(player);
    player.close();
    CHECK(player.exit_code == 0u);
}

TEST_CASE("PB-1 a fixed root that enables a feature whose module the engine started without is refused as "
          "render_feature_requires_restart@1 with the feature and the module, and is listed by that name",
          "[pb1][problems][render-fallback][rpc]") {
    TempProject project{"restart"};
    writeFile(project.rootPath(), withoutLastBrace(project.fixed_root));
    PlayerRpc player{project.root, project.stderrLog("restart"), headless_flags};
    REQUIRE(player.result("get_status").at("render_pipeline").at("state") == "fallback");

    // Fixed, and now enabling debug_draw: the fallback started without its
    // DebugDraw module, and the module graph is frozen.
    auto fixed = Json::parse(project.fixed_root);
    fixed.at("rendering")["features"] = Json::array({"engine://features/debug_draw.json"});
    writeFile(project.rootPath(), fixed.dump(2) + "\n");
    const auto result = player.settle(player.result("reload_render_documents"));
    INFO(result.dump(2));
    REQUIRE(result.at("status") == "rejected");
    const auto &error = result.at("error");
    CHECK(error.at("code") == "render_feature_requires_restart@1");
    CHECK(error.at("feature") == "debug_draw");
    CHECK(error.at("module") == "DebugDraw");
    CHECK_THAT(error.at("message").get<std::string>(),
               Catch::Matchers::ContainsSubstring("feature 'debug_draw' needs runtime module DebugDraw") &&
                   Catch::Matchers::ContainsSubstring("restart the engine"));

    const auto problem = onlyProblem(player);
    INFO(problem.dump(2));
    CHECK(problem.at("source") == "reload_rpc");
    CHECK(problem.at("code") == "render_feature_requires_restart@1");
    CHECK(problem.at("fields").at("feature") == "debug_draw");
    CHECK(problem.at("fields").at("module") == "DebugDraw");
    player.close();
    CHECK(player.exit_code == 0u);
}
#endif

#ifdef _WIN32
namespace {

// G-4's forward-only direct root without material_route_default: the
// engine's built-in materials take the deferred route, which no pass serves.
std::string forwardOnlyRoot() {
    const Json rendering{
        {"render_targets",
         Json::array({{{"name", "scene_color"},
                       {"extent_scale", 1.0},
                       {"format", "R16G16B16A16_SFLOAT"},
                       {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
                       {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
                      {{"name", "scene_depth"},
                       {"extent_scale", 1.0},
                       {"format", "D32_SFLOAT"},
                       {"format_class", "data"},
                       {"usage", {"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"}}}})},
        {"rendering_passes",
         Json::array({{{"name", "main_render"},
                       {"passes",
                        Json::array({{{"name", "forward"},
                                      {"type", "material"},
                                      {"material_contract", "forward_opaque_v1"},
                                      {"clear_color", {1.0, 1.0, 1.0, 1.0}},
                                      {"output", {{"color", {"scene_color"}}, {"depth", "scene_depth"}}}},
                                     {{"name", "present"},
                                      {"type", "fullscreen"},
                                      {"input", {"scene_color"}},
                                      {"output", {{"color", {"display"}}, {"depth", nullptr}}},
                                      {"shader",
                                       {{"vertex", "engine://fullscreen"},
                                        {"fragment", "project://shaders/present"}}}}})}}})},
    };
    return Json{{"schema", "pelican.graph"}, {"version", 1},        {"definitions", Json::object()},
                {"nodes", Json::array()},    {"outputs", Json::object()}, {"rendering", rendering}}
               .dump(2) +
           "\n";
}

constexpr const char *present_shader =
    "#version 460\n#extension GL_GOOGLE_include_directive : require\n"
    "#include \"pelican_sets.glsl\"\n#include \"pelican_view.glsl\"\n\n"
    "layout(location = 0) in vec2 inUV;\nlayout(location = 0) out vec4 outColor;\n"
    "PELICAN_DECLARE_INPUT_0(in_scene_color);\n\n"
    "void main() {\n    outColor = PELICAN_TEXTURE_2D_0(in_scene_color, inUV);\n}\n";

} // namespace

TEST_CASE("PB-1 a start whose built-in materials find no pass for their route runs the fallback generation with "
          "material_route_unserved@1 as a startup problem, and a reload of a fixed root brings the project back",
          "[pb1][problems][render-fallback][rpc]") {
    TempProject project{"unserved"};
    writeFile(project.root / "shaders" / "present.frag", present_shader);
    writeFile(project.rootPath(), forwardOnlyRoot());
    PlayerRpc player{project.root, project.stderrLog("unserved"), headless_flags};

    // 1. Up, on the fallback, refused by the route's name.
    const auto status = player.result("get_status").at("render_pipeline");
    INFO(status.dump(2));
    REQUIRE(status.at("state") == "fallback");
    const auto &refusal = status.at("refusal");
    CHECK(refusal.at("code") == "material_route_unserved@1");
    CHECK(refusal.at("rendering_config") == root_reference);
    REQUIRE_FALSE(refusal.at("unserved").empty());
    CHECK(refusal.at("unserved").at(0).at("route") == "deferred_geometry");
    CHECK_THAT(refusal.at("message").get<std::string>(), Catch::Matchers::ContainsSubstring("Hint:"));
    {
        const auto before = player.result("get_status").at("frame").get<std::uint64_t>();
        (void)player.result("step_frame");
        (void)player.result("step_frame");
        CHECK(player.result("get_status").at("frame").get<std::uint64_t>() > before);
    }

    // 2. Listed from the start, as the root's rendering section's problem.
    const auto problem = onlyProblem(player);
    INFO(problem.dump(2));
    CHECK(problem.at("source") == "startup");
    CHECK(problem.at("code") == "material_route_unserved@1");
    CHECK(problem.at("document") == root_document);
    CHECK(problem.at("document_kind") == "root_rendering");
    CHECK(problem.at("fields").at("unserved") == refusal.at("unserved"));

    // 3. The root fixed on disk (Blank's own, which draws no material) and
    // reloaded: the project's frames, nothing listed.
    writeFile(project.rootPath(), project.fixed_root);
    {
        const auto result = player.settle(player.result("reload_render_documents"));
        INFO(result.dump(2));
        REQUIRE(result.at("status") == "committed");
    }
    CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");
    requireNoProblems(player);
    player.close();
    CHECK(player.exit_code == 0u);
}
#endif

#ifdef _WIN32
namespace {

constexpr const char *tint_reference = "shaders/tint.frag";
const std::string tint_document = std::string{"project://"} + tint_reference;
// Line 5 is the one the broken copy breaks.
constexpr const char *tint_shader = "#version 460\n"
                                    "layout(location = 0) in vec2 inUV;\n"
                                    "layout(location = 0) out vec4 outColor;\n"
                                    "void main() {\n"
                                    "    outColor = vec4(inUV, 0.5, 1.0);\n"
                                    "}\n";
constexpr const char *broken_tint_shader = "#version 460\n"
                                           "layout(location = 0) in vec2 inUV;\n"
                                           "layout(location = 0) out vec4 outColor;\n"
                                           "void main() {\n"
                                           "    outColor = vec4(inUV, 0.5, 1.0) + ;\n"
                                           "}\n";

// Blank's present pass, drawn with the project's own shader.
std::string tintRoot(const std::string &blank_root) {
    auto document = Json::parse(blank_root);
    auto &pass = document.at("rendering").at("rendering_passes").at(0).at("passes").at(0);
    pass.at("shader").at("fragment") = "project://shaders/tint";
    return document.dump(2) + "\n";
}

void requireShaderProblem(const Json &problem, std::string_view source) {
    INFO(problem.dump(2));
    CHECK(problem.at("source") == std::string{source});
    CHECK(problem.at("document") == tint_document);
    CHECK(problem.at("document_kind") == "shader");
    CHECK(problem.at("line") == 5);
}

} // namespace

TEST_CASE("PB-1 a project shader that does not compile is listed as the shader's problem with its line: at start "
          "under the fallback, and on reload_shaders while the engine runs; fixed, it leaves the list",
          "[pb1][problems][shader][rpc]") {
    TempProject project{"shader"};
    writeFile(project.root / tint_reference, broken_tint_shader);
    project.fixed_root = tintRoot(project.fixed_root);
    writeFile(project.rootPath(), project.fixed_root);
    PlayerRpc player{project.root, project.stderrLog("shader"), headless_flags};

    // 1. At start: the rendering configuration is refused for the shader;
    // the problem is the shader file's, at the line the compiler named.
    REQUIRE(player.result("get_status").at("render_pipeline").at("state") == "fallback");
    {
        const auto problem = onlyProblem(player);
        requireShaderProblem(problem, "startup");
        CHECK_THAT(problem.at("message").get<std::string>(),
                   Catch::Matchers::ContainsSubstring("Shader compile failed"));
    }
    writeFile(project.root / tint_reference, tint_shader);
    REQUIRE(player.settle(player.result("reload_render_documents")).at("status") == "committed");
    REQUIRE(player.result("get_status").at("render_pipeline").at("state") == "ready");
    requireNoProblems(player);

    // 2. Running: the shader broken on disk and reload_shaders asked for.
    writeFile(project.root / tint_reference, broken_tint_shader);
    {
        const auto answer = player.call("reload_shaders");
        INFO(answer.dump(2));
        REQUIRE(answer.contains("error"));
        CHECK(answer.at("error").at("data").at("error_code") == "shader_reload_failed@1");
    }
    {
        const auto problem = onlyProblem(player);
        requireShaderProblem(problem, "reload_rpc");
        CHECK(problem.at("code") == "shader_reload_failed@1");
        // The compiler's first error line, the whole log beside it.
        CHECK_THAT(problem.at("message").get<std::string>(),
                   Catch::Matchers::ContainsSubstring("tint.frag:5: error:"));
        CHECK_THAT(problem.at("fields").at("log").get<std::string>(),
                   Catch::Matchers::ContainsSubstring("syntax error"));
        CHECK(problem.contains("at_frame"));
    }
    // The last generation goes on.
    CHECK(player.result("get_status").at("render_pipeline").at("state") == "ready");

    // 3. Fixed and reloaded: nothing listed.
    writeFile(project.root / tint_reference, tint_shader);
    {
        const auto answer = player.result("reload_shaders");
        INFO(answer.dump(2));
        CHECK(answer.at("committed") == true);
    }
    requireNoProblems(player);
    player.close();
    CHECK(player.exit_code == 0u);
}
#endif

#ifdef _WIN32
TEST_CASE("AN-2 animation component RPC admission and invalid scene Problems recover without stopping frames",
          "[an2][animation][pb1][problems][rpc][gpu]") {
    TempProject project{"animation"};
    Pelican::TestSkeletalFixture::writeGlb(project.root / "assets/models/character.glb");
    Pelican::TestSkeletalFixture::writeGlb(project.root / "assets/models/other.glb");
    writeFile(project.root / "assets/asset_data.json",
              R"({"schema":"pelican.asset_data","version":2,"models":[{"name":"character","path":"assets/models/character.glb"}]})");
    const std::string clip = "assets/models/character.glb#animation/Turn";
    const auto scene_path = project.root / "scenes/main.scene.json";
    auto scene = Json::parse(readFile(scene_path));
    auto object = [&](const char *name, const char *model, bool invalid) {
        Json components = Json::array({{{"name", "transform"}},
                                      {{"name", "simplemodelview"}, {"model", model}}});
        if (invalid) components.push_back({{"name", "animation"}, {"clip", clip}});
        return Json{{"name", name}, {"components", components}};
    };
    auto &objects = scene["scenes"]["default_scene"]["objects"];
    objects.push_back(object("Cube", "engine://primitives/cube", true));
    objects.push_back(object("Character", "character", false));
    writeFile(scene_path, scene.dump(2));
    const auto disk_before = readFile(scene_path);
    // The finite non-RPC loop lets the startup mutation reach the engine's
    // top-level catch, instead of being translated into a step_frame RPC error.
    {
        const auto log = project.stderrLog("animation-finite");
        PlayerRpc startup{project.root, log, L"--headless --frames 3"};
        startup.close();
        INFO(readFile(log));
        REQUIRE(startup.exit_code == 0u);
    }
    PlayerRpc player{project.root, project.stderrLog("animation"), headless_flags + L" --start-state edit"};
    (void)player.result("step_frame");
    auto id = [&](const char *name) {
        const auto tree = player.result("scene_tree");
        for (const auto &entry : tree.at("objects"))
            if (entry.value("name", std::string{}) == name)
                return entry.at("authoring_object_id").get<std::uint64_t>();
        FAIL("missing scene object " << name);
        return std::uint64_t{};
    };
    const auto cube = id("Cube"), character = id("Character");
    const auto actor = player.result("open_editor_session", {{"display_name", "AN-2"}}).at("actor_id");
    auto edit = [&](Json operation) {
        return player.settle(player.result("edit", {
            {"actor_id", actor},
            {"base_revision", player.result("get_scene_revision").at("scene_revision")},
            {"operations", Json::array({operation})}}));
    };
    auto add = [&](std::uint64_t target, const std::string &reference) {
        return Json{{"op", "add_component"}, {"object_id", target},
                    {"component", {{"name", "animation"}, {"clip", reference}}}};
    };
    auto set = [&](std::uint64_t target, const char *slot, const char *field, const std::string &value) {
        return Json{{"op", "set_component_value"}, {"object_id", target},
                    {"component_slot", slot}, {"field_path", field}, {"value", value}};
    };
    auto remove = [&](std::uint64_t target) {
        return Json{{"op", "remove_component"}, {"object_id", target}, {"component_slot", "animation"}};
    };
    auto refuse = [&](const Json &operation, const std::string &reason) {
        const auto revision = player.result("get_scene_revision");
        const auto before = player.result("scene_tree");
        const auto answer = edit(operation);
        INFO(answer.dump(2));
        REQUIRE(answer.at("status") == "rejected");
        CHECK(answer.at("error").at("code") == "animation.component_invalid@1");
        CHECK_THAT(answer.at("error").at("message").get<std::string>(),
                   Catch::Matchers::ContainsSubstring(reason));
        CHECK(player.result("get_scene_revision") == revision);
        CHECK(player.result("scene_tree") == before);
        CHECK(readFile(scene_path) == disk_before);
        return answer;
    };

    // T2: authored invalid data survives startup and several ECS frames, once per entity.
    const auto problem = onlyProblem(player);
    CHECK(problem.at("code") == "animation.component_invalid@1");
    CHECK(problem.at("fields").at("object") == cube);
    CHECK(problem.at("fields").at("name") == "Cube");
    CHECK(problem.at("fields").at("scene_id") == "default_scene");
    CHECK_THAT(problem.at("message").get<std::string>(),
               Catch::Matchers::ContainsSubstring("without a glTF skin"));
    const auto frame = player.result("get_status").at("frame").get<std::uint64_t>();
    const auto revision = revisionOf(player);
    (void)player.result("step_frame");
    (void)player.result("step_frame");
    CHECK(player.result("get_status").at("frame").get<std::uint64_t>() > frame);
    CHECK(onlyProblem(player).at("id") == problem.at("id"));
    CHECK(revisionOf(player) == revision);
    SECTION("remove the invalid component") {
        REQUIRE(edit(remove(cube)).at("status") == "committed");
    }
    SECTION("repair the model while keeping its clip") {
        REQUIRE(edit(set(cube, "simplemodelview", "/model", "character")).at("status") == "committed");
        (void)player.result("step_frame");
        requireNoProblems(player);
        REQUIRE(edit(remove(cube)).at("status") == "committed");
        REQUIRE(edit(set(cube, "simplemodelview", "/model", "engine://primitives/cube")).at("status") == "committed");
    }
    (void)player.result("step_frame");
    requireNoProblems(player);

    // T1: no skin, wrong file, wrong fragment, missing clip; the valid clip commits.
    refuse(add(cube, clip), "without a glTF skin");
    refuse(add(character, "assets/models/other.glb#animation/Turn"), "source does not match");
    refuse(add(character, "assets/models/character.glb#mesh/Cube"), "must be a #animation fragment");
    refuse(add(character, "assets/models/character.glb#animation/Absent"), "Absent");
    REQUIRE(edit(add(character, clip)).at("status") == "committed");
    (void)player.result("step_frame");
    requireNoProblems(player);
    refuse(set(character, "animation", "/clip", "assets/models/other.glb#animation/Turn"),
           "source does not match");

    // T3: model replacement is rejected with the removal-first hint.
    const auto refused = refuse(set(character, "simplemodelview", "/model", "engine://primitives/cube"),
                               "without a glTF skin");
    CHECK_THAT(refused.dump(), Catch::Matchers::ContainsSubstring("Remove animation first"));
    REQUIRE(edit(remove(character)).at("status") == "committed");
    REQUIRE(edit(set(character, "simplemodelview", "/model", "engine://primitives/cube")).at("status") == "committed");
    requireNoProblems(player);
    player.close();
    CHECK(player.exit_code == 0u);
}
#endif
